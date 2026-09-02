/*
 * MXL domain and flow locator for Media eXchange Layer flows
 *
 * AVFMT_NOFILE demuxer; requires "/path/to/domain/<id>.mxl-flow"
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

#ifndef AVFORMAT_MXL_LOC_H
#define AVFORMAT_MXL_LOC_H

typedef struct mxl_loc {
    char *domain_path;
    char **flow_ids;
    int nb_flow_ids;
} mxl_loc;

/**
 * domain must exist
 * host must not exist
 * one or more flow IDs must exist
 */
int  mxl_loc_parse(void *logctx, const char *input, mxl_loc *loc);

void mxl_loc_free(mxl_loc *loc);

#endif /* AVFORMAT_MXL_LOC_H */
