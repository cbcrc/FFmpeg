/*
 * Minimal MXL test source for FFmpeg regression tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Phase 3: paced, zero-filled audio/video with independent random gaps
 * and bounded catch-up on the original absolute media timeline.
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
    uint64_t written;
    uint64_t skipped;
} VideoSink;

typedef struct AudioSink {
    mxlFlowWriter writer;
    uint64_t next_index;
    uint64_t next_frame;
    size_t max_write_samples;
    uint64_t written;
    uint64_t skipped;
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

typedef struct GapSchedule {
    const char *name;
    uint64_t rng;
    double interval_mean;
    double duration_mean;
    double next_start;
    double active_end;
    uint64_t count;
    int active;
} GapSchedule;

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
    sink->written++;
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
        sink->written += count;
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

static int current_media_time(const MediaClock *clock, double *seconds)
{
    struct timespec now;
    double elapsed;

    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        perror("mxl_test_source: clock_gettime");
        return -1;
    }
    elapsed = (double)(now.tv_sec - clock->start.tv_sec) +
              (double)(now.tv_nsec - clock->start.tv_nsec) / 1e9;
    *seconds = elapsed * clock->speed;
    if (*seconds < 0 || !isfinite(*seconds)) {
        fprintf(stderr, "mxl_test_source: invalid media clock\n");
        return -1;
    }
    return 0;
}

static double frame_time(uint64_t frame)
{
    return (double)frame * VIDEO_RATE_DEN / VIDEO_RATE_NUM;
}

/* Saturating conversion: a 60000/1001 frame occupies 1001/60 ms. */
static uint64_t frames_per_ms(uint64_t milliseconds, unsigned numerator)
{
    uint64_t whole = milliseconds / 1001;
    uint64_t fraction = (milliseconds % 1001) * numerator / 1001;

    if (whole > (UINT64_MAX - fraction) / numerator)
        return UINT64_MAX;
    return whole * numerator + fraction;
}

static uint64_t random_u64(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static double random_exponential(uint64_t *state, double mean)
{
    /* A uniform value strictly inside (0,1) avoids log(0). */
    double u = (double)(random_u64(state) >> 11) * 0x1p-53 + 0x1p-54;
    return -mean * log(u);
}

static void init_gap(GapSchedule *gap, const char *name, uint64_t seed,
                     double rate_per_minute, double mean_duration_ms)
{
    memset(gap, 0, sizeof(*gap));
    gap->name = name;
    gap->rng = seed;
    gap->next_start = INFINITY;

    if (rate_per_minute > 0 && mean_duration_ms > 0) {
        gap->interval_mean = 60.0 / rate_per_minute;
        gap->duration_mean = mean_duration_ms / 1000.0;
        gap->next_start = random_exponential(&gap->rng, gap->interval_mean);
    }
}

static void update_gap(GapSchedule *gap, double now)
{
    while (gap->next_start <= now) {
        double start = gap->next_start;
        double length = random_exponential(&gap->rng, gap->duration_mean);
        double end = start + length;
        double next = start + random_exponential(&gap->rng, gap->interval_mean);

        if (end > gap->active_end)
            gap->active_end = end;
        gap->next_start = next > start ? next : nextafter(start, INFINITY);
        gap->count++;
        fprintf(stderr, "mxl_test_source: %s gap at %.3f s for %.1f ms\n",
                gap->name, start, length * 1000.0);
    }

    if (gap->active != (now < gap->active_end)) {
        gap->active = now < gap->active_end;
        if (!gap->active)
            fprintf(stderr, "mxl_test_source: %s resumed at %.3f s\n",
                    gap->name, now);
    }
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static double min_double(double a, double b)
{
    return a < b ? a : b;
}

static int catch_up(MXLTestSource *source, const TestOptions *opts,
                    uint64_t due_frame, uint64_t video_limit,
                    uint64_t audio_limit, int video_active, int audio_active)
{
    if (video_active) {
        uint64_t first = due_frame > video_limit ? due_frame - video_limit : 0;
        uint64_t next = source->video.next_index - opts->start_index;

        if (next < first) {
            uint64_t count = first - next;
            fprintf(stderr, "mxl_test_source: video skip %" PRIu64
                    " grains (%" PRIu64 " -> %" PRIu64 ")\n",
                    count, source->video.next_index, opts->start_index + first);
            source->video.skipped += count;
            source->video.next_index = opts->start_index + first;
        }
    }

    if (audio_active) {
        uint64_t first = due_frame > audio_limit ? due_frame - audio_limit : 0;

        if (source->audio.next_frame < first) {
            uint64_t new_index;
            uint64_t count;

            if (video_to_audio_index(opts->start_index + first, &new_index) ||
                new_index < source->audio.next_index)
                return -1;
            count = new_index - source->audio.next_index;
            fprintf(stderr, "mxl_test_source: audio skip %" PRIu64
                    " samples (frame %" PRIu64 " -> %" PRIu64 ")\n",
                    count, source->audio.next_frame, first);
            source->audio.skipped += count;
            source->audio.next_index = new_index;
            source->audio.next_frame = first;
        }
    }
    return 0;
}

static int write_audio_frame(AudioSink *sink, const TestSignal *signal,
                             uint64_t start_index)
{
    uint64_t end;

    if (start_index == UINT64_MAX ||
        sink->next_frame >= UINT64_MAX - start_index - 1 ||
        video_to_audio_index(start_index + sink->next_frame + 1, &end) ||
        end < sink->next_index) {
        fprintf(stderr, "mxl_test_source: audio index overflow\n");
        return -1;
    }
    if (write_audio(sink, signal, end))
        return -1;
    sink->next_frame++;
    return 0;
}

static int generate_media(MXLTestSource *source, const TestOptions *opts,
                          const sigset_t *stop_signals)
{
    const TestSignal signal = { SIGNAL_ZERO };
    MediaClock clock;
    GapSchedule video_gap, audio_gap;
    uint64_t video_capacity = frames_per_ms(opts->history_duration, 60);
    uint64_t video_limit = min_u64(frames_per_ms(opts->catchup_limit, 60),
                                   video_capacity ? video_capacity - 1 : 0);
    uint64_t audio_limit = min_u64(frames_per_ms(opts->catchup_limit, 60),
                                   frames_per_ms(opts->history_duration, 30));
    int result;

    init_gap(&video_gap, "video", UINT64_C(0x2a63d5ad973e8847),
             opts->video_gap_rate, opts->video_gap_mean);
    init_gap(&audio_gap, "audio", UINT64_C(0xb5491b926add1143),
             opts->audio_gap_rate, opts->audio_gap_mean);
    if (start_clock(&clock, opts->speed))
        return -1;

    for (;;) {
        double now, next_event;
        uint64_t due_frame;
        int wrote = 0;

        if (current_media_time(&clock, &now))
            return -1;
        if (now >= opts->duration)
            break;
        if (now * VIDEO_RATE_NUM / VIDEO_RATE_DEN >=
            (double)(UINT64_MAX - opts->start_index - 1)) {
            fprintf(stderr, "mxl_test_source: media index overflow\n");
            return -1;
        }
        due_frame = (uint64_t)(now * VIDEO_RATE_NUM / VIDEO_RATE_DEN);

        update_gap(&video_gap, now);
        update_gap(&audio_gap, now);
        if (catch_up(source, opts, due_frame, video_limit, audio_limit,
                     !video_gap.active, !audio_gap.active))
            return -1;

        /* Poll even while draining a catch-up backlog. */
        result = wait_for_media_time(&clock, now, stop_signals);
        if (result < 0)
            return -1;
        if (result > 0)
            break;

        if (!video_gap.active &&
            source->video.next_index - opts->start_index <= due_frame) {
            if (source->video.next_index == UINT64_MAX ||
                write_video(&source->video, &signal))
                return -1;
            wrote = 1;
        }
        if (!audio_gap.active && source->audio.next_frame <= due_frame) {
            if (write_audio_frame(&source->audio, &signal, opts->start_index))
                return -1;
            wrote = 1;
        }
        if (wrote)
            continue;

        next_event = opts->duration;
        next_event = min_double(next_event, video_gap.next_start);
        next_event = min_double(next_event, audio_gap.next_start);
        next_event = min_double(next_event, video_gap.active ?
                                video_gap.active_end :
                                frame_time(source->video.next_index - opts->start_index));
        next_event = min_double(next_event, audio_gap.active ?
                                audio_gap.active_end : frame_time(source->audio.next_frame));

        result = wait_for_media_time(&clock, next_event, stop_signals);
        if (result < 0)
            return -1;
        if (result > 0)
            break;
    }

    fprintf(stderr, "mxl_test_source: wrote %" PRIu64 " video grains and %" PRIu64
            " audio samples; skipped %" PRIu64 " video grains and %" PRIu64
            " audio samples; gaps %" PRIu64 " video, %" PRIu64 " audio\n",
            source->video.written, source->audio.written,
            source->video.skipped, source->audio.skipped,
            video_gap.count, audio_gap.count);
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
