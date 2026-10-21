/* Catch wrong extent length decoding, missed corruption, indirect arithmetic,
 * stale cached reads and per-block I/O regressions using the actual backend.
 * Disk fixture offsets below are taken independently from Linux's disk format;
 * real mke2fs/debugfs images provide the external oracle. */
#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/block/core/storage_internal.h"

static uint8_t *disk;
static uint64_t disk_len, reads, writes;
static int fail_read;
static struct storage_volume volume;
#ifndef EXT4_JOURNAL_TRANSPORT_FIXTURE
int storage_ext4_device_flush(const struct storage_volume *v) { (void)v; return 0; }
#endif
void *kernel_malloc(size_t n) { return malloc(n); }
void kernel_free(void *p) { free(p); }
int time_wall_clock(struct reliefos_time_info *info)
{ memset(info,0,sizeof(*info)); info->unix_seconds=1790640000; return 0; }
uint64_t mm_alloc_pages(uint32_t n) { return (uintptr_t)calloc(n, 4096); }
void console_printf(const char *fmt, ...) { (void)fmt; }

void console_log(enum console_log_level level, const char *fmt, ...)
{
    (void)level; (void)fmt;
}
void storage_memzero(void *p, size_t n) { memset(p, 0, n); }
void storage_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
int storage_read_device(const struct storage_volume *v, uint64_t lba,
                        uint32_t n, void *out)
{
    (void)v;
    reads++;
    if (fail_read) return -RELIEFOS_EIO;
    assert(lba <= disk_len / 512 && n <= disk_len / 512 - lba);
    memcpy(out, disk + lba * 512, (size_t)n * 512);
    return 0;
}
int storage_write_device(const struct storage_volume *v, uint64_t lba,
                         uint32_t n, const void *in)
{
    (void)v;
    writes++;
    assert(lba <= disk_len / 512 && n <= disk_len / 512 - lba);
    memcpy(disk + lba * 512, in, (size_t)n * 512);
    return 0;
}

static void w16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void w32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; i++) p[i] = n >> (8 * i); }
static void header(uint8_t *p, unsigned entries, unsigned max, unsigned depth)
{ w16(p, 0xf30a); w16(p+2, entries); w16(p+4, max); w16(p+6, depth); }
static void extent(uint8_t *p, uint32_t logical, uint64_t physical, unsigned len)
{ w32(p, logical); w16(p+4, len); w16(p+6, physical >> 32); w32(p+8, physical); }
static void index_entry(uint8_t *p, uint32_t logical, uint64_t block)
{ w32(p, logical); w32(p+4, block); w16(p+8, block >> 32); }
static uint8_t *block(unsigned n) { return disk + volume.ext_start_lba * 512 + n * 1024; }
static void invalidate(void) { storage_ext4_cache_invalidate(&volume); }
static struct ext4_inode_view inode(void)
{
    struct ext4_inode_view in = {0};
    in.mode = 0100644; in.flags = 0x80000; in.generation = 71;
    in.size = 128 * 1024; header(in.i_block_raw, 2, 4, 0);
    extent(in.i_block_raw+12, 2, 100, 4);
    extent(in.i_block_raw+24, 10, 110, 0x8003);
    return in;
}
static void expect_map(struct ext4_inode_view *in, uint64_t logical,
                       uint64_t physical, uint32_t len, bool hole, bool unwritten)
{
    struct storage_ext4_map_result map;
    assert(storage_ext4_map_block(&volume, 12, in, logical, false, &map) == 0);
    assert(map.physical == physical && map.length == len);
    assert(map.hole == hole && map.unwritten == unwritten);
}
static void corrupt(struct ext4_inode_view *in)
{
    struct storage_ext4_map_result map;
    assert(storage_ext4_map_block(&volume, 12, in, 2, false, &map) == -RELIEFOS_EIO);
}
static void checksum_node(uint8_t *node, struct ext4_inode_view *in)
{
    /* Independent bitwise Castagnoli oracle: no production CRC helper. */
    uint8_t prefix[8]; w32(prefix, 12); w32(prefix+4, in->generation);
    uint32_t crc = 0x12345678;
    for (unsigned j = 0; j < 8 + 1020; j++) {
        crc ^= j < 8 ? prefix[j] : node[j-8];
        for (unsigned k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    w32(node+1020, crc);
}
static void unit(void)
{
    disk_len = 2 * 1024 * 1024 + 512;
    disk = calloc(1, disk_len); assert(disk);
    volume.ext_start_lba = 1; volume.ext_sector_count = (disk_len-512)/512;
    volume.mount_generation = 1;
    volume.ext4.block_size = 1024; volume.ext4.blocks_count = 2048;
    volume.ext4.inodes_count = 128; volume.ext4.inode_size = 256;
    volume.ext4.inodes_per_group = 128; volume.ext4.blocks_per_group = 2048;
    volume.ext4.group_count = 1; volume.ext4.desc_size = 32;
    volume.ext4.first_data_block = 1;
    volume.ext4.super_view.feature_incompat = 0x40;
    struct ext4_inode_view in = inode();
    expect_map(&in, 0, 0, 2, true, false);
    expect_map(&in, 3, 101, 3, false, false);
    expect_map(&in, 6, 0, 4, true, false);
    expect_map(&in, 11, 111, 2, false, true);
    struct storage_ext4_map_result map;
    assert(storage_ext4_map_block(&volume, 12, &in, UINT32_MAX, false, &map) == -27);
    in.i_block_raw[4] = 5; corrupt(&in);
    in = inode(); w16(in.i_block_raw+16, 0); corrupt(&in);
    in = inode(); w32(in.i_block_raw+24, 3); corrupt(&in);
    in = inode(); w32(in.i_block_raw+20, 2047); corrupt(&in);
    in = inode(); w32(in.i_block_raw+12, UINT32_MAX-1); corrupt(&in);
    in = inode();
    header(in.i_block_raw, 1, 4, 2); index_entry(in.i_block_raw+12, 2, 20);
    header(block(20), 1, 84, 1); index_entry(block(20)+12, 2, 21);
    header(block(21), 1, 84, 0); extent(block(21)+12, 2, 100, 4);
    expect_map(&in, 2, 100, 4, false, false);
    uint64_t before = reads;
    expect_map(&in, 3, 101, 3, false, false); assert(reads == before);
    /* Corrupt cached header too: every hit must still validate structure. */
    uint8_t *cached; assert(storage_ext4_cache_get(&volume, 21, &cached, false) == 0);
    w16(cached+6, 1); corrupt(&in); invalidate();
    volume.ext4.super_view.feature_ro_compat = 0x400;
    volume.ext4.super_view.feature_incompat |= 0x2000;
    volume.ext4.super_view.checksum_seed = 0x12345678;
    checksum_node(block(20), &in); checksum_node(block(21), &in);
    expect_map(&in, 2, 100, 4, false, false);
    block(21)[1019] ^= 1; invalidate(); corrupt(&in);
    block(21)[1019] ^= 1; invalidate();
    in.generation++; corrupt(&in); in.generation--;
    volume.ext4.super_view.feature_ro_compat = 0;
    w32(block(21)+12, 1); invalidate(); corrupt(&in);
    w32(block(21)+12, 2); w32(block(20)+16, 20); invalidate(); corrupt(&in);
    /* Direct + all three levels, and a missing subtree that spans 256 blocks. */
    memset(&in, 0, sizeof(in)); in.mode = 0100644;
    w32(in.i_block_raw, 100); w32(in.i_block_raw+4, 101);
    w32(in.i_block_raw+48, 30); w32(block(30), 110);
    w32(in.i_block_raw+52, 31); w32(block(31), 32); w32(block(32), 120);
    w32(in.i_block_raw+56, 33); w32(block(33), 34);
    w32(block(34), 35); w32(block(35), 130); invalidate();
    expect_map(&in, 0, 100, 2, false, false);
    expect_map(&in, 12, 110, 1, false, false);
    expect_map(&in, 268, 120, 1, false, false);
    expect_map(&in, 524, 0, 256, true, false);
    expect_map(&in, 65804, 130, 1, false, false);
    assert(storage_ext4_map_block(&volume, 12, &in, 16843020, false, &map) == -27);
    w32(block(34), 33); invalidate();
    assert(storage_ext4_map_block(&volume, 12, &in, 65804, false, &map) == -RELIEFOS_EIO);
    /* Linux treats 0x8000 as initialized, not an empty unwritten extent. */
    in = inode(); header(in.i_block_raw, 1, 4, 0);
    extent(in.i_block_raw+12, 0, 100, 0x8000);
    volume.ext4.blocks_count = 65536;
    expect_map(&in, 0, 100, 32768, false, false);
    volume.ext4.blocks_count = 2048;
    /* Byte reads preserve holes, unwritten zeros, EOF and partial blocks. */
    in = inode(); in.size = 13 * 1024;
    memset(block(100), 0x5a, 4*1024); memset(block(110), 0xab, 3*1024);
    uint8_t output[16*1024]; uint32_t got;
    assert(storage_ext4_read_file_range(&volume, 12, &in, 512, output, sizeof(output), &got) == 0);
    assert(got == 13*1024-512);
    for (unsigned i = 0; i < got; i++)
        assert(output[i] == ((i+512 >= 2048 && i+512 < 6144) ? 0x5a : 0));
    assert(storage_ext4_read_file_range(&volume, 12, &in, UINT64_MAX, output, 1, &got) == 0 && got == 0);
    /* Large reads must batch transport I/O, while preserving dirty cache data. */
    volume.ext4.fs_error = 0; /* Fresh synthetic mount after corruption tests. */
    in = inode(); in.size = 64 * 1024;
    header(in.i_block_raw, 1, 4, 0); extent(in.i_block_raw+12, 0, 100, 64);
    uint8_t *large = malloc(64*1024); assert(large); invalidate();
    before = reads;
    assert(storage_ext4_read_file_range(&volume, 12, &in, 0, large, 64*1024, &got) == 0);
    assert(got == 64*1024 && reads-before <= 2);
    assert(storage_ext4_cache_get(&volume, 101, &cached, true) == 0);
    cached[0] = 0x77; assert(storage_ext4_cache_mark_dirty(&volume, 101) == 0);
    assert(storage_ext4_read_file_range(&volume, 12, &in, 0, large, 64*1024, &got) == 0);
    assert(large[1024] == 0x77); invalidate();
    fail_read = 1;
    assert(storage_ext4_read_file_range(&volume, 12, &in, 0, large, 64*1024, &got) == -RELIEFOS_EIO);
    assert(got == 0); fail_read = 0;
    /* Inode field parsing catches huge-file block units and short extra area. */
    uint8_t raw[256] = {0}; struct ext4_inode_view parsed;
    w16(raw, 0100644); w32(raw+4, 123); w32(raw+108, 2);
    w32(raw+28, 3); w32(raw+32, 0x40000); w16(raw+116, 1);
    volume.ext4.super_view.feature_ro_compat = 8;
    volume.ext4.super_view.block_size = 1024;
    assert(storage_ext4_parse_inode(raw, sizeof(raw), &volume.ext4.super_view, &parsed) == 0);
    assert(parsed.size == 8589934715ULL && parsed.blocks == 8589934598ULL);
    w16(raw+128, 255);
    assert(storage_ext4_parse_inode(raw, sizeof(raw), &volume.ext4.super_view, &parsed) == -RELIEFOS_EINVAL);
    free(large); free(disk); disk = NULL;
    assert(writes == 0);
    puts("PASS synthetic extent/indirect/corruption/cache/read tests");
}

static uint8_t *load(const char *path, uint64_t *len)
{
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); assert(n >= 0);
    rewind(f); uint8_t *p = malloc((size_t)n + 1); assert(p);
    assert(fread(p, 1, n, f) == (size_t)n); fclose(f); *len = n; return p;
}
static void image_test(char **argv)
{
    uint64_t expected_len; disk = load(argv[1], &disk_len);
    uint8_t *expected = load(argv[3], &expected_len);
    struct storage_ext4_super_view sb;
    assert(storage_ext4_parse_super(disk+1024, 1024, &sb) == 0);
    volume.ext4.super_view = sb; volume.ext4.block_size = sb.block_size;
    volume.ext4.blocks_count = sb.blocks_count; volume.ext4.group_count = sb.group_count;
    volume.ext4.desc_size = sb.desc_size; volume.ext4.first_data_block = sb.first_data_block;
    volume.ext4.inode_size = sb.inode_size; volume.ext4.inodes_count = sb.inodes_count;
    volume.ext4.inodes_per_group = sb.inodes_per_group; volume.ext4.blocks_per_group = sb.blocks_per_group;
    volume.ext_sector_count = disk_len/512; volume.mount_generation = 1;
    struct ext4_inode_view in; uint64_t ino = strtoull(argv[2], NULL, 10);
    assert(storage_ext4_read_inode(&volume, ino, &in) == 0);
    assert(in.size == expected_len);
    uint8_t *out = malloc(128*1024); assert(out); uint32_t got;
    for (uint64_t offset = 0; offset < expected_len;) {
        assert(storage_ext4_read_file_range(&volume, ino, &in, offset, out, 128*1024, &got) == 0);
        assert(got && got <= expected_len-offset && memcmp(out, expected+offset, got) == 0);
        offset += got;
    }
    for (unsigned i = 0; i < 100; i++) {
        uint64_t offset = (i * 7919ULL) % expected_len;
        assert(storage_ext4_read_file_range(&volume, ino, &in, offset, out, 1513, &got) == 0);
        assert(got && memcmp(out, expected+offset, got) == 0);
    }
    struct storage_ext4_fiemap_extent maps[128]; uint32_t count;
    assert(storage_ext4_fiemap(&volume, ino, 0, in.size, maps, 128, &count) == 0);
    assert(count && count <= 128);
    for (unsigned i = 0; i < count; i++) {
        assert(maps[i].length && maps[i].logical < in.size);
        assert(maps[i].physical && maps[i].physical + maps[i].length <= disk_len);
        assert(memcmp(disk + maps[i].physical, expected + maps[i].logical, maps[i].length) == 0);
    }
    assert(storage_ext4_fiemap(&volume, ino, 0, in.size, NULL, 0, &count) == 0 && count);
    assert(storage_ext4_fiemap(&volume, ino, 31, 100, maps, 1, &count) == 0);
    assert(count == 1 && maps[0].logical == 31 && maps[0].length == 100);
    assert(!(maps[0].flags & EXT4_FIEMAP_LAST));
    assert(storage_ext4_fiemap(&volume, ino, (in.size+sb.block_size-1)/sb.block_size*sb.block_size,
                              UINT64_MAX, maps, 128, &count) == 0 && count == 0);
    if (sb.feature_ro_compat & 0x400) {
        /* Break a real inode checksum, not a synthetic mirror of the parser. */
        struct storage_ext4_group_view group;
        assert(storage_ext4_read_group(&volume, (ino-1)/sb.inodes_per_group, &group) == 0);
        uint64_t offset = group.inode_table * sb.block_size + ((ino-1)%sb.inodes_per_group)*sb.inode_size;
        disk[offset+8] ^= 1; invalidate();
        assert(storage_ext4_read_inode(&volume, ino, &in) == -RELIEFOS_EIO);
        disk[offset+8] ^= 1; invalidate();
    }
    assert(writes == 0);
    printf("PASS image block_size=%u inode=%llu bytes=%llu reads=%llu\n", sb.block_size,
           (unsigned long long)ino, (unsigned long long)in.size, (unsigned long long)reads);
    invalidate(); free(disk); free(expected); free(out);
}
int main(int argc, char **argv)
{ if (argc == 1) unit(); else { assert(argc == 4); image_test(argv); } return 0; }
