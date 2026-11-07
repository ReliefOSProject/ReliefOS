#define _GNU_SOURCE
#include "watch.h"
#include "selection.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch_file(const char *name)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    assert(close(fd) == 0);
}

static int ready(int fd, int timeout)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    int result;
    do { result = poll(&pfd, 1, timeout); } while (result < 0 && errno == EINTR);
    assert(result >= 0);
    return result && (pfd.revents & POLLIN);
}

static void expect_changes(struct fileman_watch *watch)
{
    assert(ready(watch->fd, 1000));
    assert(fileman_watch_drain(watch) & FILEMAN_WATCH_CHANGED);
    assert(fileman_watch_drain(watch) == 0);
}

static void test_events(void)
{
    struct fileman_watch watch;
    assert(fileman_watch_open(&watch) == 0);
    assert(fcntl(watch.fd, F_GETFL) & O_NONBLOCK);
    assert(fcntl(watch.fd, F_GETFD) & FD_CLOEXEC);
    assert(mkdir("first", 0700) == 0);
    assert(mkdir("second", 0700) == 0);
    assert(fileman_watch_set(&watch, "first") == 0);

    int dir = open("first", O_RDONLY | O_DIRECTORY);
    assert(dir >= 0 && close(dir) == 0);
    assert(!ready(watch.fd, 60)); /* Fileman's own scans must not trigger a refresh. */
    touch_file("first/created");
    expect_changes(&watch);
    assert(rename("first/created", "first/renamed") == 0);
    expect_changes(&watch);
    assert(chmod("first/renamed", 0640) == 0);
    expect_changes(&watch);
    assert(unlink("first/renamed") == 0);
    expect_changes(&watch);

    touch_file("first/queued");
    /* More than one callback's read budget remains queued when navigation
     * removes the old watch. None of it may leak into the new directory. */
    for (unsigned i = 0; i < 1800; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "first/burst-%04u", i);
        touch_file(path);
    }
    assert(ready(watch.fd, 1000));
    assert(fileman_watch_set(&watch, "second") == 0);
    assert(!ready(watch.fd, 60)); /* The old queue and IN_IGNORED were discarded. */
    touch_file("first/obsolete");
    assert(!ready(watch.fd, 60));
    touch_file("second/current");
    expect_changes(&watch);

    assert(rename("second", "moved") == 0);
    assert(ready(watch.fd, 1000));
    unsigned flags = fileman_watch_drain(&watch);
    assert((flags & (FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_LOST)) ==
           (FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_LOST));
    assert(watch.wd == -1);
    touch_file("moved/after-move");
    assert(fileman_watch_drain(&watch) == 0);

    assert(fileman_watch_set(&watch, "missing") < 0 && watch.wd == -1);
    assert(fileman_watch_set(&watch, "moved") == 0);
    assert(inotify_rm_watch(watch.fd, watch.wd) == 0);
    assert(ready(watch.fd, 1000));
    flags = fileman_watch_drain(&watch);
    assert((flags & FILEMAN_WATCH_LOST) && watch.wd == -1);

    assert(mkdir("vanishing", 0700) == 0);
    assert(fileman_watch_set(&watch, "vanishing") == 0);
    assert(rmdir("vanishing") == 0);
    assert(ready(watch.fd, 1000));
    flags = fileman_watch_drain(&watch);
    assert((flags & FILEMAN_WATCH_LOST) && watch.wd == -1);
    fileman_watch_close(&watch);
    assert(watch.fd == -1 && watch.wd == -1);
}

static void test_records(void)
{
    struct fileman_watch watch = {-1, 42};
    struct inotify_event event = {42, IN_CREATE, 0, 0};
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) == FILEMAN_WATCH_CHANGED);
    event.wd = 41;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) == 0);
    event.wd = 42;
    event.mask = IN_OPEN | IN_ACCESS | IN_CLOSE_NOWRITE;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) == 0);
    event.wd = -1;
    event.mask = IN_Q_OVERFLOW;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) ==
           (FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_OVERFLOW));
    event.wd = 42;
    event.mask = IN_IGNORED;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) ==
           (FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_LOST));
    event.mask = IN_UNMOUNT;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) ==
           (FILEMAN_WATCH_CHANGED | FILEMAN_WATCH_LOST));
    event.mask = IN_CREATE;
    event.len = 100;
    assert(fileman_watch_consume(&watch, &event, sizeof(event)) & FILEMAN_WATCH_ERROR);
}

static void test_selection(void)
{
    struct reliefos_dir_entry before[] = {{1, "alpha"}, {1, "beta"}, {1, "gamma"}};
    struct reliefos_dir_entry after[] = {{1, "aardvark"}, {1, "beta"}, {1, "alpha"}};
    struct fileman_selection snapshot;
    uint64_t marked;
    int32_t selected;
    fileman_selection_capture(&snapshot, before, 3, 1, (1ULL << 0) | (1ULL << 2));
    fileman_selection_restore(&snapshot, after, 3, &selected, &marked);
    assert(selected == 1 && marked == (1ULL << 2));
    fileman_selection_capture(&snapshot, before, 3, 2, 1ULL << 2);
    fileman_selection_restore(&snapshot, after, 3, &selected, &marked);
    assert(selected == -1 && marked == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    test_records();
    test_selection();
    test_events();
    puts("ok - Fileman inotify events, navigation, loss, overflow, and selection preservation");
    return 0;
}
