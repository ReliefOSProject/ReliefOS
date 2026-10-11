/* Installer disk/partition and payload operations shared by the Motif and TTY
 * frontends.  The wizard state lives here; the frontends repaint through
 * refresh_ui() and drive the page machine via the model in model.h. */
#include <reliefos/fs.h>
#include <reliefos/blockdev.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>
#include "../locale_settings.h"
#include "installer_directory.h"
#include "installer_setup.h"
#include "../../auth/standard_accounts.h"
#include "installer_copy.h"
#include "install_ops.h"
#include "model.h"
#include <errno.h>
#include <libintl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned long reliefos_uptime_ms(void);

#define T(s) gettext(s)

uint8_t page = INSTALLER_PAGE_LANGUAGE;
uint8_t install_mode = INSTALLER_MODE_FRESH;
struct reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS];
uint32_t disk_count;
int32_t selected_disk = -1;
struct reliefos_block_partition partitions[RELIEFOS_BLOCK_MAX_PARTITIONS];
uint32_t partition_count;
int32_t selected_partition = -1;
int32_t installer_root_partition = -1;
int32_t installer_esp_partition = -1;
uint8_t installer_partition_auto = 1;
uint8_t installer_partition_table_replaced;
static char partition_disk_path[RELIEFOS_BLOCK_PATH_LEN];
char confirm_text[16];
struct installer_setup setup;
char status_text[128] = "Ready";
char detail_text[128] = "";
char progress_text[256] = "Ready";
uint32_t progress_value;
uint8_t install_success;
int reboot_error;
uint8_t install_running;
uint8_t installer_tty_mode;
static uint32_t copy_total;
static uint32_t copy_done;
static uint64_t copy_total_bytes;
static uint64_t copy_done_bytes;
static uint32_t tty_last_progress = 0xffffffffu;
static char tty_last_status[128];
static uint8_t copy_buf[COPY_BUF_SIZE];

int text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int name_is_dot(const char *name)
{
    return text_eq(name, ".") || text_eq(name, "..");
}

void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (!buf || !pos || *pos + 1 >= cap) {
        return -1;
    }
    buf[(*pos)++] = ch;
    buf[*pos] = 0;
    return 0;
}

static int append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text)
{
    for (uint32_t i = 0; text && text[i]; ++i) {
        if (append_char(buf, pos, cap, text[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

static int append_u64(char *buf, uint32_t *pos, uint32_t cap, uint64_t value)
{
    char tmp[24];
    uint32_t n = 0;
    if (value == 0) {
        return append_char(buf, pos, cap, '0');
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        if (append_char(buf, pos, cap, tmp[--n]) < 0) {
            return -1;
        }
    }
    return 0;
}

static int append_i32(char *buf, uint32_t *pos, uint32_t cap, int32_t value)
{
    uint32_t mag;
    if (value < 0) {
        if (append_char(buf, pos, cap, '-') < 0) {
            return -1;
        }
        mag = (uint32_t)(-value);
    } else {
        mag = (uint32_t)value;
    }
    return append_u64(buf, pos, cap, mag);
}

void set_status(const char *status, const char *detail)
{
    copy_text(status_text, sizeof(status_text), status);
    copy_text(detail_text, sizeof(detail_text), detail);
    /* The status line is the only account of why an install or update stopped,
     * and it lives in the GUI. Mirror it to the serial console so a failure can
     * be diagnosed from a log instead of a screenshot: without this, a reported
     * "mount failed ret=-22" gives no way to tell which check produced it. */
    printf("[installer.elf] status: %s%s%s\n",
           status ? status : "",
           (detail && detail[0]) ? " - " : "",
           (detail && detail[0]) ? detail : "");
}

static void set_progress_text(const char *status, const char *detail)
{
    uint32_t pos = 0;
    progress_text[0] = 0;
    (void)append_text(progress_text, &pos, sizeof(progress_text), status ? status : "");
    if (detail && detail[0]) {
        (void)append_text(progress_text, &pos, sizeof(progress_text), " - ");
        (void)append_text(progress_text, &pos, sizeof(progress_text), detail);
    }
}

void set_error_status(const char *prefix, int ret)
{
    uint32_t pos = 0;
    detail_text[0] = 0;
    append_text(detail_text, &pos, sizeof(detail_text), prefix);
    append_text(detail_text, &pos, sizeof(detail_text), " ret=");
    append_i32(detail_text, &pos, sizeof(detail_text), ret);
    copy_text(status_text, sizeof(status_text), T("Installation failed"));
}

static int path_join(char *dst, uint32_t cap, const char *base, const char *name)
{
    uint32_t pos = 0;
    if (!dst || cap == 0 || !base || !name) {
        return -1;
    }
    dst[0] = 0;
    if (append_text(dst, &pos, cap, base) < 0) {
        return -1;
    }
    if (pos > 0 && dst[pos - 1] != '/') {
        if (append_char(dst, &pos, cap, '/') < 0) {
            return -1;
        }
    }
    while (*name == '/') ++name;
    return append_text(dst, &pos, cap, name);
}

const char *mode_action_text(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? T("Update") : T("Install");
}

const char *mode_progress_title(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? T("Updating ReliefOS")
                                               : T("Installing ReliefOS");
}

void set_disk_select_status(void)
{
    if (install_mode == INSTALL_MODE_UPDATE) {
        set_status(T("Select the disk to update"),
                   T("Setup will check for an existing ReliefOS system."));
    } else {
        set_status(T("Select the target disk"),
                   T("Automatic mode replaces its partition table; manual mode can preserve other partitions."));
    }
}

void format_disk_line(char *buf, uint32_t cap,
                      const struct reliefos_block_disk_info *disk)
{
    if (!disk) {
        copy_text(buf, cap, "");
        return;
    }
    installer_model_format_disk_line(buf, cap, disk->id, disk->name,
                                    disk->sector_count, disk->sector_size);
}

static int partition_at_selection(struct reliefos_block_partition **out)
{
    if (!out || selected_partition < 0 ||
        (uint32_t)selected_partition >= partition_count) return -EINVAL;
    *out = &partitions[selected_partition];
    return 0;
}

void refresh_partitions(void)
{
    int32_t previous_index = -1;
    int disk_changed = selected_disk < 0 ||
        (uint32_t)selected_disk >= disk_count ||
        !text_eq(partition_disk_path, disks[selected_disk].path);
    if (disk_changed) {
        selected_partition = -1;
        installer_root_partition = -1;
        installer_esp_partition = -1;
        installer_partition_table_replaced = 0;
        partition_disk_path[0] = 0;
        if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count)
            copy_text(partition_disk_path, sizeof(partition_disk_path),
                      disks[selected_disk].path);
    } else if (selected_partition >= 0 && (uint32_t)selected_partition < partition_count) {
        previous_index = (int32_t)partitions[selected_partition].index;
    }
    partition_count = 0;
    selected_partition = -1;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) return;
    if (reliefos_block_list_partitions(disks[selected_disk].path, partitions,
                                       RELIEFOS_BLOCK_MAX_PARTITIONS,
                                       &partition_count) < 0) {
        partition_count = 0;
    }
    if (previous_index >= 0) {
        for (uint32_t i = 0; i < partition_count; ++i) {
            if ((int32_t)partitions[i].index == previous_index) {
                selected_partition = (int32_t)i;
                break;
            }
        }
    }
    if (installer_root_partition >= 0 || installer_esp_partition >= 0) {
        int root_seen = 0, esp_seen = 0;
        for (uint32_t i = 0; i < partition_count; ++i) {
            if ((int32_t)partitions[i].index == installer_root_partition) root_seen = 1;
            if ((int32_t)partitions[i].index == installer_esp_partition) esp_seen = 1;
        }
        if (!root_seen) installer_root_partition = -1;
        if (!esp_seen) installer_esp_partition = -1;
    }
}

void format_partition_line(char *buf, uint32_t cap,
                           const struct reliefos_block_partition *partition)
{
    uint64_t size = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    if (!partition) return;
    if (disks[selected_disk].sector_size)
        size = (partition->sector_count * disks[selected_disk].sector_size) /
               (1024ULL * 1024ULL);
    snprintf(buf, cap, "Partition %u  %s  %llu MiB  %s  %s%s",
             partition->index + 1u,
             partition->name[0] ? partition->name : "Unnamed",
             (unsigned long long)size,
             reliefos_block_filesystem_name(partition->filesystem),
             reliefos_block_gpt_type_name(partition->gpt_type),
             (int32_t)partition->index == installer_root_partition ? "  [root]" :
             ((int32_t)partition->index == installer_esp_partition ? "  [ESP]" : ""));
}

int installer_partition_plan_valid(void)
{
    int root_seen = 0, esp_seen = 0;
    if (installer_partition_auto) return selected_disk >= 0 &&
        (uint32_t)selected_disk < disk_count;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count ||
        installer_root_partition < 0 || installer_esp_partition < 0 ||
        installer_root_partition == installer_esp_partition)
        return 0;
    for (uint32_t i = 0; i < partition_count; ++i) {
        if ((int32_t)partitions[i].index == installer_root_partition)
            root_seen = partitions[i].gpt_type == RELIEFOS_BLOCK_GPT_LINUX &&
                        partitions[i].sector_count >= 128ULL * 2048ULL;
        if ((int32_t)partitions[i].index == installer_esp_partition)
            esp_seen = partitions[i].gpt_type == RELIEFOS_BLOCK_GPT_ESP &&
                       partitions[i].sector_count >= 128ULL * 2048ULL;
    }
    return root_seen && esp_seen;
}

static int installer_partition_disk(void)
{
    return selected_disk >= 0 && (uint32_t)selected_disk < disk_count ? 0 : -EINVAL;
}

static int installer_manual_partition_disk(void)
{
    int ret = installer_partition_disk();
    if (!ret) installer_partition_auto = 0;
    return ret;
}

int installer_partition_initialize(void)
{
    int ret = installer_manual_partition_disk();
    if (!ret) ret = reliefos_block_gpt_initialize(disks[selected_disk].path, 1);
    if (!ret) {
        installer_partition_table_replaced = 1;
        installer_root_partition = installer_esp_partition = -1;
        refresh_partitions();
    }
    return ret;
}

int installer_partition_auto_layout(void)
{
    uint32_t esp, root, root_mib;
    struct reliefos_block_disk_info info;
    int ret = installer_partition_disk();
    if (!ret) ret = reliefos_block_get_info(disks[selected_disk].path, &info);
    if (!ret) ret = reliefos_block_gpt_initialize(disks[selected_disk].path, 1);
    if (!ret) installer_partition_table_replaced = 1;
    if (!ret) ret = reliefos_block_gpt_create(disks[selected_disk].path,
                                               RELIEFOS_BLOCK_FILESYSTEM_FAT32,
                                               128, "RELIEFOS_ESP", &esp);
    if (!ret) ret = reliefos_block_gpt_set_type(disks[selected_disk].path, esp,
                                                 RELIEFOS_BLOCK_GPT_ESP);
    if (!ret) {
        root_mib = info.sector_count * (uint64_t)info.sector_size /
                   (1024ULL * 1024ULL);
        if (root_mib > 256u) root_mib -= 131u; else root_mib = 64u;
    }
    if (!ret) ret = reliefos_block_gpt_create(disks[selected_disk].path,
                                               RELIEFOS_BLOCK_FILESYSTEM_EXT4,
                                               root_mib, "RELIEFOS_ROOT", &root);
    if (!ret) ret = reliefos_block_gpt_set_type(disks[selected_disk].path, root,
                                                 RELIEFOS_BLOCK_GPT_LINUX);
    if (!ret) {
        installer_partition_auto = 0;
        installer_root_partition = (int32_t)root;
        installer_esp_partition = (int32_t)esp;
        refresh_partitions();
    }
    return ret;
}

int installer_partition_create(uint32_t filesystem, uint32_t size_mib,
                               const char *name)
{
    uint32_t index;
    int ret = installer_manual_partition_disk();
    if (!ret) ret = reliefos_block_gpt_create(disks[selected_disk].path, filesystem,
                                               size_mib, name, &index);
    if (!ret) {
        refresh_partitions();
        for (uint32_t i = 0; i < partition_count; ++i)
            if (partitions[i].index == index) selected_partition = (int32_t)i;
    }
    return ret;
}

int installer_partition_resize(uint32_t size_mib)
{
    struct reliefos_block_partition *partition;
    int ret = partition_at_selection(&partition);
    if (!ret) installer_partition_auto = 0;
    if (!ret) ret = reliefos_block_gpt_resize(disks[selected_disk].path,
                                               partition->index, size_mib);
    if (!ret) refresh_partitions();
    return ret;
}

int installer_partition_rename(const char *name)
{
    struct reliefos_block_partition *partition;
    int ret = partition_at_selection(&partition);
    if (!ret) installer_partition_auto = 0;
    if (!ret) ret = reliefos_block_gpt_set_name(disks[selected_disk].path,
                                                partition->index, name);
    if (!ret) refresh_partitions();
    return ret;
}

int installer_partition_set_type(uint32_t type)
{
    struct reliefos_block_partition *partition;
    int32_t index;
    int ret = partition_at_selection(&partition);
    if (!ret) {
        installer_partition_auto = 0;
        index = (int32_t)partition->index;
    }
    if (!ret) ret = reliefos_block_gpt_set_type(disks[selected_disk].path,
                                                partition->index, type);
    if (!ret) {
        if (type == RELIEFOS_BLOCK_GPT_ESP) {
            installer_esp_partition = index;
            if (installer_root_partition == index) installer_root_partition = -1;
        } else if (type == RELIEFOS_BLOCK_GPT_LINUX) {
            installer_root_partition = index;
            if (installer_esp_partition == index) installer_esp_partition = -1;
        } else {
            if (installer_root_partition == index) installer_root_partition = -1;
            if (installer_esp_partition == index) installer_esp_partition = -1;
        }
        refresh_partitions();
    }
    return ret;
}

int installer_partition_delete(void)
{
    struct reliefos_block_partition *partition;
    int ret = partition_at_selection(&partition);
    if (!ret) installer_partition_auto = 0;
    if (!ret) ret = reliefos_block_gpt_delete(disks[selected_disk].path,
                                               partition->index);
    if (!ret) {
        if (installer_root_partition == (int32_t)partition->index) installer_root_partition = -1;
        if (installer_esp_partition == (int32_t)partition->index) installer_esp_partition = -1;
        refresh_partitions();
    }
    return ret;
}

int installer_partition_format(uint32_t filesystem, const char *label)
{
    struct reliefos_block_partition *partition;
    int ret = partition_at_selection(&partition);
    if (!ret) installer_partition_auto = 0;
    if (!ret) ret = reliefos_block_format(partition->path, filesystem, label);
    if (!ret) refresh_partitions();
    return ret;
}

void reset_confirm(void)
{
    confirm_text[0] = 0;
}

static char installer_root_uuid[37], installer_esp_uuid[37];

/* The feature profile is stated explicitly instead of inheriting mke2fs
 * defaults: it must match the images built by tools/build/images.sh, and it
 * must stay inside the kernel's accepted mask (extents, 64bit, flex_bg,
 * filetype, csum_seed incompat; metadata_csum/extra_isize/dir_nlink/... ro). */
#define INSTALLER_MKFS_EXT4_FEATURES \
    "none,filetype,extents,dir_index,metadata_csum,64bit,flex_bg," \
    "has_journal,large_file,huge_file,extra_isize"

static int installer_format_ext4(const char *path)
{
    char *const argv[] = { (char *)"mkfs.ext4", (char *)"-F",
                           (char *)"-b", (char *)"4096",
                           (char *)"-I", (char *)"256",
                           (char *)"-O", (char *)INSTALLER_MKFS_EXT4_FEATURES,
                           (char *)path, NULL };
    pid_t child;
    int status;
    int ret = posix_spawnp(&child, "mkfs.ext4", NULL, NULL, argv, environ);
    if (ret) return -ret;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -EIO;
    return 0;
}

/* Commit fstab only after the root payload is copied; failure aborts install. */
static int installer_write_fstab(void)
{
    char text[320], temporary[] = INSTALL_ROOT_MOUNT "/etc/.fstab.XXXXXX";
    if (!installer_root_uuid[0] || !installer_esp_uuid[0]) return -EINVAL;
    int length = snprintf(text, sizeof(text),
        "# <source> <mountpoint> <type> <options> <dump> <pass>\n"
        "/dev/disk/by-partuuid/%s / ext4 defaults 0 1\n"
        "/dev/disk/by-partuuid/%s /boot vfat defaults 0 2\n",
        installer_root_uuid, installer_esp_uuid);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    int fd = mkstemp(temporary), ret = 0;
    if (fd < 0) return -errno;
    for (int done = 0; done < length;) {
        ssize_t count = write(fd, text + done, (size_t)(length - done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ret = count < 0 ? -errno : -EIO; break; }
        done += (int)count;
    }
    if (!ret && fchown(fd, 0, 0) < 0) ret = -errno;
    if (!ret && fchmod(fd, 0644) < 0) ret = -errno;
    if (!ret && fsync(fd) < 0) ret = -errno;
    if (close(fd) < 0 && !ret) ret = -errno;
    if (!ret && rename(temporary, INSTALL_ROOT_MOUNT "/etc/fstab") < 0) ret = -errno;
    if (ret < 0) (void)unlink(temporary);
    return ret;
}

static int installer_target_partitions(const char *disk_path, int fresh,
                                       char *esp_path, uint32_t esp_cap,
                                       char *root_path, uint32_t root_cap,
                                       uint32_t *root_filesystem)
{
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0, esp = UINT32_MAX, root = UINT32_MAX;
    int ret;
    if (!disk_path || !esp_path || !root_path || !root_filesystem) return -EINVAL;
    *root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    if (fresh && installer_partition_auto) {
        struct reliefos_block_disk_info info;
        uint32_t root_mib;
        ret = reliefos_block_get_info(disk_path, &info);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_initialize(disk_path, 1);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_create(disk_path, RELIEFOS_BLOCK_FILESYSTEM_FAT32,
                                      128, "RELIEFOS_ESP", &esp);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_set_type(disk_path, esp, RELIEFOS_BLOCK_GPT_ESP);
        if (ret < 0) return ret;
        root_mib = (uint32_t)((info.sector_count * info.sector_size) / (1024ULL * 1024ULL));
        /* Match main's install_write_gpt layout: a fixed 128 MiB ESP, then
         * the ext4 root consumes the remaining usable GPT area. The extra
         * 3 MiB reserve covers the primary/backup GPT and 1 MiB alignment
         * slop after the ESP. */
        if (root_mib > 256u) root_mib -= 131u; else root_mib = 64u;
        ret = reliefos_block_gpt_create(disk_path, RELIEFOS_BLOCK_FILESYSTEM_EXT4,
                                      root_mib, "RELIEFOS_ROOT", &root);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_set_type(disk_path, root, RELIEFOS_BLOCK_GPT_LINUX);
        if (ret < 0) return ret;
        /* Format through the partition nodes after the GPT reread. */
        ret = reliefos_block_partition_path(disk_path, esp, esp_path, esp_cap);
        if (ret < 0) return ret;
        ret = reliefos_block_partition_path(disk_path, root, root_path, root_cap);
        if (ret < 0) return ret;
        ret = reliefos_block_format(esp_path, RELIEFOS_BLOCK_FILESYSTEM_FAT32, "RELIEFOS");
        if (ret < 0) return ret;
        *root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
        ret = installer_format_ext4(root_path);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, root, installer_root_uuid);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, esp, installer_esp_uuid);
        return ret;
    }
    ret = reliefos_block_list_partitions(disk_path, parts, RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    printf("[installer.elf] block list partitions ret=%d count=%u disk=%s\n",
           ret, count, disk_path ? disk_path : "?");
    if (ret < 0) return ret;
    if (fresh) {
        if (installer_root_partition < 0 || installer_esp_partition < 0) return -EINVAL;
        for (uint32_t i = 0; i < count && i < RELIEFOS_BLOCK_MAX_PARTITIONS; ++i) {
            if ((int32_t)parts[i].index == installer_esp_partition) esp = i;
            if ((int32_t)parts[i].index == installer_root_partition) root = i;
        }
        if (esp == UINT32_MAX || root == UINT32_MAX || esp == root) return -ENOENT;
        if (parts[esp].sector_count < 128ULL * 2048ULL ||
            parts[root].sector_count < 128ULL * 2048ULL) return -ENOSPC;
        copy_text(esp_path, esp_cap, parts[esp].path);
        copy_text(root_path, root_cap, parts[root].path);
        ret = reliefos_block_format(esp_path, RELIEFOS_BLOCK_FILESYSTEM_FAT32, "RELIEFOS");
        if (ret < 0) return ret;
        *root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
        ret = installer_format_ext4(root_path);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, parts[root].index,
                                                       installer_root_uuid);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, parts[esp].index,
                                                       installer_esp_uuid);
        return ret;
    }
    for (uint32_t i = 0; i < count && i < RELIEFOS_BLOCK_MAX_PARTITIONS; ++i) {
        printf("[installer.elf] partition[%u] path=%s fs=%u gpt_type=%u\n",
               i, parts[i].path, parts[i].filesystem, parts[i].gpt_type);
        if (parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 && esp == UINT32_MAX) esp = i;
        if ((parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
             parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ||
             parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) && root == UINT32_MAX) root = i;
    }
    if (esp == UINT32_MAX || root == UINT32_MAX) {
        printf("[installer.elf] block partitions missing esp=%u root=%u\n", esp, root);
        return -ENOENT;
    }
    copy_text(esp_path, esp_cap, parts[esp].path);
    copy_text(root_path, root_cap, parts[root].path);
    *root_filesystem = parts[root].filesystem;
    return 0;
}

static uint32_t installer_root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;

static int installer_mount_targets(const char *disk_path, int fresh)
{
    char esp_path[RELIEFOS_BLOCK_PATH_LEN], root_path[RELIEFOS_BLOCK_PATH_LEN];
    uint32_t root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    const char *root_fs_name = NULL;
    struct stat mountpoint;
    int ret;
    /* /target belongs to the installer runtime, not the installed rootfs. */
    if (mkdir(INSTALL_ROOT_MOUNT, 0755) < 0 && errno != EEXIST) {
        ret = -errno;
        printf("[installer.elf] mountpoint mkdir failed path=%s ret=%d\n", INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    if (lstat(INSTALL_ROOT_MOUNT, &mountpoint) < 0) {
        ret = -errno;
        printf("[installer.elf] mountpoint lstat failed path=%s ret=%d\n", INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    if (!S_ISDIR(mountpoint.st_mode)) {
        printf("[installer.elf] mountpoint is not a directory path=%s\n", INSTALL_ROOT_MOUNT);
        return -ENOTDIR;
    }
    /* Update inspection leaves these mounted. Release children first, before
     * fresh installation rewrites the GPT or formats either filesystem. */
    const char *targets[] = {INSTALL_ESP_MOUNT, INSTALL_ROOT_MOUNT};
    for (uint32_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (umount2(targets[i], 0) < 0 && errno != ENOENT && errno != EINVAL) {
            ret = -errno;
            printf("[installer.elf] unmount target failed path=%s ret=%d\n", targets[i], ret);
            return ret;
        }
    }
    ret = installer_target_partitions(disk_path, fresh, esp_path, sizeof(esp_path),
                                          root_path, sizeof(root_path),
                                          &root_filesystem);
    if (ret < 0) {
        printf("[installer.elf] mount target partitions failed ret=%d disk=%s fresh=%d\n",
               ret, disk_path ? disk_path : "?", fresh);
        return ret;
    }
    installer_root_filesystem = root_filesystem;
    printf("[installer.elf] mount targets disk=%s root=%s esp=%s fresh=%d root_fs=%u\n",
           disk_path ? disk_path : "?", root_path, esp_path, fresh,
           root_filesystem);
    if (root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) {
        root_fs_name = "exfat";
    } else if (root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
               root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4) {
        root_fs_name = root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ? "ext4" : "ext2";
    } else {
        /* An unknown root filesystem must not reach mount() with a null type. */
        printf("[installer.elf] unsupported root filesystem fs=%u\n", root_filesystem);
        return -ENODEV;
    }
    if (mount(root_path, INSTALL_ROOT_MOUNT, root_fs_name, 0, NULL) < 0) {
        printf("[installer.elf] mount root failed path=%s errno=%d\n", root_path, errno);
        return -errno;
    }
    if (mkdir(INSTALL_ESP_MOUNT, 0755) < 0 && errno != EEXIST) {
        ret = -errno;
        printf("[installer.elf] mount esp mkdir failed path=%s errno=%d\n",
               INSTALL_ESP_MOUNT, errno);
        (void)umount2(INSTALL_ROOT_MOUNT, 0);
        return ret;
    }
    if (mount(esp_path, INSTALL_ESP_MOUNT, "fat32", 0, NULL) < 0) {
        ret = -errno;
        printf("[installer.elf] mount esp failed path=%s errno=%d\n", esp_path, errno);
        (void)umount2(INSTALL_ROOT_MOUNT, 0);
        return ret;
    }
    return 0;
}

int confirmation_ok(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? text_eq(confirm_text, "UPDATE")
                                               : text_eq(confirm_text, "INSTALL");
}

void refresh_disks(void)
{
    uint32_t count = 0;
    int ret = reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count);
    if (ret < 0) {
        disk_count = 0;
        selected_disk = -1;
        set_error_status("Could not list disks", ret);
        refresh_ui();
        return;
    }
    disk_count = count > RELIEFOS_BLOCK_MAX_DISKS ? RELIEFOS_BLOCK_MAX_DISKS : count;
    if (disk_count == 0) {
        selected_disk = -1;
        set_status(T("No disks were found"), T("Attach a disk and click Refresh."));
    } else {
        if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
            selected_disk = 0;
        }
        set_disk_select_status();
    }
    refresh_ui();
}

static void show_progress(uint32_t value, const char *status,
                          const char *detail)
{
    if (value > 100) {
        value = 100;
    }
    progress_value = value;
    set_status(status, detail);
    set_progress_text(status, detail);
    if (installer_tty_mode) {
        /* File copying calls this for every file. Keep the CLI readable by
         * emitting only stage changes and new percentage values. */
        if (value != tty_last_progress || !text_eq(progress_text, tty_last_status)) {
            printf("[%3u%%] %s\n", value, progress_text);
            tty_last_progress = value;
            copy_text(tty_last_status, sizeof(tty_last_status), progress_text);
        }
        return;
    }
    refresh_ui();
}

static uint32_t copy_progress_percent(void)
{
    if (copy_total_bytes == 0) {
        return copy_total ? 35 + (copy_done * 60U) / copy_total : 35;
    }
    if (copy_done_bytes > copy_total_bytes) {
        copy_done_bytes = copy_total_bytes;
    }
    return 35 + (uint32_t)((copy_done_bytes * 60ULL) / copy_total_bytes);
}

static void show_copy_progress(const char *detail)
{
    static unsigned long last_present_ms;
    if (!installer_tty_mode) {
        unsigned long now = reliefos_uptime_ms();
        if (now - last_present_ms < COPY_PRESENT_INTERVAL_MS) return;
        last_present_ms = now;
    }
    show_progress(copy_progress_percent(),
                  T("Copying system files"), detail);
}

/* mkdir(3) follows the POSIX convention and returns -1 on failure, while
 * installer traversal needs the native negative errno values (in particular
 * -17 for an already-existing directory).  Keep this narrow compatibility
 * helper local to the installer instead of changing libc semantics for every
 * application. */
static int installer_mkdir(const char *path)
{
    long ret;
    struct stat status;
    if (!path) {
        return -22;
    }
    /* Existing mount roots need no creation, and links must not redirect
     * installation writes outside the selected target tree. */
    if (lstat(path, &status) == 0)
        return S_ISDIR(status.st_mode) ? -EEXIST : -ENOTDIR;
    if (errno != ENOENT) return -errno;
    ret = syscall2(SYS_mkdir, (long)path, 0755);
    if (ret == -EEXIST) {
        if (lstat(path, &status) < 0) return -errno;
        if (!S_ISDIR(status.st_mode)) return -ENOTDIR;
    }
    return ret < 0 ? (int)ret : 0;
}

static int count_files_recursive(const char *src, uint32_t *out_count)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(child, sizeof(child), src, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Payload path is too long"));
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            struct reliefos_stat st;
            if (reliefos_stat_legacy(child, &st) == 0 && st.type == RELIEFOS_FS_TYPE_FILE) {
                copy_total_bytes += st.size;
            }
            ++*out_count;
            continue;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            /* Symlink recreation is a metadata-sized work item. */
            ++*out_count;
            continue;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = count_files_recursive(child, out_count);
            if (ret < 0) {
                goto out;
            }
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

static int path_has_type(const char *path, uint32_t type)
{
    struct reliefos_stat st;
    int ret = reliefos_stat_legacy(path, &st);
    if (ret < 0) {
        return ret;
    }
    return st.type == type ? 0 : -20;
}

/* Defined below: final-symlink-aware helpers used by traversal. */
static int path_type_nofollow(const char *path);
static int copy_symlink_path(const char *src, const char *dst);
static int remove_path_recursive(const char *path)
{
    struct stat lst;
    struct reliefos_dir_entry *entries;
    /* lstat is essential here: unlinking "/target/bin" must remove the
     * usr-merge symlink itself, never recurse into /usr/bin. */
    if (lstat(path, &lst) < 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (S_ISLNK(lst.st_mode) || !S_ISDIR(lst.st_mode)) {
        return unlink(path);
    }
    entries = NULL;
    int ret;
    for (;;) {
        uint32_t count = 0;
        uint32_t removed = 0;
        free(entries);
        ret = installer_list_dir(path, &entries, &count);
        if (ret < 0) {
            goto out;
        }
        if (count == 0) {
            break;
        }
        for (uint32_t i = 0; i < count; ++i) {
            char child[RELIEFOS_FS_PATH_LEN];
            if (name_is_dot(entries[i].name)) {
                continue;
            }
            if (path_join(child, sizeof(child), path, entries[i].name) < 0) {
                ret = -1;
                goto out;
            }
            ret = remove_path_recursive(child);
            if (ret < 0) {
                goto out;
            }
            removed = 1;
        }
        if (!removed) {
            break;
        }
    }
    ret = rmdir(path);
out:
    free(entries);
    return ret;
}

static int copy_dir_recursive(const char *src, const char *dst);
static int copy_file_path(const char *src, const char *dst)
{
    int in_fd = open(src, RELIEFOS_O_RDONLY, 0);
    int out_fd = -1;
    int error = 0;
    char temporary[RELIEFOS_FS_PATH_LEN];
    struct stat source;
    long got = 0;
    uint32_t write_slice = sizeof(copy_buf);
    /* FAT32 extends the ESP chain synchronously under the kernel lock.
     * Bound each transaction so input and painting can run between writes. */
    if (strncmp(dst, INSTALL_ESP_MOUNT "/", sizeof(INSTALL_ESP_MOUNT)) == 0)
        write_slice = COPY_ESP_WRITE_SLICE;
    if (in_fd < 0) {
        return -errno;
    }
    if (fstat(in_fd, &source) < 0) { error = errno; goto done; }
    if (!S_ISREG(source.st_mode)) { error = EINVAL; goto done; }
    const char *slash = strrchr(dst, '/');
    if (!slash) { error = EINVAL; goto done; }
    int n = snprintf(temporary, sizeof(temporary), "%.*s.reliefos-copy-XXXXXX",
                     (int)(slash - dst + 1), dst);
    if (n < 0 || (size_t)n >= sizeof(temporary)) { error = ENAMETOOLONG; goto done; }
    /* A failed copy must leave the previous file (and any hard-link aliases)
     * intact. mkstemp also refuses to follow an attacker-supplied temp link. */
    out_fd = mkstemp(temporary);
    if (out_fd < 0) {
        error = errno;
        goto done;
    }
    if (!installer_tty_mode) {
        printf("[installer.elf] copying %s -> %s\n", src, dst);
    }
    show_copy_progress(dst);
    for (;;) {
        got = installer_read_chunk(in_fd, copy_buf, sizeof(copy_buf));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        long written = 0;
        while (written < got) {
            uint32_t chunk = (uint32_t)(got - written);
            if (chunk > write_slice) chunk = write_slice;
            long ret = write(out_fd, copy_buf + written, chunk);
            if (ret < 0 && errno == EINTR) continue;
            if (ret <= 0) {
                error = ret < 0 ? errno : EIO;
                goto done;
            }
            written += ret;
            copy_done_bytes += (uint64_t)ret;
            show_copy_progress(dst);
            if (written < got) (void)sched_yield();
        }
        /* read(2) may return a single 4 KiB slice. Sleeping after every short
         * read turns each slice into a full PIT tick. Yield without a timed
         * delay; progress painting remains throttled independently. */
        (void)sched_yield();
    }
    if (got < 0) { error = errno; goto done; }
    if (fchown(out_fd, source.st_uid, source.st_gid) < 0 ||
        fchmod(out_fd, source.st_mode & 07777) < 0 || fsync(out_fd) < 0) {
        error = errno;
        goto done;
    }
    if (close(out_fd) < 0) error = errno;
    out_fd = -1;
    if (!error && rename(temporary, dst) < 0) error = errno;
    if (error) unlink(temporary);
done:
    if (out_fd >= 0) {
        close(out_fd);
        unlink(temporary);
    }
    close(in_fd);
    if (error) printf("[installer.elf] copy %s -> %s failed errno=%d\n", src, dst, error);
    return -error;
}

/* Return the type of path without following a final symlink. */
static int path_type_nofollow(const char *path)
{
    struct stat status;
    if (!path || lstat(path, &status) < 0) {
        return errno ? -errno : -2;
    }
    if (S_ISLNK(status.st_mode)) return RELIEFOS_FS_TYPE_SYMLINK;
    if (S_ISDIR(status.st_mode)) return RELIEFOS_FS_TYPE_DIR;
    if (S_ISREG(status.st_mode)) return RELIEFOS_FS_TYPE_FILE;
    return RELIEFOS_FS_TYPE_DEVICE;
}

/* Recreate a symlink with the same literal target.  The Alpine root
 * layout depends on symlinks staying symlinks across installation; copying
 * the target bytes as a directory or regular file is not equivalent. */
static int copy_symlink_path(const char *src, const char *dst)
{
    char target[RELIEFOS_FS_PATH_LEN];
    char temporary[RELIEFOS_FS_PATH_LEN];
    struct stat status;
    ssize_t length;
    if (!src || !dst) return -22;
    length = readlink(src, target, sizeof(target) - 1U);
    if (length < 0) return -errno;
    if (length >= (ssize_t)sizeof(target) - 1U) return -36;
    target[length] = 0;
    if (lstat(src, &status) < 0) return -errno;
    const char *slash = strrchr(dst, '/');
    if (!slash) return -EINVAL;
    int n = snprintf(temporary, sizeof(temporary), "%.*s.reliefos-link-XXXXXX",
                     (int)(slash - dst + 1), dst);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return -ENAMETOOLONG;
    int fd = mkstemp(temporary);
    if (fd < 0) return -errno;
    int error = close(fd) < 0 ? errno : 0;
    if (unlink(temporary) < 0 && !error) error = errno;
    if (error) return -error;
    if (symlink(target, temporary) < 0) return -errno;
    if (lchown(temporary, status.st_uid, status.st_gid) < 0 ||
        rename(temporary, dst) < 0) {
        int error = errno;
        unlink(temporary);
        return -error;
    }
    return 0;
}

static int copy_dir_recursive(const char *src, const char *dst)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        printf("[installer.elf] list source dir %s ret=%d\n", src, ret);
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Copy path is too long"));
            printf("[installer.elf] copy path too long src=%s dst=%s\n", src, dst);
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = installer_mkdir(dst_child);
            if (ret == -17) {
                struct reliefos_stat dst_st;
                ret = reliefos_stat_legacy(dst_child, &dst_st);
                if (ret == 0 && dst_st.type == RELIEFOS_FS_TYPE_DIR) {
                    ret = 0;
                } else if (ret == 0) {
                    ret = -20;
                } else {
                    printf("[installer.elf] stat existing dir %s ret=%d\n", dst_child, ret);
                }
            }
            if (ret < 0) {
                printf("[installer.elf] mkdir %s ret=%d\n", dst_child, ret);
                goto out;
            }
            ret = copy_dir_recursive(src_child, dst_child);
            if (ret < 0) {
                printf("[installer.elf] recurse copy %s -> %s ret=%d\n",
                       src_child, dst_child, ret);
                goto out;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            ret = copy_file_path(src_child, dst_child);
            if (ret < 0) {
                printf("[installer.elf] copy %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(dst_child);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            ret = copy_symlink_path(src_child, dst_child);
            if (ret < 0) {
                printf("[installer.elf] symlink %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(dst_child);
        }
    }
    struct stat directory_mode;
    if (stat(src, &directory_mode) < 0 ||
        chown(dst, directory_mode.st_uid, directory_mode.st_gid) < 0 ||
        chmod(dst, directory_mode.st_mode & 07777) < 0) {
        ret = -errno;
    } else {
        ret = 0;
    }
out:
    free(entries);
    return ret;
}
static int copy_payload_ordered(void)
{
    int ret = copy_dir_recursive(INSTALL_ROOT_PAYLOAD, INSTALL_ROOT_MOUNT);
    if (!ret) ret = installer_write_fstab();
    if (!ret && installer_setup_write(&setup, INSTALL_ROOT_MOUNT) < 0) {
        ret = -errno;
        set_status(T("Account setup failed"), strerror(errno));
    }
    if (ret < 0) {
        printf("[installer.elf] root payload copy failed src=%s dst=%s ret=%d\n",
               INSTALL_ROOT_PAYLOAD, INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    ret = copy_dir_recursive(INSTALL_ESP_PAYLOAD, INSTALL_ESP_MOUNT);
    if (ret < 0) {
        printf("[installer.elf] ESP payload copy failed src=%s dst=%s ret=%d\n",
               INSTALL_ESP_PAYLOAD, INSTALL_ESP_MOUNT, ret);
        return ret;
    }

    /* The root payload normally creates this empty directory.  Verify it
     * explicitly and make the operation idempotent for filesystems that keep
     * an end-marker or directory cache across the preceding copy. */
    {
        const char *state_path = TARGET_VAR_LIB_RELIEFOS;
        struct reliefos_stat state_st;
        ret = reliefos_stat_legacy(state_path, &state_st);
        if (ret == 0) {
            if (state_st.type == RELIEFOS_FS_TYPE_DIR) {
                return 0;
            }
            printf("[installer.elf] target state has wrong type=%u path=%s\n",
                   state_st.type, state_path);
            ret = remove_path_recursive(state_path);
            if (ret < 0) {
                printf("[installer.elf] remove wrong target state %s ret=%d\n",
                       state_path, ret);
                return ret;
            }
        } else if (ret != -2) {
            printf("[installer.elf] stat target state %s ret=%d\n", state_path, ret);
            return ret;
        }
        ret = installer_mkdir(state_path);
        if (ret == -17) {
            ret = reliefos_stat_legacy(state_path, &state_st);
            if (ret == 0 && state_st.type == RELIEFOS_FS_TYPE_DIR) {
                return 0;
            }
            printf("[installer.elf] verify existing target state %s ret=%d type=%u\n",
                   state_path, ret, ret == 0 ? state_st.type : 0u);
            return ret < 0 ? ret : -20;
        }
        if (ret < 0) {
            printf("[installer.elf] mkdir target state %s ret=%d\n", state_path, ret);
        }
        return ret;
    }
}

static int check_update_target_required(void)
{
    /* Both brand namespaces use the same non-usr-merge filesystem contract.
     * Accept a complete old namespace for migration, but never a usr-merged
     * root or symlinked hierarchy. This preflight performs no writes. */
    static const char *const required_dirs[] = {
        "/etc", "/var", "/var/lib", "/bin", "/sbin", "/lib",
        "/usr", "/usr/bin", "/usr/sbin", "/usr/lib",
    };
    for (uint32_t i = 0; i < sizeof(required_dirs) / sizeof(required_dirs[0]); ++i) {
        char path[RELIEFOS_FS_PATH_LEN];
        if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, required_dirs[i]) < 0)
            return -ENAMETOOLONG;
        if (path_type_nofollow(path) != RELIEFOS_FS_TYPE_DIR) {
            set_status(T("Unsupported root layout; use a fresh ext4 installation"), path);
            return -EINVAL;
        }
    }
    {
        static const char *const namespaces[][5] = {
            {"/etc/reliefos", "/var/lib/reliefos", "/usr/lib/reliefos",
             "/usr/lib/reliefos/apps", "/usr/lib/reliefos/libreliefos.so.2"},
            {"/etc/leonos", "/var/lib/leonos", "/usr/lib/leonos",
             "/usr/lib/leonos/apps", "/usr/lib/leonos/libleonos.so.2"},
        };
        int found = 0;
        for (uint32_t n = 0; n < sizeof(namespaces) / sizeof(namespaces[0]); ++n) {
            int complete = 1;
            for (uint32_t i = 0; i < 5; ++i) {
                char path[RELIEFOS_FS_PATH_LEN];
                int required = i == 4 ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DIR;
                if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, namespaces[n][i]) < 0)
                    return -ENAMETOOLONG;
                if (path_type_nofollow(path) != required) { complete = 0; break; }
            }
            if (complete) { found = 1; break; }
        }
        if (!found) {
            set_status(T("Existing ReliefOS was not detected"), INSTALL_ROOT_MOUNT);
            return -EINVAL;
        }
    }
    {
        static const char *const esp_dirs[] = {"boot", "boot/EFI"};
        for (uint32_t i = 0; i < sizeof(esp_dirs) / sizeof(esp_dirs[0]); ++i) {
            char path[RELIEFOS_FS_PATH_LEN];
            if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, esp_dirs[i]) < 0 ||
                path_has_type(path, RELIEFOS_FS_TYPE_DIR) < 0) {
                set_status(T("Existing ReliefOS boot partition was not detected"), path);
                return -2;
            }
        }
    }
    {
        char loader[RELIEFOS_FS_PATH_LEN];
        if (path_join(loader, sizeof(loader), TARGET_BOOT, "loader.elf") < 0 ||
            path_has_type(loader, RELIEFOS_FS_TYPE_FILE) < 0) {
            set_status(T("Existing ReliefOS boot partition was not detected"), loader);
            return -2;
        }
    }
    if (path_has_type(TARGET_ESP_KERNEL, RELIEFOS_FS_TYPE_FILE) < 0 &&
        path_has_type(TARGET_ESP_KERNEL_LEGACY, RELIEFOS_FS_TYPE_FILE) < 0) {
        set_status(T("Existing ReliefOS boot partition was not detected"), TARGET_ESP_KERNEL);
        return -2;
    }
    return 0;
}

static int check_update_payload_required(void)
{
    static const char *const required_dirs[] = {
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_ETC_RELIEFOS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_USR_LIB,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_LIB,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_DOC,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_FONTS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_RESOURCES,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LICENSES,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_ETC_SSL_CERTS,
        INSTALL_ESP_PAYLOAD "/grub",
        INSTALL_ESP_PAYLOAD "/reliefos",
        INSTALL_ESP_PAYLOAD "/leonos",
        INSTALL_ESP_PAYLOAD "/EFI",
    };
    static const char *const required_files[] = {
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_LIB "/kerneldebug.sys",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/ld-musl-x86_64.so.1",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/libc.so",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/libmimalloc.so.3",
        INSTALL_ROOT_PAYLOAD RELIEFOS_PATH_LIBRELIEFOS,
        INSTALL_ROOT_PAYLOAD "/usr/lib/leonos/libleonos.so.2",
        INSTALL_ESP_PAYLOAD "/reliefos/kernel.sys",
        INSTALL_ESP_PAYLOAD "/reliefos/loader.elf",
        INSTALL_ESP_PAYLOAD "/leonos/kernel.sys",
        INSTALL_ESP_PAYLOAD "/loader.elf",
        INSTALL_ESP_PAYLOAD "/grub/grub.cfg",
        INSTALL_ESP_PAYLOAD "/EFI/BOOT/BOOTX64.EFI",
    };
    for (uint32_t i = 0; i < sizeof(required_dirs) / sizeof(required_dirs[0]); ++i) {
        int ret = path_has_type(required_dirs[i], RELIEFOS_FS_TYPE_DIR);
        if (ret < 0) {
            set_status(T("Installation media is incomplete"),
                       required_dirs[i]);
            return ret;
        }
    }
    for (uint32_t i = 0; i < sizeof(required_files) / sizeof(required_files[0]); ++i) {
        int ret = path_has_type(required_files[i], RELIEFOS_FS_TYPE_FILE);
        if (ret < 0) {
            set_status(T("Installation media is incomplete"),
                       required_files[i]);
            return ret;
        }
    }
    return 0;
}
static int app_package_is_system(const char *package_dir)
{
    char manifest[RELIEFOS_FS_PATH_LEN];
    char text[512];
    int fd;
    long got;
    if (path_join(manifest, sizeof(manifest), package_dir, "manifest.ini") < 0) {
        return 0;
    }
    fd = open(manifest, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    got = read(fd, text, sizeof(text) - 1U);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    text[got] = 0;
    return strstr(text, "system=1") != 0 || strstr(text, "system=true") != 0;
}
static int write_target_locale(void)
{
    return write_locale_setting(TARGET_ETC_RELIEFOS "/locale.conf", language_selection()) == 0
        ? 0 : -errno;
}

/* The target keeps the image's display.conf theme= line (stamped from the
 * build configuration); setup does not select or rewrite it. */
static int write_target_preferences(void)
{
    if (install_mode == INSTALL_MODE_FRESH) {
        return write_target_locale();
    }
    return 0;
}

static void finish_install(int ret,
                           const char *prefix)
{
    install_running = 0;
    page = PAGE_FINISH;
    if (ret < 0) {
        printf("[installer.elf] %s ret=%d\n", prefix ? prefix : "install failed", ret);
        install_success = 0;
        set_error_status(prefix, ret);
        progress_value = 0;
    } else {
        fprintf(stderr, "[installer.elf] %s completed successfully\n",
               install_mode == INSTALL_MODE_UPDATE ? "update" : "installation");
        install_success = 1;
        progress_value = 100;
        set_status(install_mode == INSTALL_MODE_UPDATE
                       ? T("Update completed successfully")
                       : T("Installation completed successfully"),
                   T("Press Restart to boot from the installed disk."));
    }
    if (installer_tty_mode) {
        if (ret < 0) {
            printf("\n%s (error %d)\n", prefix ? prefix : "Install failed", ret);
        } else {
            puts("\nInstallation completed successfully.");
            puts("Remove the installation media before rebooting.");
        }
    } else {
        refresh_ui();
    }
}

void perform_install(void);
void perform_update(void);
void prepare_update_target(void);

/* Helpers for updates within the current rootfs layout. */

static int installer_mkdir_p(const char *path)
{
    char buffer[RELIEFOS_FS_PATH_LEN];
    uint32_t i;
    if (!path || !path[0] || strlen(path) >= sizeof(buffer)) {
        return path && path[0] ? -36 : -22;
    }
    copy_text(buffer, sizeof(buffer), path);
    for (i = 1; buffer[i]; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = 0;
            int ret = installer_mkdir(buffer);
            if (ret < 0 && ret != -17) {
                return ret;
            }
            buffer[i] = '/';
        }
    }
    return installer_mkdir(buffer);
}

static int apply_directory_metadata(const char *src, const char *dst)
{
    struct stat status;
    if (!src || !dst || stat(src, &status) < 0) {
        return src && dst ? -errno : -22;
    }
    if (chown(dst, status.st_uid, status.st_gid) < 0) {
        return -errno;
    }
    return chmod(dst, status.st_mode & 07777) < 0 ? -errno : 0;
}

static int payload_has_app_package(const char *name)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (path_join(path, sizeof(path), INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                  name) < 0) {
        return 0;
    }
    return path_has_type(path, RELIEFOS_FS_TYPE_DIR) == 0;
}

/* Require real hierarchy components. The standard links are copied from
 * the same layout table as fresh images; no old-root transformation occurs. */
static int apply_runtime_root_paths(void)
{
    static const struct { const char *path; const char *target; } links[] = {
#define ROOT_LINK(path, target) {INSTALL_ROOT_MOUNT path, target},
        RELIEFOS_ROOTFS_SYMLINKS(ROOT_LINK)
#undef ROOT_LINK
    };
    for (uint32_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
        int type = path_type_nofollow(links[i].path);
        if (type == -ENOENT) {
            if (symlink(links[i].target, links[i].path) < 0) return -errno;
        } else if (type == RELIEFOS_FS_TYPE_SYMLINK) {
            char target[RELIEFOS_FS_PATH_LEN];
            ssize_t length = readlink(links[i].path, target, sizeof(target) - 1);
            if (length < 0) return -errno;
            target[length] = 0;
            if (strcmp(target, links[i].target)) return -EEXIST;
        } else {
            return type < 0 ? type : -EEXIST;
        }
    }
    return 0;
}
static int ensure_runtime_layout_dirs(void)
{
    static const struct {
        const char *path;
        uint32_t mode;
    } dirs[] = {
#define ROOT_DIR(path, mode) {INSTALL_ROOT_MOUNT path, mode},
        RELIEFOS_ROOTFS_DIRECTORIES(ROOT_DIR)
#undef ROOT_DIR
    };
    for (uint32_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        int ret = installer_mkdir_p(dirs[i].path);
        if (ret < 0 && ret != -17) return ret;
        if (chown(dirs[i].path, 0, 0) < 0 ||
            chmod(dirs[i].path, dirs[i].mode) < 0) return -errno;
    }
    return 0;
}

/* Copy new payload paths over an existing tree without deleting unrelated
 * target entries.  System files are refreshed; user-created commands and
 * packages survive. */
static int overlay_dir_recursive_filtered(const char *src, const char *dst,
                                          const char *skip_source)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    int dst_type = path_type_nofollow(dst);
    if (dst_type == -2) {
        ret = installer_mkdir_p(dst);
        if (ret < 0 && ret != -17) return ret;
        ret = apply_directory_metadata(src, dst);
        if (ret < 0) return ret;
    } else if (dst_type != RELIEFOS_FS_TYPE_DIR) {
        printf("[installer.elf] overlay target is not a directory: %s type=%d\n",
               dst, dst_type);
        return -17;
    }
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) continue;
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            free(entries);
            return -1;
        }
        if (skip_source && !strcmp(src_child, skip_source)) continue;
        if (strcmp(src_child, INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS) == 0) {
            /* Packages are published by the system/optional rules below. */
            continue;
        }
        if (strcmp(src, INSTALL_ROOT_PAYLOAD "/usr/bin") == 0 &&
            payload_has_app_package(entries[i].name)) {
            char package[RELIEFOS_FS_PATH_LEN];
            if (path_join(package, sizeof(package),
                          INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                          entries[i].name) < 0) { free(entries); return -ENAMETOOLONG; }
            int publish = app_package_is_system(package);
            if (!publish) {
                char executable[RELIEFOS_FS_PATH_LEN];
                int size = snprintf(executable, sizeof(executable), "%s/%s/%s.elf",
                                    TARGET_RELIEFOS_APPS, entries[i].name, entries[i].name);
                if (size < 0 || (size_t)size >= sizeof(executable)) {
                    free(entries); return -ENAMETOOLONG;
                }
                publish = path_has_type(executable, RELIEFOS_FS_TYPE_FILE) == 0;
            }
            if (!publish) continue; /* preserve the app's existing entry */
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = overlay_dir_recursive_filtered(src_child, dst_child,
                                                 skip_source);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            int child_type = path_type_nofollow(dst_child);
            if (child_type >= 0 && child_type != RELIEFOS_FS_TYPE_FILE &&
                child_type != RELIEFOS_FS_TYPE_SYMLINK) {
                ret = -17;
            } else {
                ret = copy_file_path(src_child, dst_child);
                if (ret >= 0) ++copy_done;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            int child_type = path_type_nofollow(dst_child);
            if (child_type == -2 || child_type == RELIEFOS_FS_TYPE_FILE ||
                child_type == RELIEFOS_FS_TYPE_SYMLINK) {
                ret = copy_symlink_path(src_child, dst_child);
            } else {
                ret = -17;
            }
            if (ret >= 0 && child_type == -2) ++copy_done;
        } else {
            ret = 0;
        }
        if (ret < 0) {
            printf("[installer.elf] layout overlay conflict src=%s dst=%s ret=%d\n",
                   src_child, dst_child, ret);
            free(entries);
            return ret;
        }
    }
    free(entries);
    return 0;
}
static int overlay_dir_recursive(const char *src, const char *dst)
{
    return overlay_dir_recursive_filtered(src, dst, NULL);
}
static int sync_system_payload(void)
{
    pid_t child;
    char *const argv[] = {"sh", "/usr/lib/reliefos/reliefos-apk-update", INSTALL_ROOT_MOUNT,
                         INSTALL_ROOT_PAYLOAD "/usr/share/reliefos/apk/repository", NULL};
    char *const envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "HOME=/root", NULL};
    int ret = posix_spawn(&child, "/bin/sh", NULL, NULL, argv, envp);
    if (ret) return -ret;
    int status;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) return -errno;
        refresh_ui();
        usleep(50000);
    }
    printf("[installer.elf] APK update wait_status=%d\n", status);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}
void prepare_update_target(void)
{
    int ret;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    page = PAGE_PROGRESS;
    progress_value = 0;
    show_progress(5,
                  T("Mounting target filesystems"),
                  T("Root: /target; ESP: /target/boot"));
    ret = installer_mount_targets(disks[selected_disk].path, 0);
    if (ret < 0) {
        finish_install(ret, T("Mount failed"));
        return;
    }
    if (reliefos_account_legacy_check(INSTALL_ROOT_MOUNT) < 0) {
        finish_install(-errno,
                       T("Legacy accounts require recovery before updating"));
        return;
    }
    show_progress(18,
                  T("Checking existing ReliefOS"),
                  T("Target: /target"));
    ret = check_update_payload_required();
    if (ret < 0) {
        finish_install(ret, T("Payload check failed"));
        return;
    }
    ret = check_update_target_required();
    if (ret < 0) {
        finish_install(ret, T("Existing system check failed"));
        return;
    }
    if (path_type_nofollow(INSTALL_ROOT_MOUNT "/lib/apk/db/installed") != RELIEFOS_FS_TYPE_FILE) {
        finish_install(-ENOENT,
                       T("No APK database: reinstall from this media"));
        return;
    }
    set_status(T("Update installed ReliefOS packages"),
               T("Alpine packages and local configuration are preserved."));
    page = PAGE_CONFIRM;
    reset_confirm();
    refresh_ui();
    refresh_ui();
}

void perform_install(void)
{
    int ret;
    if (!installer_setup_valid(&setup)) {
        finish_install(-EINVAL, T("Invalid installation settings"));
        return;
    }
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    install_running = 1;
    install_success = 0;
    page = PAGE_PROGRESS;
    copy_total = 0;
    copy_done = 0;
    copy_total_bytes = 0;
    copy_done_bytes = 0;

    show_progress(2, "Preparing target disk", "");
    show_progress(22, "Mounting target filesystems", "Root: /target (ext4 for new installs; ext4 or exFAT for updates), ESP: /target/boot (FAT32)");
    ret = installer_mount_targets(disks[selected_disk].path, 1);
    if (ret < 0) {
        finish_install(ret, "Mount failed");
        return;
    }

    show_progress(30, "Scanning installation payload", "Root and boot partitions");
    ret = count_files_recursive(INSTALL_ROOT_PAYLOAD, &copy_total);
    if (ret >= 0) {
        ret = count_files_recursive(INSTALL_ESP_PAYLOAD, &copy_total);
    }
    if (ret < 0) {
        finish_install(ret, T("Payload scan failed"));
        return;
    }

    show_progress(35, T("Copying system files"),
                  T("Root: /target; boot partition: /target/boot"));
    ret = copy_payload_ordered();
    if (ret < 0) {
        finish_install(ret, T("Copy failed"));
        return;
    }

    explicit_bzero(setup.password, sizeof(setup.password));
    explicit_bzero(setup.password_confirm, sizeof(setup.password_confirm));
    explicit_bzero(setup.root_password, sizeof(setup.root_password));
    explicit_bzero(setup.root_password_confirm, sizeof(setup.root_password_confirm));
    ret = write_target_preferences();
    if (ret < 0) {
        finish_install(ret, T("Could not save installed preferences"));
        return;
    }

    show_progress(100, T("Installation completed successfully"), T("Target disk is ready."));
    finish_install(0, "");
}

void perform_update(void)
{
    static const char *const boot_dirs[] = {"reliefos", "grub", "EFI"};
    int ret;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    install_running = 1;
    install_success = 0;
    page = PAGE_PROGRESS;
    copy_total = 0;
    copy_done = 0;
    copy_total_bytes = 0;
    copy_done_bytes = 0;

    show_progress(2, T("Mounting target filesystems"),
                  T("Root: /target; ESP: /target/boot"));
    ret = installer_mount_targets(disks[selected_disk].path, 0);
    if (ret < 0) {
        finish_install(ret, T("Mount failed"));
        return;
    }

    show_progress(10, T("Checking existing ReliefOS"),
                  T("Target: /target"));
    ret = check_update_payload_required();
    if (ret < 0) {
        finish_install(ret, T("Payload check failed"));
        return;
    }
    ret = check_update_target_required();
    if (ret < 0) {
        finish_install(ret, T("Existing system check failed"));
        return;
    }
    if (installer_root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) {
        /* The current root contract needs real symlinks; exFAT cannot store
         * them, so an update would silently produce an unusable namespace.
         * Refuse instead of materializing directory copies. */
        finish_install(-38,
                       T("exFAT root cannot carry the current symlink layout; use a fresh ext4 install"));
        return;
    }

    ret = ensure_runtime_layout_dirs();
    if (ret >= 0) ret = apply_runtime_root_paths();
    if (ret < 0) {
        finish_install(ret, T("Invalid root layout"));
        return;
    }

    show_progress(22, T("Scanning update payload"),
                  T("Root and boot payloads"));
    for (uint32_t i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); ++i) {
        char src[RELIEFOS_FS_PATH_LEN];
        if (path_join(src, sizeof(src), INSTALL_ESP_PAYLOAD, boot_dirs[i]) < 0) {
            finish_install(-1, T("Payload path is too long"));
            return;
        }
        ret = count_files_recursive(src, &copy_total);
        if (ret < 0) {
            finish_install(ret, T("Payload scan failed"));
            return;
        }
    }
    show_progress(35, T("Upgrading signed ReliefOS packages"),
                  T("Checking dependencies and preserving local configuration"));
    ret = sync_system_payload();
    if (ret < 0) {
        finish_install(ret, T("Core update failed"));
        return;
    }

    /* Publish boot files only after the root payload is complete. Each file
     * is replaced after its copy succeeds; never delete the EFI/GRUB tree. */
    for (uint32_t i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); ++i) {
        char src[RELIEFOS_FS_PATH_LEN], dst[RELIEFOS_FS_PATH_LEN];
        if (path_join(src, sizeof(src), INSTALL_ESP_PAYLOAD, boot_dirs[i]) < 0 ||
            path_join(dst, sizeof(dst), INSTALL_ESP_MOUNT, boot_dirs[i]) < 0) {
            finish_install(-ENAMETOOLONG, T("Boot update failed"));
            return;
        }
        ret = !strcmp(boot_dirs[i], "grub")
            ? overlay_dir_recursive_filtered(src, dst,
                                             INSTALL_ESP_PAYLOAD "/grub/grub.cfg")
            : overlay_dir_recursive(src, dst);
        if (ret < 0) {
            finish_install(ret, T("Boot update failed"));
            return;
        }
    }
    /* Publish the new boot menu only after its kernel, loader and EFI image
     * are present. The config's legacy entry keeps the untouched old payload
     * under /leonos available if a subsequent boot fails. */
    ret = copy_file_path(INSTALL_ESP_PAYLOAD "/grub/grub.cfg",
                         INSTALL_ESP_MOUNT "/grub/grub.cfg");
    if (ret < 0) {
        finish_install(ret, T("Boot update failed"));
        return;
    }

    show_progress(100, T("Update completed successfully"),
                  T("Target disk is ready."));
    finish_install(0, "");
}
