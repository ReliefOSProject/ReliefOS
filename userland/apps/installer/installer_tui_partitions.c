#include "installer_tui_partitions.h"
#include "installer_partitions.h"
#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define T(s) gettext(s)

/** @brief Append a bounded extent to the display snapshot, without performing I/O.
 * @param table Mutable caller-owned snapshot.
 * @param part Borrowed real or planned partition, or NULL for unallocated space.
 * @param source Original partition-array index, or -1 for a non-operable row.
 * @param first First logical sector of a free extent.
 * @param count Number of sectors in a free extent.
 * @return Appended row, or NULL when the snapshot is full.
 */
static struct tui_partition_row *append_row(struct tui_partition_table *table,
    const struct reliefos_block_partition *part, int source, uint64_t first, uint64_t count)
{
    if (table->count >= (int)TUI_PARTITION_ROWS) return NULL;
    struct tui_partition_row *row = &table->rows[table->count++];
    *row = (struct tui_partition_row){.first_lba = first, .sector_count = count,
        .source = source, .role = T("Unallocated"), .symbol = '.'};
    if (!part) return row;
    row->first_lba = part->first_lba;
    row->sector_count = part->sector_count;
    row->filesystem = part->filesystem;
    row->type = part->gpt_type;
    snprintf(row->path, sizeof(row->path), "%s", part->path);
    snprintf(row->name, sizeof(row->name), "%s", part->name[0] ? part->name : T("Unnamed"));
    if (part->gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT) {
        row->role = "BIOS Boot"; row->symbol = 'B';
    } else if (part->gpt_type == RELIEFOS_BLOCK_GPT_ESP) {
        row->role = "ESP"; row->symbol = 'E';
    } else {
        row->role = T("Data partition"); row->symbol = 'D';
    }
    return row;
}

/** @brief Rebuild a sector-sorted display while keeping device selection independent of row order.
 * @param table Caller-owned snapshot, initially zeroed.
 * @param context Borrowed current installer state.
 * @param automatic Nonzero previews the shared automatic layout without disk writes.
 * @param selection Original array index updated only for actual manual selections.
 * @return Zero on success, or a negative errno without publishing an invalid row.
 */
int tui_partition_table_update(struct tui_partition_table *table,
    const struct installer_tty_context *context, int automatic, int *selection)
{
    int previous_source = table->count && table->cursor >= 0 && table->cursor < table->count
        ? table->rows[table->cursor].source : -2;
    uint64_t previous_first = previous_source != -2 ? table->rows[table->cursor].first_lba : 0;
    int same_mode = table->sector_count && table->preview == !!automatic;
    int requested_selection = selection ? *selection : -1;
    if (!automatic && selection) *selection = -1;
    table->count = 0;
    table->preview = !!automatic;
    table->selection = selection;
    if (!context || !selection || !context->selected_disk || !context->disk_count ||
        *context->selected_disk < 0 || (uint32_t)*context->selected_disk >= *context->disk_count)
        return -EINVAL;
    const struct reliefos_block_disk_info *disk = &context->disks[*context->selected_disk];
    table->sector_count = disk->sector_count;
    table->sector_size = disk->sector_size;
    char capacity[32];
    tui_partition_size(capacity, sizeof(capacity), disk->sector_count, disk->sector_size);
    snprintf(table->disk, sizeof(table->disk), "%s  %s  %s", disk->path, disk->name, capacity);
    if (!disk->sector_size || disk->sector_count < 68 || disk->sector_count > UINT64_MAX / disk->sector_size)
        return -EINVAL;
    if (automatic) {
        struct installer_partition_plan plan;
        unsigned modes = (context->setup->skip_bios ? 0 : 1u) | (context->setup->skip_uefi ? 0 : 2u);
        int result = installer_partition_plan_default(disk, modes, &plan);
        if (result) return result;
        const struct installer_partition_extent *extents[] = {&plan.bios, &plan.esp, &plan.root};
        const unsigned types[] = {RELIEFOS_BLOCK_GPT_BIOS_BOOT, RELIEFOS_BLOCK_GPT_ESP, RELIEFOS_BLOCK_GPT_LINUX};
        const unsigned filesystems[] = {0, RELIEFOS_BLOCK_FILESYSTEM_FAT32, RELIEFOS_BLOCK_FILESYSTEM_EXT4};
        const char *names[] = {"RELIEFOS_BIOS", "RELIEFOS_ESP", "RELIEFOS_ROOT"};
        for (int i = 0; i < 3; ++i) {
            if (!extents[i]->sector_count) continue;
            struct reliefos_block_partition part = {.index = extents[i]->index,
                .first_lba = extents[i]->first_lba, .sector_count = extents[i]->sector_count,
                .gpt_type = types[i], .filesystem = filesystems[i]};
            result = reliefos_block_partition_path(disk->path, part.index, part.path, sizeof(part.path));
            if (result) { table->count = 0; return result; }
            snprintf(part.name, sizeof(part.name), "%s", names[i]);
            struct tui_partition_row *row = append_row(table, &part, -1, 0, 0);
            if (i == 2) { row->role = "/"; row->symbol = 'R'; }
        }
    } else {
        unsigned sorted[RELIEFOS_BLOCK_MAX_PARTITIONS];
        unsigned count = *context->partition_count;
        if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) return -EOVERFLOW;
        for (unsigned i = 0; i < count; ++i) {
            unsigned position = i;
            while (position && context->partitions[sorted[position - 1]].first_lba > context->partitions[i].first_lba) {
                sorted[position] = sorted[position - 1];
                --position;
            }
            sorted[position] = i;
        }
        uint64_t first = 34, end = disk->sector_count - 33;
        uint64_t minimum_gap = (1024ULL * 1024ULL + disk->sector_size - 1u) / disk->sector_size;
        for (unsigned i = 0; i < count; ++i) {
            const struct reliefos_block_partition *part = &context->partitions[sorted[i]];
            if (!part->sector_count || part->first_lba < first || part->first_lba >= end ||
                part->sector_count > end - part->first_lba) {
                table->count = 0; return -EINVAL;
            }
            if (part->first_lba - first >= minimum_gap)
                append_row(table, NULL, -1, first, part->first_lba - first);
            struct tui_partition_row *row = append_row(table, part, (int)sorted[i], 0, 0);
            if ((int32_t)part->index == *context->root_partition) { row->role = "/"; row->symbol = 'R'; }
            else if ((int32_t)part->index == *context->esp_partition) row->role = "/efi";
            first = part->first_lba + part->sector_count;
        }
        if (end - first >= minimum_gap || !count) append_row(table, NULL, -1, first, end - first);
    }
    int wanted = same_mode ? table->cursor : 0;
    for (int i = 0; !automatic && i < table->count; ++i) {
        if ((requested_selection >= 0 && table->rows[i].source == requested_selection) ||
            (requested_selection < 0 && same_mode && previous_source == -1 &&
             table->rows[i].source == -1 && table->rows[i].first_lba == previous_first)) {
            wanted = i;
            break;
        }
    }
    table->cursor = wanted;
    tui_partition_table_move(table, 0);
    return 0;
}

/** @brief Clamp row movement and publish only the actual selected array index.
 * @param table Mutable snapshot and viewport.
 * @param movement Signed displacement; callers may pass +/-TUI_PARTITION_ROWS for Home/End.
 */
void tui_partition_table_move(struct tui_partition_table *table, int movement)
{
    int cursor = table->cursor + movement;
    if (cursor >= table->count) cursor = table->count - 1;
    if (cursor < 0) cursor = 0;
    table->cursor = cursor;
    if (!table->preview && table->selection)
        *table->selection = table->count ? table->rows[cursor].source : -1;
}

/** @brief Produce a bounded capacity label using checked integer arithmetic.
 * @param out Caller-owned output buffer.
 * @param capacity Output byte capacity, including the terminator.
 * @param sectors Logical-sector count.
 * @param sector_size Logical-sector size in bytes.
 */
void tui_partition_size(char *out, size_t capacity, uint64_t sectors, uint32_t sector_size)
{
    if (!sector_size || sectors > UINT64_MAX / sector_size) {
        snprintf(out, capacity, "?"); return;
    }
    uint64_t bytes = sectors * sector_size;
    const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    unsigned unit = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && unit < 6) { divisor *= 1024; ++unit; }
    if (unit < 2 || !(bytes % divisor))
        snprintf(out, capacity, "%llu %s", (unsigned long long)(bytes / divisor), units[unit]);
    else {
        uint64_t remainder = bytes % divisor, quarter = divisor / 4;
        unsigned hundredths = (unsigned)((remainder / quarter) * 25 +
            ((remainder % quarter) * 25) / quarter);
        snprintf(out, capacity, "%llu.%02u %s", (unsigned long long)(bytes / divisor), hundredths, units[unit]);
    }
}
