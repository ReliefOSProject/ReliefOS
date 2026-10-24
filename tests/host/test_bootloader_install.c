#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <mntent.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <reliefos/blockdev.h>
#include <reliefos/bootloader.h>

static const char *virtual_image, *scenario, *source_path;
static dev_t target_device;
static unsigned callbacks;

/** @brief Host metadata adapter for private raw images, never real device nodes. */
int __real_stat(const char *path, struct stat *out);
/** @brief Real host lstat used outside the virtual device namespace. */
int __real_lstat(const char *path, struct stat *out);
/** @brief Real host open used only on caller-owned files. */
int __real_open(const char *path, int flags, ...);
/** @brief Real host write for non-faulted transfers. */
ssize_t __real_write(int fd, const void *data, size_t length);
/** @brief Real host synchronization for non-faulted transfers. */
int __real_fsync(int fd);
/** @brief Actual host mount reader outside virtual device tests. */
FILE *__real_setmntent(const char *path, const char *mode);

/** @brief Provide the tested current mount namespace for virtual-device cases. */
FILE *__wrap_setmntent(const char *path, const char *mode)
{
    if (!virtual_image) return __real_setmntent(path, mode);
    static char table[] = "/dev/sda1 /already-mounted ext4 rw 0 0\n";
    return fmemopen(table, !strcmp(scenario, "bios-mounted") ? strlen(table) : 0, "r");
}

/** @brief Model Linux block metadata while the successful installer runs real GRUB. */
static int device_stat(const char *path, struct stat *out)
{
    if (!virtual_image) return 0;
    for (unsigned disk = 0; disk < 2; ++disk) {
        for (unsigned part = 0; part < 5; ++part) {
            char name[32];
            if (part) snprintf(name, sizeof(name), "/dev/sd%c%u", 'a' + disk, part);
            else snprintf(name, sizeof(name), "/dev/sd%c", 'a' + disk);
            if (strcmp(path, name)) continue;
            memset(out, 0, sizeof(*out));
            out->st_mode = S_IFBLK | 0600;
            out->st_rdev = !disk && part == 2 ? target_device : makedev(244 + disk, part);
            return 1;
        }
    }
    return 0;
}

/** @brief Interpose only synthetic device status and the source backing-device identity. */
int __wrap_stat(const char *path, struct stat *out)
{
    if (device_stat(path, out)) return 0;
    int result = __real_stat(path, out);
    if (!result && virtual_image && !strcmp(path, source_path))
        out->st_dev = !strcmp(scenario, "source-target") ? target_device : makedev(244, 90);
    return result;
}

/** @brief Preserve no-follow semantics while modeling synthetic device nodes. */
int __wrap_lstat(const char *path, struct stat *out)
{
    return device_stat(path, out) ? 0 : __real_lstat(path, out);
}

/** @brief Redirect a modeled whole disk exclusively to an owned regular-file image. */
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    if (virtual_image && (!strcmp(path, "/dev/sda") || !strcmp(path, "/dev/sdb"))) path = virtual_image;
    return __real_open(path, flags, mode);
}

/** @brief Simulate the Linux block read-only query for the owned fixture. */
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    assert(virtual_image && request == BLKROGET);
    va_list args; va_start(args, request);
    *va_arg(args, int *) = !strcmp(scenario, "readonly");
    va_end(args);
    return 0;
}

/** @brief Return the same standard geometry written into the fixture GPT. */
int __wrap_reliefos_block_get_info(const char *path, struct reliefos_block_disk_info *out)
{
    (void)path;
    memset(out, 0, sizeof(*out));
    out->sector_size = 512;
    out->sector_count = 192 * 2048;
    return 0;
}

/** @brief Return the virtual disk's real role identity to the root-config publisher. */
int __wrap_reliefos_block_partition_uuid(const char *path, uint32_t index, char uuid[37])
{
    (void)path;
    if (index == 3) strcpy(uuid, "23456789-1234-4abc-90ab-112233445566");
    else if (index == 2) strcpy(uuid, "12345678-1234-4abc-90ab-112233445566");
    else return -ENOENT;
    return 0;
}

/** @brief Model block enumeration; raw embedding is checked independently against sfdisk. */
int __wrap_reliefos_block_list_partitions(const char *disk, struct reliefos_block_partition *parts,
                                          uint32_t capacity, uint32_t *count)
{
    assert(capacity >= 3);
    memset(parts, 0, 3 * sizeof(*parts));
    const uint64_t starts[] = {2048, 4096, 266240};
    const uint64_t sizes[] = {2048, 262144, 192 * 2048 - 266240 - 33};
    const uint32_t types[] = {RELIEFOS_BLOCK_GPT_BIOS_BOOT, RELIEFOS_BLOCK_GPT_ESP,
                              RELIEFOS_BLOCK_GPT_LINUX};
    for (unsigned i = 0; i < 3; ++i) {
        parts[i].index = i;
        parts[i].first_lba = starts[i]; parts[i].sector_count = sizes[i];
        parts[i].gpt_type = types[i];
        snprintf(parts[i].path, sizeof(parts[i].path), "%s%u", disk, i + 1);
    }
    parts[1].filesystem = RELIEFOS_BLOCK_FILESYSTEM_FAT32;
    if (!strcmp(scenario, "bios-formatted")) parts[0].filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    if (!strcmp(scenario, "no-bios-space")) parts[0].gpt_type = RELIEFOS_BLOCK_GPT_BASIC_DATA;
    *count = 3;
    if (!strcmp(scenario, "ambiguous-root") || !strcmp(scenario, "explicit-root")) {
        assert(capacity >= 4);
        parts[3] = parts[2];
        parts[3].index = 3;
        snprintf(parts[3].path, sizeof(parts[3].path), "%s4", disk);
        *count = 4;
    }
    return 0;
}

/** @brief Fail a private file sync before any rename, leaving an old menu intact. */
int __wrap_fsync(int fd)
{
    struct stat st;
    if (virtual_image && !strcmp(scenario, "sync-fail") && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        errno = EIO; return -1;
    }
    return __real_fsync(fd);
}

/** @brief Inject a copy failure only into private regular destination files. */
ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    struct stat st;
    if (virtual_image && !strcmp(scenario, "copy-fail") && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        errno = ENOSPC; return -1;
    }
    return __real_write(fd, data, length);
}

/** @brief Count event-pump opportunities while copying and waiting for the real tool. */
static void progress(void *context)
{
    (void)context;
    ++callbacks;
}

/** @brief Exercise the same public API as the installer on a private boot tree. */
int main(int argc, char **argv)
{
    assert(argc == 6 || argc == 8);
    struct reliefos_boot_install_options options = {
        .source = argv[1], .boot_dir = argv[2],
        .install_bios = (uint8_t)(argv[3][0] == '1'),
        .install_uefi = (uint8_t)(argv[4][0] == '1'),
        .preserve_rollback = (uint8_t)(argv[5][0] == '1'),
        .progress = progress,
    };
    if (argc == 8) {
        struct stat target;
        assert(__real_stat(options.boot_dir, &target) == 0);
        virtual_image = argv[6]; scenario = argv[7]; source_path = argv[1];
        target_device = target.st_dev;
        options.disk_path = !strcmp(scenario, "partition") ? "/dev/sda2" :
            !strcmp(scenario, "wrong-disk") ? "/dev/sdb" : "/dev/sda";
        if (!strcmp(scenario, "bad-root")) options.root_partuuid = "bad; halt";
        if (!strcmp(scenario, "missing-root")) options.root_partuuid = "34567890-1234-4abc-90ab-112233445566";
        if (!strcmp(scenario, "explicit-root")) options.root_partuuid = "12345678-1234-4ABC-90AB-112233445566";
    }
    int result = reliefos_boot_install(&options);
    if (result) { fprintf(stderr, "boot install failed: %d\n", result); return 1; }
    uint32_t modes;
    assert(reliefos_boot_probe(options.disk_path, options.boot_dir, &modes) == 0);
    unsigned selected = (options.install_bios ? 1u : 0u) | (options.install_uefi ? 2u : 0u);
    assert((modes & selected) == selected);
    assert(callbacks > 0);
    puts("PASS shared boot publication API");
    return 0;
}
