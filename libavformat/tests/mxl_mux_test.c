/*
 * MXL muxer PTS-derived write index regression tests
 *
 * Copyright (c) 2026 CBC/Radio-Canada
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Verify that the MXL muxer writes video grains at packet PTS indices and
 * audio samples at MXL head indices derived from first-sample PTS,
 * preserves forward gaps, and rejects missing or negative PTS.
 *
 * Each scenario creates one MXL flow through libavformat and verifies its
 * contents using the native MXL reader API. There are no threads or clocks.
 *
 * Usage:
 *   tests/mxl_mux_test <absolute_domain_dir> <scenario>
 *
 * Scenarios:
 *   video-pts, audio-pts,
 *   video-discontinuity, audio-discontinuity,
 *   video-missing-pts, audio-missing-pts,
 *   video-negative-pts, audio-negative-pts
 *
 * The domain directory must not already exist; it is removed on completion.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>

#include <libavformat/avformat.h>
#include "libavformat/mux.h"
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>

#define HISTORY_DURATION_NS UINT64_C(100000000)
#define START_INDEX INT64_C(1000000)
#define VIDEO_WIDTH 192
#define VIDEO_HEIGHT 16
#define VIDEO_FRAME_BYTES (VIDEO_WIDTH * 8 / 3 * VIDEO_HEIGHT)
#define VIDEO_FRAME_COUNT 4
#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_SAMPLES_PER_PACKET 64
#define AUDIO_PACKET_COUNT 4
#define VIDEO_FLOW_ID "092b5876-d4ed-4620-9df4-96d162af9c2c"
#define AUDIO_FLOW_ID "b54f6737-dc72-413a-b7cd-6fbe7a1c32c0"

typedef struct TestDomain {
    const char *directory;
    char *options_path;
    int created;
    int options_created;
} TestDomain;

typedef struct TestMux {
    AVFormatContext *output;
    mxlInstance instance;
    mxlFlowReader reader;
    int header_written;
    enum AVMediaType media_type;
} TestMux;

typedef enum TestScenario {
    VIDEO_PTS,
    AUDIO_PTS,
    VIDEO_DISCONTINUITY,
    AUDIO_DISCONTINUITY,
    VIDEO_MISSING_PTS,
    AUDIO_MISSING_PTS,
    VIDEO_NEGATIVE_PTS,
    AUDIO_NEGATIVE_PTS,
} TestScenario;

static const char *const scenario_names[] = {
    "video-pts",
    "audio-pts",
    "video-discontinuity",
    "audio-discontinuity",
    "video-missing-pts",
    "audio-missing-pts",
    "video-negative-pts",
    "audio-negative-pts",
};

static int create_domain(TestDomain *domain, const char *directory)
{
    static const char filename[] = "/options.json";
    size_t length = strlen(directory);
    FILE *file = NULL;
    int fd = -1;
    int failed = -1;

    domain->directory = directory;
    if (length > SIZE_MAX - sizeof(filename))
        goto finally;
    domain->options_path = malloc(length + sizeof(filename));
    if (!domain->options_path)
        goto finally;
    sprintf(domain->options_path, "%s%s", directory, filename);

    if (mkdir(directory, 0777)) {
        perror("mxl_mux_test: mkdir");
        goto finally;
    }
    domain->created = 1;

    fd = open(domain->options_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        perror("mxl_mux_test: options.json");
        goto finally;
    }
    domain->options_created = 1;
    file = fdopen(fd, "w");
    if (!file) {
        perror("mxl_mux_test: fdopen");
        goto finally;
    }
    fd = -1;

    if (fprintf(file, "{\n  \"urn:x-mxl:option:history_duration/v1.0\": "
                "%" PRIu64 "\n}\n", HISTORY_DURATION_NS) < 0)
        goto finally;
    if (fclose(file)) {
        file = NULL;
        goto finally;
    }
    file = NULL;
    failed = 0;

finally:
    if (file && fclose(file))
        failed = -1;
    if (fd >= 0 && close(fd))
        failed = -1;
    return failed;
}

static int remove_domain(TestDomain *domain)
{
    int failed = 0;

    if (domain->options_created && unlink(domain->options_path)) {
        perror("mxl_mux_test: unlink options.json");
        failed = -1;
    }
    if (domain->created && rmdir(domain->directory)) {
        perror("mxl_mux_test: rmdir");
        failed = -1;
    }
    free(domain->options_path);
    domain->options_path = NULL;
    return failed;
}

static int open_muxer(TestMux *mux, const char *directory,
                      enum AVMediaType media_type)
{
    const char *flow_id = media_type == AVMEDIA_TYPE_VIDEO ?
                          VIDEO_FLOW_ID : AUDIO_FLOW_ID;
    const char *flow_option = media_type == AVMEDIA_TYPE_VIDEO ?
                              "video_flow_id" : "audio_flow_id";
    AVStream *stream;
    AVDictionary *options = NULL;
    char error[AV_ERROR_MAX_STRING_SIZE];
    mxlStatus status;
    int rc;
    int failed = -1;

    mux->media_type = media_type;
    rc = avformat_alloc_output_context2(&mux->output, NULL, "mxl", directory);
    if (rc < 0 || !mux->output)
        goto finally;

    stream = avformat_new_stream(mux->output, NULL);
    if (!stream)
        goto finally;

    stream->codecpar->codec_type = media_type;
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        stream->codecpar->codec_id = AV_CODEC_ID_V210;
        stream->codecpar->width = VIDEO_WIDTH;
        stream->codecpar->height = VIDEO_HEIGHT;
        stream->time_base = (AVRational){1001, 60000};
        stream->avg_frame_rate = (AVRational){60000, 1001};
    } else {
        stream->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
        stream->codecpar->sample_rate = AUDIO_SAMPLE_RATE;
        stream->codecpar->block_align = AUDIO_CHANNELS * sizeof(float);
        stream->time_base = (AVRational){1, AUDIO_SAMPLE_RATE};
        av_channel_layout_default(&stream->codecpar->ch_layout, AUDIO_CHANNELS);
    }

    mux->output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    rc = av_dict_set(&options, "mxl_write_index", "pts", 0);
    if (rc >= 0)
        rc = av_dict_set(&options, flow_option, flow_id, 0);
    if (rc < 0)
        goto finally;
    rc = avformat_write_header(mux->output, &options);
    if (rc < 0)
        goto finally;
    mux->header_written = 1;
    if (av_dict_count(options)) {
        fprintf(stderr, "mxl_mux_test: unrecognized muxer options\n");
        goto finally;
    }

    mux->instance = mxlCreateInstance(directory, NULL);
    if (!mux->instance) {
        fprintf(stderr, "mxl_mux_test: mxlCreateInstance failed\n");
        goto finally;
    }
    status = mxlCreateFlowReader(mux->instance, flow_id, NULL, &mux->reader);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_mux_test: mxlCreateFlowReader failed (%d)\n", (int)status);
        goto finally;
    }
    failed = 0;

finally:
    if (failed && rc < 0) {
        av_strerror(rc, error, sizeof(error));
        fprintf(stderr, "mxl_mux_test: open muxer failed: %s\n", error);
    }
    av_dict_free(&options);
    return failed;
}

static int close_muxer(TestMux *mux)
{
    mxlStatus status;
    int failed = 0;
    int rc;

    if (mux->reader) {
        status = mxlReleaseFlowReader(mux->instance, mux->reader);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_mux_test: release reader failed (%d)\n", (int)status);
            failed = -1;
        }
        mux->reader = NULL;
    }
    if (mux->instance) {
        status = mxlDestroyInstance(mux->instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_mux_test: destroy reader instance failed (%d)\n", (int)status);
            failed = -1;
        }
        mux->instance = NULL;
    }
    if (mux->output) {
        if (mux->header_written) {
            rc = av_write_trailer(mux->output);
            if (rc < 0) {
                fprintf(stderr, "mxl_mux_test: write trailer failed (%d)\n", rc);
                failed = -1;
            }
        }
        avformat_free_context(mux->output);
        mux->output = NULL;
    }
    return failed;
}

static AVPacket *make_packet(enum AVMediaType media_type, int64_t pts,
                             unsigned ordinal)
{
    AVPacket *packet = av_packet_alloc();
    size_t count = AUDIO_SAMPLES_PER_PACKET;
    int size = media_type == AVMEDIA_TYPE_VIDEO ? VIDEO_FRAME_BYTES :
               AUDIO_SAMPLES_PER_PACKET * AUDIO_CHANNELS * sizeof(float);
    unsigned sample, channel;

    if (!packet)
        goto finally;
    if (av_new_packet(packet, size) < 0)
        goto finally;
    packet->stream_index = 0;
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = media_type == AVMEDIA_TYPE_VIDEO ? 1 : count;
    packet->flags = AV_PKT_FLAG_KEY;

    if (media_type == AVMEDIA_TYPE_VIDEO) {
        memset(packet->data, (uint8_t)(ordinal + 1), packet->size);
    } else {
        float *data = (float *)packet->data;
        for (sample = 0; sample < count; sample++)
            for (channel = 0; channel < AUDIO_CHANNELS; channel++)
                data[sample * AUDIO_CHANNELS + channel] =
                    (float)(ordinal * 256 + sample * AUDIO_CHANNELS + channel);
    }
    return packet;

finally:
    av_packet_free(&packet);
    return NULL;
}

static int submit_packet(TestMux *mux, AVPacket *packet, int direct,
                         int expected_status)
{
    const FFOutputFormat *format = ffofmt(mux->output->oformat);
    int rc = direct ? format->write_packet(mux->output, packet) :
                      av_write_frame(mux->output, packet);

    if ((expected_status == 0 && rc < 0) ||
        (expected_status < 0 && rc != expected_status)) {
        fprintf(stderr, "mxl_mux_test: packet PTS=%" PRId64
                " returned %d, expected %d\n", packet->pts, rc, expected_status);
        return -1;
    }
    return 0;
}

static int verify_video(mxlFlowReader reader, const AVPacket *packet)
{
    mxlGrainInfo grain = {0};
    uint8_t *payload = NULL;
    mxlStatus status = mxlFlowReaderGetGrainNonBlocking(reader,
                                                       (uint64_t)packet->pts,
                                                       &grain, &payload);

    if (status != MXL_STATUS_OK || grain.index != (uint64_t)packet->pts ||
        !payload || grain.validSlices != grain.totalSlices ||
        !grain.totalSlices || (grain.flags & MXL_GRAIN_FLAG_INVALID) ||
        grain.grainSize < (uint32_t)packet->size ||
        memcmp(payload, packet->data, packet->size)) {
        fprintf(stderr, "mxl_mux_test: video grain verification failed at %" PRId64
                " (status %d)\n", packet->pts, (int)status);
        return -1;
    }
    return 0;
}

static int verify_audio(mxlFlowReader reader, const AVPacket *packet)
{
    mxlWrappedMultiBufferSlice payload = {0};
    size_t count = (size_t)packet->duration;
    uint64_t head_index = (uint64_t)packet->pts + count - 1;
    const float *expected = (const float *)packet->data;
    size_t sample_base = 0;
    size_t fragment, channel, sample;
    mxlStatus status;

    status = mxlFlowReaderGetSamplesNonBlocking(reader,
                                                head_index, count, &payload);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_mux_test: audio read failed at %" PRIu64
                " (status %d)\n", head_index, (int)status);
        return -1;
    }
    if (payload.count != AUDIO_CHANNELS) {
        fprintf(stderr, "mxl_mux_test: audio channel count mismatch\n");
        return -1;
    }
    for (fragment = 0; fragment < 2; fragment++) {
        const mxlBufferSlice *slice = &payload.base.fragments[fragment];
        size_t fragment_samples;

        if (slice->size % sizeof(float))
            return -1;
        fragment_samples = slice->size / sizeof(float);
        if (!fragment_samples)
            continue;
        if (!slice->pointer)
            return -1;
        if (sample_base + fragment_samples > count ||
            payload.stride < slice->size) {
            fprintf(stderr, "mxl_mux_test: invalid audio fragment layout\n");
            return -1;
        }
        for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
            const uint8_t *src = (const uint8_t *)slice->pointer +
                                 channel * payload.stride;
            for (sample = 0; sample < fragment_samples; sample++) {
                if (memcmp(src + sample * sizeof(float),
                           &expected[((sample_base + sample) * AUDIO_CHANNELS) + channel],
                           sizeof(float))) {
                    fprintf(stderr, "mxl_mux_test: audio sample mismatch at %" PRId64
                            ", channel %zu sample %zu\n", packet->pts,
                            channel, sample_base + sample);
                    return -1;
                }
            }
        }
        sample_base += fragment_samples;
    }
    if (sample_base != count) {
        fprintf(stderr, "mxl_mux_test: audio sample count mismatch\n");
        return -1;
    }
    return 0;
}

static int verify_packet(TestMux *mux, const AVPacket *packet)
{
    return mux->media_type == AVMEDIA_TYPE_VIDEO ?
           verify_video(mux->reader, packet) :
           verify_audio(mux->reader, packet);
}

static int write_and_verify(TestMux *mux, int64_t pts, unsigned ordinal)
{
    AVPacket *packet = NULL;
    int failed = -1;

    packet = make_packet(mux->media_type, pts, ordinal);
    if (!packet)
        goto finally;
    if (submit_packet(mux, packet, 0, 0) || verify_packet(mux, packet))
        goto finally;
    failed = 0;

finally:
    av_packet_free(&packet);
    return failed;
}

static int test_indices(TestMux *mux, int discontinuity)
{
    int64_t step = mux->media_type == AVMEDIA_TYPE_VIDEO ? 1 :
                   AUDIO_SAMPLES_PER_PACKET;
    unsigned count = mux->media_type == AVMEDIA_TYPE_VIDEO ?
                     VIDEO_FRAME_COUNT : AUDIO_PACKET_COUNT;
    unsigned i;

    for (i = 0; i < count; i++) {
        int64_t packet_index = i + (discontinuity && i >= 2 ? 2 : 0);
        if (write_and_verify(mux, START_INDEX + packet_index * step, i))
            return -1;
    }
    return 0;
}

static int test_invalid_pts(TestMux *mux, int missing)
{
    mxlFlowRuntimeInfo before = {0}, after = {0};
    mxlStatus status;
    AVPacket *invalid = NULL;
    int64_t step = mux->media_type == AVMEDIA_TYPE_VIDEO ? 1 :
                   AUDIO_SAMPLES_PER_PACKET;
    int failed = -1;

    if (write_and_verify(mux, START_INDEX, 0))
        goto finally;
    status = mxlFlowReaderGetRuntimeInfo(mux->reader, &before);
    if (status != MXL_STATUS_OK)
        goto finally;

    invalid = make_packet(mux->media_type,
                          missing ? AV_NOPTS_VALUE : -1, 1);
    if (!invalid)
        goto finally;

    /* Bypass libavformat's generic timestamp checks to test mxlenc.c itself. */
    if (submit_packet(mux, invalid, 1, AVERROR(EINVAL)))
        goto finally;
    status = mxlFlowReaderGetRuntimeInfo(mux->reader, &after);
    if (status != MXL_STATUS_OK || before.headIndex != after.headIndex) {
        fprintf(stderr, "mxl_mux_test: rejected packet changed MXL flow head\n");
        goto finally;
    }

    if (write_and_verify(mux, START_INDEX + step, 2))
        goto finally;
    failed = 0;

finally:
    av_packet_free(&invalid);
    return failed;
}

static int run_scenario(TestScenario scenario, const char *directory)
{
    TestDomain domain = {0};
    TestMux mux = {0};
    enum AVMediaType media_type = scenario == VIDEO_PTS ||
                                   scenario == VIDEO_DISCONTINUITY ||
                                   scenario == VIDEO_MISSING_PTS ||
                                   scenario == VIDEO_NEGATIVE_PTS ?
                                   AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    int failed = -1;

    if (create_domain(&domain, directory))
        goto finally;
    if (open_muxer(&mux, directory, media_type))
        goto finally;

    switch (scenario) {
    case VIDEO_PTS:
    case AUDIO_PTS:
        failed = test_indices(&mux, 0);
        break;
    case VIDEO_DISCONTINUITY:
    case AUDIO_DISCONTINUITY:
        failed = test_indices(&mux, 1);
        break;
    case VIDEO_MISSING_PTS:
    case AUDIO_MISSING_PTS:
        failed = test_invalid_pts(&mux, 1);
        break;
    case VIDEO_NEGATIVE_PTS:
    case AUDIO_NEGATIVE_PTS:
        failed = test_invalid_pts(&mux, 0);
        break;
    }

finally:
    if (close_muxer(&mux))
        failed = -1;
    if (remove_domain(&domain))
        failed = -1;
    return failed;
}

int main(int argc, char **argv)
{
    TestScenario scenario;
    int failed;

    if (argc != 3 || argv[1][0] != '/' || !argv[1][1]) {
        fprintf(stderr, "Usage: %s absolute_domain_dir "
                "{video-pts|audio-pts|video-discontinuity|audio-discontinuity|"
                "video-missing-pts|audio-missing-pts|"
                "video-negative-pts|audio-negative-pts}\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (scenario = VIDEO_PTS; scenario <= AUDIO_NEGATIVE_PTS; scenario++)
        if (!strcmp(argv[2], scenario_names[scenario]))
            break;
    if (scenario > AUDIO_NEGATIVE_PTS) {
        fprintf(stderr, "mxl_mux_test: unknown scenario %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    if (setenv("MXL_LOG_LEVEL", "off", 1)) {
        perror("mxl_mux_test: setenv");
        return EXIT_FAILURE;
    }

    av_log_set_level(AV_LOG_ERROR);
    failed = run_scenario(scenario, argv[1]);
    printf("mxl_mux_test: %s: %s\n", argv[2], failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
