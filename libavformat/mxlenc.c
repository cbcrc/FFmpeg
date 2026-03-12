/*
 * MXL muxer for Media eXchange Layer flows
 *
 * AVFMT_NOFILE muxer; requires "-{audio|video}_flow_id id /path/to/domain"
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

#include "mxl_common.h"
#include "mxl_status.h"
#include "mxl_flow_def.h"
#include "mux.h"
#include "avformat.h"
#include "avio.h"
#include "internal.h"

#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
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

typedef struct FlowState {
    int stream_index;
    const char *flow_id;
    const char *detail;
    mxlFlowConfigInfo *mxl_flow_config_info;
    mxlFlowWriter mxl_flow_writer;
    mxlRational mxl_grain_rate;
    uint64_t mxl_start_index;
    uint64_t mxl_cur_index;
} FlowState;

typedef struct VideoStreamState {
    VideoFlowDefParams flow_def_params;
    FlowState flow;
} VideoStreamState;

typedef struct AudioStreamState {
    AudioFlowDefParams flow_def_params;
    FlowState flow;
    struct SwrContext *swr_context;
} AudioStreamState;

typedef struct State {
    mxlInstance mxl_instance;

    bool has_video_stream;
    VideoStreamState video;

    bool has_audio_stream;
    AudioStreamState audio;
} State;

typedef struct MXLContext {

    const AVClass *class;

    // domain_path from input url
    const char *domain_path;

    // options
    const char *audio_flow_id;
    const char *video_flow_id;
    const char *teardown_sync_file;
    int teardown_sync_timeout;

    // state
    State state;
} MXLContext;


#define OFFSET(x) offsetof(MXLContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption mxl_options[] = {
    {
        .name        = "audio_flow_id",
        .help        = "MXL audio flow ID",
        .offset      = OFFSET(audio_flow_id),
        .type        = AV_OPT_TYPE_STRING,
        .default_val = { .str = NULL },
        .flags       = FLAGS,
    },
    {
        .name        = "video_flow_id",
        .help        = "MXL video flow ID",
        .offset      = OFFSET(video_flow_id),
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
        .min         = 1,
        .max         = 10000,
        .flags       = FLAGS
    },
    { NULL },
};
#undef OFFSET
#undef FLAGS

static const AVClass mxl_muxer_class = {
    .class_name = "mxl muxer",
    .item_name  = av_default_item_name,
    .option     = mxl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int validate_streams(AVFormatContext *s)
{
    MXLContext *p = s->priv_data;
    int exit_status = AVERROR_UNKNOWN;

    p->state.has_video_stream = false;
    p->state.video.flow.detail = "video";
    p->state.video.flow.stream_index = -1;

    p->state.has_audio_stream = false;
    p->state.audio.flow.detail = "audio";
    p->state.audio.flow.stream_index = -1;

    for(int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        switch(st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (p->video_flow_id) {
                if (st->codecpar->codec_id != AV_CODEC_ID_V210) {
                    loge(s, "unsupported video stream, codec must be v210\n");
                    exit_status = AVERROR_INVALIDDATA;
                    goto finally;
                }
                // v210 requires even frame width
                if (st->codecpar->width & 1) {
                    loge(s, "v210 width must be even, got %d\n", st->codecpar->width);
                    exit_status = AVERROR_INVALIDDATA;
                    goto finally;
                }
                p->state.has_video_stream = true;
                p->state.video.flow.flow_id = p->video_flow_id;
                p->state.video.flow.stream_index = i;
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (p->audio_flow_id) {
                if (st->codecpar->codec_id != AV_CODEC_ID_PCM_F32LE) {
                    loge(s, "unsupported audio stream, codec must be f32le\n");
                    exit_status = AVERROR_INVALIDDATA;
                    goto finally;
                }
                p->state.has_audio_stream = true;
                p->state.audio.flow.flow_id = p->audio_flow_id;
                p->state.audio.flow.stream_index = i;
            }
            break;
        default:
            logv(s, "ignoring unsupported stream %d (%s/%s)\n",
                 i,
                 av_get_media_type_string(st->codecpar->codec_type),
                 avcodec_get_name(st->codecpar->codec_id));
            break;
        }
    }

    if (!(p->state.has_video_stream || p->state.has_audio_stream)) {
        loge(s, "no audio or video stream detected\n");
        exit_status = AVERROR_INVALIDDATA;
        goto finally;
    }

    exit_status = 0;

finally:

    return exit_status;
}

typedef struct FlowDefFactoryResult {
    // borrowed, immutable, must not be freed
    const char * const flow_id;

    // owned by factory caller, mutable, must be freed by caller
    char *flow_def_json;

    // borrowed, mutable, must not be freed or reassigned
    FlowState * const flow_state;
} FlowDefFactoryResult;

typedef FlowDefFactoryResult (*FlowDefFactory)(AVStream *st, MXLContext *p);

static int create_mxl_flow(AVFormatContext *s, AVStream *st,
                           mxlInstance mxl_instance,
                           FlowDefFactory flow_def_factory)
{
    av_assert0(s && st && mxl_instance && flow_def_factory);

    MXLContext *p = s->priv_data;

    mxlFlowWriter flow_writer = NULL;
    mxlFlowConfigInfo *flow_config_info = NULL;

    FlowDefFactoryResult factory_res = flow_def_factory(st, p);
    av_assert0(factory_res.flow_id && factory_res.flow_def_json && factory_res.flow_state);
    logv(s, "%s flow definition:\n%s\n", factory_res.flow_state->detail, factory_res.flow_def_json);

    int exit_status = AVERROR_UNKNOWN;

    flow_config_info = av_malloc(sizeof(*flow_config_info));
    if (!flow_config_info) {
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }
    *flow_config_info = (mxlFlowConfigInfo){0};

    bool new_flow_created = false;
    mxlStatus mxl_status = mxlCreateFlowWriter(
        mxl_instance, factory_res.flow_def_json,
        NULL, &flow_writer, flow_config_info, &new_flow_created);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlCreateFlowWriter error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }
    logv(s, "mxlCreateFlowWriter opened %s flow\n",
         new_flow_created ? "a new" : "an existing");

    // all good, transfer state to context
    factory_res.flow_state->mxl_flow_config_info = flow_config_info;
    flow_config_info = NULL;

    factory_res.flow_state->mxl_flow_writer = flow_writer;
    flow_writer = NULL;

    exit_status = 0;

finally:

    if (flow_writer) {
        mxl_status = mxlReleaseFlowWriter(mxl_instance, flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowWriter error %s\n", mxl_status_to_str(mxl_status));
        flow_writer = NULL;
    }

    av_free(flow_config_info);
    av_free(factory_res.flow_def_json);

    return exit_status;
}

static FlowDefFactoryResult video_flow_def_factory(AVStream *st, MXLContext* p)
{
    av_assert0(st && p);

    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    p->state.video.flow.mxl_grain_rate = (mxlRational){fr.num, fr.den};
    AVCodecParameters *par = st->codecpar;
    p->state.video.flow_def_params = (VideoFlowDefParams) {
        .id = p->state.video.flow.flow_id,
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

    return (FlowDefFactoryResult){
        .flow_id = p->state.video.flow.flow_id,
        .flow_def_json = make_video_flow_def(&p->state.video.flow_def_params),
        .flow_state = &p->state.video.flow};
}

static int create_video_flow(AVFormatContext *s, AVStream *st,
                             mxlInstance mxl_instance)
{
    return create_mxl_flow(s, st, mxl_instance, video_flow_def_factory);
}

static FlowDefFactoryResult audio_flow_def_factory(AVStream *st, MXLContext* p)
{
    av_assert0(st && p);

    AVCodecParameters *par = st->codecpar;
    p->state.audio.flow.mxl_grain_rate = (mxlRational){par->sample_rate, 1};
    p->state.audio.flow_def_params = (AudioFlowDefParams) {
        .id = p->state.audio.flow.flow_id,
        .sample_rate = par->sample_rate,
        .channel_count = par->ch_layout.nb_channels
    };

    return (FlowDefFactoryResult){
        .flow_id = p->state.audio.flow.flow_id,
        .flow_def_json = make_audio_flow_def(&p->state.audio.flow_def_params),
        .flow_state = &p->state.audio.flow};
}

static int create_audio_flow(AVFormatContext *s, AVStream *st,
                             mxlInstance mxl_instance)
{
    return create_mxl_flow(s, st, mxl_instance, audio_flow_def_factory);
}

// create SwrContext for interleaved to planar audio conversion
static int init_swr_context(struct SwrContext **swr_context,
                            int channels, int sample_rate)
{
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout,  channels);
    av_channel_layout_default(&out_layout, channels);

    int rc = swr_alloc_set_opts2(swr_context,
                                 &out_layout, AV_SAMPLE_FMT_FLTP, sample_rate, // planar out
                                 &in_layout,  AV_SAMPLE_FMT_FLT,  sample_rate, // interleaved in
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

static int mxl_write_header(AVFormatContext *s)
{
    av_assert0(s && s->priv_data);

    MXLContext *p = s->priv_data;
    int exit_status = AVERROR_UNKNOWN;

    // inputs
    const char *domain_path = s->url;

    // state
    mxlInstance mxl_instance = NULL;

    if (!domain_path) {
        loge(s, "domain not set\n");
        exit_status = AVERROR(EINVAL);
        goto finally;
    }
    logv(s, "domain: \"%s\"\n", domain_path);

    int rc = validate_streams(s);
    if (rc < 0) {
        loge(s, "stream validation error\n");
        exit_status = rc;
        goto finally;
    }

    // post validation sanity
    av_assert0(p->state.has_video_stream || p->state.has_audio_stream);
    av_assert0(p->state.has_video_stream == !!p->state.video.flow.flow_id);
    av_assert0(p->state.has_audio_stream == !!p->state.audio.flow.flow_id);

    mxl_instance = mxlCreateInstance(domain_path, NULL);
    if (NULL == mxl_instance) {
        logv(s, "mxlCreateInstance error\n");
        exit_status = AVERROR(EIO);
        goto finally;
    }

    if (p->state.has_video_stream) {
        av_assert0(p->state.video.flow.stream_index >= 0 &&
                   p->state.video.flow.flow_id &&
                   p->state.video.flow.detail);
        rc = create_video_flow(s, s->streams[p->state.video.flow.stream_index], mxl_instance);
        if (rc < 0) {
            loge(s, "error configuring mxl video flow\n");
            exit_status = rc;
            goto finally;
        }
    }

    if (p->state.has_audio_stream) {
        av_assert0(p->state.audio.flow.stream_index >= 0 &&
                   p->state.audio.flow.flow_id &&
                   p->state.audio.flow.detail);
        rc = create_audio_flow(s, s->streams[p->state.audio.flow.stream_index], mxl_instance);
        if (rc < 0) {
            loge(s, "error configuring mxl audio flow\n");
            exit_status = rc;
            goto finally;
        }

        rc = init_swr_context(&p->state.audio.swr_context,
                              p->state.audio.flow_def_params.channel_count,
                              p->state.audio.flow_def_params.sample_rate);
        if (rc < 0) {
            loge(s, "init_swr_context failed\n");
            exit_status = rc;
            goto finally;
        }
    }

    // all good, transfer state to context
    p->domain_path = domain_path;
    domain_path = NULL;

    p->state.mxl_instance = mxl_instance;
    mxl_instance = NULL;

    exit_status = 0;

finally:

    if (mxl_instance) {
        mxlStatus mxl_status = mxlDestroyInstance(mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
    }

    return exit_status;

}

static int video_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    int exit_status = AVERROR_UNKNOWN;
    MXLContext *p = s->priv_data;

    if (0 == p->state.video.flow.mxl_start_index) {
        uint64_t cur_index = mxlGetCurrentIndex(&p->state.video.flow.mxl_grain_rate);
        av_assert0(MXL_UNDEFINED_INDEX != cur_index);
        p->state.video.flow.mxl_start_index = cur_index;
        p->state.video.flow.mxl_cur_index = cur_index;
        logv(s, "video start index = %" PRIu64 "\n", cur_index);
    }

    mxlStatus mxl_status = MXL_ERR_UNKNOWN;
    mxlGrainInfo grain_info = {0};
    bool grain_open = false;
    uint8_t *payload = NULL;

    logv(s, "write video grain for index = %" PRIu64 "\n",
         p->state.video.flow.mxl_cur_index);

    mxl_status = mxlFlowWriterOpenGrain(p->state.video.flow.mxl_flow_writer,
                                        p->state.video.flow.mxl_cur_index,
                                        &grain_info, &payload);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterOpenGrain error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }
    grain_open = true;

    size_t grain_size = grain_info.totalSlices *
        p->state.video.flow.mxl_flow_config_info->discrete.sliceSizes[0];

    if (pkt->size > grain_size) {
        loge(s, "packet size exceeds grain size (%d > %zu)\n",
             pkt->size, grain_size);
        exit_status = AVERROR_BUG;
        goto finally;
    }

    memcpy(payload, pkt->data, pkt->size);

    // mark all slices valid
    grain_info.validSlices = grain_info.totalSlices;

    if (pkt->size < grain_info.grainSize)
        logw(s, "grain under committed (pkt->size < grain_info.grainSize, %d < %" PRIu32 ")\n",
             pkt->size, grain_info.grainSize);

    mxl_status = mxlFlowWriterCommitGrain(p->state.video.flow.mxl_flow_writer, &grain_info);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterCommitGrain error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    grain_open = false;
    p->state.video.flow.mxl_cur_index++;

    exit_status = 0;

finally:

    if (grain_open) {
        mxl_status = mxlFlowWriterCancelGrain(p->state.video.flow.mxl_flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlFlowWriterCancelGrain error %s\n", mxl_status_to_str(mxl_status));
        grain_open = false;
    }

    return exit_status;
}

#define USE_SWR_CONVERT
#ifdef USE_SWR_CONVERT
static int deinterleave_to_payload_fragments(AVFormatContext *s,
                                             struct SwrContext *swr_context,
                                             mxlMutableWrappedMultiBufferSlice *payload,
                                             int fragment_idx, const float *src) {

    av_assert1(s && swr_context && payload && src);

    int exit_status = 0;

    size_t nb_samples = payload->base.fragments[fragment_idx].size/sizeof(float);
    av_assert1(nb_samples*sizeof(float) == payload->base.fragments[fragment_idx].size);

    float **dst_array = (float**)av_malloc_array(payload->count, sizeof(*dst_array));
    if (!dst_array) {
        logw(s, "deinterleave_to_payload_fragments malloc failed\n");
        exit_status = AVERROR(ENOMEM);
        goto finally;
    }
    for (int channel = 0; channel < payload->count; ++channel) {
        dst_array[channel] = (float*)((uint8_t*)payload->base.fragments[fragment_idx].pointer
                                      + channel*payload->stride);
    }

    // interleaved to planar convert
    const uint8_t *src_array[1] = { (const uint8_t *)src };
    int rc = swr_convert(swr_context,
                         (uint8_t**)dst_array, nb_samples,
                         src_array, nb_samples);
    if (rc < 0) {
        loge(s, "swr_convert error %d\n", rc);
        exit_status = rc;
        goto finally;
    }
    else if (rc != nb_samples) {
        loge(s, "incomplete fragment %d interleaved to planar conversion "
             "(expected %d samples, actual %d samples)\n", fragment_idx, nb_samples, rc);
        exit_status = AVERROR_BUG;
    }

    exit_status = nb_samples;

finally:

    av_free(dst_array);

    return exit_status;
}


static int ffmpeg_packet_to_mxl_payload(AVFormatContext *s,
                                        struct SwrContext *swr_context,
                                        mxlMutableWrappedMultiBufferSlice *payload,
                                        const AVPacket *pkt)
{
    av_assert0(s && swr_context && payload && pkt);

    int exit_status = 0;

    float *src0 = (float*)(pkt->data);
    int rc = deinterleave_to_payload_fragments(s, swr_context, payload, 0, src0);
    if (rc < 0) {
        exit_status = rc;
        goto finally;
    }

    int nb_samples0 = rc;

    if (payload->base.fragments[1].size > 0) {
        float* src1 = src0 + nb_samples0*payload->count;
        int rc = deinterleave_to_payload_fragments(s, swr_context, payload, 1, src1);
        if (rc < 0) {
            exit_status = rc;
            goto finally;
        }

        int nb_samples1 = rc;

        av_assert1((nb_samples0 + nb_samples1)*sizeof(float) ==
                   (payload->base.fragments[0].size + payload->base.fragments[1].size));
    }
    else {
        av_assert1(nb_samples0*sizeof(float) == payload->base.fragments[0].size);
    }

finally:

    return exit_status;
}
#else
// unoptimized deinterlacer, retained for reference
static int ffmpeg_packet_to_mxl_payload(AVFormatContext *s,
                                        struct SwrContext *swr_context,
                                        mxlMutableWrappedMultiBufferSlice *payload,
                                        const AVPacket *pkt)
{
    av_assert0(payload && pkt);

    for (int channel = 0; channel < payload->count; ++channel) {
        float *src = (float*)(pkt->data + channel*sizeof(float));

        float *dst0 = (float*)((uint8_t*)payload->base.fragments[0].pointer + channel*payload->stride);
        size_t nb_samples0 = payload->base.fragments[0].size/sizeof(float);
        av_assert0(nb_samples0*sizeof(float) == payload->base.fragments[0].size);
        for(int i = 0; i < nb_samples0; i++) {
            dst0[i] = *src;
            src += payload->count;
        }

        if (payload->base.fragments[1].size > 0) {
            float *dst1 = (float*)((uint8_t*)payload->base.fragments[1].pointer + channel*payload->stride);
            size_t nb_samples1 = payload->base.fragments[1].size/sizeof(float);
            av_assert0(nb_samples1*sizeof(float) == payload->base.fragments[1].size);
            for(int i = 0; i < nb_samples1; i++) {
                dst1[i] = *src;
                src += payload->count;
            }
        }
    }

    return 0;
}
#endif

static int audio_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    int exit_status = AVERROR_UNKNOWN;
    MXLContext *p = s->priv_data;

    av_assert0(s->streams[pkt->stream_index]->codecpar->ch_layout.nb_channels ==
               p->state.audio.flow_def_params.channel_count);
    av_assert0(pkt->duration <=
               pkt->size / p->state.audio.flow_def_params.channel_count / sizeof(float));

    if (0 == p->state.audio.flow.mxl_start_index) {
        uint64_t cur_index = mxlGetCurrentIndex(&p->state.audio.flow.mxl_grain_rate);
        av_assert0(MXL_UNDEFINED_INDEX != cur_index);
        p->state.audio.flow.mxl_start_index = cur_index;
        p->state.audio.flow.mxl_cur_index = cur_index;
        logv(s, "audio start index = %" PRIu64 "\n", cur_index);
    }

    mxlStatus mxl_status = MXL_ERR_UNKNOWN;
    bool samples_open = false;
    size_t sample_count = pkt->duration;

    logv(s, "write audio samples for index = %" PRIu64 ", sample_count = %zu\n",
         p->state.audio.flow.mxl_cur_index, sample_count);

    mxlMutableWrappedMultiBufferSlice payload = (mxlMutableWrappedMultiBufferSlice){0};
    mxl_status = mxlFlowWriterOpenSamples(p->state.audio.flow.mxl_flow_writer,
                                          p->state.audio.flow.mxl_cur_index,
                                          sample_count, &payload);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterOpenSamples error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EAGAIN);
        goto finally;
    }

    samples_open = true;

    int rc = ffmpeg_packet_to_mxl_payload(s, p->state.audio.swr_context, &payload, pkt);
    if (rc < 0) {
        loge(s, "audio packet to MXL payload conversion failed\n");
        exit_status = rc;
        goto finally;
    }

    mxl_status = mxlFlowWriterCommitSamples(p->state.audio.flow.mxl_flow_writer);
    if (MXL_STATUS_OK != mxl_status) {
        loge(s, "mxlFlowWriterCommitSamples error %s\n", mxl_status_to_str(mxl_status));
        exit_status = AVERROR(EIO);
        goto finally;
    }

    samples_open = false;
    p->state.audio.flow.mxl_cur_index += sample_count;

    exit_status = 0;

finally:

    if (samples_open) {
        mxl_status = mxlFlowWriterCancelSamples(p->state.audio.flow.mxl_flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlFlowWriterCancelSamples error %s\n", mxl_status_to_str(mxl_status));
        samples_open = false;
    }

    return exit_status;
}

static int mxl_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    av_assert0(s && pkt);

    MXLContext *p = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];
    av_assert0(st);

    int exit_status = AVERROR_UNKNOWN;

    switch(st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        av_assert0(p->state.has_video_stream);
        exit_status = video_write_packet(s, pkt);
        if (exit_status < 0) {
            loge(s, "video_write_packet error\n");
            goto finally;
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        av_assert0(p->state.has_audio_stream);
        exit_status = audio_write_packet(s, pkt);
        if (exit_status < 0) {
            loge(s, "audio_write_packet error\n");
            goto finally;
        }
        break;
    default:
        loge(s, "unexpected media type\n");
        exit_status = AVERROR_BUG;
        goto finally;
    }

finally:

    return exit_status;
}

typedef enum {
    SENTINEL_APPEARED = 0,
    SENTINEL_TIMEOUT = -1
} SentinelStatus;

static int wait_for_sentinel(AVFormatContext *s, const char *path, int timeout_ms)
{
    const int poll_ms = 100;
    const int poll_us = 1000*poll_ms;
    const int64_t start = av_gettime_relative();
    const int64_t timeout_us = 1000*timeout_ms;
    int status = 0;

    while ((status = avio_check(path, AVIO_FLAG_READ)) < 0) {
        if (av_gettime_relative() - start > timeout_us)
            return SENTINEL_TIMEOUT;
        logv(s, "waiting for sentinel: %s\n", path);
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
        return "UNKNOWN";
    }
}

static void flow_teardown(AVFormatContext *s, FlowState *flow_state)
{
    av_assert0(s && flow_state);

    MXLContext *p = s->priv_data;

    if (flow_state->mxl_flow_writer) {
        av_assert0(p->state.mxl_instance);
        mxlStatus mxl_status = mxlReleaseFlowWriter(p->state.mxl_instance,
                                                    flow_state->mxl_flow_writer);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlReleaseFlowWriter %s error %s\n",
                 flow_state->detail, mxl_status_to_str(mxl_status));
        flow_state->mxl_flow_writer = NULL;
    }

    av_free(flow_state->mxl_flow_config_info);
    flow_state->mxl_flow_config_info = NULL;
}

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

    if (p->state.has_video_stream) {
        flow_teardown(s, &p->state.video.flow);
    }

    if (p->state.has_audio_stream) {
        flow_teardown(s, &p->state.audio.flow);
        swr_free(&p->state.audio.swr_context);
    }

    if (p->state.mxl_instance) {
        mxl_status = mxlDestroyInstance(p->state.mxl_instance);
        if (MXL_STATUS_OK != mxl_status)
            logw(s, "mxlDestroyInstance error %s\n", mxl_status_to_str(mxl_status));
        p->state.mxl_instance = NULL;
    }

    return 0;
}

static int mxl_query_codec(enum AVCodecID id, int std_compliance)
{
    switch (id) {
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_V210:
        return 1;
    default:
        return 0;
    }
}

// MXL muxer definition
const FFOutputFormat ff_mxl_muxer = {
    // AVOutputFormat (public-facing fields)
    .p.name       = "mxl",
    .p.long_name  = NULL_IF_CONFIG_SMALL(MXL_LONG_NAME),
    .p.extensions = MXL_FLOW_EXT,
    .p.priv_class = &mxl_muxer_class,
    .p.flags      = AVFMT_NOFILE,

    .p.audio_codec = AV_CODEC_ID_NONE,
    .p.video_codec = AV_CODEC_ID_V210,

     // private state
    .priv_data_size = sizeof(MXLContext),

     // muxer callbacks
    .write_header   = mxl_write_header,
    .write_packet   = mxl_write_packet,
    .write_trailer  = mxl_write_trailer,
    .query_codec    = mxl_query_codec,
};
