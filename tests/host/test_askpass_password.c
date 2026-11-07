#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static char written[300];
static size_t used;
static int interrupt_once, fail_write;
static ssize_t capture_write(int fd, const void *buffer, size_t size)
{
    assert(fd == STDOUT_FILENO);
    if (interrupt_once) { interrupt_once = 0; errno = EINTR; return -1; }
    if (fail_write) { errno = EIO; return -1; }
    if (size > 3) size = 3;
    assert(used + size <= sizeof(written));
    memcpy(written + used, buffer, size);
    used += size;
    return (ssize_t)size;
}

#define write capture_write
#include "../../userland/apps/reliefaskpass/password_io.h"
#undef write

int main(void)
{
    interrupt_once = 1;
    assert(askpass_write_password("密码Ab3") == 0);
    assert(used == strlen("密码Ab3\n") && !memcmp(written, "密码Ab3\n", used));
    char boundary[256];
    memset(boundary, 'x', 255); boundary[255] = 0;
    used = 0;
    assert(askpass_write_password(boundary) == 0 && used == 256);
    char oversized[300];
    memset(oversized, 'x', sizeof(oversized)); oversized[299] = 0;
    used = 0;
    assert(askpass_write_password(oversized) == 1 && used == 0 && errno == E2BIG);
    used = 0;
    assert(askpass_write_password("a\nb") == 1 && used == 0 && errno == EINVAL);
    fail_write = 1;
    assert(askpass_write_password("secret") == 1 && errno == EIO);
    return 0;
}
