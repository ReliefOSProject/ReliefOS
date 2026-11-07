/* Guest-only regression on the caller's private scratch block device. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    int fd = open(argv[1], O_RDWR | O_EXCL | O_CLOEXEC);
    if (geteuid()) {
        if (fd >= 0) { close(fd); return 3; }
        if (errno != EACCES && errno != EPERM) return 4;
        puts("DM_BLOCK_DAC_DENIED");
        return 0;
    }
    if (fd < 0) { perror("open scratch disk"); return 5; }
    unsigned char sector[512];
    if (pread(fd, sector, sizeof(sector), 0) != sizeof(sector)) return 11;
    int path_fd = open(argv[1], O_PATH | O_WRONLY | O_CLOEXEC);
    if (path_fd < 0) return 12;
    struct iovec vector = {sector, sizeof(sector)};
    if (write(path_fd, sector, sizeof(sector)) != -1 || errno != EBADF ||
        writev(path_fd, &vector, 1) != -1 || errno != EBADF ||
        pwrite(path_fd, sector, sizeof(sector), 0) != -1 || errno != EBADF) return 13;
    close(path_fd);
    /* Rewrite the original unused payload sector, preserving every byte. */
    const off_t offset = 480 * 1024 * 1024;
    if (pread(fd, sector, sizeof(sector), offset) != sizeof(sector) ||
        pwrite(fd, sector, sizeof(sector), offset) != sizeof(sector) ||
        lseek(fd, offset, SEEK_SET) != offset || write(fd, sector, sizeof(sector)) != sizeof(sector) ||
        fsync(fd) || ioctl(fd, BLKRRPART, 0)) { perror("root block I/O"); close(fd); return 6; }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct saved[2], reduced[2];
    if (syscall(SYS_capget, &header, saved)) return 7;
    memcpy(reduced, saved, sizeof(saved));
    reduced[CAP_SYS_ADMIN / 32].effective &= ~(1U << (CAP_SYS_ADMIN % 32));
    if (syscall(SYS_capset, &header, reduced)) return 8;
    errno = 0;
    int result = ioctl(fd, BLKRRPART, 0), error = errno;
    if (syscall(SYS_capset, &header, saved)) return 9;
    close(fd);
    if (result != -1 || error != EACCES) return 10;
    puts("DM_BLOCK_ROOT_IO_OK_CAPABILITY_DENIED");
    return 0;
}
