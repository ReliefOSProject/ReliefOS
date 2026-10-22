#define main unused_ext_mount_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

/** @brief Match Linux exclusive-open mount conflicts without denying siblings. */
int main(void)
{
    g_install_disk_count = 2;
    g_install_disks[0].present = g_install_disks[1].present = 1;
    g_volumes[0] = (struct storage_volume){.ready = true,
        .filesystem = STORAGE_FILESYSTEM_EXT4, .source_disk_id = 0,
        .ext_device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(0, 2))};
    assert(storage_disk_block_busy(0, -1) == 1);
    assert(storage_disk_block_busy(0, 2) == 1);
    assert(storage_disk_block_busy(0, 0) == 0);
    assert(storage_disk_block_busy(1, -1) == 0);
    g_volumes[STORAGE_VOLUME_BOOT] = (struct storage_volume){.ready = true,
        .filesystem = STORAGE_FILESYSTEM_FAT32, .source_disk_id = 0,
        .esp_device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(0, 1))};
    assert(storage_disk_block_busy(0, 1) == 1 && storage_disk_block_busy(0, 0) == 0);
    g_volumes[0].ram_base = (void *)1;
    g_volumes[STORAGE_VOLUME_BOOT].ready = false;
    assert(storage_disk_block_busy(0, -1) == 0);
    /* Optical mounts have no install-disk dev_t; their zero-initialized
     * source_disk_id must not claim the unrelated first writable disk. */
    g_volumes[STORAGE_VOLUME_DYNAMIC_FIRST] = (struct storage_volume){
        .ready = true, .filesystem = STORAGE_FILESYSTEM_ISO9660,
        .kind = STORAGE_VOLUME_AHCI, .transport = STORAGE_TRANSPORT_AHCI};
    assert(storage_disk_block_busy(0, -1) == 0);
    assert(storage_disk_block_busy(0, 0) == 0);
    assert(storage_disk_block_busy(99, -1) == -22);
    puts("PASS exclusive block open: mounted disk/partition busy; siblings, other disks, RAM and unrelated optical mounts excluded");
    return 0;
}
