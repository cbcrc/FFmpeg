/*
 * MXL demuxer for Media eXchange Layer flows
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

/*
 * See: https://github.com/dmf-mxl/mxl
 */

#include "mxl_json.h"
#include "mxl_common.h"
#include "demux.h"
#include "avformat.h"

#include "libavcodec/avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/rational.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

// TODO - move below when flow.h includes it
#include <stdbool.h>

#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/time.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>

typedef enum GrainIndexInit {
    GRAIN_INDEX_INIT_CURRENT = 0,
    GRAIN_INDEX_INIT_HEAD,
    GRAIN_INDEX_INIT_TAIL,
} GrainIndexInit;

typedef enum OnTooLate {
    ON_TOO_LATE_INCREMENT = 0,
    ON_TOO_LATE_RESET
} OnTooLate;

typedef struct MXLContext {

    const AVClass *class;

    // domain and flowid parsed out of input url
    char *domain_path;
    char *flowid;

    // options
    int zero_copy;
    int non_blocking;
    int reset_on_drop;
    int max_frames;
    GrainIndexInit grain_index_init;
    OnTooLate on_too_late;

    // interface objects
    mxlInstance mxl_instance;
    mxlFlowReader mxl_flow_reader;

    // flow state
    mxlRational mxl_grain_rate;
    uint64_t mxl_start_grain_index;
    uint64_t mxl_grain_index;
    bool at_eof;
    int frame_count;
} MXLContext;


#define OFFSET(x) offsetof(MXLContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM

static const AVOption mxl_options[] = {
    {
        .name        = "zero_copy",
        .help        = "Use zero-copy packet delivery (experimental).",
        .offset      = OFFSET(zero_copy),
        .type        = AV_OPT_TYPE_BOOL,
        .default_val = { .i64 = 0 },
        .min         = 0,
        .max         = 1,
        .flags       = FLAGS
    },
    {
        .name = "non_blocking",
        .help = "Don't block waiting for data",
        .offset = OFFSET(non_blocking),
        .type = AV_OPT_TYPE_BOOL,
        .default_val = { .i64 = 0 },
        .min = 0,
        .max = 1,
        .flags = FLAGS
    },
    {
        .name        = "reset_on_drop",
        .help        = "Reset presentation timestamp to zero on frame drop.",
        .offset      = OFFSET(reset_on_drop),
        .type        = AV_OPT_TYPE_BOOL,
        .default_val = { .i64 = 0 },
        .min         = 0,
        .max         = 1,
        .flags       = FLAGS
    },
    {
        .name        = "max_frames",
        .help        = "stop after N frames",
        .offset      = OFFSET(max_frames),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = 0 },
        .min         = 0,
        .max         = INT_MAX,
        .flags       = FLAGS
    },

    // grain_index_init option group
    {
        .name        = "grain_index_init",
        .help        = "initial MXL grain index",
        .offset      = OFFSET(grain_index_init),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = GRAIN_INDEX_INIT_CURRENT },
        .min         = GRAIN_INDEX_INIT_CURRENT,
        .max         = GRAIN_INDEX_INIT_TAIL,
        .flags       = FLAGS,
        .unit        = "grain_index_init",
    },
    {
        .name        = "current",
        .help        = "current time",
        .offset      = 0,
        .type        = AV_OPT_TYPE_CONST,
        .default_val = { .i64 = GRAIN_INDEX_INIT_CURRENT },
        .flags       = FLAGS,
        .unit        = "grain_index_init",
    },
    {
        .name        = "head",
        .help        = "ring buffer head",
        .offset      = 0,
        .type        = AV_OPT_TYPE_CONST,
        .default_val = { .i64 = GRAIN_INDEX_INIT_HEAD },
        .flags       = FLAGS,
        .unit        = "grain_index_init",
    },
    {
        .name        = "tail",
        .help        = "ring buffer tail",
        .offset      = 0,
        .type        = AV_OPT_TYPE_CONST,
        .default_val = { .i64 = GRAIN_INDEX_INIT_TAIL },
        .flags       = FLAGS,
        .unit        = "grain_index_init",
    },

    // on_too_late option group
    {
        .name        = "on_too_late",
        .help        = "action when MXL reports grain index too late",
        .offset      = OFFSET(on_too_late),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = ON_TOO_LATE_INCREMENT },
        .min         = ON_TOO_LATE_INCREMENT,
        .max         = ON_TOO_LATE_RESET,
        .flags       = FLAGS,
        .unit        = "on_too_late",
    },
    {
        .name        = "increment",
        .help        = "increment the grain index",
        .offset      = 0,
        .type        = AV_OPT_TYPE_CONST,
        .default_val = { .i64 = ON_TOO_LATE_INCREMENT },
        .flags       = FLAGS,
        .unit        = "on_too_late",
    },
    {
        .name        = "reset",
        .help        = "reset to position defined by grain_index_init",
        .offset      = 0,
        .type        = AV_OPT_TYPE_CONST,
        .default_val = { .i64 = ON_TOO_LATE_RESET },
        .flags       = FLAGS,
        .unit        = "on_too_late",
    },

    { NULL }
};

#undef OFFSET
#undef FLAGS

static const AVClass mxl_demuxer_class = {
     .class_name = "mxl demuxer",
     .item_name  = av_default_item_name,
     .option     = mxl_options,
     .version    = LIBAVUTIL_VERSION_INT
};

static void log_probe_data(const AVProbeData *p)
{
    logv(NULL,
                "mxl AVProbeData:\n"
                "  filename: %s\n"
                "  buf_size: %d\n"
                "  mime_type: %s\n",
                p->filename ? p->filename : "(null)",
                p->buf_size,
                p->mime_type ? p->mime_type : "(null)");
}

static void log_format_context(const AVFormatContext *s)
{
    av_assert0(s);

    logv(s, "mxl AVFormatContext:\n");
    logv(s, "  url:            %s\n", s->url ? s->url : "(null)");
}

/**
 * Extract the directory from a path of the form
 * "/path/to/domain/<id>.mxl-flow".
 *
 * Returns a newly allocated string or null if the path contains no
 * usable domain component. Caller must free the returned string.
 */
static char *extract_domain_from_path(const char *path)
{
    if (!path)
        return NULL;

    char *tmp = av_strdup(path);
    if (!tmp)
        return NULL;

    const char* dir = av_dirname(tmp);
    av_assert0(dir);

    // reject unusable dir names
    if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0 || *dir == '\0') {
        av_free(tmp);
        return NULL;
    }

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
    if (!path)
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
    av_assert0(id_len > 0);

    char *id = av_malloc(id_len + 1);
    if (!id)
        return NULL;

    memcpy(id, base, id_len);
    id[id_len] = '\0';

    return id;
}

/**
 * Identify MXL flow inputs by filename pattern and state.
 *
 * Expected path:
 *     /path/to/domain/<flowid>.mxl-flow
 *
 * Returns a probe score:
 *   0                       : not recognized
 *   AVPROBE_SCORE_EXTENSION : recognized by file extension only
 *   AVPROBE_SCORE_EXTENSION+: flow exists but inactive
 *   AVPROBE_SCORE_MAX       : active MXL flow detected
 */
static int mxl_probe(const AVProbeData *p) {

    av_assert0(p);

    log_probe_data(p);

    char *domain_path = NULL;
    char *flowid = NULL;
    mxlInstance mxl_instance = NULL;
    mxlFlowReader flow_reader = NULL;
    int score = 0;

    if (!p->filename)
        goto finally;

    if (!av_match_ext(p->filename, MXL_FLOW_EXT))
        goto finally;

    domain_path = extract_domain_from_path(p->filename);
    if (NULL == domain_path) {
        logv(NULL, "failed to extract domain\n");
        goto finally;
    }
    logv(NULL, "domain: \"%s\"\n", domain_path);

    score = AVPROBE_SCORE_EXTENSION;

    flowid = extract_flowid_from_path(p->filename);
    if (NULL == flowid) {
        logv(NULL, "failed to extract MXL flow id\n");
        goto finally;
    }
    logv(NULL, "flow id: \"%s\"\n", flowid);

    mxl_instance = mxlCreateInstance(domain_path, NULL);
    if (NULL == mxl_instance) {
        logv(NULL, "mxlCreateInstance error\n");
        goto finally;
    }

    mxlStatus mxl_status = mxlCreateFlowReader(mxl_instance, flowid, "",
                                               &flow_reader);
    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlCreateFlowReader error %s for flow: \"%s\"\n",
                    mxl_status_to_str(mxl_status), flowid);
        goto finally;
    }

    mxlFlowInfo info = {0};
    mxl_status = mxlFlowReaderGetInfo(flow_reader, &info);
    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlFlowReaderGetInfo error %s\n",
                    mxl_status_to_str(mxl_status));
        goto finally;
    }

    bool active = false;
    mxl_status = mxlIsFlowActive(mxl_instance, flowid, &active);
    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlIsFlowActive error %s\n",
                    mxl_status_to_str(mxl_status));
        goto finally;
    }

    if (active)
        score = AVPROBE_SCORE_MAX;
    else
        score = (score + AVPROBE_SCORE_MAX)/2;

finally:

    av_free(domain_path);
    av_free(flowid);

    if (flow_reader) {
        av_assert0(mxl_instance);
        mxl_status = mxlReleaseFlowReader(mxl_instance, flow_reader);
        if (MXL_STATUS_OK != mxl_status)
            logw(NULL, "mxlReleaseFlowReader error %s\n", mxl_status_to_str(mxl_status));
    }

    if (mxl_instance) {
        mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(NULL, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    if (score) {
        const char* reason = "unknown";

        if (score <= AVPROBE_SCORE_EXTENSION) {
            reason = MXL_DOT_FLOW_EXT " extension match";
        }
        else if (score < AVPROBE_SCORE_MAX) {
            reason = "inactive flow";
        }
        else {
            av_assert0(score == AVPROBE_SCORE_MAX);
            reason = "active flow";
        }

        logv(NULL, "MXL probe successful (%s)\n", reason);
    }

    logv(NULL, "MXL probe score = %d\n", score);

    return score;
}


/**
 * Open the MXL flow, parse the flow definition, and set up the stream.
 *
 * Return values:
 *   0                   - success
 *   AVERROR(EINVAL)     - unrecognized URL
 *   AVERROR_INVALIDDATA - flow definition error
 *   AVERROR(EIO)        - MXL API error, or flow inactive
 *   AVERROR(ENOMEM)     - allocation failure
*/
static int mxl_read_header(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    log_format_context(s);

    int exit_status = AVERROR_UNKNOWN;

    char* domain_path = NULL;
    char* flowid = NULL;

    mxlInstance mxl_instance = NULL;
    mxlFlowReader flow_reader = NULL;

    char *flow_def_json = NULL;
    mxl_json_doc flow_def_doc = {0};
    mxl_json_str id = {0};
    mxl_json_str desc = {0};
    mxl_json_str label = {0};
    mxl_json_str media_type = {0};
    mxl_json_str colorspace = {0};
    mxl_json_str format = {0};
    mxl_json_num frame_width = {0};
    mxl_json_num frame_height = {0};
    mxl_json_num grain_rate_num = {0};
    mxl_json_num grain_rate_den = {0};

    if (!s->url) {
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    domain_path =  extract_domain_from_path(s->url);
    if (!domain_path) {
        loge(s, "failed to extract domain from %s\n", s->url);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    mxl_instance = mxlCreateInstance(domain_path, NULL);
    if (NULL == mxl_instance) {
        loge(s, "mxlCreateInstance error\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    flowid = extract_flowid_from_path(s->url);
    if (!flowid) {
        loge(s, "failed to extract flowid from %s\n", s->url);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    mxlStatus mxl_status = mxlCreateFlowReader(mxl_instance, flowid, "",
                                               &flow_reader);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlCreateFlowReader error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    mxlFlowInfo flow_info = {0};
    mxl_status = mxlFlowReaderGetInfo(flow_reader, &flow_info);
    if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlFlowReaderGetInfo failed\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    bool active = false;
    mxl_status = mxlIsFlowActive(mxl_instance, flowid, &active);
    if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlIsFlowActive failed\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    if (!active) {
        loge(s, "flow exists but is not active\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    logv(s, "active flow with flowid %s in %s\n",
                flowid, domain_path);

    size_t flow_def_json_size = 0;
    // initial call with NULL buffer gets size
    mxl_status = mxlGetFlowDef(mxl_instance, flowid, NULL, &flow_def_json_size);
    if (MXL_ERR_INVALID_ARG == mxl_status) {
        if (flow_def_json_size <= 0) {
            loge(s, "flow def buffer size sanity (%zu)\n", flow_def_json_size);
            exit_status = AVERROR(EIO);
            goto finally;
        }

        flow_def_json = av_malloc(flow_def_json_size);
        if (!flow_def_json) {
            loge(s, "failed to allocate flow_def_json of size %zu\n", flow_def_json_size);
            exit_status = AVERROR(ENOMEM);
            goto finally;
        }
    }
    else {
        loge(s, "unexpected mxlGetFlowDef status (%s)\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    mxl_status = mxlGetFlowDef(mxl_instance, flowid, flow_def_json, &flow_def_json_size);
    if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlGetFlowDef failed\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    logv(s, "good flow definition read:\n%s\n", flow_def_json);

    int json_status = mxl_json_doc_build(flow_def_json, &flow_def_doc);
    if (json_status) {
        loge(s, "json flow def parse error %d\n", json_status);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

#define GET1(X,Y,K1) \
    X = mxl_json_doc_get_##Y(&flow_def_doc, K1); \
    if (X.err) { \
        loge(s, "error reading flow def parameter \"%s\"\n", #K1); \
        exit_status = AVERROR_INVALIDDATA; \
        goto finally; \
    }
#define GET2(X,Y,K1,K2) \
    X = mxl_json_doc_get_##Y(&flow_def_doc, K1, K2); \
    if (X.err) { \
        loge(s, "error reading flow def parameter \"%s.%s\"\n", #K1, #K2); \
        exit_status = AVERROR_INVALIDDATA; \
        goto finally; \
    }

    GET1(id, string1, "id");
    GET1(desc, string1, "description");
    GET1(label, string1, "label");
    GET1(media_type, string1, "media_type");
    GET1(colorspace, string1, "colorspace");
    GET1(format, string1, "format");
    GET1(frame_width, double1, "frame_width");
    GET1(frame_height, double1, "frame_height");
    GET2(grain_rate_num, double2, "grain_rate", "numerator");
    GET2(grain_rate_den, double2, "grain_rate", "denominator");

#undef GET1
#undef GET2

    av_assert0(flowid);
    if (!id.value || strcmp(id.value, flowid) != 0) {
        loge(s, "flow id sanity, flow def id != flow path id, %s != %s\n",
                  id.value, flowid);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (frame_width.value <= 0 || frame_height.value <= 0) {
        loge(s, "resolution sanity, range error, %dx%d\n",
             (int)frame_width.value, (int)frame_height.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (grain_rate_den.value <= 0.0) {
        loge(s, "grain rate sanity, non-positive denominator: %f\n",
             grain_rate_den.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (grain_rate_num.value > INT_MAX || grain_rate_num.value < INT_MIN ||
        grain_rate_den.value > INT_MAX || grain_rate_den.value < INT_MIN) {
        loge(s, "grain rate sanity, exceeds INT range: %f / %f\n",
             grain_rate_num.value, grain_rate_den.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (!isfinite(frame_width.value) || !isfinite(frame_height.value)) {
        loge(s, "resolution sanity, non-finite value: %f x %f\n",
             frame_width.value, frame_height.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (!isfinite(grain_rate_num.value) || !isfinite(grain_rate_den.value)) {
        loge(s, "grain rate sanity, non-finite value: %f / %f\n",
             grain_rate_num.value, grain_rate_den.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if ((int64_t)grain_rate_num.value != flow_info.discrete.grainRate.numerator ||
        (int64_t)grain_rate_den.value != flow_info.discrete.grainRate.denominator) {
        loge(s, "grain rate sanity, flow def rate != flow info rate"
                  ", {%"PRId64",%"PRId64"} != {%"PRId64",%"PRId64"}\n",
             (int64_t)grain_rate_num.value, (int64_t)grain_rate_den.value,
             flow_info.discrete.grainRate.numerator,
             flow_info.discrete.grainRate.denominator);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (frame_width.value > INT_MAX || frame_height.value > INT_MAX) {
        loge(s, "resolution sanity, exceeds INT_MAX: %f x %f\n",
             frame_width.value, frame_height.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (strcmp("video/v210", media_type.value)) {
        loge(s, "unsupported media_type: %s\n", media_type.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    logv(s, "good flow def: %s, %dx%d, %d/%d\n",
                media_type.value,
                (int)frame_width.value, (int)frame_height.value,
                (int)grain_rate_num.value, (int)grain_rate_den.value);

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) {
        loge(s, "failed to allocate new stream\n");
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_V210;
    st->codecpar->codec_tag = MKTAG('v','2','1','0');
    st->codecpar->bits_per_coded_sample = 10;
    st->codecpar->bits_per_raw_sample = 10;
    st->codecpar->width = (int)frame_width.value;
    st->codecpar->height = (int)frame_height.value;
    st->codecpar->sample_aspect_ratio = (AVRational){1,1};
    st->codecpar->framerate = (AVRational){(int)grain_rate_num.value, (int)grain_rate_den.value};
    st->codecpar->field_order = AV_FIELD_PROGRESSIVE;

    st->avg_frame_rate = st->codecpar->framerate;
    st->time_base = av_inv_q(st->codecpar->framerate);

#define SET(X, Y) \
    if (av_dict_set(&st->metadata, X, Y, 0) < 0) { \
        logw(s, "failed to set stream metadata %s=%s\n", X, Y); \
    }

    SET("mxl_id", id.value);
    SET("mxl_format", format.value);
    SET("mxl_label", label.value);
    SET("mxl_description", desc.value);
    SET("mxl_media_type", media_type.value);
    SET("mxl_colorspace", colorspace.value);

#undef SET

    MXLContext *p = s->priv_data;

    p->domain_path = domain_path;
    domain_path = NULL;

    p->flowid = flowid;
    flowid = NULL;

    p->mxl_instance = mxl_instance;
    mxl_instance = NULL;

    p->mxl_flow_reader = flow_reader;
    flow_reader = NULL;

    p->mxl_grain_rate = flow_info.discrete.grainRate;
    p->mxl_start_grain_index = 0;
    p->mxl_grain_index = 0;

    exit_status = 0;

finally:

    av_free(domain_path);
    av_free(flowid);

    mxl_json_doc_release(&flow_def_doc);

    av_free(id.value);
    av_free(desc.value);
    av_free(label.value);
    av_free(media_type.value);
    av_free(colorspace.value);
    av_free(format.value);
    av_free(flow_def_json);

    if (flow_reader) {
        av_assert0(mxl_instance);
        mxl_status = mxlReleaseFlowReader(mxl_instance, flow_reader);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowReader error %s\n", mxl_status_to_str(mxl_status));
    }

    if (mxl_instance) {
        mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    return exit_status;
}

// zero copy packet release callback
static void mxl_zero_copy_release_cb(void *ctx, uint8_t *data)
{
    MXLContext *p = (MXLContext*)ctx;
    (void)p;
    logv(NULL, "mxl_zero_copy_release_cb %p\n", data);
}

/**
 * Read next frame, create a new packet, copy frame data to packet.
 *
 * Return codes:
 *   - 0                   - success, one packet was produced
 *   - AVERROR_EOF         - no more data, flow is inactive
 *   - AVERROR_INVALIDDATA - unexpected program state detected
 *   - AVERROR(EAGAIN)     - no data, try again later
 *   - other AVERROR_*     - fatal read error
 *
 */
static int mxl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    av_assert0(s && s->priv_data);

    MXLContext  *p  = s->priv_data;

    // early bail-out to avoid flooding the log
    if (p->at_eof) {
        logv(s, "at eof\n");
        return AVERROR_EOF;
    }

    if (p->max_frames > 0 && p->frame_count >= p->max_frames) {
        logv(s, "at max_frames (%d)\n", p->max_frames);
        return AVERROR_EOF;
    }

    // target stream
    const int stream_index = 0;
    if (s->nb_streams <= stream_index || !s->streams[stream_index])
        return AVERROR_INVALIDDATA;
    AVStream *st = s->streams[stream_index];

    int exit_status = AVERROR_UNKNOWN;
    bool active = false;

    mxlStatus mxl_status = mxlIsFlowActive(p->mxl_instance, p->flowid, &active);
    if (MXL_STATUS_OK == mxl_status && !active) {
        logv(s, "flow not active, stopping\n");
        p->at_eof = true;
        exit_status = AVERROR_EOF;
        goto finally;
    }
    else if (MXL_ERR_FLOW_NOT_FOUND == mxl_status) {
        logv(s, "flow not found, stopping\n");
        p->at_eof = true;
        exit_status = AVERROR_EOF;
        goto finally;
    }
    else if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlIsFlowActive failed %s, retrying\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }

    // init grain index
    if (0 == p->mxl_grain_index) {
        switch (p->grain_index_init) {
        case GRAIN_INDEX_INIT_CURRENT: {
            uint64_t cur_grain_index = mxlGetCurrentIndex(&p->mxl_grain_rate);
            if (MXL_UNDEFINED_INDEX == cur_grain_index) {
                loge(s, "mxlGetCurrentIndex error MXL_UNDEFINED_INDEX\n");
                exit_status = AVERROR_BUG;
                goto finally;
            }
            p->mxl_grain_index = cur_grain_index;
            logv(s, "init grain_index = %"PRIu64", policy: current time\n",
                 p->mxl_grain_index);
        }
        break;
        case GRAIN_INDEX_INIT_HEAD: {
            mxlFlowInfo flow_info = {0};
            mxl_status = mxlFlowReaderGetInfo(p->mxl_flow_reader, &flow_info);
            if (mxl_status != MXL_STATUS_OK) {
                loge(s, "mxlFlowReaderGetInfo failed\n");
                exit_status = AVERROR(EIO);
                goto finally;
            }
            p->mxl_grain_index = flow_info.discrete.headIndex;
            logv(s, "init grain index = %"PRIu64", policy: headIndex\n",
                 p->mxl_grain_index);
        }
        break;
        case GRAIN_INDEX_INIT_TAIL: {
            mxlFlowInfo flow_info = {0};
            mxl_status = mxlFlowReaderGetInfo(p->mxl_flow_reader, &flow_info);
            if (mxl_status != MXL_STATUS_OK) {
                loge(s, "mxlFlowReaderGetInfo failed\n");
                exit_status = AVERROR(EIO);
                goto finally;
            }
            p->mxl_grain_index = flow_info.discrete.headIndex - flow_info.discrete.grainCount + 1;
            logv(s, "init grain index = %"PRIu64", policy: tail index\n",
                 p->mxl_grain_index);
        }
        break;
        default:
            loge(s, "unrecognized grain_index_init = \"%d\"\n", p->grain_index_init);
            exit_status = AVERROR_BUG;
            goto finally;
        }

        if (p->reset_on_drop) {
            // presentation timestamp is mxl_start_grain_index relative
            p->mxl_start_grain_index = p->mxl_grain_index;
        }
    }

    // init the start grain index
    if (0 == p->mxl_start_grain_index)
        p->mxl_start_grain_index = p->mxl_grain_index;

    mxlGrainInfo grain_info = {0};
    uint8_t *mxl_payload;
    if (p->non_blocking) {
        mxl_status = mxlFlowReaderGetGrainNonBlocking(p->mxl_flow_reader, p->mxl_grain_index,
                                                      &grain_info, &mxl_payload);
    }
    else {
        const AVRational ns_time_base = {1, 1000000000};
        int64_t timeout_ns = av_rescale_q(1, st->time_base, ns_time_base);
        av_assert1(timeout_ns >= 0);
        mxl_status = mxlFlowReaderGetGrain(p->mxl_flow_reader, p->mxl_grain_index,
                                           (uint64_t)timeout_ns, &grain_info, &mxl_payload);
    }

    if (MXL_ERR_OUT_OF_RANGE_TOO_LATE == mxl_status) {
        switch(p->on_too_late) {
        case ON_TOO_LATE_INCREMENT:
            logv(s, "no grain, too late with grain index %"PRIu64", increment index and try again\n",
                 p->mxl_grain_index);
            p->mxl_grain_index++;
            exit_status = AVERROR(EAGAIN);
            break;
        case ON_TOO_LATE_RESET:
            logv(s, "no grain, too late with grain index %"PRIu64", reset index and try again\n",
                 p->mxl_grain_index);
            exit_status = AVERROR(EAGAIN);
            p->mxl_grain_index = 0;
            break;
        default:
            loge(s, "unrecognized on_too_late = \"%d\"\n", p->on_too_late);
            exit_status = AVERROR_BUG;
        }
        goto finally;
    }
    else if (MXL_ERR_OUT_OF_RANGE_TOO_EARLY == mxl_status) {
        logv(s, "no grain, too early with grain index %"PRIu64", try again\n",
             p->mxl_grain_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_ERR_TIMEOUT == mxl_status) {
        logv(s, "no grain, timed out with grain index %"PRIu64", try again\n",
             p->mxl_grain_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowReaderGetGrain error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    if (grain_info.grainSize != grain_info.commitedSize) {
        loge(s, "grain size sanity error, grainSize != commitedSize, %"PRIu32" != %"PRIu32"\n",
             grain_info.grainSize, grain_info.commitedSize);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (p->zero_copy) {
        AVBufferRef *zcbuf = av_buffer_create(mxl_payload, grain_info.commitedSize,
                                            mxl_zero_copy_release_cb, p, 0);
        if (!zcbuf) {
            loge(s, "av_buffer_create error\n");
            exit_status = AVERROR(ENOMEM);
            goto finally;
        }

        pkt->buf = zcbuf;
        pkt->data = mxl_payload;
        pkt->size = grain_info.commitedSize;
        pkt->stream_index = stream_index;
    }
    else {
        av_assert1(!pkt->buf);
        int rc = av_new_packet(pkt, grain_info.commitedSize);
        if (rc < 0) {
            loge(s, "av_new_packet error %d\n", rc);
            exit_status = rc;
            goto finally;
        }

        memcpy(pkt->data, mxl_payload, grain_info.commitedSize);
    }

    int64_t rel_grain_count = p->mxl_grain_index - p->mxl_start_grain_index;
    av_assert1(rel_grain_count >= 0);

    pkt->pos = -1;
    pkt->pts = rel_grain_count;
    pkt->dts = rel_grain_count;
    pkt->duration = 1;
    pkt->stream_index = stream_index;
    pkt->flags |= AV_PKT_FLAG_KEY;

    logv(s, "good frame, grain_index=%"PRIu64", relative=%"PRId64
         ", mxl_payload=%p, pkt->data=%p\n",
         p->mxl_grain_index, rel_grain_count, mxl_payload, pkt->data);

    // all good, advance to next frame
    p->mxl_grain_index++;
    p->frame_count++;

    exit_status = 0;

finally:

    if (exit_status < 0)
        av_packet_unref(pkt);

    return exit_status;
}

/**
 * Free or release all allocated resources.
 *
 * Return codes:
 *   - 0                   - success
 */
static int mxl_read_close(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    MXLContext *p = s->priv_data;

    if (p->mxl_flow_reader) {
        av_assert0(p->mxl_instance);
        if (MXL_STATUS_OK != mxlReleaseFlowReader(p->mxl_instance, p->mxl_flow_reader))
            logw(s, "mxlReleaseFlowReader error\n");
        p->mxl_flow_reader = NULL;
    }

    if (p->mxl_instance) {
        if (MXL_STATUS_OK != mxlDestroyInstance(p->mxl_instance))
            logw(s, "mxlDestroyInstance error\n");
        p->mxl_instance = NULL;
    }

    av_free(p->flowid);
    p->flowid = NULL;

    av_free(p->domain_path);
    p->domain_path = NULL;

    p->mxl_grain_rate = (mxlRational){0};
    p->mxl_start_grain_index = 0;
    p->mxl_grain_index = 0;
    p->at_eof = false;

    return 0;
}

// MXL demuxer definition
const FFInputFormat ff_mxl_demuxer = {
     // AVInputFormat (public-facing fields)
     .p.name       = "mxl",
     .p.long_name  = NULL_IF_CONFIG_SMALL(MXL_LONG_NAME),
     .p.extensions = MXL_FLOW_EXT,
     .p.priv_class = &mxl_demuxer_class,
     .p.flags      = AVFMT_NOFILE          // not a file
                     | AVFMT_NOBINSEARCH   // no binary search seek
                     | AVFMT_NOGENSEARCH   // no generic timestamp search
                     | AVFMT_NO_BYTE_SEEK, // no byte position seek

     // private state
     .priv_data_size = sizeof(MXLContext),

     // demuxer callbacks
     .read_probe   = mxl_probe,
     .read_header  = mxl_read_header,
     .read_packet  = mxl_read_packet,
     .read_close   = mxl_read_close,

     // unimplemented
     .read_seek            = NULL, // not seekable
     .read_seek2           = NULL, // no timestamp seeking
     .read_play            = NULL, // no pause/resume
     .read_pause           = NULL, // no pause/resume
     .read_timestamp       = NULL, // no timestamp probing
     .get_device_list      = NULL, // not a device input

     // internal behavior flags (init/cleanup semantic)
     .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
};
