/* Regression for Linux evdev loss notification on the shared input ring. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/reliefnt/drivers/tty/input/input.c"

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

/* This fixture covers ring semantics, not poll waiter wakeups. */
void kernel_poll_readiness_notify(void) {}

uint64_t time_uptime_us(void)
{
    static uint64_t now;
    return ++now;
}

static int has_key_state(uint16_t code)
{
    uint8_t state[(KEY_CNT + 7U) / 8U];
    input_evdev_key_state(state, sizeof(state));
    return (state[code / 8U] >> (code % 8U)) & 1U;
}

static void test_overflow_reports_loss_to_idle_mouse_reader(void)
{
    struct input_event events[8];
    uint64_t mouse_cursor;
    uint64_t independent_mouse_cursor;

    input_init();
    mouse_cursor = input_evdev_cursor_now();

    /* The reader sees the press and is now waiting for a release. */
    input_push_mouse(10, 20, 0, 0, 1);
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &mouse_cursor,
                            events, sizeof(events), 0) ==
           4 * (int)sizeof(struct input_event));
    assert(has_key_state(BTN_LEFT));

    /* A release is published, then keyboard-only traffic evicts it. */
    input_push_mouse(10, 20, 0, 0, 0);
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    independent_mouse_cursor = mouse_cursor;
    /* Retained motion after the lost release is stale once recovery ends. */
    input_push_mouse(11, 20, 1, 0, 0);

    /* Poll must wake for loss even though no mouse records remain. */
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE, mouse_cursor, 0));
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE,
                                 independent_mouse_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &mouse_cursor, events,
                            sizeof(struct input_event) + 1U, 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE,
                                 independent_mouse_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &independent_mouse_cursor,
                            events, sizeof(struct input_event), 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);

    /* The report remains pending across further queue wraps and must rebase
     * at the current tail instead of replaying retained packets. */
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    input_push_mouse(12, 20, 1, 0, 0);

    /* A one-record buffer carries SYN_REPORT on the next read. */
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE, mouse_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &mouse_cursor, events,
                            sizeof(struct input_event), 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_REPORT);
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE, mouse_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &independent_mouse_cursor,
                            events, sizeof(struct input_event), 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_REPORT);
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE,
                                  independent_mouse_cursor, 0));
    assert(!has_key_state(BTN_LEFT));

    /* Repeated keyboard-only wraparound must not look like new mouse loss. */
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE, mouse_cursor, 0));
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE,
                                  independent_mouse_cursor, 0));

    /* Fresh mouse records remain readable after the synchronization pair. */
    input_push_mouse(13, 20, 1, 0, 0);
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE, mouse_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &mouse_cursor, events,
                            sizeof(events), 0) >=
           4 * (int)sizeof(struct input_event));
    assert(events[0].type == EV_REL && events[0].code == REL_X);
    assert(events[1].type == EV_ABS && events[1].code == ABS_X);
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE,
                                 independent_mouse_cursor, 0));

    /* New opens start at the current tail, so they do not report old loss. */
    uint64_t fresh_cursor = input_evdev_cursor_now();
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE, fresh_cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &fresh_cursor, events,
                            sizeof(events), 0) == 0);
}

static void test_subrecord_read_is_rejected_without_advancing(void)
{
    struct input_event event;
    uint64_t cursor;

    input_init();
    input_push_mouse(1, 2, 0, 0, 0);
    cursor = input_evdev_cursor_now() - 4;
    uint64_t original_cursor = cursor;
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, &event,
                            sizeof(event) - 1U, 0) == -22);
    assert(cursor == original_cursor);
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, &event,
                            sizeof(event), 0) == (int)sizeof(event));
}

static void test_vt_and_grab_filters_survive_recovery(void)
{
    struct input_event events[8];
    uint64_t vt1_cursor, vt2_cursor;
    int64_t grab_token;

    input_init();
    input_set_graphical_vt(1);
    vt1_cursor = input_evdev_cursor_now();
    grab_token = input_evdev_grab(STORAGE_DEV_KIND_MOUSE, 0, 1, 7);
    assert(grab_token > 0);

    /* A non-owner advances past grabbed records without receiving them. */
    input_push_mouse(10, 20, 0, 0, 1);
    assert(input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &vt1_cursor, events,
                               sizeof(events), 0, 1) == 0);

    /* The release is hidden by the grab, then evicted by keyboard traffic. */
    input_push_mouse(10, 20, 0, 0, 0);
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    vt2_cursor = vt1_cursor;

    assert(input_evdev_grab(STORAGE_DEV_KIND_MOUSE, (uint64_t)grab_token,
                            0, 7) == grab_token);
    input_push_mouse(11, 20, 1, 0, 0);

    /* VT 2 gets only the loss boundary, never VT 1's retained mouse data. */
    assert(input_evdev_available_vt(STORAGE_DEV_KIND_MOUSE, vt2_cursor,
                                    0, 2));
    int bytes = input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &vt2_cursor,
                                    events, sizeof(events), 0, 2);
    assert(bytes == 2 * (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);
    assert(events[1].type == EV_SYN && events[1].code == SYN_REPORT);
    assert(!input_evdev_available_vt(STORAGE_DEV_KIND_MOUSE, vt2_cursor,
                                     0, 2));

    /* VT 1 sees the pair only; retained motion predating the report is stale. */
    bytes = input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &vt1_cursor, events,
                                sizeof(events), 0, 1);
    assert(bytes == 2 * (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);
    assert(events[1].type == EV_SYN && events[1].code == SYN_REPORT);
    assert(!input_evdev_available_vt(STORAGE_DEV_KIND_MOUSE, vt1_cursor,
                                     0, 1));

    /* New input after synchronization is delivered to its recorded VT. */
    input_push_mouse(12, 20, 1, 0, 0);
    bytes = input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &vt1_cursor, events,
                                sizeof(events), 0, 1);
    assert(bytes == 4 * (int)sizeof(struct input_event));
    assert(events[0].type == EV_REL && events[0].code == REL_X);
    assert(!input_evdev_available_vt(STORAGE_DEV_KIND_MOUSE, vt2_cursor,
                                     0, 2));

    /* Opening after the overflow starts at the current tail, without loss. */
    uint64_t fresh_cursor = input_evdev_cursor_now();
    assert(!input_evdev_available_vt(STORAGE_DEV_KIND_MOUSE, fresh_cursor,
                                     0, 1));
}

static void test_sequence_wrap_and_pending_report(void)
{
    struct input_event events[8];
    uint64_t cursor;

    input_init();
    evdev_next_sequence = INPUT_EVDEV_CURSOR_SEQUENCE_MASK - 2U;
    evdev_first_sequence = evdev_next_sequence;
    evdev_first_slot = 0;
    evdev_next_slot = 0;
    evdev_record_count = 0;
    cursor = input_evdev_cursor_now();

    /* Ordinary records cross the sequence boundary without marker collision. */
    input_push_mouse(10, 20, 1, 0, 0);
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, events,
                            sizeof(events), 0) ==
           4 * (int)sizeof(struct input_event));
    assert(cursor == 2U);
    assert(events[0].type == EV_REL && events[0].code == REL_X);

    /* The mouse records are evicted after wrap. */
    input_push_mouse(10, 20, 0, 0, 1);
    input_push_mouse(10, 20, 0, 0, 0);
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, events,
                            sizeof(struct input_event), 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);
    assert(cursor & INPUT_EVDEV_CURSOR_SYNC_REPORT);

    /* More wraparound while pending cannot collide with the cursor flag. */
    for (unsigned i = 0; i < 400; ++i)
        input_push_key(30, 1);
    assert(input_evdev_available(STORAGE_DEV_KIND_MOUSE, cursor, 0));
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, events,
                            sizeof(struct input_event), 0) ==
           (int)sizeof(struct input_event));
    assert(events[0].type == EV_SYN && events[0].code == SYN_REPORT);
    assert(!(cursor & INPUT_EVDEV_CURSOR_SYNC_REPORT));
    assert(cursor == evdev_next_sequence);
    assert(!input_evdev_available(STORAGE_DEV_KIND_MOUSE, cursor, 0));

    input_push_mouse(11, 20, 1, 0, 0);
    assert(input_evdev_read(STORAGE_DEV_KIND_MOUSE, &cursor, events,
                            sizeof(events), 0) ==
           4 * (int)sizeof(struct input_event));
    assert(events[0].type == EV_REL && events[0].code == REL_X);
}

int main(void)
{
    test_overflow_reports_loss_to_idle_mouse_reader();
    test_subrecord_read_is_rejected_without_advancing();
    test_vt_and_grab_filters_survive_recovery();
    test_sequence_wrap_and_pending_report();
    puts("evdev-overflow: PASS");
    return 0;
}
