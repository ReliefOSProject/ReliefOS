#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { printf("[console-fd] FAIL line=%d errno=%d %s\n", \
    __LINE__, errno, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[console-fd] BEGIN standard descriptors and inherited status");
    for (int fd = 0; fd < 3; ++fd) {
        CHECK(fcntl(fd, F_GETFL) >= 0);
        struct stat st;
        CHECK(fstat(fd, &st) == 0);
        if (argc > 1 && !strcmp(argv[1], "--boot-stdio")) {
            struct termios attributes;
            struct winsize size;
            CHECK(S_ISCHR(st.st_mode) && major(st.st_rdev) == 5 && minor(st.st_rdev) == 1);
            CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR);
            CHECK(isatty(fd) == 1 && tcgetattr(fd, &attributes) == 0);
            CHECK(ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col);
        }
    }
    int console = open("/dev/console", O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(console >= 3 && isatty(console));
    struct stat console_stat;
    struct termios attributes;
    struct winsize size;
    CHECK(fstat(console, &console_stat) == 0 && S_ISCHR(console_stat.st_mode));
    CHECK(major(console_stat.st_rdev) == 5 && minor(console_stat.st_rdev) == 1);
    CHECK(lseek(console, 0, SEEK_CUR) == -1 && errno == ESPIPE);
    CHECK(write(console, NULL, 0) == 0);
    CHECK(tcgetattr(console, &attributes) == 0);
    CHECK(tcsetattr(console, TCSANOW, &attributes) == 0);
    CHECK(ioctl(console, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col);
    if (argc > 1 && !strcmp(argv[1], "--boot-stdio")) {
        struct vt_stat state;
        int unused = 0;
        CHECK(ioctl(console, VT_GETSTATE, &state) == 0);
        CHECK(state.v_active >= 1 && state.v_active <= 6);
        CHECK(state.v_state & (1u << state.v_active));
        CHECK(ioctl(console, VT_OPENQRY, &unused) == 0 && unused != state.v_active);
        unsigned other = state.v_active == 2 ? 3 : 2;
        CHECK(ioctl(console, VT_ACTIVATE, other) == 0);
        int other_console = open("/dev/console", O_RDWR | O_NOCTTY | O_NONBLOCK);
        CHECK(other_console >= 3 && isatty(other_console));
        struct winsize other_size, marker, observed;
        CHECK(ioctl(other_console, TIOCGWINSZ, &other_size) == 0);
        marker = other_size;
        marker.ws_row = size.ws_row + 1;
        marker.ws_col = size.ws_col + 1;
        CHECK(ioctl(other_console, TIOCSWINSZ, &marker) == 0);
        CHECK(ioctl(console, TIOCGWINSZ, &observed) == 0);
        CHECK(!memcmp(&observed, &size, sizeof(size)));
        CHECK(ioctl(console, VT_ACTIVATE, state.v_active) == 0);
        CHECK(ioctl(other_console, TIOCGWINSZ, &observed) == 0);
        CHECK(!memcmp(&observed, &marker, sizeof(marker)));
        CHECK(ioctl(other_console, TIOCSWINSZ, &other_size) == 0);
        CHECK(close(other_console) == 0);
        puts("[console-fd] boot TTY metadata, VT occupancy and console binding passed");
    }
    struct pollfd ready = {.fd = console, .events = POLLOUT};
    CHECK(poll(&ready, 1, 0) == 1 && ready.revents == POLLOUT);
    ready.events = POLLIN;
    if (poll(&ready, 1, 0) == 0) {
        char byte;
        CHECK(read(console, &byte, 1) == -1 && errno == EAGAIN);
    }
    int console_copy = dup(console);
    CHECK(console_copy >= 3 && tcgetattr(console_copy, &attributes) == 0);
    CHECK(close(console_copy) == 0 && close(console) == 0);
    int kmsg = open("/dev/kmsg", O_WRONLY | O_NONBLOCK);
    CHECK(kmsg >= 3);
    CHECK(fstat(kmsg, &console_stat) == 0);
    CHECK(major(console_stat.st_rdev) == 1 && minor(console_stat.st_rdev) == 11);
    CHECK(!isatty(kmsg));
    CHECK(tcgetattr(kmsg, &attributes) == -1 && errno == ENOTTY);
    CHECK(ioctl(kmsg, TIOCGWINSZ, &size) == -1 && errno == ENOTTY);
    CHECK(close(kmsg) == 0);
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    CHECK(!isatty(pipefd[1]));
    CHECK(ioctl(pipefd[1], TIOCGWINSZ, &size) == -1 && errno == ENOTTY);
    CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    int copy = dup(1), original = fcntl(1, F_GETFL);
    CHECK(copy >= 3 && original >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setsid() >= 0);
        int detached_console = open("/dev/console", O_RDWR);
        CHECK(detached_console >= 3 && isatty(detached_console));
        CHECK(open("/dev/tty", O_RDONLY | O_NOCTTY) == -1 && errno == ENXIO);
        CHECK(close(detached_console) == 0);
        CHECK(fcntl(copy, F_SETFL, original | O_NONBLOCK) == 0);
        CHECK(close(1) == 0);
        CHECK(fcntl(1, F_GETFL) == -1 && errno == EBADF);
        CHECK(open("/dev/null", O_RDWR) == 1);
        CHECK(fcntl(copy, F_GETFL) & O_NONBLOCK);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(fcntl(1, F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(1, F_SETFL, original) == 0);
    CHECK(close(copy) == 0);
    puts("[console-fd] DONE failures=0");
    return 0;
}
