#include "selection.h"

#include <string.h>

void fileman_selection_capture(struct fileman_selection *snapshot,
                               const struct reliefos_dir_entry *entries, uint32_t count,
                               int32_t selected, uint64_t marked)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (count > RELIEFOS_FS_MAX_ENTRIES) count = RELIEFOS_FS_MAX_ENTRIES;
    snapshot->count = count;
    snapshot->selected = selected;
    snapshot->marked = marked;
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(snapshot->names[i], entries[i].name, RELIEFOS_FS_NAME_LEN);
        snapshot->names[i][RELIEFOS_FS_NAME_LEN - 1U] = 0;
    }
}

void fileman_selection_restore(const struct fileman_selection *snapshot,
                               const struct reliefos_dir_entry *entries, uint32_t count,
                               int32_t *selected, uint64_t *marked)
{
    *selected = -1;
    *marked = 0;
    if (count > RELIEFOS_FS_MAX_ENTRIES) count = RELIEFOS_FS_MAX_ENTRIES;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < snapshot->count; ++j) {
            if (strcmp(entries[i].name, snapshot->names[j])) continue;
            if ((int32_t)j == snapshot->selected) *selected = (int32_t)i;
            if (snapshot->marked & (1ULL << j)) *marked |= 1ULL << i;
            break;
        }
    }
}
