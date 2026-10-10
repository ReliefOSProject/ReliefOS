#ifndef RELIEFOS_BLOCKDEV_H
#define RELIEFOS_BLOCKDEV_H

#include <stdint.h>

#define RELIEFOS_BLOCK_MAX_DISKS 8U
#define RELIEFOS_BLOCK_MAX_PARTITIONS 128U
#define RELIEFOS_BLOCK_PATH_LEN 64U
#define RELIEFOS_BLOCK_NAME_LEN 72U

enum reliefos_block_filesystem {
    RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN = 0,
    RELIEFOS_BLOCK_FILESYSTEM_FAT32 = 1,
    RELIEFOS_BLOCK_FILESYSTEM_EXT2 = 2,
    RELIEFOS_BLOCK_FILESYSTEM_ISO9660 = 3,
    RELIEFOS_BLOCK_FILESYSTEM_EXFAT = 4,
    RELIEFOS_BLOCK_FILESYSTEM_EXT4 = 5,
};

enum reliefos_block_gpt_type {
    RELIEFOS_BLOCK_GPT_BASIC_DATA = 1,
    RELIEFOS_BLOCK_GPT_ESP = 2,
    RELIEFOS_BLOCK_GPT_LINUX = 3,
};

struct reliefos_block_disk_info {
    uint32_t id;
    uint32_t sector_size;
    uint64_t sector_count;
    char path[RELIEFOS_BLOCK_PATH_LEN];
    char name[32];
};

struct reliefos_block_partition {
    uint32_t index;
    uint32_t filesystem;
    uint32_t gpt_type;
    uint32_t flags;
    uint64_t first_lba;
    uint64_t sector_count;
    char path[RELIEFOS_BLOCK_PATH_LEN];
    char name[RELIEFOS_BLOCK_NAME_LEN];
};

int reliefos_block_list_disks(struct reliefos_block_disk_info *disks, uint32_t capacity,
                            uint32_t *out_count);
int reliefos_block_get_info(const char *path, struct reliefos_block_disk_info *out);
/* Read the validated GPT unique partition GUID into a 37-byte buffer. */
int reliefos_block_partition_uuid(const char *disk_path, uint32_t index, char uuid[37]);
int reliefos_block_list_partitions(const char *disk_path,
                                 struct reliefos_block_partition *partitions,
                                 uint32_t capacity, uint32_t *out_count);
int reliefos_block_gpt_initialize(const char *disk_path, int force);
int reliefos_block_gpt_create(const char *disk_path, uint32_t filesystem,
                            uint32_t size_mib, const char *name,
                            uint32_t *out_index);
int reliefos_block_gpt_delete(const char *disk_path, uint32_t index);
int reliefos_block_gpt_set_type(const char *disk_path, uint32_t index, uint32_t type);
int reliefos_block_gpt_set_name(const char *disk_path, uint32_t index, const char *name);
int reliefos_block_gpt_resize(const char *disk_path, uint32_t index, uint32_t size_mib);
int reliefos_block_format(const char *partition_path, uint32_t filesystem,
                        const char *label);
int reliefos_block_probe_filesystem(const char *partition_path, uint32_t *out_filesystem);
int reliefos_block_partition_path(const char *disk_path, uint32_t index,
                                char *out, uint32_t capacity);
const char *reliefos_block_filesystem_name(uint32_t filesystem);
const char *reliefos_block_gpt_type_name(uint32_t type);

#endif
