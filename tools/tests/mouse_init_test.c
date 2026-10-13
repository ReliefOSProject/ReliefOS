#include <assert.h>
#include <stdio.h>

#include "../../kernel/reliefnt/drivers/input/mouse/mouse.c"

static uint8_t aux_next;
static uint8_t ack_pending;
static uint8_t mouse_bytes[16];
static unsigned mouse_count;
static const uint8_t wheel_packets[] = {0x08, 0, 0, 1, 0x08, 0, 0, 0x0f};
static unsigned packet_read = sizeof(wheel_packets);
static int32_t wheel_events[2];
static unsigned wheel_count;

static uint8_t controller_inb(uint16_t port)
{
    if (port == PS2_STATUS)
        return ack_pending || packet_read < sizeof(wheel_packets) ? 0x21 : 0;
    if (port == PS2_DATA && packet_read < sizeof(wheel_packets))
        return wheel_packets[packet_read++];
    assert(port == PS2_DATA && ack_pending);
    ack_pending = 0;
    return 0xfa;
}

static void pointer_event(int32_t x, int32_t y, int32_t dx, int32_t dy, uint8_t buttons)
{
    (void)x; (void)y; (void)dx; (void)dy; (void)buttons;
}

static void wheel_event(int32_t x, int32_t y, int32_t wheel, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    assert(wheel_count < 2);
    wheel_events[wheel_count++] = wheel;
}

static void controller_outb(uint16_t port, uint8_t value)
{
    if (port == PS2_COMMAND) {
        assert(value == 0xd4);
        aux_next = 1;
        return;
    }
    assert(port == PS2_DATA);
    if (aux_next) {
        assert(mouse_count < sizeof(mouse_bytes));
        mouse_bytes[mouse_count++] = value;
    }
    /* Both devices ACK writes, so an ACK alone cannot prove routing. */
    aux_next = 0;
    ack_pending = 1;
}

int main(void)
{
    static const struct reliefos_driver_kernel_api api = {
        .inb = controller_inb, .outb = controller_outb,
        .input_push_mouse = pointer_event, .input_push_mouse_wheel = wheel_event,
    };
    static const uint8_t expected[] = {0xe8, 3, 0xf3, 200, 0xf4};
    kernel_api = &api;
    assert(mouse_write_param(0xe8, 3));
    assert(mouse_write_param(0xf3, 200));
    assert(mouse_write_ack(0xf4));
    if (mouse_count != sizeof(expected)) {
        fprintf(stderr, "mouse received %u bytes, expected 5: parameters went to keyboard\n",
                mouse_count);
        return 1;
    }
    for (unsigned i = 0; i < sizeof(expected); ++i) assert(mouse_bytes[i] == expected[i]);
    assert(vmware_axis_position(0, 1280) == 0);
    assert(vmware_axis_position(65535, 1280) == 1279);
    assert(vmware_axis_position(65535, 800) == 799);
    assert(vmware_axis_position(32768, 1280) == 639);
    assert(vmware_axis_position(65535, 1) == 0);
    assert(vmware_axis_position(65535, 0) == 0);
    packet_size = 4;
    packet_read = 0;
    mouse_hardware_poll();
    assert(wheel_count == 2 && wheel_events[0] == -1 && wheel_events[1] == 1);
    puts("Mouse initialization tests passed: command parameters reach the auxiliary port");
    return 0;
}
