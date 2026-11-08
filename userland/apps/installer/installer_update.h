#ifndef RELIEFOS_INSTALLER_UPDATE_H
#define RELIEFOS_INSTALLER_UPDATE_H

#include <stddef.h>
#include <stdint.h>

/* Immutable inspection results travel from the worker to either frontend.
 * Applying an update requires the exact inspected disk, partition identities,
 * signed index and installed database, so a stale confirmation cannot retarget. */
struct installer_update_info {
    char disk[64], root_device[64], esp_device[64];
    char root_uuid[37], esp_uuid[37];
    char installed_version[64], media_version[64], profile[16];
    char installed_kernel[96], media_kernel[96];
    char index_sha256[65], database_sha256[65], world_sha256[65], pending[32];
    char failure[128];
    uint64_t free_bytes, required_bytes;
    uint32_t boot_modes;
    int error_number;
};

struct installer_update_packages {
    uint64_t installed_bytes, largest_package;
    char kernel_version[96];
};

struct installer_update_checkpoint {
    char phase[32], root_uuid[37], index_sha256[65], database_sha256[65], world_sha256[65];
};

#ifdef __cplusplus
extern "C" {
#endif

/* Read data files without sourcing shell text or following hierarchy links.
 * All functions return zero or a negative errno. */
int installer_update_read_system(const char *root, struct installer_update_info *info);
int installer_update_read_media(const char *root, const char *profile, struct installer_update_info *info);
int installer_update_read_packages(const char *root, struct installer_update_packages *packages);
int installer_update_read_checkpoint(const char *root, struct installer_update_checkpoint *checkpoint);
int installer_update_write_checkpoint(const char *root, const struct installer_update_checkpoint *checkpoint);

typedef void (*installer_update_observer)(void *context, unsigned progress, const char *message);
int installer_update_check(const char *disk, struct installer_update_info *info,
                           installer_update_observer observer, void *context);
int installer_update_apply(const struct installer_update_info *expected, unsigned boot_modes,
                           struct installer_update_info *result,
                           installer_update_observer observer, void *context);

/* Decode a complete bounded worker line. Returns 1 for metadata, 2 for
 * progress, 0 for an unrelated line, or a negative errno for invalid data. */
int installer_update_decode(struct installer_update_info *info, unsigned *progress,
                            char *message, size_t capacity, const char *line);

/* Run the isolated native backend before any FLTK initialization. */
int installer_update_worker(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
