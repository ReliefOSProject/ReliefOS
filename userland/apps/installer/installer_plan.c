#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_plan.h"
#include "installer_partitions.h"
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define T(s) gettext(s)
#define PREFIX_BYTES 2048u
#define GPT_HEAD_BYTES (34u * 512u)
#define GPT_TAIL_BYTES (33u * 512u)
#define GPT_TABLE_BYTES (128u * 128u)

static uint64_t little64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

static int read_exact(int fd, void *buffer, size_t size, uint64_t offset)
{
    unsigned char *bytes = buffer;
    while (size) {
        ssize_t count = pread(fd, bytes, size, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -errno : -EIO;
        bytes += count; size -= (size_t)count; offset += (uint64_t)count;
    }
    return 0;
}

/* Read exactly the same metadata extents for loading and later validation.
 * A separate table read also covers noncanonical, valid GPT table locations. */
static int snapshot_read(const struct installer_disk_plan *plan, unsigned char *out)
{
    int fd = open(plan->disk.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    uint64_t bytes = plan->disk.sector_count * 512u;
    int result = read_exact(fd, out, GPT_HEAD_BYTES, 0);
    if (!result) result = read_exact(fd, out + GPT_HEAD_BYTES, GPT_TAIL_BYTES,
                                     bytes - GPT_TAIL_BYTES);
    size_t position = GPT_HEAD_BYTES + GPT_TAIL_BYTES;
    uint64_t table_lba = plan->snapshot ? little64(plan->snapshot + 512u + 72u)
                                       : little64(out + 512u + 72u);
    if (!result && table_lba >= 2 && table_lba <= plan->disk.sector_count - 32u)
        result = read_exact(fd, out + position, GPT_TABLE_BYTES, table_lba * 512u);
    else memset(out + position, 0, GPT_TABLE_BYTES);
    position += GPT_TABLE_BYTES;
    for (unsigned i = 0; !result && i < plan->base_count; ++i) {
        const struct reliefos_block_partition *part = &plan->base[i];
        if (!part->sector_count || part->first_lba >= plan->disk.sector_count ||
            part->sector_count > plan->disk.sector_count - part->first_lba) {
            result = -EINVAL; break;
        }
        size_t size = part->sector_count >= 4u ? PREFIX_BYTES : (size_t)part->sector_count * 512u;
        result = read_exact(fd, out + position, size, part->first_lba * 512u);
        position += PREFIX_BYTES;
    }
    if (close(fd) < 0 && !result) result = -errno;
    return result;
}

void installer_plan_dispose(struct installer_disk_plan *plan)
{
    free(plan->snapshot);
    memset(plan, 0, sizeof(*plan));
}

int installer_plan_load(struct installer_disk_plan *plan,
                         const struct reliefos_block_disk_info *disk)
{
    if (!plan || !disk || disk->sector_size != 512u) return -ENOTSUP;
    if (disk->sector_count < 68u || disk->sector_count > INT64_MAX / 512u) return -EINVAL;
    installer_plan_dispose(plan);
    plan->disk = *disk;
    uint32_t count = 0;
    plan->table_error = reliefos_block_list_partitions(disk->path, plan->base,
        RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    /* A missing/structurally invalid GPT can be explicitly replaced. Read,
     * device and allocation failures never become a successful empty disk. */
    if (plan->table_error && plan->table_error != -ENOENT && plan->table_error != -EINVAL)
        return plan->invalid = plan->table_error;
    if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) return plan->invalid = -EOVERFLOW;
    if (!plan->table_error) plan->base_count = count;
    plan->snapshot_size = GPT_HEAD_BYTES + GPT_TAIL_BYTES + GPT_TABLE_BYTES +
        (size_t)plan->base_count * PREFIX_BYTES;
    unsigned char *snapshot = calloc(1, plan->snapshot_size);
    if (!snapshot) return plan->invalid = -ENOMEM;
    int result = snapshot_read(plan, snapshot);
    if (result) { free(snapshot); return plan->invalid = result; }
    plan->snapshot = snapshot;
    plan->loaded = 1;
    plan->count = plan->base_count;
    memcpy(plan->parts, plan->base, plan->count * sizeof(plan->parts[0]));
    plan->first_usable = 34u;
    plan->end_usable = disk->sector_count - 33u;
    plan->slots = RELIEFOS_BLOCK_MAX_PARTITIONS;
    if (!memcmp(snapshot + 512u, "EFI PART", 8)) {
        if (!plan->table_error) {
            plan->first_usable = little64(snapshot + 512u + 40u);
            plan->end_usable = little64(snapshot + 512u + 48u) + 1u;
            plan->slots = (unsigned)little64(snapshot + 512u + 80u);
            /* The entry-count field is a 32-bit value followed by entry size. */
            plan->slots &= 0xffffffffu;
        }
    } else if (!plan->table_error) plan->table_error = -ENOENT;
    return 0;
}

int installer_plan_check(struct installer_disk_plan *plan)
{
    if (!plan || !plan->loaded) return -ENODEV;
    if (plan->invalid) return plan->invalid;
    struct reliefos_block_disk_info current = {0};
    int result = reliefos_block_get_info(plan->disk.path, &current);
    if (result) return result;
    if (current.sector_size != plan->disk.sector_size ||
        current.sector_count != plan->disk.sector_count ||
        strcmp(current.name, plan->disk.name)) return plan->invalid = -ESTALE;
    unsigned char *snapshot = calloc(1, plan->snapshot_size);
    if (!snapshot) return -ENOMEM;
    result = snapshot_read(plan, snapshot);
    if (!result && memcmp(snapshot, plan->snapshot, plan->snapshot_size))
        result = plan->invalid = -ESTALE;
    free(snapshot);
    return result;
}

static struct reliefos_block_partition *find_part(struct installer_disk_plan *plan, unsigned index)
{
    for (unsigned i = 0; i < plan->count; ++i)
        if (plan->parts[i].index == index) return &plan->parts[i];
    return NULL;
}

static int editable(const struct installer_disk_plan *plan)
{
    if (!plan || !plan->loaded) return -ENODEV;
    if (plan->invalid) return plan->invalid;
    if (plan->operation_count == INSTALLER_PLAN_MAX_OPERATIONS) return -ENOSPC;
    return 0;
}

static int valid_name(const char *name)
{
    if (!name) return -EINVAL;
    size_t length = strnlen(name, RELIEFOS_BLOCK_NAME_LEN);
    if (length >= RELIEFOS_BLOCK_NAME_LEN) return -ENAMETOOLONG;
    /* Match GPT's 36 UTF-16-code-unit limit before staging any write. */
    unsigned units = 0;
    for (size_t offset = 0; offset < length;) {
        unsigned char first = (unsigned char)name[offset++];
        uint32_t code = first, minimum = 0;
        unsigned continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) { code &= 31u; continuation = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code &= 15u; continuation = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code &= 7u; continuation = 3; minimum = 0x10000; }
        else if (first >= 0x80) return -EILSEQ;
        while (continuation--) {
            if (offset >= length || ((unsigned char)name[offset] & 0xc0) != 0x80) return -EILSEQ;
            code = (code << 6) | ((unsigned char)name[offset++] & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return -EILSEQ;
        units += code >= 0x10000 ? 2u : 1u;
        if (units > 36u) return -ENAMETOOLONG;
    }
    return 0;
}

static int filesystem_valid(unsigned filesystem, int raw)
{
    return (raw && filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN) ||
        filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 || filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
        filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT3 || filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ||
        filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT;
}

static void append(struct installer_disk_plan *plan, unsigned kind, unsigned index,
                     unsigned value, unsigned size, const char *name)
{
    struct installer_plan_operation *operation = &plan->operations[plan->operation_count++];
    *operation = (struct installer_plan_operation){.kind = kind, .index = index,
        .value = value, .size_mib = size};
    if (name) snprintf(operation->name, sizeof(operation->name), "%s", name);
}

int installer_plan_initialize(struct installer_disk_plan *plan)
{
    int result = editable(plan);
    if (result) return result;
    /* Initializing supersedes all earlier edits; no disk operation occurs. */
    plan->operation_count = plan->count = 0;
    plan->table_error = 0;
    plan->first_usable = 34u; plan->end_usable = plan->disk.sector_count - 33u;
    plan->slots = RELIEFOS_BLOCK_MAX_PARTITIONS;
    append(plan, INSTALLER_PLAN_INITIALIZE, 0, 0, 0, NULL);
    return 0;
}

int installer_plan_create(struct installer_disk_plan *plan, unsigned filesystem,
                           unsigned size, const char *name, unsigned *index)
{
    int result = editable(plan);
    if (!result && plan->table_error) result = plan->table_error;
    if (!result) result = valid_name(name ? name : "ReliefOS Data");
    if (result) return result;
    if (!size || !filesystem_valid(filesystem, 1)) return -EINVAL;
    if (plan->count >= plan->slots || plan->count >= RELIEFOS_BLOCK_MAX_PARTITIONS) return -ENOSPC;
    unsigned slot = 0;
    while (slot < plan->slots && find_part(plan, slot)) ++slot;
    uint64_t cursor = plan->first_usable, required = (uint64_t)size * 2048u;
    for (;;) {
        cursor = (cursor + 2047u) & ~2047ULL;
        uint64_t next = plan->end_usable;
        struct reliefos_block_partition *following = NULL;
        for (unsigned i = 0; i < plan->count; ++i)
            if (plan->parts[i].first_lba >= cursor && plan->parts[i].first_lba < next) {
                following = &plan->parts[i]; next = following->first_lba;
            }
        if (cursor <= next && required <= next - cursor) break;
        if (!following) return -ENOSPC;
        cursor = following->first_lba + following->sector_count;
    }
    /* The current block API chooses the first aligned gap itself. Refuse
     * unusual layouts where its candidate would overlap a partition starting
     * before that alignment, rather than publishing an unsafe table. */
    for (unsigned i = 0; i < plan->count; ++i)
        if (cursor < plan->parts[i].first_lba + plan->parts[i].sector_count &&
            plan->parts[i].first_lba < cursor + required) return -ENOTSUP;
    struct reliefos_block_partition part = {.index = slot, .filesystem = filesystem,
        .first_lba = cursor, .sector_count = required,
        .gpt_type = (filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
            filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT3 || filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4)
            ? RELIEFOS_BLOCK_GPT_LINUX : RELIEFOS_BLOCK_GPT_BASIC_DATA};
    result = reliefos_block_partition_path(plan->disk.path, slot, part.path, sizeof(part.path));
    if (result) return result;
    snprintf(part.name, sizeof(part.name), "%s", name && *name ? name : "ReliefOS Data");
    plan->parts[plan->count++] = part;
    append(plan, INSTALLER_PLAN_CREATE, slot, filesystem, size, part.name);
    if (index) *index = slot;
    return 0;
}

int installer_plan_resize(struct installer_disk_plan *plan, unsigned index, unsigned size)
{
    int result = editable(plan);
    if (result) return result;
    struct reliefos_block_partition *part = find_part(plan, index);
    if (!part || !size) return -EINVAL;
    uint64_t sectors = (uint64_t)size * 2048u;
    if (sectors == part->sector_count) return 0;
    /* There is no filesystem resizer in the installer. Never expose trailing
     * partition shrink as filesystem shrink, even if a format was also queued. */
    for (unsigned i = 0; i < plan->base_count; ++i)
        if (plan->base[i].index == index && plan->base[i].first_lba == part->first_lba &&
            sectors < plan->base[i].sector_count) return -ENOTSUP;
    uint64_t end = plan->end_usable;
    for (unsigned i = 0; i < plan->count; ++i)
        if (plan->parts[i].first_lba > part->first_lba && plan->parts[i].first_lba < end)
            end = plan->parts[i].first_lba;
    if (part->first_lba > end || sectors > end - part->first_lba) return -ENOSPC;
    part->sector_count = sectors;
    append(plan, INSTALLER_PLAN_RESIZE, index, 0, size, NULL);
    return 0;
}

int installer_plan_rename(struct installer_disk_plan *plan, unsigned index, const char *name)
{
    int result = editable(plan);
    if (!result) result = valid_name(name);
    if (result) return result;
    struct reliefos_block_partition *part = find_part(plan, index);
    if (!part) return -ENOENT;
    snprintf(part->name, sizeof(part->name), "%s", name);
    append(plan, INSTALLER_PLAN_RENAME, index, 0, 0, name);
    return 0;
}

int installer_plan_type(struct installer_disk_plan *plan, unsigned index, unsigned type)
{
    int result = editable(plan);
    if (result) return result;
    struct reliefos_block_partition *part = find_part(plan, index);
    if (!part || type < RELIEFOS_BLOCK_GPT_BASIC_DATA || type > RELIEFOS_BLOCK_GPT_BIOS_BOOT) return -EINVAL;
    if (type == RELIEFOS_BLOCK_GPT_BIOS_BOOT && part->filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN) return -EINVAL;
    if (part->gpt_type == type) return 0;
    part->gpt_type = type;
    append(plan, INSTALLER_PLAN_TYPE, index, type, 0, NULL);
    return 0;
}

int installer_plan_delete(struct installer_disk_plan *plan, unsigned index)
{
    int result = editable(plan);
    if (result) return result;
    for (unsigned i = 0; i < plan->count; ++i) if (plan->parts[i].index == index) {
        memmove(&plan->parts[i], &plan->parts[i + 1], (plan->count - i - 1) * sizeof(plan->parts[0]));
        --plan->count;
        append(plan, INSTALLER_PLAN_DELETE, index, 0, 0, NULL);
        return 0;
    }
    return -ENOENT;
}

int installer_plan_format(struct installer_disk_plan *plan, unsigned index,
                           unsigned filesystem, const char *label)
{
    int result = editable(plan);
    if (result) return result;
    struct reliefos_block_partition *part = find_part(plan, index);
    if (!part || !filesystem_valid(filesystem, 0)) return -EINVAL;
    if (part->gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT) return -EPERM;
    size_t label_capacity = filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ? 12u : 17u;
    if (label && strnlen(label, label_capacity) >= label_capacity) return -ENAMETOOLONG;
    part->filesystem = filesystem;
    append(plan, INSTALLER_PLAN_FORMAT, index, filesystem, 0, label);
    return 0;
}

/* Apply table edits first. Formatting is coalesced against the final layout so
 * a subsequently deleted partition is never formatted, and root/ESP keep their
 * existing ordered installer formatting paths. */
int installer_plan_requires_format(const struct installer_disk_plan *plan, unsigned index)
{
    int required = 0;
    for (unsigned i = 0; i < plan->operation_count; ++i) {
        const struct installer_plan_operation *op = &plan->operations[i];
        if (op->index != index) continue;
        if (op->kind == INSTALLER_PLAN_DELETE) required = 0;
        if (op->kind == INSTALLER_PLAN_CREATE || op->kind == INSTALLER_PLAN_FORMAT)
            required = op->value != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    }
    return required;
}

/* The summary and writer must agree about which surviving BIOS extents are
 * cleared. Earlier roles on deleted partitions or final data roles do not
 * describe the final write, even if the operation log mentions BIOS Boot. */
static int bios_requires_clear(const struct installer_disk_plan *plan,
                                const struct reliefos_block_partition *part)
{
    if (part->gpt_type != RELIEFOS_BLOCK_GPT_BIOS_BOOT) return 0;
    for (unsigned i = 0; i < plan->operation_count; ++i)
        if (plan->operations[i].index == part->index &&
            (plan->operations[i].kind == INSTALLER_PLAN_CREATE ||
             plan->operations[i].kind == INSTALLER_PLAN_TYPE)) return 1;
    return 0;
}

int installer_plan_apply(struct installer_disk_plan *plan, int root, int esp)
{
    int result = installer_plan_check(plan);
    if (result) return result;
    if (plan->table_error) return plan->table_error;
    for (unsigned i = 0; !result && i < plan->operation_count; ++i) {
        const struct installer_plan_operation *op = &plan->operations[i];
        unsigned index = 0;
        switch (op->kind) {
        case INSTALLER_PLAN_INITIALIZE: result = reliefos_block_gpt_initialize(plan->disk.path, 1); break;
        case INSTALLER_PLAN_CREATE:
            result = reliefos_block_gpt_create(plan->disk.path, op->value, op->size_mib, op->name, &index);
            if (!result && index != op->index) result = -ESTALE;
            break;
        case INSTALLER_PLAN_RESIZE: result = reliefos_block_gpt_resize(plan->disk.path, op->index, op->size_mib); break;
        case INSTALLER_PLAN_RENAME: result = reliefos_block_gpt_set_name(plan->disk.path, op->index, op->name); break;
        case INSTALLER_PLAN_TYPE: result = reliefos_block_gpt_set_type(plan->disk.path, op->index, op->value); break;
        case INSTALLER_PLAN_DELETE: result = reliefos_block_gpt_delete(plan->disk.path, op->index); break;
        case INSTALLER_PLAN_FORMAT: break;
        default: result = -EINVAL; break;
        }
    }
    for (unsigned p = 0; !result && p < plan->count; ++p) {
        const struct reliefos_block_partition *part = &plan->parts[p];
        if (part->gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT) {
            if (bios_requires_clear(plan, part)) {
                struct installer_partition_extent bios = {part->index, part->first_lba, part->sector_count};
                result = installer_partition_clear_bios(plan->disk.path, &bios);
            }
            continue;
        }
        if ((int)part->index == root || (int)part->index == esp) continue;
        const struct installer_plan_operation *format = NULL;
        for (unsigned i = 0; i < plan->operation_count; ++i) {
            const struct installer_plan_operation *op = &plan->operations[i];
            if (op->index != part->index) continue;
            if (op->kind == INSTALLER_PLAN_DELETE) format = NULL;
            if (op->kind == INSTALLER_PLAN_FORMAT || op->kind == INSTALLER_PLAN_CREATE) format = op;
        }
        if (format && format->value != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN)
            result = reliefos_block_format(part->path, format->value,
                format->kind == INSTALLER_PLAN_FORMAT ? format->name : NULL);
    }
    /* A partly applied transaction must be inspected again, never replayed. */
    if (plan->operation_count) plan->invalid = -ESTALE;
    return result;
}

int installer_plan_describe(const struct installer_disk_plan *plan, char *out, size_t capacity)
{
    if (!out || !capacity) return -EINVAL;
    out[0] = 0;
    size_t used = 0;
    for (unsigned i = 0; plan && i < plan->operation_count; ++i) {
        const struct installer_plan_operation *op = &plan->operations[i];
        char line[256];
        const char *verb = op->kind == INSTALLER_PLAN_INITIALIZE ? T("Replace partition table") :
            op->kind == INSTALLER_PLAN_CREATE ? T("Create partition") :
            op->kind == INSTALLER_PLAN_RESIZE ? T("Resize partition") :
            op->kind == INSTALLER_PLAN_RENAME ? T("Rename partition") :
            op->kind == INSTALLER_PLAN_TYPE ? T("Change partition type") :
            op->kind == INSTALLER_PLAN_DELETE ? T("Delete partition") : T("Format partition");
        int length;
        if (op->kind == INSTALLER_PLAN_INITIALIZE)
            length = snprintf(line, sizeof(line), "%s\n", verb);
        else if (op->kind == INSTALLER_PLAN_CREATE)
            length = snprintf(line, sizeof(line), "%s %u: %s, %u MiB, %s\n", verb,
                op->index + 1u, op->name, op->size_mib,
                op->value ? reliefos_block_filesystem_name(op->value) : T("unformatted"));
        else if (op->kind == INSTALLER_PLAN_RESIZE)
            length = snprintf(line, sizeof(line), "%s %u: %u MiB\n", verb, op->index + 1u, op->size_mib);
        else if (op->kind == INSTALLER_PLAN_FORMAT)
            length = snprintf(line, sizeof(line), "%s %u: %s%s%s\n", verb, op->index + 1u,
                reliefos_block_filesystem_name(op->value), op->name[0] ? ", " : "", op->name);
        else if (op->kind == INSTALLER_PLAN_TYPE)
            length = snprintf(line, sizeof(line), "%s %u: %s\n", verb, op->index + 1u,
                reliefos_block_gpt_type_name(op->value));
        else length = snprintf(line, sizeof(line), "%s %u%s%s\n", verb, op->index + 1u,
                op->name[0] ? ": " : "", op->name);
        if (length < 0 || (size_t)length >= sizeof(line) ||
            (size_t)length >= capacity - used) return -ENOSPC;
        memcpy(out + used, line, (size_t)length + 1u); used += (size_t)length;
    }
    for (unsigned i = 0; plan && i < plan->count; ++i) {
        const struct reliefos_block_partition *part = &plan->parts[i];
        if (!bios_requires_clear(plan, part)) continue;
        int length = snprintf(out + used, capacity - used,
            T("Clear all contents of BIOS Boot partition %u (%llu bytes).\n"),
            part->index + 1u, (unsigned long long)(part->sector_count * 512u));
        if (length < 0 || (size_t)length >= capacity - used) return -ENOSPC;
        used += (size_t)length;
    }
    return (int)used;
}
