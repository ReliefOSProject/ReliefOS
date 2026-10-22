/* Linux-compatible fd timestamp operations used by GNU cp -p. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static unsigned failures;
#define CHECK(c, label) do { int ok = (c); \
    printf("[futimens] %s %s errno=%d\n", ok ? "PASS" : "FAIL", label, ok ? 0 : errno); \
    failures += !ok; } while (0)

int main(void)
{
    char path[] = "/tmp/futimens-probe-XXXXXX", moved[128];
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    struct timespec t[2] = {{1700000010, 0}, {1700000020, 0}};
    struct stat st;
    CHECK(syscall(SYS_utimensat, fd, NULL, t, 0) == 0, "raw utimensat(fd,NULL)");
    CHECK(fstat(fd, &st) == 0 && st.st_atim.tv_sec == t[0].tv_sec &&
          st.st_mtim.tv_sec == t[1].tv_sec, "fd timestamps persisted");
    CHECK(futimens(fd, t) == 0, "libc futimens");
    CHECK(syscall(SYS_utimensat, fd, NULL, t, AT_SYMLINK_NOFOLLOW) == -1 && errno == EINVAL,
          "flags rejected on NULL pathname");
    CHECK(syscall(SYS_utimensat, -1, NULL, t, 0) == -1 && errno == EBADF, "invalid fd");
    int pathfd = open(path, O_PATH);
    CHECK(pathfd >= 0 && syscall(SYS_utimensat, pathfd, NULL, t, 0) == -1 && errno == EBADF,
          "O_PATH rejected for fd-only call");
    if (pathfd >= 0) close(pathfd);
    snprintf(moved, sizeof(moved), "%s-renamed", path);
    CHECK(rename(path, moved) == 0 && futimens(fd, t) == 0, "fd survives rename");
    CHECK(unlink(moved) == 0 && futimens(fd, t) == 0, "fd survives unlink");
    CHECK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec == t[1].tv_sec, "unlinked inode timestamps");
    close(fd);
    unlink(path); unlink(moved);
    printf("FUTIMENS_DONE failures=%u\n", failures);
    return failures ? 1 : 0;
}
