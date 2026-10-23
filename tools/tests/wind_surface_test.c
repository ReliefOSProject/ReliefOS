#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"
#include <stdlib.h>
#include <sys/un.h>

#include "../../userland/runtime/src/wind.c"

int reliefos_ipc_flush(int fd) { (void)fd; return 0; }

static int backing_fd;
static int reply_fd = -1;
static int allow_fetch = 1;
static int destroy_before_reply;
static unsigned fetches;
static struct reliefos_win_fetch_ack reply = {
    .window_id = 7, .width = 2, .height = 2, .stride = 12};
static uint32_t reply_length = sizeof(reply);

int reliefos_ipc_connect(const char *path) { (void)path; assert(0); return -1; }
int reliefos_ipc_set_nonblock(int fd, int enabled) { (void)fd; (void)enabled; return 0; }
int reliefos_ipc_close(int fd) { return close(fd); }

int reliefos_ipc_send(int fd, uint32_t type, const void *payload, uint32_t length)
{
    assert(fd == wind_policy_fd && type == RELIEFOS_WIN_MSG_FETCH);
    assert(length == sizeof(struct reliefos_win_fetch));
    assert(((const struct reliefos_win_fetch *)payload)->window_id != 0);
    ++fetches;
    if (!allow_fetch) { errno = EAGAIN; return -1; }
    return 0;
}

int reliefos_ipc_recv_fd(int fd, uint32_t *type, void *payload, uint32_t capacity,
                      uint32_t *length, int *received_fd)
{
    assert(fd == wind_policy_fd && received_fd);
    if (destroy_before_reply) {
        struct reliefos_gui_window_msg message = {.type = 3, .window_id = reply.window_id};
        assert(capacity >= sizeof(message));
        memcpy(payload, &message, sizeof(message));
        *type = RELIEFOS_WIN_MSG_WINDOW_NOTIFY;
        *length = sizeof(message);
        *received_fd = -1;
        destroy_before_reply = 0;
        return 0;
    }
    assert(capacity >= sizeof(reply));
    memcpy(payload, &reply, sizeof(reply));
    *type = RELIEFOS_WIN_MSG_FETCH_ACK;
    *length = reply_length;
    *received_fd = reply_fd = dup(backing_fd);
    assert(reply_fd >= 0);
    return 0;
}

static void notify(uint32_t type, uint32_t id)
{
    struct reliefos_gui_window_msg message = {.type = type, .window_id = id};
    wind_route_frame(RELIEFOS_WIN_MSG_WINDOW_NOTIFY, (const uint8_t *)&message,
                     sizeof(message));
}

static void assert_reply_closed(void)
{
    assert(fcntl(reply_fd, F_GETFD) < 0 && errno == EBADF);
}

int main(void)
{
    char path[256];
    host_tmp_path(path, sizeof(path), "surface-XXXXXX");
    backing_fd = mkstemp(path);
    assert(backing_fd >= 0);
    assert(unlink(path) == 0 && ftruncate(backing_fd, 4096) == 0);
    const uint32_t first[] = {11, 12, 0, 13, 14, 0};
    assert(pwrite(backing_fd, first, sizeof(first), 0) == sizeof(first));
    wind_policy_fd = 1000;
    uint32_t pixels[8] = {0}, width, height;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, &width, &height) == 1);
    assert(width == 2 && height == 2);
    assert(pixels[0] == 11 && pixels[1] == 12 && pixels[2] == 0 &&
           pixels[4] == 13 && pixels[5] == 14 && pixels[6] == 0);

    /* Once acquired, repainting must work even when windowd cannot service
     * another RPC. Mutating the actual shared file must still change pixels. */
    allow_fetch = 0;
    for (unsigned frame = 0; frame < 100; ++frame) {
        const uint32_t next[] = {21 + frame, 22, 0, 23, 24, 0};
        assert(pwrite(backing_fd, next, sizeof(next), 0) == sizeof(next));
        notify(2, 7);
        notify(4, 7);
        assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, &width, &height) == 1);
        assert(pixels[0] == 21 + frame && pixels[1] == 22 &&
               pixels[4] == 23 && pixels[5] == 24);
    }
    assert(fetches == 1);
    assert(fcntl(reply_fd, F_GETFD) >= 0); /* ReliefOS SHM lifetime is FD-owned. */
    assert(fcntl(reply_fd, F_GETFD) & FD_CLOEXEC);
    memset(pixels, 0, sizeof(pixels));
    assert(reliefos_gui_fetch_window(7, 2, 2, 1, pixels, 0, 0) == 1);
    assert(pixels[0] == 120 && pixels[1] == 23 && pixels[2] == 0);
    notify(3, 7);
    assert_reply_closed();

    allow_fetch = 1;
    reply.width = reply.height = 1;
    reply.stride = 4;
    notify(1, 7);
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, &width, &height) == 1);
    assert(width == 1 && height == 1 && fetches == 2);
    notify(3, 7);
    assert_reply_closed();

    destroy_before_reply = 1;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, 0, 0) < 0);
    assert(errno == ENOENT);
    assert_reply_closed();

    reply_length = sizeof(reply) - 1;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, 0, 0) < 0 && errno == EPROTO);
    assert_reply_closed();
    reply_length = sizeof(reply);
    reply.window_id = 8;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, 0, 0) < 0 && errno == EPROTO);
    assert_reply_closed();
    reply.window_id = 7;
    reply.stride = 1;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, 0, 0) < 0 && errno == EPROTO);
    assert_reply_closed();
    reply.stride = 4;
    assert(reliefos_gui_fetch_window(7, 2, 2, 4, pixels, 0, 0) == 1);
    notify(3, 7);
    assert_reply_closed();

    /* Missing lifecycle messages must not exhaust a bounded cache forever. */
    for (unsigned id = 1; id <= 40; ++id) {
        reply.window_id = id;
        assert(reliefos_gui_fetch_window(id, 2, 2, 4, pixels, 0, 0) == 1);
    }
    for (unsigned id = 1; id <= 40; ++id) notify(3, id);
    assert_reply_closed();
    close(backing_fd);
    puts("Window surface tests passed: 101 fresh frames with one FETCH; lifecycle releases SHM");
    return 0;
}
