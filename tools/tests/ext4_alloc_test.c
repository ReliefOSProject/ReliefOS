/* Host-side tests for the ext4 group descriptor, bitmap and flex-group
 * allocator (kernel/reliefnt/fs/storage_ext4_alloc.c).
 *
 * Covers, against synthetic ext4 images built byte by byte here and against
 * real mke2fs images:
 *
 *  - storage_ext4_read_group()/storage_ext4_write_group(): descriptor
 *    addressing with desc_size and a 64-bit group count (including a
 *    descriptor that straddles two blocks), checksum verification on read,
 *    byte preservation and bg_checksum refresh on write, and the bounded
 *    group-summary cache (EXT4_GROUP_CACHE_ENTRIES, LRU, mount-generation
 *    stamped).
 *  - storage_ext4_alloc_blocks(): sequential allocation across two flex
 *    groups, a contiguous run crossing a block-group boundary near goal,
 *    goal-vicinity hits (forward/backward scan), fragmented fallback to the
 *    best shorter run, no allocation past the end of the filesystem or past
 *    a group boundary, uninit_bg first-touch bitmap initialization
 *    (metadata bits and past-the-end bits set, free count recomputed,
 *    BLOCK_UNINIT cleared, bitmap checksum refreshed), double-free
 *    rejection, bitmap checksum corruption -> -RELIEFOS_EIO plus the sticky
 *    volume error flag, ENOSPC, free/realloc reuse, the reservation window
 *    around next_goal_block, and superblock free counts staying consistent
 *    with the group descriptors.
 *  - storage_ext4_alloc_inode()/storage_ext4_free_inode(): reserved inodes
 *    1-10 never returned and never freed, regular vs directory group
 *    choice, INODE_UNINIT first-touch init, used_dirs bookkeeping,
 *    i_links_count validation and inode-table zeroing on free.
 *  - real mke2fs image cross-check: the group descriptors and bitmaps of a
 *    metadata_csum+flex_bg+uninit_bg image must verify with our checksum
 *    primitives and report the same free counts dumpe2fs prints; after our
 *    allocations the modified image must still pass e2fsck -f -n and
 *    dumpe2fs must show exactly the blocks we took.
 *
 * Fixture conventions follow tools/tests/ext4_mount_test.c: kernel fakes and
 * a RAM-disk device layer that counts I/O, with the production storage
 * fragments linked as separate translation units (format/checksum/cache/alloc;
 * storage_ext4_mount.c is included below and needs the two cache-internal
 * statics stubbed here).
 *
 * Build (task 5; 4-path include convention plus -include for the fragment
 * TUs that carry no includes of their own):
 *   cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 *     -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *     -DRELIEFOS_STORAGE_STANDALONE_TU \
 *     -Ikernel/reliefnt/include -Iinclude \
 *     -Ikernel/reliefnt/include/uapi -Ikernel/reliefnt/kernel/reliefnt/include \
 *     -include kernel/reliefnt/block/storage_internal.h \
 *     tools/tests/ext4_alloc_test.c \
 *     kernel/reliefnt/fs/storage_ext4_format.c \
 *     kernel/reliefnt/fs/storage_ext4_checksum.c \
 *     kernel/reliefnt/fs/storage_ext4_cache.c \
 *     kernel/reliefnt/fs/storage_ext4_alloc.c \
 *     kernel/reliefnt/fs/storage_ext4_extent.c \
 *     kernel/reliefnt/fs/storage_ext4_ops.c \
 *     kernel/reliefnt/fs/storage_ext4_journal.c \
 *     -o /tmp/reliefos-ext4-alloc-test
 *   ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
 *     /tmp/reliefos-ext4-alloc-test
 * EXT4_GROUP_CACHE_ENTRIES may be overridden with -D to exercise the
 * group-summary LRU at small scale (the "group-cache-small" case).
 */
/* -D_GNU_SOURCE also comes from the build command (the -include'd
 * storage_internal.h runs before this file's defines). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kernel/reliefnt/block/storage_internal.h"

/* ---- kernel fakes and fake disk layer --------------------------------- */
static uint8_t *disk_bytes;
static uint64_t disk_size;
static uint64_t test_read_commands, test_write_commands;
static char console_log[16384];

void *kernel_malloc(size_t size) { return malloc(size); }
int storage_ext4_device_flush(const struct storage_volume *v) { (void)v; return 0; }
void kernel_free(void *p) { free(p); }
int time_wall_clock(struct reliefos_time_info *info)
{ memset(info,0,sizeof(*info)); info->unix_seconds=1790640000; return 0; }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
void mm_free_page(uint64_t p) { free((void *)(uintptr_t)p); }
uint64_t mm_alloc_pages(uint32_t pages)
{ return (uint64_t)(uintptr_t)calloc(pages, 4096); }
void console_printf(const char *format, ...)
{
    va_list ap;
    size_t used = strlen(console_log);
    va_start(ap, format);
    vsnprintf(console_log + used, sizeof(console_log) - used, format, ap);
    va_end(ap);
}
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }

/* Fake disk layer: the four facade helpers the standalone fragment TUs
 * link against (RELIEFOS_STORAGE_STANDALONE_TU prototypes in
 * storage_internal.h).  Bounds mirror the production STORAGE_VOLUME_RAM
 * branch of storage_read_device. */
void storage_memzero(void *dst, size_t len) { memset(dst, 0, len); }
void storage_memcpy(void *dst, const void *src, size_t len)
{ memcpy(dst, src, len); }

int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                        uint32_t sector_count, void *buffer)
{
    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t bytes = (uint64_t)sector_count * SECTOR_SIZE;
    (void)volume;
    ++test_read_commands;
    if (!buffer || !sector_count || offset + bytes < offset ||
        offset + bytes > disk_size)
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
    ++test_write_commands;
    if (!buffer || !sector_count || offset + bytes < offset ||
        offset + bytes > disk_size)
        return -RELIEFOS_EINVAL;
    memcpy(disk_bytes + offset, buffer, (size_t)bytes);
    return 0;
}

/* storage_ext4_mount.c reaches into cache.c's internals; cache.c keeps both
 * static, so this TU provides its own copies for the included mount path. */
static void ext4_cache_note_checksum_ok(struct storage_volume *volume,
                                        uint64_t block)
{
    (void)volume;
    (void)block;
}

static int ext4_cache_block_range(const struct storage_volume *volume,
                                  uint64_t block, uint64_t *out_lba,
                                  uint32_t *out_sectors)
{
    uint32_t block_size;
    uint32_t sectors;
    uint64_t offset;

    if (!volume)
        return -RELIEFOS_EINVAL;
    block_size = volume->ext4.block_size;
    if (block_size < EXT4_MIN_BLOCK_SIZE || block_size > EXT4_MAX_BLOCK_SIZE ||
        (block_size % SECTOR_SIZE) != 0 || block >= volume->ext4.blocks_count)
        return -RELIEFOS_EINVAL;
    sectors = block_size / SECTOR_SIZE;
    offset = block * (uint64_t)sectors;
    if (offset / sectors != block || offset + sectors > volume->ext_sector_count ||
        volume->ext_start_lba + offset < volume->ext_start_lba)
        return -RELIEFOS_EINVAL;
    *out_lba = volume->ext_start_lba + offset;
    *out_sectors = sectors;
    return 0;
}

#include "../../kernel/reliefnt/fs/storage_ext4_mount.c"

static uint8_t ext4_alloc_block_test[EXT4_MAX_BLOCK_SIZE];

/* storage_ext4_cache.c keeps its capacity macro file-local with the same
 * default; the -D build overrides both copies identically. */
#ifndef EXT4_BLOCK_CACHE_ENTRIES
#define EXT4_BLOCK_CACHE_ENTRIES 64u
#endif

/* Test-local copies of the superblock offsets this fixture writes (the
 * frozen tests' two-copy convention: a wrong shared constant must not
 * write and read the same wrong slot).  Verified against real mke2fs
 * images: s_log_groups_per_flex at 0x174, s_backup_bgs at 0x24C. */
#define T_SB_FLEX_LOG 0x174
#define T_SB_BACKUP_BGS 0x24C

/* ---- synthetic image fixture ------------------------------------------ */
#define T_BSIZE 4096u
#define T_INODE_SIZE 256u

enum fixture_layout {
    FIXTURE_INLINE = 0, /* each group's bitmaps/itable at its own start */
    FIXTURE_FLEX = 1,   /* all groups' bitmaps/itables clustered in group 0 */
};

struct img_spec {
    uint32_t bpg;
    uint32_t ipg;
    uint32_t groups;
    uint32_t blocks;
    uint32_t desc_size;
    uint8_t flex_log;
    uint8_t layout;
    uint8_t sparse_super;
    uint8_t sparse_super2;
    uint8_t gdt_csum_only;
    uint32_t backup_bgs[2];
};

static struct img_spec spec;
static struct storage_ext4_super_view fixture_view;
static int fixture_synthetic;

static uint32_t itb_blocks(void) { return spec.ipg * T_INODE_SIZE / T_BSIZE; }
static uint32_t gdt_blocks(void)
{ return (spec.groups * spec.desc_size + T_BSIZE - 1) / T_BSIZE; }
static uint32_t base_meta(void) { return 1u + gdt_blocks(); }

static uint64_t group_first(uint32_t group)
{ return (uint64_t)group * spec.bpg; }

static uint32_t group_blocks(uint32_t group)
{
    uint64_t first = group_first(group);
    return (uint32_t)((spec.blocks - first) < spec.bpg ? (spec.blocks - first)
                                                       : spec.bpg);
}

static uint32_t bbm_block(uint32_t group)
{
    if (spec.layout == FIXTURE_FLEX)
        return base_meta() + group;
    return (uint32_t)group_first(group) + base_meta();
}

static uint32_t ibm_block(uint32_t group)
{
    if (spec.layout == FIXTURE_FLEX)
        return base_meta() + spec.groups + group;
    return (uint32_t)group_first(group) + base_meta() + 1u;
}

static uint32_t itable_block(uint32_t group)
{
    if (spec.layout == FIXTURE_FLEX)
        return base_meta() + 2u * spec.groups + group * itb_blocks();
    return (uint32_t)group_first(group) + base_meta() + 2u;
}

/* test_root() from linux/fs/ext4/balloc.c: is `a` a pure power of `b`? */
static int test_root(uint32_t a, uint32_t b)
{
    while (a % b == 0) {
        a /= b;
        if (a == 1)
            return 1;
    }
    return 0;
}

static int group_has_super(uint32_t group)
{
    if (group == 0)
        return 1;
    if ((fixture_view.feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) != 0)
        return group == spec.backup_bgs[0] || group == spec.backup_bgs[1];
    if (group <= 1 ||
        (fixture_view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0)
        return 1;
    return test_root(group, 3) || test_root(group, 5) || test_root(group, 7);
}

/* Metadata blocks of `group` that live inside `group`'s own block range. */
static uint32_t inline_overhead(uint32_t group)
{
    if (spec.layout == FIXTURE_FLEX) {
        /* Only the superblock/gd backups (groups that carry one) are
         * in-group; the bitmaps and itables live in group 0. */
        return group_has_super(group) ? 2u : 0u;
    }
    return base_meta() + 2u + itb_blocks();
}

static uint8_t *gd_raw(uint32_t group)
{ return disk_bytes + T_BSIZE + (uint64_t)group * spec.desc_size; }

static uint8_t *block_raw(uint32_t block)
{ return disk_bytes + (uint64_t)block * T_BSIZE; }

static void set_bitmap_bit(uint8_t *bm, uint32_t bit)
{ bm[bit / 8u] |= (uint8_t)(1u << (bit % 8u)); }

static void clear_bitmap_bit(uint8_t *bm, uint32_t bit)
{ bm[bit / 8u] &= (uint8_t)~(1u << (bit % 8u)); }

static int bitmap_bit(const uint8_t *bm, uint32_t bit)
{ return (bm[bit / 8u] >> (bit % 8u)) & 1u; }

/* Recompute the bitmap checksum words, the descriptor checksums and the
 * superblock checksum of the current synthetic image (fixture-side mirror
 * of the on-disk formulas; the real-image case cross-checks them against
 * mke2fs). */
static void image_resync(void)
{
    uint32_t seed = storage_ext4_super_csum_seed(&fixture_view);

    for (uint32_t g = 0; g < spec.groups; ++g) {
        uint8_t *gd = gd_raw(g);
        uint16_t flags = ext4_get_le16(gd + 0x12);

        /* Bitmaps of UNINIT groups carry no checksum (linux/fs/ext4/
         * bitmap.c callers skip them); their stored words stay untouched. */
        if ((flags & EXT4_BG_BLOCK_UNINIT) == 0) {
            uint32_t bbm_csum = storage_ext4_crc32c(
                seed, block_raw(bbm_block(g)), spec.bpg / 8u);
            ext4_put_le16(gd + 0x18, (uint16_t)(bbm_csum & 0xffffu));
            if (spec.desc_size >= 0x3Cu)
                ext4_put_le16(gd + 0x38, (uint16_t)(bbm_csum >> 16));
        }
        if ((flags & EXT4_BG_INODE_UNINIT) == 0) {
            uint32_t ibm_csum = storage_ext4_crc32c(
                seed, block_raw(ibm_block(g)), spec.ipg / 8u);
            ext4_put_le16(gd + 0x1A, (uint16_t)(ibm_csum & 0xffffu));
            if (spec.desc_size >= 0x3Cu)
                ext4_put_le16(gd + 0x3A, (uint16_t)(ibm_csum >> 16));
        }
        assert(storage_ext4_update_group_checksum(gd, spec.desc_size, g,
                                                  &fixture_view) == 0);
    }
    assert(storage_ext4_update_super_checksum(disk_bytes + 1024,
                                              EXT4_SUPERBLOCK_SIZE) == 0);
}

static void fixture_write_root_inode(void)
{
    uint8_t *ino = block_raw(itable_block(0)) + T_INODE_SIZE; /* inode 2 */
    ext4_put_le16(ino + 0x00, EXT2_S_IFDIR | 0755u);
    ext4_put_le16(ino + 0x1A, 2);
    ext4_put_le32(ino + 0x04, T_BSIZE);
    ext4_put_le32(ino + 0x64, 1); /* i_generation */
    assert(storage_ext4_update_inode_checksum(ino, T_INODE_SIZE, 2, 1,
                                              &fixture_view) == 0);
}

static void load_spec(const struct img_spec *s)
{
    uint64_t total_free_blocks = 0, total_free_inodes = 0;

    spec = *s;
    fixture_synthetic = 1;
    free(disk_bytes);
    disk_size = (uint64_t)spec.blocks * T_BSIZE;
    disk_bytes = calloc(1, (size_t)disk_size);
    assert(disk_bytes);

    memset(&fixture_view, 0, sizeof(fixture_view));
    fixture_view.blocks_count = spec.blocks;
    fixture_view.group_count = spec.groups;
    fixture_view.inodes_count = spec.ipg * spec.groups;
    fixture_view.block_size = T_BSIZE;
    fixture_view.cluster_size = T_BSIZE;
    fixture_view.blocks_per_group = spec.bpg;
    fixture_view.inodes_per_group = spec.ipg;
    fixture_view.first_data_block = 0;
    fixture_view.desc_size = spec.desc_size;
    fixture_view.inode_size = T_INODE_SIZE;
    fixture_view.feature_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE |
                                    EXT4_FEATURE_INCOMPAT_EXTENTS |
                                    EXT4_FEATURE_INCOMPAT_64BIT |
                                    EXT4_FEATURE_INCOMPAT_FLEX_BG;
    fixture_view.feature_ro_compat =
        s->gdt_csum_only ? EXT4_FEATURE_RO_COMPAT_GDT_CSUM
                         : EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    if (s->sparse_super)
        fixture_view.feature_ro_compat |= EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER;
    fixture_view.feature_compat =
        s->sparse_super2 ? EXT4_FEATURE_COMPAT_SPARSE_SUPER2 : 0;
    for (uint32_t i = 0; i < 16; ++i)
        fixture_view.uuid[i] = (uint8_t)(0x78u + i);

    /* Superblock. */
    {
        uint8_t *sb = disk_bytes + 1024;
        ext4_put_le32(sb + 0x00, fixture_view.inodes_count);
        ext4_put_le32(sb + 0x04, spec.blocks);
        ext4_put_le32(sb + 0x0C, 0); /* free blocks, patched below */
        ext4_put_le32(sb + 0x10, 0); /* free inodes, patched below */
        ext4_put_le32(sb + 0x14, 0); /* s_first_data_block */
        ext4_put_le32(sb + 0x18, 2); /* s_log_block_size */
        ext4_put_le32(sb + 0x1C, 2); /* s_log_cluster_size */
        ext4_put_le32(sb + 0x20, spec.bpg);
        ext4_put_le32(sb + 0x24, spec.bpg);
        ext4_put_le32(sb + 0x28, spec.ipg);
        ext4_put_le16(sb + 0x38, EXT4_SUPER_MAGIC);
        ext4_put_le16(sb + 0x3A, 1); /* s_state VALID */
        ext4_put_le32(sb + 0x48, 0); /* s_creator_os = EXT4_OS_LINUX */
        ext4_put_le32(sb + 0x4C, 1); /* s_rev_level dynamic */
        ext4_put_le32(sb + 0x54, EXT4_GOOD_OLD_FIRST_INO);
        ext4_put_le16(sb + 0x58, T_INODE_SIZE);
        ext4_put_le32(sb + 0x5C, fixture_view.feature_compat);
        ext4_put_le32(sb + 0x60, fixture_view.feature_incompat);
        ext4_put_le32(sb + 0x64, fixture_view.feature_ro_compat);
        memcpy(sb + 0x68, fixture_view.uuid, 16);
        ext4_put_le16(sb + 0xFE, spec.desc_size);
        sb[T_SB_FLEX_LOG] = spec.flex_log;
        ext4_put_le32(sb + T_SB_BACKUP_BGS, spec.backup_bgs[0]);
        ext4_put_le32(sb + T_SB_BACKUP_BGS + 4, spec.backup_bgs[1]);
        ext4_put_le32(sb + 0x0C, 0);
    }

    /* Group descriptors, bitmaps and the root inode. */
    for (uint32_t g = 0; g < spec.groups; ++g) {
        uint8_t *gd = gd_raw(g);
        uint32_t blocks = group_blocks(g);
        uint32_t overhead = inline_overhead(g);
        uint32_t free_blocks = blocks - overhead;
        uint32_t free_inodes = spec.ipg;
        uint8_t *bbm = block_raw(bbm_block(g));
        uint8_t *ibm = block_raw(ibm_block(g));

        if (spec.layout == FIXTURE_FLEX && g == 0) {
            /* Group 0 carries every group's bitmaps and itables. */
            overhead = base_meta() + 2u * spec.groups +
                       spec.groups * itb_blocks();
            free_blocks = blocks - overhead;
        }
        if (g == 0) {
            free_inodes = spec.ipg - 10u; /* inodes 1-10 reserved */
        }
        ext4_put_le32(gd + 0x00, bbm_block(g));
        ext4_put_le32(gd + 0x04, ibm_block(g));
        ext4_put_le32(gd + 0x08, itable_block(g));
        ext4_put_le16(gd + 0x0C, (uint16_t)free_blocks);
        ext4_put_le16(gd + 0x0E, (uint16_t)free_inodes);
        ext4_put_le16(gd + 0x10, g == 0 ? 1u : 0u); /* used_dirs */
        ext4_put_le16(gd + 0x12, EXT4_BG_INODE_ZEROED);
        ext4_put_le16(gd + 0x1C,
                      g == 0 ? (uint16_t)(spec.ipg - EXT4_GOOD_OLD_FIRST_INO)
                             : (uint16_t)spec.ipg); /* itable_unused */
        total_free_blocks += free_blocks;
        total_free_inodes += free_inodes;

        /* Block bitmap: the metadata blocks of this group, the tail past
         * the end of the filesystem and (flex layout) nothing else. */
        memset(bbm, 0, T_BSIZE);
        if (spec.layout == FIXTURE_INLINE) {
            for (uint32_t b = 0; b < overhead; ++b)
                set_bitmap_bit(bbm, b);
        } else {
            for (uint32_t b = 0; b < base_meta(); ++b) {
                if (group_has_super(g))
                    set_bitmap_bit(bbm, b);
            }
            if (g == 0) {
                for (uint32_t b = 0; b < 2u * spec.groups +
                                                spec.groups * itb_blocks();
                     ++b)
                    set_bitmap_bit(bbm, base_meta() + b);
            }
        }
        for (uint32_t b = blocks; b < spec.bpg; ++b)
            set_bitmap_bit(bbm, b); /* past the end of the fs */

        /* Inode bitmap: inodes 1-10 reserved in group 0. */
        memset(ibm, 0, T_BSIZE);
        if (g == 0) {
            for (uint32_t i = 0; i < 10; ++i)
                set_bitmap_bit(ibm, i);
        }
    }

    /* Inode table of group 0 holds the root inode. */
    fixture_write_root_inode();

    /* Superblock free counts. */
    {
        uint8_t *sb = disk_bytes + 1024;
        ext4_put_le32(sb + 0x0C, (uint32_t)total_free_blocks);
        ext4_put_le32(sb + 0x10, (uint32_t)total_free_inodes);
    }
    image_resync();
}

static struct storage_volume *mount_now(void)
{
    static uint32_t generation;
    struct storage_volume *volume = &g_volumes[0];

    if (fixture_synthetic)
        image_resync(); /* patches after load_spec must refresh checksums */
    storage_memzero(volume, sizeof(*volume));
    /* Keep mount_generation monotonic across cases like production reuse:
     * resetting it would let the cache serve blocks of the previous
     * image (the entries are stamped with the generation). */
    volume->mount_generation = ++generation;
    volume->volume_id = 0;
    volume->kind = STORAGE_VOLUME_RAM;
    volume->ext_start_lba = 0;
    volume->ext_sector_count = disk_size / SECTOR_SIZE;
    g_active_volume = volume;
    assert(storage_ext4_mount(volume) == 0);
    return volume;
}

/* ---- fixture patch helpers -------------------------------------------- */
static void patch_gd_u16(uint32_t group, uint32_t off, uint16_t value)
{ ext4_put_le16(gd_raw(group) + off, value); }

static void patch_gd_flags(uint32_t group, uint16_t flags)
{ ext4_put_le16(gd_raw(group) + 0x12, flags); }

static void patch_fill_bitmap(uint32_t group, int inode_bm, uint8_t fill)
{ memset(block_raw(inode_bm ? ibm_block(group) : bbm_block(group)), fill,
         T_BSIZE); }

static void patch_block_bitmap_pattern(uint32_t group, const uint8_t *keep_free,
                                       uint32_t keep_count)
{
    uint8_t *bm = block_raw(bbm_block(group));
    memset(bm, 0xff, T_BSIZE);
    for (uint32_t i = 0; i < keep_count; ++i)
        clear_bitmap_bit(bm, keep_free[i]);
}

static void patch_flip_byte(uint32_t block, uint32_t off)
{ block_raw(block)[off] ^= 0xffu; }

/* Make `used` inodes of the group appear used (first bits set). */
static void patch_inode_used(uint32_t group, uint32_t used)
{
    uint8_t *ibm = block_raw(ibm_block(group));
    memset(ibm, 0, T_BSIZE);
    for (uint32_t i = 0; i < used; ++i)
        set_bitmap_bit(ibm, i);
    patch_gd_u16(group, 0x0E, (uint16_t)(spec.ipg - used));
}

/* Mutations live in the block cache until flush; inspect through it. */
static void read_block_cached(struct storage_volume *volume, uint64_t block,
                              uint8_t *buf)
{
    assert(storage_ext4_cache_read(volume, block, buf) == 0);
}

/* Patch an inode's i_links_count through the cache so the change is
 * visible to the allocator (raw writes would race the cached copy). */
static void patch_inode_links(struct storage_volume *volume, uint64_t ino,
                              uint16_t links)
{
    uint64_t group = (ino - 1) / spec.ipg;
    uint32_t bit = (uint32_t)((ino - 1) % spec.ipg);
    uint64_t block = itable_block(group) +
                     ((uint64_t)bit * T_INODE_SIZE) / T_BSIZE;
    uint32_t off = (uint32_t)(((uint64_t)bit * T_INODE_SIZE) % T_BSIZE);
    uint8_t *data;

    assert(storage_ext4_cache_get(volume, block, &data, true) == 0);
    ext4_put_le16(data + off + 0x1A, links);
    assert(storage_ext4_cache_mark_dirty(volume, block) == 0);
}

/* ---- cases ------------------------------------------------------------- */
static void case_read_group(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_ext4_group_view view;
    struct storage_volume *volume;
    uint64_t reads;

    load_spec(&s);
    volume = mount_now();
    assert(volume->ext4.group_count == 8);
    assert(storage_ext4_read_group(volume, 0, &view) == 0);
    assert(view.block_bitmap == 2 && view.inode_bitmap == 3 &&
           view.inode_table == 4);
    assert(view.free_blocks_count == 56 && view.free_inodes_count == 54 &&
           view.used_dirs_count == 1);
    assert(view.flags == EXT4_BG_INODE_ZEROED);
    assert(storage_ext4_read_group(volume, 7, &view) == 0);
    assert(view.free_blocks_count == 44); /* short last group */
    assert(view.block_bitmap_csum != 0 && view.inode_bitmap_csum != 0);

    /* Second reads are served from the bounded summary cache. */
    reads = test_read_commands;
    assert(storage_ext4_read_group(volume, 7, &view) == 0);
    assert(storage_ext4_read_group(volume, 3, &view) == 0);
    assert(test_read_commands == reads);

    /* Bounds. */
    assert(storage_ext4_read_group(volume, 8, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_read_group(NULL, 0, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_read_group(volume, 0, NULL) == -RELIEFOS_EINVAL);

    /* write_group round trip preserves unknown bytes and refreshes
     * bg_checksum. */
    {
        uint8_t before[EXT4_MAX_DESC_SIZE];
        uint8_t after[EXT4_MAX_DESC_SIZE];
        memcpy(before, gd_raw(3), s.desc_size);
        assert(storage_ext4_read_group(volume, 3, &view) == 0);
        view.free_blocks_count = 12;
        view.flags = EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT;
        assert(storage_ext4_write_group(volume, 3, &view) == 0);
        assert(storage_ext4_read_group(volume, 3, &view) == 0);
        assert(view.free_blocks_count == 12);
        assert(view.flags == (EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT));
        memcpy(after, gd_raw(3), s.desc_size);
        /* Bytes the view does not carry are preserved (exclude bitmap). */
        assert(!memcmp(before + 0x14, after + 0x14, 4));
        /* The 64-bit address words are rewritten from the view unchanged. */
        assert(!memcmp(before + 0x20, after + 0x20, 12));
        /* bg_checksum matches the verifier. */
        assert(storage_ext4_verify_group_checksum(after, s.desc_size, 3,
                                                  &volume->ext4.super_view) == 0);
    }
    puts("PASS ext4 alloc: group descriptor read/write, cache hit, bounds");
}

static void case_two_flex_and_cross_run(void)
{
    struct img_spec s = {64, 16, 12, 768, 64, 3, FIXTURE_FLEX, 1};
    struct storage_volume *volume;
    struct storage_ext4_group_view v10, v11, v2, v1;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    /* Group 10 keeps only its last two blocks free so no in-group run of
     * six exists; group 11 is fully free (data-only group with flex
     * metadata), giving a contiguous run across the 10|11 boundary. */
    {
        uint8_t keep[] = {62, 63};
        patch_block_bitmap_pattern(10, keep, 2);
        patch_gd_u16(10, 0x0C, 2);
    }
    volume = mount_now();
    assert(volume->ext4.group_count == 12);

    /* Sequential allocation across the two flex groups {0..7} and
     * {8..11} (flex size 8). */
    assert(storage_ext4_alloc_blocks(volume, 100, 4, &first, &allocated) == 0);
    assert(first == 100 && allocated == 4);
    assert(storage_ext4_read_group(volume, 1, &v1) == 0);
    assert(v1.free_blocks_count == 62 - 4);
    assert(storage_ext4_alloc_blocks(volume, 520, 3, &first, &allocated) == 0);
    assert(first == 520 && allocated == 3);
    assert(storage_ext4_read_group(volume, 8, &v2) == 0);
    assert(v2.free_blocks_count == 64 - 3);
    assert(storage_ext4_read_group(volume, 10, &v10) == 0);
    assert(v10.free_blocks_count == 2); /* goal group stayed put */

    /* Contiguous run crossing the group boundary near goal: only group
     * 10's tail (2 blocks) is free there and group 11 is all free, so the
     * six blocks 702..707 straddle the 10|11 boundary. */
    assert(storage_ext4_alloc_blocks(volume, 11 * 64 - 2, 6, &first,
                                     &allocated) == 0);
    assert(first == 11 * 64 - 2 && allocated == 6);
    assert(storage_ext4_read_group(volume, 10, &v10) == 0);
    assert(v10.free_blocks_count == 0);
    assert(storage_ext4_read_group(volume, 11, &v11) == 0);
    assert(v11.free_blocks_count == 64 - 4);
    puts("PASS ext4 alloc: two flex groups, cross-group contiguous run");
}

static void case_goal_vicinity(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first;
    uint32_t allocated;
    uint8_t keep[] = {40, 41, 42, 43, 44, 45, 46, 47};

    load_spec(&s);
    patch_block_bitmap_pattern(1, keep, sizeof(keep));
    patch_gd_u16(1, 0x0C, 8);
    volume = mount_now();

    /* Goal inside the only run: the allocation starts at goal, not at the
     * run start and not at the group start. */
    assert(storage_ext4_alloc_blocks(volume, 64 + 43, 4, &first, &allocated) ==
           0);
    assert(first == 64 + 43 && allocated == 4);
    /* Goal below the run: the run above goal is used from its start. */
    assert(storage_ext4_alloc_blocks(volume, 64 + 20, 3, &first, &allocated) ==
           0);
    assert(first == 64 + 40 && allocated == 3);
    puts("PASS ext4 alloc: goal-vicinity hits (forward and backward)");
}

static void case_fragmented_short_run(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    /* No group holds a run of five; group 3 holds the longest run (3). */
    for (uint32_t g = 0; g < s.groups; ++g) {
        uint8_t keep[8];
        uint32_t n = 0;
        if (g == 3) {
            keep[n++] = 30;
            keep[n++] = 31;
            keep[n++] = 32;
            patch_gd_u16(g, 0x0C, 3);
        } else {
            keep[n++] = 20;
            keep[n++] = 21;
            patch_gd_u16(g, 0x0C, 2);
        }
        patch_block_bitmap_pattern(g, keep, n);
    }
    volume = mount_now();

    assert(storage_ext4_alloc_blocks(volume, 3 * 64 + 30, 5, &first,
                                     &allocated) == 0);
    assert(allocated == 3); /* short run fallback */
    assert(first == 3 * 64 + 30);
    assert(storage_ext4_read_group(volume, 3, &view) == 0);
    assert(view.free_blocks_count == 0);
    puts("PASS ext4 alloc: fragmented fallback to the best shorter run");
}

static void case_boundary_last_group(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    for (uint32_t g = 0; g < s.groups; ++g) {
        if (g == 7) {
            uint8_t keep[] = {45, 46, 47, 48, 49, 50, 51};
            patch_block_bitmap_pattern(g, keep, sizeof(keep));
            patch_gd_u16(g, 0x0C, 7);
        } else {
            patch_fill_bitmap(g, 0, 0xff);
            patch_gd_u16(g, 0x0C, 0);
        }
    }
    volume = mount_now();

    /* Seven free blocks at the end of the filesystem: the run is shorter
     * than requested and never crosses the filesystem end. */
    assert(storage_ext4_alloc_blocks(volume, 7 * 64 + 48, 10, &first,
                                     &allocated) == 0);
    assert(first == 7 * 64 + 45 && allocated == 7);
    assert(first + allocated == 500);

    /* Out-of-range goals and zero counts are refused before any I/O. */
    assert(storage_ext4_alloc_blocks(volume, 500, 1, &first, &allocated) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_alloc_blocks(volume, UINT64_MAX, 1, &first,
                                     &allocated) == -RELIEFOS_EINVAL);
    assert(storage_ext4_alloc_blocks(volume, 0, 0, &first, &allocated) ==
           -RELIEFOS_EINVAL);
    puts("PASS ext4 alloc: boundary runs stop at the filesystem end");
}

static void case_uninit_inline(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    /* Last group (short, past-the-end bits set) with an uninitialized
     * block bitmap: garbage content and a zero bitmap checksum. */
    patch_gd_flags(7, EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT);
    patch_fill_bitmap(7, 0, 0xAA);
    {
        uint8_t *gd = gd_raw(7);
        ext4_put_le16(gd + 0x18, 0);
        ext4_put_le16(gd + 0x38, 0);
    }
    volume = mount_now();

    assert(storage_ext4_read_group(volume, 7, &view) == 0);
    assert((view.flags & EXT4_BG_BLOCK_UNINIT) != 0);
    assert(storage_ext4_alloc_blocks(volume, 7 * 64 + 10, 4, &first,
                                     &allocated) == 0);
    assert(first == 7 * 64 + 10 && allocated == 4);

    /* First touch initialized the bitmap: metadata set, tail past the end
     * of the fs set, garbage gone, flag cleared, free count recomputed
     * (44 after init) minus the four allocated blocks. */
    assert(storage_ext4_read_group(volume, 7, &view) == 0);
    assert((view.flags & EXT4_BG_BLOCK_UNINIT) == 0);
    assert(view.free_blocks_count == 40);
    {
        static uint8_t bm[T_BSIZE];
        uint32_t clear = 0;
        read_block_cached(volume, bbm_block(7), bm);
        for (uint32_t b = 0; b < 64; ++b) {
            if (b < 8 || b >= 52 || (b >= 10 && b < 14))
                assert(bitmap_bit(bm, b) == 1);
            else
                ++clear;
        }
        assert(clear == 40);
        /* Past s_blocks_per_group the bitmap padding is set and the 0xAA
         * garbage is gone. */
        for (uint32_t b = 64; b < T_BSIZE * 8u; b += 61u)
            assert(bitmap_bit(bm, b) == 1);
        assert(bm[0] == 0xffu);
        /* The stored bitmap checksum now matches the content. */
        uint32_t seed = storage_ext4_super_csum_seed(&volume->ext4.super_view);
        uint32_t csum = storage_ext4_crc32c(seed, bm, 64 / 8);
        assert(view.block_bitmap_csum == csum);
    }
    puts("PASS ext4 alloc: uninit block bitmap first-touch initialization");
}

static void case_uninit_super_group(void)
{
    struct img_spec s = {64, 16, 12, 768, 64, 3, FIXTURE_FLEX, 1};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    /* Group 9 carries a superblock backup but its bitmaps/itable live in
     * group 0 (flex layout): initialization only sets its own base
     * metadata. */
    patch_gd_flags(9, EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT);
    patch_fill_bitmap(9, 0, 0x55);
    {
        uint8_t *gd = gd_raw(9);
        ext4_put_le16(gd + 0x18, 0);
        ext4_put_le16(gd + 0x38, 0);
    }
    volume = mount_now();

    assert(storage_ext4_alloc_blocks(volume, 9 * 64 + 20, 3, &first,
                                     &allocated) == 0);
    assert(first == 9 * 64 + 20 && allocated == 3);
    assert(storage_ext4_read_group(volume, 9, &view) == 0);
    assert((view.flags & EXT4_BG_BLOCK_UNINIT) == 0);
    assert(view.free_blocks_count == 62 - 3);
    {
        uint8_t bm[T_BSIZE];
        read_block_cached(volume, bbm_block(9), bm);
        assert(bitmap_bit(bm, 0) && bitmap_bit(bm, 1)); /* super + gd */
        for (uint32_t b = 2; b < 20; ++b)
            assert(bitmap_bit(bm, b) == 0);
    }
    puts("PASS ext4 alloc: uninit bitmap of a super-carrying flex group");
}

static void case_uninit_inode_bitmap(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t ino;

    load_spec(&s);
    /* Group 2 has the largest free-inode count (all free) and an
     * uninitialized inode bitmap; the other candidates give way. */
    for (uint32_t g = 1; g < s.groups; ++g)
        if (g != 2)
            patch_inode_used(g, 34);
    patch_gd_flags(2, EXT4_BG_INODE_ZEROED | EXT4_BG_INODE_UNINIT);
    patch_fill_bitmap(2, 1, 0xAA);
    {
        uint8_t *gd = gd_raw(2);
        ext4_put_le16(gd + 0x1A, 0);
        ext4_put_le16(gd + 0x3A, 0);
    }
    volume = mount_now();

    /* Directory allocation prefers the group with the most free inodes. */
    assert(storage_ext4_alloc_inode(volume, true, &ino) == 0);
    assert(ino == 2 * 64 + 1);
    assert(storage_ext4_read_group(volume, 2, &view) == 0);
    assert((view.flags & EXT4_BG_INODE_UNINIT) == 0);
    assert(view.free_inodes_count == 63);
    assert(view.used_dirs_count == 1);
    /* bg_itable_unused: the whole table was unused, one inode now taken
     * from the front (Linux ext4_mark_inode_used semantics). */
    assert(view.itable_unused == 63);
    {
        uint8_t ibm[T_BSIZE];
        read_block_cached(volume, ibm_block(2), ibm);
        assert(bitmap_bit(ibm, 0) == 1);
        for (uint32_t i = 1; i < 8; ++i)
            assert(bitmap_bit(ibm, i) == 0);
        /* Padding past s_inodes_per_group is set. */
        for (uint32_t i = 64; i < 96; ++i)
            assert(bitmap_bit(ibm, i) == 1);
        uint32_t seed = storage_ext4_super_csum_seed(&volume->ext4.super_view);
        uint32_t csum = storage_ext4_crc32c(seed, ibm, 64 / 8);
        assert(view.inode_bitmap_csum == csum);
    }
    puts("PASS ext4 alloc: uninit inode bitmap first-touch initialization");
}

static void case_double_free(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view, before;
    uint64_t first, ino;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();

    assert(storage_ext4_alloc_blocks(volume, 300, 2, &first, &allocated) == 0);
    assert(storage_ext4_free_blocks(volume, first, 2) == 0);
    assert(storage_ext4_free_blocks(volume, first, 2) == -RELIEFOS_EINVAL);
    /* A range mixing freed and never-allocated blocks is refused whole:
     * nothing in it is freed (validate-then-commit). */
    assert(storage_ext4_read_group(volume, 4, &before) == 0);
    assert(storage_ext4_free_blocks(volume, 4 * 64 + 30, 4) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_read_group(volume, 4, &view) == 0);
    assert(view.free_blocks_count == before.free_blocks_count);
    /* Freed blocks stay free. */
    assert(storage_ext4_alloc_blocks(volume, 4 * 64 + 30, 4, &first,
                                     &allocated) == 0);
    assert(first == 4 * 64 + 30 && allocated == 4);

    /* Bounds. */
    assert(storage_ext4_free_blocks(volume, 499, 2) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_blocks(volume, 0, 1) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_blocks(volume, 10, 0) == -RELIEFOS_EINVAL);

    /* Inodes: double free, reserved inodes and range. */
    assert(storage_ext4_alloc_inode(volume, false, &ino) == 0);
    assert(storage_ext4_free_inode(volume, ino, false) == 0);
    assert(storage_ext4_free_inode(volume, ino, false) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_inode(volume, 10, false) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_inode(volume, 1, true) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_inode(volume, 5, false) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_inode(volume, 513, false) == -RELIEFOS_EINVAL);
    assert(storage_ext4_free_inode(volume, 0, false) == -RELIEFOS_EINVAL);
    puts("PASS ext4 alloc: double free and out-of-range rejected");
}

static void case_bitmap_csum_corrupt(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first, ino;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();
    /* A single flipped bitmap byte must fail the checksum verify with
     * -EIO and mark the volume errored. */
    console_log[0] = 0;
    patch_flip_byte(bbm_block(1), 3);
    assert(storage_ext4_alloc_blocks(volume, 64 + 20, 1, &first, &allocated) ==
           -RELIEFOS_EIO);
    assert(volume->ext4.fs_error == 1);
    assert(strstr(console_log, "bitmap checksum") != NULL);

    load_spec(&s);
    volume = mount_now();
    patch_flip_byte(ibm_block(1), 5); /* directory allocs pick group 1 */
    assert(storage_ext4_alloc_inode(volume, true, &ino) == -RELIEFOS_EIO);
    assert(volume->ext4.fs_error == 1);
    puts("PASS ext4 alloc: bitmap checksum corruption -> -EIO, volume marked");
}

static void case_inode_alloc_free(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t ino, ino2, ino3;

    load_spec(&s);
    volume = mount_now();

    /* Regular inodes take group 0 first (inode 11 = first free bit after
     * the reserved 1-10); directories pick the group with the most free
     * inodes (group 1 with 64). */
    assert(storage_ext4_alloc_inode(volume, false, &ino) == 0);
    assert(ino == 11);
    assert(storage_ext4_alloc_inode(volume, true, &ino2) == 0);
    assert(ino2 == 65);
    assert(storage_ext4_read_group(volume, 1, &view) == 0);
    assert(view.used_dirs_count == 1 && view.free_inodes_count == 63);
    assert(storage_ext4_alloc_inode(volume, false, &ino3) == 0);
    assert(ino3 == 12);

    /* i_links_count != 0 blocks the free. */
    {
        uint8_t slot[T_INODE_SIZE];
        patch_inode_links(volume, 12, 1);
        assert(storage_ext4_free_inode(volume, 12, false) ==
               -RELIEFOS_EINVAL);
        assert(storage_ext4_read_group(volume, 0, &view) == 0);
        assert(view.free_inodes_count == 52); /* unchanged */
        patch_inode_links(volume, 12, 0);
        assert(storage_ext4_free_inode(volume, 12, false) == 0);
        /* The inode table slot was zeroed through the cache. */
        read_block_cached(volume, itable_block(0), ext4_alloc_block_test);
        memcpy(slot, ext4_alloc_block_test + 11 * T_INODE_SIZE, T_INODE_SIZE);
        for (uint32_t i = 0; i < T_INODE_SIZE; ++i)
            assert(slot[i] == 0);
    }

    assert(storage_ext4_free_inode(volume, 11, false) == 0);
    assert(storage_ext4_free_inode(volume, 65, true) == 0);
    assert(storage_ext4_read_group(volume, 1, &view) == 0);
    assert(view.used_dirs_count == 0 && view.free_inodes_count == 64);
    assert(storage_ext4_read_group(volume, 0, &view) == 0);
    assert(view.free_inodes_count == 54);

    /* Freed inodes are reused before the group fills up. */
    assert(storage_ext4_alloc_inode(volume, false, &ino) == 0);
    assert(ino == 11);
    puts("PASS ext4 alloc: inode allocation, used_dirs, links check, reuse");
}

static void case_enospc(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first, ino;
    uint32_t allocated;
    uint64_t total_blocks = 0, total_inodes = 0;

    load_spec(&s);
    volume = mount_now();

    /* Drain every free block: 7 full groups * 56 + 44 = 436.  The goal is
     * the owner hint (its reservation window never hides blocks from it);
     * once it reaches the filesystem end it is clamped back in range. */
    for (;;) {
        uint64_t goal = volume->ext4.next_goal_block;
        int ret;

        if (goal >= 500)
            goal = 499;
        ret = storage_ext4_alloc_blocks(volume, goal, 1, &first, &allocated);
        if (ret == -RELIEFOS_ENOSPC)
            break;
        assert(ret == 0 && allocated == 1);
        ++total_blocks;
        assert(total_blocks < 1000);
    }
    assert(total_blocks == 436);

    /* Drain every allocatable inode: 512 - 10 reserved. */
    for (;;) {
        int ret = storage_ext4_alloc_inode(volume, false, &ino);
        if (ret == -RELIEFOS_ENOSPC)
            break;
        assert(ret == 0 && ino >= EXT4_GOOD_OLD_FIRST_INO);
        ++total_inodes;
        assert(total_inodes < 1000);
    }
    assert(total_inodes == 502);
    puts("PASS ext4 alloc: ENOSPC after draining blocks and inodes");
}

static void case_window(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();

    assert(storage_ext4_alloc_blocks(volume, 0, 1, &first, &allocated) == 0);
    assert(first == 8);
    assert(volume->ext4.next_goal_block == 9);
    assert(volume->ext4.reserved_window_end ==
           9 + EXT4_RESERVATION_WINDOW_BLOCKS);

    /* A foreign goal must not step into the reservation window. */
    assert(storage_ext4_alloc_blocks(volume, 0, 2, &first, &allocated) == 0);
    assert(first == 9 + EXT4_RESERVATION_WINDOW_BLOCKS && allocated == 2);

    /* Once the window has moved on, the earlier windowed blocks are
     * available again (no leak). */
    assert(storage_ext4_alloc_blocks(volume, 0, 1, &first, &allocated) == 0);
    assert(first == 9);

    /* The window owner (goal at the window head) allocates from the goal. */
    assert(storage_ext4_alloc_blocks(volume, volume->ext4.next_goal_block, 3,
                                     &first, &allocated) == 0);
    assert(first == 10 && allocated == 3);

    /* Freeing blocks releases the reservation. */
    assert(storage_ext4_free_blocks(volume, 9, 1) == 0);
    assert(volume->ext4.reserved_window_end == 0);
    puts("PASS ext4 alloc: reservation window around next_goal_block");
}

static void case_super_counts(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();
    assert(storage_ext4_alloc_blocks(volume, 10, 5, &first, &allocated) == 0);
    assert(storage_ext4_alloc_inode(volume, false, &first) == 0);
    assert(storage_ext4_free_blocks(volume, 10, 2) == 0);
    assert(storage_ext4_cache_flush(volume) == 0);

    /* Superblock free counts follow the allocations and its checksum is
     * fresh. */
    {
        uint8_t *sb = disk_bytes + 1024;
        assert(ext4_get_le32(sb + 0x0C) == 436 - 3);
        assert(ext4_get_le32(sb + 0x10) == 502 - 1);
        assert(storage_ext4_verify_super_checksum(sb, EXT4_SUPERBLOCK_SIZE) ==
               0);
    }
    /* Group descriptor checksums survive the write-back too. */
    for (uint32_t g = 0; g < s.groups; ++g) {
        assert(storage_ext4_verify_group_checksum(gd_raw(g), s.desc_size, g,
                                                  &volume->ext4.super_view) ==
               0);
    }
    puts("PASS ext4 alloc: superblock/group free counts stay consistent");
}

static void case_flex_log_clamp(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;

    load_spec(&s);
    disk_bytes[1024 + T_SB_FLEX_LOG] = 9;
    image_resync();
    console_log[0] = 0;
    volume = mount_now();
    assert(volume->ext4.flex_log_groups == EXT4_MAX_FLEX_LOG);
    assert(strstr(console_log, "s_log_groups_per_flex") != NULL);
    puts("PASS ext4 alloc: s_log_groups_per_flex clamped to EXT4_MAX_FLEX_LOG");
}

static void case_desc_straddle(void)
{
    struct img_spec s = {16, 16, 64, 1024, 96, 0, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();
    assert(volume->ext4.group_count == 64);
    assert(volume->ext4.desc_size == 96);
    /* Group 42's descriptor starts 64 bytes before the end of the GDT's
     * first block and straddles into the next one. */
    assert(42u * 96u % T_BSIZE == 4032);
    assert(storage_ext4_read_group(volume, 42, &view) == 0);
    assert(view.block_bitmap == 42u * 16u + 3u);
    assert(view.free_blocks_count == 10);
    /* A straddled descriptor commit pins the bitmap plus both descriptor
     * blocks at once; that needs a cache that can hold three entries. */
    if (EXT4_BLOCK_CACHE_ENTRIES >= 8) {
        assert(storage_ext4_alloc_blocks(volume, 42u * 16u + 8u, 2, &first,
                                         &allocated) == 0);
        assert(first == 42u * 16u + 8u && allocated == 2);
        assert(storage_ext4_read_group(volume, 42, &view) == 0);
        assert(view.free_blocks_count == 8);
    }
    /* The straddled descriptor was written back consistently. */
    assert(storage_ext4_verify_group_checksum(gd_raw(42), s.desc_size, 42,
                                              &volume->ext4.super_view) == 0);
    assert(storage_ext4_write_group(volume, 42, &view) == 0);
    assert(storage_ext4_read_group(volume, 42, &view) == 0);
    assert(view.free_blocks_count ==
           (EXT4_BLOCK_CACHE_ENTRIES >= 8 ? 8u : 10u));
    puts("PASS ext4 alloc: straddling group descriptors read/write");
}

static void case_group_cache_small(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t reads;

    if (EXT4_GROUP_CACHE_ENTRIES > 4) {
        puts("SKIP ext4 alloc: group-cache-small (build with "
             "-DEXT4_GROUP_CACHE_ENTRIES=2)");
        return;
    }
    load_spec(&s);
    volume = mount_now();

    /* LRU at capacity 2: the third group evicts the oldest summary.  The
     * block cache is dropped in between so a summary hit is observable as
     * "no device read". */
    assert(storage_ext4_read_group(volume, 0, &view) == 0);
    assert(storage_ext4_read_group(volume, 1, &view) == 0);
    assert(storage_ext4_read_group(volume, 2, &view) == 0); /* evicts 0 */
    storage_ext4_cache_invalidate(volume);
    reads = test_read_commands;
    assert(storage_ext4_read_group(volume, 1, &view) == 0);
    assert(test_read_commands == reads); /* summary hit */
    assert(storage_ext4_read_group(volume, 0, &view) == 0);
    assert(test_read_commands == reads + 1); /* evicted: refetched */

    /* Pin discipline: rejected frees must not leave write pins behind.
     * With a 2-entry cache three rejected free_inode calls would exhaust
     * it and turn the next read into -RELIEFOS_ENOMEM. */
    storage_ext4_cache_invalidate(volume);
    for (uint32_t g = 0; g < 3; ++g) {
        assert(storage_ext4_free_inode(volume, g * 64 + 11, false) ==
               -RELIEFOS_EINVAL);
    }
    assert(storage_ext4_free_blocks(volume, 4 * 64 + 30, 3) ==
           -RELIEFOS_EINVAL); /* never allocated: also rejected cleanly */
    {
        uint8_t buf[T_BSIZE];
        assert(storage_ext4_cache_read(volume, 300, buf) == 0);
    }

    /* write_group refreshes the cached summary: dropping the dirty block
     * cache afterwards still serves the new value. */
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_read_group(volume, 3, &view) == 0);
    view.free_blocks_count = 5;
    assert(storage_ext4_write_group(volume, 3, &view) == 0);
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_read_group(volume, 3, &view) == 0);
    assert(view.free_blocks_count == 5);
    puts("PASS ext4 alloc (small group cache): LRU order and refresh");
}

static void case_sparse_super2_backup(void)
{
    struct img_spec s = {64, 16, 6, 384, 64, 3, FIXTURE_FLEX, 0, 1, 0, {1, 3}};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    /* Groups 1 and 3 carry the superblock/GDT backups (sparse_super2),
     * groups 2/4/5 carry none; both kinds get an uninitialized bitmap. */
    patch_gd_flags(1, EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT);
    patch_fill_bitmap(1, 0, 0xA5);
    patch_gd_flags(2, EXT4_BG_INODE_ZEROED | EXT4_BG_BLOCK_UNINIT);
    patch_fill_bitmap(2, 0, 0x5A);
    volume = mount_now();

    /* The backup-group words must come from the right superblock slot. */
    assert(volume->ext4.backup_bgs[0] == 1 && volume->ext4.backup_bgs[1] == 3);

    /* Backup group 1: first-touch initialization marks its super/GDT
     * backups used and the allocator must never hand them out. */
    assert(storage_ext4_alloc_blocks(volume, 64, 3, &first, &allocated) == 0);
    assert(first == 64 + 2 && allocated == 3);
    {
        uint8_t bm[T_BSIZE];
        read_block_cached(volume, bbm_block(1), bm);
        assert(bitmap_bit(bm, 0) && bitmap_bit(bm, 1));
    }

    /* Data-only group 2: without a backup its first block is plain data. */
    assert(storage_ext4_alloc_blocks(volume, 128, 2, &first, &allocated) == 0);
    assert(first == 128 && allocated == 2);
    assert(storage_ext4_read_group(volume, 2, &view) == 0);
    assert(view.free_blocks_count == 62);
    puts("PASS ext4 alloc: sparse_super2 backup groups keep their metadata");
}

static void case_huge_group_boundary(void)
{
    static uint8_t gd[EXT4_MAX_DESC_SIZE];
    struct storage_volume *volume = &g_volumes[0];
    uint64_t huge_first = 1ull << 32;

    /* Hand-built volume whose group ends pass 2^32 blocks (group 131072
     * starts at 2^32): the free path must walk 64-bit boundaries and
     * reject promptly instead of stalling on a group end truncated to 32
     * bits.  No checksum features: nothing else is consulted. */
    free(disk_bytes);
    disk_size = 2560ull * T_BSIZE;
    disk_bytes = calloc(1, (size_t)disk_size);
    assert(disk_bytes);
    fixture_synthetic = 0;
    storage_memzero(volume, sizeof(*volume));
    volume->mount_generation = 0x7000; /* distinct from every mounted case */
    volume->kind = STORAGE_VOLUME_RAM;
    volume->ext_sector_count = disk_size / SECTOR_SIZE;
    volume->ext4.blocks_count = (1ull << 32) + 40000;
    volume->ext4.group_count = 131073;
    volume->ext4.block_size = T_BSIZE;
    volume->ext4.blocks_per_group = 32768;
    volume->ext4.inodes_per_group = 64;
    volume->ext4.inodes_count = 64;
    volume->ext4.inode_size = T_INODE_SIZE;
    volume->ext4.first_data_block = 0;
    volume->ext4.desc_size = 64;
    g_active_volume = volume;

    /* Descriptor of group 131072 lives at gd block 2049; its block
     * bitmap (block 2100) is fully set. */
    memset(gd, 0, sizeof(gd));
    ext4_put_le32(gd + 0x00, 2100);
    ext4_put_le32(gd + 0x04, 2101);
    ext4_put_le32(gd + 0x08, 2102);
    ext4_put_le16(gd + 0x0C, 30000);
    ext4_put_le16(gd + 0x0E, 64);
    ext4_put_le16(gd + 0x1C, 64);
    memcpy(disk_bytes + 2049ull * T_BSIZE, gd, sizeof(gd));
    memset(disk_bytes + 2100ull * T_BSIZE, 0xff, T_BSIZE);

    assert(storage_ext4_free_blocks(volume, huge_first, 1) ==
           -RELIEFOS_EINVAL);
    puts("PASS ext4 alloc: 64-bit group boundaries do not stall the free path");
}

static void case_commit_rollback(void)
{
    struct img_spec s = {64, 16, 12, 768, 64, 3, FIXTURE_FLEX, 1, 0, 0, {0, 0}};
    struct storage_volume *volume;
    struct storage_ext4_group_view v10, v11;
    uint64_t first;
    uint32_t allocated;
    uint64_t super_before;

    load_spec(&s);
    {
        uint8_t keep[] = {62, 63};
        patch_block_bitmap_pattern(10, keep, 2);
        patch_gd_u16(10, 0x0C, 2);
    }
    /* Group 11 lies about its free count: the second chunk of the
     * cross-group run must fail without drifting any counter. */
    patch_gd_u16(11, 0x0C, 1);
    volume = mount_now();
    super_before = volume->ext4.super_view.free_blocks_count;
    assert(super_before == 720);

    assert(storage_ext4_alloc_blocks(volume, 11 * 64 - 2, 6, &first,
                                     &allocated) == -RELIEFOS_EIO);
    assert(volume->ext4.fs_error == 1);
    /* Super and group counts are exactly where they started. */
    assert(volume->ext4.super_view.free_blocks_count == super_before);
    assert(storage_ext4_read_group(volume, 10, &v10) == 0);
    assert(v10.free_blocks_count == 2);
    assert(storage_ext4_read_group(volume, 11, &v11) == 0);
    assert(v11.free_blocks_count == 1);
    /* Group 11's bitmap was never touched: the count guard fires before
     * any bit is set. */
    {
        uint8_t bm[T_BSIZE];
        read_block_cached(volume, bbm_block(11), bm);
        assert(bitmap_bit(bm, 0) == 0);
    }
    /* The rolled-back run is available again. */
    assert(storage_ext4_alloc_blocks(volume, 11 * 64 - 2, 2, &first,
                                     &allocated) == -RELIEFOS_EROFS);
    assert(storage_ext4_cache_flush(volume) == 0);
    assert(storage_ext4_mount(volume) == 0);
    assert(storage_ext4_alloc_blocks(volume, 11 * 64 - 2, 2, &first,
                                     &allocated) == 0);
    assert(first == 11 * 64 - 2 && allocated == 2);
    assert(storage_ext4_read_group(volume, 10, &v10) == 0);
    assert(v10.free_blocks_count == 0);
    puts("PASS ext4 alloc: failed multi-chunk commit leaves no count drift");
}

static void case_gdt_csum_only(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0, 0, 1, {0, 0}};
    struct storage_volume *volume;
    uint8_t before[8];
    uint64_t first;
    uint32_t allocated;

    load_spec(&s);
    volume = mount_now();
    /* Without metadata_csum the descriptor bitmap-csum words are not
     * ours to touch: they must survive an allocation unchanged. */
    memcpy(before, gd_raw(1) + 0x18, 4);
    memcpy(before + 4, gd_raw(1) + 0x38, 4);
    assert(storage_ext4_alloc_blocks(volume, 64 + 10, 2, &first, &allocated) ==
           0);
    assert(first == 64 + 10 && allocated == 2);
    assert(storage_ext4_cache_flush(volume) == 0);
    assert(memcmp(before, gd_raw(1) + 0x18, 4) == 0);
    assert(memcmp(before + 4, gd_raw(1) + 0x38, 4) == 0);
    puts("PASS ext4 alloc: gdt_csum-only volumes keep the bitmap csum words");
}

static void case_pin_discipline(void)
{
    struct img_spec s = {64, 64, 8, 500, 64, 2, FIXTURE_INLINE, 0, 0, 0, {0, 0}};
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint64_t ino;
    uint8_t buf[T_BSIZE];

    /* The pins live in the block cache (EXT4_BLOCK_CACHE_ENTRIES): the
     * leak is only observable when that cache is tiny. */
    if (EXT4_BLOCK_CACHE_ENTRIES > 4) {
        puts("SKIP ext4 alloc: pin discipline (build with "
             "-DEXT4_BLOCK_CACHE_ENTRIES=2)");
        return;
    }

    /* (a) per-group ENOSPC retries must not stack write pins. */
    load_spec(&s);
    for (uint32_t g = 0; g < 8; ++g) {
        patch_inode_used(g, 64);
        patch_gd_u16(g, 0x0E, 64); /* stale count keeps every group a candidate */
    }
    volume = mount_now();
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_alloc_inode(volume, false, &ino) == -RELIEFOS_ENOSPC);
    assert(storage_ext4_cache_read(volume, 300, buf) == 0);

    /* (b) checksum failures on a write-pin path release the pin. */
    load_spec(&s);
    volume = mount_now();
    patch_flip_byte(ibm_block(0), 5);
    patch_flip_byte(ibm_block(1), 5);
    storage_ext4_cache_invalidate(volume);
    assert(storage_ext4_alloc_inode(volume, false, &ino) == -RELIEFOS_EIO);
    assert(storage_ext4_alloc_inode(volume, true, &ino) == -RELIEFOS_EROFS);
    assert(storage_ext4_cache_read(volume, 300, buf) == 0);

    /* (c) a failing straddling write_group releases both block pins. */
    {
        struct img_spec cs = {16, 16, 64, 1024, 96, 0,
                              FIXTURE_INLINE, 0, 0, 0, {0, 0}};
        uint8_t *held;

        load_spec(&cs);
        volume = mount_now();
        /* Occupy the last cache slot so the second descriptor block of
         * the straddled write cannot be pinned. */
        assert(storage_ext4_cache_get(volume, 100, &held, true) == 0);
        assert(storage_ext4_read_group(volume, 42, &view) == 0);
        assert(storage_ext4_write_group(volume, 42, &view) ==
               -RELIEFOS_ENOMEM);
        assert(storage_ext4_cache_read(volume, 300, buf) == 0);
    }
    puts("PASS ext4 alloc: rejected operations release their cache pins");
}

/* ---- real image cross-check -------------------------------------------- */
static int run_capture(const char *cmd, char *out, size_t cap)
{
    FILE *pipe = popen(cmd, "r");
    size_t n;
    int status;

    assert(pipe);
    n = fread(out, 1, cap - 1, pipe);
    out[n] = 0;
    status = pclose(pipe);
    assert(status >= 0 && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void load_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;

    fixture_synthetic = 0;

    assert(file && !fseek(file, 0, SEEK_END));
    size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    free(disk_bytes);
    disk_size = (uint64_t)size;
    disk_bytes = malloc((size_t)size);
    assert(disk_bytes && fread(disk_bytes, 1, (size_t)size, file) ==
                             (size_t)size);
    assert(!fclose(file));
}

static void save_file(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(disk_bytes, 1, (size_t)disk_size, file) == (size_t)disk_size);
    assert(!fclose(file));
}

/* Parse "Group N:" headers and the "%u free blocks, %u free inodes" line
 * of LANG=C dumpe2fs output. */
static void parse_dumpe2fs(const char *text, uint32_t *free_blocks,
                           uint32_t *free_inodes, uint32_t groups)
{
    const char *p = text;
    uint32_t current = UINT32_MAX;

    while ((p = strchr(p, '\n')) != NULL) {
        ++p;
        if (strncmp(p, "Group ", 6) == 0) {
            unsigned g = 0;
            if (sscanf(p, "Group %u:", &g) == 1)
                current = g;
        } else if (current < groups && strstr(p, "free blocks, ") != NULL) {
            unsigned blocks = 0, inodes = 0;
            if (sscanf(p, "%u free blocks, %u free inodes", &blocks, &inodes) ==
                2) {
                free_blocks[current] = blocks;
                free_inodes[current] = inodes;
            }
        }
    }
}

static void case_real_sparse_super2(void)
{
    char dir[] = "build/ext4-ss2-XXXXXX";
    char img[256], cmd[1024], *out;
    struct storage_volume *volume;
    unsigned backup0 = 0, backup1 = 0;

    assert(mkdir("build", 0777) == 0 || errno == EEXIST);
    assert(mkdtemp(dir));
    snprintf(img, sizeof(img), "%s/ss2.ext4", dir);
    {
        FILE *file = fopen(img, "wb");
        assert(file);
        assert(!fclose(file));
        assert(truncate(img, 32 * 1024 * 1024) == 0);
    }
    out = malloc(1 << 20);
    assert(out);
    snprintf(cmd, sizeof(cmd),
             "mke2fs -q -t ext4 -F -b 4096 -I 256 -g 1024 "
             "-O none,filetype,extents,64bit,flex_bg,metadata_csum,uninit_bg,"
             "extra_isize,dir_nlink,huge_file,sparse_super2 -m 0 %s",
             img);
    assert(run_capture(cmd, out, 1 << 20) == 0);
    load_file(img);
    volume = mount_now();

    /* Independent oracle: dumpe2fs reports the same backup-group words
     * from the superblock, so a wrong offset constant cannot hide. */
    snprintf(cmd, sizeof(cmd), "LANG=C dumpe2fs %s", img);
    assert(run_capture(cmd, out, 1 << 20) == 0);
    {
        const char *line = strstr(out, "Backup block groups:");
        assert(line != NULL);
        assert(sscanf(line, "Backup block groups: %u %u", &backup0, &backup1) >=
               1);
    }
    assert(volume->ext4.backup_bgs[0] == backup0);
    assert(volume->ext4.backup_bgs[1] == backup1);

    /* Behavioral half: a backup group's super/GDT blocks survive the
     * uninit first touch. */
    if (backup0 != 0) {
        struct storage_ext4_group_view view;
        uint64_t first;
        uint32_t allocated;

        assert(storage_ext4_read_group(volume, backup0, &view) == 0);
        if ((view.flags & EXT4_BG_BLOCK_UNINIT) != 0) {
            assert(storage_ext4_alloc_blocks(volume, (uint64_t)backup0 * 1024,
                                             3, &first, &allocated) == 0);
            assert(first == (uint64_t)backup0 * 1024 + 2 && allocated == 3);
        }
    }
    free(out);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    assert(system(cmd) == 0);
    puts("PASS ext4 alloc: real sparse_super2 image backup groups verified");
}

static void case_real_image(void)
{
    char dir[] = "build/ext4-alloc-XXXXXX";
    char img[256], cmd[1024], *out;
    struct storage_volume *volume;
    struct storage_ext4_group_view view;
    uint32_t free_blocks[64], free_inodes[64];
    uint64_t first, ino;
    uint32_t allocated;

    assert(mkdir("build", 0777) == 0 || errno == EEXIST);
    assert(mkdtemp(dir));
    snprintf(img, sizeof(img), "%s/real.ext4", dir);
    {
        FILE *file = fopen(img, "wb");
        assert(file);
        assert(!fclose(file));
        assert(truncate(img, 16 * 1024 * 1024) == 0);
    }
    out = malloc(1 << 20);
    assert(out);
    snprintf(cmd, sizeof(cmd),
             "mke2fs -q -t ext4 -F -b 4096 -I 256 -g 1024 "
             "-O none,filetype,extents,64bit,flex_bg,metadata_csum,uninit_bg,"
             "extra_isize,dir_nlink,huge_file,sparse_super -m 0 %s",
             img);
    assert(run_capture(cmd, out, 1 << 20) == 0);
    load_file(img);
    volume = mount_now();
    assert(volume->ext4.group_count == 4);
    snprintf(cmd, sizeof(cmd), "LANG=C dumpe2fs %s", img);
    assert(run_capture(cmd, out, 1 << 20) == 0);
    memset(free_blocks, 0, sizeof(free_blocks));
    memset(free_inodes, 0, sizeof(free_inodes));
    parse_dumpe2fs(out, free_blocks, free_inodes, 64);

    for (uint32_t g = 0; g < volume->ext4.group_count; ++g) {
        assert(storage_ext4_read_group(volume, g, &view) == 0);
        assert(view.free_blocks_count == free_blocks[g]);
        assert(view.free_inodes_count == free_inodes[g]);

        /* The bitmap checksums stored by mke2fs verify with our
         * primitives (bitmaps of uninit groups are skipped, matching
         * linux/fs/ext4/bitmap.c callers). */
        {
            uint8_t bm[EXT4_MAX_BLOCK_SIZE];
            uint32_t seed =
                storage_ext4_super_csum_seed(&volume->ext4.super_view);
            assert(storage_ext4_cache_read(volume, view.block_bitmap, bm) == 0);
            if ((view.flags & EXT4_BG_BLOCK_UNINIT) != 0) {
                assert(view.block_bitmap_csum == 0);
            } else {
                assert(storage_ext4_crc32c(seed, bm, 1024 / 8) ==
                       view.block_bitmap_csum);
            }
            assert(storage_ext4_cache_read(volume, view.inode_bitmap, bm) == 0);
            if ((view.flags & EXT4_BG_INODE_UNINIT) != 0) {
                assert(view.inode_bitmap_csum == 0);
            } else {
                assert(storage_ext4_crc32c(seed, bm, 1024 / 8) ==
                       view.inode_bitmap_csum);
            }
        }
    }

    /* The unmodified image must be clean for e2fsprogs before we touch
     * it. */
    save_file(img);
    snprintf(cmd, sizeof(cmd), "e2fsck -f -n %s", img);
    assert(run_capture(cmd, out, 1 << 20) == 0);

    /* Allocate through the uninit group (group 2 with sparse_super has no
     * superblock backup and is BLOCK_UNINIT on this image). */
    assert(storage_ext4_read_group(volume, 2, &view) == 0);
    if ((view.flags & EXT4_BG_BLOCK_UNINIT) != 0) {
        uint32_t was_free = view.free_blocks_count;
        assert(storage_ext4_alloc_blocks(volume, 2 * 1024 + 10, 4, &first,
                                         &allocated) == 0);
        assert(first == 2 * 1024 + 10 && allocated == 4);
        assert(storage_ext4_read_group(volume, 2, &view) == 0);
        assert((view.flags & EXT4_BG_BLOCK_UNINIT) == 0);
        assert(view.free_blocks_count == was_free - 4);
    }
    assert(storage_ext4_alloc_inode(volume, true, &ino) == 0);
    assert(ino >= EXT4_GOOD_OLD_FIRST_INO);
    assert(storage_ext4_free_inode(volume, ino, true) == 0);
    assert(storage_ext4_cache_flush(volume) == 0);
    save_file(img);

    /* The modified image may only report the four allocated blocks as
     * unattached (they belong to no inode until the write path of a later
     * task links them; everything else - counts, bitmaps, checksums -
     * must agree). */
    snprintf(cmd, sizeof(cmd), "LANG=C dumpe2fs %s", img);
    assert(run_capture(cmd, out, 1 << 20) == 0);
    memset(free_blocks, 0, sizeof(free_blocks));
    memset(free_inodes, 0, sizeof(free_inodes));
    parse_dumpe2fs(out, free_blocks, free_inodes, 64);
    for (uint32_t g = 0; g < volume->ext4.group_count; ++g) {
        assert(storage_ext4_read_group(volume, g, &view) == 0);
        assert(view.free_blocks_count == free_blocks[g]);
        assert(view.free_inodes_count == free_inodes[g]);
    }

    snprintf(cmd, sizeof(cmd), "LANG=C e2fsck -f -n %s", img);
    {
        int rc = run_capture(cmd, out, 1 << 20);
        assert(rc == 4); /* errors left uncorrected: our own allocation */
        assert(strstr(out, "Block bitmap differences") != NULL);
        assert(strstr(out, "2058") != NULL && strstr(out, "2061") != NULL);
        assert(strstr(out, "Inode bitmap differences") == NULL);
        assert(strstr(out, "count wrong") == NULL);
        assert(strstr(out, "checksum") == NULL);
    }
    free(out);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    assert(system(cmd) == 0);
    puts("PASS ext4 alloc: real mke2fs image verified against dumpe2fs/e2fsck");
}

int main(void)
{
    case_read_group();
    case_two_flex_and_cross_run();
    case_goal_vicinity();
    case_fragmented_short_run();
    case_boundary_last_group();
    case_uninit_inline();
    case_uninit_super_group();
    case_uninit_inode_bitmap();
    case_double_free();
    case_bitmap_csum_corrupt();
    case_inode_alloc_free();
    case_enospc();
    case_window();
    case_super_counts();
    case_flex_log_clamp();
    case_desc_straddle();
    case_sparse_super2_backup();
    case_huge_group_boundary();
    case_commit_rollback();
    case_gdt_csum_only();
    case_pin_discipline();
    case_group_cache_small();
    case_real_sparse_super2();
    case_real_image();
    puts("PASS ext4 alloc: all cases");
    return 0;
}
