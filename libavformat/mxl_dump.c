/*
 * MXL raw sample dump for Media eXchange Layer flows
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

#include "mxl_dump.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int mxl_dump_init_audio_writer(mxl_dump_audio_writer *writer, const char *path)
{
    int fd;

    if (!writer || !path)
        return EINVAL;

    writer->fd = -1;

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return errno;

    writer->fd = fd;
    return 0;
}

int mxl_dump_write_audio(mxl_dump_audio_writer *writer, const uint8_t *data, size_t size)
{
    const uint8_t *p;
    size_t remaining;

    if (!writer || writer->fd < 0 || (!data && size > 0))
        return EINVAL;

    p = data;
    remaining = size;

    while (remaining > 0) {
        ssize_t n = write(writer->fd, p, remaining);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }

        if (n == 0)
            return EIO;

        p += n;
        remaining -= (size_t)n;
    }

    return 0;
}

int mxl_dump_close_audio_writer(mxl_dump_audio_writer *writer)
{
    int ret;

    if (!writer)
        return EINVAL;

    if (writer->fd < 0)
        return 0;

    ret = close(writer->fd);
    writer->fd = -1;

    if (ret < 0)
        return errno;

    return 0;
}
