#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    printf("[epoll-pty] FAIL line=%d errno=%d: %s\n", __LINE__, errno, #condition); \
    return 1; } } while (0)

static long long milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(5);
    int master, slave, saved_stderr = dup(2);
    struct termios settings;
    CHECK(saved_stderr >= 0 && openpty(&master, &slave, NULL, NULL, NULL) == 0);
    CHECK(tcgetattr(slave, &settings) == 0);
    cfmakeraw(&settings);
    CHECK(tcsetattr(slave, TCSANOW, &settings) == 0 && dup2(slave, 2) == 2);
    int ep = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event output[1024];
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0xabcdef0123456789ULL};
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, 2, &event) == 0);
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, 2, &event) == -1 && errno == EEXIST);
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, -1, &event) == -1 && errno == EBADF);
    CHECK(syscall(SYS_epoll_wait, ep, output, 0, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_epoll_wait, ep, output, INT_MAX, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_epoll_pwait, ep, output, 1024, 0, NULL, 8) == 0);
    for (int i = 0; i < 2; ++i) {
        long long start = milliseconds();
        CHECK(start >= 0);
        CHECK(syscall(SYS_epoll_pwait, ep, output, 1024, 40, NULL, 8) == 0);
        long long elapsed = milliseconds() - start;
        CHECK(elapsed >= 30 && elapsed < 1500);
    }
    puts("[epoll-pty] PASS PTY stderr registration, 1024 events and repeated timeouts");
    CHECK(write(master, "reply", 5) == 5);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1024, 1000) == 1);
    CHECK(output[0].events == EPOLLIN && output[0].data.u64 == event.data.u64);
    char buffer[8];
    CHECK(read(2, buffer, sizeof(buffer)) == 5 && memcmp(buffer, "reply", 5) == 0);
    event.events |= EPOLLONESHOT;
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_MOD, 2, &event) == 0);
    CHECK(write(master, "r", 1) == 1);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1024, 0) == 1);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1024, 0) == 0);
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_MOD, 2, &event) == 0);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1024, 0) == 1);
    CHECK(read(2, buffer, sizeof(buffer)) == 1);
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_DEL, 2, NULL) == 0);
    CHECK(close(ep) == 0 && dup2(saved_stderr, 2) == 2);
    CHECK(close(saved_stderr) == 0 && close(slave) == 0 && close(master) == 0);
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    ep = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    event = (struct epoll_event){.events = EPOLLIN | EPOLLOUT | EPOLLET,
                                 .data.u64 = 0x13579bdf2468ace0ULL};
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, sockets[0], &event) == 0);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    CHECK(output[0].events & EPOLLOUT);
    CHECK(write(sockets[1], "a", 1) == 1);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    CHECK(output[0].events & EPOLLIN);
    CHECK(read(sockets[0], buffer, 1) == 1 && buffer[0] == 'a');
    CHECK(write(sockets[1], "b", 1) == 1);
    /* Refill before the next readiness probe.  ET must report the new input
     * generation even though the previous EPOLLIN bit was never observed
     * clear by epoll_wait. */
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    CHECK(output[0].events & EPOLLIN);
    CHECK(read(sockets[0], buffer, 1) == 1 && buffer[0] == 'b');
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 0);
    CHECK(close(ep) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0);
    puts("[epoll-pty] PASS ET socket: persistent EPOLLOUT does not mask EPOLLIN edges");
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char listener_path[PATH_MAX];
    CHECK(snprintf(listener_path, sizeof(listener_path), "%s/epoll-listener-%ld",
                   tmpdir, (long)getpid()) > 0);
    (void)unlink(listener_path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    struct sockaddr_un listener_address = {.sun_family = AF_UNIX};
    CHECK(snprintf(listener_address.sun_path, sizeof(listener_address.sun_path), "%s",
                   listener_path) > 0);
    socklen_t listener_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                             strlen(listener_address.sun_path) + 1);
    CHECK(bind(listener, (struct sockaddr *)&listener_address, listener_length) == 0);
    CHECK(listen(listener, 4) == 0);
    ep = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    event = (struct epoll_event){.events = EPOLLIN | EPOLLET,
                                 .data.u64 = 0x2468ace013579bdfULL};
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, listener, &event) == 0);
    int first_client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(first_client >= 0);
    CHECK(connect(first_client, (struct sockaddr *)&listener_address, listener_length) == 0);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    int accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    int second_client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(second_client >= 0);
    CHECK(connect(second_client, (struct sockaddr *)&listener_address, listener_length) == 0);
    /* The first EPOLLIN bit was never observed clear by epoll_wait.  A new
     * pending connection must still produce an edge via rx_written. */
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    CHECK(close(second_client) == 0 && close(first_client) == 0 && close(accepted) == 0);
    CHECK(close(ep) == 0 && close(listener) == 0 && unlink(listener_path) == 0);
    puts("[epoll-pty] PASS ET listener: pending connections produce fresh edges");
    int stale[2], replacement[2];
    CHECK(pipe(stale) == 0);
    ep = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    event = (struct epoll_event){.events = EPOLLIN, .data.u64 = 0x1111222233334444ULL};
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, stale[0], &event) == 0);
    int stale_fd = stale[0];
    CHECK(close(stale[0]) == 0 && pipe(replacement) == 0 && replacement[0] == stale_fd);
    event.data.u64 = 0x5555666677778888ULL;
    CHECK(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, replacement[0], &event) == 0);
    CHECK(write(replacement[1], "r", 1) == 1);
    CHECK(syscall(SYS_epoll_wait, ep, output, 1, 0) == 1);
    CHECK(output[0].events == EPOLLIN && output[0].data.u64 == event.data.u64);
    CHECK(read(replacement[0], buffer, 1) == 1);
    CHECK(close(ep) == 0 && close(replacement[0]) == 0 && close(replacement[1]) == 0 &&
          close(stale[1]) == 0);
    puts("[epoll-pty] PASS close/reopen removes stale fd callbacks");
    int pipes[2], on = -1, off = 0;
    CHECK(pipe2(pipes, O_CLOEXEC) == 0);
    int copy = dup(pipes[0]);
    CHECK(copy >= 0);
    CHECK(syscall(SYS_ioctl, pipes[0], FIONBIO, &on) == 0);
    CHECK((fcntl(copy, F_GETFL) & O_NONBLOCK) != 0);
    CHECK(fcntl(pipes[0], F_GETFD) == FD_CLOEXEC && fcntl(copy, F_GETFD) == 0);
    CHECK(read(copy, buffer, 1) == -1 && errno == EAGAIN);
    CHECK(syscall(SYS_ioctl, copy, FIONBIO, NULL) == -1 && errno == EFAULT);
    CHECK((fcntl(copy, F_GETFL) & O_NONBLOCK) != 0);
    CHECK(syscall(SYS_ioctl, copy, FIONBIO, &off) == 0);
    CHECK((fcntl(pipes[0], F_GETFL) & O_NONBLOCK) == 0);
    CHECK(close(copy) == 0 && close(pipes[0]) == 0 && close(pipes[1]) == 0);
    CHECK(syscall(SYS_ioctl, copy, FIONBIO, NULL) == -1 && errno == EBADF);
    char filename[PATH_MAX];
    int filename_length = snprintf(filename, sizeof(filename), "%s/epoll-fionbio-XXXXXX", tmpdir);
    CHECK(filename_length > 0 && (size_t)filename_length < sizeof(filename));
    int regular = mkostemp(filename, O_APPEND | O_CLOEXEC);
    CHECK(regular >= 0);
    CHECK(syscall(SYS_ioctl, regular, FIONBIO, &on) == 0);
    CHECK((fcntl(regular, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) ==
          (O_RDWR | O_APPEND | O_NONBLOCK));
    CHECK(fcntl(regular, F_GETFD) == FD_CLOEXEC);
    CHECK(write(regular, "ok", 2) == 2);
    CHECK(syscall(SYS_ioctl, regular, FIONBIO, &off) == 0);
    CHECK((fcntl(regular, F_GETFL) & (O_APPEND | O_NONBLOCK)) == O_APPEND);
    int path = open(filename, O_PATH);
    CHECK(path >= 0 && syscall(SYS_ioctl, path, FIONBIO, &on) == -1 && errno == EBADF);
    CHECK(close(path) == 0 && close(regular) == 0 && unlink(filename) == 0);
    alarm(0);
    puts("[epoll-pty] PASS readiness, payload, oneshot, rearm and delete");
    puts("[epoll-pty] PASS FIONBIO shared flags, actual pipe EAGAIN, regular file and errors");
    puts("[epoll-pty] DONE failures=0");
    return 0;
}
