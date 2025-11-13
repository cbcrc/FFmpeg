/*
 * MXL muxer for Media eXchange Layer flows
 *
 * AVFMT_NOFILE muxer; requires "-flow_id id /path/to/domain"
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
#include "mxl_flow_def.h"
#include "mux.h"
#include "avformat.h"
#include "avio.h"
#include "internal.h"

#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/file.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#include <mxl/mxl.h>
#include <mxl/flow.h>
#include <mxl/time.h>

#include <inttypes.h>
#include <stdbool.h>

typedef struct MXLContext {
    /* AVOptions: user-configurable parameters */
    const AVClass *class;

    // domain_path from input url
    const char *domain_path;

    // options
    const char *flow_id;
    const char *teardown_sync_file;
    int teardown_sync_timeout;

    // interface objects
    mxlInstance mxl_instance;
    mxlFlowInfo *mxl_flow_info;
    mxlFlowWriter mxl_flow_writer;

    // state
    FlowDefParams flow_def_params;
    mxlRational mxl_grain_rate;
    uint64_t mxl_start_grain_index;
    uint64_t mxl_grain_index;

} MXLContext;


#define OFFSET(x) offsetof(MXLContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption mxl_options[] = {
    {
        .name        = "flow_id",
        .help        = "MXL flow ID",
        .offset      = OFFSET(flow_id),
        .type        = AV_OPT_TYPE_STRING,
        .default_val = { .str = NULL },
        .flags       = FLAGS,
    },
    {
        .name        = "teardown_sync_file",
        .help        = "wait for sentinel file to appear before destroying flow",
        .offset      = OFFSET(teardown_sync_file),
        .type        = AV_OPT_TYPE_STRING,
        .flags       = FLAGS
    },
    {
        .name        = "teardown_sync_timeout",
        .help        = "maximum wait time in ms for teardown sync file",
        .offset      = OFFSET(teardown_sync_timeout),
        .type        = AV_OPT_TYPE_INT,
        .default_val = { .i64 = 2000 },
        .min         = 0,
        .max         = 10000,
        .flags       = FLAGS
    },
    { NULL },
};
#undef OFFSET
#undef FLAGS

/* FFmpeg purpose: AVClass ties AVOptions to this private context for logging
 * and option parsing; item_name delegates to av_default_item_name. */
static const AVClass mxl_muxer_class = {
    .class_name = "mxl muxer",
    .item_name  = av_default_item_name,
    .option     = mxl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int mxl_write_header(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    MXLContext *p = s->priv_data;
    int exit_status = AVERROR_UNKNOWN;

    // inputs
    const char *domain_path = s->url;
    const char *flow_id = p->flow_id;


    // state
    mxlInstance mxl_instance = NULL;
    mxlFlowWriter flow_writer = NULL;
    mxlFlowInfo *flow_info = NULL;
    bool flow_created = false;
    uint8_t *flow_def_json = NULL;

    if (s->nb_streams != 1) {
        loge(s, "exactly one stream is supported\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    AVStream *st = s->streams[0];
    av_assert0(st);
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        st->codecpar->codec_id != AV_CODEC_ID_V210) {
        loge(s, "unsupport stream\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }

    // v210 requires even frame width
    AVCodecParameters *par = st->codecpar;
    if (par->width & 1) {
        loge(s, "v210 width must be even, got %d\n", par->width);
        return AVERROR_INVALIDDATA;
    }

    if (!domain_path) {
        loge(s, "domain not set\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }
    logv(s, "domain: \"%s\"\n", domain_path);

    flow_info = av_malloc(sizeof(*flow_info));
    if (!flow_info) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }
    *flow_info = (mxlFlowInfo){0};

    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    p->mxl_grain_rate = (mxlRational){fr.num, fr.den};
    p->flow_def_params = (FlowDefParams){
        .id = flow_id,
        .grain_rate_num = fr.num,
        .grain_rate_den = fr.den,
        .frame_width = par->width,
        .frame_height = par->height,
        .y_width = par->width,
        .y_height = par->height,
        .y_bit_depth = 10,
        .cb_width = par->width/2,
        .cb_height = par->height,
        .cb_bit_depth = 10,
        .cr_width = par->width/2,
        .cr_height = par->height,
        .cr_bit_depth = 10
    };

    flow_def_json = make_flow_def(&p->flow_def_params);

    mxl_instance = mxlCreateInstance(domain_path, NULL);
    if (NULL == mxl_instance) {
        logv(s, "mxlCreateInstance error\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    mxlStatus mxl_status = MXL_ERR_UNKNOWN;
    mxl_status = mxlCreateFlow(mxl_instance, flow_def_json, NULL, flow_info);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlCreateFlow error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }
    flow_created = true;

    mxl_status = mxlCreateFlowWriter(mxl_instance, flow_id, NULL, &flow_writer);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlCreateFlowWriter error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    // all good, transfer state to context
    p->domain_path = domain_path;
    domain_path = NULL;

    p->mxl_instance = mxl_instance;
    mxl_instance = NULL;

    p->mxl_flow_info = flow_info;
    flow_info = NULL;

    p->mxl_flow_writer = flow_writer;
    flow_writer = NULL;

    // flow is now the responsibility of mxl_write_trailer to destroy
    flow_created = false;

    exit_status = 0;

finally:

    if (flow_writer) {
        av_assert0(mxl_instance);
        mxl_status = mxlReleaseFlowWriter(mxl_instance, flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowWriter error %s\n", mxl_status_to_str(mxl_status));
    }

    if (flow_created) {
        av_assert0(mxl_instance);
        mxl_status = mxlDestroyFlow(mxl_instance, flow_id);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyFlow error %s\n", mxl_status_to_str(mxl_status));
    }

    if (mxl_instance) {
        mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    if (flow_info)
        av_free(flow_info);

    if (flow_def_json)
        av_free(flow_def_json);

    return exit_status;
}

static int mxl_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    av_assert0(s && s->priv_data);
    av_assert0(s->nb_streams == 1);
    av_assert0(pkt->stream_index == 0);

    MXLContext *p = s->priv_data;
    int exit_status = AVERROR_UNKNOWN;

    // lazy init start time and current grain index
    if (0 == p->mxl_start_grain_index) {
        uint64_t grain_index = mxlGetCurrentIndex(&p->mxl_grain_rate);
        av_assert0(MXL_UNDEFINED_INDEX != grain_index);
        p->mxl_start_grain_index = grain_index;
        p->mxl_grain_index = grain_index;
        logv(s, "start grain index = %" PRIu64 "\n", grain_index);
    }

    mxlStatus mxl_status = MXL_ERR_UNKNOWN;
    mxlGrainInfo grain_info = {0};
    bool grain_open = false;
    uint8_t *grain_payload = NULL;

    logv(s, "open grain for grain_index = %" PRIu64 "\n", p->mxl_grain_index);
    mxl_status = mxlFlowWriterOpenGrain(p->mxl_flow_writer, p->mxl_grain_index,
                                        &grain_info, &grain_payload);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterOpenGrain error %s\n", mxl_status_to_str(mxl_status));
        // TODO - determine best error. It may also be EIO.
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }

    grain_open = true;

    logv(s, "packet size = %d, grain_info grainSize = %" PRIu32 ", committedSize = %" PRIu32 "\n",
         pkt->size, grain_info.grainSize, grain_info.commitedSize);
    if (pkt->size > grain_info.grainSize) {
        loge(s, "packet size exceeds grain size (%d > %" PRIu32 ")\n",
             pkt->size, grain_info.grainSize);
        exit_status = AVERROR_BUG;
        goto finally;
    }

    memcpy(grain_payload, pkt->data, pkt->size);
    grain_info.commitedSize = pkt->size;
    if (pkt->size < grain_info.grainSize)
        logw(s, "grain under committed (pkt->size < grain_info.grainSize, %d < %" PRIu32 ")\n",
             pkt->size, grain_info.grainSize);

    mxl_status = mxlFlowWriterCommitGrain(p->mxl_flow_writer, &grain_info);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterCommitGrain error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    grain_open = false;
    p->mxl_grain_index++;

    exit_status = 0;

finally:

    if (grain_open) {
        mxl_status = mxlFlowWriterCancelGrain(p->mxl_flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlFlowWriterCancelGrain error %s\n", mxl_status_to_str(mxl_status));
    }

    return exit_status;
}

typedef enum {
    SENTINEL_APPEARED = 0,
    SENTINEL_TIMEOUT = -1
} SentinelStatus;

static int wait_for_sentinel(AVFormatContext *s, const char *path, int timeout_ms)
{
    const int poll_ms = 1000;
    const int poll_us = 1000*poll_ms;
    const int64_t start = av_gettime_relative();

    int status = 0;
    while ((status = avio_check(path, AVIO_FLAG_READ)) < 0) {
        if (av_gettime_relative() - start > (int64_t)timeout_ms * 1000)
            return SENTINEL_TIMEOUT;
        logv(s, "waiting for sentinel: %s, s = %d\n", path, status);
        av_usleep(poll_us);
    }
    return SENTINEL_APPEARED;
}

static const char *sentinel_status_to_str(SentinelStatus st)
{
    switch (st) {
    case SENTINEL_APPEARED:
        return "SENTINEL_APPEARED";
    case SENTINEL_TIMEOUT:
        return "SENTINEL_TIMEOUT";
    default:
        return "UNKOWN";
    }
}

/* ------------------------------------------------------------------------- */
/* FFmpeg purpose: write_trailer() finalizes the container, signals EOF to
 * consumers, flushes and releases all resources (munmap/close/free/etc.). */
static int mxl_write_trailer(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    MXLContext *p = s->priv_data;

    mxlStatus mxl_status = MXL_ERR_UNKNOWN;

    if (p->teardown_sync_file) {
        av_assert0(p->teardown_sync_timeout > 0);
        SentinelStatus sentinel_status =
            wait_for_sentinel(s, p->teardown_sync_file, p->teardown_sync_timeout);
        logv(s, "sentinel status = %s\n", sentinel_status_to_str(sentinel_status));
    }

    if (p->mxl_flow_writer) {
        av_assert0(p->mxl_instance);
        mxl_status = mxlReleaseFlowWriter(p->mxl_instance, p->mxl_flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowWriter error %s\n", mxl_status_to_str(mxl_status));
        p->mxl_flow_writer = NULL;
    }

    if (p->mxl_flow_info) {
        av_assert0(p->mxl_instance && p->flow_def_params.id);
        mxl_status = mxlDestroyFlow(p->mxl_instance, p->flow_def_params.id);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyFlow error %s\n", mxl_status_to_str(mxl_status));
        av_free(p->mxl_flow_info);
        p->mxl_flow_info = NULL;
    }

    if (p->mxl_instance) {
        mxl_status = mxlDestroyInstance(p->mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
        p->mxl_instance = NULL;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* FFmpeg purpose: query_codec() optionally restricts supported codecs for
 * this muxer (e.g., only AV_CODEC_ID_V210 initially). Return 1 if supported. */
static int mxl_query_codec(enum AVCodecID id, int std_compliance)
{
    switch (id) {
    case AV_CODEC_ID_V210:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* FFmpeg purpose: AVOutputFormat describes the muxer to libavformat—
 * names, default codecs, capability flags, and the function vtable. */
const FFOutputFormat ff_mxl_muxer = {
    // AVOutputFormat (public-facing fields)
    .p.name       = "mxl",
    .p.long_name  = NULL_IF_CONFIG_SMALL(MXL_LONG_NAME),
    .p.extensions = MXL_FLOW_EXT,
    .p.priv_class = &mxl_muxer_class,
    .p.flags      = AVFMT_NOFILE,

    .p.audio_codec = AV_CODEC_ID_NONE,
    .p.video_codec = AV_CODEC_ID_V210,

    .priv_data_size = sizeof(MXLContext),
    .write_header   = mxl_write_header,
    .write_packet   = mxl_write_packet,
    .write_trailer  = mxl_write_trailer,
    .query_codec    = mxl_query_codec,
};
