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
 * Phase 1 creates the source MXL domain and selected flow writers.
 * FFmpeg will create the destination domain and flows; destination
 * reader initialization and media verification follow in later phases.
 *
 * Usage:
 *   tests/mxl_transcode_test <source_domain>
 *       [--video=UUID] [--audio=UUID] [--audio-samples-per-buffer=N]
 *       <destination_domain> [--video=UUID] [--audio=UUID]
 *
 * The source flow options select video, audio, or A/V testing. The
 * destination must specify the same media types with different flow IDs.
 * The source domain must not already exist and is removed on completion.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>

#define DEFAULT_AUDIO_SAMPLES_PER_BUFFER 800

#define MEDIA_VIDEO 1
#define MEDIA_AUDIO 2

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
    const char *source_directory;
    const char *destination_directory;
    const char *source_video_id;
    const char *source_audio_id;
    const char *destination_video_id;
    const char *destination_audio_id;
    size_t audio_samples_per_buffer;
    int audio_samples_specified;
    int media;
} TestOptions;

typedef struct TestDomain {
    const char *directory;
    int created;
} TestDomain;

typedef struct TestContext {
    TestDomain source_domain;
    mxlInstance source_instance;
    mxlFlowWriter video_writer;
    mxlFlowWriter audio_writer;
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
    if (!opts->source_directory || !opts->source_directory[0] ||
        !opts->destination_directory || !opts->destination_directory[0] ||
        !strcmp(opts->source_directory, opts->destination_directory)) {
        fprintf(stderr, "mxl_transcode_test: invalid domain paths\n");
        return -1;
    }
    if (!opts->source_video_id && !opts->source_audio_id) {
        fprintf(stderr, "mxl_transcode_test: no source flows specified\n");
        return -1;
    }
    if (!!opts->source_video_id != !!opts->destination_video_id ||
        !!opts->source_audio_id != !!opts->destination_audio_id) {
        fprintf(stderr, "mxl_transcode_test: source and destination media must match\n");
        return -1;
    }
    if (opts->audio_samples_specified && !opts->source_audio_id) {
        fprintf(stderr, "mxl_transcode_test: audio buffer size requires audio\n");
        return -1;
    }
    if ((opts->source_video_id &&
         !strcmp(opts->source_video_id, opts->destination_video_id)) ||
        (opts->source_audio_id &&
         !strcmp(opts->source_audio_id, opts->destination_audio_id))) {
        fprintf(stderr, "mxl_transcode_test: source and destination flow IDs must differ\n");
        return -1;
    }

    opts->media = (opts->source_video_id ? MEDIA_VIDEO : 0) |
                  (opts->source_audio_id ? MEDIA_AUDIO : 0);
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
    int is_source = 1;
    int failed = -1;
    int i;

    opts->audio_samples_per_buffer = DEFAULT_AUDIO_SAMPLES_PER_BUFFER;
    if (argc < 5)
        goto finally;
    opts->source_directory = argv[1];

    for (i = 2; i < argc; i++) {
        argument = argv[i];
        if (strncmp(argument, "--", 2)) {
            if (!is_source)
                goto finally;
            opts->destination_directory = argument;
            is_source = 0;
        } else if (!strncmp(argument, "--video=", 8)) {
            flow_id = is_source ? &opts->source_video_id :
                                  &opts->destination_video_id;
            if (*flow_id || !valid_uuid(argument + 8))
                goto finally;
            *flow_id = argument + 8;
        } else if (!strncmp(argument, "--audio=", 8)) {
            flow_id = is_source ? &opts->source_audio_id :
                                  &opts->destination_audio_id;
            if (*flow_id || !valid_uuid(argument + 8))
                goto finally;
            *flow_id = argument + 8;
        } else if (is_source &&
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

    if (create_domain(&context->source_domain, opts->source_directory))
        goto finally;

    context->source_instance = mxlCreateInstance(opts->source_directory, NULL);
    if (!context->source_instance) {
        fprintf(stderr, "mxl_transcode_test: create source instance failed\n");
        goto finally;
    }

    if ((opts->media & MEDIA_VIDEO) &&
        create_flow(context->source_instance, video_flow_template,
                    opts->source_video_id, "video", &context->video_writer))
        goto finally;
    if ((opts->media & MEDIA_AUDIO) &&
        create_flow(context->source_instance, audio_flow_template,
                    opts->source_audio_id, "audio", &context->audio_writer))
        goto finally;

    failed = 0;

finally:
    return failed;
}

static int cleanup_test(TestContext *context)
{
    mxlStatus status;
    int failed = 0;

    if (context->video_writer) {
        status = mxlReleaseFlowWriter(context->source_instance,
                                      context->video_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release video writer failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->video_writer = NULL;
    }
    if (context->audio_writer) {
        status = mxlReleaseFlowWriter(context->source_instance,
                                      context->audio_writer);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: release audio writer failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->audio_writer = NULL;
    }
    if (context->source_instance) {
        status = mxlDestroyInstance(context->source_instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_transcode_test: destroy source instance failed (%d)\n",
                    (int)status);
            failed = -1;
        }
        context->source_instance = NULL;
    }
    if (remove_domain(&context->source_domain))
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
    puts("mxl_transcode_test: destination domain and flows managed by FFmpeg");
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
