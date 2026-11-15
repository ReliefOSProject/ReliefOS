#ifndef RELIEFOS_INSTALLER_TUI_PARTITIONS_H
#define RELIEFOS_INSTALLER_TUI_PARTITIONS_H

#include <stddef.h>
#include "installer_tty.h"

#define TUI_PARTITION_ROWS (RELIEFOS_BLOCK_MAX_PARTITIONS * 2u + 1u)

/** @brief One physical extent; source is the original array index, or -1 for free space. */
struct tui_partition_row {
    uint64_t first_lba, sector_count;
    int source;
    unsigned filesystem, type;
    char path[RELIEFOS_BLOCK_PATH_LEN], name[RELIEFOS_BLOCK_NAME_LEN];
    const char *role;
    char symbol;
};

/** @brief Read-only disk snapshot and persistent keyboard viewport; no disk I/O. */
struct tui_partition_table {
    struct tui_partition_row rows[TUI_PARTITION_ROWS];
    int count, cursor, first, visible, preview;
    int *selection;
    uint32_t sector_size;
    uint64_t sector_count;
    char disk[192];
};

/** @brief Rebuild the sorted physical view or the shared pending automatic plan.
 * @param table Caller-owned snapshot and viewport, initially zeroed.
 * @param context Borrowed installer disk, role and partition state.
 * @param automatic Nonzero displays the pending plan without selecting a real partition.
 * @param selection Original partition-array index, updated only in manual mode.
 * @return Zero on success, or a negative errno for invalid geometry/plan.
 */
int tui_partition_table_update(struct tui_partition_table *table,
    const struct installer_tty_context *context, int automatic, int *selection);

/** @brief Move within physical rows, preserving the original partition mapping.
 * @param table Mutable snapshot and viewport.
 * @param movement Signed row displacement, clamped to the available rows.
 */
void tui_partition_table_move(struct tui_partition_table *table, int movement);

/** @brief Format a binary capacity without overflowing sector-to-byte conversion.
 * @param out Caller-owned output buffer.
 * @param capacity Output byte capacity, including the terminator.
 * @param sectors Count of logical sectors.
 * @param sector_size Logical-sector bytes.
 */
void tui_partition_size(char *out, size_t capacity, uint64_t sectors, uint32_t sector_size);

#endif
