#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        printf("[io-runtime] FAIL line=%d errno=%d: %s\n", __LINE__, errno, #condition); \
        return 1; \
    } \
} while (0)

static int regular_file(void)
{
    char path[512], data[16] = {0};
    const char *temporary = getenv("TMPDIR");
    int length = snprintf(path, sizeof(path), "%s/io-runtime-XXXXXX", temporary ? temporary : "/tmp");
    CHECK(length > 0 && (size_t)length < sizeof(path));
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    int readonly = open(path, O_RDONLY);
    CHECK(readonly >= 0 && unlink(path) == 0);
    struct iovec output[2] = {{"ab", 2}, {"cdef", 4}};
    CHECK(writev(fd, output, 2) == 6);
    CHECK(lseek(fd, 1, SEEK_SET) == 1);
    struct iovec input[2] = {{data, 2}, {data + 2, 3}};
    CHECK(readv(fd, input, 2) == 5 && !memcmp(data, "bcdef", 5));
    CHECK(lseek(fd, 0, SEEK_CUR) == 6);
    CHECK(pwrite(fd, "XY", 2, 2) == 2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 6);
    CHECK(pread(fd, data, 6, 0) == 6 && !memcmp(data, "abXYef", 6));
    CHECK(lseek(fd, 0, SEEK_CUR) == 6);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    int duplicate = dup(fd);
    CHECK(duplicate >= 0);
    CHECK(read(duplicate, data, 2) == 2 && !memcmp(data, "ab", 2));
    CHECK(lseek(fd, 0, SEEK_CUR) == 2);
    CHECK(write(duplicate, "Q", 1) == 1);
    CHECK(lseek(fd, 0, SEEK_CUR) == 3);
    CHECK(pread(fd, data, 6, 0) == 6 && !memcmp(data, "abQYef", 6));
    errno = 0;
    CHECK(write(readonly, "x", 1) == -1 && errno == EBADF);
    CHECK(close(duplicate) == 0 && close(readonly) == 0 && close(fd) == 0);
    return 0;
}

static int pipe_and_signal(void)
{
    int pair[2], status;
    char data[16] = {0};
    CHECK(pipe(pair) == 0);
    CHECK(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    errno = 0;
    CHECK(read(pair[0], data, 1) == -1 && errno == EAGAIN);
    CHECK(fcntl(pair[0], F_SETFL, 0) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct timespec delay = {.tv_nsec = 10000000};
        close(pair[0]);
        if (dup2(pair[1], STDOUT_FILENO) != STDOUT_FILENO) _exit(2);
        close(pair[1]);
        nanosleep(&delay, NULL);
        _exit(write(STDOUT_FILENO, "redirect", 8) == 8 ? 0 : 3);
    }
    CHECK(close(pair[1]) == 0);
    struct iovec vectors[2] = {{data, 3}, {data + 3, 5}};
    CHECK(readv(pair[0], vectors, 2) == 8 && !memcmp(data, "redirect", 8));
    CHECK(read(pair[0], data, 1) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(pair[0]) == 0);
    CHECK(pipe(pair) == 0 && close(pair[0]) == 0);
    struct sigaction ignore = {.sa_handler = SIG_IGN}, old;
    CHECK(sigemptyset(&ignore.sa_mask) == 0 && sigaction(SIGPIPE, &ignore, &old) == 0);
    errno = 0;
    CHECK(write(pair[1], "x", 1) == -1 && errno == EPIPE);
    CHECK(close(pair[1]) == 0 && sigaction(SIGPIPE, &old, NULL) == 0);
    return 0;
}

static int socket_io(void)
{
    int pair[2];
    char data[8] = {0};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct iovec output[2] = {{"abc", 3}, {"def", 3}};
    CHECK(writev(pair[0], output, 2) == 6);
    CHECK(read(pair[1], data, 6) == 6 && !memcmp(data, "abcdef", 6));
    CHECK(write(pair[1], "reply", 5) == 5);
    struct iovec input[2] = {{data, 2}, {data + 2, 3}};
    CHECK(readv(pair[0], input, 2) == 5 && !memcmp(data, "reply", 5));
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

static int terminal_io(void)
{
    char path[128], data[8] = {0};
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *name = ptsname(master);
    CHECK(name != NULL && strlen(name) < sizeof(path));
    strcpy(path, name);
    int slave = open(path, O_RDWR | O_NOCTTY);
    CHECK(slave >= 0);
    struct termios mode;
    CHECK(tcgetattr(slave, &mode) == 0);
    mode.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    mode.c_oflag &= ~OPOST;
    mode.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    mode.c_cflag = (mode.c_cflag & ~(CSIZE | PARENB)) | CS8;
    mode.c_cc[VMIN] = 1; mode.c_cc[VTIME] = 0;
    CHECK(tcsetattr(slave, TCSANOW, &mode) == 0);
    CHECK(write(master, "input", 5) == 5);
    CHECK(read(slave, data, 5) == 5 && !memcmp(data, "input", 5));
    CHECK(write(slave, "output", 6) == 6);
    CHECK(read(master, data, 6) == 6 && !memcmp(data, "output", 6));
    CHECK(close(slave) == 0 && close(master) == 0);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);
    if (regular_file() || pipe_and_signal() || socket_io() || terminal_io()) return 1;
    int fd = open("/dev/null", O_RDWR);
    char data[512] = {0};
    CHECK(fd >= 0 && write(fd, data, sizeof(data)) == sizeof(data));
    CHECK(read(fd, data, sizeof(data)) == 0 && close(fd) == 0);
    alarm(0);
    printf("[io-runtime] DONE checks=%u failures=0\n", checks);
    return 0;
}
