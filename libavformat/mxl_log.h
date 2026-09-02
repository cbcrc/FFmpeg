/*
 * MXL logging for Media eXchange Layer flows
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

#ifndef AVFORMAT_MXL_LOG_H
#define AVFORMAT_MXL_LOG_H

#include "libavutil/log.h"

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

static inline void logi(const void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog((void *)avcl, AV_LOG_INFO, fmt, ap);
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

#endif /* AVFORMAT_MXL_LOG_H */
