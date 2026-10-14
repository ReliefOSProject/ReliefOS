#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/block/storage_internal.h"
#include "../../kernel/reliefnt/block/storage_state.c"
#include "../../kernel/reliefnt/block/storage_state_data.c"
#include "../../kernel/reliefnt/fs/storage_ext2_cache.c"
#include "../../kernel/reliefnt/drivers/storage/ide/storage_ide.c"
#include "../../kernel/reliefnt/drivers/storage/ahci/storage_ahci.c"
#include "../../kernel/reliefnt/drivers/storage/nvme/storage_nvme.c"

static uint8_t media[32768];
static unsigned commands, reject_batch, fail_sector;
static int read_result;
static unsigned read_failures;
static int fake_ahci_read(struct ahci_hba_port *port, uint64_t lba,
                          uint32_t sectors, void *buffer)
{
    assert(port && lba == 100 && sectors == 8);
    if (!read_result) memcpy(buffer, media, sectors * 512);
    return read_result;
}
static int fake_ahci_write(struct ahci_hba_port *port, uint64_t lba,
                            uint32_t sectors, const void *buffer)
{
    assert(port && lba >= 100 && lba + sectors <= 164 && sectors <= 64);
    ++commands;
    if (reject_batch && sectors > 1) {
        memcpy(media + (lba - 100) * 512, buffer, 512);
        return -5;
    }
    if (fail_sector && lba == 103) return -5;
    memcpy(media + (lba - 100) * 512, buffer, sectors * 512);
    return 0;
}
#define ahci_write_lba_retry fake_ahci_write
#define ahci_read_lba_retry fake_ahci_read
#include "../../kernel/reliefnt/block/storage_block.c"
#undef ahci_write_lba_retry
#undef ahci_read_lba_retry

void console_printf(const char *format, ...)
{ if (strstr(format, "device read failed")) ++read_failures; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; if (strstr(format, "device read failed")) ++read_failures;
}
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
uint32_t smp_cpu_count(void) { return 1; }
uint8_t x86_64_inb(uint16_t port) { (void)port; assert(0); return 0; }
uint16_t x86_64_inw(uint16_t port) { (void)port; assert(0); return 0; }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; assert(0); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; assert(0); }
uint64_t time_ticks(void) { return 1; }
void exfat_cache_invalidate(void) {}
void storage_ext4_cache_invalidate_write(const struct storage_volume *writer,
                                         uint64_t lba, uint32_t count)
{
    (void)writer;
    (void)lba;
    (void)count;
}

int main(void)
{
    struct ahci_hba_port port = {0};
    g_storage.kind = STORAGE_VOLUME_AHCI;
    g_storage.transport = STORAGE_TRANSPORT_AHCI;
    g_storage.hba_port = &port;
    uint8_t data[32768];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 17);
    assert(!storage_write_sectors(100, 64, data));
    assert(!memcmp(data, media, sizeof(data)) && commands == 1);
    memset(media, 0, sizeof(media));
    commands = 0;
    reject_batch = 1;
    assert(!storage_write_sectors(100, 64, data));
    assert(!memcmp(data, media, sizeof(data)) && commands == 65);
    fail_sector = 1;
    assert(storage_write_sectors(100, 64, data) == -5);
    read_result = -RELIEFOS_EAGAIN;
    assert(storage_read_device(&g_storage, 100, 8, data) == -RELIEFOS_EAGAIN);
    assert(read_failures == 0);
    read_result = 0;
    assert(storage_read_device(&g_storage, 100, 8, data) == 0);
    assert(!memcmp(data, media, 4096));
    read_result = -5;
    assert(storage_read_device(&g_storage, 100, 8, data) == -5);
    assert(read_failures == 1);
    puts("PASS bounded AHCI write batching, partial-command retry and error propagation");
}
