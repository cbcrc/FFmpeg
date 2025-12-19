fate-mxl-json: libavformat/tests/mxl_json$(EXESUF)
fate-mxl-json: CMD = run libavformat/tests/mxl_json$(EXESUF)

# Note: MXL_OPTIONS is defined explicitly (even though it matches the
# default MXL value) to make the test's dependency on this
# history_duration setting unambiguous and intentional.

MXL_TMP_DIR := $(TARGET_PATH)/tests/data/tmp/mxl
MXL_DOMAIN_DIR := $(MXL_TMP_DIR)/domain
MXL_OPTIONS_JSON  := $(MXL_DOMAIN_DIR)/options.json
MXL_OPTIONS := "{"\"urn:x-mxl:option:history_duration/v1.0\"":100000000}"
MXL_VIDEO_FLOW_ID := 717f834b-4224-4c9b-8a64-ecb7726803b8
MXL_VIDEO_FLOW_DIR =$(MXL_DOMAIN_DIR)/$(MXL_VIDEO_FLOW_ID).mxl-flow
MXL_AUDIO_FLOW_ID := ff39f65b-d760-4a7c-808d-4f2778de5658
MXL_AUDIO_FLOW_DIR =$(MXL_DOMAIN_DIR)/$(MXL_AUDIO_FLOW_ID).mxl-flow
MXL_AUDIO_MAX_SAMPLES=2559
MXL_AUDIO_SAMPLES_PER_PACKET=512
MXL_VIDEO_SENTINEL := $(MXL_TMP_DIR)/sentinel.video
MXL_AUDIO_SENTINEL := $(MXL_TMP_DIR)/sentinel.audio
MXL_SENTINEL_TIMEOUT := 4000
DEMUX_RETRY_SLEEP := 0.3
DEMUX_RETRY_LIMIT := 10

.PHONY: mxl_domain_init
mxl_domain_init:
	$(Q)rm -rf $(MXL_DOMAIN_DIR)
	$(Q)mkdir -p $(MXL_DOMAIN_DIR)
	$(Q)echo $(MXL_OPTIONS) > $(MXL_OPTIONS_JSON)

fate-mxl-video-encdec: CMD = \
    ( \
        set -e; \
        rm -rf $(MXL_VIDEO_FLOW_DIR); \
        rm -f $(MXL_VIDEO_SENTINEL); \
        $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -v error -re \
	    -f lavfi -i testsrc2=size=1920x1080:rate=50 -frames:v 5 -c:v v210 \
	    -f mxl -video_flow_id $(MXL_VIDEO_FLOW_ID) \
	    -teardown_sync_file $(MXL_VIDEO_SENTINEL) -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
	    $(MXL_DOMAIN_DIR) & \
        MUX_PID=$$!; \
        trap "kill $$MUX_PID 2>/dev/null || true" INT TERM EXIT; \
        set +e; \
	RETRIES=0; \
	while [ $$RETRIES -lt $(DEMUX_RETRY_LIMIT) ]; do \
            sleep $(DEMUX_RETRY_SLEEP); \
            $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -v error \
	        -f mxl -max_video_frames 5 -grain_index_init 2 -i $(MXL_VIDEO_FLOW_DIR) \
	        -f framemd5 pipe:1; \
            DEMUX_STATUS=$$?; \
            if [ $$DEMUX_STATUS -eq 0 ]; then \
	        touch $(MXL_VIDEO_SENTINEL); \
		break; \
            else RETRIES=$$((RETRIES+1)); \
            fi; \
        done; \
        set -e; \
	if [ $$RETRIES -ge $(DEMUX_RETRY_LIMIT) ]; then echo "demuxer failed"; exit 1; fi; \
        wait $$MUX_PID; \
        trap - INT TERM EXIT; \
    )

fate-mxl-video-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-video-encdec

fate-mxl-audio-encdec: CMD = \
    ( \
        set -e; \
        rm -rf $(MXL_AUDIO_FLOW_DIR); \
        rm -f $(MXL_AUDIO_SENTINEL); \
        $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -re -v error \
	    -f lavfi -i "anoisesrc=sample_rate=48000:nb_samples=$(MXL_AUDIO_SAMPLES_PER_PACKET):seed=0,aformat=sample_fmts=flt:channel_layouts=stereo,atrim=end_sample=$(MXL_AUDIO_MAX_SAMPLES)" \
	    -map 0:a:0 -c:a pcm_f32le \
	    -f mxl -audio_flow_id $(MXL_AUDIO_FLOW_ID) \
	    -teardown_sync_file $(MXL_AUDIO_SENTINEL) -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
	    $(MXL_DOMAIN_DIR) & \
	MUX_PID=$$!; \
        trap "kill $$MUX_PID 2>/dev/null || true" INT TERM EXIT; \
        set +e; \
	RETRIES=0; \
	while [ $$RETRIES -lt $(DEMUX_RETRY_LIMIT) ]; do \
            sleep $(DEMUX_RETRY_SLEEP); \
            $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -v error \
                -f mxl -max_audio_samples $(MXL_AUDIO_MAX_SAMPLES) -max_audio_samples_per_read $(MXL_AUDIO_SAMPLES_PER_PACKET) -grain_index_init 2 \
                -i $(MXL_AUDIO_FLOW_DIR) \
	        -f framemd5 pipe:1; \
            DEMUX_STATUS=$$?; \
            if [ $$DEMUX_STATUS -eq 0 ]; then \
	        touch $(MXL_AUDIO_SENTINEL); \
		break; \
            else RETRIES=$$((RETRIES+1)); \
            fi; \
        done; \
        set -e; \
	if [ $$RETRIES -ge $(DEMUX_RETRY_LIMIT) ]; then echo "demuxer failed"; exit 1; fi; \
        wait $$MUX_PID; \
        trap - INT TERM EXIT; \
    )

fate-mxl-audio-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-audio-encdec

fate-mxl-video-encdec fate-mxl-audio-encdec: | mxl_domain_init

# json test if demuxer is enabled
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-json)

# video encode/decode test if both muxer and demuxer are enabled
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-video-encdec))

# audio encode/decode test if both muxer and demuxer are enabled
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-audio-encdec))
