fate-mxl-json: libavformat/tests/mxl_json$(EXESUF)
fate-mxl-json: CMD = run libavformat/tests/mxl_json$(EXESUF)

MXL_TMP_DIR := $(TARGET_PATH)/tests/data/tmp/mxl
MXL_DOMAIN_DIR := $(MXL_TMP_DIR)/domain
MXL_FLOW_ID := 717f834b-4224-4c9b-8a64-ecb7726803b8
MXL_FLOW_DIR =$(MXL_DOMAIN_DIR)/$(MXL_FLOW_ID).mxl-flow
MXL_SENTINEL := $(MXL_TMP_DIR)/sentinel
MXL_SENTINEL_TIMEOUT := 3000
DEMUX_RETRY_SLEEP := 0.3
DEMUX_RETRY_LIMIT := 10

fate-mxl-encdec: CMD = \
    ( \
        set -e; \
        mkdir -p $(MXL_TMP_DIR); \
        mkdir -p $(MXL_DOMAIN_DIR); \
        rm -rf $(MXL_FLOW_DIR); \
        rm -f $(MXL_SENTINEL); \
        $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -v error \
	    -f lavfi -i testsrc2=size=1920x1080:rate=50 -frames:v 5 -c:v v210 \
	    -f mxl -flow_id $(MXL_FLOW_ID) \
	    -teardown_sync_file $(MXL_SENTINEL) -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
	    $(MXL_DOMAIN_DIR) & \
        MUX_PID=$$!; \
        trap "kill $$MUX_PID 2>/dev/null || true" INT TERM EXIT; \
        set +e; \
	RETRIES=0; \
	while [ $$RETRIES -lt $(DEMUX_RETRY_LIMIT) ]; do \
            sleep $(DEMUX_RETRY_SLEEP); \
            $(TARGET_PATH)/ffmpeg -hide_banner -nostdin -v error \
	        -f mxl -max_frames 5 -grain_index_init 2 -i $(MXL_FLOW_DIR) \
	        -f framemd5 pipe:1; \
            DEMUX_STATUS=$$?; \
            if [ $$DEMUX_STATUS -eq 0 ]; then \
	        touch $(MXL_SENTINEL); \
		break; \
            else RETRIES=$$((RETRIES+1)); \
            fi; \
        done; \
        set -e; \
	if [ $$RETRIES -ge $(DEMUX_RETRY_LIMIT) ]; then echo "demuxer failed"; exit 1; fi; \
        wait $$MUX_PID; \
        trap - INT TERM EXIT; \
    )

fate-mxl-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-encdec

# json test if either muxer or demuxer are enabled
FATE-yes += $(sort \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-json) \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-json))

# encode/decode test if both muxer and demuxer are enabled
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-encdec))
