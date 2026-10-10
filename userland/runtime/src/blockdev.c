/* Block-device helpers for ReliefOS userland tools.
 *
 * This module deliberately uses only POSIX file descriptors, Linux block
 * ioctls, and sector-aligned raw I/O.  It is the shared implementation for
 * fdisk, mkfs.*, the installer and diskmgr; it does not call any private
 * kernel disk-management ABI. */
#include <reliefos/blockdev.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BLOCK_SECTOR_SIZE 512u
#define GPT_ENTRY_MAX 128u
#define GPT_ENTRY_SIZE 128u
#define GPT_DEFAULT_TABLE_BYTES (GPT_ENTRY_MAX * GPT_ENTRY_SIZE)
#define GPT_DEFAULT_TABLE_SECTORS (GPT_DEFAULT_TABLE_BYTES / BLOCK_SECTOR_SIZE)
#define BLOCK_IO_CHUNK (32u * 1024u)
#define BLOCK_IO_SLICE 4096u
#define BLOCK_EIO 5
#define BLOCK_ENOENT 2
#define BLOCK_EEXIST 17
#define BLOCK_EINVAL 22
#define BLOCK_ENOSPC 28
#define BLOCK_EBUSY 16
#define BLOCK_ENOTSUP 95
#define FAT32_EOC 0x0ffffff8u
#define EXT2_SUPER_MAGIC 0xef53u
#define EXT2_BLOCK_SIZE 4096u
#define EXT2_BLOCKS_PER_GROUP 32768u
#define EXT2_INODES_PER_GROUP 8192u

struct __attribute__((packed)) block_gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
};

struct __attribute__((packed)) block_gpt_entry {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
};

struct __attribute__((packed)) block_fat32_bpb {
    uint8_t jump[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors16;
    uint8_t media;
    uint16_t fat_size16;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
};

struct __attribute__((packed)) block_ext2_super {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    int32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    int16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
};

struct __attribute__((packed)) block_ext2_group {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
};

struct __attribute__((packed)) block_ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_512;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t size_high;
    uint32_t faddr;
    uint8_t osd2[12];
};

struct __attribute__((packed)) block_ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
};

struct block_gpt_table {
    struct block_gpt_header primary;
    struct block_gpt_header backup;
    struct block_gpt_entry *entries;
    uint32_t entry_bytes;
    uint32_t entry_sectors;
};

static const uint8_t block_guid_basic[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};
static const uint8_t block_guid_esp[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
static const uint8_t block_guid_linux[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

static uint32_t block_crc32(const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void block_copy(char *destination, uint32_t capacity, const char *source)
{
    uint32_t i = 0;
    if (!destination || !capacity) return;
    while (source && source[i] && i + 1u < capacity) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = 0;
}

static int block_io(int fd, uint64_t offset, void *buffer, uint32_t length, int write_mode)
{
    uint8_t *bytes = buffer;
    uint32_t done = 0;
    long position;
    if (!buffer || (offset & (BLOCK_SECTOR_SIZE - 1u)) ||
        (length & (BLOCK_SECTOR_SIZE - 1u))) return -BLOCK_EINVAL;
    position = lseek(fd, (long)offset, 0);
    if (position < 0) return -errno;
    while (done < length) {
        uint32_t chunk = length - done;
        long ret;
        if (chunk > BLOCK_IO_CHUNK) chunk = BLOCK_IO_CHUNK;
        /* Device reads are deliberately capped by the kernel syscall layer.
         * Treat a short transfer as progress and continue from the descriptor
         * offset; requiring one syscall to return the whole GPT table turns a
         * valid disk into a false EIO. */
        if (chunk > BLOCK_IO_SLICE) chunk = BLOCK_IO_SLICE;
        ret = write_mode ? write(fd, bytes + done, chunk) : read(fd, bytes + done, chunk);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (ret == 0 || (ret & (BLOCK_SECTOR_SIZE - 1u)) != 0) return -BLOCK_EIO;
        if ((uint32_t)ret > chunk) return -BLOCK_EIO;
        done += (uint32_t)ret;
    }
    return 0;
}

static int block_zero(int fd, uint64_t offset, uint64_t length)
{
    uint8_t zeros[BLOCK_IO_CHUNK] = {0};
    while (length) {
        uint32_t chunk = length > sizeof(zeros) ? sizeof(zeros) : (uint32_t)length;
        int ret = block_io(fd, offset, zeros, chunk, 1);
        if (ret < 0) return ret;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}

static int block_open_info(const char *path, int writable, int *out_fd,
                           uint64_t *out_sectors, uint32_t *out_sector_size)
{
    int fd;
    uint64_t bytes = 0;
    int sector_size = BLOCK_SECTOR_SIZE;
    long ret;
    if (!path || !out_fd || !out_sectors || !out_sector_size) return -BLOCK_EINVAL;
    fd = open(path, writable ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) return -errno;
    ret = ioctl(fd, BLKGETSIZE64, &bytes);
    if (ret < 0) {
        int error = errno;
        (void)close(fd);
        return -error;
    }
    ret = ioctl(fd, BLKSSZGET, &sector_size);
    if (ret < 0) {
        int error = errno;
        (void)close(fd);
        return -error;
    }
    if (sector_size != BLOCK_SECTOR_SIZE || !bytes || bytes % BLOCK_SECTOR_SIZE) {
        (void)close(fd);
        return -BLOCK_EINVAL;
    }
    *out_fd = fd;
    *out_sectors = bytes / BLOCK_SECTOR_SIZE;
    *out_sector_size = (uint32_t)sector_size;
    return 0;
}

static int block_disk_index(const char *path, uint32_t *out_index)
{
    const char *p;
    uint32_t value = 0;
    if (!path || !out_index || strncmp(path, "/dev/", 5) != 0) return -BLOCK_EINVAL;
    p = path + 5;
    if (p[0] == 's' && p[1] == 'd' && p[2] >= 'a' && p[2] <= 'z') {
        uint32_t letters = 0;
        do {
            if (*p < 'a' || *p > 'z') return -BLOCK_EINVAL;
            letters = letters * 26u + (uint32_t)(*p - 'a' + 1u);
            ++p;
        } while (*p >= 'a' && *p <= 'z');
        if (*p) return -BLOCK_EINVAL;
        *out_index = letters - 1u;
        return 0;
    }
    if (strncmp(p, "nvme", 4) != 0) return -BLOCK_EINVAL;
    p += 4;
    if (*p < '0' || *p > '9') return -BLOCK_EINVAL;
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (uint32_t)(*p++ - '0');
    }
    if (*p++ != 'n' || *p < '1' || *p > '9') return -BLOCK_EINVAL;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p) return -BLOCK_EINVAL;
    *out_index = value;
    return 0;
}

int reliefos_block_partition_path(const char *disk_path, uint32_t index,
                                char *out, uint32_t capacity)
{
    uint32_t length;
    if (!disk_path || !out || !capacity || index >= RELIEFOS_BLOCK_MAX_PARTITIONS) {
        return -BLOCK_EINVAL;
    }
    length = (uint32_t)strlen(disk_path);
    if (length + 8u >= capacity) return -BLOCK_EINVAL;
    memcpy(out, disk_path, length);
    if (strncmp(disk_path, "/dev/nvme", 9) == 0)
        out[length++] = 'p';
    {
        char digits[12];
        uint32_t count = 0;
        uint32_t value = index + 1u;
        do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
        while (count) out[length++] = digits[--count];
    }
    out[length] = 0;
    return 0;
}

const char *reliefos_block_filesystem_name(uint32_t filesystem)
{
    switch (filesystem) {
    case RELIEFOS_BLOCK_FILESYSTEM_FAT32: return "fat32";
    case RELIEFOS_BLOCK_FILESYSTEM_EXT2: return "ext2";
    case RELIEFOS_BLOCK_FILESYSTEM_EXT4: return "ext4";
    case RELIEFOS_BLOCK_FILESYSTEM_ISO9660: return "iso9660";
    case RELIEFOS_BLOCK_FILESYSTEM_EXFAT: return "exfat";
    default: return "unknown";
    }
}

const char *reliefos_block_gpt_type_name(uint32_t type)
{
    switch (type) {
    case RELIEFOS_BLOCK_GPT_ESP: return "esp";
    case RELIEFOS_BLOCK_GPT_LINUX: return "linux";
    case RELIEFOS_BLOCK_GPT_BASIC_DATA: return "basic";
    default: return "other";
    }
}

static int block_guid_empty(const uint8_t guid[16])
{
    for (uint32_t i = 0; i < 16; ++i) if (guid[i]) return 0;
    return 1;
}

static uint32_t block_guid_type(const uint8_t guid[16])
{
    if (memcmp(guid, block_guid_esp, 16) == 0) return RELIEFOS_BLOCK_GPT_ESP;
    if (memcmp(guid, block_guid_linux, 16) == 0) return RELIEFOS_BLOCK_GPT_LINUX;
    if (memcmp(guid, block_guid_basic, 16) == 0) return RELIEFOS_BLOCK_GPT_BASIC_DATA;
    return 0;
}

static void block_set_guid(uint8_t guid[16], uint32_t type)
{
    const uint8_t *source = type == RELIEFOS_BLOCK_GPT_ESP ? block_guid_esp :
                            type == RELIEFOS_BLOCK_GPT_LINUX ? block_guid_linux : block_guid_basic;
    memcpy(guid, source, 16);
}

static void block_guid_make(uint8_t guid[16], uint64_t seed)
{
    for (uint32_t i = 0; i < 16; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        guid[i] = (uint8_t)(seed >> ((i & 7u) * 8u));
    }
    guid[6] = (guid[6] & 0x0fu) | 0x40u;
    guid[8] = (guid[8] & 0x3fu) | 0x80u;
}

static void block_name_set(uint16_t name[36], const char *text)
{
    uint32_t i = 0;
    for (; i < 36u && text && text[i]; ++i)
        name[i] = (uint8_t)text[i] >= 32u && (uint8_t)text[i] <= 126u ? (uint8_t)text[i] : '?';
    for (; i < 36u; ++i) name[i] = 0;
}

static void block_name_get(char *text, uint32_t capacity, const uint16_t name[36])
{
    uint32_t i = 0;
    if (!text || !capacity) return;
    while (i < 36u && name[i] && i + 1u < capacity) {
        text[i] = name[i] < 128u ? (char)name[i] : '?';
        ++i;
    }
    text[i] = 0;
}

static int block_gpt_header_valid(const struct block_gpt_header *header, uint64_t sectors)
{
    struct block_gpt_header copy;
    uint64_t table_bytes;
    uint64_t table_sectors;
    uint64_t table_last_lba;
    if (!header || header->signature != 0x5452415020494645ULL ||
        header->revision < 0x00010000u || header->header_size != sizeof(*header) ||
        header->reserved != 0 || sectors < 3u ||
        header->current_lba >= sectors || header->backup_lba >= sectors ||
        header->backup_lba == header->current_lba ||
        header->first_usable_lba > header->last_usable_lba ||
        header->last_usable_lba >= sectors || header->first_usable_lba < 2u ||
        header->partition_entry_count == 0 || header->partition_entry_count > GPT_ENTRY_MAX ||
        header->partition_entry_size != GPT_ENTRY_SIZE ||
        header->partition_entries_lba < 2u ||
        header->partition_entries_lba >= sectors) return -BLOCK_EINVAL;
    table_bytes = (uint64_t)header->partition_entry_count * header->partition_entry_size;
    table_sectors = (table_bytes + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
    if (table_bytes > GPT_DEFAULT_TABLE_BYTES || table_sectors == 0 ||
        table_sectors > sectors - header->partition_entries_lba) {
        return -BLOCK_EINVAL;
    }
    table_last_lba = header->partition_entries_lba + table_sectors - 1u;
    if ((header->current_lba >= header->partition_entries_lba &&
         header->current_lba <= table_last_lba) ||
        (header->backup_lba >= header->partition_entries_lba &&
         header->backup_lba <= table_last_lba) ||
        !(table_last_lba < header->first_usable_lba ||
          header->partition_entries_lba > header->last_usable_lba)) {
        return -BLOCK_EINVAL;
    }
    copy = *header;
    copy.header_crc32 = 0;
    return block_crc32(&copy, header->header_size) == header->header_crc32 ? 0 : -BLOCK_EIO;
}

static void block_gpt_free(struct block_gpt_table *table)
{
    if (table && table->entries) free(table->entries);
    if (table) memset(table, 0, sizeof(*table));
}

static void block_gpt_rebuild_backup(const struct block_gpt_header *primary,
                                     uint64_t sectors,
                                     struct block_gpt_header *backup)
{
    uint64_t table_bytes;
    uint64_t table_sectors;
    if (!primary || !backup) return;
    table_bytes = (uint64_t)primary->partition_entry_count * primary->partition_entry_size;
    table_sectors = (table_bytes + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
    if (!table_sectors || sectors <= table_sectors + 1u) return;
    *backup = *primary;
    backup->current_lba = sectors - 1u;
    backup->backup_lba = primary->current_lba;
    backup->partition_entries_lba = sectors - table_sectors - 1u;
    backup->header_crc32 = 0;
}

static int block_gpt_entries_valid(const struct block_gpt_table *table)
{
    if (!table || !table->entries) return -BLOCK_EINVAL;
    for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
        const struct block_gpt_entry *entry = &table->entries[i];
        if (block_guid_empty(entry->type_guid)) continue;
        if (block_guid_empty(entry->unique_guid) ||
            entry->first_lba < table->primary.first_usable_lba ||
            entry->last_lba < entry->first_lba ||
            entry->last_lba > table->primary.last_usable_lba) {
            return -BLOCK_EINVAL;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const struct block_gpt_entry *other = &table->entries[j];
            if (!block_guid_empty(other->type_guid) &&
                !(entry->last_lba < other->first_lba ||
                  entry->first_lba > other->last_lba)) {
                return -BLOCK_EINVAL;
            }
        }
    }
    return 0;
}

static int block_gpt_load_fd(int fd, uint64_t sectors, struct block_gpt_table *table)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint32_t checksum;
    int ret;
    if (!table) return -BLOCK_EINVAL;
    memset(table, 0, sizeof(*table));
    ret = block_io(fd, BLOCK_SECTOR_SIZE, sector, sizeof(sector), 0);
    if (ret == 0) memcpy(&table->primary, sector, sizeof(table->primary));
    if (ret < 0) return ret;
    if (table->primary.signature != 0x5452415020494645ULL) return -BLOCK_ENOENT;
    ret = block_gpt_header_valid(&table->primary, sectors);
    if (ret < 0 || table->primary.current_lba != 1u) {
        return ret < 0 ? ret : -BLOCK_EINVAL;
    }
    table->entry_bytes = table->primary.partition_entry_count *
                         table->primary.partition_entry_size;
    table->entry_sectors = (table->entry_bytes + BLOCK_SECTOR_SIZE - 1u) /
                           BLOCK_SECTOR_SIZE;
    table->entries = calloc(table->entry_sectors, BLOCK_SECTOR_SIZE);
    if (!table->entries) return -12;
    ret = block_io(fd, table->primary.partition_entries_lba * BLOCK_SECTOR_SIZE,
                   table->entries, table->entry_sectors * BLOCK_SECTOR_SIZE, 0);
    if (ret < 0) { block_gpt_free(table); return ret; }
    checksum = block_crc32(table->entries, table->entry_bytes);
    if (checksum != table->primary.partition_entries_crc32) {
        block_gpt_free(table);
        return -BLOCK_EIO;
    }
    ret = block_gpt_entries_valid(table);
    if (ret < 0) {
        block_gpt_free(table);
        return ret;
    }
    /* A valid primary table is sufficient for inspection and recovery.  The
     * backup header is optional here because older installers could leave it
     * stale after an interrupted write.  Rebuild its canonical location so
     * the next GPT update repairs both copies. */
    ret = block_io(fd, table->primary.backup_lba * BLOCK_SECTOR_SIZE,
                   sector, sizeof(sector), 0);
    if (ret == 0) memcpy(&table->backup, sector, sizeof(table->backup));
    if (ret < 0 || block_gpt_header_valid(&table->backup, sectors) < 0 ||
        table->backup.current_lba != table->primary.backup_lba ||
        table->backup.backup_lba != 1u ||
        table->backup.partition_entries_lba != sectors - table->entry_sectors - 1u ||
        table->backup.partition_entry_count != table->primary.partition_entry_count ||
        table->backup.partition_entry_size != table->primary.partition_entry_size ||
        memcmp(table->backup.disk_guid, table->primary.disk_guid,
               sizeof(table->primary.disk_guid)) != 0 ||
        table->backup.first_usable_lba != table->primary.first_usable_lba ||
        table->backup.last_usable_lba != table->primary.last_usable_lba ||
        table->backup.partition_entries_crc32 != checksum) {
        block_gpt_rebuild_backup(&table->primary, sectors, &table->backup);
    }
    table->backup.partition_entries_crc32 = checksum;
    return 0;
}

static int block_gpt_write_fd(int fd, struct block_gpt_table *table)
{
    uint8_t sector[BLOCK_SECTOR_SIZE] = {0};
    uint32_t checksum;
    int ret;
    if (!table || !table->entries) return -BLOCK_EINVAL;
    if (!table->entry_bytes || !table->entry_sectors) return -BLOCK_EINVAL;
    checksum = block_crc32(table->entries, table->entry_bytes);
    table->primary.partition_entries_crc32 = checksum;
    table->backup.partition_entries_crc32 = checksum;
    table->primary.header_crc32 = 0;
    table->backup.header_crc32 = 0;
    table->primary.header_crc32 = block_crc32(&table->primary, table->primary.header_size);
    table->backup.header_crc32 = block_crc32(&table->backup, table->backup.header_size);
    ret = block_io(fd, table->backup.partition_entries_lba * BLOCK_SECTOR_SIZE,
                   table->entries, table->entry_sectors * BLOCK_SECTOR_SIZE, 1);
    if (ret < 0) return ret;
    memcpy(sector, &table->backup, sizeof(table->backup));
    ret = block_io(fd, table->backup.current_lba * BLOCK_SECTOR_SIZE,
                   sector, sizeof(sector), 1);
    if (ret < 0) return ret;
    ret = block_io(fd, table->primary.partition_entries_lba * BLOCK_SECTOR_SIZE,
                   table->entries, table->entry_sectors * BLOCK_SECTOR_SIZE, 1);
    if (ret < 0) return ret;
    memset(sector, 0, sizeof(sector));
    memcpy(sector, &table->primary, sizeof(table->primary));
    return block_io(fd, table->primary.current_lba * BLOCK_SECTOR_SIZE,
                    sector, sizeof(sector), 1);
}

static int block_reread(int fd)
{
    long ret = ioctl(fd, BLKRRPART, 0);
    return ret < 0 ? -errno : 0;
}

int reliefos_block_get_info(const char *path, struct reliefos_block_disk_info *out)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t index;
    int ret;
    if (!out || !path) return -BLOCK_EINVAL;
    memset(out, 0, sizeof(*out));
    ret = block_open_info(path, 0, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    out->sector_count = sectors;
    out->sector_size = sector_size;
    block_copy(out->path, sizeof(out->path), path);
    if (block_disk_index(path, &index) == 0) out->id = index;
    block_copy(out->name, sizeof(out->name), "Block Disk");
    (void)close(fd);
    return 0;
}

int reliefos_block_list_disks(struct reliefos_block_disk_info *disks, uint32_t capacity,
                            uint32_t *out_count)
{
    uint32_t count = 0;
    int first_error = 0;
    if (!out_count || (capacity && !disks)) return -BLOCK_EINVAL;
    for (uint32_t index = 0; index < RELIEFOS_BLOCK_MAX_DISKS; ++index) {
        char path[RELIEFOS_BLOCK_PATH_LEN];
        struct reliefos_block_disk_info info;
        snprintf(path, sizeof(path), "/dev/sd%c", (char)('a' + index));
        int ret = reliefos_block_get_info(path, &info);
        if (ret < 0) {
            if (ret != -BLOCK_ENOENT && !first_error) first_error = ret;
            continue;
        }
        info.id = index;
        if (count < capacity) disks[count] = info;
        ++count;
    }
    for (uint32_t controller = 0; controller < RELIEFOS_BLOCK_MAX_DISKS; ++controller) {
        for (uint32_t namespace_id = 1; namespace_id <= RELIEFOS_BLOCK_MAX_PARTITIONS / 16u;
             ++namespace_id) {
            char path[RELIEFOS_BLOCK_PATH_LEN];
            struct reliefos_block_disk_info info;
            snprintf(path, sizeof(path), "/dev/nvme%un%u", controller, namespace_id);
            int ret = reliefos_block_get_info(path, &info);
            if (ret < 0) {
                if (ret != -BLOCK_ENOENT && !first_error) first_error = ret;
                continue;
            }
            info.id = count;
            if (count < capacity) disks[count] = info;
            ++count;
            if (count >= RELIEFOS_BLOCK_MAX_DISKS) break;
        }
        if (count >= RELIEFOS_BLOCK_MAX_DISKS) break;
    }
    *out_count = count;
    return first_error;
}

int reliefos_block_gpt_initialize(const char *disk_path, int force)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    struct block_gpt_table existing;
    struct block_gpt_table table;
    uint8_t mbr[BLOCK_SECTOR_SIZE] = {0};
    int ret;
    ret = block_open_info(disk_path, 1, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    if (sectors < 655360ULL) { (void)close(fd); return -BLOCK_ENOSPC; }
    if (block_gpt_load_fd(fd, sectors, &existing) == 0) {
        block_gpt_free(&existing);
        if (!force) { (void)close(fd); return -BLOCK_EEXIST; }
    }
    memset(&table, 0, sizeof(table));
    table.entries = calloc(GPT_ENTRY_MAX, sizeof(*table.entries));
    if (!table.entries) { (void)close(fd); return -12; }
    table.primary.signature = table.backup.signature = 0x5452415020494645ULL;
    table.primary.revision = table.backup.revision = 0x00010000u;
    table.primary.header_size = table.backup.header_size = sizeof(struct block_gpt_header);
    table.primary.current_lba = 1u;
    table.primary.backup_lba = sectors - 1u;
    table.primary.first_usable_lba = 2048u;
    table.primary.last_usable_lba = sectors - GPT_DEFAULT_TABLE_SECTORS - 2u;
    table.primary.partition_entries_lba = 2u;
    table.primary.partition_entry_count = GPT_ENTRY_MAX;
    table.primary.partition_entry_size = GPT_ENTRY_SIZE;
    block_guid_make(table.primary.disk_guid, sectors ^ 0x4c34475054494e49ULL);
    table.backup = table.primary;
    table.backup.current_lba = sectors - 1u;
    table.backup.backup_lba = 1u;
    table.backup.partition_entries_lba = sectors - GPT_DEFAULT_TABLE_SECTORS - 1u;
    table.entry_bytes = GPT_DEFAULT_TABLE_BYTES;
    table.entry_sectors = GPT_DEFAULT_TABLE_SECTORS;
    ret = block_zero(fd, 0, 2048ULL * BLOCK_SECTOR_SIZE);
    if (ret == 0) ret = block_zero(fd, table.backup.partition_entries_lba * BLOCK_SECTOR_SIZE,
                                   (GPT_DEFAULT_TABLE_SECTORS + 1u) * BLOCK_SECTOR_SIZE);
    if (ret == 0) {
        mbr[446 + 4] = 0xee;
        mbr[446 + 8] = 1;
        {
            uint32_t span = sectors - 1u > 0xffffffffULL ? 0xffffffffu : (uint32_t)(sectors - 1u);
            memcpy(mbr + 446 + 12, &span, sizeof(span));
        }
        mbr[510] = 0x55; mbr[511] = 0xaa;
        ret = block_io(fd, 0, mbr, sizeof(mbr), 1);
    }
    if (ret == 0) ret = block_gpt_write_fd(fd, &table);
    if (ret == 0) ret = block_reread(fd);
    block_gpt_free(&table);
    (void)close(fd);
    return ret;
}

static int block_partition_probe_fd(int fd, uint32_t *out_filesystem)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    int ret;
    if (!out_filesystem) return -BLOCK_EINVAL;
    *out_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    ret = block_io(fd, 0, sector, sizeof(sector), 0);
    if (ret < 0) return ret;
    if (sector[510] == 0x55 && sector[511] == 0xaa && memcmp(sector + 82, "FAT32   ", 8) == 0)
        *out_filesystem = RELIEFOS_BLOCK_FILESYSTEM_FAT32;
    else if (sector[510] == 0x55 && sector[511] == 0xaa && memcmp(sector + 3, "EXFAT   ", 8) == 0)
        *out_filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXFAT;
    else {
        ret = block_io(fd, 1024, sector, sizeof(sector), 0);
        if (ret < 0) return ret;
        if (*(uint16_t *)(void *)(sector + 56) == EXT2_SUPER_MAGIC) {
            uint32_t incompat = *(uint32_t *)(void *)(sector + 96);
            *out_filesystem = (incompat & 0x40u) ?
                RELIEFOS_BLOCK_FILESYSTEM_EXT4 : RELIEFOS_BLOCK_FILESYSTEM_EXT2;
        }
    }
    return 0;
}

int reliefos_block_probe_filesystem(const char *partition_path, uint32_t *out_filesystem)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    int ret = block_open_info(partition_path, 0, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    ret = block_partition_probe_fd(fd, out_filesystem);
    (void)close(fd);
    return ret;
}

int reliefos_block_list_partitions(const char *disk_path,
                                 struct reliefos_block_partition *partitions,
                                 uint32_t capacity, uint32_t *out_count)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    struct block_gpt_table table;
    uint32_t count = 0;
    int ret;
    if (!out_count || (capacity && !partitions)) return -BLOCK_EINVAL;
    *out_count = 0;
    ret = block_open_info(disk_path, 0, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    ret = block_gpt_load_fd(fd, sectors, &table);
    (void)close(fd);
    if (ret == -BLOCK_ENOENT) {
        *out_count = 0;
        return 0;
    }
    if (ret < 0) return ret;
    for (uint32_t index = 0; index < table.primary.partition_entry_count; ++index) {
        const struct block_gpt_entry *entry = &table.entries[index];
        if (block_guid_empty(entry->type_guid)) continue;
        if (count < capacity) {
            struct reliefos_block_partition *out = &partitions[count];
            memset(out, 0, sizeof(*out));
            out->index = index;
            out->first_lba = entry->first_lba;
            out->sector_count = entry->last_lba - entry->first_lba + 1u;
            out->gpt_type = block_guid_type(entry->type_guid);
            block_name_get(out->name, sizeof(out->name), entry->name);
            (void)reliefos_block_partition_path(disk_path, index, out->path, sizeof(out->path));
            (void)reliefos_block_probe_filesystem(out->path, &out->filesystem);
        }
        ++count;
    }
    block_gpt_free(&table);
    *out_count = count;
    return 0;
}

int reliefos_block_partition_uuid(const char *disk_path, uint32_t index, char uuid[37])
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    struct block_gpt_table table;
    if (!uuid) return -BLOCK_EINVAL;
    int ret = block_open_info(disk_path, 0, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    ret = block_gpt_load_fd(fd, sectors, &table);
    (void)close(fd);
    if (ret < 0) return ret;
    if (index >= table.primary.partition_entry_count ||
        block_guid_empty(table.entries[index].type_guid)) ret = -BLOCK_ENOENT;
    else {
        const uint8_t *g = table.entries[index].unique_guid;
        snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 g[3],g[2],g[1],g[0],g[5],g[4],g[7],g[6],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);
    }
    block_gpt_free(&table);
    return ret;
}

static int block_gpt_update(const char *disk_path,
                            int (*update)(struct block_gpt_table *, void *), void *context)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    struct block_gpt_table table;
    int ret = block_open_info(disk_path, 1, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    ret = block_gpt_load_fd(fd, sectors, &table);
    if (ret == 0) ret = update(&table, context);
    if (ret == 0) ret = block_gpt_write_fd(fd, &table);
    if (ret == 0) ret = block_reread(fd);
    block_gpt_free(&table);
    (void)close(fd);
    return ret;
}

struct block_create_request { uint32_t filesystem; uint32_t size_mib; const char *name; uint32_t index; };
static int block_create_entry(struct block_gpt_table *table, void *context)
{
    struct block_create_request *request = context;
    uint64_t required = (uint64_t)request->size_mib * 2048u;
    uint64_t cursor = table->primary.first_usable_lba;
    uint32_t free_index = table->primary.partition_entry_count;
    if (!required) return -BLOCK_EINVAL;
    for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i)
        if (block_guid_empty(table->entries[i].type_guid)) { free_index = i; break; }
    if (free_index == table->primary.partition_entry_count) return -BLOCK_ENOSPC;
    for (;;) {
        uint64_t next = table->primary.last_usable_lba + 1u;
        uint64_t end;
        cursor = (cursor + 2047u) & ~2047ULL;
        for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
            const struct block_gpt_entry *entry = &table->entries[i];
            if (!block_guid_empty(entry->type_guid) && entry->first_lba >= cursor && entry->first_lba < next)
                next = entry->first_lba;
        }
        if (required <= next - cursor) {
            end = cursor + required - 1u;
            if (end > table->primary.last_usable_lba) return -BLOCK_ENOSPC;
            memset(&table->entries[free_index], 0, sizeof(table->entries[free_index]));
            block_set_guid(table->entries[free_index].type_guid,
                           (request->filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
                            request->filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4) ?
                           RELIEFOS_BLOCK_GPT_LINUX : RELIEFOS_BLOCK_GPT_BASIC_DATA);
            block_guid_make(table->entries[free_index].unique_guid,
                            cursor ^ ((uint64_t)free_index << 32));
            table->entries[free_index].first_lba = cursor;
            table->entries[free_index].last_lba = end;
            block_name_set(table->entries[free_index].name,
                           request->name && request->name[0] ? request->name : "ReliefOS Data");
            request->index = free_index;
            return 0;
        }
        if (next > table->primary.last_usable_lba) return -BLOCK_ENOSPC;
        for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
            const struct block_gpt_entry *entry = &table->entries[i];
            if (!block_guid_empty(entry->type_guid) && entry->first_lba == next) {
                cursor = entry->last_lba + 1u;
                break;
            }
        }
    }
}

int reliefos_block_gpt_create(const char *disk_path, uint32_t filesystem,
                            uint32_t size_mib, const char *name, uint32_t *out_index)
{
    struct block_create_request request = {filesystem, size_mib, name, 0};
    int ret;
    if (filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN &&
        filesystem != RELIEFOS_BLOCK_FILESYSTEM_FAT32 &&
        filesystem != RELIEFOS_BLOCK_FILESYSTEM_EXT2 &&
        filesystem != RELIEFOS_BLOCK_FILESYSTEM_EXT4 &&
        filesystem != RELIEFOS_BLOCK_FILESYSTEM_EXFAT) return -BLOCK_EINVAL;
    ret = block_gpt_update(disk_path, block_create_entry, &request);
    if (ret == 0 && out_index) *out_index = request.index;
    return ret;
}

struct block_index_request { uint32_t index; uint32_t type; const char *name; };
static int block_delete_entry(struct block_gpt_table *table, void *context)
{
    struct block_index_request *request = context;
    if (request->index >= table->primary.partition_entry_count || block_guid_empty(table->entries[request->index].type_guid)) return -BLOCK_ENOENT;
    memset(&table->entries[request->index], 0, sizeof(table->entries[request->index]));
    return 0;
}
static int block_set_type_entry(struct block_gpt_table *table, void *context)
{
    struct block_index_request *request = context;
    if (request->index >= table->primary.partition_entry_count || block_guid_empty(table->entries[request->index].type_guid) ||
        request->type < RELIEFOS_BLOCK_GPT_BASIC_DATA || request->type > RELIEFOS_BLOCK_GPT_LINUX) return -BLOCK_EINVAL;
    block_set_guid(table->entries[request->index].type_guid, request->type);
    return 0;
}
static int block_set_name_entry(struct block_gpt_table *table, void *context)
{
    struct block_index_request *request = context;
    if (request->index >= table->primary.partition_entry_count || block_guid_empty(table->entries[request->index].type_guid)) return -BLOCK_ENOENT;
    block_name_set(table->entries[request->index].name, request->name);
    return 0;
}
int reliefos_block_gpt_delete(const char *disk_path, uint32_t index)
{
    struct block_index_request request = {index, 0, NULL};
    return block_gpt_update(disk_path, block_delete_entry, &request);
}
int reliefos_block_gpt_set_type(const char *disk_path, uint32_t index, uint32_t type)
{
    struct block_index_request request = {index, type, NULL};
    return block_gpt_update(disk_path, block_set_type_entry, &request);
}
int reliefos_block_gpt_set_name(const char *disk_path, uint32_t index, const char *name)
{
    struct block_index_request request = {index, 0, name};
    return block_gpt_update(disk_path, block_set_name_entry, &request);
}

struct block_resize_request { uint32_t index; uint32_t size_mib; };
static int block_resize_entry(struct block_gpt_table *table, void *context)
{
    struct block_resize_request *request = context;
    struct block_gpt_entry *entry;
    uint64_t sectors, new_last, next = table->primary.last_usable_lba + 1u;
    if (request->index >= table->primary.partition_entry_count ||
        block_guid_empty(table->entries[request->index].type_guid) ||
        !request->size_mib) return -BLOCK_EINVAL;
    entry = &table->entries[request->index];
    sectors = (uint64_t)request->size_mib * 2048u;
    if (sectors > UINT64_MAX - entry->first_lba) return -BLOCK_EINVAL;
    new_last = entry->first_lba + sectors - 1u;
    for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
        const struct block_gpt_entry *other = &table->entries[i];
        if (i != request->index && !block_guid_empty(other->type_guid) &&
            other->first_lba > entry->first_lba && other->first_lba < next)
            next = other->first_lba;
    }
    if (new_last >= next || new_last > table->primary.last_usable_lba)
        return -BLOCK_ENOSPC;
    entry->last_lba = new_last;
    return 0;
}

int reliefos_block_gpt_resize(const char *disk_path, uint32_t index, uint32_t size_mib)
{
    struct block_resize_request request = {index, size_mib};
    return block_gpt_update(disk_path, block_resize_entry, &request);
}

static void block_put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = value; bytes[1] = value >> 8; bytes[2] = value >> 16; bytes[3] = value >> 24;
}
static void block_put64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8u));
}
static void block_put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = value; bytes[1] = value >> 8;
}

static int block_format_fat32(int fd, uint64_t sectors, const char *label)
{
    uint8_t sector[BLOCK_SECTOR_SIZE] = {0};
    const uint32_t reserved = 32u;
    const uint32_t fat_count = 2u;
    uint32_t spc = sectors <= 262144u ? 2u : sectors <= 1048576u ? 4u : 8u;
    uint32_t fat_size = 1u, data_sectors = 0, clusters = 0, data_start;
    int ret;
    if (sectors < 65536u || sectors > 0xffffffffULL) return -BLOCK_ENOSPC;
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t next;
        data_sectors = (uint32_t)sectors - reserved - fat_count * fat_size;
        clusters = data_sectors / spc;
        next = ((clusters + 2u) * 4u + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
        if (next == fat_size) break;
        fat_size = next;
    }
    data_start = reserved + fat_count * fat_size;
    if (clusters < 2u) return -BLOCK_ENOSPC;
    ret = block_zero(fd, 0, (uint64_t)(data_start + spc) * BLOCK_SECTOR_SIZE);
    if (ret < 0) return ret;
    {
        struct block_fat32_bpb *bpb = (struct block_fat32_bpb *)(void *)sector;
        bpb->jump[0] = 0xeb; bpb->jump[1] = 0x58; bpb->jump[2] = 0x90;
        memcpy(bpb->oem, "RELIEFOS", 8);
        bpb->bytes_per_sector = BLOCK_SECTOR_SIZE; bpb->sectors_per_cluster = spc;
        bpb->reserved_sector_count = reserved; bpb->fat_count = fat_count; bpb->media = 0xf8;
        bpb->sectors_per_track = 63; bpb->head_count = 255; bpb->total_sectors32 = sectors;
        bpb->fat_size32 = fat_size; bpb->root_cluster = 2; bpb->fs_info = 1; bpb->backup_boot_sector = 6;
        bpb->drive_number = 0x80; bpb->boot_signature = 0x29; bpb->volume_id = 0x4c454f34u;
        memset(bpb->volume_label, ' ', sizeof(bpb->volume_label));
        if (label) memcpy(bpb->volume_label, label, strlen(label) > 11 ? 11 : strlen(label));
        else memcpy(bpb->volume_label, "RELIEFOS   ", 11);
        memcpy(bpb->fs_type, "FAT32   ", 8); sector[510] = 0x55; sector[511] = 0xaa;
    }
    ret = block_io(fd, 0, sector, sizeof(sector), 1);
    if (ret < 0) return ret;
    ret = block_io(fd, 6u * BLOCK_SECTOR_SIZE, sector, sizeof(sector), 1);
    if (ret < 0) return ret;
    memset(sector, 0, sizeof(sector));
    block_put32(sector, 0x41615252u); block_put32(sector + 484, 0x61417272u);
    block_put32(sector + 488, clusters - 1u); block_put32(sector + 492, 3u); block_put32(sector + 508, 0xaa550000u);
    ret = block_io(fd, BLOCK_SECTOR_SIZE, sector, sizeof(sector), 1);
    if (ret < 0) return ret;
    ret = block_io(fd, 7u * BLOCK_SECTOR_SIZE, sector, sizeof(sector), 1);
    if (ret < 0) return ret;
    for (uint32_t copy = 0; copy < fat_count; ++copy) {
        memset(sector, 0, sizeof(sector));
        block_put32(sector, 0x0ffffff8u); block_put32(sector + 4, 0xffffffffu); block_put32(sector + 8, FAT32_EOC);
        ret = block_io(fd, (uint64_t)(reserved + copy * fat_size) * BLOCK_SECTOR_SIZE,
                       sector, sizeof(sector), 1);
        if (ret < 0) return ret;
    }
    return 0;
}

static void block_set_bit(uint8_t *bitmap, uint32_t bit) { bitmap[bit / 8u] |= 1u << (bit & 7u); }
static int block_write_ext2_block(int fd, uint32_t block, const void *data)
{
    return block_io(fd, (uint64_t)block * EXT2_BLOCK_SIZE, (void *)data, EXT2_BLOCK_SIZE, 1);
}
static void block_group_desc(struct block_ext2_group *desc, uint32_t group, uint32_t blocks,
                             uint32_t descriptor_blocks, uint32_t inode_table_blocks)
{
    uint32_t start = group * EXT2_BLOCKS_PER_GROUP;
    uint32_t metadata = 3u + descriptor_blocks + inode_table_blocks;
    uint32_t allocated = metadata + (group == 0u ? 1u : 0u);
    memset(desc, 0, sizeof(*desc));
    desc->block_bitmap = start + 1u + descriptor_blocks;
    desc->inode_bitmap = desc->block_bitmap + 1u;
    desc->inode_table = desc->inode_bitmap + 1u;
    desc->free_blocks_count = blocks > allocated ? blocks - allocated : 0;
    desc->free_inodes_count = EXT2_INODES_PER_GROUP - (group == 0u ? 10u : 0u);
    desc->used_dirs_count = group == 0u;
}

static int block_format_ext_family(int fd, uint64_t sectors, const char *label, int ext4)
{
    uint32_t blocks, groups, descriptors_per_block, descriptor_blocks, inode_table_blocks, free_blocks = 0, free_inodes = 0;
    uint8_t data[EXT2_BLOCK_SIZE];
    struct block_ext2_super super;
    int ret;
    if (sectors < 262144ULL || sectors / 8u > 0xffffffffULL) return -BLOCK_ENOSPC;
    blocks = sectors / 8u; groups = (blocks + EXT2_BLOCKS_PER_GROUP - 1u) / EXT2_BLOCKS_PER_GROUP;
    descriptors_per_block = EXT2_BLOCK_SIZE / sizeof(struct block_ext2_group);
    descriptor_blocks = (groups + descriptors_per_block - 1u) / descriptors_per_block;
    inode_table_blocks = (EXT2_INODES_PER_GROUP * 128u) / EXT2_BLOCK_SIZE;
    if (!groups || blocks < inode_table_blocks + descriptor_blocks + 4u) return -BLOCK_ENOSPC;
    memset(&super, 0, sizeof(super));
    for (uint32_t group = 0; group < groups; ++group) {
        struct block_ext2_group desc;
        uint32_t group_blocks = blocks - group * EXT2_BLOCKS_PER_GROUP;
        if (group_blocks > EXT2_BLOCKS_PER_GROUP) group_blocks = EXT2_BLOCKS_PER_GROUP;
        block_group_desc(&desc, group, group_blocks, descriptor_blocks, inode_table_blocks);
        free_blocks += desc.free_blocks_count; free_inodes += desc.free_inodes_count;
    }
    super.inodes_count = groups * EXT2_INODES_PER_GROUP; super.blocks_count = blocks;
    super.free_blocks_count = free_blocks; super.free_inodes_count = free_inodes;
    super.log_block_size = 2; super.log_frag_size = 2; super.blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    super.frags_per_group = EXT2_BLOCKS_PER_GROUP; super.inodes_per_group = EXT2_INODES_PER_GROUP;
    super.magic = EXT2_SUPER_MAGIC; super.state = 1; super.errors = 1; super.rev_level = 1;
    super.first_ino = 11; super.inode_size = 128;
    super.feature_incompat = 2u | (ext4 ? 0x40u : 0u);
    memcpy(super.volume_name, label && label[0] ? label : "RELIEFOS", label && label[0] && strlen(label) < 16 ? strlen(label) : 8);
    for (uint32_t group = 0; group < groups; ++group) {
        uint32_t start = group * EXT2_BLOCKS_PER_GROUP;
        memset(data, 0, sizeof(data));
        memcpy(data + (group == 0u ? 1024u : 0u), &super, sizeof(super));
        ret = block_write_ext2_block(fd, start, data);
        if (ret < 0) return ret;
        for (uint32_t table_block = 0; table_block < descriptor_blocks; ++table_block) {
            struct block_ext2_group *descs = (struct block_ext2_group *)(void *)data;
            memset(data, 0, sizeof(data));
            for (uint32_t slot = 0; slot < descriptors_per_block; ++slot) {
                uint32_t descriptor_group = table_block * descriptors_per_block + slot;
                uint32_t group_blocks;
                if (descriptor_group >= groups) break;
                group_blocks = blocks - descriptor_group * EXT2_BLOCKS_PER_GROUP;
                if (group_blocks > EXT2_BLOCKS_PER_GROUP) group_blocks = EXT2_BLOCKS_PER_GROUP;
                block_group_desc(&descs[slot], descriptor_group, group_blocks, descriptor_blocks, inode_table_blocks);
            }
            ret = block_write_ext2_block(fd, start + 1u + table_block, data);
            if (ret < 0) return ret;
        }
    }
    for (uint32_t group = 0; group < groups; ++group) {
        uint32_t start = group * EXT2_BLOCKS_PER_GROUP, group_blocks = blocks - start;
        uint32_t metadata = 3u + descriptor_blocks + inode_table_blocks;
        uint32_t block_bitmap = start + 1u + descriptor_blocks, inode_bitmap = block_bitmap + 1u, inode_table = inode_bitmap + 1u;
        if (group_blocks > EXT2_BLOCKS_PER_GROUP) group_blocks = EXT2_BLOCKS_PER_GROUP;
        memset(data, 0, sizeof(data));
        for (uint32_t bit = 0; bit < metadata + (group == 0u); ++bit) block_set_bit(data, bit);
        for (uint32_t bit = group_blocks; bit < EXT2_BLOCK_SIZE * 8u; ++bit) block_set_bit(data, bit);
        ret = block_write_ext2_block(fd, block_bitmap, data);
        if (ret < 0) return ret;
        memset(data, 0, sizeof(data));
        if (group == 0u) for (uint32_t bit = 0; bit < 10u; ++bit) block_set_bit(data, bit);
        for (uint32_t bit = EXT2_INODES_PER_GROUP; bit < EXT2_BLOCK_SIZE * 8u; ++bit) block_set_bit(data, bit);
        ret = block_write_ext2_block(fd, inode_bitmap, data);
        if (ret < 0) return ret;
        memset(data, 0, sizeof(data));
        for (uint32_t i = 0; i < inode_table_blocks; ++i) {
            ret = block_write_ext2_block(fd, inode_table + i, data);
            if (ret < 0) return ret;
        }
    }
    memset(data, 0, sizeof(data));
    {
        struct block_ext2_inode *root = (struct block_ext2_inode *)(void *)(data + 128u);
        uint32_t root_block = 3u + descriptor_blocks + inode_table_blocks;
        root->mode = 0040755u; root->size_lo = EXT2_BLOCK_SIZE; root->links_count = 2; root->blocks_512 = 8;
        if (ext4) {
            root->flags = 0x00080000u;
            root->block[0] = 0x0001f30au;
            root->block[1] = 4u;
            root->block[2] = 0u;
            root->block[3] = 0u;
            root->block[4] = 1u;
            root->block[5] = root_block;
        } else {
            root->block[0] = root_block;
        }
    }
    ret = block_write_ext2_block(fd, 3u + descriptor_blocks, data);
    if (ret < 0) return ret;
    memset(data, 0, sizeof(data));
    {
        struct block_ext2_dirent *dot = (struct block_ext2_dirent *)(void *)data;
        struct block_ext2_dirent *dotdot = (struct block_ext2_dirent *)(void *)(data + 12u);
        dot->inode = 2; dot->rec_len = 12; dot->name_len = 1; dot->file_type = 2; dot->name[0] = '.';
        dotdot->inode = 2; dotdot->rec_len = EXT2_BLOCK_SIZE - 12u; dotdot->name_len = 2; dotdot->file_type = 2;
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
    }
    return block_write_ext2_block(fd, 3u + descriptor_blocks + inode_table_blocks, data);
}

static int block_format_ext2(int fd, uint64_t sectors, const char *label)
{
    return block_format_ext_family(fd, sectors, label, 0);
}

static int block_format_ext4(int fd, uint64_t sectors, const char *label)
{
    return block_format_ext_family(fd, sectors, label, 1);
}

#include "blockdev_exfat_upcase.inc"

static uint32_t block_exfat_boot_checksum(const uint8_t boot[BLOCK_SECTOR_SIZE])
{
    uint32_t sum = 0;
    for (uint32_t sector = 0; sector < 11u; ++sector) for (uint32_t byte = 0; byte < BLOCK_SECTOR_SIZE; ++byte) {
        uint8_t value = sector ? ((sector <= 8u && byte == 510u) ? 0x55u : (sector <= 8u && byte == 511u) ? 0xaau : 0u) : boot[byte];
        if (sector == 0u && (byte == 106u || byte == 107u || byte == 112u)) continue;
        sum = (sum << 31) | (sum >> 1); sum += value;
    }
    return sum;
}
static uint32_t block_exfat_upcase_checksum(uint32_t sum, uint8_t value)
{
    return ((sum << 31) | (sum >> 1)) + value;
}

static int block_format_exfat(int fd, uint64_t sectors, const char *label)
{
    uint8_t boot[BLOCK_SECTOR_SIZE] = {0};
    uint8_t scratch[BLOCK_IO_CHUNK] = {0};
    uint8_t spc_shift = sectors <= 524288ULL ? 3u : sectors <= 67108864ULL ? 6u : 8u;
    uint32_t spc = 1u << spc_shift, fat_length = 1u, fat_offset = 24u, heap_offset, cluster_count = 0;
    uint64_t bitmap_bytes, upcase_bytes = exfat_standard_upcase_len;
    uint32_t bitmap_clusters, upcase_clusters, bitmap_cluster = 2u, upcase_cluster, root_cluster, checksum = 0;
    int ret;
    if (sectors < 262144ULL || sectors > 0xffffffffULL) return -BLOCK_ENOSPC;
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t next;
        heap_offset = fat_offset + fat_length; cluster_count = (sectors - heap_offset) / spc;
        next = (((uint64_t)cluster_count + 2u) * 4u + 511u) / 512u;
        if (next == fat_length) break;
        fat_length = next;
    }
    heap_offset = fat_offset + fat_length; cluster_count = (sectors - heap_offset) / spc;
    bitmap_bytes = ((uint64_t)cluster_count + 7u) / 8u;
    bitmap_clusters = (bitmap_bytes + (uint64_t)spc * 512u - 1u) / ((uint64_t)spc * 512u);
    upcase_clusters = (upcase_bytes + (uint64_t)spc * 512u - 1u) / ((uint64_t)spc * 512u);
    upcase_cluster = bitmap_cluster + bitmap_clusters; root_cluster = upcase_cluster + upcase_clusters;
    if ((uint64_t)root_cluster - 2u >= cluster_count) return -BLOCK_ENOSPC;
    ret = block_zero(fd, 0, ((uint64_t)heap_offset +
                             (1u + bitmap_clusters + upcase_clusters) * (uint64_t)spc) * 512u);
    if (ret < 0) return ret;
    for (uint32_t sector = 0; sector < fat_length; ++sector) {
        memset(scratch, 0, 512); if (!sector) { block_put32(scratch, 0xfffffff8u); block_put32(scratch + 4, 0xffffffffu); }
        for (uint32_t marker = 0; marker < 3; ++marker) {
            uint32_t cluster = marker == 0 ? root_cluster : marker == 1 ? bitmap_cluster : upcase_cluster;
            if (cluster / 128u == sector) block_put32(scratch + (cluster % 128u) * 4u, 0xffffffffu);
        }
        ret = block_io(fd, (uint64_t)(fat_offset + sector) * 512u, scratch, 512, 1);
        if (ret < 0) return ret;
    }
    for (uint64_t sector = 0; sector < (bitmap_bytes + 511u) / 512u; ++sector) {
        uint64_t first = sector * 4096u, last = first + 4096u;
        if (last > cluster_count) last = cluster_count;
        memset(scratch, 0, 512);
        for (uint64_t bit = first; bit < last; ++bit) if (bit + 2u <= root_cluster) scratch[(bit - first) / 8u] |= 1u << ((bit - first) & 7u);
        ret = block_io(fd, ((uint64_t)heap_offset +
                            (uint64_t)(bitmap_cluster - 2u) * spc + sector) * 512u,
                       scratch, 512, 1);
        if (ret < 0) return ret;
    }
    for (uint64_t offset = 0; offset < upcase_bytes;) {
        uint32_t bytes = upcase_bytes - offset > sizeof(scratch) ? sizeof(scratch) : upcase_bytes - offset;
        uint32_t write_bytes = (bytes + 511u) & ~511u;
        memset(scratch, 0, write_bytes); memcpy(scratch, exfat_standard_upcase + offset, bytes);
        for (uint32_t i = 0; i < bytes; ++i) checksum = block_exfat_upcase_checksum(checksum, scratch[i]);
        ret = block_io(fd, ((uint64_t)heap_offset +
                            (uint64_t)(upcase_cluster - 2u) * spc + offset / 512u) * 512u,
                       scratch, write_bytes, 1);
        if (ret < 0) return ret;
        offset += bytes;
    }
    const char *volume_label = label && label[0] ? label : "RELIEFOS";
    uint32_t volume_label_length = (uint32_t)strlen(volume_label);
    if (volume_label_length > 11u) volume_label_length = 11u;
    memset(scratch, 0, sizeof(scratch)); scratch[0] = 0x83; scratch[1] = (uint8_t)volume_label_length;
    for (uint32_t i = 0; i < volume_label_length; ++i) block_put16(scratch + 2u + i * 2u, (uint8_t)volume_label[i]);
    scratch[32] = 0x81; block_put32(scratch + 52, bitmap_cluster); block_put64(scratch + 56, bitmap_bytes);
    scratch[64] = 0x82; block_put32(scratch + 68, checksum); block_put32(scratch + 84, upcase_cluster); block_put64(scratch + 88, upcase_bytes);
    ret = block_io(fd, ((uint64_t)heap_offset + (uint64_t)(root_cluster - 2u) * spc) * 512u,
                   scratch, 4096u, 1);
    if (ret < 0) return ret;
    boot[0] = 0xeb; boot[1] = 0x76; boot[2] = 0x90; memcpy(boot + 3, "EXFAT   ", 8);
    block_put64(boot + 64, 0); block_put64(boot + 72, sectors); block_put32(boot + 80, fat_offset); block_put32(boot + 84, fat_length);
    block_put32(boot + 88, heap_offset); block_put32(boot + 92, cluster_count); block_put32(boot + 96, root_cluster); block_put32(boot + 100, 0x4c344658u);
    block_put16(boot + 104, 0x0100); boot[108] = 9; boot[109] = spc_shift; boot[110] = 1; boot[111] = 0x80; boot[510] = 0x55; boot[511] = 0xaa;
    ret = block_io(fd, 0, boot, 512, 1);
    if (ret < 0) return ret;
    ret = block_io(fd, 12u * 512u, boot, 512, 1);
    if (ret < 0) return ret;
    memset(scratch, 0, sizeof(scratch));
    for (uint32_t sector = 1; sector < 11; ++sector) {
        if (sector <= 8) { scratch[510] = 0x55; scratch[511] = 0xaa; }
        ret = block_io(fd, (uint64_t)sector * 512u, scratch, 512, 1);
        if (ret < 0) return ret;
        ret = block_io(fd, (uint64_t)(12u + sector) * 512u, scratch, 512, 1);
        if (ret < 0) return ret;
        scratch[510] = scratch[511] = 0;
    }
    for (uint32_t offset = 0; offset < 512; offset += 4) block_put32(scratch + offset, block_exfat_boot_checksum(boot));
    ret = block_io(fd, 11u * 512u, scratch, 512, 1);
    if (ret < 0) return ret;
    return block_io(fd, 23u * 512u, scratch, 512, 1);
}

int reliefos_block_format(const char *partition_path, uint32_t filesystem, const char *label)
{
    int fd;
    uint64_t sectors;
    uint32_t sector_size;
    int ret = block_open_info(partition_path, 1, &fd, &sectors, &sector_size);
    if (ret < 0) return ret;
    if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32) ret = block_format_fat32(fd, sectors, label);
    else if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2) ret = block_format_ext2(fd, sectors, label);
    else if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4) ret = block_format_ext4(fd, sectors, label);
    else if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) ret = block_format_exfat(fd, sectors, label);
    else ret = -BLOCK_EINVAL;
    (void)close(fd);
    return ret;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_block_filesystem_name) leonos_block_filesystem_name __attribute__((alias("reliefos_block_filesystem_name")));
extern __typeof__(reliefos_block_format) leonos_block_format __attribute__((alias("reliefos_block_format")));
extern __typeof__(reliefos_block_get_info) leonos_block_get_info __attribute__((alias("reliefos_block_get_info")));
extern __typeof__(reliefos_block_gpt_create) leonos_block_gpt_create __attribute__((alias("reliefos_block_gpt_create")));
extern __typeof__(reliefos_block_gpt_delete) leonos_block_gpt_delete __attribute__((alias("reliefos_block_gpt_delete")));
extern __typeof__(reliefos_block_gpt_initialize) leonos_block_gpt_initialize __attribute__((alias("reliefos_block_gpt_initialize")));
extern __typeof__(reliefos_block_gpt_set_name) leonos_block_gpt_set_name __attribute__((alias("reliefos_block_gpt_set_name")));
extern __typeof__(reliefos_block_gpt_resize) leonos_block_gpt_resize __attribute__((alias("reliefos_block_gpt_resize")));
extern __typeof__(reliefos_block_gpt_set_type) leonos_block_gpt_set_type __attribute__((alias("reliefos_block_gpt_set_type")));
extern __typeof__(reliefos_block_gpt_type_name) leonos_block_gpt_type_name __attribute__((alias("reliefos_block_gpt_type_name")));
extern __typeof__(reliefos_block_list_disks) leonos_block_list_disks __attribute__((alias("reliefos_block_list_disks")));
extern __typeof__(reliefos_block_list_partitions) leonos_block_list_partitions __attribute__((alias("reliefos_block_list_partitions")));
extern __typeof__(reliefos_block_partition_path) leonos_block_partition_path __attribute__((alias("reliefos_block_partition_path")));
extern __typeof__(reliefos_block_partition_uuid) leonos_block_partition_uuid __attribute__((alias("reliefos_block_partition_uuid")));
extern __typeof__(reliefos_block_probe_filesystem) leonos_block_probe_filesystem __attribute__((alias("reliefos_block_probe_filesystem")));
