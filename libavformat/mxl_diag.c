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

#include "mxl_diag.h"
#include "mxl_log.h"

#include "libavutil/avassert.h"
#include <libavutil/error.h>
#include <libavutil/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char *msg_type_to_string(enum mxl_diag_msg_type type)
{
    switch (type) {
    case MXL_DIAG_MSG_UNDEFINED:
        return "UNDEFINED";
    case MXL_DIAG_MSG_CONNECT:
        return "CONNECT";
    case MXL_DIAG_MSG_RELEASE:
        return "RELEASE";
    case MXL_DIAG_MSG_VIDEO_READ:
        return "VIDEO_READ";
    case MXL_DIAG_MSG_AUDIO_READ:
        return "AUDIO_READ";
    default:
        return "UNKNOWN";
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return AVERROR(errno);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return AVERROR(errno);
    }
    return 0;
}

static int is_path_socket(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        return 0;
    }
    return S_ISSOCK(st.st_mode);
}

static int is_same_addr(const struct sockaddr_un *a, socklen_t a_len,
                               const struct sockaddr_un *b, socklen_t b_len)
{
    if (a_len != b_len) {
        return 0;
    }
    return memcmp(a, b, (size_t)a_len) == 0;
}

static void clear_client(mxl_diag_server *s)
{
    memset(&s->client_addr, 0, sizeof(s->client_addr));
    s->client_addr_len = 0;
    s->has_client = 0;
}

static int validate_path(const char *path)
{
    size_t len;

    if (!path || !*path) {
        return AVERROR(EINVAL);
    }

    len = strlen(path);
    if (len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return AVERROR(ENAMETOOLONG);
    }

    return 0;
}

int mxl_diag_init(void *logctx, mxl_diag_server *server, const char *server_path)
{
    (void)logctx;
    struct sockaddr_un addr;
    int fd = -1;
    int ret;

    if (!server)
        return AVERROR(EINVAL);

    memset(server, 0, sizeof(*server));
    server->fd = -1;

    ret = validate_path(server_path);
    if (ret < 0) {
        return ret;
    }

    // must not exist, or must be a socket
    if (access(server_path, F_OK) == 0) {
        if (!is_path_socket(server_path)) {
            return AVERROR(EEXIST);
        }
        if (unlink(server_path) < 0) {
            return AVERROR(errno);
        }
    } else if (errno != ENOENT) {
        return AVERROR(errno);
    }

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return AVERROR(errno);
    }

    ret = set_nonblocking(fd);
    if (ret < 0) {
        close(fd);
        return ret;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, server_path);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ret = AVERROR(errno);
        close(fd);
        return ret;
    }

    server->fd = fd;
    strcpy(server->server_path, server_path);
    clear_client(server);

    return 0;
}

void mxl_diag_close(void *logctx, mxl_diag_server *server)
{
    (void)logctx;

    if (!server)
        return;

    if (server->fd >= 0) {
        close(server->fd);
        server->fd = -1;
    }

    if (server->server_path[0] != '\0') {
        unlink(server->server_path);
        server->server_path[0] = '\0';
    }

    clear_client(server);
}

int mxl_diag_poll(void *logctx, mxl_diag_server *server)
{
    mxl_diag_msg msg;
    struct sockaddr_un from_addr;
    socklen_t from_addr_len = (socklen_t)sizeof(from_addr);
    int same_sender = 0;
    ssize_t n;

    if (!server || server->fd < 0)
        return AVERROR(EINVAL);

    memset(&msg, 0, sizeof(msg));
    memset(&from_addr, 0, sizeof(from_addr));

    struct iovec iov = {
        .iov_base = &msg,
        .iov_len  = sizeof(msg),
    };
    struct msghdr mh = {
        .msg_name       = &from_addr,
        .msg_namelen    = from_addr_len,
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = NULL,
        .msg_controllen = 0,
        .msg_flags      = 0,
    };

    n = recvmsg(server->fd, &mh, 0);
    from_addr_len = mh.msg_namelen;

    // system error
    if (n < 0) {
        // transient, retry later
        if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)
            return 0;

        logd(logctx, "DIAG server recvmsg error: %s\n", av_err2str(AVERROR(errno)));
        return AVERROR(errno);
    }

    if (mh.msg_flags & MSG_TRUNC) {
        logd(logctx, "DIAG server datagram was truncated\n");
        return AVERROR(MXL_DIAG_EDATATRUNCATED);
    }

    // data size smaller than sizeof(msg.header)
    if ((size_t)n < sizeof(msg.header)) {
        logd(logctx, "DIAG server message is smaller than sizeof(msg.header)\n");
        return AVERROR(MXL_DIAG_EDATATOOSMALL);
    }

    // header.size smaller than sizeof(msg.header)
    if (msg.header.size < (uint16_t)sizeof(msg.header)) {
        logd(logctx, "DIAG header.size is smaller than sizeof(msg.header)\n");
        return AVERROR(MXL_DIAG_ESIZETOOSMALL);
    }

    // header.size bigger than largest message
    if (msg.header.size > (uint16_t)sizeof(msg)) {
        logd(logctx, "DIAG server message header.size is larger than "
             "maximum possible message size\n");
        return AVERROR(MXL_DIAG_ESIZETOOBIG);
    }

    // datagram size vs header.size mismatch
    if ((uint16_t)n != msg.header.size) {
        logd(logctx, "DIAG server message size mismatch\n");
        return AVERROR(MXL_DIAG_ESIZEMISMATCH);
    }

    // header.version bad
    if (msg.header.version != MXL_DIAG_MSG_VERSION) {
        logd(logctx, "DIAG server message bad version: %d\n", msg.header.version);
        return AVERROR(MXL_DIAG_EBADVERSION);
    }

    if (server->has_client) {
        same_sender = is_same_addr(&from_addr, from_addr_len,
                                   &server->client_addr, server->client_addr_len);
    }

    logd(logctx, "DIAG server received control message: %s\n",
         msg_type_to_string(msg.header.type));

    switch (msg.header.type) {
    case MXL_DIAG_MSG_CONNECT:

        if ((size_t)n != sizeof(msg.header)) {
            logd(logctx, "DIAG connect message size mismatch (%d != %d)\n", n, sizeof(msg.header));
            return AVERROR(MXL_DIAG_EMSGSIZEMISMATCH);
        }

        if (same_sender) {
            logd(logctx, "DIAG server duplicate connect from client '%s'\n",
                 from_addr.sun_path);
            return 1;
        }

        if (server->has_client) {
            logd(logctx, "DIAG server ignored registration request from client '%s'\n",
                 from_addr.sun_path);
            return 1;
        }

        server->client_addr = from_addr;
        server->client_addr_len = from_addr_len;
        server->has_client = 1;

        logd(logctx, "DIAG server registered client '%s'\n", from_addr.sun_path);

        return 1;

    case MXL_DIAG_MSG_RELEASE:
        if ((size_t)n != sizeof(msg.header)) {
            logd(logctx, "DIAG release message size mismatch (%d != %d)\n", n, sizeof(msg.header));
            return AVERROR(MXL_DIAG_EMSGSIZEMISMATCH);
        }

        if (same_sender) {
            logd(logctx, "DIAG server released client '%s'\n", from_addr.sun_path);
            clear_client(server);
        }
        else
            logd(logctx, "DIAG server ignored release from an unregistered client '%s'\n",
                 from_addr.sun_path);

        return 1;

    default:
        logd(logctx, "DIAG server unknown client message from '%s'\n", from_addr.sun_path);
        return AVERROR(MXL_DIAG_EUNKNOWN);
    }
}

int mxl_diag_send(void *logctx, mxl_diag_server *server, const mxl_diag_msg *msg)
{
    ssize_t n;

    if (!server || !msg) {
        return AVERROR(EINVAL);
    }

    if (server->fd < 0) {
        return AVERROR(EBADF);
    }

    // lower bound sanity
    if ((size_t)msg->header.size < sizeof(msg->header)) {
        logd(logctx, "DIAG server sendto sanity, header.size is smaller "
             "than minimum message size\n");
        return AVERROR(EINVAL);
    }

    // upper bound sanity
    if (msg->header.size > sizeof(*msg)) {
        logd(logctx, "DIAG server sendto sanity, header.size is larger"
             " than maximum message size\n");
        return AVERROR(EINVAL);
    }

    if (!server->has_client) {
        return 0;
    }

    n = sendto(server->fd, msg, (size_t)msg->header.size, 0,
                        (const struct sockaddr *)&server->client_addr,
                        server->client_addr_len);
    if (n < 0) {
        // client disappeared
        if (ENOENT == errno || ECONNREFUSED == errno) {
            clear_client(server);
            return 0;
        }

        // transient, retry later
        if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)
            return 0;

        // an error worth logging
        logd(logctx, "DIAG server sendto failed: %s\n", strerror(errno));
        return AVERROR(errno);
    }

    if ((size_t)n != (size_t)msg->header.size) {
        logd(logctx, "DIAG server send length sanity: expected=%zu actual=%zd\n",
             (size_t)msg->header.size, n);
        return AVERROR(EIO);
    }

    return 1;
}
