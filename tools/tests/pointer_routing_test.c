#include <assert.h>
#include <sys/un.h>
#include <reliefos/syscall.h>
#include <reliefnt/input.h>
#include <reliefnt/lock.h>
#include <reliefnt/storage.h>

static long test_read(long number, long fd, long buffer, long length);
#define syscall3 test_read
#define WINDOWD_VT_TEST
static int windowd_vt_active(void);
#define main windowd_program_main
#include "../../userland/apps/windowd/main.c"
#undef main
#undef WINDOWD_VT_TEST
#undef syscall3
#include "../../kernel/reliefnt/kernel/reliefnt/input.c"
#include "../../userland/runtime/src/ui_input.c"
#include "../../kernel/reliefnt/arch/x86_64/keyboard_led.h"

static uint64_t read_cursor;
static struct reliefos_input_event delivered[128];
static unsigned delivered_count;
static int vt_active = 1;
static unsigned console_key_calls;
uint32_t pty_vt_active(void) { return 1; }
void pty_console_key_event(uint8_t keycode, uint8_t pressed)
{ (void)keycode; (void)pressed; ++console_key_calls; }

static int windowd_vt_active(void) { return vt_active; }

uint64_t time_uptime_us(void) { return 0; }
const struct framebuffer *framebuffer_get(void)
{
    static const struct framebuffer fb = {.available = 1, .width = 1280, .height = 800};
    return &fb;
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    (void)lock;
    *flags = 0;
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)lock;
    (void)flags;
}

static long test_read(long number, long fd, long buffer, long length)
{
    assert(number == SYS_read && (fd == 10 || fd == 12));
    return input_evdev_read_vt(fd == 10 ? STORAGE_DEV_KIND_MOUSE : STORAGE_DEV_KIND_KEYBOARD, &read_cursor,
                               (void *)buffer, (uint32_t)length, 0, 1);
}

int reliefos_ipc_send(int fd, uint32_t type, const void *payload, uint32_t length)
{
    assert(fd == 11 && type == RELIEFOS_WIN_MSG_INPUT);
    assert(length == sizeof(delivered[0]) && delivered_count < 128);
    delivered[delivered_count++] = *(const struct reliefos_input_event *)payload;
    return 0;
}

static void test_evdev_damaged_records(void)
{
    struct input_event events[3];
    uint64_t cursor = 0;
    input_init();
    /* Reproduce the two damaged sequence numbers from the VMware core. */
    while (evdev_next_sequence < 5842) {
        evdev_publish(STORAGE_DEV_KIND_MOUSE, EV_SYN, SYN_REPORT, 0);
    }
    evdev_queue[5103 % INPUT_EVDEV_QUEUE_CAP].sequence = 0xffffffa1u;
    evdev_queue[5104 % INPUT_EVDEV_QUEUE_CAP].sequence = 0;
    assert(!input_evdev_available(STORAGE_DEV_KIND_KEYBOARD, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor,
                            events, sizeof(events), 0) == 0);
    assert(cursor == 5842);

    input_push_key(30, 1);
    assert(input_evdev_available(STORAGE_DEV_KIND_KEYBOARD, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor,
                            events, sizeof(events), 0) == sizeof(events));
    assert(events[0].type == EV_LED && events[0].code == LED_CAPSL && events[0].value == 0);
    assert(events[1].type == EV_KEY && events[1].code == 30 && events[1].value == 1);
    assert(events[2].type == EV_SYN && events[2].code == SYN_REPORT);
    assert(cursor == input_evdev_cursor_now());

    /* The scan must also terminate when every retained record is damaged. */
    for (unsigned i = 0; i < INPUT_EVDEV_QUEUE_CAP; ++i) {
        evdev_queue[i].sequence = 0;
    }
    cursor = 0;
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor,
                            events, sizeof(events), 0) == 0);
    assert(cursor == input_evdev_cursor_now());
}

static void test_caps_lock_routing(void)
{
    char ch;
    input_init();
    input_set_graphical_vt(1);
    delivered_count = 0;
    policy_slot = 0;
    clients[0].fd = 11;
    input_push_key(KEY_CAPSLOCK, 1);
    assert(input_caps_lock_active());
    input_push_key(KEY_CAPSLOCK, 1); /* Typematic is not another lock transition. */
    assert(input_caps_lock_active());
    input_push_key(KEY_CAPSLOCK, 0);
    /* A new reader/app never saw Caps Lock being pressed. */
    read_cursor = input_evdev_cursor_now();
    input_push_key(30, 1);
    input_push_key(30, 0);
    input_push_key(KEY_CAPSLOCK, 1);
    input_push_key(KEY_CAPSLOCK, 0);
    input_push_key(30, 1);
    assert(!input_caps_lock_active());
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    assert(delivered_count == 5);
    assert(delivered[0].modifiers == RELIEFOS_INPUT_MOD_CAPS_LOCK);
    /* Queued letters use historical state even though the lock is now off. */
    for (unsigned i = 0; i < 2; ++i) {
        reliefos_ui_set_keyboard_modifiers(delivered[0].modifiers);
        assert(reliefos_ui_keycode_to_char_shift(30, 0, &ch) && ch == 'A');
        assert(reliefos_ui_keycode_to_char_shift(30, 1, &ch) && ch == 'a');
        assert(reliefos_ui_keycode_to_char_shift(2, 0, &ch) && ch == '1');
        assert(reliefos_ui_keycode_to_char_shift(2, 1, &ch) && ch == '!');
    }
    reliefos_ui_set_keyboard_modifiers(delivered[4].modifiers);
    assert(reliefos_ui_keycode_to_char_shift(30, 0, &ch) && ch == 'a');
    assert(reliefos_ui_keycode_to_char_shift(30, 1, &ch) && ch == 'A');
    _Static_assert(sizeof(struct reliefos_input_event) == 24, "input wire size");
    _Static_assert(sizeof(struct reliefos_gui_app_event) == 36, "app wire size");
    puts("Caps Lock routing passed: repeats, late readers, ordered snapshots and Shift XOR");
    delivered_count = 0;
}

static void test_keyboard_led_protocol(void)
{
    struct keyboard_led_command state = {.applied = 0xff};
    uint8_t byte;
    assert(!keyboard_led_next(&state, 0, 0, 1, &byte));
    assert(keyboard_led_next(&state, 0, 0, 0, &byte) && byte == 0xed);
    assert(!keyboard_led_reply(&state, 30));
    assert(!keyboard_led_next(&state, 4, 1, 0, &byte));
    assert(keyboard_led_reply(&state, 0xfa));
    assert(keyboard_led_next(&state, 4, 2, 0, &byte) && byte == 0);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(state.applied == 0);
    assert(keyboard_led_next(&state, 4, 3, 0, &byte) && byte == 0xed);
    assert(keyboard_led_reply(&state, 0xfe));
    assert(keyboard_led_next(&state, 4, 4, 0, &byte) && byte == 0xed);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(keyboard_led_next(&state, 4, 5, 0, &byte) && byte == 4);
    assert(keyboard_led_reply(&state, 0xfa));
    assert(!keyboard_led_next(&state, 4, 6, 0, &byte));
    assert(state.applied == 4);
    assert(keyboard_led_next(&state, 0, 7, 0, &byte));
    for (unsigned i = 1; i <= 3; ++i)
        assert(keyboard_led_next(&state, 0, 7 + i * 100000, 0, &byte));
    assert(!keyboard_led_next(&state, 0, 400007, 0, &byte));
    assert(!keyboard_led_next(&state, 0, 400008, 0, &byte));
    puts("PS/2 LEDs passed: ACK, RESEND, busy controller and bounded timeout recovery");
}

static void test_absolute_frames_resynchronize(void)
{
    struct input_event ev;
    uint64_t cursor;
    int saw_x = 0, saw_y = 0, saw_syn = 0;
    input_init();
    input_push_mouse(100, 200, 100, 200, 0);
    /* A reader that misses this frame (a wrapped ring under a VMware-rate
     * burst) must still resynchronize on the next one: the surviving frame
     * carries the authoritative absolute position, which is the contract
     * xorg.conf's IgnoreAbsoluteAxes "false" relies on. A delta-only view of
     * this frame is (800,500) and can never recover the lost (100,200)
     * frame. */
    cursor = input_evdev_cursor_now();
    input_push_mouse(900, 700, 800, 500, 0);
    while (input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, &ev, sizeof(ev), 0) ==
           (int)sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_X) {
            assert(ev.value == 900);
            saw_x = 1;
        }
        if (ev.type == EV_ABS && ev.code == ABS_Y) {
            assert(ev.value == 700);
            saw_y = 1;
        }
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            saw_syn = 1;
        }
    }
    assert(saw_x && saw_y && saw_syn);
    puts("Absolute resync passed: a lost motion frame heals from the next EV_ABS");
}

int main(void)
{
    input_init();
    input_handle_scancode(30, 1);
    assert(console_key_calls == 0); /* Physical IRQ must not render a VT. */
    input_process_pending();
    assert(console_key_calls == 1);
    test_keyboard_led_protocol();
    test_evdev_damaged_records();
    test_caps_lock_routing();
    test_absolute_frames_resynchronize();
    input_init();
    input_set_graphical_vt(1);
    read_cursor = input_evdev_cursor_now();
    policy_slot = 0;
    clients[0].fd = 11;
    display_state.fb_width = 1280;
    display_state.fb_height = 800;
    /* The driver starts at the framebuffer center, independently of windowd. */
    input_push_mouse(900, 550, 260, 150, 0);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 1);
    assert(delivered[0].x == 900 && delivered[0].y == 550);

    /* A button and both coordinates must be observed as one input packet. */
    input_push_mouse(1279, 799, 379, 249, 1);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 2);
    assert(delivered[1].x == 1279 && delivered[1].y == 799);
    assert(delivered[1].buttons == 1);
    input_push_mouse(0, 0, -1279, -799, 0);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 3 && delivered[2].x == 0 && delivered[2].y == 0);
    assert(delivered[2].buttons == 0);

    /* A lost old motion packet must not offset later absolute positions. */
    input_push_mouse(400, 300, 400, 300, 0);
    read_cursor = input_evdev_cursor_now();
    input_push_mouse(950, 650, 550, 350, 0);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 4);
    assert(delivered[3].x == 950 && delivered[3].y == 650);
    input_push_mouse_wheel(950, 650, -1, 0);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 5 && delivered[4].type == RELIEFOS_INPUT_MOUSE_WHEEL);
    assert(delivered[4].x == 950 && delivered[4].y == 650 && delivered[4].dy == -1);
    /* An inactive graphical session must drain devices without taking input
     * away from the current text terminal or replaying old events on return. */
    vt_active = 0;
    input_set_graphical_vt(0);
    input_push_key(30, 1);
    input_push_mouse(600, 400, -350, -250, 0);
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(delivered_count == 5);
    vt_active = 1;
    input_set_graphical_vt(1);
    input_push_key(48, 1);
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    assert(delivered[delivered_count - 1].keycode == 48);
    unsigned before_switch = delivered_count;
    /* A stalled windowd must not replay text-VT input after switching back. */
    vt_active = 0;
    input_set_graphical_vt(0);
    input_push_key(31, 1);
    vt_active = 1;
    input_set_graphical_vt(1);
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    for (unsigned i = before_switch; i < delivered_count; ++i)
        assert(delivered[i].keycode != 31);
    input_push_key(KEY_LEFTSHIFT, 1);
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    vt_active = 0;
    input_set_graphical_vt(0);
    input_push_key(KEY_LEFTSHIFT, 0);
    vt_active = 1;
    input_set_graphical_vt(1);
    before_switch = delivered_count;
    pump_input_device(12, RELIEFOS_INPUT_KEYBOARD);
    int shift_released = 0;
    for (unsigned i = before_switch; i < delivered_count; ++i)
        if (delivered[i].keycode == KEY_LEFTSHIFT && !delivered[i].pressed) shift_released = 1;
    assert(shift_released);
    /* Release on a text VT must clear a drag even if windowd was paused. */
    input_push_mouse(600, 400, 0, 0, 1);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(cursor_buttons == 1);
    vt_active = 0;
    input_set_graphical_vt(0);
    input_push_mouse(600, 400, 0, 0, 0);
    vt_active = 1;
    input_set_graphical_vt(1);
    pump_input_device(10, RELIEFOS_INPUT_MOUSE);
    assert(cursor_buttons == 0);
    assert(delivered[delivered_count - 1].buttons == 0);
    struct input_absinfo info;
    assert(input_evdev_absinfo(ABS_X, &info) == 0);
    assert(info.value == 600 && info.minimum == 0 && info.maximum == 1279);
    assert(input_evdev_absinfo(ABS_Y, &info) == 0);
    assert(info.value == 400 && info.maximum == 799);
    uint8_t bits[8];
    input_evdev_capabilities(STORAGE_DEV_KIND_MOUSE, 0, bits, sizeof(bits));
    assert(bits[0] & (1u << EV_ABS));
    input_evdev_capabilities(STORAGE_DEV_KIND_MOUSE, EV_ABS, bits, sizeof(bits));
    assert((bits[0] & 3u) == 3u);
    puts("Pointer routing passed: exact positions, packet boundaries, wheel and resynchronization");
    return 0;
}
