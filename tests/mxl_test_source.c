/*
 * Minimal MXL test source for FFmpeg regression tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Phase 2: paced, zero-filled video and audio at absolute MXL indices.
 * Disruption simulation is reserved for Phase 3.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>

#define NS_PER_MS UINT64_C(1000000)
#define VIDEO_RATE_NUM 60000
#define VIDEO_RATE_DEN 1001
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_SAMPLES_PER_FIVE_FRAMES UINT64_C(4004)
#define AUDIO_CHANNELS 2

#define VIDEO_FLOW_ID "932dc2da-0c7b-4c63-81d8-a48bb004c709"
#define AUDIO_FLOW_ID "40178de3-71a0-452a-a541-dd14979ae7c1"

static const char video_flow_def[] =
    "{"
    "\"id\":\"" VIDEO_FLOW_ID "\","
    "\"label\":\"MXL test source video\","
    "\"description\":\"MXL test source video flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Test Source:Video\"]},"
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
    "\"label\":\"MXL test source audio\","
    "\"description\":\"MXL test source audio flow\","
    "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"MXL Test Source:Audio\"]},"
    "\"format\":\"urn:x-nmos:format:audio\","
    "\"parents\":[],"
    "\"media_type\":\"audio/float32\","
    "\"sample_rate\":{\"numerator\":48000},"
    "\"channel_count\":2,"
    "\"bit_depth\":32"
    "}";

typedef struct TestOptions {
    const char *domain_dir;
    uint64_t start_index;
    double duration;
    double speed;
    uint64_t history_duration;
    uint64_t catchup_limit;
    const char *signal;
    double video_gap_rate;
    double video_gap_mean;
    double audio_gap_rate;
    double audio_gap_mean;
} TestOptions;

typedef enum SignalType {
    SIGNAL_ZERO
} SignalType;

typedef struct TestSignal {
    SignalType type;
} TestSignal;

typedef struct VideoSink {
    mxlFlowWriter writer;
    uint64_t next_index;
} VideoSink;

typedef struct AudioSink {
    mxlFlowWriter writer;
    uint64_t next_index;
    size_t max_write_samples;
} AudioSink;

typedef struct MXLTestSource {
    mxlInstance instance;
    VideoSink video;
    AudioSink audio;
} MXLTestSource;

typedef struct MediaClock {
    struct timespec start;
    double speed;
} MediaClock;

static int parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!text[0] || text[0] == '-')
        return -1;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end || parsed > UINT64_MAX)
        return -1;

    *value = (uint64_t)parsed;
    return 0;
}

static int parse_double(const char *text, double *value)
{
    char *end;
    double parsed;

    if (!text[0])
        return -1;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno || *end || !isfinite(parsed))
        return -1;

    *value = parsed;
    return 0;
}

static int parse_options(int argc, char **argv, TestOptions *opts)
{
    if (argc != 12) {
        fprintf(stderr,
                "Usage: %s domain_dir start_index duration speed "
                "history_duration catchup_limit signal "
                "video_gap_rate video_gap_mean audio_gap_rate audio_gap_mean\n",
                argv[0]);
        return -1;
    }

    memset(opts, 0, sizeof(*opts));
    opts->domain_dir = argv[1];
    opts->signal = argv[7];

    if (!opts->domain_dir[0] ||
        parse_uint64(argv[2], &opts->start_index) ||
        parse_double(argv[3], &opts->duration) ||
        parse_double(argv[4], &opts->speed) ||
        parse_uint64(argv[5], &opts->history_duration) ||
        parse_uint64(argv[6], &opts->catchup_limit) ||
        strcmp(opts->signal, "zero") ||
        parse_double(argv[8], &opts->video_gap_rate) ||
        parse_double(argv[9], &opts->video_gap_mean) ||
        parse_double(argv[10], &opts->audio_gap_rate) ||
        parse_double(argv[11], &opts->audio_gap_mean) ||
        opts->duration <= 0 || opts->speed <= 0 ||
        opts->history_duration == 0 ||
        opts->history_duration > UINT64_MAX / NS_PER_MS ||
        opts->catchup_limit == 0 ||
        opts->video_gap_rate < 0 || opts->video_gap_mean < 0 ||
        opts->audio_gap_rate < 0 || opts->audio_gap_mean < 0) {
        fprintf(stderr, "mxl_test_source: invalid arguments\n");
        return -1;
    }

    return 0;
}

static int write_domain_options(const char *domain, uint64_t history_ms)
{
    static const char filename[] = "/options.json";
    size_t length = strlen(domain);
    char *path;
    FILE *file;
    struct stat st;
    int failed = 0;

    if (mkdir(domain, 0777) && errno != EEXIST) {
        perror("mxl_test_source: mkdir");
        return -1;
    }
    if (stat(domain, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mxl_test_source: invalid domain directory: %s\n", domain);
        return -1;
    }
    if (length > SIZE_MAX - sizeof(filename))
        return -1;

    path = malloc(length + sizeof(filename));
    if (!path)
        return -1;
    sprintf(path, "%s%s", domain, filename);

    file = fopen(path, "w");
    if (!file) {
        perror("mxl_test_source: options.json");
        free(path);
        return -1;
    }

    if (fprintf(file,
                "{\n  \"urn:x-mxl:option:history_duration/v1.0\": "
                "%" PRIu64 "\n}\n", history_ms * NS_PER_MS) < 0)
        failed = 1;
    if (fclose(file))
        failed = 1;
    if (failed)
        fprintf(stderr, "mxl_test_source: failed to write %s\n", path);

    free(path);
    return failed ? -1 : 0;
}

static int create_flow(mxlInstance instance, const char *definition,
                       const char *name, mxlFlowWriter *writer)
{
    mxlStatus status = mxlCreateFlowWriter(instance, definition, NULL,
                                           writer, NULL, NULL);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_test_source: create %s flow failed (%d)\n",
                name, (int)status);
        return -1;
    }
    return 0;
}

static int video_to_audio_index(uint64_t video_index, uint64_t *audio_index)
{
    uint64_t whole = video_index / 5;
    uint64_t fraction = (video_index % 5) * AUDIO_SAMPLES_PER_FIVE_FRAMES / 5;

    if (whole > (UINT64_MAX - fraction) / AUDIO_SAMPLES_PER_FIVE_FRAMES)
        return -1;
    *audio_index = whole * AUDIO_SAMPLES_PER_FIVE_FRAMES + fraction;
    return 0;
}

static int init_sinks(MXLTestSource *source, uint64_t start_index)
{
    mxlStatus status;

    source->video.next_index = start_index;
    if (video_to_audio_index(start_index, &source->audio.next_index)) {
        fprintf(stderr, "mxl_test_source: start index exceeds audio index range\n");
        return -1;
    }

    status = mxlFlowWriterGetMaxWriteLengthSamples(source->audio.writer,
                                                    &source->audio.max_write_samples);
    if (status != MXL_STATUS_OK || !source->audio.max_write_samples) {
        fprintf(stderr, "mxl_test_source: audio write limit unavailable (%d)\n",
                (int)status);
        return -1;
    }
    return 0;
}

static int fill_video(const TestSignal *signal, uint64_t index,
                      uint8_t *payload, size_t size)
{
    (void)index;
    if (signal->type != SIGNAL_ZERO)
        return -1;
    memset(payload, 0, size);
    return 0;
}

static int fill_audio(const TestSignal *signal, uint64_t index, size_t samples,
                      mxlMutableWrappedMultiBufferSlice *payload)
{
    size_t expected, actual, fragment, channel;

    (void)index;
    if (signal->type != SIGNAL_ZERO || payload->count != AUDIO_CHANNELS ||
        samples > SIZE_MAX / sizeof(float))
        return -1;

    expected = samples * sizeof(float);
    if (payload->base.fragments[0].size > expected ||
        payload->base.fragments[1].size > expected - payload->base.fragments[0].size)
        return -1;
    actual = payload->base.fragments[0].size + payload->base.fragments[1].size;
    if (actual != expected)
        return -1;

    for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
        for (fragment = 0; fragment < 2; fragment++) {
            mxlMutableBufferSlice *slice = &payload->base.fragments[fragment];
            if (slice->size) {
                if (!slice->pointer ||
                    (channel && payload->stride < slice->size))
                    return -1;
                memset((uint8_t *)slice->pointer + channel * payload->stride,
                       0, slice->size);
            }
        }
    }
    return 0;
}

static int write_video(VideoSink *sink, const TestSignal *signal)
{
    mxlGrainInfo grain = {0};
    uint8_t *payload = NULL;
    mxlStatus status;
    int opened = 0;
    int failed = -1;

    status = mxlFlowWriterOpenGrain(sink->writer, sink->next_index,
                                    &grain, &payload);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_test_source: open video grain %" PRIu64 " failed (%d)\n",
                sink->next_index, (int)status);
        goto finally;
    }
    opened = 1;
    if (!payload || !grain.grainSize || !grain.totalSlices ||
        fill_video(signal, sink->next_index, payload, grain.grainSize)) {
        fprintf(stderr, "mxl_test_source: invalid video grain payload\n");
        goto finally;
    }

    grain.flags = 0;
    grain.validSlices = grain.totalSlices;
    status = mxlFlowWriterCommitGrain(sink->writer, &grain);
    if (status != MXL_STATUS_OK) {
        fprintf(stderr, "mxl_test_source: commit video grain %" PRIu64 " failed (%d)\n",
                sink->next_index, (int)status);
        goto finally;
    }
    opened = 0;
    sink->next_index++;
    failed = 0;

finally:
    if (opened) {
        status = mxlFlowWriterCancelGrain(sink->writer);
        if (status != MXL_STATUS_OK)
            fprintf(stderr, "mxl_test_source: cancel video grain failed (%d)\n",
                    (int)status);
    }
    return failed;
}

static int write_audio(AudioSink *sink, const TestSignal *signal,
                       uint64_t end_index)
{
    while (sink->next_index < end_index) {
        mxlMutableWrappedMultiBufferSlice payload = {0};
        uint64_t remaining = end_index - sink->next_index;
        size_t count = remaining > sink->max_write_samples ?
                       sink->max_write_samples : (size_t)remaining;
        mxlStatus status;
        int opened = 0;
        int failed = -1;

        status = mxlFlowWriterOpenSamples(sink->writer, sink->next_index,
                                          count, &payload);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: open audio samples %" PRIu64
                    " (%zu) failed (%d)\n", sink->next_index, count, (int)status);
            goto finally;
        }
        opened = 1;
        if (fill_audio(signal, sink->next_index, count, &payload)) {
            fprintf(stderr, "mxl_test_source: invalid audio sample payload\n");
            goto finally;
        }
        status = mxlFlowWriterCommitSamples(sink->writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: commit audio samples %" PRIu64
                    " failed (%d)\n", sink->next_index, (int)status);
            goto finally;
        }
        opened = 0;
        sink->next_index += count;
        failed = 0;

finally:
        if (opened) {
            status = mxlFlowWriterCancelSamples(sink->writer);
            if (status != MXL_STATUS_OK)
                fprintf(stderr, "mxl_test_source: cancel audio samples failed (%d)\n",
                        (int)status);
        }
        if (failed)
            return -1;
    }
    return 0;
}

static int write_av_frame(MXLTestSource *source, const TestSignal *signal)
{
    uint64_t audio_end;

    if (source->video.next_index == UINT64_MAX ||
        video_to_audio_index(source->video.next_index + 1, &audio_end) ||
        audio_end < source->audio.next_index) {
        fprintf(stderr, "mxl_test_source: media index overflow\n");
        return -1;
    }
    if (write_video(&source->video, signal) ||
        write_audio(&source->audio, signal, audio_end))
        return -1;
    return 0;
}

static int start_clock(MediaClock *clock, double speed)
{
    clock->speed = speed;
    if (clock_gettime(CLOCK_MONOTONIC, &clock->start)) {
        perror("mxl_test_source: clock_gettime");
        return -1;
    }
    return 0;
}

/* Return 0 when due, 1 on SIGINT/SIGTERM, -1 on failure. */
static int wait_for_media_time(const MediaClock *clock, double media_seconds,
                               const sigset_t *stop_signals)
{
    for (;;) {
        struct timespec now, timeout;
        double elapsed, remaining;
        int signum;

        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            perror("mxl_test_source: clock_gettime");
            return -1;
        }
        elapsed = (double)(now.tv_sec - clock->start.tv_sec) +
                  (double)(now.tv_nsec - clock->start.tv_nsec) / 1e9;
        remaining = media_seconds / clock->speed - elapsed;
        if (remaining < 0)
            remaining = 0;
        if (remaining > 1.0)
            remaining = 1.0;
        timeout.tv_sec = (time_t)remaining;
        timeout.tv_nsec = (long)((remaining - (double)timeout.tv_sec) * 1e9);

        signum = sigtimedwait(stop_signals, NULL, &timeout);
        if (signum == SIGINT || signum == SIGTERM)
            return 1;
        if (signum < 0 && errno != EAGAIN && errno != EINTR) {
            perror("mxl_test_source: sigtimedwait");
            return -1;
        }
        if (remaining == 0 && signum < 0 && errno == EAGAIN)
            return 0;
    }
}

static int generate_media(MXLTestSource *source, const TestOptions *opts,
                          const sigset_t *stop_signals)
{
    const TestSignal signal = { SIGNAL_ZERO };
    MediaClock clock;
    uint64_t frames = 0;
    uint64_t first_audio = source->audio.next_index;
    int result;

    if (start_clock(&clock, opts->speed))
        return -1;

    for (;;) {
        double media_seconds = (double)frames * VIDEO_RATE_DEN / VIDEO_RATE_NUM;
        if (media_seconds >= opts->duration)
            break;

        result = wait_for_media_time(&clock, media_seconds, stop_signals);
        if (result < 0)
            return -1;
        if (result > 0)
            break;
        if (write_av_frame(source, &signal))
            return -1;
        frames++;
    }

    fprintf(stderr, "mxl_test_source: wrote %" PRIu64 " video grains and %" PRIu64
            " audio samples\n", frames, source->audio.next_index - first_audio);
    return 0;
}

static int close_source(MXLTestSource *source)
{
    mxlStatus status;
    int failed = 0;

    if (source->video.writer) {
        status = mxlReleaseFlowWriter(source->instance, source->video.writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: release video flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->video.writer = NULL;
    }
    if (source->audio.writer) {
        status = mxlReleaseFlowWriter(source->instance, source->audio.writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: release audio flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->audio.writer = NULL;
    }
    if (source->instance) {
        status = mxlDestroyInstance(source->instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: destroy instance failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->instance = NULL;
    }
    return failed ? -1 : 0;
}

int main(int argc, char **argv)
{
    TestOptions opts;
    MXLTestSource source = {0};
    sigset_t stop_signals;
    int failed = 0;

    if (parse_options(argc, argv, &opts))
        return EXIT_FAILURE;

    /* Block termination signals before MXL can start internal threads. */
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &stop_signals, NULL)) {
        perror("mxl_test_source: sigprocmask");
        return EXIT_FAILURE;
    }

    if (write_domain_options(opts.domain_dir, opts.history_duration))
        return EXIT_FAILURE;

    source.instance = mxlCreateInstance(opts.domain_dir, NULL);
    if (!source.instance) {
        fprintf(stderr, "mxl_test_source: mxlCreateInstance failed\n");
        failed = 1;
        goto finally;
    }
    if (create_flow(source.instance, video_flow_def, "video",
                    &source.video.writer) ||
        create_flow(source.instance, audio_flow_def, "audio",
                    &source.audio.writer)) {
        failed = 1;
        goto finally;
    }

    if (init_sinks(&source, opts.start_index)) {
        failed = 1;
        goto finally;
    }

    /* The test harness can start its reader after this notification. */
    puts("mxl_test_source: flows ready");
    fflush(stdout);

    if (generate_media(&source, &opts, &stop_signals))
        failed = 1;

finally:
    if (close_source(&source))
        failed = 1;
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
