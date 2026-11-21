#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "../../userland/apps/installer/install_ops.c"
#include "../../userland/runtime/src/blockdev.c"

static const char *fixture_image;
static int fixture_readonly, writable_opens;
static dev_t fixture_source_device;
int __real_open(const char *, int, ...);
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    if (!strcmp(path, "/dev/sda")) {
        if ((flags & O_ACCMODE) != O_RDONLY) ++writable_opens;
        return __real_open(fixture_image, flags, mode);
    }
    if (!strncmp(path, "/dev/sda", 8)) { errno = ENOENT; return -1; }
    return __real_open(path, flags, mode);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    if (request == BLKGETSIZE64) {
        struct stat status;
        assert(!fstat(fd, &status));
        *va_arg(args, uint64_t *) = status.st_size;
    } else if (request == BLKSSZGET) *va_arg(args, int *) = 512;
    else if (request == BLKROGET) *va_arg(args, int *) = fixture_readonly;
    else assert(request == BLKRRPART);
    va_end(args);
    return 0;
}

static int fixture_stat(const char *path, struct stat *status)
{
    if (!strcmp(path, "/install/esp")) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFDIR | 0755;
        status->st_dev = fixture_source_device;
        return 0;
    }
    if (!strcmp(path, "/dev/sda") || !strcmp(path, "/dev/sda1") ||
        !strcmp(path, "/dev/sda128")) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFBLK | 0600;
        status->st_rdev = makedev(8, !strcmp(path, "/dev/sda") ? 0 :
            !strcmp(path, "/dev/sda1") ? 1 : 128);
        return 0;
    }
    if (!strncmp(path, "/dev/sda", 8)) { errno = ENOENT; return -1; }
    return 1;
}

int __wrap_stat(const char *path, struct stat *status)
{
    int result = fixture_stat(path, status);
    return result == 1 ? __real_stat(path, status) : result;
}

int __wrap_lstat(const char *path, struct stat *status)
{
    int result = fixture_stat(path, status);
    return result == 1 ? __real_lstat(path, status) : result;
}

FILE *__wrap_setmntent(const char *path, const char *mode)
{
    (void)path; (void)mode;
    FILE *stream = tmpfile();
    assert(stream);
    return stream;
}

/* Keep the real GPT reader/writer, and the real shared boot validator. Only
 * Linux device metadata is translated to this test's owned regular file. */
static void corrupt_structure(int fd)
{
    unsigned char sector[512];
    assert(pread(fd, sector, sizeof(sector), 512) == sizeof(sector));
    struct block_gpt_header *header = (void *)sector;
    header->reserved = 1;
    header->header_crc32 = 0;
    header->header_crc32 = block_crc32(header, header->header_size);
    assert(pwrite(fd, sector, sizeof(sector), 512) == sizeof(sector));
    assert(!fsync(fd));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    fixture_image = argv[1];
    fixture_source_device = makedev(11, 0);
    assert(!reliefos_block_gpt_initialize("/dev/sda", 1));
    int fd = __real_open(fixture_image, O_RDWR);
    assert(fd >= 0);
    corrupt_structure(fd);
    assert(!reliefos_block_get_info("/dev/sda", &disks[0]));
    disk_count = 1; selected_disk = 0;
    assert(!installer_plan_load(&partition_draft, &disks[0]));
    assert(partition_draft.table_error == -EINVAL);
    struct reliefos_boot_install_options boot = {
        .source = "/install/esp", .boot_dir = "/target/boot", .disk_path = "/dev/sda",
        .install_bios = 1, .install_uefi = 1,
    };
    writable_opens = 0;
    installer_partition_auto = 0;
    assert(installer_boot_disk_preflight(&boot) == -EINVAL);
    assert(!installer_plan_initialize(&partition_draft));
    assert(!installer_boot_disk_preflight(&boot));
    fixture_readonly = 1;
    assert(installer_boot_disk_preflight(&boot) == -EROFS);
    fixture_readonly = 0;
    fixture_source_device = makedev(8, 0);
    assert(installer_boot_disk_preflight(&boot) == -EBUSY);
    fixture_source_device = makedev(8, 1);
    assert(installer_boot_disk_preflight(&boot) == -EBUSY);
    /* Check every supported slot, including sparse kernel partition nodes. */
    fixture_source_device = makedev(8, 128);
    assert(installer_boot_disk_preflight(&boot) == -EBUSY);
    fixture_source_device = makedev(11, 0);
    unsigned char byte;
    assert(pread(fd, &byte, 1, 512 + 56) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 512 + 56) == 1 && !fsync(fd));
    assert(installer_boot_disk_preflight(&boot) == -ESTALE);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 512 + 56) == 1 && !fsync(fd));
    installer_plan_dispose(&partition_draft);
    assert(!installer_plan_load(&partition_draft, &disks[0]));
    installer_partition_auto = 1;
    assert(!installer_boot_disk_preflight(&boot));
    /* An unreadable/checksum-failed table is never a structural replacement. */
    unsigned char sector[512];
    assert(pread(fd, sector, sizeof(sector), 512) == sizeof(sector));
    struct block_gpt_header *header = (void *)sector;
    header->reserved = 0;
    header->header_crc32 ^= 1;
    assert(pwrite(fd, sector, sizeof(sector), 512) == sizeof(sector) && !fsync(fd));
    assert(installer_boot_disk_preflight(&boot) == -EIO);
    assert(!writable_opens);
    close(fd);
    installer_plan_dispose(&partition_draft);
    puts("PASS fresh boot preflight: explicit GPT replacement, source ownership, readonly, stale and I/O gates; no writes");
    return 0;
}
