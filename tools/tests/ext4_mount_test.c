/* Host-side tests for the ext4 mount path, feature policy and bounded caches.
 *
 * Covers kernel/reliefnt/fs/storage_ext4_mount.c and
 * storage_ext4_cache.c against real mke2fs images that
 * tools/tests/ext4_mount_test.py patches per case:
 *
 *  - storage_ext4_mount(): feature policy verdicts (rw / ro plus
 *    read_only_reason / -RELIEFOS_EOPNOTSUPP with the offending bit mask
 *    logged), ext2-vs-ext4 classification, partition-boundary and checksum
 *    failures, journal RECOVER handling, and mount_generation growth across
 *    remounts.  Failed mounts must release the per-volume ext4 state.
 *  - the five bounded cache functions: read-through and hits, pinning via
 *    storage_ext4_cache_get(..., for_write), dirty publication via
 *    storage_ext4_cache_mark_dirty, write-back on flush and on eviction,
 *    invalidate, -RELIEFOS_ENOMEM when every entry is pinned, the uncached
 *    fallback of storage_ext4_cache_read() when the table cannot be
 *    allocated, and generation-guarded staleness.
 *  - storage_sync_volume(): ext-family volumes flush the ext4 cache before
 *    the transport flush, and cache write-back never runs while the transport
 *    spinlock is held.
 *
 * Fixture conventions follow tools/tests/storage_rename_test.c: the storage
 * fragments are included into this translation unit with hand-written kernel
 * fakes and a RAM-disk device layer that counts I/O.  The case is selected by
 * argv[2]; the image is always argv[1].
 *
 * Build (task 4; the four-path include convention is progress.md preflight
 * #5):
 *   cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 *     -Ikernel/reliefnt/include -Iinclude \
 *     -Ikernel/reliefnt/include/uapi -Ikernel/reliefnt/kernel/reliefnt/include \
 *     tools/tests/ext4_mount_test.c -o build/ext4-mount-test
 *   ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
 *     build/ext4-mount-test <image> <case>
 * The EXT4_*_CACHE_ENTRIES macros may be overridden with -D to exercise
 * eviction at small scale (the "cache-small" case; see ext4_mount_test.py).
 */
#include <reliefnt/console.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/block/storage_internal.h"
#include "../../kernel/reliefnt/block/storage_state_data.c"

/* ---- kernel fakes ---------------------------------------------------- */
static uint8_t *disk_bytes;
static uint64_t disk_size;
static uint64_t test_read_commands, test_write_commands;
static uint64_t test_read_bytes, test_write_bytes;
static uint64_t test_step, test_cache_write_step, test_transport_flush_step;
static unsigned test_fail_cache_alloc;
static unsigned transport_lock_depth;
static unsigned test_transport_flushes;
static char console_log_buffer[16384];

void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *p) { free(p); }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
void mm_free_page(uint64_t p) { free((void *)(uintptr_t)p); }
uint64_t mm_alloc_pages(uint32_t pages)
{ return test_fail_cache_alloc ? 0 : (uint64_t)(uintptr_t)calloc(pages, 4096); }
static void fixture_console_vprintf(const char *format, va_list arguments)
{
    size_t used = strlen(console_log_buffer);
    vsnprintf(console_log_buffer + used, sizeof(console_log_buffer) - used, format, arguments);
}

void console_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
/* The transport spinlock guards command submission only; device I/O issued by
 * the ext4 cache must never run while it is held (it would deadlock against
 * storage_read_device's own locking in the kernel). */
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; ++transport_lock_depth; }
void kernel_spin_unlock(struct kernel_spinlock *lock)
{ (void)lock; assert(transport_lock_depth); --transport_lock_depth; }
uint8_t x86_64_inb(uint16_t port) { (void)port; abort(); }
uint16_t x86_64_inw(uint16_t port) { (void)port; abort(); }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; abort(); }
int time_wall_clock(struct reliefos_time_info *info)
{ *info = (struct reliefos_time_info){.unix_seconds = 1800000000}; return 0; }
void storage_memzero(void *out, size_t n) { memset(out, 0, n); }
void storage_memcpy(void *out, const void *in, size_t n) { memcpy(out, in, n); }
void storage_copy_text(char *out, uint32_t cap, const char *text)
{ snprintf(out, cap, "%s", text); }
size_t storage_strlen(const char *text) { return strlen(text); }
int storage_text_eq(const char *a, const char *b) { return !strcmp(a, b); }
void storage_cache_invalidate(void) {}
void storage_disk_block_cache_reset(void) {}
const char *storage_root_filesystem_name(void) { return "ext2"; }
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{ (void)bus; (void)slot; (void)function; (void)offset; return 0; }
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{ (void)bus; (void)slot; (void)function; (void)offset; return 0xffffu; }
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{ (void)phys; (void)len; return false; }
void *paging_kernel_direct_map(uint64_t phys) { (void)phys; return NULL; }

/* RAM-disk device layer.  Bounds mirror the production STORAGE_VOLUME_RAM
 * branch of storage_read_device; counters and the lock-depth check are the
 * test's observability points. */
int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                               uint32_t sector_count, void *buffer)
{
    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t bytes = (uint64_t)sector_count * SECTOR_SIZE;
    (void)volume;
    assert(transport_lock_depth == 0);
    ++test_read_commands;
    test_read_bytes += bytes;
    if (!buffer || !sector_count || offset + bytes < offset || offset + bytes > disk_size)
        return -RELIEFOS_EINVAL;
    memcpy(buffer, disk_bytes + offset, (size_t)bytes);
    return 0;
}

int storage_write_device(const struct storage_volume *volume, uint64_t lba,
                                uint32_t sector_count, const void *buffer)
{
    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t bytes = (uint64_t)sector_count * SECTOR_SIZE;
    (void)volume;
    assert(transport_lock_depth == 0);
    ++test_write_commands;
    test_write_bytes += bytes;
    test_cache_write_step = ++test_step;
    if (!buffer || !sector_count || offset + bytes < offset || offset + bytes > disk_size)
        return -RELIEFOS_EINVAL;
    memcpy(disk_bytes + offset, buffer, (size_t)bytes);
    return 0;
}

#include "../../kernel/reliefnt/drivers/storage/ide/storage_ide.c"

void storage_volume_ide_device(const struct storage_volume *volume,
                                      struct ide_device_info *device)
{
    (void)volume;
    *device = (struct ide_device_info){.present = 1, .command_base = 0x1f0,
                                       .control_base = 0x3f6};
}
int nvme_flush_cache(struct nvme_controller *controller, uint32_t nsid)
{ (void)controller; (void)nsid; abort(); }
int ahci_flush_cache(struct ahci_hba_port *port)
{
    assert(port && transport_lock_depth == 1);
    ++test_transport_flushes;
    test_transport_flush_step = ++test_step;
    return 0;
}

/* Route-level fakes for storage_mount.c: ext2_mount() is the fallback under
 * test, so it records whether the legacy backend was ever reached. */
static unsigned ext2_mount_calls;
int ext2_mount(void)
{
    ++ext2_mount_calls;
    g_storage.filesystem = STORAGE_FILESYSTEM_EXT2;
    return 0;
}
int exfat_mount(void) { return -2; }
int fat32_mount(void) { return -2; }
int iso9660_mount(void) { return -2; }
int gpt_find_esp(void) { return -2; }
void storage_volume_from_install_disk(struct storage_volume *volume,
                                             const struct install_disk_state *disk)
{ (void)volume; (void)disk; }
const char *storage_transport_name(uint8_t transport)
{ (void)transport; return "ahci"; }
void ahci_pending_clear(void) {}
int ahci_setup_port(struct ahci_hba_port *port) { (void)port; return -1; }
void nvme_reset_all_controllers(void) {}
void storage_scan_nvme_controller(uint8_t bus, uint8_t slot, uint8_t function,
                                         uint8_t *root_ready)
{ (void)bus; (void)slot; (void)function; (void)root_ready; }

#include "../../kernel/reliefnt/fs/storage_ext4_format.c"
#include "../../kernel/reliefnt/fs/storage_ext4_checksum.c"
#include "../../kernel/reliefnt/fs/storage_ext4_cache.c"
#include "../../kernel/reliefnt/fs/storage_ext4_alloc.c"
#include "../../kernel/reliefnt/fs/storage_ext4_extent.c"
#include "../../kernel/reliefnt/fs/storage_ext4_ops.c"
#include "../../kernel/reliefnt/fs/storage_ext4_xattr.c"
#include "../../kernel/reliefnt/fs/storage_ext4_journal.c"
#include "../../kernel/reliefnt/fs/storage_ext4_mount.c"
#include "../../kernel/reliefnt/fs/storage_mount.c"
#include "../../kernel/reliefnt/block/storage_sync.c"

/* ---- fixture helpers -------------------------------------------------- */
static void load_disk(const char *image, uint64_t part_offset_sectors,
                      uint64_t part_sector_count)
{
    FILE *file = fopen(image, "rb");
    long size;
    struct storage_volume *volume = &g_volumes[0];
    assert(file && !fseek(file, 0, SEEK_END));
    size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    disk_size = (uint64_t)size;
    free(disk_bytes);
    disk_bytes = malloc((size_t)size);
    assert(disk_bytes && fread(disk_bytes, 1, (size_t)size, file) == (size_t)size);
    assert(!fclose(file));
    storage_memzero(volume, sizeof(*volume));
    volume->volume_id = 0;
    volume->kind = STORAGE_VOLUME_RAM;
    volume->ext_start_lba = part_offset_sectors;
    volume->ext_sector_count = part_sector_count ? part_sector_count
                                                : (disk_size / SECTOR_SIZE) - part_offset_sectors;
    storage_copy_text(volume->mount_path, sizeof(volume->mount_path), "/");
    g_active_volume = volume;
}

static void assert_volume_untouched(const struct storage_volume *volume)
{
    assert(volume->filesystem == STORAGE_FILESYSTEM_NONE);
    assert(volume->read_only_reason == STORAGE_EXT4_READ_ONLY_NONE);
    assert(volume->ext4.blocks_count == 0 && volume->ext4.block_size == 0 &&
           volume->ext4.group_count == 0 && volume->ext4.desc_size == 0);
}

struct reject_case {
    const char *name;
    const char *log_needle;
};

/* The needles end at the line break so a short mask can never match inside a
 * longer one ("0x1" vs "0x1000"). */
static const struct reject_case reject_cases[] = {
    {"reject-unknown-incompat", "incompat=0x400000\n"},
    {"reject-incompat-compression", "incompat=0x1\n"},
    {"reject-incompat-journal-dev", "incompat=0x8\n"},
    {"reject-incompat-metabg", "incompat=0x10\n"},
    {"reject-incompat-mmp", "incompat=0x100\n"},
    {"reject-incompat-ea-inode", "incompat=0x400\n"},
    {"reject-incompat-dirdata", "incompat=0x1000\n"},
    {"reject-incompat-largedir", "incompat=0x4000\n"},
    {"reject-incompat-inline-data", "incompat=0x8000\n"},
    {"reject-incompat-encrypt", "incompat=0x10000\n"},
    {"reject-incompat-casefold", "incompat=0x20000\n"},
    {"reject-ro-quota", "ro_compat=0x100\n"},
    {"reject-ro-bigalloc", "ro_compat=0x200\n"},
    {"reject-ro-project", "ro_compat=0x2000\n"},
    {"reject-ro-verity", "ro_compat=0x8000\n"},
    {"reject-compat-fast-commit", "compat=0x400\n"},
};

static void expect_reject(const char *image, const char *needle)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    console_log_buffer[0] = 0;
    assert(storage_ext4_mount(volume) == -RELIEFOS_EOPNOTSUPP);
    assert(strstr(console_log_buffer, needle) != NULL);
    assert_volume_untouched(volume);
    printf("PASS ext4 reject: %s", needle); /* needle ends at the line break */
}

static void case_valid(const char *image)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT4);
    assert(volume->read_only_reason == STORAGE_EXT4_READ_ONLY_NONE);
    assert(volume->ext4.block_size == 4096);
    assert(volume->ext4.blocks_count == disk_size / 4096);
    assert(volume->ext4.inode_size == 256);
    assert(volume->ext4.desc_size == 32 || volume->ext4.desc_size == 64);
    assert(volume->ext4.first_data_block == 0);
    assert(volume->ext4.partition_start_lba == 0);
    assert(volume->ext4.partition_sector_count == disk_size / SECTOR_SIZE);
    assert(volume->ext4.blocks_per_group > 0 && volume->ext4.inodes_per_group > 0);
    assert(volume->ext4.group_count ==
           (volume->ext4.blocks_count + volume->ext4.blocks_per_group - 1) /
               volume->ext4.blocks_per_group);
    uint32_t generation = volume->mount_generation;
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->mount_generation == generation + 1);
    puts("PASS ext4 mount: valid image geometry, classification and mount_generation");
}

static void case_valid_offset(const char *image)
{
    /* The Python driver prepends one MiB of partition padding. */
    const uint64_t pad_sectors = 2048;
    struct storage_volume *volume;
    load_disk(image, pad_sectors, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT4);
    assert(volume->read_only_reason == STORAGE_EXT4_READ_ONLY_NONE);
    assert(volume->ext4.partition_start_lba == pad_sectors);
    assert(volume->ext4.blocks_count == (disk_size / 4096) - (pad_sectors * SECTOR_SIZE / 4096));
    assert(volume->ext4.partition_sector_count == (disk_size / SECTOR_SIZE) - pad_sectors);
    puts("PASS ext4 mount: superblock read is partition-relative");
}

static void case_ext2_classify(const char *image)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT2);
    assert(volume->read_only_reason == STORAGE_EXT4_READ_ONLY_NONE);
    puts("PASS ext4 mount: classic ext2 feature set classifies as EXT2");
}

static void case_failure(const char *image, int expected, const char *label)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == expected);
    assert_volume_untouched(volume);
    printf("PASS ext4 mount failure: %s\n", label);
}

static void case_boundary(const char *image)
{
    /* The partition claims only half of the image's sectors. */
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    volume->ext_sector_count = (disk_size / SECTOR_SIZE) / 2;
    assert(storage_ext4_mount(volume) == -RELIEFOS_EINVAL);
    assert_volume_untouched(volume);
    puts("PASS ext4 mount failure: partition smaller than the filesystem");
}

static void case_ro(const char *image, uint32_t reason, uint8_t filesystem, const char *label)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->filesystem == filesystem);
    assert(volume->read_only_reason == reason);
    assert(volume->ext4.block_size == 4096 && volume->ext4.blocks_count > 0);
    if (reason != STORAGE_EXT4_READ_ONLY_NONE) {
        uint64_t first; uint32_t allocated;
        assert(storage_ext4_alloc_blocks(volume, 100, 1, &first, &allocated) == -RELIEFOS_EROFS);
        assert(test_write_commands == 0);
    }
    printf("PASS ext4 mount read-only: %s\n", label);
}

static void case_multi_group(const char *image)
{
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT4);
    assert(volume->ext4.group_count > 1);
    assert(volume->ext4.group_count ==
           (volume->ext4.blocks_count + volume->ext4.blocks_per_group - 1) /
               volume->ext4.blocks_per_group);
    puts("PASS ext4 mount: multi-group descriptors all verified");
}

/* ---- root route cases (storage_mount_ext_family) ---------------------- */
static void case_route_policy_reject(const char *image)
{
    /* ext2-shaped image with RO_COMPAT_BIGALLOC: the feature policy must
     * reject it and the route must NOT fall back to the legacy ext2 backend
     * (global constraint: unknown/unsupported features never downgrade to
     * ext2 logic, which would mount them read-write). */
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    console_log_buffer[0] = 0;
    assert(storage_mount_ext_family(volume) == -RELIEFOS_EOPNOTSUPP);
    assert(ext2_mount_calls == 0);
    assert(!volume->ready);
    assert_volume_untouched(volume);
    assert(strstr(console_log_buffer, "ro_compat=0x200\n") != NULL);
    puts("PASS ext4 root route: policy rejection never reaches ext2_mount");
}

static void case_route_probe_fallback(const char *image)
{
    /* A probe failure that is not a policy rejection (unrecognized image)
     * keeps the legacy fallback behavior. */
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_mount_ext_family(volume) == -RELIEFOS_EINVAL);
    assert(ext2_mount_calls == 0);
    puts("PASS ext-family probe: non-ext media never enters a legacy backend");
}

static void case_route_corrupt_metadata(const char *image)
{
    load_disk(image, 0, 0);
    assert(storage_mount_ext_family(&g_volumes[0]) == -RELIEFOS_EIO);
    assert(ext2_mount_calls == 0 && test_write_commands == 0);
    puts("PASS corrupt ext metadata never falls back to legacy ext2");
}

static void case_route_ext2_classify(const char *image)
{
    /* Classic ext2 classification also keeps the legacy backend. */
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_mount_ext_family(volume) == 0);
    assert(ext2_mount_calls == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT2);
    puts("PASS ext-family route: EXT2 classification uses the unified backend");
}

static void case_route_ext4(const char *image)
{
    /* A policy-clean ext4 image mounts with the new state and never touches
     * the legacy backend. */
    struct storage_volume *volume;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_mount_ext_family(volume) == 0);
    assert(ext2_mount_calls == 0);
    assert(volume->filesystem == STORAGE_FILESYSTEM_EXT4);
    assert(volume->read_only_reason == STORAGE_EXT4_READ_ONLY_NONE);
    puts("PASS ext4 root route: ext4 image mounts without ext2_mount");
}

/* ---- bounded cache cases ---------------------------------------------- */
static void test_cache_alloc_fail(const char *image)
{
    /* Fresh process: the cache tables start unallocated.  Mounting with
     * failing allocation still succeeds because reads degrade to uncached
     * device I/O; get() must report -ENOMEM. */
    struct storage_volume *volume;
    uint8_t buf[EXT4_MAX_BLOCK_SIZE];
    uint8_t *data;
    uint64_t reads_before;

    test_fail_cache_alloc = 1;
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 100, buf) == 0);
    assert(test_read_commands == reads_before + 1);
    assert(!memcmp(buf, disk_bytes + 100 * (uint64_t)volume->ext4.block_size,
                   volume->ext4.block_size));
    assert(storage_ext4_cache_get(volume, 100, &data, false) == -RELIEFOS_ENOMEM);
    assert(storage_ext4_cache_get(volume, 100, &data, true) == -RELIEFOS_ENOMEM);
    test_fail_cache_alloc = 0;
    /* Allocation works again once the fault clears. */
    assert(storage_ext4_cache_get(volume, 100, &data, true) == 0);
    puts("PASS ext4 cache: allocation failure degrades reads and fails get with ENOMEM");
}

static void test_cache_semantics(const char *image)
{
    struct storage_volume *volume;
    uint8_t buf[EXT4_MAX_BLOCK_SIZE];
    uint8_t *data;
    uint64_t reads_before;
    uint32_t bs;

    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    bs = volume->ext4.block_size;

    /* Read-through, then a hit with no further device I/O. */
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 100, buf) == 0);
    assert(test_read_commands == reads_before + 1);
    assert(!memcmp(buf, disk_bytes + 100 * (uint64_t)bs, bs));
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 100, buf) == 0);
    assert(test_read_commands == reads_before);

    /* Pin, modify, publish with mark_dirty, write back with flush. */
    assert(storage_ext4_cache_get(volume, 100, &data, true) == 0);
    memset(data, 'X', 16);
    assert(storage_ext4_cache_mark_dirty(volume, 100) == 0);
    assert(storage_ext4_cache_flush(volume) == 0);
    assert(disk_bytes[100 * (uint64_t)bs] == 'X');
    assert(!memcmp(disk_bytes + 100 * (uint64_t)bs, data, bs));

    /* mark_dirty publishes an entry and clears its pin; flush drains it. */
    assert(storage_ext4_cache_mark_dirty(volume, 234) == -RELIEFOS_ENOENT);
    assert(storage_ext4_cache_mark_dirty(volume, volume->ext4.blocks_count + 5) ==
           -RELIEFOS_EINVAL);

    /* invalidate drops the volume's entries; the next read refetches. */
    memset(disk_bytes + 100 * (uint64_t)bs, 'Z', 16);
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_cache_read(volume, 100, buf) == 0 && buf[0] == 'Z');

    /* Pinned entries are never evicted; exhaustion returns -ENOMEM. */
    storage_ext4_cache_invalidate(volume);
    {
        uint8_t *pinned[EXT4_BLOCK_CACHE_ENTRIES];
        for (uint32_t i = 0; i < EXT4_BLOCK_CACHE_ENTRIES; ++i)
            assert(storage_ext4_cache_get(volume, 200 + i, &pinned[i], true) == 0);
        assert(storage_ext4_cache_get(volume, 200 + EXT4_BLOCK_CACHE_ENTRIES, &data, true) ==
               -RELIEFOS_ENOMEM);
        assert(storage_ext4_cache_read(volume, 200 + EXT4_BLOCK_CACHE_ENTRIES, buf) ==
               -RELIEFOS_ENOMEM);
        /* mark_dirty releases the pin and marks the entry dirty: the next
         * insert evicts it and writes the modification back first. */
        memset(pinned[0], 'A', 16);
        assert(storage_ext4_cache_mark_dirty(volume, 200) == 0);
        assert(storage_ext4_cache_get(volume, 300, &data, true) == 0);
        assert(disk_bytes[200 * (uint64_t)bs] == 'A');
    }
    /* flush() also releases outstanding pins (transaction end). */
    assert(storage_ext4_cache_flush(volume) == 0);

    /* Entries carry volume->mount_generation; a newer generation (every
     * mount increments it) makes old entries stale. */
    memset(disk_bytes + 300 * (uint64_t)bs, 'G', 16);
    ++volume->mount_generation;
    assert(storage_ext4_cache_read(volume, 300, buf) == 0 && buf[0] == 'G');

    /* Out-of-range blocks are refused before any device I/O. */
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, volume->ext4.blocks_count + 1, buf) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_cache_get(volume, volume->ext4.blocks_count + 1, &data, true) ==
           -RELIEFOS_EINVAL);
    assert(test_read_commands == reads_before);
    puts("PASS ext4 cache: read-through, pin/dirty/flush, ENOMEM, invalidate, generation guard");
}

static void test_cache_small(const char *image)
{
    struct storage_volume *volume;
    uint8_t buf[EXT4_MAX_BLOCK_SIZE];
    uint8_t *data;
    uint64_t reads_before;

    /* The driver builds this binary with -DEXT4_*_CACHE_ENTRIES=2: the
     * capacity macros are compile-time overridable. */
    assert(EXT4_BLOCK_CACHE_ENTRIES == 2 && EXT4_INODE_CACHE_ENTRIES == 2 &&
           EXT4_DIR_CACHE_ENTRIES == 2 && EXT4_JOURNAL_CACHE_ENTRIES == 2);
    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);

    /* LRU: fill two blocks, touch the first again, then a third insert
     * evicts the untouched second block. */
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_cache_read(volume, 300, buf) == 0);
    assert(storage_ext4_cache_read(volume, 301, buf) == 0);
    assert(storage_ext4_cache_read(volume, 300, buf) == 0);
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 302, buf) == 0);
    assert(test_read_commands == reads_before + 1); /* 301 evicted */
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 300, buf) == 0);
    assert(test_read_commands == reads_before); /* 300 survived */
    reads_before = test_read_commands;
    assert(storage_ext4_cache_read(volume, 301, buf) == 0);
    assert(test_read_commands == reads_before + 1); /* refetched */

    /* Dirty eviction writes back before reuse. */
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_cache_get(volume, 310, &data, true) == 0);
    memset(data, 'D', 16);
    assert(storage_ext4_cache_mark_dirty(volume, 310) == 0);
    assert(storage_ext4_cache_get(volume, 311, &data, true) == 0);
    memset(data, 'E', 16);
    assert(storage_ext4_cache_mark_dirty(volume, 311) == 0);
    assert(storage_ext4_cache_read(volume, 312, buf) == 0);
    assert(disk_bytes[310 * (uint64_t)volume->ext4.block_size] == 'D');

    /* Full and pinned at capacity 2. */
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_cache_get(volume, 320, &data, true) == 0);
    assert(storage_ext4_cache_get(volume, 321, &data, true) == 0);
    assert(storage_ext4_cache_get(volume, 322, &data, true) == -RELIEFOS_ENOMEM);
    puts("PASS ext4 cache (small capacities): LRU order, dirty eviction, pinned exhaustion");
}

static void case_sync(const char *image){
    struct storage_volume *volume;
    uint8_t *data;

    load_disk(image, 0, 0);
    volume = &g_volumes[0];
    assert(storage_ext4_mount(volume) == 0);
    assert(storage_ext4_cache_get(volume, 150, &data, true) == 0);
    memset(data, 'S', 16);
    assert(storage_ext4_cache_mark_dirty(volume, 150) == 0);
    /* Present an AHCI transport so the device-cache flush is observable. */
    volume->kind = STORAGE_VOLUME_AHCI;
    volume->transport = STORAGE_TRANSPORT_AHCI;
    volume->hba_port = (struct ahci_hba_port *)(uintptr_t)4096;
    volume->ready = true;
    assert(storage_sync_volume(0) == 0);
    assert(disk_bytes[150 * (uint64_t)volume->ext4.block_size] == 'S');
    assert(test_transport_flushes == 1);
    assert(test_cache_write_step != 0 && test_cache_write_step < test_transport_flush_step);
    puts("PASS ext4 sync: cache flush precedes the transport flush, outside the spinlock");
}

int main(int argc, char **argv)
{
    const char *image;
    const char *test_case;
    assert(argc == 3);
    image = argv[1];
    test_case = argv[2];
    for (size_t i = 0; i < sizeof(reject_cases) / sizeof(reject_cases[0]); ++i) {
        if (!strcmp(test_case, reject_cases[i].name)) {
            expect_reject(image, reject_cases[i].log_needle);
            free(disk_bytes);
            return 0;
        }
    }
    if (!strcmp(test_case, "valid")) case_valid(image);
    else if (!strcmp(test_case, "valid-partition-offset")) case_valid_offset(image);
    else if (!strcmp(test_case, "ext2-classify")) case_ext2_classify(image);
    else if (!strcmp(test_case, "multi-group")) case_multi_group(image);
    else if (!strcmp(test_case, "bad-magic")) case_failure(image, -RELIEFOS_EINVAL, "bad magic");
    else if (!strcmp(test_case, "huge-blocks-count"))
        case_failure(image, -RELIEFOS_EINVAL, "huge blocks_count");
    else if (!strcmp(test_case, "boundary-small-partition")) case_boundary(image);
    else if (!strcmp(test_case, "super-checksum-broken"))
        case_failure(image, -RELIEFOS_EIO, "superblock checksum");
    else if (!strcmp(test_case, "gd-checksum-broken"))
        case_failure(image, -RELIEFOS_EIO, "group descriptor checksum");
    else if (!strcmp(test_case, "gd-checksum-group3"))
        case_failure(image, -RELIEFOS_EIO, "non-zero group descriptor checksum");
    else if (!strcmp(test_case, "inode-checksum-broken"))
        case_failure(image, -RELIEFOS_EIO, "root inode checksum");
    else if (!strcmp(test_case, "ro-readonly-feature"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_READONLY_FEATURE, STORAGE_FILESYSTEM_EXT4,
                "READONLY_FEATURE");
    else if (!strcmp(test_case, "ro-unknown-rocompat"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT, STORAGE_FILESYSTEM_EXT4,
                "UNKNOWN_RO_COMPAT");
    else if (!strcmp(test_case, "ro-unknown-rocompat-ext2shape"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT, STORAGE_FILESYSTEM_EXT4,
                "UNKNOWN_RO_COMPAT on a classic ext2 shape");
    else if (!strcmp(test_case, "journal-recover"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_NONE, STORAGE_FILESYSTEM_EXT4,
                "empty journal recovered");
    else if (!strcmp(test_case, "journal-clean"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_NONE, STORAGE_FILESYSTEM_EXT4, "clean journal");
    else if (!strcmp(test_case, "journal-corrupt"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT, STORAGE_FILESYSTEM_EXT4, "corrupt journal");
    else if (!strcmp(test_case, "bare-recover"))
        case_ro(image, STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY, STORAGE_FILESYSTEM_EXT4,
                "bare RECOVER without HAS_JOURNAL");
    else if (!strcmp(test_case, "route-policy-reject")) case_route_policy_reject(image);
    else if (!strcmp(test_case, "route-corrupt-metadata")) case_route_corrupt_metadata(image);
    else if (!strcmp(test_case, "route-probe-fallback")) case_route_probe_fallback(image);
    else if (!strcmp(test_case, "route-ext2-classify")) case_route_ext2_classify(image);
    else if (!strcmp(test_case, "route-ext4")) case_route_ext4(image);
    else if (!strcmp(test_case, "cache-alloc-fail")) test_cache_alloc_fail(image);
    else if (!strcmp(test_case, "cache")) test_cache_semantics(image);
    else if (!strcmp(test_case, "cache-small")) test_cache_small(image);
    else if (!strcmp(test_case, "sync")) case_sync(image);
    else {
        fprintf(stderr, "unknown case %s\n", test_case);
        return 2;
    }
    free(disk_bytes);
    return 0;
}
