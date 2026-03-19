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

#include "mxl_uri.h"
#include "mxl_common.h"

#include "libavformat/avformat.h"
#include "libavutil/mem.h"
#include "libavutil/error.h"
#include "libavutil/avstring.h"

#include <ctype.h>
#include <string.h>

/* returns 0 if flow_ids are unique */
static int validate_unique_ids(void* logctx, int nb_flow_ids, char** flow_ids)
{
    // demuxer requires unique flow IDs
    for(int i = 0; i < nb_flow_ids; i++)
        for(int j = i+1; j < nb_flow_ids; j++)
            if (0 == av_strcasecmp(flow_ids[i], flow_ids[j])) {
                logv(logctx, "URI locator flow ID not unique: %s\n", flow_ids[j]);
                return -1;
            }

    return 0;
}

/* returns boolean if uri is valid (0 = not valid) */
int mxl_uri_is_valid_uuid(const char *s)
{
    if (!s)
        return 0;

    if (strlen(s) != 36)
        return 0;

    for (int i = 0; i < 36; i++) {
        switch (i) {
        case 8:
        case 13:
        case 18:
        case 23:
            if (s[i] != '-')
                return 0;
            break;

        default:
            if (!isxdigit((unsigned char)s[i]))
                return 0;
        }
    }

    return 1;
}

void mxl_uri_free(mxl_uri *u)
{
    if (!u)
        return;

    av_free(u->host);
    av_free(u->domain);

    for (int i = 0; i < u->nb_flow_ids; i++)
        av_free(u->flow_ids[i]);
    av_free(u->flow_ids);

    memset(u, 0, sizeof(*u));
}

int mxl_uri_is_mxl_scheme(const char *uri)
{
    return uri && av_stristart(uri, "mxl:", NULL);
}

int mxl_parse_uri(void *logctx, const char *uri, mxl_uri *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->port = -1;
    }
    else
        return AVERROR(EINVAL);

    if (!uri)
        return AVERROR(EINVAL);

    /* protect against runaway reads */
    size_t len = av_strnlen(uri, MXL_URI_MAX + 1);
    if (len == MXL_URI_MAX + 1)
        return AVERROR(ENAMETOOLONG);

    const char *p = strchr(uri, ':');
    if (!p) {
        logv(logctx, "URI scheme not found\n");
        return AVERROR(EINVAL);
    }

    if ((size_t)(p - uri) >= MXL_SCHEME_MAX) {
        logv(logctx, "URI scheme too long\n");
        return AVERROR(ENAMETOOLONG);
    }

    if (strchr(uri, '#')) {
        logv(logctx, "URI fragments not supported\n");
        return AVERROR(EINVAL);
    }

    int exit_status = AVERROR_UNKNOWN;

    char  *out_host = NULL;
    int    out_port = -1;
    char  *out_domain = NULL;
    char **out_flow_ids = NULL;
    int    out_nb_flow_ids = 0;

    int port = -1;
    char scheme[MXL_SCHEME_MAX] = { 0 };
    char auth[2];
    char *host = av_mallocz(MXL_HOST_MAX);
    char *path = av_mallocz(MXL_PATH_MAX);

    if (!host || !path) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    av_url_split(scheme, sizeof(scheme),
                 auth, sizeof(auth),
                 host, MXL_HOST_MAX,
                 &port,
                 path, MXL_PATH_MAX,
                 uri);

    if (av_strcasecmp(scheme, "mxl") != 0) {
        logv(logctx, "Unknown URI scheme: %s\n", scheme);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    /* require mxl:// (authority marker present) */
    if (len < 6 || av_strncasecmp(uri, "mxl://", 6) != 0) {
        logv(logctx, "Authority marker required in URI (\"mxl://\")\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    if (auth[0] != '\0') {
        logv(logctx, "URI user authorization info not supported\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    /* reject empty IPv6 literal: mxl://[]/... */
    const char *auth_start = uri + 6;
    if (auth_start[0] == '[' && host[0] == '\0') {
        logv(logctx, "Invalid IPv6 host literal: []\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    /* reject host without domain path */
    if (!path || path[0] != '/' || path[1] == '\0' || path[1] == '?') {
        logv(logctx, "MXL URI must contain a non-empty domain path\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    // port=-1 is returned by av_url_split to indicate that the port
    // is absent, therefore, anything less is an invalid negative
    // port. Port equal to 0 is returned in abberrant cases such as
    // absent port or non integer port number.
    if (port < -1 || port == 0 || port > 65535) {
        logv(logctx, "Invalid URI port: %d\n", port);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }
    else
        out_port = port;

    /* copy host */
    out_host = av_strdup(host);
    if (!out_host) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    /* split path and query */
    char *query = strchr(path, '?');
    if (query) {
        *query++ = '\0';
    }

    /* domain must exist  */
    if (!path[0]) {
        logv(logctx, "Invalid URI missing domain path\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    out_domain = av_strdup(path);
    if (!out_domain) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    if (query) {
        char *strtok_context = NULL;
        char *token = strtok_r(query, "&", &strtok_context);
        while (token) {

            if (!strncmp(token, "id=", 3)) {
                char *flow_id = av_strdup(token + 3);
                char **tmp;

                if (!flow_id) {
                    exit_status = AVERROR(ENOMEM);
                    goto finally;
                }

                if (flow_id[0] == '\0') {
                    logv(logctx, "Invalid URI query parameter: %s\n", token);
                    av_free(flow_id);
                    exit_status = AVERROR(EINVAL);
                    goto finally;
                }

                if (!mxl_uri_is_valid_uuid(flow_id)) {
                    logv(logctx, "Invalid URI flow ID parameter: %s\n", flow_id);
                    av_free(flow_id);
                    exit_status = AVERROR(EINVAL);
                    goto finally;
                }

                tmp = av_realloc_array(out_flow_ids,
                                       out_nb_flow_ids + 1,
                                       sizeof(char *));
                if (!tmp) {
                    av_free(flow_id);
                    exit_status = AVERROR(ENOMEM);
                    goto finally;
                }

                out_flow_ids = tmp;
                out_flow_ids[out_nb_flow_ids++] = flow_id;
            }

            token = strtok_r(NULL, "&", &strtok_context);
        }
    }

    int rc = validate_unique_ids(logctx, out_nb_flow_ids, out_flow_ids);
    if (rc) {
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    out->host = out_host;
    out->port = out_port;
    out->domain = out_domain;
    out->flow_ids = out_flow_ids;
    out->nb_flow_ids = out_nb_flow_ids;

    out_host = NULL;
    out_flow_ids = NULL;
    out_domain = NULL;

    exit_status = 0;

finally:

    av_free(host);
    av_free(path);

    av_free(out_host);
    av_free(out_domain);

    if (out_flow_ids) {
        for (int i = 0; i < out_nb_flow_ids; i++)
            av_free(out_flow_ids[i]);
        av_free(out_flow_ids);
    }

    return exit_status;
}
