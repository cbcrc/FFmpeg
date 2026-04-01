/*
 * MXL runtime diagnostics for Media eXchange Layer flows
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

#ifndef MXL_DIAG_H
#define MXL_DIAG_H

#include "libavutil/avassert.h"

#include <mxl/mxl.h>

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct mxl_diag_server {
    int fd;
    char server_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un client_addr;
    socklen_t client_addr_len;
    int has_client;
} mxl_diag_server;

enum mxl_diag_error_type {
    MXL_DIAG_EDATATOOSMALL = 2001,
    MXL_DIAG_EDATATRUNCATED,
    MXL_DIAG_ESIZETOOSMALL,
    MXL_DIAG_ESIZETOOBIG,
    MXL_DIAG_ESIZEMISMATCH,
    MXL_DIAG_EMSGSIZEMISMATCH,
    MXL_DIAG_EBADVERSION,
    MXL_DIAG_EUNKNOWN
};

enum { MXL_DIAG_MSG_VERSION = 1 };

enum mxl_diag_msg_type {
    MXL_DIAG_MSG_UNDEFINED = 0,
    MXL_DIAG_MSG_CONNECT,
    MXL_DIAG_MSG_RELEASE,
    MXL_DIAG_MSG_VIDEO_READ,
    MXL_DIAG_MSG_AUDIO_READ,
};

typedef struct mxl_diag_msg {
    struct {
        uint16_t version;
        uint16_t type;
        uint16_t size;
        uint8_t reserved[10];
    } header;
    union {
        struct {
            uint64_t timestamp;
            uint64_t tail_index;
            uint64_t head_index;
            uint64_t read_index;
            uint32_t mxl_status;
            uint8_t reserved[4];
        } video_read;
        struct {
            uint64_t timestamp;
            uint64_t tail_index;
            uint64_t head_index;
            uint64_t read_index;
            uint32_t read_size;
            uint32_t mxl_status;
        } audio_read;
    } u;
} mxl_diag_msg;

#define MXL_DIAG_LAYOUT_CHECK(cond, name) \
    typedef char mxl_diag_layout_check_##name[(cond) ? 1 : -1]
MXL_DIAG_LAYOUT_CHECK(sizeof(((mxl_diag_msg *)0)->header) == 16, header_size_must_be_16);
MXL_DIAG_LAYOUT_CHECK(sizeof(((mxl_diag_msg *)0)->u.video_read) == 40, video_read_size_must_be_40);
MXL_DIAG_LAYOUT_CHECK(sizeof(((mxl_diag_msg *)0)->u.audio_read) == 40, audio_read_size_must_be_40);
MXL_DIAG_LAYOUT_CHECK(sizeof(mxl_diag_msg) == 56, msg_size_must_be_56);

static inline void mxl_diag_init_msg_connect(mxl_diag_msg *msg)
{
    av_assert1(msg);
    int size = sizeof(msg->header);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_CONNECT;
    msg->header.size = size;
}

static inline void mxl_diag_init_msg_release(mxl_diag_msg *msg)
{
    av_assert1(msg);
    int size = sizeof(msg->header);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_RELEASE;
    msg->header.size = size;
}

static inline void mxl_diag_init_msg_video_read(
    mxl_diag_msg *msg,
    uint64_t timestamp,
    uint64_t tail_index,
    uint64_t head_index,
    uint64_t read_index,
    mxlStatus mxl_status)
{
    av_assert1(msg);
    int size = sizeof(msg->header) + sizeof(msg->u.video_read);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_VIDEO_READ;
    msg->header.size = size;
    msg->u.video_read.timestamp  = timestamp;
    msg->u.video_read.tail_index = tail_index;
    msg->u.video_read.head_index = head_index;
    msg->u.video_read.read_index = read_index;
    msg->u.video_read.mxl_status = mxl_status;
}

static inline void mxl_diag_init_msg_audio_read(
    mxl_diag_msg *msg,
    uint64_t timestamp,
    uint64_t tail_index,
    uint64_t head_index,
    uint64_t read_index,
    uint32_t read_size,
    mxlStatus mxl_status)
{
    av_assert1(msg);
    int size = sizeof(msg->header) + sizeof(msg->u.audio_read);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_AUDIO_READ;
    msg->header.size = size;
    msg->u.audio_read.timestamp  = timestamp;
    msg->u.audio_read.tail_index = tail_index;
    msg->u.audio_read.head_index = head_index;
    msg->u.audio_read.read_index = read_index;
    msg->u.audio_read.read_size = read_size;
    msg->u.audio_read.mxl_status = mxl_status;
}

/**
 * Initialize a diagnostic server socket.
 *
 * Creates a Unix domain datagram socket at `server_path`, configures it
 * non-blocking, and prepares it to accept registration from a single client.
 * If a stale socket file already exists at `server_path`, it is unlinked
 * before bind. If a non-socket filesystem entry exists there, initialization
 * fails.
 *
 * @param logctx      Log context for `av_log()`. May be NULL.
 * @param server      Server object to initialize.
 * @param server_path Filesystem path for the server Unix datagram socket.
 *
 * @return 0 on success, or a negative AVERROR(...) code on failure.
 */
int mxl_diag_init(void *logctx,
                  mxl_diag_server *server,
                  const char *server_path);

/**
 * Close a diagnostic server socket and release its resources.
 *
 * Closes the server socket, forgets any registered client, and unlinks the
 * server socket pathname if one is present.
 *
 * @param logctx   Log context for `av_log()`. May be NULL.
 * @param server   Server object to close.
 */
void mxl_diag_close(void *logctx,
                    mxl_diag_server *server);

/**
 * Poll the inbound socket and process at most one datagram.
 *
 * Transient errors EAGAIN, EWOULDBLOCK, and EINTR are ignored and
 * return zero. Other errors are returned as negative AVERROR(...)
 * values wrapping either a mxl_diag_error_type code or a system
 * error code.
 *
 * @param logctx Log context for `av_log()`. May be NULL.
 * @param server Server object to poll.
 * @return 0     If no datagram is available
 * @return 1     A datagram was processed
 * @return <0    A malformed-message or system error occurred
 */
int mxl_diag_poll(void *logctx, mxl_diag_server *server);

/**
 * Send one diagnostic message to the currently registered client.
 *
 * The message size is taken from msg->header.size and transmitted as-is.
 * The caller must ensure that the message header is initialized correctly
 * and that header.size reflects the number of bytes to send.
 *
 * If no client is currently registered, the message is silently
 * dropped and 0 is returned.
 *
 * Call mxl_diag_poll() periodically to process client connect and
 * release messages.
 *
 * If the client appears to have gone away, its registration is
 * cleared.
 *
 * System error codes ENOENT and ECONNREFUSED are handled, clear the
 * registered client, and return 0. Transient errors EAGAIN,
 * EWOULDBLOCK, and EINTR are ignored and return zero. Any other
 * system error is returned as a negative AVERROR(...) code.
 *
 * @param logctx  Log context for `av_log()`. May be NULL.
 * @param server  Diagnostic server state.
 * @param msg     Message to send. Must not be NULL.
 *
 * @return 1      On successful send.
 * @return 0      If no client is registered, or if a transient send error
 *                occurred that was handled
 * @return <0     A send error occurred
 */
int mxl_diag_send(void *logctx, mxl_diag_server *server, const mxl_diag_msg *msg);

#endif /* MXL_DIAG_H */
