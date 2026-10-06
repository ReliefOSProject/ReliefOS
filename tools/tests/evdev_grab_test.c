/*
 * Linux EVIOCGRAB contract for the evdev keyboard/mouse descriptors.
 *
 * The Linux UAPI passes the grab request as a scalar zero/nonzero argument, not as a
 * pointer to an int.  The exclusive grab belongs to the open file description:
 * fork() and dup() share it, and only the last close releases it — even when
 * the surviving descriptor belongs to a different process than the one that
 * issued EVIOCGRAB. Grabbing the underlying input device excludes other
 * handlers; on a graphical VT, only the Ctrl+Alt+Fn chord remains visible to
 * the console so VT switching still works.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall.c"
#include "../../kernel/reliefnt/kernel/reliefnt/input.c"

static bool output_writable = true;
bool user_range_ok(uint64_t address, uint64_t length)
{ return address != 0 || length == 0; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return output_writable && user_range_ok(address, length); }
static uint32_t calling_pid = 1;
uint32_t sched_current_pid(void) { return calling_pid; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *ptr) { free(ptr); }
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }
int storage_disk_block_read(uint32_t disk_id, int32_t partition_index,
                            uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *out_read)
{ (void)disk_id; (void)partition_index; (void)offset; (void)buffer; (void)length; (void)out_read; return -5; }
int kernel_random_fill(void *buffer, size_t length)
{ (void)buffer; (void)length; return -5; }
int64_t pty_read_input(uint32_t pty_id, char *buffer, uint32_t length)
{ (void)pty_id; (void)buffer; (void)length; return 0; }
void task_pipe_release(struct task_file *f) { (void)f; }
void task_socket_release(struct task_file *f) { (void)f; }
void task_inet_release(struct task_file *f) { (void)f; }
void task_shm_release(struct task_file *f) { (void)f; }
void task_socket_collect(void) {}
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q; return 0; }
uint64_t time_uptime_us(void) { return 0; }
const struct framebuffer *framebuffer_get(void) { return NULL; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }

/* Count console handler invocations, which the real input layer must exclude. */
static unsigned console_key_events;
void pty_console_key_event(uint8_t keycode, uint8_t pressed)
{ (void)keycode; (void)pressed; ++console_key_events; }
uint32_t pty_vt_active(void) { return 1; }
int pty_vt_open_query(void) { return -1; }
uint32_t pty_vt_state_bitmap(void) { return 0x0bu; }
int pty_vt_graphical_active(void) { return 0; }

uint32_t pty_vt_id(uint32_t number) { return number >= 1 && number <= 6 ? number : 0u; }
uint32_t pty_vt_number(uint32_t pty_id) { return pty_vt_id(pty_id); }

static struct task_file *keyboard_file(struct task_file *file)
{
    *file = (struct task_file){0};
    file->used = 1;
    file->flags = TASK_FILE_FLAG_DEV_NODE;
    file->node.first_cluster = STORAGE_DEV_KIND_KEYBOARD;
    return file;
}

static void regression(const char *name)
{
    struct task_file first, other, duplicate = {0};
    input_init();
    keyboard_file(&first);
    keyboard_file(&other);
    if (!strcmp(name, "ioctl-query")) {
        int available = 42;
        struct vt_stat state = {.v_signal = 0xbeef};
        assert(task_vt_query_ioctl(VT_OPENQRY, (uintptr_t)&available) == 0);
        assert(available == -1);
        assert(task_vt_query_ioctl(VT_GETSTATE, (uintptr_t)&state) == 0);
        assert(state.v_active == 1 && state.v_state == 0x0bu && state.v_signal == 0xbeef);
        output_writable = false;
        assert(task_vt_query_ioctl(VT_OPENQRY, (uintptr_t)&available) == -RELIEFOS_EFAULT);
        assert(task_vt_query_ioctl(VT_GETSTATE, (uintptr_t)&state) == -RELIEFOS_EFAULT);
    } else if (!strcmp(name, "vt-bind")) {
        struct task owner = {0};
        struct task_file delayed, explicit_raw;
        struct input_event events[8];
        uint32_t raw_vt = 0;

        /* Xorg may open event0 before xf86OpenConsole exposes /dev/tty1. */
        keyboard_file(&delayed);
        delayed.aux = input_evdev_cursor_now();
        owner.pty_fds[0] = (struct task_pty_fd){
            .used = 1, .pty_id = 1, .endpoint = TASK_PTY_ENDPOINT_SLAVE,
        };
        input_set_graphical_vt(1);
        input_handle_scancode(30, 1);
        input_process_pending();
        assert(delayed.input_vt == 0);
        assert(task_device_read(&owner, &delayed, events, sizeof(events)) > 0);
        assert(delayed.input_vt == 1);
        keyboard_file(&explicit_raw);
        assert(task_evdev_ioctl(&explicit_raw, RELIEFOS_EVIOCSVT,
                                (uintptr_t)&raw_vt) == 0);
        task_evdev_bind_vt(&owner, &explicit_raw);
        assert(explicit_raw.input_vt == 0);
    } else if (!strcmp(name, "scalar")) {
        const uint64_t values[] = {2, UINT64_MAX, 1ULL << 32, (uintptr_t)&first};
        for (unsigned i = 0; i < 4; ++i) {
            assert(task_evdev_ioctl(&first, EVIOCGRAB, values[i]) == 0);
            assert(first.aux2);
            assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
        }
    } else if (!strcmp(name, "repeat")) {
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        uint64_t token = first.aux2;
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(first.aux2 == token);
    } else if (!strcmp(name, "release")) {
        assert(task_evdev_ioctl(&other, EVIOCGRAB, 0) == -RELIEFOS_EINVAL);
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        assert(task_evdev_ioctl(&other, EVIOCGRAB, 0) == -RELIEFOS_EINVAL);
        assert(task_evdev_ioctl(&other, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == -RELIEFOS_EINVAL);
    } else if (!strcmp(name, "console")) {
        input_set_graphical_vt(1);
        input_handle_scancode(29, 1);
        input_process_pending();
        assert(console_key_events == 1);
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        input_handle_scancode(56, 1);
        input_handle_scancode(60, 1);
        input_process_pending();
        assert(console_key_events == 3);
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
        input_handle_scancode(60, 0);
        input_process_pending();
        assert(console_key_events == 4);
        /* Leaving the graphical VT releases console ownership even if the
         * Xorg open description has not dropped EVIOCGRAB yet. */
        input_set_graphical_vt(0);
        input_handle_scancode(30, 1);
        input_process_pending();
        assert(console_key_events == 5);
        input_set_graphical_vt(1);
        input_handle_scancode(116, 1); /* Right Ctrl */
        input_handle_scancode(115, 1); /* Right Alt */
        input_handle_scancode(61, 1);  /* Right-side chord still reaches console */
        input_process_pending();
        assert(console_key_events == 8);
    } else if (!strcmp(name, "publication")) {
        struct input_event events[16];
        uint64_t cursor = input_evdev_cursor_now();
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        input_handle_scancode(30, 1);
        input_process_pending();
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
        assert(!input_evdev_available(STORAGE_DEV_KIND_KEYBOARD, cursor, 0));
        assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor, events, sizeof(events), 0) == 0);
        cursor = input_evdev_cursor_now();
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        input_handle_scancode(30, 0);
        input_process_pending();
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
        assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &cursor, events,
                                sizeof(events), first.aux2) > 0);
    } else if (!strcmp(name, "ofd")) {
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
        uint64_t token = first.aux2;
        assert(task_file_reference(&duplicate, &first) == 0);
        assert(first.description == duplicate.description);
        clear_task_file(&first); /* first close must keep the shared grab */
        calling_pid = 2;
        assert(task_evdev_ioctl(&other, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(duplicate.description->aux2 == token);
        clear_task_file(&duplicate); /* final close by a different process */
        assert(task_evdev_ioctl(&other, EVIOCGRAB, 1) == 0);
        assert(other.aux2 && other.aux2 != token);
        clear_task_file(&other);
    } else { assert(!"unknown regression"); }
    printf("PASS evdev regression %s\n", name);
}

int main(int argc, char **argv)
{
    if (argc == 2) { regression(argv[1]); return 0; }
    struct task_file first;
    struct task_file second;

    input_init();
    keyboard_file(&first);
    keyboard_file(&second);

    /* ---- EVIOCGRAB takes the Linux scalar 1/0, never a user pointer. ---- */
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 2) == 0);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, (uintptr_t)&first) == 0);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 0) == 0);

    /* ---- The grab belongs to the open file description. ---- */
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
    {
        uint64_t token = first.aux2;
        assert(token != 0);
        /* Linux rejects a repeated request, even from the same OFD. */
        assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(first.aux2 == token);
        /* A separate open file description cannot take it. */
        calling_pid = 2;
        assert(task_evdev_ioctl(&second, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(second.aux2 == 0);
        calling_pid = 1;

        /* fork()/dup() share the OFD: the copied descriptor carries the token. */
        second = first;
        assert(second.aux2 == token);

        /* A release naming a foreign token must not disturb this grab. */
        input_evdev_release(STORAGE_DEV_KIND_KEYBOARD, token + 1000u);
        calling_pid = 2;
        assert(task_evdev_ioctl(&second, EVIOCGRAB, 1) == -RELIEFOS_EBUSY);
        assert(second.aux2 == token);

        /* The last close of the shared description releases the grab; which
         * process performed it is irrelevant because fork()/dup() share it. */
        input_evdev_release(STORAGE_DEV_KIND_KEYBOARD, second.aux2);
        keyboard_file(&second);
        calling_pid = 2;
        assert(task_evdev_ioctl(&second, EVIOCGRAB, 1) == 0);
        assert(second.aux2 != 0 && second.aux2 != token);
        input_evdev_release(STORAGE_DEV_KIND_KEYBOARD, second.aux2);
    }

    /* ---- A graphical grab excludes printable console input but preserves VT switching. ---- */
    keyboard_file(&first);
    keyboard_file(&second);
    console_key_events = 0;
    input_handle_scancode(29, 1); /* Ctrl */
    input_process_pending();
    assert(console_key_events == 1);
    input_set_graphical_vt(1);
    assert(task_evdev_ioctl(&first, EVIOCGRAB, 1) == 0);
    {
        struct input_event events[8];
        uint64_t holder_cursor = input_evdev_cursor_now();
        uint64_t other_cursor = holder_cursor;
        input_handle_scancode(56, 1); /* Alt */
        input_handle_scancode(60, 1); /* F2 */
        input_process_pending();
        assert(console_key_events == 3);
        /* The grab holder still receives the events ... */
        assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &holder_cursor, events,
                                sizeof(events), first.aux2) > 0);
        /* ... while a non-owning reader observes none of them. */
        input_handle_scancode(60, 0);
        input_process_pending();
        assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD, &other_cursor, events,
                                sizeof(events), second.aux2) == 0);
    }
    input_evdev_release(STORAGE_DEV_KIND_KEYBOARD, first.aux2);

    printf("PASS Linux EVIOCGRAB scalar argument and OFD grab lifetime\n");
    return 0;
}
