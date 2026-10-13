#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/block/storage_internal.h"
#include "../../kernel/reliefnt/block/storage_state_data.c"

#define EXT4_BLOCK_CACHE_ENTRIES 2u
#include "../../kernel/reliefnt/fs/storage_ext4_cache.c"

static uint8_t disk[64 * 4096];
static uint8_t output[8 * 4096];
static unsigned commands, fail_cache, pending, high_dma;
static void *pending_buffer;
static int device_error;

uint64_t mm_alloc_pages(uint32_t pages)
{
    if (fail_cache == 2 || (fail_cache && pages == 3)) return 0;
    if (high_dma && pages == 32) return UINT64_C(0x100000000);
    return (uintptr_t)calloc(pages, 4096);
}
void storage_memzero(void *out, size_t len) { memset(out, 0, len); }
void storage_memcpy(void *out, const void *in, size_t len) { memcpy(out, in, len); }
int storage_read_device(const struct storage_volume *v, uint64_t lba,
                        uint32_t sectors, void *buffer)
{
    (void)v;
    /* A caller's virtual buffer cannot be used as the DMA physical address. */
    uintptr_t address = (uintptr_t)buffer;
    assert(address < (uintptr_t)output || address >= (uintptr_t)output + sizeof(output));
    assert(address != UINT64_C(0x100000000)); /* AHCI has a 32-bit DMA address. */
    ++commands;
    if (pending == 1) {
        pending_buffer = buffer;
        pending = 2;
        return -RELIEFOS_EAGAIN;
    }
    if (pending == 2) {
        assert(buffer == pending_buffer);
        pending = 0;
    }
    if (device_error) return device_error;
    assert(lba * 512 + sectors * 512 <= sizeof(disk));
    memcpy(buffer, disk + lba * 512, sectors * 512);
    return 0;
}
int storage_write_device(const struct storage_volume *v, uint64_t lba,
                         uint32_t sectors, const void *buffer)
{ (void)v; (void)lba; (void)sectors; (void)buffer; return 0; }
int storage_ext4_journal_capture(struct storage_volume *v, uint64_t block, const uint8_t *data)
{ (void)v; (void)block; (void)data; return 0; }
int storage_ext4_journal_publish(struct storage_volume *v, uint64_t block, const uint8_t *data)
{ (void)v; (void)block; (void)data; return 0; }
bool storage_ext4_journal_owns(const struct storage_volume *v, uint64_t block)
{ (void)v; (void)block; return false; }

int main(int argc, char **argv)
{
    struct storage_volume v = {0};
    v.ext4.block_size = 4096;
    v.ext4.blocks_count = 64;
    v.ext_sector_count = 64 * 8;
    v.mount_generation = 1;
    for (unsigned i = 0; i < sizeof(disk); ++i) disk[i] = (i * 17 + i / 4096 + 3) % 251;
    fail_cache = argc > 1 && !strcmp(argv[1], "uncached");
    if (argc > 1 && !strcmp(argv[1], "no-memory")) fail_cache = 2;
    high_dma = argc > 1 && !strcmp(argv[1], "high-memory");
    assert(storage_ext4_cache_read_blocks(&v, 4, 8, output) == 0);
    assert(commands == 1);
    assert(!memcmp(output, disk + 4 * 4096, sizeof(output)));
    memset(output, 0x5a, sizeof(output));
    pending = 1;
    assert(storage_ext4_cache_read_blocks(&v, 20, 8, output) == -RELIEFOS_EAGAIN);
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0x5a);
    assert(storage_ext4_cache_read_blocks(&v, 20, 8, output) == 0);
    assert(!memcmp(output, disk + 20 * 4096, sizeof(output)));
    memset(output, 0x5a, sizeof(output));
    device_error = -RELIEFOS_EIO;
    assert(storage_ext4_cache_read_blocks(&v, 40, 8, output) == -RELIEFOS_EIO);
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0x5a);
    device_error = 0;
    assert(storage_ext4_cache_read(&v, 39, output) == 0);
    assert(!memcmp(output, disk + 39 * 4096, 4096));
    if (!fail_cache) {
        uint8_t *cached;
        assert(storage_ext4_cache_get(&v, 40, &cached, true) == 0);
        memset(cached, 0x7e, 4096);
        assert(storage_ext4_cache_mark_dirty(&v, 40) == 0);
        assert(storage_ext4_cache_read_blocks(&v, 39, 3, output) == 0);
        assert(!memcmp(output, disk + 39 * 4096, 4096));
        for (unsigned i = 4096; i < 8192; ++i) assert(output[i] == 0x7e);
        assert(!memcmp(output + 8192, disk + 41 * 4096, 4096));
    }
    puts("PASS ext4 DMA reads: staging, batching, retry lifetime, errors and dirty cache");
    return 0;
}
