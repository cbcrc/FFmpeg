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

#include "mxl_loc.h"
#include "mxl_uri.h"
#include "mxl_common.h"

#include "libavutil/error.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavformat/avformat.h"

#include <string.h>
/**
 * Extract the directory from a path of the form
 * "/path/to/domain/<id>.mxl-flow".
 *
 * Returns a newly allocated string or null if the path contains no
 * usable domain component. Caller must free the returned string.
 */
static char *extract_domain_from_path(const char *path)
{
    if (NULL == path)
        return NULL;

    char *tmp = av_strdup(path);
    if (NULL == tmp)
        return NULL;

    const char* dir = av_dirname(tmp);
    av_assert0(dir);

    // av_dirname special case
    if (*dir == '\0')
        dir = "/";

    // stable copy
    char *out = av_strdup(dir);
    av_free(tmp);

    return out;
}

/**
 * Extract the id from a path of the form "/path/to/domain/<id>.mxl-flow".
 *
 * Returns a newly allocated string or NULL if the path has no usable
 * id component. Caller must free the returned string.
 */
static char *extract_flowid_from_path(const char *path)
{
    if (NULL == path)
        return NULL;

    const char *base = av_basename(path);
    av_assert0(base);

    const char *ext = MXL_DOT_FLOW_EXT;
    size_t ext_len  = sizeof(MXL_DOT_FLOW_EXT) - 1;
    size_t base_len = strlen(base);

    if (base_len <= ext_len)
        return NULL;

    if (strcmp(base + base_len - ext_len, ext) != 0)
        return NULL;

    size_t id_len = base_len - ext_len;
    av_assert1(id_len > 0);

    char *id = av_malloc(id_len + 1);
    if (NULL == id)
        return NULL;

    memcpy(id, base, id_len);
    id[id_len] = '\0';

    return id;
}

int mxl_loc_parse(void* logctx, const char *locator, mxl_loc *loc)
{
    if (NULL == locator || NULL == loc)
        return AVERROR(EINVAL);

    if (*locator == '\0')
        return AVERROR(ENOENT);

    char *domain_path = NULL;
    char **flow_ids = NULL;
    int nb_flow_ids = 0;
    mxl_uri uri = {0};
    int exit_status = AVERROR(EINVAL);

    // URI case
    if (mxl_uri_is_mxl_scheme(locator)) {
        int parse_rc = mxl_parse_uri(logctx, locator, &uri);
        if (parse_rc) {
            logv(logctx, "LOC URI scheme found but parse failed: \"%s\"\n", locator);
            exit_status = parse_rc;
            goto finally;
        }

        av_assert0(uri.host);
        if (uri.host[0] != '\0') {
            logv(logctx, "LOC URI host not supported: \"%s\"\n", locator);
            exit_status = AVERROR(EINVAL);
            goto finally;
        }

        // take ownership of URI domain
        av_assert0(uri.domain);
        domain_path = uri.domain;
        uri.domain = NULL;

        // take ownership of URI flow_ids array
        nb_flow_ids = uri.nb_flow_ids;
        flow_ids = uri.flow_ids;
        uri.flow_ids = NULL;
        uri.nb_flow_ids = 0;
    }
    // filesystem path case
    else if (av_match_ext(locator, MXL_FLOW_EXT)) {
        domain_path = extract_domain_from_path(locator);
        if (NULL == domain_path) {
            logv(logctx, "LOC URI failed to extract domain\n");
            exit_status = AVERROR(EINVAL);
            goto finally;
        }

        char *flow_id = extract_flowid_from_path(locator);
        if (NULL == flow_id) {
            logv(logctx, "LOC URI failed to extract MXL flow id\n");
            exit_status = AVERROR(EINVAL);;
            goto finally;
        }
        if (!mxl_uri_is_valid_uuid(flow_id)) {
            logv(logctx, "LOC URI flow ID is not a valid uuid: %s\n", flow_id);
            exit_status = AVERROR(EINVAL);
            goto finally;
        }

        // transfer ownership of flow_id to flow_ids array
        nb_flow_ids = 1;
        flow_ids = av_malloc_array(nb_flow_ids, sizeof(*flow_ids));
        if (NULL == flow_ids) {
            exit_status = AVERROR(ENOMEM);
            goto finally;
        }
        flow_ids[0] = flow_id;
        flow_id = NULL;
    }
    else {
        domain_path = av_strdup(locator);
        nb_flow_ids = 0;
        flow_ids = NULL;
    }

    // loc takes ownerhsip of domain_path and flow_ids
    loc->domain_path = domain_path;
    domain_path = NULL;
    loc->nb_flow_ids = nb_flow_ids;
    loc->flow_ids = flow_ids;
    flow_ids = NULL;

    exit_status = 0;

finally:

    // no execution path will leave flow_ids set
    av_assert0(NULL == flow_ids);

    av_free(domain_path);
    mxl_uri_free(&uri);

    if (exit_status) {
        memset(loc, 0, sizeof(*loc));
    }

    return exit_status;
}

void mxl_loc_free(mxl_loc *loc)
{
    if (NULL == loc)
        return;

    av_free(loc->domain_path);

    for (int i = 0; i < loc->nb_flow_ids; i++)
        av_free(loc->flow_ids[i]);
    av_free(loc->flow_ids);

    memset(loc, 0, sizeof(*loc));
}
