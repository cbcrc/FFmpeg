/*
 * MXL demuxer timestamp and discontinuity regression tests
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
 * License along with FFmpeg; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Verify that the MXL demuxer preserves absolute audio/video
 * timestamps, maintains PTS monotonicity, and correctly handles delayed
 * stream startup and recovery from midstream index discontinuities.
 *
 * Four independent scenarios are supported:
 *
 *   delayed-audio        Delay audio startup by 500 ms.
 *   delayed-video        Delay video startup by 500 ms.
 *   audio-interruption   Interrupt and resume an established audio flow.
 *   video-interruption   Interrupt and resume an established video flow.
 *
 * Each scenario creates an MXL domain with video and audio flows, writes
 * synthetic media, and validates packets read through libavformat. The
 * interruption tests exercise the demuxer's TOO_LATE recovery mechanism.
 *
 * Media progression is index-driven, without real-time pacing or threads.
 * Each invocation runs one scenario and reports PASS or FAIL.
 *
 * Usage:
 *   tests/mxl_demux_test <absolute_domain_dir> <scenario>
 *
 * Example:
 *   tests/mxl_demux_test /tmp/mxl-test video-interruption
 *
 * The domain directory must not already exist. It is removed on completion.
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
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>

#define HISTORY_DURATION_NS UINT64_C(100000000)
#define START_INDEX UINT64_C(1000000)
#define STARTUP_DELAY_FRAMES 30
#define TEST_FRAMES 72
#define INTERRUPTION_START_FRAME 30
#define INTERRUPTION_END_FRAME 60
#define INTERRUPTION_TEST_FRAMES 100
#define AUDIO_CHANNELS 2
#define VIDEO_FLOW_ID "932dc2da-0c7b-4c63-81d8-a48bb004c709"
#define AUDIO_FLOW_ID "40178de3-71a0-452a-a541-dd14979ae7c1"

static const char video_flow_def[] =
    "{"
    "\"id\":\"" VIDEO_FLOW_ID "\","
    "\"label\":\"MXL demux test video\","
    "\"description\":\"MXL demux test video flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Demux Test:Video\"]},"
    "\"format\":\"urn:x-nmos:format:video\","
    "\"parents\":[],"
    "\"media_type\":\"video/v210\","
    "\"grain_rate\":{\"numerator\":60000,\"denominator\":1001},"
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

static const char audio_flow_def[] =
    "{"
    "\"id\":\"" AUDIO_FLOW_ID "\","
    "\"label\":\"MXL demux test audio\","
    "\"description\":\"MXL demux test audio flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Demux Test:Audio\"]},"
    "\"format\":\"urn:x-nmos:format:audio\","
    "\"parents\":[],"
    "\"media_type\":\"audio/float32\","
    "\"sample_rate\":{\"numerator\":48000},"
    "\"channel_count\":2,"
    "\"bit_depth\":32"
    "}";

typedef struct TestDomain {
    const char *directory;
    char *options_path;
    int created;
    int options_created;
} TestDomain;

typedef struct TestSource {
    mxlInstance instance;
    mxlFlowWriter video;
    mxlFlowWriter audio;
} TestSource;

static int create_domain(TestDomain *domain, const char *directory)
{
    static const char filename[] = "/options.json";
    size_t length = strlen(directory);
    FILE *file;
    int fd;
    int failed = 0;

    domain->directory = directory;
    if (length > SIZE_MAX - sizeof(filename))
        return -1;
    domain->options_path = malloc(length + sizeof(filename));
    if (!domain->options_path)
        return -1;
    sprintf(domain->options_path, "%s%s", directory, filename);

    if (mkdir(directory, 0777)) {
        perror("mxl_demux_test: mkdir");
        return -1;
    }
    domain->created = 1;

    fd = open(domain->options_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        perror("mxl_demux_test: options.json");
        return -1;
    }
    domain->options_created = 1;
    file = fdopen(fd, "w");
    if (!file) {
        perror("mxl_demux_test: fdopen");
        close(fd);
        return -1;
    }
    if (fprintf(file, "{\n  \"urn:x-mxl:option:history_duration/v1.0\": "
                "%" PRIu64 "\n}\n", HISTORY_DURATION_NS) < 0)
        failed = 1;
    if (fclose(file))
        failed = 1;
    if (failed)
        fprintf(stderr, "mxl_demux_test: failed to write %s\n", domain->options_path);

    return failed ? -1 : 0;
}

static int remove_domain(TestDomain *domain)
{
    int failed = 0;

    if (domain->options_created && unlink(domain->options_path)) {
        perror("mxl_demux_test: unlink options.json");
        failed = 1;
    }
    if (domain->created && rmdir(domain->directory)) {
        perror("mxl_demux_test: rmdir");
        failed = 1;
    }
    free(domain->options_path);
    domain->options_path = NULL;
    return failed ? -1 : 0;
}

static int create_flow(mxlInstance instance, const char *definition,
                       const char *name, mxlFlowWriter *writer)
{
    mxlStatus status = mxlCreateFlowWriter(instance, definition, NULL,
                                           writer, NULL, NULL);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_demux_test: create %s flow failed (%d)\n",
                name, (int)status);
        return -1;
    }
    return 0;
}

static int close_source(TestSource *source)
{
    mxlStatus status;
    int failed = 0;

    if (source->video) {
        status = mxlReleaseFlowWriter(source->instance, source->video);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_demux_test: release video flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->video = NULL;
    }
    if (source->audio) {
        status = mxlReleaseFlowWriter(source->instance, source->audio);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_demux_test: release audio flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->audio = NULL;
    }
    if (source->instance) {
        status = mxlDestroyInstance(source->instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_demux_test: destroy instance failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->instance = NULL;
    }
    return failed ? -1 : 0;
}

static int open_demuxer(AVFormatContext **context, const char *directory)
{
    static const char prefix[] = "mxl://";
    static const char suffix[] = "?id=" VIDEO_FLOW_ID "&id=" AUDIO_FLOW_ID;
    const AVInputFormat *format = av_find_input_format("mxl");
    AVDictionary *options = NULL;
    char *url = NULL;
    char error[AV_ERROR_MAX_STRING_SIZE];
    size_t length = strlen(directory);
    int rc = AVERROR(EINVAL);

    if (!format) {
        fprintf(stderr, "mxl_demux_test: MXL demuxer unavailable\n");
        return -1;
    }
    if (length > SIZE_MAX - sizeof(prefix) - sizeof(suffix))
        return -1;
    url = malloc(sizeof(prefix) - 1 + length + sizeof(suffix));
    if (!url)
        return -1;
    sprintf(url, "%s%s%s", prefix, directory, suffix);

    rc = av_dict_set(&options, "blocking", "0", 0);
    if (rc >= 0)
        rc = av_dict_set(&options, "mxl_read_index_init", "head", 0);
    if (rc >= 0)
        rc = av_dict_set(&options, "on_too_late", "reset", 0);
    if (rc >= 0)
        rc = avformat_open_input(context, url, format, &options);
    if (rc >= 0 && av_dict_count(options)) {
        fprintf(stderr, "mxl_demux_test: unrecognized demuxer options\n");
        rc = AVERROR(EINVAL);
    }
    if (rc < 0) {
        av_strerror(rc, error, sizeof(error));
        fprintf(stderr, "mxl_demux_test: open demuxer failed: %s\n", error);
    }

    av_dict_free(&options);
    free(url);
    return rc < 0 ? -1 : 0;
}

static int check_streams(const AVFormatContext *context)
{
    unsigned i;
    int video = 0;
    int audio = 0;

    if (context->nb_streams != 2) {
        fprintf(stderr, "mxl_demux_test: expected 2 streams, got %u\n",
                context->nb_streams);
        return -1;
    }
    for (i = 0; i < context->nb_streams; i++) {
        switch (context->streams[i]->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            video++;
            break;
        case AVMEDIA_TYPE_AUDIO:
            audio++;
            break;
        default:
            break;
        }
    }
    if (video != 1 || audio != 1) {
        fprintf(stderr, "mxl_demux_test: expected one video and one audio stream\n");
        return -1;
    }
    return 0;
}


/* Convert an absolute video grain index to the nearest audio sample index. */
static int64_t audio_index(uint64_t video_index)
{
    return av_rescale_q((int64_t)video_index, (AVRational){1001, 60000},
                        (AVRational){1, 48000});
}

static int write_video(mxlFlowWriter writer, uint64_t index)
{
    mxlGrainInfo grain = {0};
    uint8_t *payload = NULL;
    mxlStatus status;
    int opened = 0;
    int failed = -1;

    status = mxlFlowWriterOpenGrain(writer, index, &grain, &payload);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_demux_test: open video grain %" PRIu64 " failed (%d)\n",
                index, (int)status);
        goto finally;
    }
    opened = 1;
    if (!payload || !grain.grainSize || !grain.totalSlices) {
        fprintf(stderr, "mxl_demux_test: invalid video grain\n");
        goto finally;
    }
    memset(payload, 0, grain.grainSize);
    grain.flags = 0;
    grain.validSlices = grain.totalSlices;
    status = mxlFlowWriterCommitGrain(writer, &grain);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_demux_test: commit video grain %" PRIu64 " failed (%d)\n",
                index, (int)status);
        goto finally;
    }
    opened = 0;
    failed = 0;

finally:
    if (opened) {
        status = mxlFlowWriterCancelGrain(writer);
        if (status != MXL_STATUS_OK)
            fprintf(stderr, "mxl_demux_test: cancel video grain failed (%d)\n",
                    (int)status);
    }
    return failed;
}

/* Write audio samples in the half-open interval [start, end).  The
 * end index is exclusive. Split writes into chunks of at most
 * max_samples samples.
 */
static int write_audio(mxlFlowWriter writer, uint64_t start, uint64_t end,
                       size_t max_samples)
{
    while (start < end) {
        mxlMutableWrappedMultiBufferSlice payload = {0};
        uint64_t remaining = end - start;
        size_t count = remaining > max_samples ? max_samples : (size_t)remaining;
        size_t expected = count * sizeof(float);
        size_t channel, fragment;
        mxlStatus status;
        int opened = 0;
        int failed = -1;

        status = mxlFlowWriterOpenSamples(writer, start, count, &payload);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_demux_test: open audio samples %" PRIu64
                    " failed (%d)\n", start, (int)status);
            goto finally;
        }
        opened = 1;
        if (payload.count != AUDIO_CHANNELS ||
            payload.base.fragments[0].size > expected ||
            payload.base.fragments[1].size !=
                expected - payload.base.fragments[0].size) {
            fprintf(stderr, "mxl_demux_test: invalid audio payload\n");
            goto finally;
        }
        for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
            for (fragment = 0; fragment < 2; fragment++) {
                mxlMutableBufferSlice *slice = &payload.base.fragments[fragment];
                if (!slice->size)
                    continue;
                if (!slice->pointer ||
                    (channel && payload.stride < slice->size)) {
                    fprintf(stderr, "mxl_demux_test: invalid audio buffer\n");
                    goto finally;
                }
                memset((uint8_t *)slice->pointer + channel * payload.stride,
                       0, slice->size);
            }
        }
        status = mxlFlowWriterCommitSamples(writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_demux_test: commit audio samples %" PRIu64
                    " failed (%d)\n", start, (int)status);
            goto finally;
        }
        opened = 0;
        failed = 0;

finally:
        if (opened) {
            status = mxlFlowWriterCancelSamples(writer);
            if (status != MXL_STATUS_OK)
                fprintf(stderr, "mxl_demux_test: cancel audio samples failed (%d)\n",
                        (int)status);
        }
        if (failed)
            return -1;
        start += count;
    }
    return 0;
}

typedef enum TestScenario {
    STARTUP_DELAY_AUDIO,
    STARTUP_DELAY_VIDEO,
    INTERRUPT_VIDEO,
    INTERRUPT_AUDIO,
} TestScenario;

typedef struct ReadState {
    int video_stream;                    // Video stream index
    int audio_stream;                    // Audio stream index
    int64_t last_video_pts;              // Last received video PTS
    int64_t last_audio_end;              // End PTS of last audio packet
    unsigned total_reads;                // Total packets received
    unsigned video_packets;              // Video packets received
    unsigned audio_packets;              // Audio packets received
    int interrupted_stream;              // Delayed-start or midstream-interrupted stream
    int midstream_interrupted_stream;    // Midstream-interrupted stream, or -1
    unsigned startup_delay_frames;       // Startup delay in video frames
    int64_t pair_min_pts;                // Earliest interrupted PTS for A/V pairing
    const char *pair_label;              // "startup" or "recovery"
    int64_t last_before_pts;             // Last interrupted PTS before interruption
    unsigned last_before_read;           // Read number of last pre-gap packet
    unsigned last_before_count;          // Stream packet count at that read
    int64_t first_after_pts;             // First interrupted PTS after recovery
    unsigned recovered_packets;          // Interrupted packets received after recovery
    int recovery_logging;                // Enable verbose recovery logging
    double pair_difference_ms;           // First A/V pair timestamp difference
    int64_t first_interrupted_pts;       // First eligible interrupted packet PTS
    unsigned first_interrupted_read;     // Read number of first interrupted packet
    unsigned first_interrupted_count;    // Interrupted packet count at that read
    int64_t latest_uninterrupted_pts;    // Most recent uninterrupted-stream PTS
    unsigned latest_uninterrupted_read;  // Read number of latest uninterrupted packet
    unsigned latest_uninterrupted_count; // Uninterrupted-stream packet count at that read
    int64_t nearest_uninterrupted_pts;   // Closest uninterrupted-stream PTS to interrupted
    unsigned nearest_uninterrupted_read; // Read number of closest uninterrupted packet
    unsigned nearest_uninterrupted_count;// Uninterrupted-stream packet count at that read
    int pair_reported;                   // First A/V pair has been reported
} ReadState;

static int init_read_state(const AVFormatContext *context, ReadState *state,
                           TestScenario scenario)
{
    unsigned i;

    memset(state, 0, sizeof(*state));
    state->video_stream = -1;
    state->audio_stream = -1;
    state->last_video_pts = AV_NOPTS_VALUE;
    state->last_audio_end = AV_NOPTS_VALUE;
    state->first_interrupted_pts = AV_NOPTS_VALUE;
    state->last_before_pts = AV_NOPTS_VALUE;
    state->first_after_pts = AV_NOPTS_VALUE;
    state->latest_uninterrupted_pts = AV_NOPTS_VALUE;
    state->nearest_uninterrupted_pts = AV_NOPTS_VALUE;
    for (i = 0; i < context->nb_streams; i++) {
        const AVStream *stream = context->streams[i];
        switch (stream->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            state->video_stream = (int)i;
            if (av_cmp_q(stream->time_base, (AVRational){1001, 60000}))
                return -1;
            break;
        case AVMEDIA_TYPE_AUDIO:
            state->audio_stream = (int)i;
            if (av_cmp_q(stream->time_base, (AVRational){1, 48000}))
                return -1;
            break;
        default:
            return -1;
        }
    }
    state->interrupted_stream = scenario == STARTUP_DELAY_VIDEO ||
                                scenario == INTERRUPT_VIDEO ?
                                state->video_stream : state->audio_stream;
    state->midstream_interrupted_stream = scenario == INTERRUPT_VIDEO ||
                                          scenario == INTERRUPT_AUDIO ?
                                          state->interrupted_stream : -1;
    state->startup_delay_frames = state->midstream_interrupted_stream < 0 ?
                                  STARTUP_DELAY_FRAMES : 0;
    state->pair_min_pts = state->interrupted_stream == state->video_stream ?
        (int64_t)(START_INDEX + (state->midstream_interrupted_stream < 0 ?
                 STARTUP_DELAY_FRAMES : INTERRUPTION_END_FRAME)) :
        (int64_t)audio_index(START_INDEX +
                            (state->midstream_interrupted_stream < 0 ?
                             STARTUP_DELAY_FRAMES : INTERRUPTION_END_FRAME));
    state->pair_label = state->midstream_interrupted_stream < 0 ? "startup" : "recovery";
    return state->video_stream < 0 || state->audio_stream < 0 ? -1 : 0;
}

static double packet_time_ms(const AVFormatContext *context, int stream_index,
                             int64_t pts)
{
    return 1000.0 * (double)pts *
           av_q2d(context->streams[stream_index]->time_base);
}

static void log_packet(const ReadState *state, const AVFormatContext *context,
                       const AVPacket *packet)
{
    int video = packet->stream_index == state->video_stream;

    printf("mxl_demux_test: read #%u [video=%u audio=%u] %s "
           "%s=%" PRId64 " PTS=%.3f ms duration=%" PRId64 "\n",
           state->total_reads, state->video_packets, state->audio_packets,
           video ? "video" : "audio", video ? "grain" : "sample",
           packet->pts,
           packet_time_ms(context, packet->stream_index, packet->pts),
           packet->duration);
}

static void log_first_pair(ReadState *state, const AVFormatContext *context)
{
    int interrupted_video = state->interrupted_stream == state->video_stream;
    int64_t video_pts = interrupted_video ? state->first_interrupted_pts :
                                        state->nearest_uninterrupted_pts;
    int64_t audio_pts = interrupted_video ? state->nearest_uninterrupted_pts :
                                        state->first_interrupted_pts;
    unsigned video_read = interrupted_video ? state->first_interrupted_read :
                                          state->nearest_uninterrupted_read;
    unsigned audio_read = interrupted_video ? state->nearest_uninterrupted_read :
                                          state->first_interrupted_read;
    unsigned video_count = interrupted_video ? state->first_interrupted_count :
                                            state->nearest_uninterrupted_count;
    unsigned audio_count = interrupted_video ? state->nearest_uninterrupted_count :
                                            state->first_interrupted_count;
    double video_ms = packet_time_ms(context, state->video_stream, video_pts);
    double audio_ms = packet_time_ms(context, state->audio_stream, audio_pts);
    double difference_ms = audio_ms - video_ms;

    if (difference_ms > -0.0005 && difference_ms < 0.0005)
        difference_ms = 0.0;
    printf("mxl_demux_test: first A/V pair after %s %s:\n",
           interrupted_video ? "video" : "audio", state->pair_label);
    printf("mxl_demux_test:   video #%u (read #%u) grain=%" PRId64
           " PTS=%.3f ms\n", video_count, video_read, video_pts, video_ms);
    printf("mxl_demux_test:   audio #%u (read #%u) sample=%" PRId64
           " PTS=%.3f ms\n", audio_count, audio_read, audio_pts, audio_ms);
    printf("mxl_demux_test:   A-V PTS difference: %+.3f ms\n",
           difference_ms);
    state->pair_difference_ms = difference_ms;
    state->pair_reported = 1;
}

/*
 * Validate each demuxed packet against the expected MXL timeline:
 * PTS/DTS equality, positive duration, valid index range, and monotonic
 * timestamp progression. Reject packets from the interrupted interval.
 *
 * For midstream interruptions, record the last pre-interruption packet,
 * the first recovered packet, and subsequent recovery progress.
 *
 * For both delayed startup and midstream recovery, identify the first
 * eligible packet from the interrupted stream and find the closest PTS
 * from the uninterrupted stream. Record their A/V timestamp difference
 * for subsequent synchronization validation.
 *
 * Maintain packet counts and read order for diagnostics.
 *
 * Return 0 if the packet is valid, or -1 on validation failure.
 */
static int check_packet(ReadState *state, const AVFormatContext *context,
                        const AVPacket *packet, unsigned frame)
{
    int64_t pts = packet->pts;
    int video = packet->stream_index == state->video_stream;
    int audio = packet->stream_index == state->audio_stream;
    int interrupted = packet->stream_index == state->interrupted_stream;
    int valid = pts != AV_NOPTS_VALUE && packet->dts == pts &&
                packet->duration > 0;

    if (video) {
        int64_t start = (int64_t)START_INDEX +
                        (state->interrupted_stream == state->video_stream ?
                         state->startup_delay_frames : 0);
        valid = valid && pts >= start &&
                pts <= (int64_t)(START_INDEX + frame) &&
                (state->last_video_pts == AV_NOPTS_VALUE ||
                 pts > state->last_video_pts);
        if (state->midstream_interrupted_stream == state->video_stream)
            valid = valid &&
                    (pts < (int64_t)(START_INDEX + INTERRUPTION_START_FRAME) ||
                     pts >= (int64_t)(START_INDEX + INTERRUPTION_END_FRAME));
        if (valid) {
            state->last_video_pts = pts;
            state->video_packets++;
        }
    } else if (audio) {
        int64_t start = (int64_t)audio_index(START_INDEX +
                        (state->interrupted_stream == state->audio_stream ?
                         state->startup_delay_frames : 0));
        valid = valid && pts >= start &&
                pts <= (int64_t)audio_index(START_INDEX + frame + 1) &&
                (state->last_audio_end == AV_NOPTS_VALUE ||
                 pts >= state->last_audio_end);
        if (state->midstream_interrupted_stream == state->audio_stream)
            valid = valid &&
                    (pts < (int64_t)audio_index(START_INDEX +
                                               INTERRUPTION_START_FRAME) ||
                     pts >= (int64_t)audio_index(START_INDEX +
                                                INTERRUPTION_END_FRAME));
        if (valid) {
            state->last_audio_end = pts + packet->duration;
            state->audio_packets++;
        }
    } else {
        valid = 0;
    }
    if (!valid) {
        fprintf(stderr, "mxl_demux_test: invalid packet stream=%d pts=%" PRId64
                " dts=%" PRId64 " duration=%" PRId64 " at frame %u\n",
                packet->stream_index, pts, packet->dts, packet->duration, frame);
        return -1;
    }

    state->total_reads++;
    if (packet->stream_index == state->midstream_interrupted_stream) {
        if (pts < (int64_t)(video ?
                  START_INDEX + INTERRUPTION_START_FRAME :
                  audio_index(START_INDEX + INTERRUPTION_START_FRAME))) {
            state->last_before_pts = pts;
            state->last_before_read = state->total_reads;
            state->last_before_count = video ? state->video_packets :
                                                state->audio_packets;
        } else if (state->first_after_pts == AV_NOPTS_VALUE) {
            double before_ms, after_ms;
            unsigned count = video ? state->video_packets : state->audio_packets;

            state->first_after_pts = pts;
            state->recovery_logging = 0;
            after_ms = packet_time_ms(context, packet->stream_index, pts);
            printf("mxl_demux_test: first %s packet after recovery: "
                   "%s=%" PRId64 " PTS=%.3f ms (read #%u, %s #%u)\n",
                   video ? "video" : "audio", video ? "grain" : "sample",
                   pts, after_ms, state->total_reads,
                   video ? "video" : "audio", count);
            if (state->last_before_pts != AV_NOPTS_VALUE) {
                before_ms = packet_time_ms(context, packet->stream_index,
                                           state->last_before_pts);
                printf("mxl_demux_test:   last pre-interruption %s "
                       "%s=%" PRId64 " PTS=%.3f ms (read #%u, %s #%u)\n"
                       "mxl_demux_test:   received PTS jump: %.3f ms\n",
                       video ? "video" : "audio", video ? "grain" : "sample",
                       state->last_before_pts, before_ms,
                       state->last_before_read, video ? "video" : "audio",
                       state->last_before_count, after_ms - before_ms);
            }
        }
        if (state->first_after_pts != AV_NOPTS_VALUE)
            state->recovered_packets++;
    }
    if ((state->total_reads - 1) % 10 == 0 ||
        (interrupted && (video ? state->video_packets : state->audio_packets) == 1))
        log_packet(state, context, packet);

    if (interrupted && pts >= state->pair_min_pts &&
        state->first_interrupted_pts == AV_NOPTS_VALUE) {
        state->first_interrupted_pts = pts;
        state->first_interrupted_read = state->total_reads;
        state->first_interrupted_count = video ? state->video_packets :
                                             state->audio_packets;
        state->nearest_uninterrupted_pts = state->latest_uninterrupted_pts;
        state->nearest_uninterrupted_read = state->latest_uninterrupted_read;
        state->nearest_uninterrupted_count = state->latest_uninterrupted_count;
    } else if (!interrupted && !state->pair_reported) {
        if (state->first_interrupted_pts != AV_NOPTS_VALUE) {
            double interrupted_ms = packet_time_ms(context, state->interrupted_stream,
                                                   state->first_interrupted_pts);
            double uninterrupted_ms = packet_time_ms(context, packet->stream_index, pts);
            double nearest_ms = state->nearest_uninterrupted_pts == AV_NOPTS_VALUE ?
                0.0 : packet_time_ms(context, packet->stream_index,
                                     state->nearest_uninterrupted_pts);
            double difference = uninterrupted_ms - interrupted_ms;
            double nearest_difference = nearest_ms - interrupted_ms;

            if (state->nearest_uninterrupted_pts == AV_NOPTS_VALUE ||
                difference * difference < nearest_difference * nearest_difference) {
                state->nearest_uninterrupted_pts = pts;
                state->nearest_uninterrupted_read = state->total_reads;
                state->nearest_uninterrupted_count = video ? state->video_packets :
                                                    state->audio_packets;
            }
            if (uninterrupted_ms >= interrupted_ms &&
                state->nearest_uninterrupted_pts != AV_NOPTS_VALUE)
                log_first_pair(state, context);
        }
        state->latest_uninterrupted_pts = pts;
        state->latest_uninterrupted_read = state->total_reads;
        state->latest_uninterrupted_count = video ? state->video_packets :
                                           state->audio_packets;
    }
    return 0;
}

/*
 * Drain available packets from the non-blocking MXL demuxer using
 * round-robin reads across the audio and video flows. Validate each
 * received packet and update the read state.
 *
 * Attempt up to 32 reads per call, stopping after four consecutive
 * EAGAIN results. EAGAIN indicates that no packet is currently available
 * and is not treated as an error. Successful reads reset the EAGAIN count.
 *
 * Temporarily enable verbose demuxer logging during interruption
 * recovery, restoring the previous log level after each read.
 *
 * Return 0 on success, or -1 on allocation, read, or validation failure.
 */
static int read_available(AVFormatContext *context, ReadState *state,
                          unsigned frame)
{
    AVPacket *packet = av_packet_alloc();
    char error[AV_ERROR_MAX_STRING_SIZE];
    unsigned attempts, empty = 0;
    int failed = -1;

    if (!packet)
        return -1;
    for (attempts = 0; attempts < 32 && empty < 4; attempts++) {
        int level;
        int rc;

        if (state->recovery_logging) {
            level = av_log_get_level();
            av_log_set_level(AV_LOG_VERBOSE);
        }
        rc = av_read_frame(context, packet);
        if (state->recovery_logging)
            av_log_set_level(level);
        if (rc == AVERROR(EAGAIN)) {
            empty++;
            continue;
        }
        if (rc < 0) {
            av_strerror(rc, error, sizeof(error));
            fprintf(stderr, "mxl_demux_test: read packet failed: %s\n", error);
            goto finally;
        }
        empty = 0;
        rc = check_packet(state, context, packet, frame);
        av_packet_unref(packet);
        if (rc)
            goto finally;
    }
    failed = 0;

finally:
    av_packet_free(&packet);
    return failed;
}

/*
 * Verify correct demuxer behavior when one MXL flow starts later than
 * the other. Begin writing the uninterrupted flow immediately and delay
 * the interrupted flow by 30 video frames (approximately 500 ms).
 *
 * Advance both flows on the same logical media timeline, reading and
 * validating available packets after each frame. Verify that the
 * uninterrupted stream produces packets before the delayed stream starts,
 * and that no packets arrive prematurely from the delayed stream.
 *
 * After startup, verify that both streams deliver sufficient packets,
 * that an A/V timestamp pair is observed, and that the demuxer keeps
 * up with the writer. Packet timestamp validity and monotonicity are
 * checked by check_packet().
 *
 * Return 0 if all checks pass, or -1 on failure.
 */
static int test_startup_gap(TestSource *source, AVFormatContext *context,
                            TestScenario scenario)
{
    ReadState reader;
    size_t max_audio_samples;
    mxlStatus status;
    unsigned frame;
    int delay_video = scenario == STARTUP_DELAY_VIDEO;

    if (init_read_state(context, &reader, scenario)) {
        fprintf(stderr, "mxl_demux_test: invalid stream time bases\n");
        return -1;
    }
    status = mxlFlowWriterGetMaxWriteLengthSamples(source->audio,
                                                   &max_audio_samples);
    if (status != MXL_STATUS_OK || !max_audio_samples) {
        fprintf(stderr, "mxl_demux_test: audio write limit unavailable (%d)\n",
                (int)status);
        return -1;
    }

    printf("mxl_demux_test: startup gap test (delayed %s)\n",
           delay_video ? "video" : "audio");
    if (delay_video)
        printf("mxl_demux_test: starting audio at sample index %" PRId64 "\n",
               audio_index(START_INDEX));
    else
        printf("mxl_demux_test: starting video at grain index %" PRIu64 "\n",
               START_INDEX);
    printf("mxl_demux_test: delaying %s by %u video frames (%.1f ms)\n",
           delay_video ? "video" : "audio", STARTUP_DELAY_FRAMES,
           STARTUP_DELAY_FRAMES * 1001.0 / 60.0);

    for (frame = 0; frame < TEST_FRAMES; frame++) {
        uint64_t index = START_INDEX + frame;

        if (frame == STARTUP_DELAY_FRAMES &&
            !(delay_video ? reader.audio_packets : reader.video_packets)) {
            fprintf(stderr, "mxl_demux_test: reader did not receive %s "
                    "before delayed startup\n", delay_video ? "audio" : "video");
            return -1;
        }
        if (frame >= (delay_video ? STARTUP_DELAY_FRAMES : 0)) {
            if (delay_video && frame == STARTUP_DELAY_FRAMES)
                printf("mxl_demux_test: starting video at grain index %" PRIu64 "\n",
                       index);
            if (write_video(source->video, index))
                return -1;
        }
        if (frame >= (delay_video ? 0 : STARTUP_DELAY_FRAMES)) {
            if (!delay_video && frame == STARTUP_DELAY_FRAMES)
                printf("mxl_demux_test: starting audio at sample index %" PRId64 "\n",
                       audio_index(index));
            if (write_audio(source->audio, audio_index(index),
                            audio_index(index + 1), max_audio_samples))
                return -1;
        }
        if (read_available(context, &reader, frame))
            return -1;
        if (frame < STARTUP_DELAY_FRAMES &&
            (delay_video ? reader.video_packets : reader.audio_packets)) {
            fprintf(stderr, "mxl_demux_test: %s arrived before startup\n",
                    delay_video ? "video" : "audio");
            return -1;
        }
    }
    if (reader.video_packets < 8 || reader.audio_packets < 8) {
        fprintf(stderr, "mxl_demux_test: too few packets (video=%u, audio=%u)\n",
                reader.video_packets, reader.audio_packets);
        return -1;
    }
    if (!reader.pair_reported) {
        fprintf(stderr, "mxl_demux_test: no post-startup A/V pair observed\n");
        return -1;
    }
    if (reader.last_video_pts == AV_NOPTS_VALUE ||
        reader.last_audio_end == AV_NOPTS_VALUE ||
        reader.last_video_pts < (int64_t)(START_INDEX + TEST_FRAMES - 6) ||
        reader.last_audio_end <
            (int64_t)audio_index(START_INDEX + TEST_FRAMES - 6)) {
        fprintf(stderr, "mxl_demux_test: reader did not keep up with writer\n");
        return -1;
    }
    printf("mxl_demux_test: verified %u video and %u audio packets "
           "(PTS/DTS, timeline, monotonicity)\n",
           reader.video_packets, reader.audio_packets);
    return 0;
}

/*
 * Verify demuxer recovery from a midstream audio or video interruption.
 * Begin with synchronized A/V, then stop writing one flow for 30 frames
 * (approximately 500 ms) while the other continues uninterrupted.
 * Resume the interrupted flow at its current media index, leaving a gap.
 *
 * Validate packets throughout the test, confirming that both streams
 * are established before interruption and that the uninterrupted stream
 * continues delivering packets during the gap.
 *
 * After resumption, verify recovery, timestamp monotonicity, and A/V
 * synchronization within one video frame (approximately 16.7 ms).
 * Confirm that both streams progress close to the end of the written
 * timeline.
 *
 * Return 0 if all checks pass, or -1 on failure.
 */
static int test_interruption(TestSource *source, AVFormatContext *context,
                             TestScenario scenario)
{
    ReadState reader;
    size_t max_audio_samples;
    mxlStatus status;
    unsigned continuous_before = 0;
    unsigned frame;
    int interrupt_video = scenario == INTERRUPT_VIDEO;

    if (init_read_state(context, &reader, scenario)) {
        fprintf(stderr, "mxl_demux_test: invalid stream time bases\n");
        return -1;
    }
    status = mxlFlowWriterGetMaxWriteLengthSamples(source->audio,
                                                   &max_audio_samples);
    if (status != MXL_STATUS_OK || !max_audio_samples) {
        fprintf(stderr, "mxl_demux_test: audio write limit unavailable (%d)\n",
                (int)status);
        return -1;
    }

    printf("mxl_demux_test: %s interruption test\n",
           interrupt_video ? "video" : "audio");
    printf("mxl_demux_test: starting synchronized A/V at video grain %" PRIu64
           " and audio sample %" PRId64 "\n",
           START_INDEX, audio_index(START_INDEX));
    printf("mxl_demux_test: normal A/V for %u frames; interrupt %s for %u "
           "frames (%.1f ms); recover for %u frames\n",
           INTERRUPTION_START_FRAME, interrupt_video ? "video" : "audio",
           INTERRUPTION_END_FRAME - INTERRUPTION_START_FRAME,
           (INTERRUPTION_END_FRAME - INTERRUPTION_START_FRAME) * 1001.0 / 60.0,
           INTERRUPTION_TEST_FRAMES - INTERRUPTION_END_FRAME);

    for (frame = 0; frame < INTERRUPTION_TEST_FRAMES; frame++) {
        uint64_t index = START_INDEX + frame;
        int interrupted = frame >= INTERRUPTION_START_FRAME &&
                          frame < INTERRUPTION_END_FRAME;

        if (frame == INTERRUPTION_START_FRAME) {
            if (reader.video_packets < 8 || reader.audio_packets < 8 ||
                reader.last_before_pts == AV_NOPTS_VALUE) {
                fprintf(stderr, "mxl_demux_test: insufficient A/V reception "
                        "before interruption (video=%u audio=%u)\n",
                        reader.video_packets, reader.audio_packets);
                return -1;
            }
            continuous_before = interrupt_video ? reader.audio_packets :
                                                 reader.video_packets;
            printf("mxl_demux_test: interrupting %s at %s index %" PRIu64 "\n",
                   interrupt_video ? "video" : "audio",
                   interrupt_video ? "grain" : "sample",
                   interrupt_video ? index : (uint64_t)audio_index(index));
            printf("mxl_demux_test: continuing %s during interruption\n",
                   interrupt_video ? "audio" : "video");
        }
        if (frame == INTERRUPTION_END_FRAME) {
            unsigned continuous_now = interrupt_video ? reader.audio_packets :
                                                        reader.video_packets;
            if (continuous_now < continuous_before + 12) {
                fprintf(stderr, "mxl_demux_test: uninterrupted %s did not "
                        "make progress during gap (%u packets)\n",
                        interrupt_video ? "audio" : "video",
                        continuous_now - continuous_before);
                return -1;
            }
            printf("mxl_demux_test: uninterrupted %s delivered %u packets "
                   "during gap\n", interrupt_video ? "audio" : "video",
                   continuous_now - continuous_before);
            printf("mxl_demux_test: resuming %s at %s index %" PRIu64 "\n",
                   interrupt_video ? "video" : "audio",
                   interrupt_video ? "grain" : "sample",
                   interrupt_video ? index : (uint64_t)audio_index(index));
            reader.recovery_logging = 1;
        }
        if (!(interrupted && interrupt_video) &&
            write_video(source->video, index))
            return -1;
        if (!(interrupted && !interrupt_video) &&
            write_audio(source->audio, audio_index(index),
                        audio_index(index + 1), max_audio_samples))
            return -1;
        if (read_available(context, &reader, frame))
            return -1;
    }

    if (reader.recovered_packets < 8 || !reader.pair_reported) {
        fprintf(stderr, "mxl_demux_test: insufficient %s recovery "
                "(post-gap packets=%u, A/V pair=%s)\n",
                interrupt_video ? "video" : "audio",
                reader.recovered_packets, reader.pair_reported ? "yes" : "no");
        return -1;
    }
    if (reader.pair_difference_ms < -1001.0 / 60.0 ||
        reader.pair_difference_ms > 1001.0 / 60.0) {
        fprintf(stderr, "mxl_demux_test: post-recovery A/V PTS "
                "difference too large: %.3f ms\n",
                reader.pair_difference_ms);
        return -1;
    }
    if (reader.last_video_pts < (int64_t)(START_INDEX +
                                          INTERRUPTION_TEST_FRAMES - 6) ||
        reader.last_audio_end < (int64_t)audio_index(START_INDEX +
                                                    INTERRUPTION_TEST_FRAMES - 6)) {
        fprintf(stderr, "mxl_demux_test: reader did not keep up after recovery\n");
        return -1;
    }
    printf("mxl_demux_test: verified %u video and %u audio packets "
           "(%u recovered %s packets; PTS/DTS, discontinuity, monotonicity)\n",
           reader.video_packets, reader.audio_packets, reader.recovered_packets,
           interrupt_video ? "video" : "audio");
    return 0;
}

int main(int argc, char **argv)
{
    TestDomain domain = {0};
    TestSource source = {0};
    AVFormatContext *context = NULL;
    TestScenario scenario;
    int failed = 0;

    if (argc != 3 || argv[1][0] != '/' || !argv[1][1] ||
        (strcmp(argv[2], "delayed-audio") &&
         strcmp(argv[2], "delayed-video") &&
         strcmp(argv[2], "video-interruption") &&
         strcmp(argv[2], "audio-interruption"))) {
        fprintf(stderr, "Usage: %s absolute_domain_dir "
                "{delayed-audio|delayed-video|video-interruption|"
                "audio-interruption}\n", argv[0]);
        puts("mxl_demux_test: FAIL");
        return EXIT_FAILURE;
    }
    if (!strcmp(argv[2], "delayed-video"))
        scenario = STARTUP_DELAY_VIDEO;
    else if (!strcmp(argv[2], "video-interruption"))
        scenario = INTERRUPT_VIDEO;
    else if (!strcmp(argv[2], "audio-interruption"))
        scenario = INTERRUPT_AUDIO;
    else
        scenario = STARTUP_DELAY_AUDIO;
    printf("mxl_demux_test: creating MXL domain %s\n", argv[1]);
    if (create_domain(&domain, argv[1])) {
        failed = 1;
        goto finally;
    }
    source.instance = mxlCreateInstance(argv[1], NULL);
    if (!source.instance) {
        fprintf(stderr, "mxl_demux_test: mxlCreateInstance failed\n");
        failed = 1;
        goto finally;
    }
    if (create_flow(source.instance, video_flow_def, "video", &source.video) ||
        create_flow(source.instance, audio_flow_def, "audio", &source.audio)) {
        failed = 1;
        goto finally;
    }
    puts("mxl_demux_test: created video and audio flows");
    puts("mxl_demux_test: opening MXL demuxer (non-blocking)");
    if (open_demuxer(&context, argv[1]) || check_streams(context)) {
        failed = 1;
        goto finally;
    }
    puts("mxl_demux_test: initialized audio and video streams");
    if ((scenario == INTERRUPT_VIDEO || scenario == INTERRUPT_AUDIO ?
         test_interruption(&source, context, scenario) :
         test_startup_gap(&source, context, scenario))) {
        failed = 1;
        goto finally;
    }

finally:
    avformat_close_input(&context);
    if (close_source(&source))
        failed = 1;
    if (remove_domain(&domain))
        failed = 1;
    if (!failed)
        puts("mxl_demux_test: cleanup complete");
    printf("mxl_demux_test: %s: %s\n", argv[2], failed ? "FAIL" : "PASS");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
