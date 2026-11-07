#include "watch.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define FILEMAN_WATCH_CHANGES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | \
                               IN_ATTRIB | IN_CLOSE_WRITE)
#define FILEMAN_WATCH_END (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)

static int remove_watch(int fd, int wd)
{
    int result;
    do { result = inotify_rm_watch(fd, wd); } while (result < 0 && errno == EINTR);
    /* A deleted/unmounted inode may already have removed the watch. */
    return result < 0 && errno != EINVAL ? -EIO : 0;
}

static int discard_old_events(int fd)
{
    union {
        struct inotify_event alignment;
        unsigned char bytes[4096];
    } buffer;
    for (;;) {
        ssize_t length = read(fd, buffer.bytes, sizeof(buffer.bytes));
        if (length > 0) continue;
        if (length < 0 && errno == EINTR) continue;
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -EIO;
    }
}

int fileman_watch_open(struct fileman_watch *watch)
{
    watch->fd = -1;
    watch->wd = -1;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -errno;
    watch->fd = fd;
    return 0;
}

int fileman_watch_set(struct fileman_watch *watch, const char *path)
{
    if (watch->fd < 0) return -ENODEV;
    int old = watch->wd;
    watch->wd = -1;
    if (old >= 0 && remove_watch(watch->fd, old) < 0) return -EIO;
    /* Throw away the previous directory's queued events, including IN_IGNORED,
     * before a new watch descriptor can be installed. No watch remains here,
     * so unlike event-loop reads the finite old queue can be drained fully. */
    if (discard_old_events(watch->fd) < 0) return -EIO;
    int wd = inotify_add_watch(watch->fd, path, FILEMAN_WATCH_CHANGES |
                               IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
    if (wd < 0) return -errno;
    watch->wd = wd;
    return 0;
}

unsigned fileman_watch_consume(const struct fileman_watch *watch, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    unsigned result = 0;
    while (size) {
        struct inotify_event event;
        if (size < sizeof(event)) return result | FILEMAN_WATCH_ERROR;
        memcpy(&event, bytes, sizeof(event));
        if (event.len > size - sizeof(event)) return result | FILEMAN_WATCH_ERROR;
        if (event.mask & IN_Q_OVERFLOW) {
            result |= FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_OVERFLOW;
        } else if (watch->wd >= 0 && event.wd == watch->wd) {
            if (event.mask & FILEMAN_WATCH_CHANGES) result |= FILEMAN_WATCH_CHANGED;
            if (event.mask & FILEMAN_WATCH_END) result |= FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_LOST;
        }
        size_t record_size = sizeof(event) + event.len;
        bytes += record_size;
        size -= record_size;
    }
    return result;
}

unsigned fileman_watch_drain(struct fileman_watch *watch)
{
    union {
        struct inotify_event alignment;
        unsigned char bytes[4096];
    } buffer;
    unsigned result = 0;
    if (watch->fd < 0) return FILEMAN_WATCH_ERROR;
    /* Yield back to FLTK even if another process is generating events
     * continuously. The descriptor remains readable if more are queued. */
    for (unsigned reads = 0; reads < 16; ++reads) {
        ssize_t length = read(watch->fd, buffer.bytes, sizeof(buffer.bytes));
        if (length > 0) {
            result |= fileman_watch_consume(watch, buffer.bytes, (size_t)length);
            continue;
        }
        if (length < 0 && errno == EINTR) continue;
        if (length == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            result |= FILEMAN_WATCH_ERROR;
        break;
    }
    if ((result & FILEMAN_WATCH_LOST) && watch->wd >= 0) {
        int old = watch->wd;
        watch->wd = -1;
        /* IN_MOVE_SELF does not remove the kernel watch by itself. */
        if (remove_watch(watch->fd, old) < 0) result |= FILEMAN_WATCH_ERROR;
    }
    return result;
}

void fileman_watch_close(struct fileman_watch *watch)
{
    if (watch->fd >= 0) close(watch->fd);
    watch->fd = -1;
    watch->wd = -1;
}
