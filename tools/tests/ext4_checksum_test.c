/* Host-side tests for the ext4 metadata checksum layer.
 *
 * Covers the CRC32C/CRC16 primitives, the superblock / group-descriptor /
 * inode checksum algorithms in
 * kernel/reliefnt/fs/storage_ext4_checksum.c.
 *
 * Independent oracles (the implementation under test is table-driven and
 * shares no code with these):
 *
 *  - CRC32C goldens come from a textbook MSB-first reference of the
 *    published CRC-32C parameter set (poly 0x1EDC6F41, refin=refout=true,
 *    xorout=0xFFFFFFFF; check value of "123456789" is 0xE3069283), built
 *    as raw(seed, D) = reflect32(msb_first(reflect32(seed), refl(D))).
 *    The oracle (/tmp/crc_oracle.c, not committed) was cross-checked
 *    against libext2fs ext2fs_crc32c_le() (e2fsprogs 1.47.2) and the
 *    public check value.  storage_ext4_crc32c() has Linux ext4_chksum
 *    semantics: reflected Castagnoli, caller seed, no final xor, so the
 *    public vector appears as raw ^ 0xFFFFFFFF.
 *
 *  - CRC16 goldens come from compiling linux/lib/crc/crc16.c's table and
 *    loop verbatim in a standalone oracle (/tmp/crc_oracle.c); the table
 *    was additionally proven identical to a poly(0x8005)/refl(0xA001)
 *    generated table.
 *
 *  - superblock / group-descriptor / inode checksums are pinned by real
 *    mke2fs image round trips: verify() must accept what mke2fs wrote,
 *    update() must reproduce the on-disk checksum bytes exactly, and any
 *    single-byte tamper must fail with -RELIEFOS_EIO.
 *
 * Build (task 3 step 4; the 4-path include convention is the standing
 * ruling for this plan, progress.md preflight #5):
 *   cc -std=c11 -O1 -g -fsanitize=address,undefined \
 *     -Ikernel/reliefnt/include -Iinclude \
 *     -Ikernel/reliefnt/include/uapi -Ikernel/reliefnt/kernel/reliefnt/include \
 *     tools/tests/ext4_checksum_test.c \
 *     kernel/reliefnt/fs/storage_ext4_checksum.c \
 *     -o /tmp/reliefos-ext4-checksum-test
 *   ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
 *     /tmp/reliefos-ext4-checksum-test
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kernel/reliefnt/block/storage_internal.h"

/* ext4_super_block offsets (linux/fs/ext4/ext4.h:1369). */
#define SB_INODES_COUNT 0x00
#define SB_BLOCKS_COUNT_LO 0x04
#define SB_FREE_BLOCKS_COUNT_LO 0x0C
#define SB_FREE_INODES_COUNT 0x10
#define SB_FIRST_DATA_BLOCK 0x14
#define SB_LOG_BLOCK_SIZE 0x18
#define SB_LOG_CLUSTER_SIZE 0x1C
#define SB_BLOCKS_PER_GROUP 0x20
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
#define SB_JOURNAL_INUM 0xE0
#define SB_DESC_SIZE 0xFE
#define SB_CHECKSUM_SEED 0x270
#define SB_CHECKSUM 0x3FC

/* ext4_group_desc offsets (linux/fs/ext4/ext4.h:402). */
#define GD_BLOCK_BITMAP_LO 0x00
#define GD_INODE_BITMAP_LO 0x04
#define GD_INODE_TABLE_LO 0x08
#define GD_FLAGS 0x12
#define GD_ITABLE_UNUSED_LO 0x1C
#define GD_CHECKSUM 0x1E
#define GD_INODE_TABLE_HI 0x28

/* ext4_inode offsets (linux/fs/ext4/ext4.h:231). */
#define INO_GENERATION 0x64
#define INO_CHECKSUM_LO 0x7C
#define INO_EXTRA_ISIZE 0x80
#define INO_CHECKSUM_HI 0x82
/* EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi): offsetof(i_checksum_hi) +
 * 2 - EXT4_GOOD_OLD_INODE_SIZE == 4 (ext4.h:869-873). */
#define INO_CHECKSUM_HI_EXTRA_END 4u

/* Test-local little-endian helpers: this test must not link
 * storage_ext4_format.c, so the offsets above plus these helpers form an
 * independent copy of the on-disk layout. */
static uint16_t t_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t t_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void t_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void t_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Fixed UUID used by every fixture and by the golden constants. */
static const uint8_t TEST_UUID[16] = {
    0x05, 0x68, 0x16, 0x38, 0xfc, 0x13, 0x44, 0xfc,
    0x9d, 0xdf, 0x50, 0xb4, 0xc8, 0x59, 0xbc, 0x55
};

/* The s_csum_seed that Linux/e2fsprogs derive from TEST_UUID when the
 * CSUM_SEED feature is absent: crc32c(~0, uuid, 16), golden-pinned below. */
#define SEED_FROM_UUID 0x570dbd47u

/* 32 zero bytes; 256 ascending bytes; 64 bytes of 7*i+1. */
static uint8_t zeros32[32];
static uint8_t asc256[256];
static uint8_t asc64[64];
static const char FOX[] = "The quick brown fox jumps over the lazy dog";

static void fill_patterns(void)
{
    uint32_t i;

    memset(zeros32, 0, sizeof(zeros32));
    for (i = 0; i < 256; i++)
        asc256[i] = (uint8_t)i;
    for (i = 0; i < 64; i++)
        asc64[i] = (uint8_t)(i * 7 + 1);
}

/* ---- CRC primitives ------------------------------------------------- */

static void test_crc32c_vectors(void)
{
    uint32_t whole, part;

    /* Public CRC-32C check value (standard parameter set).  The raw
     * chain form has no final xor, so the check value appears as
     * raw ^ 0xFFFFFFFF. */
    assert(storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 9) ==
           0x1cf96d7cu);
    assert((storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 9) ^
            0xffffffffu) == 0xe3069283u);

    /* Golden values from the textbook MSB-first oracle (see header). */
    assert(storage_ext4_crc32c(0x00000000u, (const uint8_t *)"123456789", 9) ==
           0x58e3fa20u);
    assert(storage_ext4_crc32c(0x12345678u, (const uint8_t *)FOX, 43) ==
           0x6b729d78u);
    assert(storage_ext4_crc32c(0xffffffffu, zeros32, 32) == 0x756ec955u);
    assert(storage_ext4_crc32c(0x00000000u, asc256, 256) == 0x2436a9dbu);
    assert(storage_ext4_crc32c(0x12345678u, asc64, 64) == 0x3e839e3eu);
    assert(storage_ext4_crc32c(0xdeadbeefu, TEST_UUID, 16) == 0x30c7444fu);
    assert(storage_ext4_crc32c(0xffffffffu, TEST_UUID, 16) == SEED_FROM_UUID);
    puts("PASS crc32c vectors");

    /* Empty input returns the seed unchanged (any seed, NULL or not). */
    assert(storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 0) ==
           0xffffffffu);
    assert(storage_ext4_crc32c(0x00000000u, (const uint8_t *)"123456789", 0) ==
           0x00000000u);
    assert(storage_ext4_crc32c(0x12345678u, NULL, 0) == 0x12345678u);
    assert(storage_ext4_crc32c(0xdeadbeefu, NULL, 0) == 0xdeadbeefu);
    /* NULL data with a non-zero length yields 0. */
    assert(storage_ext4_crc32c(0xdeadbeefu, NULL, 1) == 0u);
    assert(storage_ext4_crc32c(0xdeadbeefu, NULL, 999) == 0u);
    puts("PASS crc32c empty/null");

    /* Segmented seed chaining: feeding the running seed across a split
     * must equal the single-shot computation. */
    whole = storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 9);
    assert(storage_ext4_crc32c(
               storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 4),
               (const uint8_t *)"123456789" + 4, 5) == whole);
    assert(storage_ext4_crc32c(
               storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 1),
               (const uint8_t *)"123456789" + 1, 8) == whole);
    assert(storage_ext4_crc32c(
               storage_ext4_crc32c(0xffffffffu, (const uint8_t *)"123456789", 8),
               (const uint8_t *)"123456789" + 8, 1) == whole);
    part = storage_ext4_crc32c(0x12345678u, asc256, 100);
    assert(storage_ext4_crc32c(part, asc256 + 100, 156) ==
           storage_ext4_crc32c(0x12345678u, asc256, 256));
    /* Empty segments in the middle do not move the chain. */
    assert(storage_ext4_crc32c(storage_ext4_crc32c(part, NULL, 0),
                               asc256 + 100, 156) ==
           storage_ext4_crc32c(part, asc256 + 100, 156));
    puts("PASS crc32c seed chain");
}

static void test_crc16_vectors(void)
{
    uint32_t part;

    /* Golden values from the linux/lib/crc/crc16.c standalone oracle. */
    assert(storage_ext4_crc16(0xffffu, (const uint8_t *)"123456789", 9) ==
           0x4b37u);
    assert(storage_ext4_crc16(0x0000u, (const uint8_t *)"123456789", 9) ==
           0xbb3du);
    assert(storage_ext4_crc16(0xffffu, (const uint8_t *)FOX, 43) == 0xa89cu);
    assert(storage_ext4_crc16(0x1234u, asc256, 256) == 0x986fu);
    assert(storage_ext4_crc16(0xffffu, TEST_UUID, 16) == 0x8d42u);
    puts("PASS crc16 vectors");

    /* Empty input returns the seed; NULL with len > 0 yields 0. */
    assert(storage_ext4_crc16(0xffffu, (const uint8_t *)"123456789", 0) ==
           0xffffu);
    assert(storage_ext4_crc16(0x1234u, NULL, 0) == 0x1234u);
    assert(storage_ext4_crc16(0x1234u, NULL, 7) == 0u);
    puts("PASS crc16 empty/null");

    /* Segmented seed chaining. */
    assert(storage_ext4_crc16(
               storage_ext4_crc16(0xffffu, (const uint8_t *)"123456789", 4),
               (const uint8_t *)"123456789" + 4, 5) == 0x4b37u);
    part = storage_ext4_crc16(0x1234u, asc256, 33);
    assert(storage_ext4_crc16(part, asc256 + 33, 223) ==
           storage_ext4_crc16(0x1234u, asc256, 256));
    puts("PASS crc16 seed chain");
}

/* ---- Superblock checksum -------------------------------------------- */

/* Hand-built superblock fixture; offsets pinned independently above. */
static uint8_t sb_raw[1024];
static uint8_t scratch[4096];

static void build_super(uint32_t feature_compat, uint32_t feature_incompat,
                        uint32_t feature_ro_compat)
{
    memset(sb_raw, 0, sizeof(sb_raw));
    t_put32(sb_raw + SB_INODES_COUNT, 2048);
    t_put32(sb_raw + SB_BLOCKS_COUNT_LO, 8192);
    t_put32(sb_raw + SB_FREE_BLOCKS_COUNT_LO, 7000);
    t_put32(sb_raw + SB_FREE_INODES_COUNT, 2000);
    t_put32(sb_raw + SB_FIRST_DATA_BLOCK, 0);
    t_put32(sb_raw + SB_LOG_BLOCK_SIZE, 2);
    t_put32(sb_raw + SB_LOG_CLUSTER_SIZE, 2);
    t_put32(sb_raw + SB_BLOCKS_PER_GROUP, 8192);
    t_put32(sb_raw + SB_INODES_PER_GROUP, 2048);
    t_put16(sb_raw + SB_MAGIC, EXT4_SUPER_MAGIC);
    t_put32(sb_raw + SB_STATE, 1);
    t_put32(sb_raw + SB_CREATOR_OS, 0);
    t_put32(sb_raw + SB_REV_LEVEL, 1);
    t_put32(sb_raw + SB_FIRST_INO, 11);
    t_put16(sb_raw + SB_INODE_SIZE, 256);
    t_put32(sb_raw + SB_FEATURE_COMPAT, feature_compat);
    t_put32(sb_raw + SB_FEATURE_INCOMPAT, feature_incompat);
    t_put32(sb_raw + SB_FEATURE_RO_COMPAT, feature_ro_compat);
    memcpy(sb_raw + SB_UUID, TEST_UUID, 16);
    t_put32(sb_raw + SB_JOURNAL_INUM, 8);
    t_put32(sb_raw + SB_CHECKSUM_SEED, 0xd46d944bu);
}

static void test_super_checksum(void)
{
    uint32_t want;

    /* metadata_csum: seed is the fixed ~0, never s_checksum_seed. */
    build_super(EXT4_FEATURE_COMPAT_HAS_JOURNAL,
                EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
                EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
    assert(storage_ext4_update_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    want = storage_ext4_crc32c(0xffffffffu, sb_raw, SB_CHECKSUM);
    assert(want != 0);
    assert(t_get32(sb_raw + SB_CHECKSUM) == want);
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    puts("PASS super checksum valid");

    /* Single-byte corruption anywhere in the covered range is -EIO. */
    sb_raw[300] ^= 0x01u;
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) ==
           -RELIEFOS_EIO);
    sb_raw[300] ^= 0x01u;
    sb_raw[SB_UUID + 3] ^= 0x80u;
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) ==
           -RELIEFOS_EIO);
    sb_raw[SB_UUID + 3] ^= 0x80u;
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    /* Corruption of the checksum field itself is -EIO too. */
    sb_raw[SB_CHECKSUM + 1] ^= 0x01u;
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) ==
           -RELIEFOS_EIO);
    sb_raw[SB_CHECKSUM + 1] ^= 0x01u;
    /* update() repairs a damaged field in place. */
    sb_raw[SB_CHECKSUM] ^= 0xffu;
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) ==
           -RELIEFOS_EIO);
    assert(storage_ext4_update_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    puts("PASS super checksum corruption");

    /* Without metadata_csum both entry points are no-ops. */
    build_super(EXT4_FEATURE_COMPAT_HAS_JOURNAL,
                EXT4_FEATURE_INCOMPAT_EXTENTS,
                EXT4_FEATURE_RO_COMPAT_GDT_CSUM);
    t_put32(sb_raw + SB_CHECKSUM, 0xbadf00du);
    memcpy(scratch, sb_raw, sizeof(sb_raw));
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(storage_ext4_update_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(memcmp(scratch, sb_raw, sizeof(sb_raw)) == 0);
    build_super(0, EXT4_FEATURE_INCOMPAT_EXTENTS, 0);
    memcpy(scratch, sb_raw, sizeof(sb_raw));
    assert(storage_ext4_verify_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(storage_ext4_update_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(memcmp(scratch, sb_raw, sizeof(sb_raw)) == 0);
    puts("PASS super checksum feature gate");

    /* A buffer longer than one superblock behaves like the first 1024. */
    build_super(0, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
                EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
    memset(scratch, 0xa5, sizeof(scratch));
    memcpy(scratch, sb_raw, sizeof(sb_raw));
    assert(storage_ext4_update_super_checksum(scratch, sizeof(scratch)) == 0);
    want = storage_ext4_crc32c(0xffffffffu, scratch, SB_CHECKSUM);
    assert(t_get32(scratch + SB_CHECKSUM) == want);
    assert(storage_ext4_verify_super_checksum(scratch, sizeof(scratch)) == 0);
    assert(storage_ext4_verify_super_checksum(scratch, 1024) == 0);
    puts("PASS super checksum long buffer");

    /* NULL and out-of-range lengths. */
    assert(storage_ext4_verify_super_checksum(NULL, 1024) == -RELIEFOS_EINVAL);
    assert(storage_ext4_update_super_checksum(NULL, 1024) == -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_super_checksum(sb_raw, 1023) == -RELIEFOS_EINVAL);
    assert(storage_ext4_update_super_checksum(sb_raw, 1023) == -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_super_checksum(sb_raw, 0) == -RELIEFOS_EINVAL);
    assert(storage_ext4_update_super_checksum(sb_raw, 0) == -RELIEFOS_EINVAL);
    assert(storage_ext4_update_super_checksum(sb_raw, sizeof(sb_raw)) == 0);
    assert(storage_ext4_verify_super_checksum(sb_raw, 1024) == 0);
    puts("PASS super checksum null/short");
}

/* ---- Group descriptor checksum -------------------------------------- */

static uint8_t gd_raw[256];

static void build_gd(uint32_t desc_size)
{
    uint32_t i;

    memset(gd_raw, 0, sizeof(gd_raw));
    for (i = 0; i < desc_size; i++)
        gd_raw[i] = (uint8_t)(0x30 + i * 5);
    t_put16(gd_raw + GD_CHECKSUM, 0);
}

static void build_view(struct storage_ext4_super_view *v, uint32_t incompat,
                       uint32_t ro_compat, int use_field_seed)
{
    memset(v, 0, sizeof(*v));
    v->blocks_count = 8192;
    v->group_count = 1;
    v->block_size = 4096;
    v->cluster_size = 4096;
    v->blocks_per_group = 8192;
    v->inodes_per_group = 2048;
    v->inode_size = 256;
    v->desc_size = 64;
    v->feature_incompat = incompat;
    v->feature_ro_compat = ro_compat;
    memcpy(v->uuid, TEST_UUID, 16);
    v->checksum_seed = use_field_seed ? 0xd46d944bu : 0;
}

/* Reference composition of the metadata_csum group checksum, spelled out
 * per linux/fs/ext4/super.c:3262-3300 with the seed handed in. */
static uint16_t ref_group_csum32(uint32_t seed, const uint8_t *gd,
                                 uint32_t desc_size, uint32_t group)
{
    uint8_t le_group[4];
    static const uint8_t zeros2[2];
    uint32_t c;

    t_put32(le_group, group);
    c = storage_ext4_crc32c(seed, le_group, 4);
    c = storage_ext4_crc32c(c, gd, GD_CHECKSUM);
    c = storage_ext4_crc32c(c, zeros2, 2);
    if (desc_size > 32)
        c = storage_ext4_crc32c(c, gd + 0x20, desc_size - 0x20);
    return (uint16_t)(c & 0xffffu);
}

/* Reference composition of the legacy gdt_csum (crc16) chain.  The tail
 * beyond the checksum slot only participates when INCOMPAT_64BIT is set
 * and desc_size > 32 (linux super.c:3293-3295). */
static uint16_t ref_group_csum16(const struct storage_ext4_super_view *v,
                                 const uint8_t *gd, uint32_t desc_size,
                                 uint32_t group)
{
    uint8_t le_group[4];
    uint16_t c;

    t_put32(le_group, group);
    c = storage_ext4_crc16(0xffffu, v->uuid, 16);
    c = storage_ext4_crc16(c, le_group, 4);
    c = storage_ext4_crc16(c, gd, GD_CHECKSUM);
    if ((v->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0 &&
        desc_size > 32)
        c = storage_ext4_crc16(c, gd + 0x20, desc_size - 0x20);
    return c;
}

static void group_roundtrip(const struct storage_ext4_super_view *v,
                            uint32_t desc_size, uint16_t expect,
                            int tail_covered)
{
    uint8_t copy[256];

    assert(storage_ext4_update_group_checksum(gd_raw, desc_size, 7, v) == 0);
    assert(t_get16(gd_raw + GD_CHECKSUM) == expect);
    assert(storage_ext4_verify_group_checksum(gd_raw, desc_size, 7, v) == 0);
    /* Corruption of any covered byte is -EIO. */
    memcpy(copy, gd_raw, desc_size);
    copy[3] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, desc_size, 7, v) ==
           -RELIEFOS_EIO);
    memcpy(copy, gd_raw, desc_size);
    copy[GD_CHECKSUM] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, desc_size, 7, v) ==
           -RELIEFOS_EIO);
    if (tail_covered && desc_size > 32) {
        memcpy(copy, gd_raw, desc_size);
        copy[desc_size - 1] ^= 0x80u;
        assert(storage_ext4_verify_group_checksum(copy, desc_size, 7, v) ==
               -RELIEFOS_EIO);
    } else if (desc_size > 32) {
        /* The tail is outside the checksum: corruption there is
         * invisible to verify(). */
        memcpy(copy, gd_raw, desc_size);
        copy[desc_size - 1] ^= 0x80u;
        assert(storage_ext4_verify_group_checksum(copy, desc_size, 7, v) == 0);
    }
    /* Wrong group number invalidates the checksum. */
    assert(storage_ext4_verify_group_checksum(gd_raw, desc_size, 8, v) ==
           -RELIEFOS_EIO);
    assert(storage_ext4_verify_group_checksum(gd_raw, desc_size, 0xffffffffu, v) ==
           -RELIEFOS_EIO);
    assert(storage_ext4_verify_group_checksum(gd_raw, desc_size, 7, v) == 0);
    /* update() is idempotent. */
    memcpy(copy, gd_raw, desc_size);
    assert(storage_ext4_update_group_checksum(gd_raw, desc_size, 7, v) == 0);
    assert(memcmp(copy, gd_raw, desc_size) == 0);
}

static void test_group_checksum(void)
{
    struct storage_ext4_super_view v;
    uint8_t copy[256];

    /* metadata_csum, seed derived from the UUID. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);
    assert(storage_ext4_super_csum_seed(&v) == SEED_FROM_UUID);
    build_gd(32);
    group_roundtrip(&v, 32, ref_group_csum32(SEED_FROM_UUID, gd_raw, 32, 7), 1);
    build_gd(64);
    group_roundtrip(&v, 64, ref_group_csum32(SEED_FROM_UUID, gd_raw, 64, 7), 1);
    build_gd(256);
    group_roundtrip(&v, 256, ref_group_csum32(SEED_FROM_UUID, gd_raw, 256, 7), 1);
    build_gd(36);
    group_roundtrip(&v, 36, ref_group_csum32(SEED_FROM_UUID, gd_raw, 36, 7), 1);
    puts("PASS group checksum metadata_csum");

    /* metadata_csum with CSUM_SEED: the s_checksum_seed field wins and
     * is deliberately different from the UUID-derived seed. */
    build_view(&v,
               EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS |
                   EXT4_FEATURE_INCOMPAT_CSUM_SEED,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 1);
    assert(storage_ext4_super_csum_seed(&v) == 0xd46d944bu);
    assert(storage_ext4_super_csum_seed(&v) != SEED_FROM_UUID);
    build_gd(64);
    group_roundtrip(&v, 64, ref_group_csum32(0xd46d944bu, gd_raw, 64, 7), 1);
    build_gd(32);
    group_roundtrip(&v, 32, ref_group_csum32(0xd46d944bu, gd_raw, 32, 7), 1);
    puts("PASS group checksum csum_seed feature");

    /* Legacy gdt_csum path (crc16), 32 and 64 byte descriptors. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_GDT_CSUM, 0);
    build_gd(32);
    group_roundtrip(&v, 32, ref_group_csum16(&v, gd_raw, 32, 7), 0);
    build_gd(64);
    group_roundtrip(&v, 64, ref_group_csum16(&v, gd_raw, 64, 7), 0);
    /* 64-bit + gdt_csum: tail range participates. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_GDT_CSUM, 0);
    build_gd(64);
    group_roundtrip(&v, 64, ref_group_csum16(&v, gd_raw, 64, 7), 1);
    puts("PASS group checksum gdt_csum");

    /* gdt_csum tail gate: without INCOMPAT_64BIT the bytes beyond the
     * checksum slot stay out of the crc16 chain even at desc_size 64
     * (linux super.c:3293-3295), while the metadata_csum path has no
     * such gate. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_GDT_CSUM, 0);
    build_gd(64);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, &v) == 0);
    memcpy(copy, gd_raw, 64);
    copy[0x30] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, 64, 7, &v) == 0);
    copy[0x30] ^= 0x01u;
    copy[3] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, 64, 7, &v) ==
           -RELIEFOS_EIO);
    /* update() output is independent of the uncovered tail. */
    memcpy(copy, gd_raw, 64);
    copy[0x30] ^= 0xffu;
    assert(storage_ext4_update_group_checksum(copy, 64, 7, &v) == 0);
    assert(t_get16(copy + GD_CHECKSUM) == t_get16(gd_raw + GD_CHECKSUM));
    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_GDT_CSUM, 0);
    build_gd(64);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, &v) == 0);
    memcpy(copy, gd_raw, 64);
    copy[0x30] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, 64, 7, &v) ==
           -RELIEFOS_EIO);
    /* metadata_csum hashes the tail whenever desc_size > 32. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);
    build_gd(64);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, &v) == 0);
    memcpy(copy, gd_raw, 64);
    copy[0x30] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(copy, 64, 7, &v) ==
           -RELIEFOS_EIO);
    puts("PASS group checksum gdt_csum 64bit tail gate");

    /* metadata_csum wins when both feature bits are set (Linux order). */
    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |
                   EXT4_FEATURE_RO_COMPAT_GDT_CSUM,
               0);
    build_gd(64);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, &v) == 0);
    assert(t_get16(gd_raw + GD_CHECKSUM) ==
           ref_group_csum32(SEED_FROM_UUID, gd_raw, 64, 7));
    assert(storage_ext4_verify_group_checksum(gd_raw, 64, 7, &v) == 0);
    puts("PASS group checksum feature precedence");

    /* No checksum feature at all: verify passes, update never writes. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS, 0, 0);
    build_gd(64);
    t_put16(gd_raw + GD_CHECKSUM, 0x1234u);
    memcpy(copy, gd_raw, 64);
    assert(storage_ext4_verify_group_checksum(gd_raw, 64, 7, &v) == 0);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, &v) == 0);
    assert(memcmp(copy, gd_raw, 64) == 0);
    puts("PASS group checksum feature gate");

    /* Invalid inputs. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);
    build_gd(64);
    assert(storage_ext4_verify_group_checksum(NULL, 64, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_group_checksum(NULL, 64, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_group_checksum(gd_raw, 64, 7, NULL) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_group_checksum(gd_raw, 64, 7, NULL) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_group_checksum(gd_raw, 0, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_group_checksum(gd_raw, 31, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_group_checksum(gd_raw, 33, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_group_checksum(gd_raw, 260, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_group_checksum(gd_raw, 28, 7, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_group_checksum(gd_raw, 0xffffffffu, 7, &v) ==
           -RELIEFOS_EINVAL);
    /* Domain endpoints stay valid. */
    build_gd(EXT4_MIN_DESC_SIZE);
    assert(storage_ext4_update_group_checksum(gd_raw, EXT4_MIN_DESC_SIZE, 7, &v) == 0);
    assert(storage_ext4_verify_group_checksum(gd_raw, EXT4_MIN_DESC_SIZE, 7, &v) == 0);
    build_gd(EXT4_MAX_DESC_SIZE);
    assert(storage_ext4_update_group_checksum(gd_raw, EXT4_MAX_DESC_SIZE, 7, &v) == 0);
    assert(storage_ext4_verify_group_checksum(gd_raw, EXT4_MAX_DESC_SIZE, 7, &v) == 0);
    puts("PASS group checksum null/invalid size");
}

/* ---- Inode checksum -------------------------------------------------- */

static uint8_t ino_raw[4096];

static void build_inode(uint32_t inode_size, uint32_t extra_isize)
{
    uint32_t i;

    memset(ino_raw, 0, sizeof(ino_raw));
    for (i = 0; i < inode_size; i++)
        ino_raw[i] = (uint8_t)(i * 11 + 3);
    t_put32(ino_raw + INO_GENERATION, 0x01020304u);
    t_put16(ino_raw + INO_CHECKSUM_LO, 0);
    if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        t_put16(ino_raw + INO_EXTRA_ISIZE, (uint16_t)extra_isize);
        t_put16(ino_raw + INO_CHECKSUM_HI, 0);
    }
}

/* Reference composition of the inode checksum chain
 * (linux/fs/ext4/inode.c:58-124, seed inode.c:5404-5411).  Past the
 * good-old size the i_checksum_hi slot at 0x82 is hashed as two zero
 * bytes only when EXT4_FITS_IN_INODE says the field exists
 * (i_extra_isize >= 4); otherwise 0x82 onward is ordinary hash input and
 * only the low 16 bits of the result ever reach disk.  force=1/2 override
 * the branch (wrong-formula fixtures), 0 reads i_extra_isize from the
 * buffer like Linux does. */
static uint32_t ref_inode_csum_ex(uint32_t seed, const uint8_t *ino,
                                  uint32_t inode_size, uint32_t ino_no,
                                  uint32_t generation, int force)
{
    uint8_t le_ino[4], le_gen[4];
    static const uint8_t zeros2[2];
    uint32_t c;
    int fits;

    t_put32(le_ino, ino_no);
    t_put32(le_gen, generation);
    c = storage_ext4_crc32c(seed, le_ino, 4);
    c = storage_ext4_crc32c(c, le_gen, 4);
    c = storage_ext4_crc32c(c, ino, INO_CHECKSUM_LO);
    c = storage_ext4_crc32c(c, zeros2, 2);
    c = storage_ext4_crc32c(c, ino + 0x7e, 2);
    if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        fits = force == 0 ? (t_get16(ino + INO_EXTRA_ISIZE) >=
                             INO_CHECKSUM_HI_EXTRA_END)
                          : (force == 1);
        c = storage_ext4_crc32c(c, ino + INO_EXTRA_ISIZE, 2);
        if (fits) {
            c = storage_ext4_crc32c(c, zeros2, 2);
            c = storage_ext4_crc32c(c, ino + INO_CHECKSUM_HI + 2,
                                    inode_size - INO_CHECKSUM_HI - 2);
        } else {
            c = storage_ext4_crc32c(c, ino + INO_CHECKSUM_HI,
                                    inode_size - INO_CHECKSUM_HI);
        }
    }
    return c;
}

static uint32_t ref_inode_csum(uint32_t seed, const uint8_t *ino,
                               uint32_t inode_size, uint32_t ino_no,
                               uint32_t generation)
{
    return ref_inode_csum_ex(seed, ino, inode_size, ino_no, generation, 0);
}

static void inode_roundtrip(const struct storage_ext4_super_view *v,
                            uint32_t inode_size, uint32_t ino_no,
                            uint32_t generation, uint32_t seed)
{
    uint8_t copy[4096];
    uint32_t expect = ref_inode_csum(seed, ino_raw, inode_size, ino_no,
                                     generation);
    int fits = inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
               t_get16(ino_raw + INO_EXTRA_ISIZE) >= INO_CHECKSUM_HI_EXTRA_END;

    memcpy(copy, ino_raw, inode_size);
    assert(storage_ext4_update_inode_checksum(ino_raw, inode_size, ino_no,
                                              generation, v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == (uint16_t)(expect & 0xffffu));
    if (fits) {
        assert(t_get16(ino_raw + INO_CHECKSUM_HI) == (uint16_t)(expect >> 16));
    } else if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        /* i_checksum_hi does not exist: the slot is hash input and must
         * survive update() untouched. */
        assert(memcmp(copy + INO_CHECKSUM_HI, ino_raw + INO_CHECKSUM_HI, 2) ==
               0);
    }
    assert(storage_ext4_verify_inode_checksum(ino_raw, inode_size, ino_no,
                                              generation, v) == 0);
    /* Corruption inside the covered ranges is -EIO. */
    memcpy(copy, ino_raw, inode_size);
    copy[5] ^= 0x01u;
    assert(storage_ext4_verify_inode_checksum(copy, inode_size, ino_no,
                                              generation, v) == -RELIEFOS_EIO);
    memcpy(copy, ino_raw, inode_size);
    copy[INO_CHECKSUM_LO] ^= 0x01u;
    assert(storage_ext4_verify_inode_checksum(copy, inode_size, ino_no,
                                              generation, v) == -RELIEFOS_EIO);
    if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        memcpy(copy, ino_raw, inode_size);
        copy[INO_CHECKSUM_HI + 1] ^= 0x80u;
        assert(storage_ext4_verify_inode_checksum(copy, inode_size, ino_no,
                                                  generation, v) ==
               -RELIEFOS_EIO);
        memcpy(copy, ino_raw, inode_size);
        copy[inode_size - 1] ^= 0x20u;
        assert(storage_ext4_verify_inode_checksum(copy, inode_size, ino_no,
                                                  generation, v) ==
               -RELIEFOS_EIO);
    }
    /* Wrong inode number and wrong generation invalidate the checksum. */
    assert(storage_ext4_verify_inode_checksum(ino_raw, inode_size, ino_no + 1,
                                              generation, v) == -RELIEFOS_EIO);
    assert(storage_ext4_verify_inode_checksum(ino_raw, inode_size, 0,
                                              generation, v) == -RELIEFOS_EIO);
    assert(storage_ext4_verify_inode_checksum(ino_raw, inode_size, ino_no,
                                              generation + 1, v) ==
           -RELIEFOS_EIO);
    assert(storage_ext4_verify_inode_checksum(ino_raw, inode_size, ino_no,
                                              generation, v) == 0);
    /* update() is idempotent. */
    memcpy(copy, ino_raw, inode_size);
    assert(storage_ext4_update_inode_checksum(ino_raw, inode_size, ino_no,
                                              generation, v) == 0);
    assert(memcmp(copy, ino_raw, inode_size) == 0);
}

static void test_inode_checksum(void)
{
    struct storage_ext4_super_view v;
    uint8_t copy[4096];
    uint32_t i;

    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);

    /* 256-byte inode, UUID-derived seed. */
    build_inode(256, 32);
    inode_roundtrip(&v, 256, 2, 0x01020304u, SEED_FROM_UUID);
    inode_roundtrip(&v, 256, 17, 0x01020304u, SEED_FROM_UUID);
    puts("PASS inode checksum 256");

    /* 128-byte inode: only the low 16 bits live on disk. */
    build_inode(128, 0);
    inode_roundtrip(&v, 128, 2, 0x01020304u, SEED_FROM_UUID);
    /* update() must not touch anything outside 0x7C..0x7E. */
    build_inode(128, 0);
    memcpy(copy, ino_raw, 128);
    assert(storage_ext4_update_inode_checksum(ino_raw, 128, 2, 0x01020304u,
                                              &v) == 0);
    for (i = 0; i < 128; i++)
        if (i < INO_CHECKSUM_LO || i >= INO_CHECKSUM_LO + 2)
            assert(ino_raw[i] == copy[i]);
    puts("PASS inode checksum 128");

    /* 384-byte inode exercises a larger tail range. */
    build_inode(384, 32);
    inode_roundtrip(&v, 384, 11, 0x89abcdefu, SEED_FROM_UUID);

    /* CSUM_SEED feature: the field seed drives the inode chain. */
    build_view(&v,
               EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_CSUM_SEED,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 1);
    build_inode(256, 32);
    inode_roundtrip(&v, 256, 2, 0x01020304u, 0xd46d944bu);
    puts("PASS inode checksum csum_seed feature");

    /* Without metadata_csum: verify passes, update never writes. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS, 0, 0);
    build_inode(256, 32);
    t_put16(ino_raw + INO_CHECKSUM_LO, 0x2222u);
    t_put16(ino_raw + INO_CHECKSUM_HI, 0x3333u);
    memcpy(copy, ino_raw, 256);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(memcmp(copy, ino_raw, 256) == 0);
    puts("PASS inode checksum feature gate");

    /* Invalid inputs. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);
    build_inode(256, 32);
    assert(storage_ext4_verify_inode_checksum(NULL, 256, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_inode_checksum(NULL, 256, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0, NULL) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0, NULL) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 0, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 127, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 200, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 5000, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    assert(storage_ext4_update_inode_checksum(ino_raw, 0xffffffffu, 2, 0, &v) ==
           -RELIEFOS_EINVAL);
    /* Domain endpoints stay valid. */
    build_inode(EXT4_GOOD_OLD_INODE_SIZE, 0);
    assert(storage_ext4_update_inode_checksum(ino_raw, EXT4_GOOD_OLD_INODE_SIZE,
                                              2, 0x01020304u, &v) == 0);
    assert(storage_ext4_verify_inode_checksum(ino_raw, EXT4_GOOD_OLD_INODE_SIZE,
                                              2, 0x01020304u, &v) == 0);
    build_inode(EXT4_MAX_BLOCK_SIZE, 32);
    assert(storage_ext4_update_inode_checksum(ino_raw, EXT4_MAX_BLOCK_SIZE, 2,
                                              0x01020304u, &v) == 0);
    assert(storage_ext4_verify_inode_checksum(ino_raw, EXT4_MAX_BLOCK_SIZE, 2,
                                              0x01020304u, &v) == 0);
    puts("PASS inode checksum null/invalid size");
}

/* FITS_IN_INODE branch, pinned to goldens derived from e2fsprogs' own
 * ext2fs_inode_csum_set() (libext2fs 1.47.2) over these exact synthetic
 * buffers: image uuid 05681638-fc13-44fc-9ddf-50b4c859bc55 (= TEST_UUID),
 * ino 2, generation 0x01020304.  Goldens (lo16 or full32):
 *   sz=256 extra=32 -> 0x5059afe4   sz=256 extra=4  -> 0xfc992d60
 *   sz=256 extra=3  -> lo 0x32ff    sz=256 extra=0  -> lo 0x6119
 *   sz=128          -> lo 0x1fa7
 * For extra_isize < 4 e2fsprogs stores only the low 16 bits (hi slot left
 * untouched) and verifies the same way, matching linux inode.c:58-124. */
static void test_inode_checksum_fits(void)
{
    struct storage_ext4_super_view v;
    uint8_t copy[4096];
    uint32_t c;

    build_view(&v, EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_EXTENTS,
               EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 0);

    /* extra_isize=32: i_checksum_hi fits, full 32-bit checksum. */
    build_inode(256, 32);
    assert(ref_inode_csum(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u) ==
           0x5059afe4u);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == 0xafe4u);
    assert(t_get16(ino_raw + INO_CHECKSUM_HI) == 0x5059u);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    /* The hi slot is a checksum field here: corruption is -EIO. */
    ino_raw[INO_CHECKSUM_HI + 1] ^= 0x01u;
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    ino_raw[INO_CHECKSUM_HI + 1] ^= 0x01u;
    /* A buffer stamped with the non-fits formula must be rejected. */
    build_inode(256, 32);
    c = ref_inode_csum_ex(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u, 2);
    t_put16(ino_raw + INO_CHECKSUM_LO, (uint16_t)(c & 0xffffu));
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    puts("PASS inode checksum fits extra=32");

    /* extra_isize=4: boundary where i_checksum_hi still fits. */
    build_inode(256, 4);
    assert(ref_inode_csum(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u) ==
           0xfc992d60u);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == 0x2d60u);
    assert(t_get16(ino_raw + INO_CHECKSUM_HI) == 0xfc99u);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    puts("PASS inode checksum fits extra=4");

    /* extra_isize=3: i_checksum_hi does not fit.  Only the low 16 bits
     * live at 0x7C and 0x82 onward is plain hash input. */
    build_inode(256, 3);
    assert((ref_inode_csum(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u) &
            0xffffu) == 0x32ffu);
    memcpy(copy, ino_raw, 256);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == 0x32ffu);
    assert(memcmp(copy + INO_CHECKSUM_HI, ino_raw + INO_CHECKSUM_HI, 2) == 0);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    /* The 0x82 slot participates in the hash: corruption is -EIO. */
    ino_raw[INO_CHECKSUM_HI] ^= 0x01u;
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    ino_raw[INO_CHECKSUM_HI] ^= 0x01u;
    ino_raw[INO_CHECKSUM_HI + 1] ^= 0x80u;
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    ino_raw[INO_CHECKSUM_HI + 1] ^= 0x80u;
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    /* A buffer stamped with the fits formula must be rejected. */
    build_inode(256, 3);
    c = ref_inode_csum_ex(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u, 1);
    t_put16(ino_raw + INO_CHECKSUM_LO, (uint16_t)(c & 0xffffu));
    t_put16(ino_raw + INO_CHECKSUM_HI, (uint16_t)(c >> 16));
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    puts("PASS inode checksum nonfits extra=3");

    /* extra_isize=0: same non-fits semantics. */
    build_inode(256, 0);
    assert((ref_inode_csum(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u) &
            0xffffu) == 0x6119u);
    memcpy(copy, ino_raw, 256);
    assert(storage_ext4_update_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == 0x6119u);
    assert(memcmp(copy + INO_CHECKSUM_HI, ino_raw + INO_CHECKSUM_HI, 2) == 0);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == 0);
    ino_raw[INO_CHECKSUM_HI] ^= 0x01u;
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    ino_raw[INO_CHECKSUM_HI] ^= 0x01u;
    build_inode(256, 0);
    c = ref_inode_csum_ex(SEED_FROM_UUID, ino_raw, 256, 2, 0x01020304u, 1);
    t_put16(ino_raw + INO_CHECKSUM_LO, (uint16_t)(c & 0xffffu));
    t_put16(ino_raw + INO_CHECKSUM_HI, (uint16_t)(c >> 16));
    assert(storage_ext4_verify_inode_checksum(ino_raw, 256, 2, 0x01020304u,
                                              &v) == -RELIEFOS_EIO);
    puts("PASS inode checksum nonfits extra=0");

    /* 128-byte inode golden (lo only, no extra space at all). */
    build_inode(128, 0);
    assert((ref_inode_csum(SEED_FROM_UUID, ino_raw, 128, 2, 0x01020304u) &
            0xffffu) == 0x1fa7u);
    assert(storage_ext4_update_inode_checksum(ino_raw, 128, 2, 0x01020304u,
                                              &v) == 0);
    assert(t_get16(ino_raw + INO_CHECKSUM_LO) == 0x1fa7u);
    assert(storage_ext4_verify_inode_checksum(ino_raw, 128, 2, 0x01020304u,
                                              &v) == 0);
    puts("PASS inode checksum 128 golden");
}

/* ---- s_csum_seed selection ------------------------------------------- */

static void test_super_csum_seed(void)
{
    struct storage_ext4_super_view v;

    /* CSUM_SEED wins over everything and uses the raw field. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_CSUM_SEED, 0, 1);
    assert(storage_ext4_super_csum_seed(&v) == 0xd46d944bu);
    /* metadata_csum alone derives from the UUID. */
    build_view(&v, 0, EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, 1);
    assert(storage_ext4_super_csum_seed(&v) == SEED_FROM_UUID);
    /* ea_inode alone derives from the UUID too. */
    build_view(&v, EXT4_FEATURE_INCOMPAT_EA_INODE, 0, 1);
    assert(storage_ext4_super_csum_seed(&v) == SEED_FROM_UUID);
    /* Nothing set: 0. */
    build_view(&v, 0, 0, 1);
    assert(storage_ext4_super_csum_seed(&v) == 0);
    build_view(&v, EXT4_FEATURE_INCOMPAT_EXTENTS, EXT4_FEATURE_RO_COMPAT_GDT_CSUM, 1);
    assert(storage_ext4_super_csum_seed(&v) == 0);
    /* NULL view: 0. */
    assert(storage_ext4_super_csum_seed(NULL) == 0);
    puts("PASS super_csum_seed selection");
}

/* ---- Real mke2fs images --------------------------------------------- */

struct image_config {
    const char *tag;
    const char *features;
    int inode_size;
    int metadata_csum;
    int gdt_csum;
};

static void require_mke2fs(void)
{
    int st = system("command -v mke2fs >/dev/null 2>&1");

    if (st != 0) {
        fprintf(stderr,
                "FATAL: mke2fs not found in PATH; the ext4 checksum tests "
                "need e2fsprogs to build real reference images\n");
        abort();
    }
}

static void make_image(const char *path, const struct image_config *cfg)
{
    char cmd[512];
    FILE *p;
    int st;

    snprintf(cmd, sizeof(cmd),
             "mke2fs -t ext4 -F -q -b 4096 -I %d -O none,%s %s 8192 "
             ">/dev/null 2>&1",
             cfg->inode_size, cfg->features, path);
    p = popen(cmd, "w");
    if (p == NULL) {
        fprintf(stderr, "FATAL: popen(mke2fs) failed for %s\n", path);
        abort();
    }
    st = pclose(p);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "FATAL: mke2fs failed for %s (status %d): %s\n", path,
                st, cfg->features);
        abort();
    }
}

static void read_at(FILE *f, uint64_t off, uint8_t *buf, size_t len)
{
    assert(fseek(f, (long)off, SEEK_SET) == 0);
    assert(fread(buf, 1, len, f) == len);
}

static void run_image(const struct image_config *cfg, int index)
{
    char path[128];
    FILE *f;
    uint8_t sb[1024], gd[256], ino[4096], copy[4096];
    struct storage_ext4_super_view v;
    uint32_t block_size, first_data_block, desc_size, inode_size;
    uint64_t gdt_off, itable, ino_off;
    uint32_t generation;
    int gate_super;

    snprintf(path, sizeof(path), "/tmp/reliefos-ext4-csumtest-%d-%d.img",
             (int)getpid(), index);
    make_image(path, cfg);

    f = fopen(path, "rb");
    assert(f != NULL);
    read_at(f, EXT4_SUPERBLOCK_OFFSET, sb, sizeof(sb));
    assert(t_get16(sb + SB_MAGIC) == EXT4_SUPER_MAGIC);

    block_size = 1024u << t_get32(sb + SB_LOG_BLOCK_SIZE);
    first_data_block = t_get32(sb + SB_FIRST_DATA_BLOCK);
    desc_size = (t_get32(sb + SB_FEATURE_INCOMPAT) &
                 EXT4_FEATURE_INCOMPAT_64BIT)
                    ? t_get16(sb + SB_DESC_SIZE)
                    : EXT4_MIN_DESC_SIZE;
    inode_size = t_get16(sb + SB_INODE_SIZE);
    assert(desc_size == 32 || desc_size == 64);
    assert((uint32_t)cfg->inode_size == inode_size);

    /* View mirrors storage_ext4_parse_super's inputs for this image. */
    memset(&v, 0, sizeof(v));
    v.block_size = block_size;
    v.cluster_size = block_size;
    v.blocks_count = t_get32(sb + SB_BLOCKS_COUNT_LO);
    v.first_data_block = first_data_block;
    v.blocks_per_group = t_get32(sb + SB_BLOCKS_PER_GROUP);
    v.inodes_per_group = t_get32(sb + SB_INODES_PER_GROUP);
    v.inodes_count = t_get32(sb + SB_INODES_COUNT);
    v.free_inodes_count = t_get32(sb + SB_FREE_INODES_COUNT);
    v.free_blocks_count = t_get32(sb + SB_FREE_BLOCKS_COUNT_LO);
    v.desc_size = desc_size;
    v.inode_size = inode_size;
    v.feature_compat = t_get32(sb + SB_FEATURE_COMPAT);
    v.feature_incompat = t_get32(sb + SB_FEATURE_INCOMPAT);
    v.feature_ro_compat = t_get32(sb + SB_FEATURE_RO_COMPAT);
    v.journal_inum = t_get32(sb + SB_JOURNAL_INUM);
    v.checksum_seed = t_get32(sb + SB_CHECKSUM_SEED);
    memcpy(v.uuid, sb + SB_UUID, 16);
    assert(((v.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0) ==
           cfg->metadata_csum);
    assert(((v.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) != 0) ==
           cfg->gdt_csum);

    /* Superblock: verify what mke2fs wrote, tamper, repair byte-exactly. */
    gate_super = !cfg->metadata_csum;
    assert(storage_ext4_verify_super_checksum(sb, sizeof(sb)) == 0);
    sb[300] ^= 0x01u;
    if (gate_super) {
        /* No metadata_csum: corruption is invisible by design and
         * update() never writes. */
        assert(storage_ext4_verify_super_checksum(sb, sizeof(sb)) == 0);
        sb[300] ^= 0x01u;
        memcpy(copy, sb, sizeof(sb));
        assert(storage_ext4_update_super_checksum(sb, sizeof(sb)) == 0);
        assert(memcmp(copy, sb, sizeof(sb)) == 0);
    } else {
        assert(storage_ext4_verify_super_checksum(sb, sizeof(sb)) ==
               -RELIEFOS_EIO);
        sb[300] ^= 0x01u;
        assert(storage_ext4_verify_super_checksum(sb, sizeof(sb)) == 0);
        memcpy(copy, sb, sizeof(sb));
        memset(copy + SB_CHECKSUM, 0, 4);
        assert(storage_ext4_update_super_checksum(copy, sizeof(copy)) == 0);
        assert(memcmp(copy, sb, sizeof(sb)) == 0);
    }

    /* Group descriptor 0. */
    gdt_off = ((uint64_t)first_data_block + 1) * block_size;
    read_at(f, gdt_off, gd, desc_size);
    assert(storage_ext4_verify_group_checksum(gd, desc_size, 0, &v) == 0);
    gd[3] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(gd, desc_size, 0, &v) ==
           -RELIEFOS_EIO);
    gd[3] ^= 0x01u;
    assert(storage_ext4_verify_group_checksum(gd, desc_size, 0, &v) == 0);
    if (desc_size > 32) {
        gd[desc_size - 1] ^= 0x40u;
        assert(storage_ext4_verify_group_checksum(gd, desc_size, 0, &v) ==
               -RELIEFOS_EIO);
        gd[desc_size - 1] ^= 0x40u;
    }
    memcpy(copy, gd, desc_size);
    memset(copy + GD_CHECKSUM, 0, 2);
    assert(storage_ext4_update_group_checksum(copy, desc_size, 0, &v) == 0);
    assert(memcmp(copy, gd, desc_size) == 0);

    /* Root inode (#2) in group 0's inode table. */
    itable = t_get32(gd + GD_INODE_TABLE_LO);
    if (desc_size >= 64)
        itable |= (uint64_t)t_get32(gd + GD_INODE_TABLE_HI) << 32;
    ino_off = itable * block_size + (2 - 1) * (uint64_t)inode_size;
    read_at(f, ino_off, ino, inode_size);
    generation = t_get32(ino + INO_GENERATION);

    if (cfg->metadata_csum) {
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 2,
                                                  generation, &v) == 0);
        ino[5] ^= 0x01u;
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 2,
                                                  generation, &v) ==
               -RELIEFOS_EIO);
        ino[5] ^= 0x01u;
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 2,
                                                  generation, &v) == 0);
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 3,
                                                  generation, &v) ==
               -RELIEFOS_EIO);
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 2,
                                                  generation ^ 0x5a5a5a5au,
                                                  &v) == -RELIEFOS_EIO);
        memcpy(copy, ino, inode_size);
        memset(copy + INO_CHECKSUM_LO, 0, 2);
        if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
            memset(copy + INO_CHECKSUM_HI, 0, 2);
        assert(storage_ext4_update_inode_checksum(copy, inode_size, 2,
                                                  generation, &v) == 0);
        assert(memcmp(copy, ino, inode_size) == 0);
    } else {
        /* No metadata_csum: verify is a no-op and update never writes. */
        memcpy(copy, ino, inode_size);
        assert(storage_ext4_verify_inode_checksum(ino, inode_size, 2,
                                                  generation, &v) == 0);
        assert(storage_ext4_update_inode_checksum(ino, inode_size, 2,
                                                  generation, &v) == 0);
        assert(memcmp(copy, ino, inode_size) == 0);
    }

    fclose(f);
    assert(unlink(path) == 0);
    printf("PASS real image %s\n", cfg->tag);
}

static void test_real_images(void)
{
    static const struct image_config configs[] = {
        /* The exact task-3 image: metadata_csum + 64bit + journal. */
        {"metadata_csum_64bit_i256",
         "metadata_csum,64bit,has_journal,extents", 256, 1, 0},
        /* CSUM_SEED feature: seed comes from s_checksum_seed. */
        {"metadata_csum_seed_i256",
         "metadata_csum,64bit,has_journal,extents,metadata_csum_seed", 256, 1,
         0},
        /* 32-bit descriptors and 128-byte inodes. */
        {"metadata_csum_desc32_i128", "metadata_csum,extents", 128, 1, 0},
        /* Legacy gdt_csum path: uninit_bg is mke2fs's name for GDT_CSUM. */
        {"gdt_csum_desc32_i256", "uninit_bg,extents", 256, 0, 1},
        {"gdt_csum_desc64_i256", "uninit_bg,64bit,extents", 256, 0, 1},
    };
    uint32_t i;

    require_mke2fs();
    for (i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
        run_image(&configs[i], (int)i);
}

int main(void)
{
    /* Unbuffered: PASS lines stay visible even when an assert aborts. */
    setvbuf(stdout, NULL, _IONBF, 0);
    fill_patterns();
    test_crc32c_vectors();
    test_crc16_vectors();
    test_super_csum_seed();
    test_super_checksum();
    test_group_checksum();
    test_inode_checksum();
    test_inode_checksum_fits();
    test_real_images();
    puts("PASS ext4_checksum_test");
    return 0;
}
