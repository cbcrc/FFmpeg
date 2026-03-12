/*
 * MXL json flow definition for Media eXchange Layer flows
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

/*
 * See: https://github.com/dmf-mxl/mxl
 */

#ifndef MXL_FLOW_DEF_H
#define MXL_FLOW_DEF_H

#include "libavutil/mem.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Sample flow definitions for reference
{
  "$copyright": "SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.",
  "$license": "SPDX-License-Identifier: Apache-2.0",
  "description": "MXL Test Flow, 1080p29",
  "id": "5fbec3b1-1b0f-417d-9059-8b94a47197ed",
  "tags": {
    "urn:x-nmos:tag:grouphint/v1.0": [
      "Media Function XYZ:Video"
    ]
  },
  "format": "urn:x-nmos:format:video",
  "label": "MXL Test Flow, 1080p29",
  "parents": [],
  "media_type": "video/v210",
  "grain_rate": {
    "numerator": 30000,
    "denominator": 1001
  },
  "frame_width": 1920,
  "frame_height": 1080,
  "interlace_mode": "progressive",
  "colorspace": "BT709",
  "components": [
    {
      "name": "Y",
      "width": 1920,
      "height": 1080,
      "bit_depth": 10
    },
    {
      "name": "Cb",
      "width": 960,
      "height": 1080,
      "bit_depth": 10
    },
    {
      "name": "Cr",
      "width": 960,
      "height": 1080,
      "bit_depth": 10
    }
  ]
}

{
  "$copyright": "SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.",
  "$license": "SPDX-License-Identifier: Apache-2.0",
  "description": "MXL Audio Flow",
  "format": "urn:x-nmos:format:audio",
  "tags": {
    "urn:x-nmos:tag:grouphint/v1.0": [
      "Media Function XYZ:Audio"
    ]
  },
  "label": "MXL Audio Flow",
  "version": "1441812152:154331951",
  "id": "b3bb5be7-9fe9-4324-a5bb-4c70e1084449",
  "media_type": "audio/float32",
  "sample_rate": {
    "numerator": 48000
  },
  "channel_count": 2,
  "bit_depth": 32,
  "parents": [],
  "source_id": "2aa143ac-0ab7-4d75-bc32-5c00c13d186f",
  "device_id": "169feb2c-3fae-42a5-ae2e-f6f8cbce29cf"
}
*/

typedef struct VideoFlowDefParams {
    const char *id;
    int grain_rate_num;
    int grain_rate_den;
    int frame_width;
    int frame_height;
    int y_width, y_height, y_bit_depth;
    int cb_width, cb_height, cb_bit_depth;
    int cr_width, cr_height, cr_bit_depth;
} VideoFlowDefParams;

static const char *video_flow_def_fmt =
"{\n"
"  \"description\": \"FFmpeg video stream\",\n"
"  \"id\": \"%s\",\n"
"  \"tags\": {\n"
"  \"urn:x-nmos:tag:grouphint/v1.0\": [\n"
"     \"Media Function XYZ:Video\"\n"
"    ]\n"
"  },\n"
"  \"format\": \"urn:x-nmos:format:video\",\n"
"  \"label\": \"FFmpeg video stream\",\n"
"  \"parents\": [],\n"
"  \"media_type\": \"video/v210\",\n"
"  \"grain_rate\": {\n"
"    \"numerator\": %d,\n"
"    \"denominator\": %d\n"
"  },\n"
"  \"frame_width\": %d,\n"
"  \"frame_height\": %d,\n"
"  \"interlace_mode\": \"progressive\",\n"
"  \"colorspace\": \"BT709\",\n"
"  \"components\": [\n"
"    {\n"
"      \"name\": \"Y\",\n"
"      \"width\": %d,\n"
"      \"height\": %d,\n"
"      \"bit_depth\": %d\n"
"    },\n"
"    {\n"
"      \"name\": \"Cb\",\n"
"      \"width\": %d,\n"
"      \"height\": %d,\n"
"      \"bit_depth\": %d\n"
"    },\n"
"    {\n"
"      \"name\": \"Cr\",\n"
"      \"width\": %d,\n"
"      \"height\": %d,\n"
"      \"bit_depth\": %d\n"
"    }\n"
"  ]\n"
"}\n";

typedef struct AudioFlowDefParams {
    const char *id;
    int         sample_rate;
    int         channel_count;
} AudioFlowDefParams;

static const char  *audio_flow_def_fmt =
"{\n"
"  \"description\": \"FFmpeg audio stream\",\n"
"  \"id\": \"%s\",\n"
"  \"tags\": {\n"
"    \"urn:x-nmos:tag:grouphint/v1.0\": [\n"
"      \"Media Function XYZ:Audio\"\n"
"    ]\n"
"  },\n"
"  \"format\": \"urn:x-nmos:format:audio\",\n"
"  \"label\": \"FFmpeg audio stream\",\n"
"  \"parents\": [],\n"
"  \"media_type\": \"audio/float32\",\n"
"  \"sample_rate\": {\n"
"    \"numerator\": %d\n"
"  },\n"
"  \"channel_count\": %d,\n"
"  \"bit_depth\": 32\n"
"}\n";

/**
 * Create an MXL video flow definition.
 *
 * Formats a JSON video flow definition using fields from
 * VideoFlowDefParams p and returns a newly allocated string. The
 * caller owns the returned string.
 *
 * @param p Pointer to a VideoFlowDefParams structure containing all values.
 * @return Newly allocated null-terminated JSON string, or NULL on failure.
 */
static char *make_video_flow_def(const VideoFlowDefParams *p)
{
    if (!p || !p->id)
        return NULL;

    int len = snprintf(NULL, 0, video_flow_def_fmt,
        p->id,
        p->grain_rate_num, p->grain_rate_den,
        p->frame_width, p->frame_height,
        p->y_width,  p->y_height,  p->y_bit_depth,
        p->cb_width, p->cb_height, p->cb_bit_depth,
        p->cr_width, p->cr_height, p->cr_bit_depth
    );

    if (len < 0)
        return NULL;

    size_t buf_size = len+1;
    char *buf = av_malloc(buf_size);
    if (!buf)
        return NULL;

    int rc = snprintf(buf, buf_size, video_flow_def_fmt,
        p->id,
        p->grain_rate_num, p->grain_rate_den,
        p->frame_width, p->frame_height,
        p->y_width,  p->y_height,  p->y_bit_depth,
        p->cb_width, p->cb_height, p->cb_bit_depth,
        p->cr_width, p->cr_height, p->cr_bit_depth
    );
    if (rc < 0 || (size_t)rc >= buf_size) {
        av_free(buf);
        return NULL;
    }

    return buf;
}

/**
 * Create an MXL audio flow definition.
 *
 * Formats a JSON audio flow definition using fields from
 * AudioFlowDefParams p and returns a newly allocated string. The
 * caller owns the returned string.
 *
 * @param p Pointer to an AudioFlowDefParams structure containing all values.
 * @return Newly allocated null-terminated JSON string, or NULL on failure.
 */
static char *make_audio_flow_def(const AudioFlowDefParams *p)
{
    if (!p || !p->id)
        return NULL;

    int len = snprintf(NULL, 0, audio_flow_def_fmt,
                       p->id,
                       p->sample_rate,
                       p->channel_count);
    if (len < 0)
        return NULL;

    size_t buf_size = len+1;
    char *buf = av_malloc(buf_size);
    if (!buf)
        return NULL;

    int rc = snprintf(buf, buf_size, audio_flow_def_fmt,
                           p->id,
                           p->sample_rate,
                           p->channel_count);
    if (rc < 0 || (size_t)rc >= buf_size) {
        av_free(buf);
        return NULL;
    }

    return buf;
}

#endif /* MXL_FLOW_DEF_H */
