#ifndef RELIEFOS_FILEMAN_SELECTION_H
#define RELIEFOS_FILEMAN_SELECTION_H

#include <reliefos/fs_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fileman_selection {
    char names[RELIEFOS_FS_MAX_ENTRIES][RELIEFOS_FS_NAME_LEN];
    uint32_t count;
    int32_t selected;
    uint64_t marked;
};

void fileman_selection_capture(struct fileman_selection *snapshot,
                               const struct reliefos_dir_entry *entries, uint32_t count,
                               int32_t selected, uint64_t marked);
void fileman_selection_restore(const struct fileman_selection *snapshot,
                               const struct reliefos_dir_entry *entries, uint32_t count,
                               int32_t *selected, uint64_t *marked);

#ifdef __cplusplus
}
#endif
#endif
