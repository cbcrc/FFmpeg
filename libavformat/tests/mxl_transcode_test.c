/*
 * MXL-to-MXL stream copy regression test
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
 * Verify that an FFmpeg MXL demuxer-to-muxer stream copy preserves
 * absolute grain/sample indices and media payloads.
 *
 * Video-only and audio-only operation transfer and verify one second of
 * media through an external FFmpeg stream-copy process. Combined A/V
 * verification will follow. FFmpeg creates the destination flows; the
 * external runner creates the destination domain directory.
 *
 * Usage:
 *   tests/mxl_transcode_test <source_domain>
 *       [--video=UUID] [--audio=UUID] [--audio-samples-per-buffer=N]
 *       <destination_domain> [--video=UUID] [--audio=UUID]
 *
 * The source flow options select video, audio, or A/V testing. The
 * destination must specify the same media types with different flow IDs.
 * The source domain must not already exist and is removed on completion.
 * The destination directory is managed by the external FFmpeg runner.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>

#include "libavutil/time.h"

#define DEFAULT_AUDIO_SAMPLES_PER_BUFFER 800
#define VIDEO_GRAIN_COUNT 60
#define AUDIO_SAMPLE_COUNT 48000
#define AUDIO_CHANNELS 2
#define VIDEO_START_INDEX UINT64_C(1000000)
#define AUDIO_START_INDEX UINT64_C(1000000)
#define READ_TIMEOUT_NS UINT64_C(30000000000)
#define DST_STARTUP_RETRIES 60

typedef enum TestMedia {
    MEDIA_VIDEO = 1 << 0,
    MEDIA_AUDIO = 1 << 1,
} TestMedia;

static const char video_flow_template[] =
    "{"
    "\"id\":\"%s\","
    "\"label\":\"MXL transcode test video\","
    "\"description\":\"MXL transcode test video flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Transcode Test:Video\"]},"
    "\"format\":\"urn:x-nmos:format:video\","
    "\"parents\":[],"
    "\"media_type\":\"video/v210\","
    "\"grain_rate\":{\"numerator\":60,\"denominator\":1},"
    "\"frame_width\":1280,"
    "\"frame_height\":720,"
    "\"interlace_mode\":\"progressive\","
    "\"colorspace\":\"BT709\","
    "\"components\":["
      "{\"name\":\"Y\",\"width\":1280,\"height\":720,\"bit_depth\":10},"
      "{\"name\":\"Cb\",\"width\":640,\"height\":720,\"bit_depth\":10},"
      "{\"name\":\"Cr\",\"width\":640,\"height\":720,\"bit_depth\":10}"
    "]"
    "}";

static const char audio_flow_template[] =
    "{"
    "\"id\":\"%s\","
    "\"label\":\"MXL transcode test audio\","
    "\"description\":\"MXL transcode test audio flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Transcode Test:Audio\"]},"
    "\"format\":\"urn:x-nmos:format:audio\","
    "\"parents\":[],"
    "\"media_type\":\"audio/float32\","
    "\"sample_rate\":{\"numerator\":48000},"
    "\"channel_count\":2,"
    "\"bit_depth\":32"
    "}";

typedef struct TestOptions {
    const char *src_directory;
    const char *dst_directory;
    const char *src_video_id;
    const char *src_audio_id;
    const char *dst_video_id;
    const char *dst_audio_id;
    size_t audio_samples_per_buffer;
    int audio_samples_specified;
    TestMedia media;
} TestOptions;

typedef struct TestDomain {
    const char *directory;
    int created;
} TestDomain;

typedef struct TestContext {
    TestDomain src_domain;
    mxlInstance src_instance;
    mxlFlowWriter video_writer;
    mxlFlowWriter audio_writer;
    mxlInstance dst_instance;
    mxlFlowReader video_reader;
    mxlFlowReader audio_reader;
    uint32_t video_grain_size;
} TestContext;

static int valid_uuid(const char *id)
{
    unsigned i;

    if (strlen(id) != 36)
        return 0;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)id[i])) {
            return 0;
        }
    }
    return 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s <source_domain> "
            "[--video=UUID] [--audio=UUID] "
            "[--audio-samples-per-buffer=N] "
            "<destination_domain> [--video=UUID] [--audio=UUID]\n",
            program);
}

static int validate_options(TestOptions *opts)
{
    if (!opts->src_directory || !opts->src_directory[0] ||
        !opts->dst_directory || !opts->dst_directory[0] ||
        !strcmp(opts->src_directory, opts->dst_directory)) {
        fprintf(stderr, "mxl_transcode_test: invalid domain paths\n");
        return -1;
    }
    if (!opts->src_video_id && !opts->src_audio_id) {
        fprintf(stderr, "mxl_transcode_test: no source flows specified\n");
        return -1;
    }
    if (!!opts->src_video_id != !!opts->dst_video_id ||
        !!opts->src_audio_id != !!opts->dst_audio_id) {
        fprintf(stderr, "mxl_transcode_test: source and destination media must match\n");
        return -1;
    }
    if (opts->audio_samples_specified && !opts->src_audio_id) {
        fprintf(stderr, "mxl_transcode_test: audio buffer size requires audio\n");
        return -1;
    }
    if ((opts->src_video_id &&
         !strcmp(opts->src_video_id, opts->dst_video_id)) ||
        (opts->src_audio_id &&
         !strcmp(opts->src_audio_id, opts->dst_audio_id))) {
        fprintf(stderr, "mxl_transcode_test: source and destination flow IDs must differ\n");
        return -1;
    }

    if (opts->src_audio_id && AUDIO_SAMPLE_COUNT % opts->audio_samples_per_buffer) {
        fprintf(stderr, "mxl_transcode_test: --audio-samples-per-buffer must "
                "evenly divide %d\n", AUDIO_SAMPLE_COUNT);
        return -1;
    }

    opts->media = (opts->src_video_id ? MEDIA_VIDEO : 0) |
                  (opts->src_audio_id ? MEDIA_AUDIO : 0);
    return 0;
}

static int parse_audio_samples(const char *text, size_t *samples)
{
    char *end;
    unsigned long long value;

    if (!text[0] || text[0] == '-')
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || *end || !value || value > SIZE_MAX)
        return -1;
    *samples = (size_t)value;
    return 0;
}

static int parse_options(int argc, char **argv, TestOptions *opts)
{
    static const char samples_option[] = "--audio-samples-per-buffer=";
    const char *argument;
    const char **flow_id;
    int is_src = 1;
    int failed = -1;
    int i;

    opts->audio_samples_per_buffer = DEFAULT_AUDIO_SAMPLES_PER_BUFFER;
    if (argc < 5)
        goto finally;
    opts->src_directory = argv[1];

    for (i = 2; i < argc; i++) {
        argument = argv[i];
        if (strncmp(argument, "--", 2)) {
            if (!is_src)
                goto finally;
            opts->dst_directory = argument;
            is_src = 0;
        } else if (!strncmp(argument, "--video=", 8)) {
            flow_id = is_src ? &opts->src_video_id :
                                  &opts->dst_video_id;
            if (*flow_id || !valid_uuid(argument + 8))
                goto finally;
            *flow_id = argument + 8;
        } else if (!strncmp(argument, "--audio=", 8)) {
            flow_id = is_src ? &opts->src_audio_id :
                                  &opts->dst_audio_id;
            if (*flow_id || !valid_uuid(argument + 8))
                goto finally;
            *flow_id = argument + 8;
        } else if (is_src &&
                   !strncmp(argument, samples_option, sizeof(samples_option) - 1)) {
            if (opts->audio_samples_specified ||
                parse_audio_samples(argument + sizeof(samples_option) - 1,
                                    &opts->audio_samples_per_buffer))
                goto finally;
            opts->audio_samples_specified = 1;
        } else {
            goto finally;
        }
    }

    if (validate_options(opts))
        goto finally;
    failed = 0;

finally:
    if (failed)
        print_usage(argv[0]);
    return failed;
}

static int create_domain(TestDomain *domain, const char *directory)
{
    domain->directory = directory;
    if (mkdir(directory, 0777)) {
        perror("mxl_transcode_test: mkdir");
        return -1;
    }
    domain->created = 1;
    return 0;
}

static int remove_domain(TestDomain *domain)
{
    if (domain->created && rmdir(domain->directory)) {
        perror("mxl_transcode_test: rmdir");
        return -1;
    }
    return 0;
}

static int create_flow(mxlInstance instance, const char *flow_template,
                       const char *flow_id, const char *name,
                       mxlFlowWriter *writer)
{
    char definition[1024];
    mxlStatus status;
    int length;

    length = snprintf(definition, sizeof(definition), flow_template, flow_id);
    if (length < 0 || (size_t)length >= sizeof(definition)) {
        fprintf(stderr, "mxl_transcode_test: %s flow definition too long\n", name);
        return -1;
    }
    status = mxlCreateFlowWriter(instance, definition, NULL, writer, NULL, NULL);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: create %s flow failed (%d)\n",
                name, (int)status);
        return -1;
    }
    return 0;
}

static int initialize_test(TestContext *context, const TestOptions *opts)
{
    int failed = -1;

    if (create_domain(&context->src_domain, opts->src_directory))
        goto finally;

    context->src_instance = mxlCreateInstance(opts->src_directory, NULL);
    if (!context->src_instance) {
        fprintf(stderr, "mxl_transcode_test: create source instance failed\n");
        goto finally;
    }

    if ((opts->media & MEDIA_VIDEO) &&
        create_flow(context->src_instance, video_flow_template,
                    opts->src_video_id, "video", &context->video_writer))
        goto finally;
    if ((opts->media & MEDIA_AUDIO) &&
        create_flow(context->src_instance, audio_flow_template,
                    opts->src_audio_id, "audio", &context->audio_writer))
        goto finally;

    failed = 0;

finally:
    return failed;
}


static int open_dst_reader(TestContext *context, const TestOptions *opts,
                           const char *flow_id, const char *media,
                           mxlFlowReader *reader)
{
    struct stat st;
    mxlStatus status = MXL_STATUS_OK;
    int attempt;
    int failed = -1;

    printf("mxl_transcode_test: waiting for FFmpeg destination %s flow\n", media);
    fflush(stdout);
    for (attempt = 0; attempt < DST_STARTUP_RETRIES; attempt++) {
        if (!context->dst_instance &&
            !stat(opts->dst_directory, &st) && S_ISDIR(st.st_mode)) {
            context->dst_instance = mxlCreateInstance(opts->dst_directory, NULL);
        }
        if (context->dst_instance) {
            status = mxlCreateFlowReader(context->dst_instance, flow_id,
                                         NULL, reader);
            if (status == MXL_STATUS_OK) {
                failed = 0;
                goto finally;
            }
        }
        sleep(1);
    }
    fprintf(stderr, "mxl_transcode_test: destination %s flow unavailable "
            "after %d retries (last status %d)\n",
            media, DST_STARTUP_RETRIES, (int)status);

finally:
    return failed;
}

static int write_video_grain(TestContext *context, uint64_t index)
{
    mxlGrainInfo grain = {0};
    uint8_t *payload = NULL;
    mxlStatus status;
    int opened = 0;
    int failed = -1;

    status = mxlFlowWriterOpenGrain(context->video_writer, index,
                                    &grain, &payload);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: open video grain %" PRIu64
                " failed (%d)\n", index, (int)status);
        goto finally;
    }
    opened = 1;
    if (!payload || !grain.grainSize || !grain.totalSlices) {
        fprintf(stderr, "mxl_transcode_test: invalid source video grain %"
                PRIu64 "\n", index);
        goto finally;
    }
    if (context->video_grain_size &&
        grain.grainSize != context->video_grain_size) {
        fprintf(stderr, "mxl_transcode_test: source video grain size changed "
                "at index %" PRIu64 "\n", index);
        goto finally;
    }
    context->video_grain_size = grain.grainSize;
    memset(payload, (uint8_t)(index % 255), grain.grainSize);
    grain.validSlices = grain.totalSlices;
    status = mxlFlowWriterCommitGrain(context->video_writer, &grain);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: commit video grain %" PRIu64
                " failed (%d)\n", index, (int)status);
        goto finally;
    }
    opened = 0;
    failed = 0;

finally:
    if (opened && mxlFlowWriterCancelGrain(context->video_writer) != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: cancel video grain %" PRIu64
                " failed\n", index);
        failed = -1;
    }
    return failed;
}

static int verify_video_grain(TestContext *context, uint64_t index)
{
    mxlGrainInfo grain = {0};
    uint8_t *payload = NULL;
    uint8_t expected_byte = (uint8_t)(index % 255);
    mxlStatus status;
    size_t i;
    int failed = -1;

    status = mxlFlowReaderGetGrain(context->video_reader, index,
                                   READ_TIMEOUT_NS, &grain, &payload);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: read video grain %" PRIu64
                " failed (%d)\n", index, (int)status);
        goto finally;
    }
    if (grain.index != index || grain.grainSize != context->video_grain_size ||
        !payload || !grain.totalSlices || grain.validSlices != grain.totalSlices ||
        (grain.flags & MXL_GRAIN_FLAG_INVALID)) {
        fprintf(stderr, "mxl_transcode_test: video grain %" PRIu64
                " metadata mismatch (received index=%" PRIu64
                ", size=%" PRIu32 ", slices=%u/%u, flags=%" PRIu32 ")\n",
                index, grain.index, grain.grainSize,
                grain.validSlices, grain.totalSlices, grain.flags);
        goto finally;
    }
    for (i = 0; i < grain.grainSize; i++) {
        if (payload[i] != expected_byte) {
            fprintf(stderr, "mxl_transcode_test: video grain %" PRIu64
                    " payload mismatch at byte %zu (expected %u, got %u)\n",
                    index, i, expected_byte, payload[i]);
            goto finally;
        }
    }
    failed = 0;

finally:
    return failed;
}

static int test_video(TestContext *context, const TestOptions *opts)
{
    uint64_t index;
    int frame;
    int failed = -1;

    for (frame = 0; frame < VIDEO_GRAIN_COUNT; frame++) {
        index = VIDEO_START_INDEX + frame;
        if (write_video_grain(context, index))
            goto finally;
        if (frame == 0 && open_dst_reader(context, opts, opts->dst_video_id, "video",
                                          &context->video_reader))
            goto finally;
        if (verify_video_grain(context, index))
            goto finally;
        if (frame == 0 || (frame + 1) % 10 == 0) {
            printf("mxl_transcode_test: verified video grain %d/%d at index %"
                   PRIu64 "\n", frame + 1, VIDEO_GRAIN_COUNT, index);
            fflush(stdout);
        }
    }
    failed = 0;

finally:
    return failed;
}

static float expected_audio_sample(uint64_t index, size_t channel)
{
    return (float)((index % 255) + 256 * channel);
}

static int write_audio_buffer(TestContext *context, uint64_t head_index,
                              size_t count)
{
    mxlMutableWrappedMultiBufferSlice buffers = {0};
    mxlStatus status;
    size_t samples_written = 0;
    size_t fragment_index, sample, channel;
    int opened = 0;
    int failed = -1;

    status = mxlFlowWriterOpenSamples(context->audio_writer, head_index,
                                      count, &buffers);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: open audio samples at head %" PRIu64
                " failed (%d)\n", head_index, (int)status);
        goto finally;
    }
    opened = 1;
    if (buffers.count != AUDIO_CHANNELS) {
        fprintf(stderr, "mxl_transcode_test: source audio channel count mismatch\n");
        goto finally;
    }
    for (fragment_index = 0; fragment_index < 2; fragment_index++) {
        const mxlMutableBufferSlice *fragment =
            &buffers.base.fragments[fragment_index];
        size_t fragment_samples;

        if (fragment->size % sizeof(float)) {
            fprintf(stderr, "mxl_transcode_test: invalid source audio fragment size\n");
            goto finally;
        }
        fragment_samples = fragment->size / sizeof(float);
        if (!fragment_samples)
            continue;
        if (!fragment->pointer || fragment_samples > count - samples_written ||
            buffers.stride < fragment->size) {
            fprintf(stderr, "mxl_transcode_test: invalid source audio buffer layout\n");
            goto finally;
        }
        for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
            uint8_t *data = (uint8_t *)fragment->pointer + channel * buffers.stride;
            for (sample = 0; sample < fragment_samples; sample++) {
                float value = expected_audio_sample(
                    head_index - count + 1 + samples_written + sample, channel);
                memcpy(data + sample * sizeof(value), &value, sizeof(value));
            }
        }
        samples_written += fragment_samples;
    }
    if (samples_written != count) {
        fprintf(stderr, "mxl_transcode_test: source audio sample count mismatch\n");
        goto finally;
    }
    status = mxlFlowWriterCommitSamples(context->audio_writer);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: commit audio samples at head %" PRIu64
                " failed (%d)\n", head_index, (int)status);
        goto finally;
    }
    opened = 0;
    failed = 0;

finally:
    if (opened && mxlFlowWriterCancelSamples(context->audio_writer) != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: cancel audio samples at head %" PRIu64
                " failed\n", head_index);
        failed = -1;
    }
    return failed;
}

/*
 * The MXL blocking audio reader can time out with TOO_EARLY even when the
 * requested samples are available and a nonblocking read succeeds.
 * Until this MXL library issue is resolved, emulate blocking reads by polling
 * the nonblocking API. USE_MXL_BLOCKING_AUDIO_READ selects the native blocking
 * API for retesting once the library has been fixed.
 */
#define USE_MXL_BLOCKING_AUDIO_READ 0
#define AUDIO_READ_POLL_INTERVAL_US 10000
static mxlStatus test_mxlFlowReaderGetSamples(mxlFlowReader reader,
                                              uint64_t head_index, size_t count,
                                              uint64_t timeout_ns,
                                              mxlWrappedMultiBufferSlice *buffers)
{
    mxlStatus status;

#if USE_MXL_BLOCKING_AUDIO_READ
    status = mxlFlowReaderGetSamples(reader, head_index, count,
                                     timeout_ns, buffers);
#else
    int64_t start_us = av_gettime_relative();

    do {
        status = mxlFlowReaderGetSamplesNonBlocking(reader, head_index,
                                                    count, buffers);
        if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY ||
            (uint64_t)(av_gettime_relative() - start_us) >= timeout_ns / 1000)
            break;
        av_usleep(AUDIO_READ_POLL_INTERVAL_US);
    } while (1);
#endif

    return status;
}

static int verify_audio_buffer(TestContext *context, uint64_t head_index,
                               size_t count)
{
    mxlWrappedMultiBufferSlice buffers = {0};
    mxlStatus status;
    size_t samples_read = 0;
    size_t fragment_index, sample, channel;
    int failed = -1;

    status = test_mxlFlowReaderGetSamples(context->audio_reader, head_index,
                                          count, READ_TIMEOUT_NS, &buffers);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_transcode_test: read audio samples at head %" PRIu64
                " failed (%d)\n", head_index, (int)status);
        goto finally;
    }
    if (buffers.count != AUDIO_CHANNELS) {
        fprintf(stderr, "mxl_transcode_test: destination audio channel count mismatch\n");
        goto finally;
    }
    for (fragment_index = 0; fragment_index < 2; fragment_index++) {
        const mxlBufferSlice *fragment = &buffers.base.fragments[fragment_index];
        size_t fragment_samples;

        if (fragment->size % sizeof(float)) {
            fprintf(stderr, "mxl_transcode_test: invalid destination audio fragment size\n");
            goto finally;
        }
        fragment_samples = fragment->size / sizeof(float);
        if (!fragment_samples)
            continue;
        if (!fragment->pointer || fragment_samples > count - samples_read ||
            buffers.stride < fragment->size) {
            fprintf(stderr, "mxl_transcode_test: invalid destination audio buffer layout\n");
            goto finally;
        }
        for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
            const uint8_t *data = (const uint8_t *)fragment->pointer +
                                  channel * buffers.stride;
            for (sample = 0; sample < fragment_samples; sample++) {
                uint64_t index = head_index - count + 1 + samples_read + sample;
                float expected = expected_audio_sample(index, channel);
                float actual;

                memcpy(&actual, data + sample * sizeof(actual), sizeof(actual));
                if (actual != expected) {
                    fprintf(stderr, "mxl_transcode_test: audio mismatch at index %" PRIu64
                            ", channel %zu (expected %g, got %g)\n",
                            index, channel, (double)expected, (double)actual);
                    goto finally;
                }
            }
        }
        samples_read += fragment_samples;
    }
    if (samples_read != count) {
        fprintf(stderr, "mxl_transcode_test: destination audio sample count mismatch\n");
        goto finally;
    }
    failed = 0;

finally:
    return failed;
}

static int test_audio(TestContext *context, const TestOptions *opts)
{
    size_t count = opts->audio_samples_per_buffer;
    size_t offset;
    uint64_t head_index;
    int failed = -1;

    for (offset = 0; offset < AUDIO_SAMPLE_COUNT; offset += count) {
        head_index = AUDIO_START_INDEX + offset + count - 1;
        if (write_audio_buffer(context, head_index, count))
            goto finally;
        if (offset == 0 &&
            open_dst_reader(context, opts, opts->dst_audio_id, "audio",
                            &context->audio_reader))
            goto finally;
        if (verify_audio_buffer(context, head_index, count))
            goto finally;
        printf("mxl_transcode_test: verified audio buffer %zu/%zu "
               "at head %" PRIu64 "\n",
               offset / count + 1, (size_t)AUDIO_SAMPLE_COUNT / count,
               head_index);
        fflush(stdout);
    }
    failed = 0;

finally:
    return failed;
}

static int cleanup_test(TestContext *context)
{
    mxlStatus status;
    int failed = 0;

    if (context->audio_reader) {
        status = mxlReleaseFlowReader(context->dst_instance,
                                      context->audio_reader);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release audio reader failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->audio_reader = NULL;
    }
    if (context->video_reader) {
        status = mxlReleaseFlowReader(context->dst_instance,
                                      context->video_reader);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release video reader failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->video_reader = NULL;
    }
    if (context->dst_instance) {
        status = mxlDestroyInstance(context->dst_instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: destroy destination instance failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->dst_instance = NULL;
    }
    if (context->video_writer) {
        status = mxlReleaseFlowWriter(context->src_instance,
                                      context->video_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release video writer failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->video_writer = NULL;
    }
    if (context->audio_writer) {
        status = mxlReleaseFlowWriter(context->src_instance,
                                      context->audio_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release audio writer failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->audio_writer = NULL;
    }
    if (context->src_instance) {
        status = mxlDestroyInstance(context->src_instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: destroy source instance failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->src_instance = NULL;
    }
    if (remove_domain(&context->src_domain))
        failed = -1;
    return failed;
}

int main(int argc, char **argv)
{
    TestOptions opts = {0};
    TestContext context = {0};
    int failed = -1;

    if (parse_options(argc, argv, &opts))
        return EXIT_FAILURE;
    if (setenv("MXL_LOG_LEVEL", "off", 1)) {
        perror("mxl_transcode_test: setenv");
        goto finally;
    }
    if (initialize_test(&context, &opts))
        goto finally;

    printf("mxl_transcode_test: initialized %s source flow(s)\n",
           opts.media == MEDIA_VIDEO ? "video" :
           opts.media == MEDIA_AUDIO ? "audio" : "av");
    puts("mxl_transcode_test: destination flows managed by FFmpeg");
    if (opts.media == MEDIA_VIDEO && test_video(&context, &opts))
        goto finally;
    if (opts.media == MEDIA_AUDIO && test_audio(&context, &opts))
        goto finally;
    if (opts.media == (MEDIA_VIDEO | MEDIA_AUDIO))
        puts("mxl_transcode_test: A/V media transfer not yet implemented");
    failed = 0;

finally:
    if (cleanup_test(&context))
        failed = -1;
    if (!failed)
        puts("mxl_transcode_test: cleanup complete");
    printf("mxl_transcode_test: %s: %s\n",
           opts.media == MEDIA_VIDEO ? "video" :
           opts.media == MEDIA_AUDIO ? "audio" : "av",
           failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
