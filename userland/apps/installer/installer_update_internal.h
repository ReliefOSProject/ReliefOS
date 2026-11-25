#ifndef RELIEFOS_INSTALLER_UPDATE_INTERNAL_H
#define RELIEFOS_INSTALLER_UPDATE_INTERNAL_H
#include "installer_update.h"

struct installer_update_target {
    struct installer_update_info info;
    char workspace[256], root[256], boot[256], esp[256];
    const char *filesystem;
    int lock_fd, root_mounted, boot_mounted, esp_mounted, namespaced;
};

/* The coordinator and all active descendants share the advisory file lock.
 * The target lives in shared memory so the coordinator can recover mounts after
 * a stage exits before publishing its mount flags. */
int installer_update_target_lock(struct installer_update_target *target);
int installer_update_target_open(struct installer_update_target *target,
                                 const char *disk, const char *expected_root, const char *expected_esp);
int installer_update_target_writable(struct installer_update_target *target);
int installer_update_target_unmount(struct installer_update_target *target);
int installer_update_target_unlock(struct installer_update_target *target);
int installer_update_target_close(struct installer_update_target *target);

#endif
