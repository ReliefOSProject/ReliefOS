#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/block/storage_internal.h"
#include "../../kernel/reliefnt/block/storage_state_data.c"
static unsigned commands, last_command, transport_calls;
static int transport_result;
static unsigned char port_status = 0x40;
void storage_memzero(void *buffer, size_t length) { memset(buffer, 0, length); }
uint8_t x86_64_inb(uint16_t port) { (void)port; return port_status; }
void x86_64_outb(uint8_t value, uint16_t port)
{ if ((port & 7) == 7) { ++commands; last_command = value; } }
#include "../../kernel/reliefnt/drivers/storage/ide/storage_ide.c"
void storage_volume_ide_device(const struct storage_volume *volume, struct ide_device_info *device)
{ assert(volume->ready); *device = (struct ide_device_info){.present = 1, .command_base = 0x1f0, .control_base = 0x3f6}; }
int ahci_flush_cache(struct ahci_hba_port *port)
{ assert(port && !storage_io_async_context); ++transport_calls; return transport_result; }
int nvme_flush_cache(struct nvme_controller *controller, uint32_t nsid)
{ assert(controller && nsid == 1 && !storage_io_async_context); ++transport_calls; return transport_result; }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
#include "../../kernel/reliefnt/block/storage_sync.c"
int storage_ext4_journal_commit(struct storage_volume *v, bool wait)
{ (void)wait; assert(!v->ext4.journal); return 0; }
int storage_ext4_journal_checkpoint(struct storage_volume *v)
{ assert(!v->ext4.journal); return 0; }
int main(void)
{
    storage_io_async_context = true;
    g_volumes[0] = (struct storage_volume){.ready = true, .kind = STORAGE_VOLUME_IDE, .transport = STORAGE_TRANSPORT_IDE_PIO};
    assert(storage_sync_volume(0) == 0 && commands == 1 && last_command == 0xe7 && storage_io_async_context);
    port_status = 0x41;
    assert(storage_sync_volume(0) == -5 && commands == 1 && storage_io_async_context);
    g_volumes[0].kind = STORAGE_VOLUME_AHCI;
    g_volumes[0].transport = STORAGE_TRANSPORT_AHCI;
    g_volumes[0].hba_port = (void *)4096;
    transport_result = -5;
    assert(storage_sync_volume(0) == -5 && transport_calls == 1 && storage_io_async_context);
    g_volumes[0].kind = STORAGE_VOLUME_NVME;
    g_volumes[0].transport = STORAGE_TRANSPORT_NVME;
    g_volumes[0].nvme = (void *)4096;
    g_volumes[0].nvme_nsid = 1;
    transport_result = -110;
    assert(storage_sync_volume(0) == -110 && transport_calls == 2);
    assert(storage_sync_volume(1) == -19 && storage_sync_volume(STORAGE_MAX_VOLUMES) == -19);
    g_volumes[1] = g_volumes[0];
    assert(storage_sync_all() == -110 && transport_calls == 4);
    puts("PASS production sync routing: ATA command, device errors, all-volume attempts and async state restoration");
}
