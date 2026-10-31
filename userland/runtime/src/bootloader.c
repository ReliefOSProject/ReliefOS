/* Shared standard GRUB installation policy; frontends own their UI and disks. */
#include "bootloader_internal.h"
#include "kernel_bundle_internal.h"
#include <reliefos/blockdev.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Canonicalize a nonzero GPT UUID without permitting GRUB syntax in data. */
static int root_uuid_text(const char *text, char out[37])
{
    if (!text || strlen(text) != 36) return -EINVAL;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return -EINVAL;
            out[i] = '-';
        } else {
            if (!isxdigit((unsigned char)text[i])) return -EINVAL;
            out[i] = (char)tolower((unsigned char)text[i]);
            nonzero |= out[i] != '0';
        }
    }
    out[36] = 0;
    return nonzero ? 0 : -EINVAL;
}

/** @brief Check borrowed option strings and boolean ranges without I/O. */
static int options_valid(const struct reliefos_boot_install_options *options)
{
    return options && options->source && options->source[0] == '/' &&
        options->boot_dir && options->boot_dir[0] == '/' &&
        options->install_bios <= 1 && options->install_uefi <= 1 &&
        options->preserve_rollback <= 1;
}

/** @brief Require a nonempty regular media file and reject symlink payloads. */
static int payload_file(const struct reliefos_boot_install_options *options, const char *relative)
{
    char path[BOOT_PATH_MAX];
    int result = boot_join(path, options->source, relative);
    if (result) return result;
    struct stat st;
    if (lstat(path, &st) < 0) {
        int error = -errno;
        fprintf(stderr, "[boot-install] unavailable payload: %s (%d)\n", path, error);
        return error;
    }
    return S_ISREG(st.st_mode) && st.st_size > 0 ? 0 : -EINVAL;
}

/** @brief Verify every platform module declared by the upstream dependency manifest. */
static int bios_payload(const struct reliefos_boot_install_options *options)
{
    const char *const required[] = {"boot.img", "diskboot.img", "kernel.img",
        "lzma_decompress.img", "moddep.lst", "modinfo.sh", "command.lst", "fs.lst",
        "biosdisk.mod", "part_gpt.mod", "fat.mod", "normal.mod", "multiboot2.mod", "cmp.mod",
        "hashsum.mod", "gcry_sha256.mod", "crypto.mod", "loadenv.mod", "regexp.mod", "test.mod"};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char relative[256];
        snprintf(relative, sizeof(relative), "grub/i386-pc/%s", required[i]);
        int result = payload_file(options, relative);
        if (result) return result;
    }
    const char *const tools[] = {BOOT_GRUB_INSTALL, BOOT_GRUB_BIOS_SETUP,
        BOOT_GRUB_PROBE, BOOT_GRUB_MKIMAGE};
    for (unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i) {
        struct stat tool;
        if (stat(tools[i], &tool) < 0 || access(tools[i], X_OK) < 0) return -errno;
        if (!S_ISREG(tool.st_mode)) return -EINVAL;
    }
    char path[BOOT_PATH_MAX];
    int result = boot_join(path, options->source, "grub/i386-pc/moddep.lst");
    if (result) return result;
    FILE *stream = fopen(path, "r");
    if (!stream) return -errno;
    char line[8192];
    unsigned count = 0;
    while (fgets(line, sizeof(line), stream)) {
        char *colon = strchr(line, ':');
        if (!colon || colon == line || colon - line > 200) { result = -EINVAL; break; }
        *colon = 0;
        /* moddep names the built-in kernel.img as kernel, without a kernel.mod. */
        if (!strcmp(line, "kernel")) continue;
        for (char *p = line; *p; ++p)
            if (!isalnum((unsigned char)*p) && *p != '_') { result = -EINVAL; break; }
        if (result) break;
        char relative[256];
        snprintf(relative, sizeof(relative), "grub/i386-pc/%.200s.mod", line);
        result = payload_file(options, relative);
        if (result) break;
        ++count;
    }
    if (!result && (ferror(stream) || !count)) result = -EIO;
    fclose(stream);
    return result;
}

/** @brief Preflight selected media files and tools, retaining no descriptors or state. */
int reliefos_boot_validate_payload(const struct reliefos_boot_install_options *options)
{
    if (!options_valid(options)) return -EINVAL;
    if (options->root_partuuid && *options->root_partuuid) {
        char uuid[37];
        int error = root_uuid_text(options->root_partuuid, uuid);
        if (error || !options->disk_path) return error ? error : -EINVAL;
    }
    int root = boot_open_directory(options->source);
    if (root < 0) return root;
    close(root);
    const char *const common[] = {"grub/grub.cfg", "grub/kernel-slots.cfg",
        "grub/fonts/leonos-unicode.pf2", "grub/theme/theme.txt"};
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
        int result = payload_file(options, common[i]);
        if (result) return result;
    }
    int result = options->install_uefi ? payload_file(options, "EFI/BOOT/BOOTX64.EFI") : 0;
    if (!result && options->install_bios) result = bios_payload(options);
    if (!result) result = boot_walk_payload(options, -1, 0);
    if (!result) {
        root = boot_open_directory(options->source);
        if (root < 0) return root;
        struct reliefos_kernel_slot identity;
        result = kernel_bundle_select(root, &identity, NULL);
        close(root);
    }
    return result;
}

/** @brief Recognize complete supported whole-disk names, never a partition node. */
static int whole_disk(const char *path)
{
    if (!path) return 0;
    const char *p;
    if (!strncmp(path, "/dev/sd", 7)) {
        p = path + 7;
        if (*p < 'a' || *p > 'z') return 0;
        while (*p >= 'a' && *p <= 'z') ++p;
        return !*p;
    }
    if (strncmp(path, "/dev/nvme", 9)) return 0;
    p = path + 9;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) ++p;
    if (*p++ != 'n' || *p < '1' || *p > '9') return 0;
    while (isdigit((unsigned char)*p)) ++p;
    return !*p;
}

/** @brief Refuse a mounted BIOS extent, including mount sources named by aliases. */
static int bios_unmounted(const char *path, dev_t device)
{
    FILE *stream = setmntent("/proc/self/mounts", "r");
    if (!stream) return errno ? -errno : -EIO;
    int result = 0;
    struct mntent *entry;
    while ((entry = getmntent(stream))) {
        struct stat source;
        if (!strcmp(entry->mnt_fsname, path) ||
            (!stat(entry->mnt_fsname, &source) && S_ISBLK(source.st_mode) && source.st_rdev == device)) {
            result = -EBUSY;
            break;
        }
    }
    if (!result && ferror(stream)) result = -EIO;
    endmntent(stream);
    return result;
}

/**
 * @brief Check the disk before partitioning, without requiring a boot filesystem.
 * Source files must be on a different device from every selected-disk partition.
 * The installer may omit the existing BIOS-space check only when its validated
 * fresh default GPT plan will create that extent; installation checks it again.
 */
int reliefos_boot_validate_disk(const struct reliefos_boot_install_options *options, int require_space)
{
    if (!options_valid(options) || !whole_disk(options->disk_path)) return -EINVAL;
    struct stat disk, source;
    if (lstat(options->disk_path, &disk) < 0 || stat(options->source, &source) < 0) return -errno;
    if (!S_ISBLK(disk.st_mode)) return -ENOTBLK;
    if (source.st_dev == disk.st_rdev) return -EBUSY;
    int fd = open(options->disk_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    int readonly = 0;
    int result = ioctl(fd, BLKROGET, &readonly) < 0 ? -errno : readonly ? -EROFS : 0;
    close(fd);
    if (result) return result;
    struct reliefos_block_disk_info info;
    result = reliefos_block_get_info(options->disk_path, &info);
    if (result) return result;
    if (info.sector_size != 512) return -ENOTSUP;
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0;
    result = reliefos_block_list_partitions(options->disk_path, parts, RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    if (result) return result;
    if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) return -EOVERFLOW;
    int bios = 0;
    for (uint32_t i = 0; i < count; ++i) {
        struct stat partition;
        if (stat(parts[i].path, &partition) < 0) return -errno;
        if (!S_ISBLK(partition.st_mode)) return -ENOTBLK;
        if (source.st_dev == partition.st_rdev) return -EBUSY;
        if (options->install_bios && parts[i].gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT) {
            if (require_space && parts[i].filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN) return -EINVAL;
            result = bios_unmounted(parts[i].path, partition.st_rdev);
            if (result) return result;
        }
        if (parts[i].gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT &&
            parts[i].first_lba >= 34 && parts[i].sector_count >= 2048 &&
            parts[i].first_lba < info.sector_count &&
            parts[i].sector_count <= info.sector_count - parts[i].first_lba) bios = 1;
    }
    return options->install_bios && require_space && !bios ? -ENOSPC : 0;
}

/** @brief Check a published nonempty regular boot file through safe ancestors. */
static int installed_file(int root, const char *relative)
{
    int fd = boot_open_file(root, relative);
    if (fd < 0) return fd;
    struct stat st;
    int result = fstat(fd, &st) < 0 ? -errno : st.st_size > 0 ? 0 : -EINVAL;
    close(fd);
    return result;
}

/** @brief Resolve the explicit or sole Linux root on the already selected disk. */
static int target_root_uuid(const struct reliefos_boot_install_options *options, char out[37])
{
    char wanted[37] = {0};
    if (options->root_partuuid && *options->root_partuuid) {
        int error = root_uuid_text(options->root_partuuid, wanted);
        if (error) return error;
    }
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0;
    int result = reliefos_block_list_partitions(options->disk_path, parts,
        RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    if (result) return result;
    if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) return -EOVERFLOW;
    unsigned found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (parts[i].gpt_type != RELIEFOS_BLOCK_GPT_LINUX) continue;
        char uuid[37];
        result = reliefos_block_partition_uuid(options->disk_path, parts[i].index, uuid);
        if (result) return result;
        if (wanted[0] && strcmp(wanted, uuid)) continue;
        if (++found > 1) return -ENOTUNIQ;
        memcpy(out, uuid, sizeof(uuid));
    }
    return found ? 0 : -ENOENT;
}

/** @brief Bind a checked final root to the exact complete kernel carried by media.
 * @param options Borrowed validated media/disk settings.
 * @param path Absolute final root, opened without symlink components.
 * @param identity Optional complete media identity output on success.
 * @return Zero for matching checksums, module membership and selected disk root;
 * negative errno otherwise. No filesystem or firmware writes are performed.
 */
static int checked_root(const struct reliefos_boot_install_options *options, const char *path,
                         struct reliefos_kernel_slot *identity)
{
    if (!options_valid(options) || !path) return -EINVAL;
    int media = boot_open_directory(options->source);
    if (media < 0) return media;
    struct reliefos_kernel_slot expected;
    int result = kernel_bundle_select(media, &expected, NULL);
    close(media);
    if (result) return result;
    struct kernel_bundle_checked bundle = {.root = -1};
    result = kernel_bundle_check_open(&bundle, path, expected.release);
    expected.generation = 1;
    if (!result && !reliefos_kernel_slot_equal(&expected, &bundle.identity)) result = -EINVAL;
    if (!result && options->disk_path) {
        struct stat root;
        char wanted[37];
        result = fstat(bundle.root, &root) ? -errno : target_root_uuid(options, wanted);
        struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
        uint32_t count = 0;
        if (!result) result = reliefos_block_list_partitions(options->disk_path, parts,
            RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
        if (!result && count > RELIEFOS_BLOCK_MAX_PARTITIONS) result = -EOVERFLOW;
        int belongs = 0;
        for (uint32_t i = 0; !result && i < count; ++i) {
            if (parts[i].gpt_type != RELIEFOS_BLOCK_GPT_LINUX) continue;
            char uuid[37]; struct stat partition;
            result = reliefos_block_partition_uuid(options->disk_path, parts[i].index, uuid);
            if (result || strcmp(wanted, uuid)) continue;
            if (stat(parts[i].path, &partition)) { result = -errno; break; }
            if (S_ISBLK(partition.st_mode) && partition.st_rdev == root.st_dev) belongs = 1;
        }
        if (!result && !belongs) result = -EXDEV;
    }
    if (!result && identity) *identity = expected;
    kernel_bundle_check_close(&bundle);
    return result;
}

int reliefos_boot_validate_root(const struct reliefos_boot_install_options *options, const char *root)
{
    return checked_root(options, root, NULL);
}

/** @brief Decode the versioned mode record, checking actual loader files too. */
int reliefos_boot_probe(const char *disk_path, const char *boot_dir, uint32_t *modes)
{
    if (!modes) return -EINVAL;
    *modes = 0;
    int root = boot_open_directory(boot_dir);
    if (root < 0) return root;
    int fd = boot_open_file(root, "reliefos/config/bootloaders.conf");
    int result = 0;
    if (fd == -ENOENT) {
        result = installed_file(root, "EFI/BOOT/BOOTX64.EFI");
        if (!result) *modes = RELIEFOS_BOOT_UEFI;
        else if (result == -ENOENT) result = 0;
    } else if (fd < 0) result = fd;
    else {
        char record[64];
        size_t length = 0;
        while (length < sizeof(record) - 1) {
            ssize_t got = read(fd, record + length, sizeof(record) - 1 - length);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) { result = -errno; break; }
            if (!got) break;
            length += (size_t)got;
        }
        close(fd);
        record[length] = 0;
        int found = 0;
        for (unsigned value = 0; !result && value < 4; ++value) {
            char expected[64];
            int n = snprintf(expected, sizeof(expected), "version=1\nbios=%u\nuefi=%u\n", value & 1u, (value >> 1) & 1u);
            if ((size_t)n == length && !memcmp(expected, record, length)) {
                *modes = value; found = 1; break;
            }
        }
        if (!result && !found) result = -EINVAL;
        if (!result && (*modes & RELIEFOS_BOOT_UEFI)) result = installed_file(root, "EFI/BOOT/BOOTX64.EFI");
        if (!result && (*modes & RELIEFOS_BOOT_BIOS)) result = installed_file(root, "grub/i386-pc/core.img");
        if (!result && (*modes & RELIEFOS_BOOT_BIOS) && disk_path) {
            int disk = open(disk_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (disk < 0) result = -errno;
            else {
                unsigned char sector[512];
                ssize_t got;
                do { got = read(disk, sector, sizeof(sector)); } while (got < 0 && errno == EINTR);
                if (got != sizeof(sector)) result = got < 0 ? -errno : -EIO;
                else {
                    unsigned code = 0;
                    for (unsigned i = 0; i < 440; ++i) code |= sector[i];
                    if (!code || sector[510] != 0x55 || sector[511] != 0xaa) result = -EINVAL;
                }
                close(disk);
            }
        }
    }
    close(root);
    if (result) *modes = 0;
    return result;
}

/** @brief Validate selected disk ownership and all planned paths before any publication. */
int reliefos_boot_validate_target(const struct reliefos_boot_install_options *options)
{
    if (!options_valid(options) || (options->install_bios && !options->disk_path)) return -EINVAL;
    int root = boot_open_directory(options->boot_dir);
    if (root < 0) return root;
    int result = 0;
    if (options->disk_path) {
        result = reliefos_boot_validate_disk(options, 1);
        struct stat boot;
        if (!result && fstat(root, &boot) < 0) result = -errno;
        struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
        uint32_t count = 0;
        if (!result) result = reliefos_block_list_partitions(options->disk_path, parts,
            RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
        int belongs = 0;
        if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) result = -EOVERFLOW;
        for (uint32_t i = 0; !result && i < count; ++i) {
            struct stat partition;
            if (stat(parts[i].path, &partition) < 0) { result = -errno; break; }
            if (parts[i].gpt_type == RELIEFOS_BLOCK_GPT_ESP &&
                parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 &&
                S_ISBLK(partition.st_mode) && partition.st_rdev == boot.st_dev) belongs = 1;
        }
        if (!result && !belongs) result = -EXDEV;
        char uuid[37];
        if (!result) result = target_root_uuid(options, uuid);
    }
    uint32_t previous;
    if (!result) result = reliefos_boot_probe(options->disk_path, options->boot_dir, &previous);
    if (!result) result = boot_walk_payload(options, root, 0);
    if (!result && options->disk_path) {
        int file = boot_open_file(root, "reliefos/config/root.cfg");
        if (file >= 0) close(file);
        else if (file != -ENOENT) result = file;
    }
    close(root);
    return result;
}

/**
 * @brief Execute one preflighted installation with the boot menu published last.
 * No global environment or locks are changed. The caller owns the target and
 * serializes installs. Errors can leave completed file or raw-disk stages in
 * place; they never report a failed GRUB invocation as a successful install.
 */
int reliefos_boot_install_at_root(const struct reliefos_boot_install_options *options, const char *installed_root)
{
    int result = reliefos_boot_validate_payload(options);
    if (!result) result = reliefos_boot_validate_target(options);
    struct reliefos_kernel_slot identity;
    if (!result) result = checked_root(options, installed_root, &identity);
    if (result) return result;
    uint32_t modes = 0;
    if (options->preserve_rollback) {
        result = reliefos_boot_probe(options->disk_path, options->boot_dir, &modes);
        if (result) return result;
    }
    int root = boot_open_directory(options->boot_dir);
    if (root < 0) return root;
    result = reliefos_kernel_bundle_publish(installed_root, options->boot_dir, identity.release);
    if (!result) result = boot_walk_payload(options, root, 1);
    if (!result && options->install_bios) {
        fprintf(stderr, "[boot-install] installing BIOS loader on %s\n", options->disk_path);
        result = boot_install_bios(options);
    }
    if (!result && fsync(root) < 0) result = -errno;
    if (!result && options->disk_path) {
        char uuid[37], config[96];
        result = target_root_uuid(options, uuid);
        if (!result) {
            int n = snprintf(config, sizeof(config), "set reliefos_root=PARTUUID=%s\n", uuid);
            result = boot_publish_record(options, root, "reliefos/config/root.cfg", config, (size_t)n);
        }
    }
    if (!result) {
        modes |= options->install_bios ? RELIEFOS_BOOT_BIOS : 0;
        modes |= options->install_uefi ? RELIEFOS_BOOT_UEFI : 0;
        char record[64];
        int n = snprintf(record, sizeof(record), "version=1\nbios=%u\nuefi=%u\n", modes & 1u, (modes >> 1) & 1u);
        result = boot_publish_record(options, root, "reliefos/config/bootloaders.conf", record, (size_t)n);
    }
    if (!result) result = boot_publish_file(options, root, "grub/grub.cfg", 0);
    if (!result) result = boot_retire_legacy(root);
    close(root);
    return result;
}

int reliefos_boot_install(const struct reliefos_boot_install_options *options)
{
    return reliefos_boot_install_at_root(options, "/");
}
