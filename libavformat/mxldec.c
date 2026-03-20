/*
 * MXL demuxer for Media eXchange Layer flows
 *
 * AVFMT_NOFILE demuxer; requires either
 *     "/path/to/domain/<flow_id>.mxl-flow"
 *  or "mxl:///path/to/domain?id=<flow_id>[&id=<flow_id>]"
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
#include "mxl_uri.h"
#include "mxl_loc.h"
#include "mxl_common.h"
#include "mxl_status.h"
#include "demux.h"
#include "avformat.h"

#include "libswresample/swresample.h"
#include "libavutil/channel_layout.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/rational.h"
#include "libavutil/error.h"
#include "libavutil/time.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/time.h>

#include <stdbool.h>
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

typedef struct VideoState {
    mxlRational mxl_grain_rate;
    uint64_t mxl_start_grain_index;
    uint64_t mxl_grain_index;
    int frame_count;
} VideoState;

typedef struct AudioState {
    mxlRational mxl_sample_rate;
    uint64_t mxl_start_sample_index;
    uint64_t mxl_sample_index;
    struct SwrContext *swr_context;
    int sample_point_count;
} AudioState;

typedef enum FlowType {
    UNDEFINED_FLOW = 0,
    VIDEO_FLOW = 1,
    AUDIO_FLOW = 2
} FlowType;

typedef union FlowState {
    VideoState video;
    AudioState audio;
} FlowState;

typedef struct StreamContext {
    mxlFlowReader mxl_flow_reader;
    FlowType flow_type;
    FlowState flow;
    int stream_index;
} StreamContext;

typedef struct MXLTuningParams {
    int audio_samples_per_read;
    bool video_blocking_read;
} MXLTuningParams;

typedef struct MXLContext {

    const AVClass *class;

    // domain and flow_ids parsed out of input url
    mxl_loc loc;

    // options
    int zero_copy;
    int blocking;
    int reset_on_drop;
    int max_video_frames;
    int max_audio_samples;
    int max_audio_samples_per_read;
    GrainIndexInit grain_index_init;
    OnTooLate on_too_late;

    // interface objects
    mxlInstance mxl_instance;

    // stream context array (size is loc.nb_flow_ids)
    StreamContext* stream_contexts;

    // stream configuration dependant tuning parameters
    MXLTuningParams tuning;

    // flag end of streams
    bool at_eof;

    // the index of the next stream context to read
    int next_stream_context_idx;

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
        .name = "blocking",
        .help = "Use blocking video read: auto (default), 0=non-blocking, 1=blocking" ,
        .offset = OFFSET(blocking),
        .type = AV_OPT_TYPE_INT,
        .default_val = { .i64 = -1 },
        .min = -1,
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
        .name        = "max_video_frames",
        .help        = "stop at exactly N video frames",
        .offset      = OFFSET(max_video_frames),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = 0 },
        .min         = 0,
        .max         = INT_MAX,
        .flags       = FLAGS
    },
    {
        .name        = "max_audio_samples",
        .help        = "stop at exactly N audio sample points",
        .offset      = OFFSET(max_audio_samples),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = 0 },
        .min         = 0,
        .max         = INT_MAX,
        .flags       = FLAGS
    },
    {
        .name        = "max_audio_samples_per_read",
        .help        = "max audio sample points per MXL read",
        .offset      = OFFSET(max_audio_samples_per_read),
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
    av_assert1(s);

    logv(s, "mxl AVFormatContext:\n");
    logv(s, "  url: %s\n", s->url ? s->url : "(null)");
}

static bool probe_is_flow_active(mxlInstance mxl_instance, const char* flow_id)
{
    bool active = false;
    mxlFlowReader flow_reader = NULL;

    mxlStatus mxl_status = mxlCreateFlowReader(mxl_instance, flow_id, "",
                                               &flow_reader);

    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlCreateFlowReader error %s for flow: \"%s\"\n",
                    mxl_status_to_str(mxl_status), flow_id);
        goto finally;
    }

    mxlFlowInfo info = {0};
    mxl_status = mxlFlowReaderGetInfo(flow_reader, &info);
    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlFlowReaderGetInfo error %s\n",
                    mxl_status_to_str(mxl_status));
        goto finally;
    }

    mxl_status = mxlIsFlowActive(mxl_instance, flow_id, &active);
    if (MXL_STATUS_OK != mxl_status) {
        logv(NULL, "mxlIsFlowActive error %s\n",
                    mxl_status_to_str(mxl_status));
        goto finally;
    }

finally:

    if (flow_reader) {
        av_assert0(mxl_instance);
        mxl_status = mxlReleaseFlowReader(mxl_instance, flow_reader);
        if (MXL_STATUS_OK != mxl_status)
            logw(NULL, "mxlReleaseFlowReader error %s for flow_id %s\n",
                 mxl_status_to_str(mxl_status), flow_id);
    }

    return active;
}


static int validate_locator(void *logctx, const mxl_loc* loc) {
    av_assert0(loc);

    if (loc->nb_flow_ids < 1) {
        logv(logctx, "locator requires at least one flow ID\n");
        return -1;
    }

    // mxl_loc invariants (for nb_flow_ids > 0)
    av_assert0(loc->domain_path);
    av_assert0(loc->flow_ids);

    return 0;
}


/**
 * Identify MXL flow inputs by filename pattern and state.
 *
 * Expected identifier:
 *     /path/to/domain/<flowid>.mxl-flow
 *     mxl:///path/to/domain?id=<flowid>
 *
 * Returns a probe score:
 *   0                       : not recognized
 *   AVPROBE_SCORE_EXTENSION : recognized by URI scheme or file extension
 *   AVPROBE_SCORE_EXTENSION+: flow exists but inactive
 *   AVPROBE_SCORE_MAX       : active MXL flow detected
 */
static int mxl_probe(const AVProbeData *p) {

    av_assert0(p);

    log_probe_data(p);

    mxlInstance mxl_instance = NULL;
    int score = 0;
    mxl_loc loc = {0};

    if (!p->filename)
        goto finally;

    int rc = mxl_loc_parse(NULL, p->filename, &loc);
    if (rc) {
        logv(NULL, "failed to parse locator: \"%s\"\n", p->filename);
        goto finally;
    }

    rc = validate_locator(NULL, &loc);
    if (rc) {
        logv(NULL, "invalid locator: %s\n", p->filename);
        goto finally;
    }

    score = AVPROBE_SCORE_EXTENSION;

    mxl_instance = mxlCreateInstance(loc.domain_path, NULL);
    if (NULL == mxl_instance) {
        logv(NULL, "mxlCreateInstance error\n");
        goto finally;
    }

    logv(NULL, "probe MXL domain: \"%s\"\n", loc.domain_path);
    logv(NULL, "probe %d MXL flows:\n", loc.nb_flow_ids);
    int i = 0;
    for (i = 0; i < loc.nb_flow_ids; i++)
        logv(NULL, "  flow id: \"%s\"\n", loc.flow_ids[i]);

    bool all_flows_active = true;
    for (i = 0; i < loc.nb_flow_ids; i++) {
        if (!probe_is_flow_active(mxl_instance, loc.flow_ids[i])) {
            all_flows_active = false;
            goto finally;
        }
    }

    if (all_flows_active)
        score = AVPROBE_SCORE_MAX;
    else
        score = (score + AVPROBE_SCORE_MAX)/2;

finally:

    mxl_loc_free(&loc);

    if (mxl_instance) {
        mxlStatus mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(NULL, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    if (score) {
        const char* reason = "unknown";

        if (score <= AVPROBE_SCORE_EXTENSION) {
            reason = MXL_DOT_FLOW_EXT " extension or URI scheme match";
        }
        else if (score < AVPROBE_SCORE_MAX) {
            reason = "one or more flows is inactive";
        }
        else {
            av_assert0(score == AVPROBE_SCORE_MAX);
            reason = "all flows are active";
        }

        logv(NULL, "probe successful (%s)\n", reason);
    }

    logv(NULL, "probe score = %d\n", score);

    return score;
}

#define GET1(X,Y,D,K1) \
    X = mxl_json_doc_get_##Y(D, K1); \
    if (X.err) { \
        loge(s, "error reading flow def parameter \"%s\"\n", #K1); \
        exit_status = AVERROR_INVALIDDATA; \
        goto finally; \
    }
#define GET2(X,Y,D,K1,K2) \
    X = mxl_json_doc_get_##Y(D, K1, K2); \
    if (X.err) { \
        loge(s, "error reading flow def parameter \"%s.%s\"\n", #K1, #K2); \
        exit_status = AVERROR_INVALIDDATA; \
        goto finally; \
    }

#define SET_META(ST, X, Y)                          \
    if (av_dict_set(&ST->metadata, X, Y, 0) < 0) {            \
        logw(s, "failed to set stream metadata %s=%s\n", X, Y); \
    }

static inline int pos_int_range_sanity(AVFormatContext *s, double value, const char* detail)
{
    if (value <= 0 || value > (double)INT_MAX) {
        loge(s, "%s sanity, range error, %.0f\n", detail, value);
        return AVERROR_INVALIDDATA;
    }

    if (floor(value) != value) {
        loge(s, "%s sanity, non-integer value %.6f\n", detail, value);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

#define POS_INT_SANITY(S, VALUE, DETAIL) \
    { int rc = pos_int_range_sanity(S, VALUE, DETAIL); \
      if (rc) { exit_status = rc; goto finally; } }

static int read_video_header(AVFormatContext *s, mxlFlowInfo *flow_info,
                             mxl_json_doc *flow_def_doc, AVStream *st,
                             FlowState* flow_state, const char* media_type)
{
    av_assert0(s && flow_info && flow_def_doc && st && flow_state && media_type);

    int exit_status = AVERROR_UNKNOWN;

    mxl_json_str colorspace = {0};
    mxl_json_num frame_width = {0};
    mxl_json_num frame_height = {0};
    mxl_json_num grain_rate_num = {0};
    mxl_json_num grain_rate_den = {0};

    if (strcmp(media_type, "video/v210")) {
        loge(s, "unexpected video type \"%s\"\n", media_type);
        exit_status = AVERROR_BUG;
        goto finally;
    }

    GET1(colorspace, string1, flow_def_doc, "colorspace");
    GET1(frame_width, double1, flow_def_doc, "frame_width");
    GET1(frame_height, double1, flow_def_doc, "frame_height");
    GET2(grain_rate_num, double2, flow_def_doc, "grain_rate", "numerator");
    GET2(grain_rate_den, double2, flow_def_doc, "grain_rate", "denominator");

    POS_INT_SANITY(s, frame_width.value, "frame_width");
    POS_INT_SANITY(s, frame_height.value, "frame_height");
    POS_INT_SANITY(s, grain_rate_num.value, "grain_rate_num");
    POS_INT_SANITY(s, grain_rate_den.value, "grain_rate_den");

    if ((int64_t)grain_rate_num.value != flow_info->config.common.grainRate.numerator ||
        (int64_t)grain_rate_den.value != flow_info->config.common.grainRate.denominator) {
        loge(s, "grain rate sanity, flow def rate != flow info rate"
                  ", {%"PRId64",%"PRId64"} != {%"PRId64",%"PRId64"}\n",
             (int64_t)grain_rate_num.value, (int64_t)grain_rate_den.value,
             flow_info->config.common.grainRate.numerator,
             flow_info->config.common.grainRate.denominator);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    logv(s, "good video flow def: %s %dx%d at %d/%d fps\n",
         media_type,
         (int)frame_width.value, (int)frame_height.value,
         (int)grain_rate_num.value, (int)grain_rate_den.value);

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

    SET_META(st, "mxl_colorspace", colorspace.value);

    flow_state->video.mxl_grain_rate = flow_info->config.common.grainRate;
    flow_state->video.mxl_start_grain_index = 0;
    flow_state->video.mxl_grain_index = 0;

    exit_status = 0;

finally:

    av_free(colorspace.value);

    return exit_status;
}

// create SwrContext for planar to interleaved audio conversion
static int init_swr_context(struct SwrContext **swr_context,
                            int channels, int sample_rate)
{
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout,  channels);
    av_channel_layout_default(&out_layout, channels);

    int rc = swr_alloc_set_opts2(swr_context,
                                 &out_layout, AV_SAMPLE_FMT_FLT,  sample_rate,
                                 &in_layout,  AV_SAMPLE_FMT_FLTP, sample_rate,
                                 0, NULL);
    if (rc < 0)
        return rc;

    rc = swr_init(*swr_context);
    if (rc < 0) {
        swr_free(swr_context);
        return rc;
    }

    return 0;
}

static int read_audio_header(AVFormatContext *s, mxlFlowInfo *flow_info,
                             mxl_json_doc *flow_def_doc, AVStream *st,
                             FlowState* flow_state, const char* media_type)
{
    av_assert0(s && flow_info && flow_def_doc && st && flow_state && media_type);

    int exit_status = AVERROR_UNKNOWN;

    mxl_json_num sample_rate_num = {0};
    mxl_json_num sample_rate_den = {0};
    mxl_json_num channel_count;
    mxl_json_num bit_depth;

    if (strcmp(media_type, "audio/float32")) {
        loge(s, "unexpected audio type \"%s\"\n", media_type);
        exit_status = AVERROR_BUG;
        goto finally;
    }

    GET2(sample_rate_num, double2, flow_def_doc, "sample_rate", "numerator");
    GET1(channel_count, double1, flow_def_doc, "channel_count");
    GET1(bit_depth, double1, flow_def_doc, "bit_depth");
    sample_rate_den = mxl_json_doc_get_double2(flow_def_doc, "sample_rate", "denominator");

    // if sample_rate.denominator is present then it must have value 1.0
    if (0 == sample_rate_den.err && 1.0 != sample_rate_den.value) {
        loge(s, "sample_rate_den sanity, must be absent or value 1.0, actual %f\n",
             sample_rate_den.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if (1LL != flow_info->config.common.grainRate.denominator) {
        loge(s, "grainRate.denominator sanity, must be value 1, actual %"PRId64"\n",
             flow_info->config.common.grainRate.denominator);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if ((int64_t)sample_rate_num.value != flow_info->config.common.grainRate.numerator) {
        loge(s, "sample rate sanity, flow def rate != flow info rate"
                  ", {%"PRId64",%"PRId64"} != {%"PRId64",%"PRId64"}\n",
             (int64_t)sample_rate_num.value, 1LL,
             flow_info->config.common.grainRate.numerator,
             flow_info->config.common.grainRate.denominator);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    POS_INT_SANITY(s, sample_rate_num.value, "sample_rate_num");
    POS_INT_SANITY(s, channel_count.value, "channel_count");
    POS_INT_SANITY(s, bit_depth.value, "bit_depth");

    if ((uint32_t)channel_count.value != flow_info->config.continuous.channelCount) {
        loge(s, "channel count sanity, flow def != flow info (%u != %u)\n",
             (uint32_t)channel_count.value, flow_info->config.continuous.channelCount);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    if ((int)bit_depth.value != 32) {
        loge(s, "bit_depth sanity, bit_depth != 32, actual %d\n",
             (int)bit_depth.value);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    logv(s, "good audio flow def: %s %d channels at %d hz\n",
         media_type,
         (int)channel_count.value, (int)sample_rate_num.value);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_PCM_F32LE;
    st->codecpar->codec_tag  = 0;

    st->codecpar->format = AV_SAMPLE_FMT_FLT;
    st->codecpar->bits_per_coded_sample = 32;
    st->codecpar->bits_per_raw_sample   = 32;

    st->codecpar->sample_rate = (int)sample_rate_num.value;
    st->codecpar->ch_layout.nb_channels = (int)channel_count.value;
    st->codecpar->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;

    st->time_base     = (AVRational){ 1, st->codecpar->sample_rate };
    st->avg_frame_rate = (AVRational){ 0, 1 };

    flow_state->audio.mxl_sample_rate = flow_info->config.common.grainRate;
    int rc = init_swr_context(&flow_state->audio.swr_context,
                              st->codecpar->ch_layout.nb_channels,
                              st->codecpar->sample_rate);
    if (rc < 0) {
        loge(s, "init_swr_context failed\n");
        exit_status = rc;
        goto finally;
    }

    exit_status = 0;

finally:

    return exit_status;
}

static int read_flow(AVFormatContext *s, mxlInstance mxl_instance,
                     const char* flow_id, StreamContext* stream_ctx)
{
    av_assert0(s && s->priv_data &&
               mxl_instance && flow_id &&
               stream_ctx);

    int exit_status = AVERROR_UNKNOWN;
    mxlFlowReader flow_reader = NULL;
    FlowType flow_type = UNDEFINED_FLOW;
    FlowState flow_state = {0};

    char *flow_def_json = NULL;
    mxl_json_doc flow_def_doc = {0};
    mxl_json_str id = {0};
    mxl_json_str desc = {0};
    mxl_json_str label = {0};
    mxl_json_str format = {0};
    mxl_json_str media_type = {0};
    char *unescaped_media_type = NULL;

    mxlStatus mxl_status = mxlCreateFlowReader(mxl_instance, flow_id, "",
                                               &flow_reader);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlCreateFlowReader error %s for flow ID %s\n",
             mxl_status_to_str(mxl_status), flow_id);
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
    mxl_status = mxlIsFlowActive(mxl_instance, flow_id, &active);
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

    logv(s, "active flow with ID %s\n", flow_id);

    size_t flow_def_json_size = 0;
    // initial call with NULL buffer gets size
    mxl_status = mxlGetFlowDef(mxl_instance, flow_id, NULL, &flow_def_json_size);
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

    mxl_status = mxlGetFlowDef(mxl_instance, flow_id, flow_def_json, &flow_def_json_size);
    if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlGetFlowDef failed\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    logv(s, "good flow definition read for flow ID %s:\n%s\n", flow_id, flow_def_json);

    int json_status = mxl_json_doc_build(s, flow_def_json, &flow_def_doc);
    if (json_status) {
        loge(s, "json flow def parse error %d\n", json_status);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    GET1(id, string1, &flow_def_doc, "id");
    GET1(desc, string1, &flow_def_doc, "description");
    GET1(label, string1, &flow_def_doc, "label");
    GET1(format, string1, &flow_def_doc, "format");
    GET1(media_type, string1, &flow_def_doc, "media_type");

    if (!id.value || av_strcasecmp(id.value, flow_id) != 0) {
        loge(s, "flow id sanity, flow def ID != flow locator ID, %s != %s\n",
                  id.value, flow_id);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) {
        loge(s, "failed to allocate new stream\n");
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    const char *buf = media_type.value;
    unescaped_media_type = av_get_token(&buf, "");
    if (!unescaped_media_type) {
        loge(s, "failed to allocate unescaped_media_type\n");
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }

    SET_META(st, "mxl_id", id.value);
    SET_META(st, "mxl_description", desc.value);
    SET_META(st, "mxl_label", label.value);
    SET_META(st, "mxl_format", format.value);
    SET_META(st, "mxl_media_type", unescaped_media_type);

    if ( strcmp(unescaped_media_type, "audio/float32") == 0 ) {
        flow_type = AUDIO_FLOW;
        int rc = read_audio_header(s, &flow_info, &flow_def_doc, st, &flow_state,
                                   unescaped_media_type);
        if (rc) {
            loge(s, "read audio header error\n");
            exit_status = rc;
            goto finally;
        }
    }
    else if ( strcmp(unescaped_media_type, "video/v210") == 0 ) {
        flow_type = VIDEO_FLOW;
        int rc = read_video_header(s, &flow_info, &flow_def_doc, st, &flow_state,
                                   unescaped_media_type);
        if (rc) {
            loge(s, "read video header error\n");
            exit_status = rc;
            goto finally;
        }
    }
    else {
        loge(s, "unsupported media_type \"%s\"\n", unescaped_media_type);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    // success, transfer state to stream_ctx

    stream_ctx->mxl_flow_reader = flow_reader;
    flow_reader = NULL;

    stream_ctx->flow_type = flow_type;
    flow_type = UNDEFINED_FLOW;

    stream_ctx->flow= flow_state;
    memset(&flow_state, 0, sizeof(flow_state));

    stream_ctx->stream_index = st->index;

    exit_status = 0;

finally:

    mxl_json_doc_release(&flow_def_doc);

    av_free(id.value);
    av_free(desc.value);
    av_free(label.value);
    av_free(format.value);
    av_free(media_type.value);
    av_free(unescaped_media_type);
    av_free(flow_def_json);

    if (flow_reader) {
        av_assert0(mxl_instance);
        mxl_status = mxlReleaseFlowReader(mxl_instance, flow_reader);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowReader error %s\n", mxl_status_to_str(mxl_status));
    }


    return exit_status;
}

/**
 * Examine the configured flows and compute tuning parameters to
 * optimize FFmpeg's streaming performance.
 * 1. audio samples per read set to one video frame, or 1/60s
 * 2. video blocking read set to false if both audio and video flows exit
 */
static void header_init_tuning_params(AVFormatContext *s)
{
    av_assert0(s);

    MXLContext *p = s->priv_data;

    AVStream *vst = NULL;
    AVStream *ast = NULL;
    for (int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !vst)
            vst = st;
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !ast)
            ast = st;
    }

    StreamContext *vctx = NULL;
    StreamContext *actx = NULL;
    for (int i = 0; i < p->loc.nb_flow_ids; i++) {
        if (p->stream_contexts[i].flow_type == VIDEO_FLOW && !vctx)
            vctx = &p->stream_contexts[i];
        else if (p->stream_contexts[i].flow_type == AUDIO_FLOW && !actx)
            actx = &p->stream_contexts[i];
    }

    int audio_samples_per_frame = 0;
    if (ast && vst) {
        av_assert0(actx && vctx);

        mxlRational frame_rate = vctx->flow.video.mxl_grain_rate;
        av_assert0(frame_rate.numerator && frame_rate.denominator);

        mxlRational sample_rate = actx->flow.audio.mxl_sample_rate;
        av_assert0(sample_rate.numerator && sample_rate.denominator == 1);

        audio_samples_per_frame = av_rescale_q(
            sample_rate.numerator,
            (AVRational){frame_rate.denominator, frame_rate.numerator},
            (AVRational){1, 1});
    }
    else if (ast) {
        av_assert0(actx);

        mxlRational sample_rate = actx->flow.audio.mxl_sample_rate;
        av_assert0(sample_rate.numerator && sample_rate.denominator == 1);

        // 16.66 ms = 1/60 second
        audio_samples_per_frame = sample_rate.numerator / 60;
    }

    p->tuning.audio_samples_per_read = audio_samples_per_frame;
    p->tuning.video_blocking_read = !(ast && vst);

    logv(s, "tuned audio_samples_per_read = %d\n", p->tuning.audio_samples_per_read);
    logv(s, "tuned video_blocking_read = %d\n", p->tuning.video_blocking_read);
}

static void release_stream_context(AVFormatContext *s,
                                   mxlInstance mxl_instance, StreamContext* stream_ctx)
{
    if (stream_ctx->mxl_flow_reader) {
        av_assert0(mxl_instance);
        if (MXL_STATUS_OK != mxlReleaseFlowReader(mxl_instance, stream_ctx->mxl_flow_reader))
            logw(s, "mxlReleaseFlowReader error\n");
        stream_ctx->mxl_flow_reader = NULL;
    }

    switch(stream_ctx->flow_type) {
    case VIDEO_FLOW:
        stream_ctx->flow.video = (VideoState){0};
        break;
    case AUDIO_FLOW:
        swr_free(&stream_ctx->flow.audio.swr_context);
        stream_ctx->flow.audio = (AudioState){0};
        break;
    default:
        logw(s, "unknown flow_type\n");
    }
}

/**
 * Reject configurations that have more than one video or more than
 * one audio flow.
 */
static int header_validate_flows(AVFormatContext *s,
                                 int nb_stream_contexts,
                                 const StreamContext* stream_contexts)
{
    av_assert0(stream_contexts);

    int audio_count = 0;
    int video_count = 0;
    for (int i = 0; i < nb_stream_contexts; i++) {
        switch(stream_contexts[i].flow_type) {
        case AUDIO_FLOW:
            audio_count++;
            break;
        case VIDEO_FLOW:
            video_count++;
            break;
        default:
            loge(s, "unknown stream context flow type\n");
            return -1;
        }
    }

    if (audio_count > 1) {
        loge(s, "demuxer does not support multiple audio flows\n");
        return -1;
    }
    else if (video_count > 1) {
        loge(s, "demuxer does not support multiple video flows\n");
        return -1;
    }

    return 0;
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

    mxl_loc loc = {0};
    mxlInstance mxl_instance = NULL;
    StreamContext *stream_contexts = NULL;
    int initialized_stream_contexts_count = 0;

    MXLContext *p = s->priv_data;

    if (!s->url) {
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    int rc = mxl_loc_parse(s, s->url, &loc);
    if (rc) {
        logv(s, "failed to parse resource locator: %s\n", s->url);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    rc = validate_locator(s, &loc);
    if (rc) {
        logv(s, "invalid locator: %s\n", s->url);
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    mxl_instance = mxlCreateInstance(loc.domain_path, NULL);
    if (NULL == mxl_instance) {
        loge(s, "mxlCreateInstance error\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    stream_contexts = av_malloc_array(loc.nb_flow_ids, sizeof(StreamContext));
    if (!stream_contexts) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }
    memset(stream_contexts, 0, loc.nb_flow_ids * sizeof(StreamContext));

    initialized_stream_contexts_count = 0;
    for (int i = 0; i < loc.nb_flow_ids; i++) {
        int flow_rc = read_flow(s, mxl_instance, loc.flow_ids[i], &stream_contexts[i]);
        if (flow_rc) {
            exit_status = flow_rc;
            goto finally;
        }

        initialized_stream_contexts_count++;
    }

    rc = header_validate_flows(s, loc.nb_flow_ids, stream_contexts);
    if (rc) {
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    // success, transfer state
    p->loc = loc;
    memset(&loc, 0, sizeof(loc));

    p->mxl_instance = mxl_instance;
    mxl_instance = NULL;

    p->stream_contexts = stream_contexts;
    stream_contexts = NULL;

    p->at_eof = false;

    header_init_tuning_params(s);

    exit_status = 0;

finally:

    mxl_loc_free(&loc);

    if (stream_contexts) {
        av_assert0(mxl_instance);
        for(int i = 0; i < initialized_stream_contexts_count; i++)
            release_stream_context(s, mxl_instance, &stream_contexts[i]);
        av_free(stream_contexts);
    }

    if (mxl_instance) {
        mxlStatus mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    return exit_status;
}

#undef GET1
#undef GET2
#undef SET_META
#undef POS_INT_SANITY

// zero copy packet release callback
static void mxl_zero_copy_release_cb(void *ctx, uint8_t *data)
{
    MXLContext *p = (MXLContext*)ctx;
    (void)p;
    logv(NULL, "mxl_zero_copy_release_cb %p\n", data);
}

static int read_video_packet(AVFormatContext *s, AVStream *st, AVPacket *pkt,
                             StreamContext *stream_ctx,
                             const mxlFlowInfo *flow_info)
{
    MXLContext  *p = s->priv_data;

    int exit_status = AVERROR_UNKNOWN;

    logd(s, "read video packet\n");

    if (p->max_video_frames > 0 && stream_ctx->flow.video.frame_count >= p->max_video_frames) {
        logv(s, "at max_video_frames (%d)\n", p->max_video_frames);
        return AVERROR_EOF;
    }

    // init grain index
    if (0 == stream_ctx->flow.video.mxl_grain_index) {
        switch (p->grain_index_init) {
        case GRAIN_INDEX_INIT_CURRENT: {
            uint64_t cur_grain_index = mxlGetCurrentIndex(&stream_ctx->flow.video.mxl_grain_rate);
            if (MXL_UNDEFINED_INDEX == cur_grain_index) {
                loge(s, "mxlGetCurrentIndex error MXL_UNDEFINED_INDEX\n");
                exit_status = AVERROR_BUG;
                goto finally;
            }
            stream_ctx->flow.video.mxl_grain_index = cur_grain_index;
            logv(s, "init video grain_index = %"PRIu64", policy: current time\n",
                 stream_ctx->flow.video.mxl_grain_index);
            break;
        }
        case GRAIN_INDEX_INIT_HEAD:
            stream_ctx->flow.video.mxl_grain_index = flow_info->runtime.headIndex;
            logv(s, "init video grain index = %"PRIu64", policy: headIndex\n",
                 stream_ctx->flow.video.mxl_grain_index);
            break;
        case GRAIN_INDEX_INIT_TAIL:
            stream_ctx->flow.video.mxl_grain_index = flow_info->runtime.headIndex -
                flow_info->config.discrete.grainCount + 1;
            logv(s, "init video grain index = %"PRIu64", policy: tail index\n",
                 stream_ctx->flow.video.mxl_grain_index);
            break;
        default:
            loge(s, "unrecognized grain_index_init = \"%d\"\n", p->grain_index_init);
            exit_status = AVERROR_BUG;
            goto finally;
        }

        if (p->reset_on_drop) {
            // presentation timestamp is mxl_start_grain_index relative
            stream_ctx->flow.video.mxl_start_grain_index = stream_ctx->flow.video.mxl_grain_index;
        }
    }

    // init the start grain index
    if (0 == stream_ctx->flow.video.mxl_start_grain_index)
        stream_ctx->flow.video.mxl_start_grain_index = stream_ctx->flow.video.mxl_grain_index;

    mxlGrainInfo grain_info = {0};
    uint8_t *mxl_payload = NULL;
    mxlStatus mxl_status = MXL_ERR_UNKNOWN;

    if ((p->blocking == -1 && p->tuning.video_blocking_read) || p->blocking == 1) {
        logd(s, "blocking video frame read, grain index %"PRIu64"\n",
             stream_ctx->flow.video.mxl_grain_index);
        const AVRational ns_time_base = {1, 1000000000};
        uint64_t timeout_ns = (uint64_t)av_rescale_q(1, st->time_base, ns_time_base);
        mxl_status = mxlFlowReaderGetGrain(stream_ctx->mxl_flow_reader,
                                           stream_ctx->flow.video.mxl_grain_index,
                                           timeout_ns, &grain_info,
                                           &mxl_payload);
    }
    else {
        logd(s, "non-blocking video read, 1 frame at index %"PRIu64"\n",
             stream_ctx->flow.video.mxl_grain_index);
        mxl_status = mxlFlowReaderGetGrainNonBlocking(stream_ctx->mxl_flow_reader,
                                                      stream_ctx->flow.video.mxl_grain_index,
                                                      &grain_info, &mxl_payload);
    }

    if (MXL_ERR_OUT_OF_RANGE_TOO_LATE == mxl_status) {
        switch(p->on_too_late) {
        case ON_TOO_LATE_INCREMENT:
            logv(s, "no video grain, too late with grain index %"PRIu64
                 ", increment index and try again\n",
                 stream_ctx->flow.video.mxl_grain_index);
            stream_ctx->flow.video.mxl_grain_index++;
            exit_status = AVERROR(EAGAIN);
            break;
        case ON_TOO_LATE_RESET:
            logv(s, "no grain, too late with grain index %"PRIu64
                 ", reset index and try again\n",
                 stream_ctx->flow.video.mxl_grain_index);
            exit_status = AVERROR(EAGAIN);
            stream_ctx->flow.video.mxl_grain_index = 0;
            break;
        default:
            loge(s, "unrecognized on_too_late = \"%d\"\n", p->on_too_late);
            exit_status = AVERROR_BUG;
        }
        goto finally;
    }
    else if (MXL_ERR_OUT_OF_RANGE_TOO_EARLY == mxl_status) {
        logv(s, "no grain, too early with grain index %"PRIu64", try again\n",
             stream_ctx->flow.video.mxl_grain_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_ERR_TIMEOUT == mxl_status) {
        logv(s, "no grain, timed out with grain index %"PRIu64", try again\n",
             stream_ctx->flow.video.mxl_grain_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_STATUS_OK != mxl_status) {
        logv(s, "mxlFlowReaderGetGrain error %s (%d)\n",
             mxl_status_to_str(mxl_status), mxl_status);
        exit_status = AVERROR(EIO);
        goto finally;
    }

    if (grain_info.totalSlices != grain_info.validSlices) {
        loge(s, "grain size sanity error, totalSlices != validSlices, %"
             PRIu16" != %"PRIu16"\n",
             grain_info.totalSlices, grain_info.validSlices);
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    size_t grain_size = grain_info.totalSlices * flow_info->config.discrete.sliceSizes[0];
    if (p->zero_copy) {
        AVBufferRef *zcbuf = av_buffer_create(mxl_payload, grain_size,
                                            mxl_zero_copy_release_cb, p, 0);
        if (!zcbuf) {
            loge(s, "av_buffer_create error\n");
            exit_status = AVERROR(ENOMEM);
            goto finally;
        }

        pkt->buf = zcbuf;
        pkt->data = mxl_payload;
        pkt->size = grain_size;
    }
    else
    {
        av_assert1(!pkt->buf);
        av_assert1(!pkt->data);
        int rc = av_new_packet(pkt, grain_size);
        if (rc < 0) {
            loge(s, "av_new_packet error %d\n", rc);
            exit_status = rc;
            goto finally;
        }
        av_assert1(pkt->data);
        memcpy(pkt->data, mxl_payload, grain_size);
    }

    int64_t rel_grain_count =
        stream_ctx->flow.video.mxl_grain_index - stream_ctx->flow.video.mxl_start_grain_index;
    av_assert1(rel_grain_count >= 0);

    pkt->pos = -1;
    pkt->pts = rel_grain_count;
    pkt->dts = rel_grain_count;
    pkt->duration = 1;
    pkt->stream_index = stream_ctx->stream_index;
    pkt->flags |= AV_PKT_FLAG_KEY;

    logd(s, "good frame, grain_index=%"PRIu64", relative_grain_count=%"PRId64
         ", pkt->data=%p, mxl_payload=%p\n",
         stream_ctx->flow.video.mxl_grain_index, rel_grain_count, pkt->data, mxl_payload);

    // all good, advance the grain index
    stream_ctx->flow.video.mxl_grain_index++;
    stream_ctx->flow.video.frame_count++;

    exit_status = 0;

finally:

    if (exit_status < 0)
        av_packet_unref(pkt);

    return exit_status;
}

#define USE_SWR_CONVERT
#ifdef USE_SWR_CONVERT
static int interleave_from_payload_fragments(AVFormatContext *s,
                                             struct SwrContext *swr_context,
                                             const mxlWrappedMultiBufferSlice *payload,
                                             int fragment_idx, float *dst) {

    av_assert1(s && swr_context && payload && dst);

    int exit_status = 0;

    size_t fragment_size = payload->base.fragments[fragment_idx].size;
    av_assert1(fragment_size % sizeof(float) == 0);
    av_assert1(fragment_size / sizeof(float) <= INT_MAX);

    int nb_samples = (int)(fragment_size / sizeof(float));
    av_assert1(nb_samples > 0);
    av_assert1(nb_samples*sizeof(float) == payload->base.fragments[fragment_idx].size);

    float **src_array = (float**)av_malloc_array(payload->count, sizeof(*src_array));
    if (!src_array) {
        logw(s, "interleave_from_payload_fragments malloc failed\n");
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }
    for (int channel = 0; channel < payload->count; ++channel) {
        src_array[channel] = (float*)((uint8_t*)payload->base.fragments[fragment_idx].pointer
                                      + channel*payload->stride);
    }
    // planar to interleaved convert
    uint8_t *dst_array[1] = { (uint8_t *)dst };
    int rc = swr_convert(swr_context,
                         dst_array, nb_samples,
                         (const uint8_t**)src_array, nb_samples);
    if (rc < 0) {
        loge(s, "swr_convert error %d\n", rc);
        exit_status = rc;
        goto finally;
    }
    else if (rc != nb_samples) {
        loge(s, "incomplete fragment %d planar to interleaved conversion "
             "(expected %d samples, actual %d samples)\n", fragment_idx, nb_samples, rc);
        exit_status = AVERROR_BUG;
        goto finally;
    }

    exit_status = nb_samples;

finally:

    av_free(src_array);

    return exit_status;
}

static int mxl_payload_to_ffmpeg_packet(AVFormatContext *s,
                                         struct SwrContext *swr_context,
                                         const mxlWrappedMultiBufferSlice *payload,
                                         AVPacket *pkt)
{
    av_assert1(s && swr_context && payload && pkt);

    int exit_status = 0;

    float* dst0 = (float*)(pkt->data);
    int rc = interleave_from_payload_fragments(s, swr_context, payload, 0, dst0);
    if (rc < 0) {
        exit_status = rc;
        goto finally;
    }

    int nb_samples0 = rc;

    if (payload->base.fragments[1].size > 0) {
        float* dst1 = dst0 + nb_samples0*payload->count;
        int rc = interleave_from_payload_fragments(s, swr_context, payload, 1, dst1);
        if (rc < 0) {
            exit_status = rc;
            goto finally;
        }

        int nb_samples1 = rc;

        av_assert1((nb_samples0 + nb_samples1)*sizeof(float) ==
                   (payload->base.fragments[0].size + payload->base.fragments[1].size));
    }
    else {
        av_assert1((nb_samples0)*sizeof(float) == (payload->base.fragments[0].size));
    }

finally:

    return exit_status;
}
#else
// unoptimized interlacer remains for reference
static int mxl_payload_to_ffmpeg_packet(AVFormatContext *, struct SwrContext *,
                                        const mxlWrappedMultiBufferSlice *payload,
                                        AVPacket *pkt)
{
    av_assert0(payload && pkt);

    for (int channel = 0; channel < payload->count; ++channel) {
        float *dst = (float*)(pkt->data + channel*sizeof(float));

        float *src0 = (float*)((uint8_t*)payload->base.fragments[0].pointer + channel*payload->stride);
        size_t nb_samples0 = payload->base.fragments[0].size/sizeof(float);
        av_assert0(nb_samples0*sizeof(float) == payload->base.fragments[0].size);
        for(int i = 0; i < nb_samples0; i++) {
            *dst = src0[i];
            dst += payload->count;
        }

        if (payload->base.fragments[1].size > 0) {
            float *src1 = (float*)((uint8_t*)payload->base.fragments[1].pointer + channel*payload->stride);
            size_t nb_samples1 = payload->base.fragments[1].size/sizeof(float);
            av_assert0(nb_samples1*sizeof(float) == payload->base.fragments[1].size);
            for(int i = 0; i < nb_samples1; i++) {
                *dst = src1[i];
                dst += payload->count;
            }
        }
    }

    return 0;
}
#endif

static int read_audio_packet(AVFormatContext *s, AVStream *st, AVPacket *pkt,
                             StreamContext *stream_ctx,
                             const mxlFlowInfo *flow_info)
{
    MXLContext  *p = s->priv_data;
    int exit_status = AVERROR_UNKNOWN;

    logd(s, "read audio packet\n");

    // default max_samples_per_read is the MXL batch size hint
    av_assert0(0 < flow_info->config.common.maxCommitBatchSizeHint &&
               flow_info->config.common.maxCommitBatchSizeHint <= INT_MAX);
    int max_samples_per_read = (int)flow_info->config.common.maxCommitBatchSizeHint;

    if (p->tuning.audio_samples_per_read > 0) {
        max_samples_per_read = p->tuning.audio_samples_per_read;
    }

    // override default max_samples_per_read with command line option if set
    if (p->max_audio_samples_per_read > 0) {
        max_samples_per_read = p->max_audio_samples_per_read;
    }

    if (p->max_audio_samples > 0 && stream_ctx->flow.audio.sample_point_count >= p->max_audio_samples) {
        logv(s, "at max_audio_samples (%d)\n", p->max_audio_samples);
        return AVERROR_EOF;
    }

    // init sample index
    if (0 == stream_ctx->flow.audio.mxl_sample_index) {
        switch (p->grain_index_init) {
        case GRAIN_INDEX_INIT_CURRENT: {
            uint64_t cur_sample_index = mxlGetCurrentIndex(&stream_ctx->flow.audio.mxl_sample_rate);
            if (MXL_UNDEFINED_INDEX == cur_sample_index) {
                loge(s, "mxlGetCurrentIndex error MXL_UNDEFINED_INDEX\n");
                exit_status = AVERROR_BUG;
                goto finally;
            }
            stream_ctx->flow.audio.mxl_sample_index = cur_sample_index;
            logv(s, "init audio sample_index = %"PRIu64", policy: current time\n",
                 stream_ctx->flow.audio.mxl_sample_index);
            break;
        }
        case GRAIN_INDEX_INIT_HEAD:
            stream_ctx->flow.audio.mxl_sample_index = flow_info->runtime.headIndex;
            logv(s, "init audio sample index = %"PRIu64", policy: headIndex\n",
                 stream_ctx->flow.audio.mxl_sample_index);
            break;
        case GRAIN_INDEX_INIT_TAIL: {
            // MXL limits readable audio ring buffer length to half
            // the buffer size. The tail calc must handle
            // max_audio_samples smaller than readableBufLen and, in
            // that case, align to the final (possibly partial) read
            // block.
            int readableBufLen = (int)(flow_info->config.continuous.bufferLength / 2);
            if (0 < p->max_audio_samples && p->max_audio_samples < readableBufLen) {
                int rem = p->max_audio_samples % max_samples_per_read;
                uint64_t start_index = flow_info->runtime.headIndex -
                    (uint64_t)(p->max_audio_samples - (rem ? rem : max_samples_per_read));
                av_assert0(flow_info->runtime.headIndex >= start_index);
                stream_ctx->flow.audio.mxl_sample_index = start_index;
            }
            else {
                uint64_t start_index =
                    flow_info->runtime.headIndex - readableBufLen;
                av_assert0(flow_info->runtime.headIndex >= start_index);
                stream_ctx->flow.audio.mxl_sample_index = start_index;
            }
            logv(s, "init audio sample index = %"PRIu64", policy: tail index\n",
                 stream_ctx->flow.audio.mxl_sample_index);
            break;
        }
        default:
            loge(s, "unrecognized grain_index_init = \"%d\"\n", p->grain_index_init);
            exit_status = AVERROR_BUG;
            goto finally;
        }

        if (p->reset_on_drop) {
            // presentation timestamp is mxl_start_grain_index relative
            stream_ctx->flow.audio.mxl_start_sample_index = stream_ctx->flow.audio.mxl_sample_index;
        }
    }

    // init the start grain index
    if (0 == stream_ctx->flow.audio.mxl_start_sample_index)
        stream_ctx->flow.audio.mxl_start_sample_index = stream_ctx->flow.audio.mxl_sample_index;

    int samples_this_read = max_samples_per_read;
    if (p->max_audio_samples > 0) {
        samples_this_read = p->max_audio_samples - stream_ctx->flow.audio.sample_point_count;
        if (samples_this_read > max_samples_per_read)
            samples_this_read = max_samples_per_read;
    }

    const AVRational ns_time_base = {1, 1000000000};
    uint64_t timeout_ns = (uint64_t)av_rescale_q(max_samples_per_read, st->time_base, ns_time_base);
    mxlWrappedMultiBufferSlice payload = {0};
    logd(s, "non-blocking audio read, %d samples at index %"PRIu64"\n",
         samples_this_read, stream_ctx->flow.audio.mxl_sample_index);
    mxlStatus mxl_status = mxlFlowReaderGetSamples(
        stream_ctx->mxl_flow_reader, stream_ctx->flow.audio.mxl_sample_index,
        (size_t)samples_this_read, timeout_ns, &payload);

    if (MXL_ERR_OUT_OF_RANGE_TOO_LATE == mxl_status) {

        static int64_t last_log_time;
        int64_t now = av_gettime_relative();
        if (!last_log_time || now - last_log_time >= 1000000) {
            last_log_time = now;
            logv(s, "audio too late\n");
        }

        switch(p->on_too_late) {
        case ON_TOO_LATE_INCREMENT:
            logd(s, "headIndex = %"PRIu64" vs sample_indx = %"PRIu64" diff = %d\n",
                 flow_info->runtime.headIndex, stream_ctx->flow.audio.mxl_sample_index,
                 flow_info->runtime.headIndex - stream_ctx->flow.audio.mxl_sample_index);
            logd(s, "no audio samples, too late with sample index %"PRIu64
                 ", increment index and try again\n",
                 stream_ctx->flow.audio.mxl_sample_index);
            stream_ctx->flow.audio.mxl_sample_index += max_samples_per_read;
            exit_status = AVERROR(EAGAIN);
            break;
        case ON_TOO_LATE_RESET:
            logd(s, "no audio samples, too late with sample index %"PRIu64
                 ", reset index and try again\n",
                 stream_ctx->flow.audio.mxl_sample_index);
            exit_status = AVERROR(EAGAIN);
            stream_ctx->flow.audio.mxl_sample_index = 0;
            break;
        default:
            loge(s, "unrecognized on_too_late = \"%d\"\n", p->on_too_late);
            exit_status = AVERROR_BUG;
        }
        goto finally;
    }
    else if (MXL_ERR_OUT_OF_RANGE_TOO_EARLY == mxl_status) {
        logv(s, "no audio samples, too early with sample index %"PRIu64", try again\n",
             stream_ctx->flow.audio.mxl_sample_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_ERR_TIMEOUT == mxl_status) {
        logv(s, "no audio samples, timed out with sample index %"PRIu64", try again\n",
             stream_ctx->flow.audio.mxl_sample_index);
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    else if (MXL_STATUS_OK != mxl_status) {
        logv(s, "mxlFlowReaderGetSamples error %s (%d)\n",
             mxl_status_to_str(mxl_status), mxl_status);
        exit_status = AVERROR(EIO);
        goto finally;
    }

    int payload_len_bytes = samples_this_read * payload.count * sizeof(float);

    // payload total length invariant
    av_assert0((size_t)payload_len_bytes ==
               payload.count*(payload.base.fragments[0].size + payload.base.fragments[1].size));
    av_assert1(payload_len_bytes <= INT_MAX);
    av_assert1(!pkt->buf);
    int rc = av_new_packet(pkt, payload_len_bytes);
    if (rc < 0) {
        loge(s, "av_new_packet error %d\n", rc);
        exit_status = rc;
        goto finally;
    }
    av_assert1(pkt->buf);
    av_assert1(pkt->size == payload_len_bytes);

    // stride and bufferLength relationship invariant
    av_assert0(payload.stride == flow_info->config.continuous.bufferLength * sizeof(float));

    // interleave planar source buffers into pkt->data
    rc = mxl_payload_to_ffmpeg_packet(s, stream_ctx->flow.audio.swr_context, &payload, pkt);
    if (rc < 0) {
        loge(s, "audio payload to packet conversion failed\n");
        exit_status = rc;
        goto finally;
    }

    int64_t rel_sample_count =
        stream_ctx->flow.audio.mxl_sample_index - stream_ctx->flow.audio.mxl_start_sample_index;
    av_assert1(rel_sample_count >= 0);

    pkt->pts      = rel_sample_count;
    pkt->dts      = rel_sample_count;
    pkt->duration = samples_this_read;
    pkt->stream_index = stream_ctx->stream_index;

    logd(s, "good audio block, sample_index=%"PRIu64", relative_sample_count=%"PRId64
         ", samples_this_read = %d, pkt->data=%p mxl_payload_0=%p, mxl_payload_1=%p\n",
         stream_ctx->flow.audio.mxl_sample_index, rel_sample_count, samples_this_read, pkt->data,
         payload.base.fragments[0].pointer,
         payload.base.fragments[1].size > 0 ? payload.base.fragments[1].pointer : NULL);

    // all good, advance the sample index
    stream_ctx->flow.audio.mxl_sample_index += samples_this_read;
    stream_ctx->flow.audio.sample_point_count += samples_this_read;

    exit_status = 0;

finally:

    if (exit_status < 0)
        av_packet_unref(pkt);

    return exit_status;
}

static int mxl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    av_assert0(s && s->priv_data);

    MXLContext  *p = s->priv_data;

    int stream_ctx_idx = p->next_stream_context_idx;
    p->next_stream_context_idx =
        (p->next_stream_context_idx + 1) % p->loc.nb_flow_ids;

    av_assert0(stream_ctx_idx < p->loc.nb_flow_ids);

    // early bail-out to avoid flooding the log
    if (p->at_eof) {
        logv(s, "at eof\n");
        return AVERROR_EOF;
    }

    StreamContext* stream_ctx = &p->stream_contexts[stream_ctx_idx];

    av_assert0(stream_ctx->stream_index < s->nb_streams);
    AVStream *st = s->streams[stream_ctx->stream_index];

    int exit_status = AVERROR_UNKNOWN;
    bool active = false;

    mxlStatus mxl_status = mxlIsFlowActive(
        p->mxl_instance, p->loc.flow_ids[stream_ctx_idx], &active);
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

    mxlFlowInfo flow_info = {0};
    mxl_status = mxlFlowReaderGetInfo(stream_ctx->mxl_flow_reader, &flow_info);
    if (mxl_status != MXL_STATUS_OK) {
        loge(s, "mxlFlowReaderGetInfo failed\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    switch (stream_ctx->flow_type) {
    case VIDEO_FLOW: {
        int rc = read_video_packet(s, st, pkt, stream_ctx, &flow_info);
        if (rc) {
            exit_status = rc;
            goto finally;
        }
        break;
    }
    case AUDIO_FLOW: {
        int rc = read_audio_packet(s, st, pkt, stream_ctx, &flow_info);
        if (rc) {
            exit_status = rc;
            goto finally;
        }
        break;
    }
    default:
        loge(s, "unknown flow type\n");
        exit_status = AVERROR_BUG;
        goto finally;
    }

    exit_status = 0;

finally:

    return exit_status;
}

/**
 * Free or release all allocated resources.
 *
 *   - 0                   - success
 */
static int mxl_read_close(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    MXLContext *p = s->priv_data;

    for (int i = 0; i < p->loc.nb_flow_ids; i++)
        release_stream_context(s, p->mxl_instance, &p->stream_contexts[i]);
    av_free(p->stream_contexts);
    p->stream_contexts = NULL;

    if (p->mxl_instance) {
        if (MXL_STATUS_OK != mxlDestroyInstance(p->mxl_instance))
            logw(s, "mxlDestroyInstance error\n");
        p->mxl_instance = NULL;
    }

    mxl_loc_free(&p->loc);

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
