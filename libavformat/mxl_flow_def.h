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

#include <stdio.h>
#include <stdlib.h>

/*
 * Sample flow definition for reference
{
  "description": "MXL Test File",
  "id": "5fbec3b1-1b0f-417d-9059-8b94a47197ef",
  "tags": {},
  "format": "urn:x-nmos:format:video",
  "label": "MXL Test File",
  "parents": [],
  "media_type": "video/v210",
  "grain_rate": {
    "numerator": 50,
    "denominator": 1
  },
  "frame_width": 1920,
  "frame_height": 1080,
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
*/

typedef struct FlowDefParams {
    const char *id;
    int grain_rate_num;
    int grain_rate_den;
    int frame_width;
    int frame_height;
    int y_width, y_height, y_bit_depth;
    int cb_width, cb_height, cb_bit_depth;
    int cr_width, cr_height, cr_bit_depth;
} FlowDefParams;

static const char *flow_def_fmt =
"{\n"
"  \"description\": \"FFmpeg video stream\",\n"
"  \"id\": \"%s\",\n"
"  \"tags\": {},\n"
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

/**
 * Create a MXL flow definition.
 *
 * Formats a JSON flow definition using fields from FlowDefParams p
 * and returns a newly allocated string. The caller owns the returned
 * string.
 *
 * @param p Pointer to a FlowDefParams structure containing all values.
 * @return Newly allocated null-terminated JSON string, or NULL on failure.
 */
static char *make_flow_def(const FlowDefParams *p)
{
    int len = snprintf(NULL, 0, flow_def_fmt,
        p->id,
        p->grain_rate_num, p->grain_rate_den,
        p->frame_width, p->frame_height,
        p->y_width,  p->y_height,  p->y_bit_depth,
        p->cb_width, p->cb_height, p->cb_bit_depth,
        p->cr_width, p->cr_height, p->cr_bit_depth
    );

    if (len < 0)
        return NULL;  // encoding error

    char *buf = malloc(len + 1);  // +1 for the null terminator
    if (!buf)
        return NULL;

    snprintf(buf, len + 1, flow_def_fmt,
        p->id,
        p->grain_rate_num, p->grain_rate_den,
        p->frame_width, p->frame_height,
        p->y_width,  p->y_height,  p->y_bit_depth,
        p->cb_width, p->cb_height, p->cb_bit_depth,
        p->cr_width, p->cr_height, p->cr_bit_depth
    );

    return buf;
}

static const FlowDefParams sample_flow_def_params = {
    .id = "5fbec3b1-1b0f-417d-9059-8b94a47197ef",
    .grain_rate_num = 50,
    .grain_rate_den = 1,
    .frame_width = 1920,
    .frame_height = 1080,
    .y_width = 1920,
    .y_height = 1080,
    .y_bit_depth = 10,
    .cb_width = 960,
    .cb_height = 1080,
    .cb_bit_depth = 10,
    .cr_width = 960,
    .cr_height = 1080,
    .cr_bit_depth = 10
};
