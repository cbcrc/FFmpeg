/*
 * MXL runtime diagnostics tests for Media eXchange Layer flows
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

#include "libavformat/mxl_diag.h"

#include <libavutil/error.h>
#include "libavutil/avassert.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define TEST_DIR "/tmp/mxl_diag_test"

// simulated frame rate
enum { FPS = 30 };

/*
 * file descriptor leak detection
 */
enum { MAX_TRACKED_FDS = 8,
       MAX_SCANNED_FDS = 64 };
static int startup_open_fds[MAX_TRACKED_FDS];
static int startup_open_fd_count;

static int capture_startup_open_fds(void)
{
    int fd;

    startup_open_fd_count = 0;

    for (fd = 0; fd < MAX_SCANNED_FDS; ++fd) {
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            if (startup_open_fd_count >= MAX_TRACKED_FDS) {
                fprintf(stderr, "too many open fds at startup\n");
                return -1;
            }
            startup_open_fds[startup_open_fd_count++] = fd;
        }
    }

    return 0;
}

static int fd_is_in_startup_set(int fd)
{
    int i;

    for (i = 0; i < startup_open_fd_count; ++i) {
        if (startup_open_fds[i] == fd)
            return 1;
    }

    return 0;
}

static int check_open_fds_match_startup(void)
{
    int fd;

    for (fd = 0; fd < MAX_SCANNED_FDS; ++fd) {
        int is_open;

        errno = 0;
        is_open = (fcntl(fd, F_GETFD) != -1 || errno != EBADF);

        if (is_open && !fd_is_in_startup_set(fd)) {
            fprintf(stderr, "unexpected open fd: %d\n", fd);
            return -1;
        }
    }

    return 0;
}

/*
 * test diag client
 */
typedef struct mxl_diag_client {
    int fd;
    char client_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} mxl_diag_client;

static inline void mxl_diag_client_init_msg_connect(mxl_diag_msg *msg)
{
    av_assert1(msg);
    int size = sizeof(msg->header);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_CONNECT;
    msg->header.size = size;
}

static inline void mxl_diag_client_init_msg_release(mxl_diag_msg *msg)
{
    av_assert1(msg);
    int size = sizeof(msg->header);
    memset(msg, 0, size);
    msg->header.version = MXL_DIAG_MSG_VERSION;
    msg->header.type = MXL_DIAG_MSG_RELEASE;
    msg->header.size = size;
}

static int mxl_diag_client_connect(mxl_diag_client *client,
                                   const char *server_path,
                                   const char *client_path)
{
    struct sockaddr_un local_addr;
    struct sockaddr_un server_addr;
    mxl_diag_msg msg;

    if (!client || !server_path || !client_path) {
        return AVERROR(EINVAL);
    }

    memset(client, 0, sizeof(*client));
    client->fd = -1;

    if (strlen(server_path) >= sizeof(server_addr.sun_path) ||
        strlen(client_path) >= sizeof(local_addr.sun_path)) {
        return AVERROR(ENAMETOOLONG);
    }

    client->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (client->fd < 0) {
        return AVERROR(errno);
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path, client_path);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, server_path);

    strncpy(client->client_path, client_path, sizeof(client->client_path) - 1);
    client->client_path[sizeof(client->client_path) - 1] = '\0';

    unlink(client_path);

    if (bind(client->fd, (const struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        int err = errno;
        close(client->fd);
        client->fd = -1;
        unlink(client_path);
        return AVERROR(err);
    }

    if (connect(client->fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        int err = errno;
        close(client->fd);
        client->fd = -1;
        unlink(client_path);
        return AVERROR(err);
    }

    mxl_diag_client_init_msg_connect(&msg);

    if (send(client->fd, &msg, msg.header.size, 0) < 0) {
        int err = errno;
        close(client->fd);
        client->fd = -1;
        unlink(client_path);
        return AVERROR(err);
    }

    return 0;
}

static int mxl_diag_client_release(mxl_diag_client *client)
{
    mxl_diag_msg msg;

    av_assert0(client);

    if (!client) {
        return AVERROR(EINVAL);
    }

    if (client->fd < 0) {
        return AVERROR(EBADF);
    }

    mxl_diag_client_init_msg_release(&msg);

    if (send(client->fd, &msg, msg.header.size, 0) < 0) {
        return AVERROR(errno);
    }

    return 0;
}

static int mxl_diag_client_close(mxl_diag_client *client)
{
    int rc = 0;

    av_assert0(client);

    if (!client) {
        return AVERROR(EINVAL);
    }

    if (client->fd >= 0) {
        rc = mxl_diag_client_release(client);

        if (close(client->fd) < 0 && rc >= 0) {
            rc = AVERROR(errno);
        }
        client->fd = -1;
    }

    if (client->client_path[0] != '\0') {
        if (unlink(client->client_path) < 0 && errno != ENOENT && rc >= 0) {
            rc = AVERROR(errno);
        }
        client->client_path[0] = '\0';
    }

    return rc;
}

static int mxl_diag_client_recv(mxl_diag_client *client,
                                mxl_diag_msg *msg)
{
    const int timeout_ms = 100;

    struct pollfd pfd;
    ssize_t n;
    int ret;

    if (!client || !msg) {
        return AVERROR(EINVAL);
    }

    pfd.fd = client->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        return AVERROR(errno);
    }

    if (ret == 0) {
        return AVERROR(ETIMEDOUT); /* timeout */
    }

    n = recv(client->fd, msg, sizeof(*msg), 0);
    if (n < 0) {
        return AVERROR(errno);
    }

    if ((size_t)n < sizeof(msg->header)) {
        return AVERROR_INVALIDDATA;
    }

    if (msg->header.size < sizeof(msg->header)) {
        return AVERROR_INVALIDDATA;
    }

    if ((size_t)n < msg->header.size) {
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

/*
 * asynchronous tests
 */

static void print_message(const mxl_diag_msg *msg)
{
    if (!msg) {
        return;
    }

    printf("msg: version=%"PRIu16", type=%"PRIu16", size=%"PRIu32"\n",
           msg->header.version,
           msg->header.type,
           msg->header.size);

    switch (msg->header.type) {
    case MXL_DIAG_MSG_VIDEO_READ:
        printf("video_read: timestamp = %"PRIu64
               ", exec_dur = %"PRIu64
               ", tail_index = %"PRIu64
               ", head_index = %"PRIu64", read_index = %"PRIu64
               ", mxl_status = %"PRIu32"\n",
               msg->u.video_read.timestamp,
               msg->u.video_read.exec_dur,
               msg->u.video_read.tail_index,
               msg->u.video_read.head_index,
               msg->u.video_read.read_index,
               msg->u.video_read.mxl_status);
        break;
    case MXL_DIAG_MSG_AUDIO_READ:
        printf("audio_read: timestamp = %"PRIu64
               ", exec_dur = %"PRIu64
               ", tail_index = %"PRIu64
               ", head_index = %"PRIu64", read_index = %"PRIu64
               ", read_size = %"PRIu32", mxl_status = %"PRIu32"\n",
               msg->u.audio_read.timestamp,
               msg->u.audio_read.exec_dur,
               msg->u.audio_read.tail_index,
               msg->u.audio_read.head_index,
               msg->u.audio_read.read_index,
               msg->u.audio_read.read_size,
               msg->u.audio_read.mxl_status);
        break;
    default:
        fprintf(stderr, "payload=<unknown type>\n");
        av_assert0(false);
        break;
    }
}

// retry connection every millisecond for 1/FPS seconds
static int test_client_connect(mxl_diag_client* client,
                               const char* server_path,
                               const char* client_path)
{
    int rc = -1;
    int retry = 0;
    do {
        rc = mxl_diag_client_connect(client, server_path, client_path);
        if (rc)
            usleep(1000);
    }
    while(rc && retry++ < 1000/FPS);

    return rc;
}

static int test_client_print_messages(const char *server_path)
{
    printf("\ntest: print message stream\n");

    int exit_status = AVERROR_UNKNOWN;

    const char *client_path = TEST_DIR "/test.client.sock";

    mxl_diag_client client = {-1, ""};
    mxl_diag_msg msg;

    const int max_messages = 10;

    int rc = test_client_connect(&client, server_path, client_path);
    if (rc) {
        fprintf(stderr, "client connection failed\n");
        exit_status = rc;
        goto finally;
    }

    for (int recv_count = 0; recv_count < max_messages; recv_count++) {
        rc = mxl_diag_client_recv(&client, &msg);
        if (rc) {
            exit_status = rc;
            goto finally;
        }
        print_message(&msg);
    }

    rc = mxl_diag_client_close(&client);
    if (rc) {
        fprintf(stderr, "client close failed\n");
        exit_status = rc;
        goto finally;
    }

    exit_status = 0;

finally:

    if (client.fd >= 0) {
        rc = mxl_diag_client_close(&client);
        av_assert0(rc == 0);
    }


    return exit_status;
}


static int test_client_multi_connect(const char* server_path)
{
    printf("\ntest: multiple connect attempts are ignored after the first\n");

    const char *client_path_a = TEST_DIR "/test.a.client.sock";
    const char *client_path_b = TEST_DIR "/test.b.client.sock";
    const char *client_path_c = TEST_DIR "/test.c.client.sock";

    // client A should connect and receive a message
    mxl_diag_client client_a;
    int rc = test_client_connect(&client_a, server_path, client_path_a);
    if (rc) {
        fprintf(stderr, "client A connection failed (%d)\n", rc);
        return rc;
    }
    printf("client A connected\n");
    mxl_diag_msg msg_a_1 = {0};
    rc = mxl_diag_client_recv(&client_a, &msg_a_1);
    if (rc) {
        fprintf(stderr, "client A first recv failed (%d)\n", rc);
        return rc;
    }
    print_message(&msg_a_1);

    // reconnect is harmless
    {
        mxl_diag_msg msg = {0};

        msg.header.version = MXL_DIAG_MSG_VERSION;
        msg.header.type = MXL_DIAG_MSG_CONNECT;
        msg.header.size = sizeof(msg.header);

        if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
            rc = AVERROR(errno);
            fprintf(stderr, "client A re-connect failed (%d)\n", rc);
            return rc;
        }
    }

    // client B should connect and be ignored
    mxl_diag_client client_b;
    rc = test_client_connect(&client_b, server_path, client_path_b);
    if (rc) {
        fprintf(stderr, "client B connection failed (%d)\n", rc);
        return rc;
    }
    mxl_diag_msg msg_b_1 = {0};
    rc = mxl_diag_client_recv(&client_b, &msg_b_1);
    if (rc != AVERROR(ETIMEDOUT)) {
        fprintf(stderr, "client B recv failed to timeout (%d)\n", rc);
        return rc;
    }
    printf("client B timed out (expected)\n");
    // releasing b without closing should not affect a
    rc = mxl_diag_client_release(&client_b);
    if (rc) {
        fprintf(stderr, "client B release failed");
        return rc;
    }

    // client A should still work
    mxl_diag_msg msg_a_2 = {0};
    rc = mxl_diag_client_recv(&client_a, &msg_a_2);
    if (rc) {
        fprintf(stderr, "client A second recv failed (%d)\n", rc);
        return rc;
    }
    print_message(&msg_a_2);
    printf("client A was unaffected by client B\n");

    // client C should connect and be ignored
    mxl_diag_client client_c;
    rc = test_client_connect(&client_c, server_path, client_path_c);
    if (rc) {
        fprintf(stderr, "client C connection failed (%d)\n", rc);
        return rc;
    }
    mxl_diag_msg msg_c = {0};
    rc = mxl_diag_client_recv(&client_c, &msg_c);
    if (rc != AVERROR(ETIMEDOUT)) {
        fprintf(stderr, "client C recv failed to timeout (%d)\n", rc);
        return rc;
    }
    printf("client C timed out (expected)\n");

    // closing C not affect A
    rc = mxl_diag_client_close(&client_c);
    if (rc) {
        fprintf(stderr, "client C close failed");
        return rc;
    }

    // client A should still work
    mxl_diag_msg msg_a_3 = {0};
    rc = mxl_diag_client_recv(&client_a, &msg_a_3);
    if (rc) {
        fprintf(stderr, "client A second recv failed (%d)\n", rc);
        return rc;
    }
    print_message(&msg_a_3);
    printf("client A was unaffected by client C\n");

    // client B should still timeout
    mxl_diag_msg msg_b_2 = {0};
    rc = mxl_diag_client_recv(&client_b, &msg_b_2);
    if (rc != AVERROR(ETIMEDOUT)) {
        fprintf(stderr, "client B recv failed to timeout (%d)\n", rc);
        return rc;
    }
    printf("client B still times out\n");
    rc = mxl_diag_client_close(&client_b);
    av_assert0(rc == 0);

    rc = mxl_diag_client_close(&client_a);
    av_assert0(rc == 0);

    return 0;
}

static int test_client_malformed(const char *server_path)
{
    const int malformed_case_count = 7;
    const int n = malformed_case_count * 2;

    const char *client_path_a = TEST_DIR "/test.malformed.a.client.sock";

    mxl_diag_client client_a;
    int rc;
    int i;

    printf("\ntest: malformed messages from active client are ignored\n");

    rc = test_client_connect(&client_a, server_path, client_path_a);
    if (rc) {
        fprintf(stderr, "client A connection failed (%d)\n", rc);
        return rc;
    }

    // Confirm A is active.
    {
        mxl_diag_msg msg = {0};
        rc = mxl_diag_client_recv(&client_a, &msg);
        if (rc) {
            fprintf(stderr, "client A initial recv failed (%d)\n", rc);
            mxl_diag_client_close(&client_a);
            return rc;
        }
        print_message(&msg);
    }

    for (i = 0; i < n; ++i) {
        mxl_diag_msg msg;
        const void *buf;
        size_t len;

        memset(&msg, 0, sizeof(msg));

        switch (i %  malformed_case_count) {
        case 0:
            printf("bad size, message too short\n");
            msg.header.version = MXL_DIAG_MSG_VERSION;
            msg.header.type = MXL_DIAG_MSG_CONNECT;
            msg.header.size = sizeof(msg.header);
            buf = &msg;
            len = sizeof(msg.header)-1;
            break;

        case 1:
            printf("bad size, header size sanity (too small)\n");
            msg.header.version = MXL_DIAG_MSG_VERSION;
            msg.header.type = MXL_DIAG_MSG_RELEASE;
            msg.header.size = sizeof(msg.header) - 1;
            buf = &msg;
            len = sizeof(msg.header);
            break;

        case 2:
            printf("bad size, header size sanity (too big)\n");
            msg.header.version = MXL_DIAG_MSG_VERSION;
            msg.header.type = MXL_DIAG_MSG_RELEASE;
            msg.header.size = sizeof(msg)+1;
            buf = &msg;
            len = sizeof(msg.header);
            break;

        case 3: {
            // this will appear as a "message too short" error in the log
            printf("unknown type, truncated header\n");
            static const unsigned char short_buf[sizeof(msg.header) - 1] = {0};
            buf = short_buf;
            len = sizeof(short_buf);
            break;
        }

        case 4:
            printf("bad version\n");
            msg.header.version = MXL_DIAG_MSG_VERSION + 1;
            msg.header.type = MXL_DIAG_MSG_CONNECT;
            msg.header.size = sizeof(msg.header);
            buf = &msg;
            len = msg.header.size;
            break;

        case 5:
            printf("unknown type\n");
            msg.header.version = MXL_DIAG_MSG_VERSION;
            msg.header.type = 9999;
            msg.header.size = sizeof(msg.header);
            buf = &msg;
            len = msg.header.size;
            break;

        case 6: {
            printf("garbage message\n");
            unsigned char garbage[128];
            size_t j;

            len = 1 + (size_t)(rand() % sizeof(garbage));
            for (j = 0; j < len; ++j) {
                garbage[j] = (unsigned char)(rand() & 0xff);
            }

            buf = garbage;
            break;
        }

        default:
            av_assert0(false);
        }

        if (send(client_a.fd, buf, len, 0) < 0) {
            rc = AVERROR(errno);
            fprintf(stderr, "client A malformed send %d failed (%d)\n", i, rc);
            mxl_diag_client_close(&client_a);
            return rc;
        }
    }

    printf("sent %d malformed messages from active client A\n", n);

    // give the test server time to catch up
    sleep(n/FPS + 1);

    // A should still receive normal messages afterward.
    {
        mxl_diag_msg msg = {0};
        rc = mxl_diag_client_recv(&client_a, &msg);
        if (rc) {
            fprintf(stderr, "client A recv after malformed traffic failed (%d)\n", rc);
            mxl_diag_client_close(&client_a);
            return rc;
        }
        print_message(&msg);
        printf("client A unaffected by malformed traffic\n");
    }

    rc = mxl_diag_client_close(&client_a);
    if (rc) {
        fprintf(stderr, "client A close failed (%d)\n", rc);
        return rc;
    }

    return 0;
}

static void test_server_init_msg(mxl_diag_msg *msg, uint64_t base_index)
{
    if (!msg) {
        return;
    }

    if (base_index%2) {
        mxl_diag_init_msg_video_read(
            msg, base_index, base_index + 1, base_index + 2, base_index + 3, base_index + 4,
            MXL_STATUS_OK);
    }
    else {
        mxl_diag_init_msg_audio_read(
            msg, base_index, base_index + 1, base_index + 2, base_index + 3, base_index + 4,
            48000/FPS, MXL_STATUS_OK);
    }
}

typedef struct server_thread {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool stop;
    bool started;
    const char* server_path;
} server_thread;

// send one message each simulated frame period
static void *test_server_thread_main(void *arg)
{
    server_thread *t = arg;

    mxl_diag_server server = {0};
    int rc = mxl_diag_init(NULL, &server, t->server_path);
    if (rc) {
        fprintf(stderr, "mxl_diag_init failed (%d)\n", rc);
        return NULL;
    }

    static uint64_t base_index = 0;
    bool stop = false;
    do {
        usleep(1000000/FPS);
        mxl_diag_msg msg = {0};
        test_server_init_msg(&msg, base_index++);

        rc = mxl_diag_poll(NULL, &server);
        if (rc < 0)
            fprintf(stderr, "test_server_thread_main mxl_diag_poll error %d\n", rc);

        rc = mxl_diag_send(NULL, &server, &msg);
        if (rc < 0)
            fprintf(stderr, "test_server_thread_main mxl_diag_send error %d\n", rc);

        pthread_mutex_lock(&t->mutex);
        stop = t->stop;
        pthread_mutex_unlock(&t->mutex);
    } while (!stop);

    mxl_diag_close(NULL, &server);

    return NULL;
}


static int test_server_thread_start(server_thread *t,
                                    const char* server_path)
{
    av_assert0(t && server_path);

    int exit_status = AVERROR_UNKNOWN;

    t->stop = false;
    t->started = false;
    t->server_path = server_path;
    t->thread = 0;
    pthread_mutex_init(&t->mutex, NULL);

    int rc = pthread_create(&t->thread, NULL, test_server_thread_main, t);
    if (rc != 0) {
        exit_status = AVERROR(rc);
        goto finally;
    }

    t->started = true;
    exit_status = 0;

finally:

    return exit_status;
}

static int test_server_thread_stop(server_thread *t)
{
    av_assert0(t && t->started);

    pthread_mutex_lock(&t->mutex);
    t->stop = true;
    pthread_mutex_unlock(&t->mutex);

    int rc = pthread_join(t->thread, NULL);
    if (rc) {
        fprintf(stderr, "pthread_join error (%d)\n", rc);
        return AVERROR(rc);
    }

    pthread_mutex_destroy(&t->mutex);

    t->started = false;

    return 0;
}

/*
 * synchronous tests
 */
static int test_server_path_collision_non_socket(const char *server_path)
{
    mxl_diag_server server;
    FILE *fp;
    int rc;

    printf("\ntest: init fails if server path exists as a non-socket\n");

    unlink(server_path);

    fp = fopen(server_path, "w");
    if (!fp) {
        rc = AVERROR(errno);
        fprintf(stderr, "fopen failed (%d)\n", rc);
        return rc;
    }
    fclose(fp);

    rc = mxl_diag_init(NULL, &server, server_path);
    if (rc != AVERROR(EEXIST)) {
        fprintf(stderr, "expected EEXIST, got (%d)\n", rc);
        unlink(server_path);
        return rc ? rc : AVERROR(EINVAL);
    }

    if (access(server_path, F_OK) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "server path sanity failed (%d)\n", rc);
        return rc;
    }

    unlink(server_path);

    return 0;
}

static int test_server_path_stale_socket_cleanup(const char *server_path)
{
    mxl_diag_server server;
    int stale_fd = -1;
    struct sockaddr_un addr;
    int rc;

    printf("\ntest: init cleans up stale socket path\n");

    unlink(server_path);

    if (strlen(server_path) >= sizeof(addr.sun_path)) {
        return AVERROR(ENAMETOOLONG);
    }

    stale_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (stale_fd < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "socket failed (%d)\n", rc);
        return rc;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, server_path);

    if (bind(stale_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "bind stale socket failed (%d)\n", rc);
        close(stale_fd);
        return rc;
    }

    close(stale_fd);
    stale_fd = -1;

    rc = mxl_diag_init(NULL, &server, server_path);
    if (rc) {
        fprintf(stderr, "mxl_diag_init failed (%d)\n", rc);
        unlink(server_path);
        return rc;
    }

    if (access(server_path, F_OK) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "server path missing after init (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    mxl_diag_close(NULL, &server);

    if (access(server_path, F_OK) == 0 || errno != ENOENT) {
        fprintf(stderr, "server path still exists after close\n");
        unlink(server_path);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int test_release_with_no_active_client(const char *server_path)
{
    const char *client_path_a = TEST_DIR "/test.no_active_release.a.client";

    mxl_diag_server server;
    mxl_diag_client client_a;
    struct sockaddr_un local_addr;
    struct sockaddr_un server_addr;
    mxl_diag_msg msg;
    int rc;

    printf("\ntest: release with no active client is ignored\n");

    rc = mxl_diag_init(NULL, &server, server_path);
    if (rc) {
        fprintf(stderr, "server init failed (%d)\n", rc);
        return rc;
    }

    memset(&client_a, 0, sizeof(client_a));
    client_a.fd = -1;

    if (strlen(server_path) >= sizeof(server_addr.sun_path) ||
        strlen(client_path_a) >= sizeof(local_addr.sun_path)) {
        mxl_diag_close(NULL, &server);
        return AVERROR(ENAMETOOLONG);
    }

    client_a.fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (client_a.fd < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "client socket failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path, client_path_a);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, server_path);

    strncpy(client_a.client_path, client_path_a, sizeof(client_a.client_path) - 1);
    client_a.client_path[sizeof(client_a.client_path) - 1] = '\0';

    unlink(client_path_a);

    if (bind(client_a.fd, (const struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "client bind failed (%d)\n", rc);
        close(client_a.fd);
        client_a.fd = -1;
        unlink(client_path_a);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    if (connect(client_a.fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "client connect failed (%d)\n", rc);
        close(client_a.fd);
        client_a.fd = -1;
        unlink(client_path_a);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    /* aberrant condition under test: sending release to idle server */
    av_assert0(!server.has_client);
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_RELEASE;
    msg.header.size = sizeof(msg.header);
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "client release send failed (%d)\n", rc);
        mxl_diag_client_close(&client_a);
        mxl_diag_close(NULL, &server);
        return rc;
    }
    mxl_diag_poll(NULL, &server);
    if (server.has_client) {
        fprintf(stderr, "server unexpectedly activated after idle release\n");
        mxl_diag_client_close(&client_a);
        mxl_diag_close(NULL, &server);
        return AVERROR(EINVAL);
    }

    // connect should work as expected after the aberrant release send
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_CONNECT;
    msg.header.size = sizeof(msg.header);
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "client connect send failed (%d)\n", rc);
        mxl_diag_client_close(&client_a);
        mxl_diag_close(NULL, &server);
        return rc;
    }
    mxl_diag_poll(NULL, &server);
    if (!server.has_client) {
        fprintf(stderr, "server failed to activate after valid connect\n");
        mxl_diag_client_close(&client_a);
        mxl_diag_close(NULL, &server);
        return AVERROR(EINVAL);
    }

    // redundant, but should pass
    rc = mxl_diag_client_close(&client_a);
    if (rc) {
        fprintf(stderr, "client close failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    mxl_diag_close(NULL, &server);
    return 0;
}


static int test_connect_after_junk(const char *server_path)
{
    const char *junk_path = TEST_DIR "/test.connect_after_junk.junk.sock";
    const char *client_path_a = TEST_DIR "/test.connect_after_junk.a.client.sock";

    mxl_diag_server server;
    mxl_diag_client client_a;
    struct sockaddr_un junk_addr;
    struct sockaddr_un server_addr;
    mxl_diag_msg msg;
    int junk_fd = -1;
    int rc;

    printf("\ntest: valid connect works after junk from unrelated socket\n");

    rc = mxl_diag_init(NULL, &server, server_path);
    if (rc) {
        fprintf(stderr, "server init failed (%d)\n", rc);
        return rc;
    }

    if (strlen(server_path) >= sizeof(server_addr.sun_path) ||
        strlen(junk_path) >= sizeof(junk_addr.sun_path)) {
        mxl_diag_close(NULL, &server);
        return AVERROR(ENAMETOOLONG);
    }

    junk_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (junk_fd < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "junk socket failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    memset(&junk_addr, 0, sizeof(junk_addr));
    junk_addr.sun_family = AF_UNIX;
    strcpy(junk_addr.sun_path, junk_path);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strcpy(server_addr.sun_path, server_path);

    unlink(junk_path);

    if (bind(junk_fd, (const struct sockaddr *)&junk_addr, sizeof(junk_addr)) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "junk bind failed (%d)\n", rc);
        close(junk_fd);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    if (connect(junk_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "junk connect failed (%d)\n", rc);
        close(junk_fd);
        unlink(junk_path);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    /* Send one junk message to the idle server (version too high). */
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION + 1;
    msg.header.type = MXL_DIAG_MSG_CONNECT;
    msg.header.size = sizeof(msg.header);

    if (send(junk_fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "junk send failed (%d)\n", rc);
        close(junk_fd);
        unlink(junk_path);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    mxl_diag_poll(NULL, &server);

    if (server.has_client) {
        fprintf(stderr, "server unexpectedly activated after junk message\n");
        close(junk_fd);
        unlink(junk_path);
        mxl_diag_close(NULL, &server);
        return AVERROR(EINVAL);
    }

    close(junk_fd);
    unlink(junk_path);

    /* Normal client connect should still work afterward. */
    rc = test_client_connect(&client_a, server_path, client_path_a);
    if (rc) {
        fprintf(stderr, "client A connection failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    mxl_diag_poll(NULL, &server);

    if (!server.has_client) {
        fprintf(stderr, "server failed to activate after valid connect\n");
        mxl_diag_client_close(&client_a);
        mxl_diag_close(NULL, &server);
        return AVERROR(EINVAL);
    }

    rc = mxl_diag_client_close(&client_a);
    if (rc) {
        fprintf(stderr, "client A close failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    mxl_diag_close(NULL, &server);

    return 0;
}

static int test_poll_malformed_return_codes(const char *server_path)
{
    const char *client_path_a = TEST_DIR "/test.poll_malformed.a.client.sock";

    mxl_diag_server server;
    mxl_diag_client client_a;
    mxl_diag_msg msg;
    int rc;

    printf("\ntest: mxl_diag_poll returns expected codes for malformed messages\n");

    rc = mxl_diag_init(NULL, &server, server_path);
    if (rc) {
        fprintf(stderr, "server init failed (%d)\n", rc);
        return rc;
    }

    rc = test_client_connect(&client_a, server_path, client_path_a);
    if (rc) {
        fprintf(stderr, "client A connection failed (%d)\n", rc);
        mxl_diag_close(NULL, &server);
        return rc;
    }

    /*
     * Drain the initial valid CONNECT sent by test_client_connect(),
     * so the following poll calls correspond exactly to the malformed
     * messages below.
     */
    rc = mxl_diag_poll(NULL, &server);
    if (rc != 1) {
        fprintf(stderr, "initial connect poll returned (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("bad size, datagram smaller than sizeof(msg.header)\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_CONNECT;
    msg.header.size = sizeof(msg.header);
    if (send(client_a.fd, &msg, sizeof(msg.header) - 1, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EDATATOOSMALL)) {
        fprintf(stderr, "unexpected rc for 'datagram smaller than sizeof(msg.header)' (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("bad size, header.size smaller than sizeof(msg.header)\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_RELEASE;
    msg.header.size = sizeof(msg.header)-1;
    if (send(client_a.fd, &msg, sizeof(msg.header), 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_ESIZETOOSMALL)) {
        fprintf(stderr, "unexpected rc for 'header.size smaller than sizeof(msg.header)' (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("bad size, header.size bigger than sizeof(msg)\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_RELEASE;
    msg.header.size = sizeof(msg) + 1;
    if (send(client_a.fd, &msg, sizeof(msg.header), 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_ESIZETOOBIG)) {
        fprintf(stderr, "unexpected rc for 'header.size bigger than sizeof(msg)' (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("bad size, datagram size vs header.size mismatch\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_RELEASE;
    msg.header.size = sizeof(msg.header) + 1;
    if (send(client_a.fd, &msg, sizeof(msg.header), 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_ESIZEMISMATCH)) {
        fprintf(stderr, "unexpected rc for 'datagram size vs header.size mismatch' (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("bad size, truncated datagram\n");
    char buf[sizeof(mxl_diag_msg) + 1];
    mxl_diag_msg *truncated_msg = (mxl_diag_msg *)buf;
    memset(truncated_msg, 0, sizeof(msg));
    truncated_msg->header.version = MXL_DIAG_MSG_VERSION;
    truncated_msg->header.type = MXL_DIAG_MSG_RELEASE;
    truncated_msg->header.size = sizeof(*truncated_msg);
    if (send(client_a.fd, truncated_msg, sizeof(buf), 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EDATATRUNCATED)) {
        fprintf(stderr, "unexpected rc for 'truncated datagram' (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    // bad version
    printf("bad version\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION + 1;
    msg.header.type = MXL_DIAG_MSG_CONNECT;
    msg.header.size = sizeof(msg.header);
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EBADVERSION)) {
        fprintf(stderr, "unexpected rc for bad version (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    // unknown type
    printf("unknown type\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = 9999;
    msg.header.size = sizeof(msg.header);
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EUNKNOWN)) {
        fprintf(stderr, "unexpected rc for unknown type (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    printf("truncated header\n");
    {
        static const unsigned char short_buf[sizeof(msg.header) - 1] = {0};
        if (send(client_a.fd, short_buf, sizeof(short_buf), 0) < 0) {
            rc = AVERROR(errno);
            fprintf(stderr, "send failed (%d)\n", rc);
            goto finally;
        }
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EDATATOOSMALL)) {
        fprintf(stderr, "unexpected rc for truncated header (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    // more data than necessary for connect message
    printf("connect message size mismatch\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_CONNECT;
    msg.header.size = sizeof(msg.header)+1;
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EMSGSIZEMISMATCH)) {
        fprintf(stderr, "unexpected rc for connect message size mismatch (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }

    // more data than necessary for release message
    printf("release message size mismatch\n");
    memset(&msg, 0, sizeof(msg));
    msg.header.version = MXL_DIAG_MSG_VERSION;
    msg.header.type = MXL_DIAG_MSG_RELEASE;
    msg.header.size = sizeof(msg.header)+1;
    if (send(client_a.fd, &msg, msg.header.size, 0) < 0) {
        rc = AVERROR(errno);
        fprintf(stderr, "send failed (%d)\n", rc);
        goto finally;
    }
    rc = mxl_diag_poll(NULL, &server);
    if (rc != AVERROR(MXL_DIAG_EMSGSIZEMISMATCH)) {
        fprintf(stderr, "unexpected rc for release message size mismatch (%d)\n", rc);
        rc = AVERROR(EINVAL);
        goto finally;
    }




    rc = 0;

finally:

    int close_rc = mxl_diag_client_close(&client_a);
    if (rc == 0 && close_rc) {
        rc = close_rc;
    }

    mxl_diag_close(NULL, &server);

    return rc;
}

static int run_synchronous_tests(void)
{
    int rc = -1;
    const char* server_path = TEST_DIR "/sync.server.sock";

    rc = test_server_path_collision_non_socket(server_path);
    if (rc) {
        fprintf(stderr, "test_server_path_collision_non_socket failed (%d)\n", rc);
        return rc;
    }

    rc = test_server_path_stale_socket_cleanup(server_path);
    if (rc) {
        fprintf(stderr, "test_server_path_stale_socket_cleanup (%d)\n", rc);
        return rc;
    }

    rc = test_release_with_no_active_client(server_path);
    if (rc) {
        fprintf(stderr, "test_release_with_no_active_client (%d)\n", rc);
        return rc;
    }

    rc = test_connect_after_junk(server_path);
    if (rc) {
        fprintf(stderr, "test_connect_after_junk (%d)\n", rc);
        return rc;
    }

    rc = test_poll_malformed_return_codes(server_path);
    if (rc) {
        fprintf(stderr, "test_poll_malformed_return_codes (%d)\n", rc);
        return rc;
    }

    return check_open_fds_match_startup();
}

static int run_asynchronous_tests(void)
{
    int rc = -1;
    const char* server_path = TEST_DIR "/async.server.sock";

    server_thread instr_thread = {0};
    rc = test_server_thread_start(&instr_thread, server_path);
    if (rc) {
        fprintf(stderr, "test server thread start failed (%d)\n", rc);
        return rc;
    }

    rc = test_client_print_messages(server_path);
    if (rc)
        return rc;

    rc = test_client_multi_connect(server_path);
    if (rc)
        return rc;

    rc = test_client_malformed(server_path);
    if (rc)
        return rc;

    rc = test_server_thread_stop(&instr_thread);
    if (rc)
        return rc;

    unlink(server_path);

    return check_open_fds_match_startup();
}

int main(void)
{
    printf("testing mxl_diag: mxl diagnostic server\n\n");

    capture_startup_open_fds();

    av_log_set_level(AV_LOG_QUIET);

    int rc = -1;

    if (mkdir(TEST_DIR, 0777) < 0 && errno != EEXIST) {
        return AVERROR(errno);
    }

    rc = run_synchronous_tests();
    if (rc) {
        fprintf(stderr, "synchronous tests failed\n");
        goto finally;
    }

    rc = run_asynchronous_tests();
    if (rc) {
        fprintf(stderr, "asynchronous tests failed\n");
        goto finally;
    }

    rmdir(TEST_DIR);

finally:

    if (!rc)
        printf("\nPASS\n");
    else
        printf("\nFAIL\n");

    return rc;
}
