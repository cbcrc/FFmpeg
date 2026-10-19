/*
 * Minimal MXL test source for FFmpeg regression tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Phase 1: create the MXL domain and audio/video flows, then wait for shutdown.
 * No media is written in this phase.
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

#include <mxl/mxl.h>
#include <mxl/flow.h>

#define NS_PER_MS UINT64_C(1000000)

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

typedef struct MXLTestSource {
    mxlInstance instance;
    mxlFlowWriter video_writer;
    mxlFlowWriter audio_writer;
} MXLTestSource;

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

static int close_source(MXLTestSource *source)
{
    mxlStatus status;
    int failed = 0;

    if (source->video_writer) {
        status = mxlReleaseFlowWriter(source->instance, source->video_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: release video flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->video_writer = NULL;
    }
    if (source->audio_writer) {
        status = mxlReleaseFlowWriter(source->instance, source->audio_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_test_source: release audio flow failed (%d)\n",
                    (int)status);
            failed = 1;
        }
        source->audio_writer = NULL;
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
    int signum;
    int err;
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
                    &source.video_writer) ||
        create_flow(source.instance, audio_flow_def, "audio",
                    &source.audio_writer)) {
        failed = 1;
        goto finally;
    }

    /* The test harness can start its reader after this notification. */
    puts("mxl_test_source: flows ready");
    fflush(stdout);

    /* Phase 2 will replace this wait with paced media delivery. */
    err = sigwait(&stop_signals, &signum);
    if (err) {
        fprintf(stderr, "mxl_test_source: sigwait failed (%d)\n", err);
        failed = 1;
    }

finally:
    if (close_source(&source))
        failed = 1;
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
