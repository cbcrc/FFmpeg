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

#ifndef MXL_DUMP_H
#define MXL_DUMP_H

#include <stddef.h>
#include <stdint.h>

typedef struct mxl_dump_audio_writer {
    int fd;
} mxl_dump_audio_writer;

int mxl_dump_init_audio_writer(mxl_dump_audio_writer *writer, const char *path);
int mxl_dump_write_audio(mxl_dump_audio_writer *writer, const uint8_t *data, size_t size);
int mxl_dump_close_audio_writer(mxl_dump_audio_writer *writer);

#endif
