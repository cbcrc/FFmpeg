/*
 * MXL flow readiness utility for FATE tests
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Wait until a single MXL flow has an active writer.
 *
 * Usage:
 *   tests/mxl_is_flow_active <domain> <flow-id> <timeout-ms>
 *
 * Exit successfully when mxlIsFlowActive reports an active flow, or fail
 * when the timeout expires. The domain may not yet exist at startup.
 * Produces no output on success.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>

#include <libavutil/time.h>

#define POLL_INTERVAL_US 100000

static int parse_timeout(const char *value, int64_t *timeout_us)
{
    unsigned long long timeout_ms;
    char *end;

    if (!value[0] || value[0] == '-')
        return -1;

    errno = 0;
    timeout_ms = strtoull(value, &end, 10);
    if (errno || *end || timeout_ms > INT64_MAX / 1000)
        return -1;

    *timeout_us = (int64_t)timeout_ms * 1000;
    return 0;
}

int main(int argc, char **argv)
{
    mxlInstance instance = NULL;
    mxlStatus status = MXL_STATUS_OK;
    int64_t timeout_us, start_us, remaining_us;
    int exit_status = EXIT_FAILURE;
    bool active = false;

    if (argc != 4 || !argv[1][0] || !argv[2][0] ||
        parse_timeout(argv[3], &timeout_us)) {
        fprintf(stderr, "Usage: %s <domain> <flow-id> <timeout-ms>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    start_us = av_gettime_relative();

    for (;;) {
        if (!instance)
            instance = mxlCreateInstance(argv[1], NULL);

        if (instance) {
            active = false;
            status = mxlIsFlowActive(instance, argv[2], &active);
            if (status == MXL_STATUS_OK && active) {
                exit_status = EXIT_SUCCESS;
                goto finally;
            }
        }

        remaining_us = timeout_us - (av_gettime_relative() - start_us);
        if (remaining_us <= 0)
            break;

        av_usleep(remaining_us < POLL_INTERVAL_US ?
                  (unsigned)remaining_us : POLL_INTERVAL_US);
    }

    if (!instance) {
        fprintf(stderr, "mxl_is_flow_active: timed out opening domain %s\n",
                argv[1]);
    } else {
        fprintf(stderr, "mxl_is_flow_active: timed out waiting for flow %s "
                "in domain %s (last MXL status %d)\n",
                argv[2], argv[1], (int)status);
    }

finally:
    if (instance) {
        status = mxlDestroyInstance(instance);
        if (status != MXL_STATUS_OK) {
            fprintf(stderr, "mxl_is_flow_active: mxlDestroyInstance failed (%d)\n",
                    (int)status);
            exit_status = EXIT_FAILURE;
        }
    }

    return exit_status;
}
