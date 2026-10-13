/* Host-side tests for the ext4 on-disk format layer.
 *
 * Covers the endian helpers, the bounds checker, and the superblock /
 * group-descriptor / extent-header parsers in
 * kernel/reliefnt/fs/storage_ext4_format.c.
 * Fixtures are hand-built raw byte images following the field offsets of
 * linux/fs/ext4/ext4.h (Linux v7.3-rc5 reference tree).
 *
 * Build (task 2 acceptance command):
 *   cc -std=c11 -O0 -g -fsanitize=address,undefined \
 *     -Ikernel/reliefnt/include -Iinclude \
 *     -Ikernel/reliefnt/include/uapi -Ikernel/reliefnt/kernel/reliefnt/include \
 *     tools/tests/ext4_format_test.c \
 *     kernel/reliefnt/fs/storage_ext4_format.c \
 *     -o /tmp/reliefos-ext4-format-test
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/block/storage_internal.h"

/* ext4_super_block offsets (linux/fs/ext4/ext4.h:1369). */
#define SB_INODES_COUNT 0x00
#define SB_BLOCKS_COUNT_LO 0x04
#define SB_R_BLOCKS_COUNT_LO 0x08
#define SB_FREE_BLOCKS_COUNT_LO 0x0C
#define SB_FREE_INODES_COUNT 0x10
#define SB_FIRST_DATA_BLOCK 0x14
#define SB_LOG_BLOCK_SIZE 0x18
#define SB_LOG_CLUSTER_SIZE 0x1C
#define SB_BLOCKS_PER_GROUP 0x20
#define SB_CLUSTERS_PER_GROUP 0x24
#define SB_INODES_PER_GROUP 0x28
#define SB_MAGIC 0x38
#define SB_STATE 0x3A
#define SB_CREATOR_OS 0x48
#define SB_REV_LEVEL 0x4C
#define SB_FIRST_INO 0x54
#define SB_INODE_SIZE 0x58
#define SB_FEATURE_COMPAT 0x5C
#define SB_FEATURE_INCOMPAT 0x60
#define SB_FEATURE_RO_COMPAT 0x64
#define SB_UUID 0x68
#define SB_VOLUME_NAME 0x78
#define SB_JOURNAL_INUM 0xE0
#define SB_DESC_SIZE 0xFE
#define SB_BLOCKS_COUNT_HI 0x150
#define SB_R_BLOCKS_COUNT_HI 0x154
#define SB_FREE_BLOCKS_COUNT_HI 0x158

/* ext4_group_desc offsets (linux/fs/ext4/ext4.h:402). */
#define GD_BLOCK_BITMAP_LO 0x00
#define GD_INODE_BITMAP_LO 0x04
#define GD_INODE_TABLE_LO 0x08
#define GD_FREE_BLOCKS_LO 0x0C
#define GD_FREE_INODES_LO 0x0E
#define GD_USED_DIRS_LO 0x10
#define GD_FLAGS 0x12
#define GD_ITABLE_UNUSED_LO 0x1C
#define GD_CHECKSUM 0x1E
#define GD_BLOCK_BITMAP_HI 0x20
#define GD_INODE_BITMAP_HI 0x24
#define GD_INODE_TABLE_HI 0x28
#define GD_FREE_BLOCKS_HI 0x2C
#define GD_FREE_INODES_HI 0x2E
#define GD_USED_DIRS_HI 0x30
#define GD_ITABLE_UNUSED_HI 0x32

/* ext4_extent_header offsets (linux/fs/ext4/ext4_extents.h:78). */
#define EH_MAGIC 0x00
#define EH_ENTRIES 0x02
#define EH_MAX 0x04
#define EH_DEPTH 0x06
#define EH_GENERATION 0x08

static uint8_t raw[2048];

/* Fill a valid dynamic-rev superblock.  1 KiB filesystems put the superblock
 * in block 1 (s_first_data_block = 1); larger blocks start at 0. */
static void super_base(uint32_t log_block_size, uint16_t inode_size)
{
    memset(raw, 0, sizeof(raw));
    ext4_put_le32(raw + SB_INODES_COUNT, 2048);
    ext4_put_le32(raw + SB_BLOCKS_COUNT_LO, 8192);
    ext4_put_le32(raw + SB_R_BLOCKS_COUNT_LO, 400);
    ext4_put_le32(raw + SB_FREE_BLOCKS_COUNT_LO, 7000);
    ext4_put_le32(raw + SB_FREE_INODES_COUNT, 2000);
    ext4_put_le32(raw + SB_FIRST_DATA_BLOCK, log_block_size == 0 ? 1 : 0);
    ext4_put_le32(raw + SB_LOG_BLOCK_SIZE, log_block_size);
    ext4_put_le32(raw + SB_LOG_CLUSTER_SIZE, log_block_size);
    ext4_put_le32(raw + SB_BLOCKS_PER_GROUP, 8192);
    ext4_put_le32(raw + SB_CLUSTERS_PER_GROUP, 8192);
    ext4_put_le32(raw + SB_INODES_PER_GROUP, 2048);
    ext4_put_le16(raw + SB_MAGIC, EXT4_SUPER_MAGIC);
    ext4_put_le32(raw + SB_REV_LEVEL, 1);
    ext4_put_le32(raw + SB_FIRST_INO, 11);
    ext4_put_le16(raw + SB_INODE_SIZE, inode_size);
    ext4_put_le32(raw + SB_JOURNAL_INUM, 8);
}

static void test_endian_helpers(void)
{
    uint8_t buf[8];

    ext4_put_le16(buf, 0x1234);
    assert(buf[0] == 0x34 && buf[1] == 0x12);
    assert(ext4_get_le16(buf) == 0x1234);

    ext4_put_le32(buf, 0x89abcdefu);
    assert(buf[0] == 0xef && buf[1] == 0xcd && buf[2] == 0xab && buf[3] == 0x89);
    assert(ext4_get_le32(buf) == 0x89abcdefu);

    ext4_put_le64(buf, 0x0123456789abcdefULL);
    assert(buf[0] == 0xef && buf[3] == 0x89 && buf[4] == 0x67 && buf[7] == 0x01);
    assert(ext4_get_le64(buf) == 0x0123456789abcdefULL);

    /* Field-style reads against an offset image. */
    memset(buf, 0, sizeof(buf));
    ext4_put_le32(buf + 2, 0x11223344u);
    assert(ext4_get_le32(buf + 2) == 0x11223344u);
    puts("PASS endian helpers");
}

static void test_range_ok(void)
{
    /* In range, exact fit and interior spans. */
    assert(ext4_range_ok(1024, 0, 1024) == 0);
    assert(ext4_range_ok(1024, 1023, 1) == 0);
    assert(ext4_range_ok(1024, 512, 512) == 0);
    assert(ext4_range_ok(2048, 1024, 1024) == 0);

    /* Out of range -> -EINVAL (越界引用). */
    assert(ext4_range_ok(1024, 1024, 1) == -RELIEFOS_EINVAL);
    assert(ext4_range_ok(1024, 0, 1025) == -RELIEFOS_EINVAL);
    assert(ext4_range_ok(1024, 1023, 2) == -RELIEFOS_EINVAL);
    assert(ext4_range_ok(0, 0, 1) == -RELIEFOS_EINVAL);
    assert(ext4_range_ok(0, 0, 0) == 0);

    /* offset + size arithmetic overflow -> -EOVERFLOW. */
    assert(ext4_range_ok(1024, UINT64_MAX, 8) == -RELIEFOS_EOVERFLOW);
    assert(ext4_range_ok(1024, UINT64_MAX - 3, 4) == -RELIEFOS_EOVERFLOW);
    assert(ext4_range_ok(1024, UINT64_MAX, 1) == -RELIEFOS_EOVERFLOW);
    puts("PASS range_ok bounds");
}

static void test_errno_mapping(void)
{
    /* Host errno values must equal the RELIEFOS_* Linux ABI values. */
    assert(RELIEFOS_EINVAL == EINVAL);
    assert(RELIEFOS_EOVERFLOW == EOVERFLOW);
    puts("PASS errno mapping");
}

static void test_constants(void)
{
    assert(EXT4_SUPERBLOCK_OFFSET == 1024u);
    assert(EXT4_SUPER_MAGIC == 0xef53u);
    assert(EXT4_EXT_MAGIC == 0xf30au);
    assert(EXT4_MIN_BLOCK_SIZE == 1024u);
    assert(EXT4_MAX_BLOCK_SIZE == 4096u);
    assert(EXT4_GOOD_OLD_INODE_SIZE == 128u);
    assert(EXT4_MIN_DESC_SIZE == 32u);
    assert(EXT4_MAX_DESC_SIZE == 256u);
    assert(EXT4_MAX_EXTENT_DEPTH == 5u);
    assert(EXT4_NAME_LEN == 255u);

    /* Feature masks, per linux/fs/ext4/ext4.h. */
    assert(EXT4_FEATURE_COMPAT_HAS_JOURNAL == 0x0004u);
    assert(EXT4_FEATURE_COMPAT_DIR_INDEX == 0x0020u);
    assert(EXT4_FEATURE_COMPAT_SPARSE_SUPER2 == 0x0200u);
    assert(EXT4_FEATURE_INCOMPAT_FILETYPE == 0x0002u);
    assert(EXT4_FEATURE_INCOMPAT_EXTENTS == 0x0040u);
    assert(EXT4_FEATURE_INCOMPAT_64BIT == 0x0080u);
    assert(EXT4_FEATURE_INCOMPAT_FLEX_BG == 0x0200u);
    assert(EXT4_FEATURE_RO_COMPAT_BIGALLOC == 0x0200u);
    assert(EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER == 0x0001u);
    assert(EXT4_FEATURE_RO_COMPAT_LARGE_FILE == 0x0002u);
    assert(EXT4_FEATURE_RO_COMPAT_GDT_CSUM == 0x0010u);
    assert(EXT4_FEATURE_RO_COMPAT_HUGE_FILE == 0x0008u);
    assert(EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE == 0x0040u);
    assert(EXT4_FEATURE_RO_COMPAT_METADATA_CSUM == 0x0400u);
    puts("PASS constants");
}

static void test_parse_super_valid(void)
{
    struct storage_ext4_super_view view;

    /* 1 KiB blocks, 128-byte inodes (ext2-compatible shape). */
    super_base(0, 128);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.block_size == 1024);
    assert(view.cluster_size == 1024);
    assert(view.inode_size == 128);
    assert(view.blocks_count == 8192);
    assert(view.reserved_blocks_count == 400);
    assert(view.free_blocks_count == 7000);
    assert(view.inodes_count == 2048);
    assert(view.free_inodes_count == 2000);
    assert(view.first_data_block == 1);
    assert(view.blocks_per_group == 8192);
    assert(view.inodes_per_group == 2048);
    assert(view.group_count == 1);
    assert(view.desc_size == 32);
    assert(view.journal_inum == 8);
    assert(view.feature_compat == 0 && view.feature_incompat == 0 &&
           view.feature_ro_compat == 0);
    puts("PASS parse_super 1k");

    /* 2 KiB blocks. */
    super_base(1, 256);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.block_size == 2048);
    assert(view.cluster_size == 2048);
    assert(view.inode_size == 256);
    assert(view.first_data_block == 0);
    assert(view.group_count == 1);
    puts("PASS parse_super 2k");

    /* 4 KiB blocks. */
    super_base(2, 256);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.block_size == 4096);
    assert(view.cluster_size == 4096);
    assert(view.inode_size == 256);
    puts("PASS parse_super 4k");

    /* Multi-group geometry. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_BLOCKS_COUNT_LO, 24576);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.group_count == 3);
    puts("PASS parse_super group count");

    /* Boundary inode sizes: minimum, ruling multiple-of-128 domain, and
     * inode size equal to the block size. */
    super_base(2, 128);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.inode_size == 128);
    super_base(2, 384);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.inode_size == 384);
    super_base(2, 4096);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.inode_size == 4096);
    puts("PASS parse_super inode size bounds");

    /* Good-old rev: the s_inode_size field is not used (Linux rule), the
     * effective size is EXT4_GOOD_OLD_INODE_SIZE. */
    super_base(0, 128);
    ext4_put_le32(raw + SB_REV_LEVEL, 0);
    ext4_put_le16(raw + SB_INODE_SIZE, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.inode_size == 128);
    puts("PASS parse_super good-old rev");

    /* Feature words round-trip verbatim. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_FEATURE_COMPAT, EXT4_FEATURE_COMPAT_SPARSE_SUPER2 |
                                           EXT4_FEATURE_COMPAT_DIR_INDEX);
    ext4_put_le32(raw + SB_FEATURE_INCOMPAT, EXT4_FEATURE_INCOMPAT_EXTENTS |
                                             EXT4_FEATURE_INCOMPAT_FLEX_BG);
    ext4_put_le32(raw + SB_FEATURE_RO_COMPAT,
                  EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
                  EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
                  EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                  EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
                  EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.feature_compat ==
           (EXT4_FEATURE_COMPAT_SPARSE_SUPER2 | EXT4_FEATURE_COMPAT_DIR_INDEX));
    assert(view.feature_incompat ==
           (EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_FLEX_BG));
    assert(view.feature_ro_compat ==
           (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
            EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
            EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
            EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
            EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE));
    puts("PASS parse_super features");

    /* 64-bit block counts: high words combined only with INCOMPAT_64BIT. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_FEATURE_INCOMPAT, EXT4_FEATURE_INCOMPAT_64BIT);
    ext4_put_le16(raw + SB_DESC_SIZE, 64);
    ext4_put_le32(raw + SB_BLOCKS_COUNT_LO, 0xfffff000u);
    ext4_put_le32(raw + SB_R_BLOCKS_COUNT_LO, 0xfffffff0u);
    ext4_put_le32(raw + SB_FREE_BLOCKS_COUNT_LO, 0xfffff000u);
    ext4_put_le32(raw + SB_BLOCKS_COUNT_HI, 3);
    ext4_put_le32(raw + SB_R_BLOCKS_COUNT_HI, 1);
    ext4_put_le32(raw + SB_FREE_BLOCKS_COUNT_HI, 2);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.blocks_count == 0x3fffff000ULL);
    assert(view.reserved_blocks_count == 0x1fffffff0ULL);
    assert(view.free_blocks_count == 0x2fffff000ULL);
    assert(view.group_count == (0x3fffff000ULL + 8191) / 8192);
    assert(view.desc_size == 64);

    /* Without INCOMPAT_64BIT the high words are ignored. */
    ext4_put_le32(raw + SB_FEATURE_INCOMPAT, 0);
    ext4_put_le16(raw + SB_DESC_SIZE, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.blocks_count == 0xfffff000u);
    assert(view.reserved_blocks_count == 0xfffffff0u);
    assert(view.free_blocks_count == 0xfffff000u);
    assert(view.desc_size == 32);
    puts("PASS parse_super 64-bit counts");

    /* desc_size domain: 32..256 in steps of 4 when 64bit is set. */
    ext4_put_le32(raw + SB_FEATURE_INCOMPAT, EXT4_FEATURE_INCOMPAT_64BIT);
    ext4_put_le16(raw + SB_DESC_SIZE, 32);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.desc_size == 32);
    ext4_put_le16(raw + SB_DESC_SIZE, 256);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.desc_size == 256);
    ext4_put_le16(raw + SB_DESC_SIZE, 36);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0 && view.desc_size == 36);
    puts("PASS parse_super desc size bounds");

    /* UUID and volume label round-trip. */
    super_base(2, 256);
    for (int i = 0; i < 16; i++)
        raw[SB_UUID + i] = (uint8_t)(0xa0 + i);
    memcpy(raw + SB_VOLUME_NAME, "RELIEFOS", 8);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    for (int i = 0; i < 16; i++)
        assert(view.uuid[i] == (uint8_t)(0xa0 + i));
    assert(strcmp(view.volume_name, "RELIEFOS") == 0);
    puts("PASS parse_super identity");

    /* raw_len larger than the superblock is fine. */
    super_base(2, 256);
    assert(storage_ext4_parse_super(raw, 2048, &view) == 0);
    puts("PASS parse_super raw_len");
}

static void test_parse_super_invalid(void)
{
    struct storage_ext4_super_view view;
    int rc;

    /* NULL inputs -> -EINVAL. */
    super_base(2, 256);
    rc = storage_ext4_parse_super(NULL, 1024, &view);
    assert(rc == -RELIEFOS_EINVAL);
    rc = storage_ext4_parse_super(raw, 1024, NULL);
    assert(rc == -RELIEFOS_EINVAL);

    /* Truncated buffers -> -EINVAL (越界引用). */
    rc = storage_ext4_parse_super(raw, 1023, &view);
    assert(rc == -RELIEFOS_EINVAL);
    rc = storage_ext4_parse_super(raw, 0, &view);
    assert(rc == -RELIEFOS_EINVAL);
    puts("PASS parse_super null/short");

    /* Bad magic. */
    super_base(2, 256);
    ext4_put_le16(raw + SB_MAGIC, 0xef52);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_MAGIC, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_super bad magic");

    /* Block sizes outside {1024, 2048, 4096}. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_LOG_BLOCK_SIZE, 3);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le32(raw + SB_LOG_BLOCK_SIZE, 6);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le32(raw + SB_LOG_BLOCK_SIZE, 0xffffffffu);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_super bad block size");

    /* Cluster size: bounded shift, non-bigalloc must equal block size,
     * bigalloc (RO_COMPAT) must be >= block size. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_LOG_CLUSTER_SIZE, 19);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_LOG_CLUSTER_SIZE, 1);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_FEATURE_RO_COMPAT, EXT4_FEATURE_RO_COMPAT_BIGALLOC);
    ext4_put_le32(raw + SB_LOG_CLUSTER_SIZE, 1);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_FEATURE_RO_COMPAT, EXT4_FEATURE_RO_COMPAT_BIGALLOC);
    ext4_put_le32(raw + SB_LOG_CLUSTER_SIZE, 3);
    ext4_put_le32(raw + SB_CLUSTERS_PER_GROUP, 16384);
    assert(storage_ext4_parse_super(raw, 1024, &view) == 0);
    assert(view.cluster_size == 8192);
    puts("PASS parse_super cluster size");

    /* Inode size: >= 128, multiple of 128, <= block size. */
    super_base(2, 256);
    ext4_put_le16(raw + SB_INODE_SIZE, 120);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_INODE_SIZE, 64);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_INODE_SIZE, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_INODE_SIZE, 383);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_INODE_SIZE, 4224);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(0, 128);
    ext4_put_le16(raw + SB_INODE_SIZE, 2048);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_super bad inode size");

    /* Group geometry: zero counts and bitmap overflow limits. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_BLOCKS_COUNT_LO, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_FIRST_DATA_BLOCK, 100000);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_BLOCKS_PER_GROUP, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(2, 256);
    ext4_put_le32(raw + SB_INODES_PER_GROUP, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(0, 128);
    ext4_put_le32(raw + SB_BLOCKS_PER_GROUP, 8193);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    super_base(0, 128);
    ext4_put_le32(raw + SB_INODES_PER_GROUP, 8193);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_super bad geometry");

    /* desc_size outside 4-byte steps within 32..256 when 64bit is set. */
    super_base(2, 256);
    ext4_put_le32(raw + SB_FEATURE_INCOMPAT, EXT4_FEATURE_INCOMPAT_64BIT);
    ext4_put_le16(raw + SB_DESC_SIZE, 0);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_DESC_SIZE, 30);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_DESC_SIZE, 33);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_DESC_SIZE, 257);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + SB_DESC_SIZE, 258);
    assert(storage_ext4_parse_super(raw, 1024, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_super bad desc size");
}

static void group_base(uint32_t desc_size)
{
    memset(raw, 0, sizeof(raw));
    ext4_put_le32(raw + GD_BLOCK_BITMAP_LO, 100);
    ext4_put_le32(raw + GD_INODE_BITMAP_LO, 101);
    ext4_put_le32(raw + GD_INODE_TABLE_LO, 102);
    ext4_put_le16(raw + GD_FREE_BLOCKS_LO, 5000);
    ext4_put_le16(raw + GD_FREE_INODES_LO, 1500);
    ext4_put_le16(raw + GD_USED_DIRS_LO, 10);
    ext4_put_le16(raw + GD_FLAGS, EXT4_BG_INODE_ZEROED);
    ext4_put_le16(raw + GD_ITABLE_UNUSED_LO, 30);
    ext4_put_le16(raw + GD_CHECKSUM, 0xabcd);
    /* High words only exist in 64-byte and larger descriptors. */
    if (desc_size >= 64) {
        ext4_put_le32(raw + GD_BLOCK_BITMAP_HI, 1);
        ext4_put_le32(raw + GD_INODE_BITMAP_HI, 0);
        ext4_put_le32(raw + GD_INODE_TABLE_HI, 2);
        ext4_put_le16(raw + GD_FREE_BLOCKS_HI, 1);
        ext4_put_le16(raw + GD_FREE_INODES_HI, 2);
        ext4_put_le16(raw + GD_USED_DIRS_HI, 1);
        ext4_put_le16(raw + GD_ITABLE_UNUSED_HI, 1);
    }
}

static void test_parse_group_desc_valid(void)
{
    struct storage_ext4_group_view view;

    /* 32-byte classic descriptor. */
    group_base(32);
    assert(storage_ext4_parse_group_desc(raw, 32, 32, &view) == 0);
    assert(view.block_bitmap == 100);
    assert(view.inode_bitmap == 101);
    assert(view.inode_table == 102);
    assert(view.free_blocks_count == 5000);
    assert(view.free_inodes_count == 1500);
    assert(view.used_dirs_count == 10);
    assert(view.itable_unused == 30);
    assert(view.flags == EXT4_BG_INODE_ZEROED);
    assert(view.checksum == 0xabcd);
    puts("PASS parse_group_desc 32");

    /* 64-bit descriptor: high words combined. */
    group_base(64);
    assert(storage_ext4_parse_group_desc(raw, 64, 64, &view) == 0);
    assert(view.block_bitmap == 0x100000064ULL);
    assert(view.inode_bitmap == 101);
    assert(view.inode_table == 0x200000066ULL);
    assert(view.free_blocks_count == 5000 + (1u << 16));
    assert(view.free_inodes_count == 1500 + (2u << 16));
    assert(view.used_dirs_count == 10 + (1u << 16));
    assert(view.itable_unused == 30 + (1u << 16));
    assert(view.checksum == 0xabcd);
    puts("PASS parse_group_desc 64");

    /* 256-byte descriptor still combines the 64-bit fields. */
    group_base(256);
    assert(storage_ext4_parse_group_desc(raw, 256, 256, &view) == 0);
    assert(view.block_bitmap == 0x100000064ULL);
    assert(view.inode_table == 0x200000066ULL);
    assert(view.itable_unused == 30 + (1u << 16));
    puts("PASS parse_group_desc 256");

    /* Sub-64 descriptors ignore any high-word bytes lying past the
     * declared descriptor size. */
    group_base(32);
    memset(raw + 32, 0xff, 32);
    assert(storage_ext4_parse_group_desc(raw, 64, 32, &view) == 0);
    assert(view.block_bitmap == 100);
    assert(view.inode_bitmap == 101);
    assert(view.inode_table == 102);
    assert(view.free_blocks_count == 5000);
    assert(view.itable_unused == 30);
    puts("PASS parse_group_desc hi ignored");

    /* Boundary: desc_size 36 (4-byte step) accepts and reads only the
     * classic fields. */
    group_base(32);
    assert(storage_ext4_parse_group_desc(raw, 36, 36, &view) == 0);
    assert(view.block_bitmap == 100 && view.free_blocks_count == 5000);
    puts("PASS parse_group_desc desc_size bounds");
}

static void test_parse_group_desc_invalid(void)
{
    struct storage_ext4_group_view view;
    int rc;

    group_base(32);
    rc = storage_ext4_parse_group_desc(NULL, 32, 32, &view);
    assert(rc == -RELIEFOS_EINVAL);
    rc = storage_ext4_parse_group_desc(raw, 32, 32, NULL);
    assert(rc == -RELIEFOS_EINVAL);

    /* desc_size domain: 4-byte steps within 32..256. */
    assert(storage_ext4_parse_group_desc(raw, 256, 0, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 256, 28, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 256, 33, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 256, 257, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 256, 258, &view) == -RELIEFOS_EINVAL);

    /* Buffer shorter than the descriptor -> -EINVAL. */
    assert(storage_ext4_parse_group_desc(raw, 63, 64, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 31, 32, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_group_desc(raw, 0, 32, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_group_desc invalid");
}

static void extent_base(void)
{
    memset(raw, 0, sizeof(raw));
    ext4_put_le16(raw + EH_MAGIC, EXT4_EXT_MAGIC);
    ext4_put_le16(raw + EH_ENTRIES, 4);
    ext4_put_le16(raw + EH_MAX, 4);
    ext4_put_le16(raw + EH_DEPTH, 0);
    ext4_put_le32(raw + EH_GENERATION, 7);
}

static void test_parse_extent_header(void)
{
    struct storage_ext4_extent_header_view view;

    extent_base();
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == 0);
    assert(view.magic == EXT4_EXT_MAGIC);
    assert(view.entries == 4);
    assert(view.max_entries == 4);
    assert(view.depth == 0);
    assert(view.generation == 7);
    puts("PASS parse_extent_header valid");

    /* Boundary: entries == max, empty node, maximum depth. */
    ext4_put_le16(raw + EH_ENTRIES, 0);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == 0 && view.entries == 0);
    ext4_put_le16(raw + EH_ENTRIES, 4);
    ext4_put_le16(raw + EH_MAX, 4);
    ext4_put_le16(raw + EH_DEPTH, EXT4_MAX_EXTENT_DEPTH);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == 0 &&
           view.depth == EXT4_MAX_EXTENT_DEPTH);
    puts("PASS parse_extent_header bounds");

    /* Corruption: bad magic, entries over max, depth over the limit. */
    extent_base();
    ext4_put_le16(raw + EH_MAGIC, 0xf30b);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == -RELIEFOS_EINVAL);
    extent_base();
    ext4_put_le16(raw + EH_MAGIC, 0);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == -RELIEFOS_EINVAL);
    extent_base();
    ext4_put_le16(raw + EH_ENTRIES, 5);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == -RELIEFOS_EINVAL);
    ext4_put_le16(raw + EH_ENTRIES, 0xffff);
    ext4_put_le16(raw + EH_MAX, 0);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == -RELIEFOS_EINVAL);
    extent_base();
    ext4_put_le16(raw + EH_DEPTH, EXT4_MAX_EXTENT_DEPTH + 1);
    assert(storage_ext4_parse_extent_header(raw, 12, &view) == -RELIEFOS_EINVAL);
    puts("PASS parse_extent_header corruption");

    /* NULL and truncation. */
    extent_base();
    assert(storage_ext4_parse_extent_header(NULL, 12, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_extent_header(raw, 12, NULL) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_extent_header(raw, 11, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_extent_header(raw, 0, &view) == -RELIEFOS_EINVAL);
    assert(storage_ext4_parse_extent_header(raw, 64, &view) == 0);
    puts("PASS parse_extent_header null/short");
}

int main(void)
{
    test_errno_mapping();
    test_constants();
    test_endian_helpers();
    test_range_ok();
    test_parse_super_valid();
    test_parse_super_invalid();
    test_parse_group_desc_valid();
    test_parse_group_desc_invalid();
    test_parse_extent_header();
    puts("PASS ext4_format_test");
    return 0;
}
