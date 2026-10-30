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


MXL_REL_TMP_DIR := tests/data/tmp/mxl
MXL_TMP_DIR := $(TARGET_PATH)/$(MXL_REL_TMP_DIR)

# Note: MXL_OPTIONS is defined explicitly (even though it matches the
# default MXL value) to make the test's dependency on this
# history_duration setting unambiguous and intentional.
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

# Normalize the build-dependent domain path in MXL demux regression diagnostics
FILTER_MXL_DEMUX_PATH = sed -E "s@^(mxl_demux_test: creating MXL domain )/[^[:space:]]*/tests/data/tmp/mxl/@\1tests/data/tmp/mxl/@"

# Use to normalize the log prefix in "[mxl 0xXXXXXXXX]" messages
FILTER_MXL_LOG_PREFIX = sed -E "s/(\[mxl @ 0x)[[:xdigit:]]+(\])/\1ADDR\2/g"

# Grep out normalized mxl log messages, and the final "Invalid
# argument" or "Input/output error" message
PROBE_LOG_FILTER = $(FILTER_PWD_FROM_MXL_URI) | $(FILTER_MXL_LOG_PREFIX) | \
                   grep -E "^\[mxl @ 0xADDR\]|: Invalid argument$$|: Input/output error$$"

# Usage: $(call MXL_MUX_DEMUX_TEST,<test-dir-name>,<mux-command>,<demux-command>,<failure-label>,<output-filter>)
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
# teardown. An optional `output-filter` is applied to successful demux
# output. The macro waits for the muxer to exit before completing.
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
            $(if $(strip $(5)), \
                if [ $$STATUS -eq 0 ]; then \
                    $(5) < "$$OUTFILE" > "$$OUTFILE.filtered" || exit 1; \
                    mv "$$OUTFILE.filtered" "$$OUTFILE" || exit 1; \
                fi; \
            ) \
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
            -f mxl -max_video_frames 5 -mxl_read_index_init tail \
            -i domain/$(MXL_VIDEO_FLOW_ID).mxl-flow \
            -bitexact -f framemd5 pipe:1, \
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
            -f mxl -max_audio_samples $(MXL_AUDIO_MAX_SAMPLES) -max_audio_samples_per_read $(MXL_AUDIO_SAMPLES_PER_PACKET) -mxl_read_index_init tail \
            -i domain/$(MXL_AUDIO_FLOW_ID).mxl-flow \
            -bitexact -f framemd5 pipe:1, \
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
            -mxl_read_index_init tail \
            -i "mxl://$$(pwd)/domain?id=$(MXL_VIDEO_FLOW_ID)&id=$(MXL_AUDIO_FLOW_ID)" \
            -bitexact -f framemd5 pipe:1, \
        demuxer, \
	$(abspath $(SRC_PATH)/tests/fate/mxl_normalize_framemd5.py))
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

# MXL demuxer timestamp and discontinuity regression tests

$(MXL_REL_TMP_DIR):
	$(Q)mkdir -p $@

define MXL_DEMUX_FATE_TEST
fate-mxl-demux-$(1): libavformat/tests/mxl_demux_test$(EXESUF)
fate-mxl-demux-$(1): CMD = run libavformat/tests/mxl_demux_test$(EXESUF) $(MXL_TMP_DIR)/demux-$(1) $(1) 2>&1 | $(FILTER_MXL_LOG_PREFIX) | $(FILTER_MXL_DEMUX_PATH)
fate-mxl-demux-$(1): REF = $(SRC_PATH)/tests/ref/fate/mxl-demux-$(1)
FATE-$$(CONFIG_MXL_DEMUXER) += fate-mxl-demux-$(1)
endef

MXL_DEMUX_TESTS := delayed-audio delayed-video video-interruption audio-interruption

$(foreach test,$(MXL_DEMUX_TESTS),$(eval $(call MXL_DEMUX_FATE_TEST,$(test))))

FATE_MXL_DEMUX := $(addprefix fate-mxl-demux-,$(MXL_DEMUX_TESTS))

$(FATE_MXL_DEMUX): | $(MXL_REL_TMP_DIR)

.PHONY: fate-mxl-demux
fate-mxl-demux: $(FATE_MXL_DEMUX)

# MXL muxer PTS-derived write index regression tests

define MXL_MUX_FATE_TEST
fate-mxl-mux-$(1): libavformat/tests/mxl_mux_test$(EXESUF)
fate-mxl-mux-$(1): CMD = run libavformat/tests/mxl_mux_test$(EXESUF) $(MXL_TMP_DIR)/mux-$(1) $(1)
fate-mxl-mux-$(1): REF = $(SRC_PATH)/tests/ref/fate/mxl-mux-$(1)
FATE-$$(CONFIG_MXL_MUXER) += fate-mxl-mux-$(1)
endef

MXL_MUX_TESTS := video-pts audio-pts video-discontinuity audio-discontinuity \
                 video-missing-pts audio-missing-pts video-negative-pts audio-negative-pts \
		 audio-duration audio-invalid-size

$(foreach test,$(MXL_MUX_TESTS),$(eval $(call MXL_MUX_FATE_TEST,$(test))))

FATE_MXL_MUX := $(addprefix fate-mxl-mux-,$(MXL_MUX_TESTS))

$(FATE_MXL_MUX): | $(MXL_REL_TMP_DIR)

.PHONY: fate-mxl-mux
fate-mxl-mux: $(FATE_MXL_MUX)


# MXL demuxer-to-muxer absolute-index preservation regression tests

MXL_TRANSCODE_SRC_VIDEO_FLOW_ID := b02f6c84-e174-48b3-9adc-3993ac5937d3
MXL_TRANSCODE_SRC_AUDIO_FLOW_ID := 0d809e0b-87c1-4202-80d8-6927bd082ddc
MXL_TRANSCODE_DST_VIDEO_FLOW_ID := 145bc625-d7d8-47b9-b9bc-9bd1bfd49ab8
MXL_TRANSCODE_DST_AUDIO_FLOW_ID := 77fee0d7-c1f5-4eb5-8bff-a0fe7cff2e71
MXL_TRANSCODE_FLOW_TIMEOUT_MS := 5000
MXL_TRANSCODE_AUDIO_SAMPLES := 48000
MXL_TRANSCODE_AUDIO_SAMPLES_PER_READ := 800

# Usage: $(call MXL_TRANSCODE_TEST,<test-name>,<source-options>,<destination-options>,<source-flow-ids>,<input-flow-ids>,<stream-maps>,<output-flow-options>)
#
# Start the native test, wait for each source flow to become active, then
# stream-copy with FFmpeg. Only native test output is passed to FATE;
# FFmpeg and readiness diagnostics remain in the per-test directory.
define MXL_TRANSCODE_TEST
    ( \
        set -e; \
        export MXL_LOG_LEVEL=off; \
        TEST_DIR="$(MXL_TMP_DIR)/transcode-$(strip $(1))"; \
        rm -rf "$$TEST_DIR"; \
        mkdir -p "$$TEST_DIR/dst"; \
        cd "$$TEST_DIR"; \
        $(TARGET_PATH)/libavformat/tests/mxl_transcode_test$(EXESUF) \
            "$$TEST_DIR/src" $(2) "$$TEST_DIR/dst" $(3) \
            > "$$TEST_DIR/transcode.log" 2>&1 & \
        TEST_PID=$$!; \
        FFMPEG_PID=; \
        trap "kill $$TEST_PID $$FFMPEG_PID 2>/dev/null || true" INT TERM EXIT; \
        for FLOW_ID in $(4); do \
            if ! $(TARGET_PATH)/libavformat/tests/mxl_is_flow_active$(EXESUF) \
                    "$$TEST_DIR/src" "$$FLOW_ID" $(MXL_TRANSCODE_FLOW_TIMEOUT_MS) \
                    >> "$$TEST_DIR/wait_flow.log" 2>&1; then \
                kill $$TEST_PID 2>/dev/null || true; \
                wait $$TEST_PID 2>/dev/null || true; \
                cat "$$TEST_DIR/transcode.log"; \
                exit 1; \
            fi; \
        done; \
        $(TARGET_PATH)/ffmpeg -hide_banner -loglevel info \
            -f mxl -mxl_read_index_init head \
            -max_audio_samples $(MXL_TRANSCODE_AUDIO_SAMPLES) \
            -max_audio_samples_per_read $(MXL_TRANSCODE_AUDIO_SAMPLES_PER_READ) \
            -copytb 1 -i "mxl://$$TEST_DIR/src?id=$(strip $(5))" \
            $(6) -copyts -c:v copy -c:a copy \
            -avoid_negative_ts disabled -f mxl -mxl_write_index pts \
            $(7) "$$TEST_DIR/dst" > "$$TEST_DIR/ffmpeg.log" 2>&1 & \
        FFMPEG_PID=$$!; \
        set +e; \
        wait $$FFMPEG_PID; \
        FFMPEG_STATUS=$$?; \
        wait $$TEST_PID; \
        TEST_STATUS=$$?; \
        cat "$$TEST_DIR/transcode.log"; \
        if [ $$FFMPEG_STATUS -ne 0 ] || [ $$TEST_STATUS -ne 0 ]; then exit 1; fi; \
        trap - INT TERM EXIT; \
    )
endef

FATE_MXL_TRANSCODE := fate-mxl-transcode-video fate-mxl-transcode-audio fate-mxl-transcode-av

$(FATE_MXL_TRANSCODE): libavformat/tests/mxl_transcode_test$(EXESUF) \
                      libavformat/tests/mxl_is_flow_active$(EXESUF)
$(FATE_MXL_TRANSCODE): | $(MXL_REL_TMP_DIR)

fate-mxl-transcode-video: CMD = \
    $(call MXL_TRANSCODE_TEST, \
        video, \
        --video=$(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID), \
        --video=$(MXL_TRANSCODE_DST_VIDEO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID), \
        -map 0:v:0, \
        -video_flow_id $(MXL_TRANSCODE_DST_VIDEO_FLOW_ID))
fate-mxl-transcode-video: REF = $(SRC_PATH)/tests/ref/fate/mxl-transcode-video

fate-mxl-transcode-audio: CMD = \
    $(call MXL_TRANSCODE_TEST, \
        audio, \
        --audio=$(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        --audio=$(MXL_TRANSCODE_DST_AUDIO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        -map 0:a:0, \
        -audio_flow_id $(MXL_TRANSCODE_DST_AUDIO_FLOW_ID))
fate-mxl-transcode-audio: REF = $(SRC_PATH)/tests/ref/fate/mxl-transcode-audio

fate-mxl-transcode-av: CMD = \
    $(call MXL_TRANSCODE_TEST, \
        av, \
        --video=$(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID) --audio=$(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        --video=$(MXL_TRANSCODE_DST_VIDEO_FLOW_ID) --audio=$(MXL_TRANSCODE_DST_AUDIO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID) $(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        $(MXL_TRANSCODE_SRC_VIDEO_FLOW_ID)&id=$(MXL_TRANSCODE_SRC_AUDIO_FLOW_ID), \
        -map 0:v:0 -map 0:a:0, \
        -video_flow_id $(MXL_TRANSCODE_DST_VIDEO_FLOW_ID) -audio_flow_id $(MXL_TRANSCODE_DST_AUDIO_FLOW_ID))
fate-mxl-transcode-av: REF = $(SRC_PATH)/tests/ref/fate/mxl-transcode-av

FATE-yes += $(if $(filter yes,$(CONFIG_MXL_DEMUXER)), \
            $(if $(filter yes,$(CONFIG_MXL_MUXER)),$(FATE_MXL_TRANSCODE)))

.PHONY: fate-mxl-transcode
fate-mxl-transcode: $(FATE_MXL_TRANSCODE)
