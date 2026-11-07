#include <assert.h>
#include <stdarg.h>
#include "../../userland/runtime/src/blockdev.c"
_Static_assert(BLKROGET == 0x125e, "Linux BLKROGET uses the historical _IO encoding");

static int fail_open, fail_ioctl, fail_seek, fail_io;
int open(const char *path, int flags, ...)
{
    (void)flags;
    if (fail_open || strcmp(path, "/dev/sda")) {
        errno = fail_open ? EACCES : ENOENT;
        return -1;
    }
    return 42;
}
int close(int fd) { assert(fd == 42); errno = EBUSY; return 0; }
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42);
    if (fail_ioctl) { errno = EIO; return -1; }
    va_list args;
    va_start(args, request);
    if (request == BLKGETSIZE64) *va_arg(args, uint64_t *) = 2ULL * 1024 * 1024 * 1024;
    else if (request == BLKSSZGET) *va_arg(args, int *) = 512;
    else if (request == BLKROGET) *va_arg(args, int *) = 0;
    else assert(request == BLKRRPART);
    va_end(args);
    return 0;
}
off_t lseek(int fd, off_t offset, int whence)
{
    assert(fd == 42 && whence == SEEK_SET);
    if (fail_seek) { errno = ESPIPE; return -1; }
    return offset;
}
ssize_t read(int fd, void *buffer, size_t length)
{
    assert(fd == 42);
    if (fail_io) { errno = EIO; return -1; }
    memset(buffer, 0, length);
    return length;
}
ssize_t write(int fd, const void *buffer, size_t length)
{
    assert(fd == 42); (void)buffer;
    if (fail_io) { errno = ENOSPC; return -1; }
    return length;
}

int main(void)
{
    struct reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS];
    uint32_t count = 0;
    assert(reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count) == 0);
    assert(count == 1 && disks[0].sector_count == 4194304 && disks[0].sector_size == 512);
    fail_open = 1;
    assert(reliefos_block_get_info("/dev/sda", disks) == -EACCES);
    fail_open = 0; fail_ioctl = 1;
    assert(reliefos_block_get_info("/dev/sda", disks) == -EIO);
    assert(block_reread(42) == -EIO);
    fail_ioctl = 0;
    char buffer[512];
    fail_seek = 1;
    assert(block_io(42, 0, buffer, sizeof(buffer), 0) == -ESPIPE);
    fail_seek = 0; fail_io = 1;
    assert(block_io(42, 0, buffer, sizeof(buffer), 0) == -EIO);
    assert(block_io(42, 0, buffer, sizeof(buffer), 1) == -ENOSPC);
    puts("PASS block device errors: missing disks, open/ioctl/seek/read/write and cleanup");
    return 0;
}
