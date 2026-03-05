/*
 * MXL URI parser for Media eXchange Layer flows
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
 * Parse MXL v1.0.0 URIs.
 *
 * See: https://github.com/dmf-mxl/mxl/blob/release/v1.0/docs/Addressability.md
 */

#ifndef AVFORMAT_MXL_URI_H

/* Parsing limits: av_url_split() silently truncates into
 * caller-provided buffers. We therefore cap the total URI length
 * (MXL_URI_MAX) and require MXL_URI_MAX <= the host/path buffer
 * sizes, so overlong inputs can be rejected deterministically
 * (ENAMETOOLONG) and no truncation occurs. */
enum MXL_URI_LIMITS {
    MXL_SCHEME_MAX = sizeof("mxl"),
    MXL_URI_MAX    = 1024,
    MXL_HOST_MAX   = MXL_URI_MAX,
    MXL_PATH_MAX   = MXL_URI_MAX
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(MXL_URI_MAX > 0,
               "MXL_URI_MAX must be > 0");
_Static_assert(MXL_URI_MAX <= MXL_HOST_MAX,
               "MXL_URI_MAX must be <= MXL_HOST_MAX");
_Static_assert(MXL_URI_MAX <= MXL_PATH_MAX,
               "MXL_URI_MAX must be <= MXL_PATH_MAX");
#endif

typedef struct mxl_uri {
    const char *host;
    int   port;
    const char *domain;
    const char * const *flow_ids;
    int   nb_flow_ids;
} mxl_uri;

/**
 * Parse an MXL URI and return the parsed result in @out.
 *
 * The URI grammar is:
 *
 * mxl://[authority]/domain/path?id=<uuid>[&id=<uuid>...]
 *
 * The authority component is optional.
 *
 * Example of a valid MXL URI that specifies a local domain path:
 *
 * "mxl:///dev/shm/mxl?id=3645f1e2-90fc-49cb-af9c-236c2ac124cd&id=3645f1e2-90fc-49cb-af9c-236c2ac124cd"
 *
 * "/dev/shm/mxl" is the local filesystem path to the MXL domain. The
 * "id" query parameters carry the flow IDs. This is the format that
 * is supported by the MXL demuxer.
 *
 * Note that the parser will accept URIs that include an authority
 * component, e.g. "mxl://host:port/domain/path?id=...", but the
 * demuxer implementation currently supports only local filesystem
 * domains.
 *
 * @param logctx the logger context
 * @param uri    the URI to parse
 * @param out    the parsed URI
 * @return 0     on success, or error code
 *         AVERROR(EINVAL)       URI is invalid and cannot be parsed
 *         AVERROR(ENAMETOOLONG) URI exceeds parser implementation limits
 *         AVERROR(ENOMEM)       memory allocation failed
 *         AVERROR_UNKNOWN       an unspecified error occurred
 */
int mxl_parse_uri(void *logctx, const char *uri, mxl_uri *out);

/*
 * Free a parse result produced by mxl_parse_uri(). The fields of
 * @parsed_uri are freed and cleared. The @parsed_uri pointer itself
 * is not freed.
 *
 * @param parsed_uri The parsed URI to free.
 */
void mxl_uri_free(mxl_uri *parsed_uri);

#endif /* AVFORMAT_MXL_URI_H */
