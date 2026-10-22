#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mntent.h>
#include <unistd.h>
#include "../../userland/apps/installer/installer_partitions.h"

/** @brief Map only the fixture disk; all actual reads/writes use POSIX files. */
static int fixture_open(const char *path, int flags, ...);
/** @brief Substitute device geometry and fault states, without replacing GPT code. */
static int fixture_ioctl(int fd, unsigned long request, ...);
/** @brief Provide a Linux mount table owned by this fixture. */
static FILE *fixture_mounts(const char *path, const char *mode);
#define open fixture_open
#define ioctl fixture_ioctl
#define setmntent fixture_mounts
#include "../../userland/runtime/src/blockdev.c"
#undef open
#undef ioctl
#undef setmntent

static const char *image;
static int logical_sector = 512, readonly, mounted;
static int entropy_failure;
static const char *scenario;
static int clearing;
ssize_t __real_pwrite(int fd, const void *data, size_t size, off_t offset);
int __real_fsync(int fd);

/** @brief Inject failures only into the reserved-extent clearing step. */
ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset)
{
    clearing = 1;
    if (!strcmp(scenario, "clear-write-fail")) { errno = EIO; return -1; }
    return __real_pwrite(fd, data, size, offset);
}

/** @brief A failed clear barrier must abort before any filesystem formatting. */
int __wrap_fsync(int fd)
{
    if (clearing && !strcmp(scenario, "clear-sync-fail")) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static const char *mount_source = "/dev/sda1";
int __real_open(const char *path, int flags, ...);

/** @brief Route the planner's direct POSIX erasure to the same private image. */
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (!strcmp(path, "/dev/sda")) return fixture_open(path, flags);
    return __real_open(path, flags, mode);
}

static int fixture_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/urandom")) {
        if (entropy_failure) { errno = EIO; return -1; }
        return __real_open(path, flags);
    }
    if (strcmp(path, "/dev/sda")) { errno = ENOENT; return -1; }
    return __real_open(image, flags, 0600);
}

static int fixture_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    if (request == BLKGETSIZE64) {
        struct stat st;
        assert(fstat(fd, &st) == 0);
        *va_arg(args, uint64_t *) = (uint64_t)st.st_size;
    } else if (request == BLKSSZGET) *va_arg(args, int *) = logical_sector;
    else if (request == BLKROGET) *va_arg(args, int *) = readonly;
    else assert(request == BLKRRPART);
    va_end(args);
    return 0;
}

static FILE *fixture_mounts(const char *path, const char *mode)
{
    (void)path; (void)mode;
    static char text[128];
    snprintf(text, sizeof(text), "%s /target ext4 rw 0 0\n", mount_source);
    return fmemopen(text, mounted ? strlen(text) : 0, "r");
}

/** @brief Exercise the shared planner and production GPT writer on an owned image. */
int main(int argc, char **argv)
{
    assert(argc == 3);
    image = argv[1];
    scenario = argv[2];
    struct reliefos_block_disk_info disk;
    struct installer_partition_plan plan;
    assert(reliefos_block_get_info("/dev/sda", &disk) == 0);
    if (!strcmp(argv[2], "small")) {
        assert(installer_partition_plan_default(&disk, &plan) == -ENOSPC);
        return 0;
    }
    assert(installer_partition_plan_default(&disk, &plan) == 0);
    mounted = 1;
    mount_source = "/dev/sda12";
    assert(block_check_unmounted("/dev/sda1") == 0);
    assert(block_check_unmounted("/dev/sda") == -EBUSY);
    mounted = 0;
    mount_source = "/dev/sda1";
    assert(plan.bios.index == 0 && plan.bios.first_lba == 2048 && plan.bios.sector_count == 2048);
    assert(plan.esp.index == 1 && plan.esp.first_lba == 4096 && plan.esp.sector_count == 262144);
    assert(plan.root.index == 2 && plan.root.first_lba == 266240);
    assert(plan.root.first_lba + plan.root.sector_count <= disk.sector_count - 33);
    struct reliefos_block_disk_info bad = disk;
    bad.sector_size = 4096;
    assert(installer_partition_plan_default(&bad, &plan) == -ENOTSUP);
    bad = disk; bad.sector_count = UINT64_MAX;
    assert(installer_partition_plan_default(&bad, &plan) == -EOVERFLOW);
    assert(installer_partition_plan_default(&disk, &plan) == 0);
    if (!strcmp(argv[2], "manual")) {
        assert(reliefos_block_gpt_initialize("/dev/sda", 1) == 0);
        uint32_t index;
        assert(reliefos_block_gpt_create("/dev/sda", RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN,
                                         4, "KEEP_DATA", &index) == 0 && index == 0);
        unsigned char keep[512], after[512];
        memset(keep, 0xa7, sizeof(keep));
        int fd = open(image, O_RDWR);
        assert(fd >= 0 && pwrite(fd, keep, sizeof(keep), 2048 * 512) == sizeof(keep));
        assert(reliefos_block_gpt_create("/dev/sda", RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN,
                                         1, "BIOS", &index) == 0 && index == 1);
        assert(reliefos_block_gpt_set_type("/dev/sda", index, RELIEFOS_BLOCK_GPT_BIOS_BOOT) == 0);
        assert(reliefos_block_gpt_create("/dev/sda", RELIEFOS_BLOCK_FILESYSTEM_FAT32,
                                         128, "ESP", &index) == 0 && index == 2);
        assert(reliefos_block_gpt_set_type("/dev/sda", index, RELIEFOS_BLOCK_GPT_ESP) == 0);
        assert(reliefos_block_gpt_create("/dev/sda", RELIEFOS_BLOCK_FILESYSTEM_EXT4,
                                         128, "ROOT", &index) == 0 && index == 3);
        assert(pread(fd, after, sizeof(after), 2048 * 512) == sizeof(after));
        assert(memcmp(keep, after, sizeof(keep)) == 0);
        close(fd);
        return 0;
    }
    if (!strcmp(argv[2], "readonly")) readonly = 1;
    else if (!strcmp(argv[2], "no-entropy")) entropy_failure = 1;
    else if (!strcmp(argv[2], "mounted")) mounted = 1;
    else if (!strcmp(argv[2], "4kn")) logical_sector = 4096;
    else if (!strcmp(argv[2], "badplan")) ++plan.root.first_lba;
    int result = installer_partition_apply_default("/dev/sda", &plan);
    if (!strcmp(scenario, "clear-write-fail") || !strcmp(scenario, "clear-sync-fail")) {
        assert(result == -EIO && clearing);
    } else if (readonly) assert(result == -EROFS);
    else if (entropy_failure) assert(result == -EIO);
    else if (mounted) assert(result == -EBUSY);
    else if (logical_sector != 512) assert(result == -ENOTSUP);
    else if (!strcmp(argv[2], "badplan")) assert(result == -EINVAL);
    else {
        assert(result == 0);
        struct reliefos_block_partition parts[128];
        uint32_t count;
        assert(reliefos_block_list_partitions("/dev/sda", parts, 128, &count) == 0 && count == 3);
        assert(parts[0].gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT);
        assert(parts[0].filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN);
        assert(!strcmp(reliefos_block_gpt_type_name(parts[0].gpt_type), "bios-boot"));
        char path[64], uuid[37];
        assert(reliefos_block_partition_path("/dev/nvme0n1", 2, path, sizeof(path)) == 0);
        assert(!strcmp(path, "/dev/nvme0n1p3"));
        for (unsigned i = 0; i < count; ++i) {
            assert(reliefos_block_partition_uuid("/dev/sda", parts[i].index, uuid) == 0);
            assert(strlen(uuid) == 36);
        }
    }
    return 0;
}
