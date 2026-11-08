#ifndef RELIEFOS_INSTALLER_UPDATE_INTERNAL_H
#define RELIEFOS_INSTALLER_UPDATE_INTERNAL_H
#include "installer_update.h"

struct installer_update_target {
    struct installer_update_info info;
    char workspace[256], root[256], boot[256];
    const char *filesystem;
    int lock_fd, root_mounted, boot_mounted;
};

/* Only the worker owns these mounts and the advisory installer lock. */
int installer_update_target_open(struct installer_update_target *target,
                                 const char *disk, const char *expected_root);
int installer_update_target_writable(struct installer_update_target *target);
int installer_update_target_close(struct installer_update_target *target);

#endif
