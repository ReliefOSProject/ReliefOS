/* Small block-storage applets backed exclusively by /dev block FDs, Linux
 * BLK* ioctls and mount(2).  They intentionally do not use ReliefOS private
 * disk-management ioctls. */
//config:config LEONOS_FDISK
//config: bool "fdisk (GPT editor)"
//config: default y
//config:config LEONOS_MKFS_FAT
//config: bool "mkfs.fat (FAT32 formatter)"
//config: default y
//config:config LEONOS_MKFS_EXT2
//config: bool "mkfs.ext2 (ext2 formatter)"
//config: default y
//config:config LEONOS_MKFS_EXT4
//config: bool "mkfs.ext4 (upstream ext4 formatter)"
//config: default y
//config:config LEONOS_MKFS_EXFAT
//config: bool "mkfs.exfat (exFAT formatter)"
//config: default y
//config:config LEONOS_MOUNT
//config: bool "mount"
//config: default y
//config:config LEONOS_UMOUNT
//config: bool "umount"
//config: default y
//config:config LEONOS_GRUB_INSTALLER
//config: bool "leonos-grub-installer (copy EFI payload)"
//config: default y
//config:config LEONOS_FSCK
//config: bool "fsck.*"
//config: default y
//config:config LEONOS_BLKID
//config: bool "blkid"
//config: default y
//config:config LEONOS_LSBLK
//config: bool "lsblk"
//config: default y

//applet:IF_LEONOS_FDISK(APPLET_ODDNAME(fdisk, leonos_fdisk, BB_DIR_SBIN, BB_SUID_DROP, leonos_fdisk))
//applet:IF_LEONOS_MKFS_FAT(APPLET_ODDNAME(mkfs.fat, leonos_mkfs_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_fat))
//applet:IF_LEONOS_MKFS_FAT(APPLET_ODDNAME(mkfs.fat32, leonos_mkfs_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_fat))
//applet:IF_LEONOS_MKFS_FAT(APPLET_ODDNAME(mkfs.vfat, leonos_mkfs_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_fat))
//applet:IF_LEONOS_MKFS_EXT2(APPLET_ODDNAME(mkfs.ext2, leonos_mkfs_ext2, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_ext2))
//applet:IF_LEONOS_MKFS_EXT4(APPLET_ODDNAME(mkfs.ext4, leonos_mkfs_ext4, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_ext4))
//applet:IF_LEONOS_MKFS_EXFAT(APPLET_ODDNAME(mkfs.exfat, leonos_mkfs_exfat, BB_DIR_SBIN, BB_SUID_DROP, leonos_mkfs_exfat))
//applet:IF_LEONOS_MOUNT(APPLET(mount, BB_DIR_BIN, BB_SUID_DROP))
//applet:IF_LEONOS_UMOUNT(APPLET(umount, BB_DIR_SBIN, BB_SUID_DROP))
//applet:IF_LEONOS_GRUB_INSTALLER(APPLET_ODDNAME(leonos-grub-installer, leonos_grub_installer, BB_DIR_SBIN, BB_SUID_DROP, leonos_grub_installer))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.fat, leonos_fsck_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_fat))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.fat32, leonos_fsck_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_fat))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.vfat, leonos_fsck_fat, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_fat))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.ext2, leonos_fsck_ext2, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_ext2))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.ext4, leonos_fsck_ext4, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_ext4))
//applet:IF_LEONOS_FSCK(APPLET_ODDNAME(fsck.exfat, leonos_fsck_exfat, BB_DIR_SBIN, BB_SUID_DROP, leonos_fsck_exfat))
//applet:IF_LEONOS_FSCK(APPLET(fsck, BB_DIR_SBIN, BB_SUID_DROP))
//applet:IF_LEONOS_BLKID(APPLET(blkid, BB_DIR_SBIN, BB_SUID_DROP))
//applet:IF_LEONOS_LSBLK(APPLET(lsblk, BB_DIR_BIN, BB_SUID_DROP))

//usage:#define leonos_fdisk_trivial_usage "[-l] [DISK]"
//usage:#define leonos_fdisk_full_usage "\n\nShow or edit a GPT disk. Keys: p print, n new, d delete, t type, r rename, w write/exit, q quit.\n"
//usage:#define leonos_mkfs_fat_trivial_usage "[--force] BLOCKDEV"
//usage:#define leonos_mkfs_fat_full_usage "\n\nMake a FAT32 filesystem on BLOCKDEV\n"
//usage:#define leonos_mkfs_ext2_trivial_usage "[--force] BLOCKDEV"
//usage:#define leonos_mkfs_ext2_full_usage "\n\nMake a legacy ext2 filesystem on BLOCKDEV\n"
//usage:#define leonos_mkfs_ext4_trivial_usage "[--force] BLOCKDEV"
//usage:#define leonos_mkfs_ext4_full_usage "\n\nMake an ext4 filesystem on BLOCKDEV\n"
//usage:#define leonos_mkfs_exfat_trivial_usage "[--force] BLOCKDEV"
//usage:#define leonos_mkfs_exfat_full_usage "\n\nMake an exFAT filesystem on BLOCKDEV\n"
//usage:#define mount_trivial_usage "[-t FSTYPE] BLOCKDEV DIR"
//usage:#define mount_full_usage "\n\nMount a FAT32, exFAT, ext4, or legacy ext2 block device\n"
//usage:#define umount_trivial_usage "DIR"
//usage:#define umount_full_usage "\n\nUnmount a filesystem by mount point\n"
//usage:#define leonos_grub_installer_trivial_usage "ESP-MOUNTPOINT"
//usage:#define leonos_grub_installer_full_usage "\n\nInstall the ReliefOS EFI/GRUB payload into an ESP mount\n"
//usage:#define leonos_fsck_fat_trivial_usage "BLOCKDEV"
//usage:#define leonos_fsck_fat_full_usage "\n\nCheck a FAT32 filesystem signature\n"
//usage:#define leonos_fsck_ext2_trivial_usage "BLOCKDEV"
//usage:#define leonos_fsck_ext2_full_usage "\n\nCheck a legacy ext2 filesystem signature\n"
//usage:#define leonos_fsck_ext4_trivial_usage "BLOCKDEV"
//usage:#define leonos_fsck_ext4_full_usage "\n\nCheck an ext4 filesystem signature\n"
//usage:#define leonos_fsck_exfat_trivial_usage "BLOCKDEV"
//usage:#define leonos_fsck_exfat_full_usage "\n\nCheck an exFAT filesystem signature\n"
//usage:#define fsck_trivial_usage "[-n] BLOCKDEV"
//usage:#define fsck_full_usage "\n\nCheck a filesystem signature\n"
//usage:#define blkid_trivial_usage "[BLOCKDEV...]"
//usage:#define blkid_full_usage "\n\nPrint filesystem and GPT partition identifiers\n"
//usage:#define lsblk_trivial_usage
//usage:#define lsblk_full_usage "\n\nList block devices and GPT partitions\n"

#include "libbb.h"
#include <leonos/blockdev.h>
#include <leonos/fs.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

static int read_line(const char *prompt, char *line, size_t capacity)
{
    if (prompt) fputs(prompt, stdout);
    if (!fgets(line, capacity, stdin)) return -1;
    line[strcspn(line, "\r\n")] = 0;
    return 0;
}

static int print_partitions(const char *disk)
{
    struct leonos_block_partition parts[LEONOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0;
    int ret = leonos_block_list_partitions(disk, parts, LEONOS_BLOCK_MAX_PARTITIONS, &count);
    if (ret < 0) {
        bb_error_msg("cannot read GPT on %s: error %d", disk, -ret);
        return 1;
    }
    printf("Disk %s\n", disk);
    puts("Device        Start       Sectors     Filesystem  GPT type  Name");
    for (uint32_t i = 0; i < count; ++i) {
        printf("%-13s %-11llu %-11llu %-11s %-9s %s\n", parts[i].path,
               (unsigned long long)parts[i].first_lba,
               (unsigned long long)parts[i].sector_count,
               leonos_block_filesystem_name(parts[i].filesystem),
               leonos_block_gpt_type_name(parts[i].gpt_type), parts[i].name);
    }
    return 0;
}

static int print_all_partitions(void)
{
    struct leonos_block_disk_info disks[LEONOS_BLOCK_MAX_DISKS];
    uint32_t count = 0;
    int ret = leonos_block_list_disks(disks, LEONOS_BLOCK_MAX_DISKS, &count);
    if (ret < 0) {
        bb_error_msg("cannot enumerate block devices: error %d", -ret);
        return 1;
    }
    {
        int status = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (print_partitions(disks[i].path) != 0) status = 1;
        return status;
    }
}

static int type_from_text(const char *text, uint32_t *type)
{
    if (!text || !type) return -1;
    if (!strcmp(text, "basic") || !strcmp(text, "basic-data") || !strcmp(text, "1"))
        *type = LEONOS_BLOCK_GPT_BASIC_DATA;
    else if (!strcmp(text, "esp") || !strcmp(text, "2")) *type = LEONOS_BLOCK_GPT_ESP;
    else if (!strcmp(text, "linux") || !strcmp(text, "3")) *type = LEONOS_BLOCK_GPT_LINUX;
    else return -1;
    return 0;
}

static int fdisk_partition_number(char *line, uint32_t *index)
{
    unsigned long value;
    if (read_line("Partition number: ", line, 32) < 0) return -1;
    value = strtoul(line, NULL, 10);
    if (!value || value > LEONOS_BLOCK_MAX_PARTITIONS) return -1;
    *index = (uint32_t)value - 1u;
    return 0;
}

int leonos_fdisk_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_fdisk_main(int argc, char **argv)
{
    const char *disk;
    char line[LEONOS_BLOCK_NAME_LEN];
    if (argc == 2 && !strcmp(argv[1], "-l")) return print_all_partitions();
    if (argc == 3 && !strcmp(argv[1], "-l")) return print_partitions(argv[2]);
    if (argc != 2) bb_show_usage();
    disk = argv[1];
    for (;;) {
        if (read_line("Command (p print, n new, d delete, t type, r rename, w write, q quit): ", line, sizeof(line)) < 0)
            return 0;
        if (line[0] == 'p') (void)print_partitions(disk);
        else if (line[0] == 'n') {
            unsigned long size;
            uint32_t index;
            int ret;
            if (read_line("Partition size in MiB: ", line, sizeof(line)) < 0) continue;
            size = strtoul(line, NULL, 10);
            if (!size || size > 0xffffffffu) { bb_error_msg("invalid partition size"); continue; }
            if (read_line("Partition name (optional): ", line, sizeof(line)) < 0) continue;
            ret = leonos_block_gpt_create(disk, LEONOS_BLOCK_FILESYSTEM_UNKNOWN,
                                          (uint32_t)size, line, &index);
            if (ret < 0)
                bb_error_msg("partition creation failed: error %d", -ret);
            else puts("Partition created. Use mkfs.* to format it.");
        } else if (line[0] == 'd') {
            uint32_t index;
            int ret;
            if (fdisk_partition_number(line, &index) < 0) {
                bb_error_msg("invalid partition number");
                continue;
            }
            ret = leonos_block_gpt_delete(disk, index);
            if (ret < 0)
                bb_error_msg("partition deletion failed: error %d", -ret);
            else puts("Partition deleted.");
        } else if (line[0] == 't') {
            uint32_t index, type;
            int ret;
            if (fdisk_partition_number(line, &index) < 0 ||
                read_line("GPT type (basic, esp, linux): ", line, sizeof(line)) < 0 ||
                type_from_text(line, &type) < 0) {
                bb_error_msg("invalid partition type or number");
                continue;
            }
            ret = leonos_block_gpt_set_type(disk, index, type);
            if (ret < 0)
                bb_error_msg("partition type update failed: error %d", -ret);
            else puts("Partition type updated.");
        } else if (line[0] == 'r') {
            uint32_t index;
            int ret;
            if (fdisk_partition_number(line, &index) < 0 ||
                read_line("GPT name (empty clears): ", line, sizeof(line)) < 0) {
                bb_error_msg("invalid partition number");
                continue;
            }
            ret = leonos_block_gpt_set_name(disk, index, line);
            if (ret < 0)
                bb_error_msg("partition name update failed: error %d", -ret);
            else puts("Partition name updated.");
        } else if (line[0] == 'w' || line[0] == 'q') return 0;
        else puts("Unknown command.");
    }
}

static int format_partition(int argc, char **argv, uint32_t filesystem)
{
    const char *path;
    int ret;
    if (argc == 2) path = argv[1];
    else if (argc == 3 && !strcmp(argv[1], "--force")) path = argv[2];
    else { bb_show_usage(); return 1; }
    ret = leonos_block_format(path, filesystem, NULL);
    if (ret < 0) {
        bb_error_msg("format failed for %s: error %d", path, -ret);
        return 1;
    }
    printf("Formatted %s.\n", path);
    return 0;
}

int leonos_mkfs_fat_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_mkfs_fat_main(int argc, char **argv) { return format_partition(argc, argv, LEONOS_BLOCK_FILESYSTEM_FAT32); }
int leonos_mkfs_ext2_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_mkfs_ext2_main(int argc, char **argv) { return format_partition(argc, argv, LEONOS_BLOCK_FILESYSTEM_EXT2); }
int leonos_mkfs_ext4_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_mkfs_ext4_main(int argc, char **argv) { return format_partition(argc, argv, LEONOS_BLOCK_FILESYSTEM_EXT4); }
int leonos_mkfs_exfat_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_mkfs_exfat_main(int argc, char **argv) { return format_partition(argc, argv, LEONOS_BLOCK_FILESYSTEM_EXFAT); }

int mount_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int mount_main(int argc, char **argv)
{
    const char *type = NULL;
    const char *source;
    const char *target;
    if (argc == 3) { source = argv[1]; target = argv[2]; }
    else if (argc == 5 && !strcmp(argv[1], "-t")) { type = argv[2]; source = argv[3]; target = argv[4]; }
    else { bb_show_usage(); return 1; }
    if (mount(source, target, type, 0, NULL) < 0) {
        bb_perror_msg("mount %s", source);
        return 1;
    }
    printf("%s mounted on %s\n", source, target);
    return 0;
}

int umount_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int umount_main(int argc, char **argv)
{
    if (argc != 2) bb_show_usage();
    if (umount2(argv[1], 0) < 0) { bb_perror_msg("umount %s", argv[1]); return 1; }
    return 0;
}

static int fsck_device(const char *path, uint32_t expected, int allow_any)
{
    uint32_t filesystem;
    int ret = leonos_block_probe_filesystem(path, &filesystem);
    if (ret < 0) {
        bb_error_msg("cannot read %s: error %d", path, -ret);
        return 1;
    }
    if (filesystem == LEONOS_BLOCK_FILESYSTEM_UNKNOWN) {
        bb_error_msg("%s: filesystem signature is invalid", path);
        return 1;
    }
    if (!allow_any && filesystem != expected) {
        bb_error_msg("%s is %s, not %s", path, leonos_block_filesystem_name(filesystem),
                     leonos_block_filesystem_name(expected));
        return 1;
    }
    printf("%s: clean, %s filesystem\n", path, leonos_block_filesystem_name(filesystem));
    return 0;
}

int leonos_fsck_fat_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_fsck_fat_main(int argc, char **argv) { return fsck_device(argc == 3 && !strcmp(argv[1], "-n") ? argv[2] : argc == 2 ? argv[1] : (bb_show_usage(), ""), LEONOS_BLOCK_FILESYSTEM_FAT32, 0); }
int leonos_fsck_ext2_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_fsck_ext2_main(int argc, char **argv) { return fsck_device(argc == 3 && !strcmp(argv[1], "-n") ? argv[2] : argc == 2 ? argv[1] : (bb_show_usage(), ""), LEONOS_BLOCK_FILESYSTEM_EXT2, 0); }
int leonos_fsck_ext4_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_fsck_ext4_main(int argc, char **argv) { return fsck_device(argc == 3 && !strcmp(argv[1], "-n") ? argv[2] : argc == 2 ? argv[1] : (bb_show_usage(), ""), LEONOS_BLOCK_FILESYSTEM_EXT4, 0); }
int leonos_fsck_exfat_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_fsck_exfat_main(int argc, char **argv) { return fsck_device(argc == 3 && !strcmp(argv[1], "-n") ? argv[2] : argc == 2 ? argv[1] : (bb_show_usage(), ""), LEONOS_BLOCK_FILESYSTEM_EXFAT, 0); }
int fsck_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int fsck_main(int argc, char **argv) { return fsck_device(argc == 3 && !strcmp(argv[1], "-n") ? argv[2] : argc == 2 ? argv[1] : (bb_show_usage(), ""), 0, 1); }

static int print_blkid(const char *path, const char *label)
{
    uint32_t filesystem;
    int ret = leonos_block_probe_filesystem(path, &filesystem);
    if (ret < 0) {
        bb_error_msg("cannot read %s: error %d", path, -ret);
        return 1;
    }
    printf("%s: TYPE=\"%s\"%s%s%s\n", path, leonos_block_filesystem_name(filesystem),
           label && label[0] ? " PARTLABEL=\"" : "", label && label[0] ? label : "",
           label && label[0] ? "\"" : "");
    return 0;
}

int blkid_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int blkid_main(int argc, char **argv)
{
    int status = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) if (print_blkid(argv[i], NULL)) status = 1;
        return status;
    }
    {
        struct leonos_block_disk_info disks[LEONOS_BLOCK_MAX_DISKS]; uint32_t disks_count = 0;
        int ret = leonos_block_list_disks(disks, LEONOS_BLOCK_MAX_DISKS, &disks_count);
        if (ret < 0) {
            bb_error_msg("cannot enumerate block devices: error %d", -ret);
            return 1;
        }
        for (uint32_t d = 0; d < disks_count; ++d) {
            struct leonos_block_partition parts[LEONOS_BLOCK_MAX_PARTITIONS]; uint32_t count = 0;
            int ret = leonos_block_list_partitions(disks[d].path, parts,
                                                   LEONOS_BLOCK_MAX_PARTITIONS, &count);
            if (ret < 0) {
                bb_error_msg("cannot read GPT on %s: error %d", disks[d].path, -ret);
                status = 1;
                continue;
            }
            for (uint32_t i = 0; i < count; ++i)
                if (print_blkid(parts[i].path, parts[i].name)) status = 1;
        }
    }
    return status;
}

int lsblk_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int lsblk_main(int argc, char **argv)
{
    struct leonos_block_disk_info disks[LEONOS_BLOCK_MAX_DISKS]; uint32_t disks_count = 0;
    (void)argv;
    if (argc != 1) bb_show_usage();
    {
        int ret = leonos_block_list_disks(disks, LEONOS_BLOCK_MAX_DISKS, &disks_count);
        if (ret < 0) {
            bb_error_msg("cannot enumerate block devices: error %d", -ret);
            return 1;
        }
    }
    puts("NAME        SIZE        TYPE  FSTYPE  LABEL");
    for (uint32_t d = 0; d < disks_count; ++d) {
        struct leonos_block_partition parts[LEONOS_BLOCK_MAX_PARTITIONS]; uint32_t count = 0;
        int ret;
        printf("disk%u       %llu      disk\n", disks[d].id, (unsigned long long)disks[d].sector_count * disks[d].sector_size);
        ret = leonos_block_list_partitions(disks[d].path, parts,
                                           LEONOS_BLOCK_MAX_PARTITIONS, &count);
        if (ret < 0) {
            bb_error_msg("cannot read GPT on %s: error %d", disks[d].path, -ret);
            return 1;
        }
        for (uint32_t i = 0; i < count; ++i)
            printf("disk%up%-4u %-11llu part  %-7s %s\n", disks[d].id, parts[i].index + 1u,
                   (unsigned long long)parts[i].sector_count * disks[d].sector_size,
                   leonos_block_filesystem_name(parts[i].filesystem), parts[i].name);
    }
    return 0;
}

static int grub_copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb"); FILE *out; unsigned char buffer[8192]; size_t got;
    if (!in || !(out = fopen(destination, "wb"))) { if (in) fclose(in); return -1; }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) != 0)
        if (fwrite(buffer, 1, got, out) != got) { fclose(in); fclose(out); return -1; }
    fclose(in); return fclose(out);
}
static int grub_copy_tree(const char *source, const char *destination)
{
    DIR *dir = opendir(source); struct dirent *entry;
    if (!dir) return -1;
    (void)mkdir(destination, 0777);
    while ((entry = readdir(dir)) != NULL) {
        char src[LEONOS_FS_PATH_LEN], dst[LEONOS_FS_PATH_LEN];
        if (snprintf(src, sizeof(src), "%s/%s", source, entry->d_name) < 0 ||
            snprintf(dst, sizeof(dst), "%s/%s", destination, entry->d_name) < 0 ||
            (entry->d_type == DT_DIR ? grub_copy_tree(src, dst) : grub_copy_file(src, dst)) < 0) {
            closedir(dir); return -1;
        }
    }
    closedir(dir); return 0;
}
int leonos_grub_installer_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int leonos_grub_installer_main(int argc, char **argv)
{
    const char *esp = argc == 2 ? argv[1] : NULL;
    static const char *const files[] = {"EFI/BOOT/BOOTX64.EFI", "reliefos/kernel.sys", "leonos/kernel.sys", NULL};
    if (!esp) bb_show_usage();
    {
        char path[LEONOS_FS_PATH_LEN];
        snprintf(path, sizeof(path), "%s/EFI", esp); (void)mkdir(path, 0777);
        snprintf(path, sizeof(path), "%s/EFI/BOOT", esp); (void)mkdir(path, 0777);
        snprintf(path, sizeof(path), "%s/reliefos", esp); (void)mkdir(path, 0777);
        snprintf(path, sizeof(path), "%s/leonos", esp); (void)mkdir(path, 0777);
    }
    for (uint32_t i = 0; files[i]; ++i) {
        char source[LEONOS_FS_PATH_LEN], destination[LEONOS_FS_PATH_LEN];
        snprintf(source, sizeof(source), "/install/esp/%s", files[i]);
        snprintf(destination, sizeof(destination), "%s/%s", esp, files[i]);
        if (grub_copy_file(source, destination) < 0) { bb_error_msg("cannot install %s", files[i]); return 1; }
    }
    {
        char source[LEONOS_FS_PATH_LEN], destination[LEONOS_FS_PATH_LEN];
        snprintf(source, sizeof(source), "/install/esp/grub");
        snprintf(destination, sizeof(destination), "%s/grub", esp);
        if (grub_copy_tree(source, destination) < 0) { bb_error_msg("cannot install GRUB files"); return 1; }
    }
    printf("ReliefOS GRUB payload installed to %s.\n", esp);
    return 0;
}
