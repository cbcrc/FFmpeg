fate-mxl-json: libavformat/tests/mxl_json$(EXESUF)
fate-mxl-json: CMD = run libavformat/tests/mxl_json$(EXESUF)
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-json)

fate-mxl-uri: libavformat/tests/mxl_uri$(EXESUF)
fate-mxl-uri: CMD = run libavformat/tests/mxl_uri$(EXESUF)
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-uri)

fate-mxl-loc: libavformat/tests/mxl_loc$(EXESUF)
fate-mxl-loc: CMD = run libavformat/tests/mxl_loc$(EXESUF)
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-loc)

fate-mxl-diag: libavformat/tests/mxl_diag$(EXESUF)
fate-mxl-diag: CMD = run libavformat/tests/mxl_diag$(EXESUF)
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-diag)

# Note: MXL_OPTIONS is defined explicitly (even though it matches the
# default MXL value) to make the test's dependency on this
# history_duration setting unambiguous and intentional.

MXL_TMP_DIR := $(TARGET_PATH)/tests/data/tmp/mxl
MXL_OPTIONS := "{"\"urn:x-mxl:option:history_duration/v1.0\"":100000000}"
MXL_VIDEO_FLOW_ID := 717f834b-4224-4c9b-8a64-ecb7726803b8
MXL_AUDIO_FLOW_ID := ff39f65b-d760-4a7c-808d-4f2778de5658
MXL_AUDIO_MAX_SAMPLES := 2559
MXL_AUDIO_SAMPLES_PER_PACKET := 512
MXL_SENTINEL_TIMEOUT := 4000
RETRY_SLEEP := 0.3
RETRY_LIMIT := 10

# Use this to construct macro arguments that require "," characters
COMMA := ,

# Use this to normalize local paths
FILTER_PWD_FROM_MXL_URI = sed "s|mxl://$$(pwd)|mxl:///path/to|g"

# Use to normalize the log prefix in "[mxl 0xXXXXXXXX]" messages
FILTER_MXL_LOG_PREFIX = sed -E "s/(\[mxl @ 0x)[[:xdigit:]]+(\])/\1ADDR\2/g"

# Grep out normalized mxl log messages, and the final "Invalid
# argument" or "Input/output error" message
PROBE_LOG_FILTER = $(FILTER_PWD_FROM_MXL_URI) | $(FILTER_MXL_LOG_PREFIX) | \
                   grep -E "^\[mxl @ 0xADDR\]|: Invalid argument$$|: Input/output error$$"

# Usage: $(call MXL_MUX_DEMUX_TEST,<test-dir-name>,<mux-command>,<demux-command>,<failure-label>)
#
# Run an MXL test in an isolated per-test temporary directory.
#
# The macro creates $(MXL_TMP_DIR)/<test-dir-name>, after removing any
# previous copy of that directory, and changes to that directory.  A
# local MXL domain directory is created in the working directory at
# ./domain, and `mux-command` is executed in the background.
#
# It then retries the supplied `demux-command` until it succeeds or
# the retry limit is reached. On success it touches the local
# ./sentinel file so the `mux-command` can perform synchronized
# teardown. The macro waits for the muxer to exit before completing.
#
define MXL_MUX_DEMUX_TEST
    ( \
        set -e; \
        export MXL_LOG_LEVEL=off; \
        TEST_DIR="$(MXL_TMP_DIR)/$(strip $(1))"; \
        rm -rf "$$TEST_DIR"; \
        mkdir -p "$$TEST_DIR"; \
        cd "$$TEST_DIR"; \
        rm -f sentinel; \
        mkdir domain; \
        echo $(MXL_OPTIONS) > domain/options.json; \
        $(2) & \
        MUX_PID=$$!; \
        trap "kill $$MUX_PID 2>/dev/null || true" INT TERM EXIT; \
        set +e; \
        RETRIES=0; \
        while [ $$RETRIES -lt $(RETRY_LIMIT) ]; do \
            sleep $(RETRY_SLEEP); \
            OUTFILE=$$(mktemp "$$TEST_DIR"/demux.out.XXXXXX) || exit 1; \
            $(3) > "$$OUTFILE" 2>&1; \
            STATUS=$$?; \
            $(FILTER_PWD_FROM_MXL_URI) < "$$OUTFILE"; \
            rm -f "$$OUTFILE"; \
            if [ $$STATUS -eq 0 ]; then \
                touch sentinel; \
                break; \
            else \
                RETRIES=$$((RETRIES+1)); \
            fi; \
        done; \
        set -e; \
        if [ $$RETRIES -ge $(RETRY_LIMIT) ]; then echo "$(4) failed"; exit 1; fi; \
        wait $$MUX_PID; \
        trap - INT TERM EXIT; \
    )
endef

# Usage: $(call MXL_PROBE,<locator>)
# e.g. $(call MXL_PROBE,domain/<flow-id>.mxl-flow)
#      $(call MXL_PROBE,mxl:///domain?id=<flow-id>)
define MXL_PROBE
$(TARGET_PATH)/ffprobe -hide_banner -v verbose -f mxl -i "$(1)" 2>&1 | $(PROBE_LOG_FILTER)
endef

fate-mxl-video-encdec: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-video-encdec, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error -re \
            -f lavfi -i testsrc2=size=1920x1080:rate=50 -frames:v 5 -c:v v210 \
            -f mxl -video_flow_id $(MXL_VIDEO_FLOW_ID) \
            -teardown_sync_file sentinel -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error \
            -f mxl -max_video_frames 5 -grain_index_init 2 \
            -i domain/$(MXL_VIDEO_FLOW_ID).mxl-flow \
            -f framemd5 pipe:1, \
        demuxer)
fate-mxl-video-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-video-encdec
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-video-encdec))

fate-mxl-audio-encdec: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-audio-encdec, \
        $(TARGET_PATH)/ffmpeg -hide_banner -re -v error \
            -f lavfi -i "anoisesrc=sample_rate=48000:nb_samples=$(MXL_AUDIO_SAMPLES_PER_PACKET):seed=0$(COMMA)aformat=sample_fmts=flt:channel_layouts=stereo$(COMMA)atrim=end_sample=$(MXL_AUDIO_MAX_SAMPLES)" \
            -map 0:a:0 -c:a pcm_f32le \
            -f mxl -audio_flow_id $(MXL_AUDIO_FLOW_ID) \
            -teardown_sync_file sentinel -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error \
            -f mxl -max_audio_samples $(MXL_AUDIO_MAX_SAMPLES) -max_audio_samples_per_read $(MXL_AUDIO_SAMPLES_PER_PACKET) -grain_index_init 2 \
            -i domain/$(MXL_AUDIO_FLOW_ID).mxl-flow \
            -f framemd5 pipe:1, \
        demuxer)
fate-mxl-audio-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-audio-encdec
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-audio-encdec))

fate-mxl-av-encdec: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-av-encdec, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error -re \
            -f lavfi -i testsrc2=size=1920x1080:rate=50 \
            -f lavfi -i "anoisesrc=sample_rate=48000:nb_samples=$(MXL_AUDIO_SAMPLES_PER_PACKET):seed=0$(COMMA)aformat=sample_fmts=flt:channel_layouts=stereo$(COMMA)atrim=end_sample=$(MXL_AUDIO_MAX_SAMPLES)" \
            -map 0:v:0 -map 1:a:0 -frames:v 5 \
            -c:v v210 -c:a pcm_f32le \
            -f mxl -video_flow_id $(MXL_VIDEO_FLOW_ID) -audio_flow_id $(MXL_AUDIO_FLOW_ID) \
            -teardown_sync_file sentinel -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error \
            -f mxl -max_video_frames 5 \
            -max_audio_samples $(MXL_AUDIO_MAX_SAMPLES) -max_audio_samples_per_read $(MXL_AUDIO_SAMPLES_PER_PACKET) \
            -grain_index_init 2 \
            -i "mxl://$$(pwd)/domain?id=$(MXL_VIDEO_FLOW_ID)&id=$(MXL_AUDIO_FLOW_ID)" \
            -f framemd5 pipe:1, \
        demuxer)
fate-mxl-av-encdec: REF = $(SRC_PATH)/tests/ref/fate/mxl-av-encdec
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-av-encdec))

fate-mxl-video-probe: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-video-probe, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error -re \
            -f lavfi -i testsrc2=size=1920x1080:rate=50 -frames:v 5 -c:v v210 \
            -f mxl -video_flow_id $(MXL_VIDEO_FLOW_ID) \
            -teardown_sync_file sentinel \
            -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffprobe -hide_banner -v info \
            mxl://$$(pwd)/domain?id=$(MXL_VIDEO_FLOW_ID) 2>&1, \
        ffprobe)
fate-mxl-video-probe: REF = $(SRC_PATH)/tests/ref/fate/mxl-video-probe
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-video-probe))

fate-mxl-audio-probe: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-audio-probe, \
        $(TARGET_PATH)/ffmpeg -hide_banner -re -v error \
            -f lavfi -i "anoisesrc=sample_rate=48000:nb_samples=$(MXL_AUDIO_SAMPLES_PER_PACKET):seed=0$(COMMA)aformat=sample_fmts=flt:channel_layouts=stereo$(COMMA)atrim=end_sample=$(MXL_AUDIO_MAX_SAMPLES)" \
            -map 0:a:0 -c:a pcm_f32le \
            -f mxl -audio_flow_id $(MXL_AUDIO_FLOW_ID) \
            -teardown_sync_file sentinel -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffprobe -hide_banner -v info \
            mxl://$$(pwd)/domain?id=$(MXL_AUDIO_FLOW_ID) 2>&1, \
        ffprobe)
fate-mxl-audio-probe: REF = $(SRC_PATH)/tests/ref/fate/mxl-audio-probe
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-audio-probe))

fate-mxl-av-probe: CMD = \
    $(call MXL_MUX_DEMUX_TEST, \
        mxl-audio-video-probe, \
        $(TARGET_PATH)/ffmpeg -hide_banner -v error -re \
            -f lavfi -i testsrc2=size=1920x1080:rate=50 \
            -f lavfi -i "anoisesrc=sample_rate=48000:nb_samples=$(MXL_AUDIO_SAMPLES_PER_PACKET):seed=0$(COMMA)aformat=sample_fmts=flt:channel_layouts=stereo$(COMMA)atrim=end_sample=$(MXL_AUDIO_MAX_SAMPLES)" \
            -map 0:v:0 -map 1:a:0 -frames:v 5 \
            -c:v v210 -c:a pcm_f32le \
            -f mxl -video_flow_id $(MXL_VIDEO_FLOW_ID) -audio_flow_id $(MXL_AUDIO_FLOW_ID) \
            -teardown_sync_file sentinel -teardown_sync_timeout $(MXL_SENTINEL_TIMEOUT) \
            domain, \
        $(TARGET_PATH)/ffprobe -hide_banner -v info \
            "mxl://$$(pwd)/domain?id=$(MXL_VIDEO_FLOW_ID)&id=$(MXL_AUDIO_FLOW_ID)" 2>&1, \
        ffprobe)
fate-mxl-av-probe: REF = $(SRC_PATH)/tests/ref/fate/mxl-av-probe
FATE-yes += \
    $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
    $(if $(filter yes,$(CONFIG_MXL_MUXER)),fate-mxl-av-probe))

fate-mxl-bad-loc: CMD = (\
    $(call MXL_PROBE,/domain/without/flow); \
    $(call MXL_PROBE,mxl:///domain/without/flow); \
    $(call MXL_PROBE,/domain/bad-flow-id.mxl-flow); \
    $(call MXL_PROBE,mxl:///domain?id=bad-flow-id); \
    $(call MXL_PROBE,/domain/bad-$(MXL_VIDEO_FLOW_ID).mxl-flow); \
    $(call MXL_PROBE,mxl:///domain?id=bad-$(MXL_VIDEO_FLOW_ID)); \
    $(call MXL_PROBE,mxl:///domain/duplicate_flows?id=$(MXL_VIDEO_FLOW_ID)&id=$(MXL_VIDEO_FLOW_ID)); \
    $(call MXL_PROBE,mxl://with-host/domain?id=$(MXL_VIDEO_FLOW_ID)); \
    true \
    )
fate-mxl-bad-loc: REF = $(SRC_PATH)/tests/ref/fate/mxl-bad-loc
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-bad-loc)

fate-mxl-bad-domain: CMD = (\
    export MXL_LOG_LEVEL=off; \
    $(call MXL_PROBE,/domain/does/not/exist/$(MXL_VIDEO_FLOW_ID).mxl-flow); \
    $(call MXL_PROBE,mxl:///domain/does/not/exist?id=$(MXL_VIDEO_FLOW_ID)); \
    true \
    )
fate-mxl-bad-domain: REF = $(SRC_PATH)/tests/ref/fate/mxl-bad-domain
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-bad-domain)

fate-mxl-bad-flow: CMD = (\
    export MXL_LOG_LEVEL=off; \
    TEST_DIR="$(MXL_TMP_DIR)/bad_flow"; \
    rm -rf "$$TEST_DIR"; \
    mkdir -p "$$TEST_DIR"/domain; \
    (cd "$$TEST_DIR"; $(call MXL_PROBE,domain/$(MXL_VIDEO_FLOW_ID).mxl-flow)); \
    $(call MXL_PROBE,mxl://"$$TEST_DIR"/domain?id=$(MXL_VIDEO_FLOW_ID)); \
    true \
    ) 
fate-mxl-bad-flow: REF = $(SRC_PATH)/tests/ref/fate/mxl-bad-flow
FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)),fate-mxl-bad-flow)
