/*
 * MXL shared functions for Media eXchange Layer flows
 *
 * Copyright (c) 2025 CBC/Radio-Canada
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

#include "libavutil/log.h"
#include <mxl/mxl.h>

#define MXL_FLOW_EXT "mxl-flow"
#define MXL_DOT_FLOW_EXT ".mxl-flow"
#define MXL_LONG_NAME "Media eXchange Layer"

static inline void logd(const void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog((void *)avcl, AV_LOG_DEBUG, fmt, ap);
    va_end(ap);
}

static inline void logv(const void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog((void *)avcl, AV_LOG_VERBOSE, fmt, ap);
    va_end(ap);
}

static inline void logw(const void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog((void *)avcl, AV_LOG_WARNING, fmt, ap);
    va_end(ap);
}

static inline void loge(const void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog((void *)avcl, AV_LOG_ERROR, fmt, ap);
    va_end(ap);
}

static const char* mxl_status_to_str(mxlStatus status) {

    switch(status) {
    case MXL_STATUS_OK:
        return "MXL_STATUS_OK";
    case MXL_ERR_UNKNOWN:
        return "MXL_ERR_UNKNOWN";
    case MXL_ERR_FLOW_NOT_FOUND:
        return "MXL_ERR_FLOW_NOT_FOUND";
    case MXL_ERR_OUT_OF_RANGE_TOO_LATE:
        return "MXL_ERR_OUT_OF_RANGE_TOO_LATE";
    case MXL_ERR_OUT_OF_RANGE_TOO_EARLY:
        return "MXL_ERR_OUT_OF_RANGE_TOO_EARLY";
    case MXL_ERR_INVALID_FLOW_READER:
        return "MXL_ERR_INVALID_FLOW_READER";
    case MXL_ERR_INVALID_FLOW_WRITER:
        return "MXL_ERR_INVALID_FLOW_WRITER";
    case MXL_ERR_TIMEOUT:
        return "MXL_ERR_TIMEOUT";
    case MXL_ERR_INVALID_ARG:
        return "MXL_ERR_INVALID_ARG";
    case MXL_ERR_CONFLICT:
        return "MXL_ERR_CONFLICT";
    case MXL_ERR_PERMISSION_DENIED:
        return "MXL_ERR_PERMISSION_DENIED";
    case MXL_ERR_FLOW_INVALID:
        return "MXL_ERR_FLOW_INVALID";
    default:
        return "UNKNOWN";
    }
}
