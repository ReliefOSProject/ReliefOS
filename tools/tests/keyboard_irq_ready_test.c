/* Regression: a controller reply consumed with IRQs masked leaves a stale
 * IRQ1 pending; reading an empty data port must not publish a phantom key. */
#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/arch/x86_64/irq.c"

static uint8_t controller_status, controller_byte;
static unsigned data_reads, input_events, eois;
static uint8_t last_key, last_pressed;

uint8_t x86_64_inb(uint16_t port)
{
    if (port == 0x64) return controller_status;
    assert(port == 0x60);
    ++data_reads;
    return controller_byte;
}
void x86_64_outb(uint8_t value, uint16_t port)
{
    assert(value == 0x20 && port == 0x20);
    ++eois;
}
void input_handle_scancode(uint8_t key, uint8_t pressed)
{
    ++input_events; last_key = key; last_pressed = pressed;
}
void pci_irq_dispatch(uint32_t vector) { (void)vector; }
void apic_eoi(void) {}
void time_on_tick(void) {}
uint64_t time_uptime_us(void) { return 0; }
uint8_t input_caps_lock_active(void) { return 0; }
struct task *userland_schedule_from_frame(struct trap_frame *frame)
{ (void)frame; return NULL; }
void smp_release_aps(void) {}
void driver_manager_mouse_poll(void) {}
uint32_t smp_current_cpu(void) { return 0; }
void sched_on_cpu_tick(void) {}
void smp_membarrier_poll(void) {}

int main(void)
{
    struct trap_frame frame = {.vector = 0x21, .cs = 0x8};
    controller_status = 0x1c; controller_byte = 0x47;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 0 && input_events == 0 && eois == 1);
    controller_status = 0x21; controller_byte = 0x08;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 0 && input_events == 0 && eois == 2);
    controller_status = 0x01; controller_byte = 0x1e;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 1 && input_events == 1 && last_key == 30 && last_pressed == 1);
    controller_byte = 0x9e;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 2 && input_events == 2 && last_key == 30 && last_pressed == 0);
    controller_byte = 0xfa;
    assert(irq_dispatch(&frame) == NULL);
    assert(data_reads == 3 && input_events == 2 && eois == 5);
    puts("PASS IRQ1: no empty/AUX read, real key make/break and ACK handling preserved");
    return 0;
}
