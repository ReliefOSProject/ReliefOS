#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/block/storage_internal.h"
#include "../../kernel/reliefnt/block/storage_state_data.c"

static FILE *disk;
static unsigned locked;
static int read_error;
static uint32_t test_inode;
static long inode_offset;

void kernel_execution_lock_irqsave(uint64_t *flags) { assert(!locked); locked = 1; *flags = 17; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { assert(locked && flags == 17); locked = 0; }
int time_wall_clock(struct reliefos_time_info *value) { value->unix_seconds = 1800000000; return 0; }
void storage_memcpy(void *dst, const void *src, size_t count) { memcpy(dst, src, count); }
int storage_select_node_volume(const struct storage_node *node, struct storage_volume **previous)
{ assert(locked && node->volume_id == 0); *previous = g_active_volume; return 0; }
void storage_restore_volume(struct storage_volume *previous) { assert(previous == g_active_volume && locked); }
int storage_read_sectors(uint64_t lba, uint32_t count, void *out)
{
    if (read_error) return read_error;
    assert(!fseek(disk, lba * 512, SEEK_SET));
    return fread(out, 512, count, disk) == count ? 0 : -5;
}
int ext2_read_inode(uint32_t number, struct ext2_inode *out)
{
    assert(number == test_inode);
    if (read_error) return read_error;
    assert(!fseek(disk, inode_offset, SEEK_SET));
    return fread(out, sizeof(*out), 1, disk) == 1 ? 0 : -5;
}
static int ext2_write_inode(uint32_t number, const struct ext2_inode *in)
{
    assert(number == test_inode && locked);
    assert(!fseek(disk, inode_offset, SEEK_SET));
    return fwrite(in, sizeof(*in), 1, disk) == 1 && !fflush(disk) ? 0 : -5;
}
int ext2_group_desc(uint32_t number, struct ext2_group_desc *out)
{
    if (read_error) return read_error;
    assert(!fseek(disk, 2048 + number * sizeof(*out), SEEK_SET));
    return fread(out, sizeof(*out), 1, disk) == 1 ? 0 : -5;
}
int fat32_read_fat_entry(uint32_t cluster, uint32_t *out)
{ *out = cluster == 2 || cluster == 4 ? 0 : 0xfffffff; return read_error; }
int exfat_bitmap_get_cached(uint32_t cluster, uint8_t *out)
{ *out = cluster != 3 && cluster != 5; return read_error; }

/* Unit-test boundaries follow the unified backend; full-facade ext4_vfs_test
 * independently checks these operations against checksummed real images. */
int storage_ext4_check_node(struct storage_volume *v,const struct storage_node *node,struct ext4_inode_view *out)
{
    (void)v; struct ext2_inode raw; int ret=ext2_read_inode(node->first_cluster,&raw); if (ret<0) return ret;
    *out=(struct ext4_inode_view){.mode=raw.mode,.uid=raw.uid|((uint32_t)raw.osd2[4]<<16)|((uint32_t)raw.osd2[5]<<24),
        .gid=raw.gid|((uint32_t)raw.osd2[6]<<16)|((uint32_t)raw.osd2[7]<<24),.links_count=raw.links_count,
        .size=raw.size_lo|((uint64_t)raw.size_high<<32),.blocks=raw.blocks_512,
        .atime=raw.atime,.mtime=raw.mtime,.ctime=raw.ctime}; memcpy(out->i_block_raw,raw.block,60); return 0;
}
int storage_ext4_write_inode(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in)
{
    (void)v; struct ext2_inode raw; int ret=ext2_read_inode(ino,&raw); if (ret<0) return ret;
    raw.mode=in->mode; raw.uid=in->uid; raw.gid=in->gid; raw.atime=in->atime; raw.mtime=in->mtime; raw.ctime=in->ctime;
    raw.osd2[4]=in->uid>>16; raw.osd2[5]=in->uid>>24; raw.osd2[6]=in->gid>>16; raw.osd2[7]=in->gid>>24;
    return ext2_write_inode(ino,&raw);
}
int storage_ext4_journal_start(struct storage_volume *v,uint32_t credits,struct storage_ext4_handle *h)
{ assert(locked); *h=(struct storage_ext4_handle){v,credits,true}; return 0; }
void storage_ext4_journal_abort(struct storage_ext4_handle *h,int error) { (void)h;(void)error; }
int storage_ext4_journal_stop(struct storage_ext4_handle *h) { assert(h->active); h->active=false; return 0; }
uint32_t ext4_get_le32(const uint8_t *p)
{ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
const struct storage_ext4_operations storage_ext4_ops={0};

#include "../../kernel/reliefnt/fs/storage_permissions.c"
#include "../../kernel/reliefnt/fs/storage_statfs.c"

int main(int argc, char **argv)
{
    assert(argc == 3);
    disk = fopen(argv[1], "r+b");
    assert(disk);
    test_inode = strtoul(argv[2], NULL, 10);
    struct ext2_superblock super;
    assert(!fseek(disk, 1024, SEEK_SET) && fread(&super, sizeof(super), 1, disk) == 1);
    assert(super.log_block_size == 0 && super.magic == EXT2_SUPER_MAGIC);
    g_storage.kind = STORAGE_VOLUME_AHCI;
    g_storage.filesystem = STORAGE_FILESYSTEM_EXT2;
    g_storage.ext2_block_size = 1024;
    g_storage.ext4.block_size = 1024;
    g_storage.ext2_inode_size = super.inode_size;
    g_storage.ext2_inodes_per_group = super.inodes_per_group;
    g_storage.ext2_group_count = (super.blocks_count - super.first_data_block + super.blocks_per_group - 1) / super.blocks_per_group;
    struct ext2_group_desc group;
    assert(!ext2_group_desc((test_inode - 1) / super.inodes_per_group, &group));
    inode_offset = group.inode_table * 1024L + ((test_inode - 1) % super.inodes_per_group) * super.inode_size;
    struct storage_node node = {.flags = STORAGE_NODE_FLAG_EXT2, .first_cluster = test_inode};
    struct reliefos_permissions mode;
    assert(!storage_inode_permissions(&node, &mode, false));
    assert(mode.mode == 0644 && mode.uid == 0 && mode.gid == 0);
    mode = (struct reliefos_permissions){06750, 70001, 90002};
    assert(!storage_inode_permissions(&node, &mode, true));
    struct linux_stat_abi st;
    assert(!storage_inode_stat(&node, &st));
    assert(st.st_mode == (LINUX_S_IFREG | 06750) && st.st_uid == 70001 && st.st_gid == 90002);
    assert(st.st_ino == test_inode && st.st_nlink == 1 && st.st_size == 5);
    assert(st.ctime_sec == 1800000000 && st.st_blocks == 2);
    struct linux_statfs_abi fs;
    assert(!storage_statfs(&node, &fs));
    assert(fs.f_bfree == super.free_blocks_count && fs.f_ffree == super.free_inodes_count);
    assert(fs.f_files == super.inodes_count && fs.f_bsize == 1024 && fs.f_type == 0xef53);
    g_storage.kind = STORAGE_VOLUME_RAM;
    assert(!storage_statfs(&node, &fs) && !(fs.f_flags & LINUX_ST_RDONLY));
    g_storage.kind = STORAGE_VOLUME_AHCI;
    assert(fs.f_bavail == fs.f_bfree - super.reserved_blocks_count && fs.f_blocks < super.blocks_count);
    read_error = -5;
    assert(storage_inode_permissions(&node, &mode, false) == -5 && !locked);
    assert(storage_statfs(&node, &fs) == -5 && !locked);
    read_error = 0;
    g_storage.filesystem = STORAGE_FILESYSTEM_FAT32;
    g_storage.cluster_bytes = 4096;
    g_storage.data_cluster_count = 4;
    assert(!storage_statfs(&node, &fs) && fs.f_blocks == 4 && fs.f_bfree == 2 && fs.f_type == 0x4d44);
    g_storage.filesystem = STORAGE_FILESYSTEM_EXFAT;
    g_storage.exfat_cluster_count = 4;
    assert(!storage_statfs(&node, &fs) && fs.f_bfree == 2 && fs.f_type == 0x2011bab0);
    g_storage.filesystem = STORAGE_FILESYSTEM_ISO9660;
    g_storage.iso_block_size = 2048;
    g_storage.iso_sector_count = 100;
    assert(!storage_statfs(&node, &fs) && fs.f_bfree == 0 && fs.f_blocks == 100 && (fs.f_flags & LINUX_ST_RDONLY));
    assert(!fclose(disk));
    puts("PASS inode ownership/mode/ctime, ext2 allocation counts, FAT/exFAT bitmaps, ISO read-only statfs");
}
