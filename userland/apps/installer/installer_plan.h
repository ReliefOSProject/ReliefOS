#ifndef RELIEFOS_INSTALLER_PLAN_H
#define RELIEFOS_INSTALLER_PLAN_H
#include <reliefos/blockdev.h>
#include <stddef.h>

#define INSTALLER_PLAN_MAX_OPERATIONS 256u
enum installer_plan_operation_kind {
    INSTALLER_PLAN_INITIALIZE, INSTALLER_PLAN_CREATE, INSTALLER_PLAN_RESIZE,
    INSTALLER_PLAN_RENAME, INSTALLER_PLAN_TYPE, INSTALLER_PLAN_DELETE,
    INSTALLER_PLAN_FORMAT,
};
struct installer_plan_operation {
    unsigned kind, index, value, size_mib;
    char name[RELIEFOS_BLOCK_NAME_LEN];
};
/* The raw metadata snapshot includes both GPT copies and per-partition
 * superblock prefixes. It is compared again before the first physical write. */
struct installer_disk_plan {
    struct reliefos_block_disk_info disk;
    struct reliefos_block_partition base[RELIEFOS_BLOCK_MAX_PARTITIONS];
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    unsigned base_count, count, operation_count;
    struct installer_plan_operation operations[INSTALLER_PLAN_MAX_OPERATIONS];
    unsigned char *snapshot;
    size_t snapshot_size;
    uint64_t first_usable, end_usable;
    unsigned slots;
    int loaded, table_error, invalid;
};
void installer_plan_dispose(struct installer_disk_plan *plan);
int installer_plan_load(struct installer_disk_plan *plan,
                         const struct reliefos_block_disk_info *disk);
int installer_plan_check(struct installer_disk_plan *plan);
int installer_plan_initialize(struct installer_disk_plan *plan);
int installer_plan_create(struct installer_disk_plan *plan, unsigned filesystem,
                           unsigned size_mib, const char *name, unsigned *index);
int installer_plan_resize(struct installer_disk_plan *plan, unsigned index, unsigned size_mib);
int installer_plan_rename(struct installer_disk_plan *plan, unsigned index, const char *name);
int installer_plan_type(struct installer_disk_plan *plan, unsigned index, unsigned type);
int installer_plan_delete(struct installer_disk_plan *plan, unsigned index);
int installer_plan_format(struct installer_disk_plan *plan, unsigned index,
                           unsigned filesystem, const char *label);
int installer_plan_apply(struct installer_disk_plan *plan, int root, int esp);
int installer_plan_requires_format(const struct installer_disk_plan *plan, unsigned index);
int installer_plan_describe(const struct installer_disk_plan *plan, char *out, size_t capacity);
#endif
