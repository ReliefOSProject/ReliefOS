/* Regression for keyboard bytes stolen by the IRQ12 mouse output-buffer drain.
 * The real mouse driver and IRQ dispatcher share mocked 8042 I/O so this
 * verifies byte ownership and then follows the keyboard byte through IRQ1. */
#include <assert.h>
#include <stdio.h>

#include "../../kernel/reliefnt/arch/x86_64/port.h"
#include "../../kernel/reliefnt/drivers/input/mouse/mouse.c"
#include "../../kernel/reliefnt/arch/x86_64/irq.c"

struct controller_byte {
    uint8_t status;
    uint8_t data;
};

static struct controller_byte controller_bytes[16];
static unsigned controller_count;
static unsigned controller_head;
static unsigned mouse_events;
static unsigned key_events;
static uint8_t key_codes[8];
static uint8_t key_pressed[8];
static unsigned eois;

static void queue_controller_byte(uint8_t status, uint8_t data)
{
    assert(controller_count < sizeof(controller_bytes) / sizeof(controller_bytes[0]));
    controller_bytes[controller_count++] = (struct controller_byte){status, data};
}

static uint8_t mock_inb(uint16_t port)
{
    if (port == PS2_STATUS)
        return controller_head < controller_count ? controller_bytes[controller_head].status : 0;
    assert(port == PS2_DATA && controller_head < controller_count);
    return controller_bytes[controller_head++].data;
}

static void mock_outb(uint16_t port, uint8_t value)
{
    assert(port == 0x20 && value == 0x20);
    ++eois;
}

static void mock_mouse_event(int32_t x, int32_t y, int32_t dx, int32_t dy,
                             uint8_t buttons)
{
    (void)x;
    (void)y;
    (void)dx;
    (void)dy;
    (void)buttons;
    ++mouse_events;
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

void input_handle_scancode(uint8_t key, uint8_t pressed)
{
    assert(key_events < sizeof(key_codes));
    key_codes[key_events] = key;
    key_pressed[key_events] = pressed;
    ++key_events;
}

void pci_irq_dispatch(uint32_t vector) { (void)vector; }
void apic_eoi(void) {}
void time_on_tick(void) {}
uint64_t time_uptime_us(void) { return 0; }
uint8_t input_caps_lock_active(void) { return 0; }
struct task *userland_schedule_from_frame(struct trap_frame *frame)
{ (void)frame; return NULL; }
void smp_release_aps(void) {}
void driver_manager_mouse_poll(void) { mouse_hardware_poll(); }
uint32_t smp_current_cpu(void) { return 0; }
void sched_on_cpu_tick(void) {}
void smp_membarrier_poll(void) {}

static void dispatch_keyboard_irq(void)
{
    struct trap_frame frame = {.vector = 0x21, .cs = 0x8};
    assert(irq_dispatch(&frame) == NULL);
}

static void queue_mouse_packet(void)
{
    queue_controller_byte(0x21, 0x08);
    queue_controller_byte(0x21, 0x00);
    queue_controller_byte(0x21, 0x00);
}

int main(void)
{
    static const struct reliefos_driver_kernel_api api = {
        .inb = mock_inb,
        .outb = mock_outb,
        .input_push_mouse = mock_mouse_event,
    };
    kernel_api = &api;

    /* Seed the driver's key state so the following Alt break is observable. */
    queue_controller_byte(0x01, 0x38);
    dispatch_keyboard_irq();
    assert(key_events == 1 && key_codes[0] == 56 && key_pressed[0] == 1);

    /* IRQ12 drains one mouse packet, encounters Alt-up and another key-down,
     * then a second mouse packet. Keyboard bytes must remain for IRQ1. */
    queue_mouse_packet();
    queue_controller_byte(0x01, 0xb8);
    queue_controller_byte(0x01, 0x1e);
    queue_mouse_packet();

    mouse_hardware_poll();

    dispatch_keyboard_irq();
    dispatch_keyboard_irq();
    assert(key_events == 3);
    assert(key_codes[1] == 56 && key_pressed[1] == 0);
    assert(key_codes[2] == 30 && key_pressed[2] == 1);
    assert(mouse_events == 1);
    assert(controller_head == 6);

    /* Once IRQ1 consumes the keyboard bytes, the queued AUX packet proceeds. */
    mouse_hardware_poll();
    assert(mouse_events == 2);
    assert(controller_head == controller_count);
    assert(eois == 3);
    puts("PASS PS/2 interleaving: IRQ12 leaves keyboard bytes for IRQ1");
    return 0;
}
