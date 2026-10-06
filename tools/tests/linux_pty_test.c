#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/pty.c"

static struct task owner;
static unsigned signals;
static unsigned blocked, awakened;
struct task *sched_current_task(void) { return &owner; }
void kernel_wait_queue_init(struct kernel_wait_queue *q) { memset(q, 0, sizeof(*q)); }
void kernel_wait_queue_remove(struct kernel_wait_queue *q, struct task *t) { (void)q; (void)t; }
void kernel_wait_queue_block_current(struct kernel_wait_queue *q) { (void)q; ++blocked; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q; ++awakened; return 1; }
static unsigned descriptor_refs = 1;
static uint8_t keyboard_caps;
uint8_t input_caps_lock_active(void) { return keyboard_caps; }
void input_set_graphical_vt(uint32_t number) { (void)number; }
static uint32_t shown_vt;
static uint32_t shown_graphical;
static char vt_written[256];
static size_t vt_written_len;
void console_vt_activate(uint32_t number, bool graphical)
{ shown_vt = number; shown_graphical = graphical; }
void console_vt_write(uint32_t number, const char *text, size_t count)
{
    (void)number;
    if (vt_written_len + count > sizeof(vt_written)) return;
    memcpy(vt_written + vt_written_len, text, count);
    vt_written_len += count;
}
void sched_set_controlling_pty(uint32_t pid, uint32_t id)
{ assert(pid == owner.pid); owner.controlling_pty_id = id; }
void sched_clear_controlling_pty(uint32_t id)
{ if (owner.controlling_pty_id == id) owner.controlling_pty_id = 0; }
struct task *sched_find(uint32_t pid) { return pid == owner.pid ? &owner : NULL; }
uint32_t sched_pty_reference_count(uint32_t id) { return descriptor_refs + pty_transfer_count(id, 0); }
uint32_t sched_pty_master_reference_count(uint32_t id) { return pty_transfer_count(id, 1); }
int sched_signal_process_group(uint32_t caller, uint32_t group, int signal)
{ (void)caller; (void)group; (void)signal; ++signals; return 0; }
int sched_signal_kernel_group(uint32_t group, int signal)
{ (void)group; (void)signal; ++signals; return 0; }
int sched_signal_user_process(uint32_t pid, int signal)
{ assert(pid == owner.pid); (void)signal; ++signals; return 0; }
int64_t sched_process_group_session(uint32_t group)
{ return group == owner.process_group ? owner.process_session : -3; }
int sched_process_group_orphaned(uint32_t group)
{ (void)group; return 0; }
int sched_hangup_user_tasks_for_pty(uint32_t id, uint32_t owner_pid)
{ (void)owner_pid; sched_clear_controlling_pty(id); return 0; }
int sched_process_group_has_pty(uint32_t group, uint32_t id)
{ (void)group; (void)id; return 1; }
const struct framebuffer *framebuffer_get(void) { return NULL; }
void console_printf(const char *format, ...) { (void)format; }
void console_write_len(const char *text, size_t count) { (void)text; (void)count; }

int main(void)
{
    owner.pid = 12;
    owner.fsuid = 1000;
    owner.fsgid = 100;
    owner.process_session = 4;
    owner.process_group = 4;
    pty_init();
    int id = pty_create(owner.pid);
    assert(id > 0);
    struct storage_node node;
    struct reliefos_permissions permissions;
    assert(pty_get_node(id, &node) == 0);
    assert(pty_inode_permissions(&node, &permissions, false) == 0);
    assert(permissions.uid == 1000 && permissions.gid == 100 && permissions.mode == 0600);
    permissions = (struct reliefos_permissions){0620, 2000, 5};
    assert(pty_inode_permissions(&node, &permissions, true) == 0);
    permissions = (struct reliefos_permissions){0};
    assert(pty_inode_permissions(&node, &permissions, false) == 0 && permissions.uid == 2000);
    struct storage_node stale = node;
    uint32_t group = 99;
    assert(pty_get_foreground_pgid(id, &group) == 0 && group == 0);
    assert(pty_destroy(owner.pid, id) == 0 && signals == 0);
    pty_init();
    id = pty_create(owner.pid);
    assert(pty_inode_permissions(&stale, &permissions, false) == -2);
    assert(pty_lookup_path("/dev/pts/1", &node) == 0);
    assert(pty_lookup_path("/dev/pts/1/more", &node) == -2);
    assert(pty_lookup_path("/dev/pts/9999999999999999999999999", &node) == -2);
    struct reliefos_pty_termios mode;
    assert(pty_get_termios(id, &mode) == 0);
    /* Linux native encodings, independent of the private PTY aliases. */
    assert((mode.c_iflag & 0x100) != 0);
    assert((mode.c_lflag & 0xb) == 0xb);
    assert(mode.c_cc[6] == 1 && mode.c_cc[5] == 0);
    mode.c_lflag &= ~0xbU;
    assert(pty_set_termios(id, &mode) == 0);
    assert(pty_write_input(owner.pid, id, "x", 1) == 1);
    char ch = 0;
    assert(pty_read_input(id, &ch, 1) == 1 && ch == 'x');
    owner.process_session = owner.pid;
    owner.process_group = owner.pid;
    owner.pty_id = id;
    owner.euid = 1000;
    assert(pty_acquire_controlling(id, owner.pid, 0, 0) == -1);
    assert(pty_acquire_controlling(id, owner.pid, 0, 1) == 0);
    assert(owner.controlling_pty_id == (uint32_t)id);
    assert(pty_acquire_controlling(id, owner.pid, 0, 1) == 0);
    owner.process_group = 99;
    owner.signal_actions[21].handler = 1;
    assert(pty_check_change(id, owner.pid, 21) == -5);
    owner.signal_actions[21].handler = 0;
    owner.blocked_signals = 1ULL << 20;
    assert(pty_check_change(id, owner.pid, 21) == -5);
    owner.blocked_signals = 0;
    owner.signal_actions[22].handler = 1;
    assert(pty_check_change(id, owner.pid, 22) == 0);
    owner.signal_actions[22].handler = 0;
    assert(pty_check_change(id, owner.pid, 22) == KERNEL_SYSCALL_BLOCKED);
    assert(signals == 1);
    signals = 0;
    owner.process_group = owner.pid;
    int other = pty_create(owner.pid);
    assert(pty_acquire_controlling(other, owner.pid, 0, 1) == -1);
    assert(pty_get_foreground_pgid(id, &group) == 0 && group == owner.pid);
    assert(pty_destroy(owner.pid, id) == 0 && signals == 2);
    pty_init();
    assert(pty_vt_init() == 0);
    id = 1;
    assert(pty_slave_open_allowed(id));
    assert(pty_read_input(id, &ch, 1) == -11);
    const uint8_t keys[] = {23, 49, 31, 20, 30, 38, 38};
    for (unsigned i = 0; i < sizeof(keys); ++i) {
        pty_console_key_event(keys[i], 1);
        pty_console_key_event(keys[i], 0);
    }
    assert(pty_read_input(id, &ch, 1) == -11);
    pty_console_key_event(28, 1);
    char line[8];
    for (unsigned i = 0; i < sizeof(line); ++i)
        assert(pty_read_input(id, &line[i], 1) == 1);
    assert(!memcmp(line, "install\n", sizeof(line)));
    assert(pty_read_input(id, &ch, 1) == -11);
    assert(pty_read_input(id, &ch, 0) == 0);
    assert(pty_get_termios(id, &mode) == 0);
    mode.c_lflag &= ~LINUX_ICANON;
    mode.c_cc[LINUX_VMIN] = 0;
    mode.c_cc[LINUX_VTIME] = 0;
    assert(pty_set_termios(id, &mode) == 0);
    assert(pty_read_input(id, &ch, 1) == 0);
    keyboard_caps = 1;
    pty_console_key_event(30, 1);
    assert(pty_read_input(id, &ch, 1) == 1 && ch == 'A');
    pty_console_key_event(58, 1); /* Input layer already applied the state. */
    pty_console_key_event(58, 1);
    pty_console_key_event(30, 1);
    assert(pty_read_input(id, &ch, 1) == 1 && ch == 'A');
    keyboard_caps = 0;
    pty_console_key_event(30, 1);
    assert(pty_read_input(id, &ch, 1) == 1 && ch == 'a');
    pty_init();
    id = pty_create(owner.pid);
    assert(pty_transfer_get(id, TASK_PTY_ENDPOINT_SLAVE) == 0);
    descriptor_refs = 0;
    assert(pty_destroy(owner.pid, id) == 0);
    assert(find_session(id) != NULL && pty_transfer_count(id, 0) == 1);
    pty_transfer_put(id, TASK_PTY_ENDPOINT_SLAVE);
    assert(find_session(id) == NULL);
    assert(pty_transfer_get(id, TASK_PTY_ENDPOINT_SLAVE) == -9);
    pty_init();
    assert(pty_vt_init() == 0);
    assert(pty_vt_active() == 1 && shown_vt == 1 && shown_graphical == 0);
    for (uint32_t number = 1; number <= 6; ++number) {
        char path[16];
        snprintf(path, sizeof(path), "/dev/tty%u", number);
        assert(pty_vt_id(number) == number);
        assert(pty_vt_number(number) == number);
        assert(pty_lookup_vt_path(path, &node) == 0);
        assert(pty_lookup_path("/dev/pts/1", &node) == -2);
    }
    assert(pty_vt_id(0) == 0 && pty_vt_id(7) == 0);
    /* Linux /dev/tty0 is the active console, which is exactly the descriptor
     * tools/tests/xorg_vt_abi_test.c opens to exercise the VT ABI. */
    assert(pty_lookup_vt_path("/dev/tty0", &node) == 0 &&
           node.first_cluster == pty_vt_active());
    assert(pty_lookup_vt_path("/dev/ttyS0", &node) == -2);
    assert(pty_create(owner.pid) == 7);
    pty_console_key_event(29, 1); /* Ctrl */
    pty_console_key_event(56, 1); /* Alt */
    pty_console_key_event(60, 1); /* F2 */
    assert(pty_vt_active() == 2 && shown_vt == 2);
    assert(pty_read_input(1, &ch, 1) == -11);
    assert(pty_read_input(2, &ch, 1) == -11);
    /* A VT hand-off clears the kernel's modifier snapshot. Model the user
     * releasing the first chord before pressing a new Ctrl+Alt+Fn chord. */
    pty_console_key_event(29, 0); /* Ctrl */
    pty_console_key_event(56, 0); /* Alt */
    pty_console_key_event(29, 1); /* Ctrl */
    pty_console_key_event(56, 1); /* Alt */
    pty_console_key_event(64, 1); /* F6 */
    assert(pty_vt_active() == 6 && shown_vt == 6);
    pty_console_key_event(29, 0);
    pty_console_key_event(56, 0);
    assert(pty_vt_switch(1) == 0);
    assert(pty_vt_set_graphics(1, 1) == 0);
    assert(pty_vt_graphical_active() && shown_graphical == 1);
    /* Linux VT_AUTO + KD_GRAPHICS refuses switches without a controller. */
    assert(pty_vt_switch(2) == 0 && pty_vt_active() == 1 && pty_vt_graphical_active());
    assert(pty_vt_set_graphics(1, 0) == 0 && !pty_vt_graphical_active());
    assert(pty_vt_wait_active(1) == 0 && blocked == 0);
    assert(pty_vt_wait_active(7) == -22 && blocked == 0);
    assert(pty_vt_wait_active(2) == KERNEL_SYSCALL_BLOCKED && blocked == 1);
    unsigned before_wake = awakened;
    uint64_t generation = pty_vt_generation();
    assert(pty_vt_switch(2) == 0 && awakened == before_wake + 1);
    assert(pty_vt_generation() == generation + 1);
    assert(pty_vt_wait_active(2) == 0);
    assert(pty_vt_switch(1) == 0);
    assert(pty_vt_switch(0) == -22 && pty_vt_active() == 1);

    /* ---- Linux VT_PROCESS hand-off contract on the fixed consoles. ---- */
    {
        struct vt_mode probe;
        struct vt_mode controller = { .mode = VT_PROCESS, .relsig = 10, .acqsig = 12 };
        /* VT_ACKACQ is an acknowledgement, never a persistent ownership mode. */
        assert(pty_vt_set_mode(1, &(struct vt_mode){ .mode = VT_ACKACQ }) == -22);
        owner.process_session = owner.pid;
        owner.process_group = owner.pid;
        owner.controlling_pty_id = 1;
        assert(pty_vt_set_mode(1, &controller) == 0);
        assert(pty_vt_set_graphics(1, 1) == 0);
        signals = 0;
        assert(pty_vt_switch(2) == 0);
        assert(pty_vt_active() == 1 && pty_vt_graphical_active()); /* held */
        assert(signals == 1);                                      /* relsig */
        assert(pty_vt_release_display(1, 0) == 0);                 /* refuse */
        assert(pty_vt_active() == 1 && pty_vt_graphical_active());
        assert(pty_vt_switch(2) == 0);
        assert(signals == 2);
        assert(pty_vt_release_display(1, 1) == 0);                 /* grant */
        assert(pty_vt_active() == 2 && !pty_vt_graphical_active());
        assert(pty_vt_get_mode(2, &probe) == 0 && probe.mode == VT_AUTO);
        signals = 0;
        assert(pty_vt_switch(1) == 0);
        assert(pty_vt_active() == 1 && signals == 1); /* acqsig to tty1's controller */
        /* A dying controller leaves a usable text console behind. */
        assert(pty_vt_set_graphics(1, 1) == 0);
        assert(pty_vt_switch(2) == 0);
        assert(pty_vt_active() == 1);
        pty_process_session_exit(owner.pid);
        assert(pty_vt_graphical(1) == 0 && !pty_vt_graphical_active());
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_AUTO);
        assert(pty_vt_release_display(1, 1) == -22); /* hand-off cancelled */
        /* K_OFF keeps the console tty silent. */
        pty_flush_input(1);
        assert(pty_vt_set_keyboard_mode(1, K_OFF) == 0);
        pty_console_key_event(30, 1);
        pty_console_key_event(30, 0);
        assert(pty_read_input(1, &ch, 1) == -11);
        assert(pty_vt_set_keyboard_mode(1, K_XLATE) == 0);
    }
    /* ---- Linux OPOST output post-processing on the slave write path. ---- */
    {
        char out[32];
        pty_init();
        id = pty_create(owner.pid);
        assert(id > 0);
        /* Defaults carry OPOST|ONLCR: a bare NL reaches the reader as CR NL. */
        assert(pty_get_termios(id, &mode) == 0);
        assert((mode.c_oflag & LINUX_OPOST) && (mode.c_oflag & LINUX_ONLCR));
        assert(pty_write_output(id, "ab\ncd\n", 6) == 6);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 8);
        assert(!memcmp(out, "ab\r\ncd\r\n", 8));
        /* OPOST off: bytes pass through untouched. */
        mode.c_oflag &= ~LINUX_OPOST;
        assert(pty_set_termios(id, &mode) == 0);
        assert(pty_write_output(id, "x\ny", 3) == 3);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 3 && !memcmp(out, "x\ny", 3));
        /* OPOST on with ONLCR off: NL stays NL. */
        pty_init();
        id = pty_create(owner.pid);
        assert(pty_get_termios(id, &mode) == 0);
        mode.c_oflag &= ~LINUX_ONLCR;
        assert(pty_set_termios(id, &mode) == 0);
        assert(pty_write_output(id, "q\nr\n", 4) == 4);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 4 && !memcmp(out, "q\nr\n", 4));
        /* ONOCR suppresses CR at column 0 and keeps it elsewhere. */
        pty_init();
        id = pty_create(owner.pid);
        assert(pty_get_termios(id, &mode) == 0);
        mode.c_oflag = LINUX_OPOST | LINUX_ONOCR;
        assert(pty_set_termios(id, &mode) == 0);
        assert(pty_write_output(id, "\ra", 2) == 2);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 1 && out[0] == 'a');
        assert(pty_write_output(id, "\rb", 2) == 2);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 2 && !memcmp(out, "\rb", 2));
        /* OCRNL maps CR to NL. */
        pty_init();
        id = pty_create(owner.pid);
        assert(pty_get_termios(id, &mode) == 0);
        mode.c_oflag = LINUX_OPOST | LINUX_OCRNL;
        assert(pty_set_termios(id, &mode) == 0);
        assert(pty_write_output(id, "a\rb", 3) == 3);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 3 && !memcmp(out, "a\nb", 3));
        /* OLCUC upper-cases and TAB3 expands to the next 8-column stop. */
        pty_init();
        id = pty_create(owner.pid);
        assert(pty_get_termios(id, &mode) == 0);
        mode.c_oflag = LINUX_OPOST | LINUX_OLCUC | LINUX_TAB3;
        assert(pty_set_termios(id, &mode) == 0);
        assert(pty_write_output(id, "a\tb", 3) == 3);
        assert(pty_read_output(owner.pid, id, out, sizeof(out)) == 9);
        assert(!memcmp(out, "A       B", 9));
        /* Console sessions take the same translated path into the VT. */
        pty_init();
        assert(pty_vt_init() == 0);
        vt_written_len = 0;
        assert(pty_write_output(1, "h\ni", 3) == 3);
        assert(vt_written_len == 4 && !memcmp(vt_written, "h\r\ni", 4));
    }
    puts("PASS native PTY modes, OPOST output post-processing, controlling-session isolation and queued-rights lifetime");
    return 0;
}
