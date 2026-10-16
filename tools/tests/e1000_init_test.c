#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../kernel/reliefnt/drivers/net/e1000/e1000.c"

static unsigned allocations, allocation_limit = 1024;
static int registration_error;
static uint16_t command_bits;
static uint8_t *registers;
static uint64_t allocate(uint32_t count)
{
    if (allocations == allocation_limit) return 0;
    void *p = mmap(NULL, count * 4096u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(p != MAP_FAILED);
    ++allocations;
    return (uintptr_t)p;
}
static void release(uint64_t p, uint32_t count)
{ assert(allocations); --allocations; assert(munmap((void *)(uintptr_t)p, count * 4096u) == 0); }
static int find(uint16_t vendor, uint16_t device, struct reliefos_driver_pci_device *out)
{
    if (vendor != 0x8086 || device != 0x100f) return -1;
    *out = (struct reliefos_driver_pci_device){.vendor_id = vendor, .device_id = device, .class_code = 2};
    return 0;
}
static uint32_t read32(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ (void)b; (void)s; (void)f; return offset == 0x10 ? (uintptr_t)registers : 0; }
static uint16_t read16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ (void)b; (void)s; (void)f; (void)offset; return command_bits; }
static void write16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset, uint16_t value)
{ (void)b; (void)s; (void)f; (void)offset; command_bits = value; }
static void pause_ms(uint64_t ms)
{ (void)ms; *(uint32_t *)(registers + E1000_REG_CTRL) &= ~(1u << 26); }
static void log_text(const char *text) { (void)text; }
static void log_record(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
static int register_ops(const struct reliefos_driver_e1000_ops *ops)
{ assert(ops->is_ready()); return registration_error; }

int main(void)
{
    registers = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(registers != MAP_FAILED);
    *(uint32_t *)(registers + E1000_REG_STATUS) = 1u << 1;
    *(uint32_t *)(registers + E1000_REG_RAL) = 0x06290c00;
    *(uint32_t *)(registers + E1000_REG_RAH) = E1000_RAH_AV | 0x9729;
    const struct reliefos_driver_kernel_api api = {
        .abi_version = RELIEFOS_DRIVER_ABI_VERSION, .struct_size = sizeof(api),
        .alloc_pages = allocate, .free_pages = release, .console_write = log_text,
        .console_log = log_record,
        .pci_find = find, .pci_read16 = read16, .pci_write16 = write16,
        .pci_read32 = read32, .sleep_ms = pause_ms, .register_e1000 = register_ops,
    };
    allocation_limit = 1;
    assert(e1000_driver_init(&api) < 0);
    assert(allocations == 0);
    allocation_limit = 100;
    if (E1000_RX_COUNT > allocation_limit) {
        assert(e1000_driver_init(&api) < 0);
        assert(allocations == 0);
    }
    allocation_limit = 1024;
    registration_error = -16;
    assert(e1000_driver_init(&api) == -16);
    assert(allocations == 0 && !g_e1000.active);
    registration_error = 0;
    assert(e1000_driver_init(&api) == 0);
    assert(command_bits & PCI_COMMAND_BUS_MASTER);
    assert(*(uint32_t *)(registers + E1000_REG_CTRL) & (1u << 6));
    assert(!memcmp(g_e1000.mac, "\x00\x0c\x29\x06\x29\x97", 6));
    *(uint32_t *)(registers + E1000_REG_STATUS) = 0;
    struct module_e1000_info info;
    e1000_get_info(&info);
    assert(info.present && !info.active && !e1000_is_ready());
    *(uint32_t *)(registers + E1000_REG_STATUS) = 1u << 1;
    assert(e1000_is_ready());
    unsigned char frame[2048];
    uint32_t length;
    g_e1000.rx[0].length = 65535;
    g_e1000.rx[0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
    assert(e1000_poll(frame, sizeof(frame), &length) == 0 && length == 0);
    /* A full TCP window arrives before the next poll, including ring wrap. */
    for (unsigned burst = 0; burst < 5; ++burst) {
        unsigned queued = 0;
        for (unsigned packet = 0; packet < 64; ++packet) {
            unsigned index = (g_e1000.rx_index + packet) % E1000_RX_COUNT;
            if (g_e1000.rx[index].status & E1000_RX_STATUS_DD) break;
            memset(g_e1000.rx_buf[index], packet, 1460);
            g_e1000.rx[index].length = 1460;
            g_e1000.rx[index].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
            ++queued;
        }
        assert(queued == 64);
        for (unsigned packet = 0; packet < queued; ++packet) {
            assert(e1000_poll(frame, sizeof(frame), &length) == 1 && length == 1460);
            for (unsigned byte = 0; byte < length; ++byte) assert(frame[byte] == packet);
        }
        assert(e1000_poll(frame, sizeof(frame), &length) == 0 && length == 0);
    }
    e1000_driver_fini();
    assert(!allocations && !g_e1000.active);
    assert(munmap(registers, 0x20000) == 0);
    puts("PASS e1000 initialization, VMware MAC, link setup, RX bounds and failure cleanup");
}
