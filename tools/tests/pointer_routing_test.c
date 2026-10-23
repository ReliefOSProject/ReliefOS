#include <assert.h>
#include <sys/un.h>
#include <reliefos/syscall.h>
#include <reliefnt/input.h>
#include <reliefnt/lock.h>
#include <reliefnt/storage.h>

void input_routing_fixture_checks(void);

static long test_read(long number, long fd, long buffer, long length);
#define syscall3 test_read
#define WINDOWD_VT_TEST
static int windowd_vt_active(void);
#define main windowd_program_main
#include "../../userland/apps/windowd/main.c"
#undef main
#undef WINDOWD_VT_TEST
#undef syscall3
#include "../../userland/runtime/src/ui_input.c"

static uint64_t read_cursor;
static struct reliefos_input_event delivered[128];
static unsigned delivered_count;
static int vt_active = 1;
static int windowd_vt_active(void) { return vt_active; }

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



int main(void)
{
    input_routing_fixture_checks();
    test_caps_lock_routing();
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
