/*
 * Linux VT_PROCESS hand-off contract for the fixed ReliefOS virtual consoles.
 *
 * A graphics session (Xorg) claims tty1 with VT_SETMODE(VT_PROCESS) and must
 * observe the same release/acquire protocol Linux documents: switching away
 * first signals the old controller and holds the display, VT_RELDISP(0) keeps
 * it, VT_RELDISP(1) commits the target and signals the new controller, and the
 * target acknowledges with VT_RELDISP(VT_ACKACQ) without ever losing the
 * VT_PROCESS ownership mode.  Keyboard translation modes follow the Linux
 * K_RAW / K_MEDIUMRAW / K_XLATE / K_OFF contract instead of always producing
 * cooked canonical characters.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/pty.c"

#define SIG_RELEASE 10
#define SIG_ACQUIRE 12

static struct task owners[3];
static struct task *current;
struct task *sched_current_task(void) { return current; }
void kernel_wait_queue_init(struct kernel_wait_queue *q) { memset(q, 0, sizeof(*q)); }
void kernel_wait_queue_remove(struct kernel_wait_queue *q, struct task *t) { (void)q; (void)t; }
void kernel_wait_queue_block_current(struct kernel_wait_queue *q) { (void)q; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q; return 1; }
uint8_t input_caps_lock_active(void) { return 0; }

static uint32_t shown_vt;
static uint32_t shown_graphical;
void input_set_graphical_vt(uint32_t number) { (void)number; }
void console_vt_activate(uint32_t number, bool graphical)
{ shown_vt = number; shown_graphical = graphical ? 1u : 0u; }
void console_vt_write(uint32_t number, const char *text, size_t count)
{ (void)number; (void)text; (void)count; }
void console_printf(const char *format, ...) { (void)format; }
void console_write_len(const char *text, size_t count) { (void)text; (void)count; }
const struct framebuffer *framebuffer_get(void) { return NULL; }

struct task *sched_find(uint32_t pid)
{
    for (uint32_t i = 0; i < 3; ++i)
        if (owners[i].pid == pid) return &owners[i];
    return NULL;
}
void sched_set_controlling_pty(uint32_t pid, uint32_t id)
{ struct task *t = sched_find(pid); if (t) t->controlling_pty_id = id; }
void sched_clear_controlling_pty(uint32_t id)
{
    for (uint32_t i = 0; i < 3; ++i)
        if (owners[i].pid && owners[i].controlling_pty_id == id)
            owners[i].controlling_pty_id = 0;
}
static uint32_t open_refs[7];
uint32_t sched_pty_reference_count(uint32_t id) { return open_refs[id]; }
uint32_t sched_pty_master_reference_count(uint32_t id) { (void)id; return 0; }
int sched_signal_process_group(uint32_t caller, uint32_t group, int signal)
{ (void)caller; return sched_signal_kernel_group(group, signal); }

/* Process and group signals are distinguished so the test can assert
 * which controller really received the release and the acquire request. */
#define SIGNAL_LOG 32
static struct { uint32_t group; uint32_t pid; int signal; } signal_log[SIGNAL_LOG];
static unsigned signal_count;
int sched_signal_kernel_group(uint32_t group, int signal)
{
    if (signal_count < SIGNAL_LOG) {
        signal_log[signal_count].group = group;
        signal_log[signal_count].signal = signal;
    }
    ++signal_count;
    return 0;
}
int sched_signal_user_process(uint32_t pid, int signal)
{
    if (!sched_find(pid)) return -3;
    if (signal_count < SIGNAL_LOG) {
        signal_log[signal_count].pid = pid;
        signal_log[signal_count].signal = signal;
    }
    ++signal_count;
    return 0;
}
int64_t sched_process_group_session(uint32_t group)
{
    for (uint32_t i = 0; i < 2; ++i)
        if (owners[i].process_group == group) return owners[i].process_session;
    return -3;
}
int sched_process_group_orphaned(uint32_t group) { (void)group; return 0; }
int sched_hangup_user_tasks_for_pty(uint32_t id, uint32_t owner_pid)
{ (void)id; (void)owner_pid; return 0; }
int sched_process_group_has_pty(uint32_t group, uint32_t id)
{ (void)group; (void)id; return 1; }

static unsigned signal_hits(int signal, uint32_t group)
{
    unsigned hits = 0;
    for (unsigned i = 0; i < signal_count && i < SIGNAL_LOG; ++i)
        if (signal_log[i].signal == signal &&
            (!group || signal_log[i].pid == group)) ++hits;
    return hits;
}
static void signal_reset(void)
{ signal_count = 0; memset(signal_log, 0, sizeof(signal_log)); }

static uint8_t drain_bytes(uint32_t pty_id, char *buffer, uint32_t capacity)
{
    uint32_t read = 0;
    while (read < capacity && pty_read_input(pty_id, &buffer[read], 1) == 1) ++read;
    return (uint8_t)read;
}

static void use_raw_termios(uint32_t pty_id)
{
    struct reliefos_pty_termios mode;
    assert(pty_get_termios(pty_id, &mode) == 0);
    mode.c_lflag &= ~RELIEFOS_PTY_LFLAG_ICANON;
    mode.c_lflag &= ~(RELIEFOS_PTY_LFLAG_ISIG | RELIEFOS_PTY_LFLAG_ECHO);
    mode.c_iflag = 0;
    mode.c_cc[RELIEFOS_PTY_CC_VMIN] = 1;
    mode.c_cc[RELIEFOS_PTY_CC_VTIME] = 0;
    assert(pty_set_termios(pty_id, &mode) == 0);
}

static void regression(const char *name)
{
    struct vt_mode mode = { .mode = VT_PROCESS, .relsig = SIG_RELEASE,
                            .acqsig = SIG_ACQUIRE, .frsig = 9 };
    struct vt_mode probe;
    char bytes[32];
    owners[0] = (struct task){ .pid = 100, .tgid = 100, .process_session = 50,
                               .process_group = 50 };
    owners[1] = (struct task){ .pid = 50, .tgid = 50, .process_session = 50,
                               .process_group = 50 };
    owners[2] = (struct task){ .pid = 101, .tgid = 100, .process_session = 50,
                               .process_group = 50 };
    current = &owners[2];
    pty_init();
    assert(pty_vt_init() == 0);
    if (!strcmp(name, "controller")) {
        assert(pty_vt_set_mode(1, &mode) == 0);
        assert(pty_vt_switch(2) == 0 && pty_vt_active() == 1);
        assert(signal_count == 1 && signal_log[0].pid == 100 && !signal_log[0].group);
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.frsig == 0);
    } else if (!strcmp(name, "reldisp")) {
        assert(pty_vt_set_mode(1, &mode) == 0);
        assert(pty_vt_release_display(1, 1) == -22);
        assert(pty_vt_release_display(1, VT_ACKACQ) == 0);
        assert(pty_vt_release_display(1, VT_ACKACQ) == 0);
        assert(pty_vt_switch(2) == 0);
        assert(pty_vt_release_display(1, 7) == 0 && pty_vt_active() == 2);
    } else if (!strcmp(name, "occupancy")) {
        assert(pty_vt_state_bitmap() == 1);
        assert(pty_vt_set_mode(2, &mode) == 0);
        assert(pty_vt_open_query() == 1);
        open_refs[1] = 1;
        open_refs[3] = 2;
        assert(pty_vt_state_bitmap() == 0x0bu);
        assert(pty_vt_open_query() == 2);
        for (unsigned i = 1; i <= 6; ++i) open_refs[i] = 1;
        assert(pty_vt_open_query() == -1);
        assert(pty_vt_state_bitmap() == 0x7fu);
    } else if (!strcmp(name, "death") || !strcmp(name, "detach")) {
        assert(pty_vt_set_mode(1, &mode) == 0);
        assert(pty_vt_set_graphics(1, 1) == 0);
        assert(pty_vt_set_keyboard_mode(1, K_OFF) == 0);
        assert(pty_acquire_controlling(1, 50, 0, 1) == 0);
        if (!strcmp(name, "detach")) assert(pty_detach_controlling(1, 50) == 0);
        else pty_process_session_exit(50);
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_PROCESS);
        assert(pty_vt_graphical(1));
        pty_process_session_exit(100);
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_AUTO);
        assert(!pty_vt_graphical(1));
        int kb;
        assert(pty_vt_get_keyboard_mode(1, &kb) == 0 && kb == K_XLATE);
    } else if (!strcmp(name, "nonleader-death")) {
        assert(pty_vt_set_mode(1, &mode) == 0);
        assert(pty_vt_set_graphics(1, 1) == 0);
        pty_process_session_exit(100);
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_AUTO);
        assert(!pty_vt_graphical(1));
    } else if (!strcmp(name, "bitmap")) {
        open_refs[3] = 1;
        assert(pty_vt_state_bitmap() == 9);
    } else if (!strcmp(name, "raw-modifier")) {
        use_raw_termios(1);
        assert(pty_vt_set_keyboard_mode(1, K_RAW) == 0);
        pty_console_key_event(42, 1);
        pty_console_key_event(42, 0);
        pty_console_key_event(116, 1);
        pty_console_key_event(116, 0);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 6);
        assert(!memcmp(bytes, "\x2a\xaa\xe0\x1d\xe0\x9d", 6));
    } else if (!strcmp(name, "graphics-keyboard")) {
        use_raw_termios(1);
        assert(pty_vt_set_graphics(1, 1) == 0);
        assert(pty_vt_set_keyboard_mode(1, K_XLATE) == 0);
        pty_console_key_event(30, 1);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a');
    } else if (!strcmp(name, "modifier-transition")) {
        use_raw_termios(1);
        pty_console_key_event(29, 1);
        assert(pty_vt_set_keyboard_mode(1, K_RAW) == 0);
        pty_console_key_event(29, 0);
        drain_bytes(1, bytes, sizeof(bytes));
        assert(pty_vt_set_keyboard_mode(1, K_XLATE) == 0);
        pty_console_key_event(30, 1);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a');
    } else if (!strcmp(name, "auto-graphics")) {
        assert(pty_vt_set_graphics(1, 1) == 0);
        assert(pty_vt_switch(2) == 0 && pty_vt_active() == 1);
        assert(pty_vt_set_graphics(1, 0) == 0);
        assert(pty_vt_switch(2) == 0 && pty_vt_active() == 2);
    } else if (!strcmp(name, "raw-termios")) {
        assert(pty_vt_set_keyboard_mode(1, K_RAW) == 0);
        pty_console_key_event(30, 1);
        assert(pty_input_available(1) == 0); /* ICANON still holds the byte */
        pty_console_key_event(10, 1); /* raw byte 0x0a is a line delimiter */
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 2);
        assert(bytes[0] == 30 && bytes[1] == 10);
        sessions[0].foreground_pgid = 50;
        pty_console_key_event(3, 1); /* raw byte 0x03 is still VINTR */
        assert(signal_count == 1 && signal_log[0].group == 50 && signal_log[0].signal == 2);
    } else if (!strcmp(name, "keyboard")) {
        use_raw_termios(1);
        const int modes[] = {K_OFF, K_RAW, K_MEDIUMRAW};
        for (unsigned i = 0; i < 3; ++i) {
            assert(pty_vt_set_keyboard_mode(1, modes[i]) == 0);
            pty_console_key_event(29, 1);
            pty_console_key_event(56, 1);
            pty_console_key_event(60, 1);
            pty_console_key_event(60, 0);
            pty_console_key_event(56, 0);
            pty_console_key_event(29, 0);
            assert(pty_vt_active() == 1);
            unsigned count = drain_bytes(1, bytes, sizeof(bytes));
            assert(count == (modes[i] == K_OFF ? 0u : 6u));
        }
        assert(pty_vt_set_graphics(1, 1) == 0);
        assert(pty_vt_set_keyboard_mode(1, K_RAW) == 0);
        pty_console_key_event(116, 1);
        pty_console_key_event(116, 0);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 4);
        assert(!memcmp(bytes, "\xe0\x1d\xe0\x9d", 4));
        assert(pty_vt_set_keyboard_mode(1, K_MEDIUMRAW) == 0);
        pty_console_key_event(112, 1);
        pty_console_key_event(112, 0);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 2);
        assert((uint8_t)bytes[0] == 125 && (uint8_t)bytes[1] == 253);
        assert(pty_vt_set_keyboard_mode(1, K_XLATE) == 0);
        pty_console_key_event(30, 1);
        assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a');
    } else if (!strcmp(name, "missing-controller")) {
        assert(pty_vt_set_mode(1, &mode) == 0);
        assert(pty_vt_set_graphics(1, 1) == 0);
        owners[0].pid = 0;
        assert(pty_vt_switch(2) == 0 && pty_vt_active() == 2);
        assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_AUTO);
    } else { assert(!"unknown regression"); }
    printf("PASS VT regression %s\n", name);
}

int main(int argc, char **argv)
{
    if (argc == 2) { regression(argv[1]); return 0; }
    char bytes[16];
    int keyboard_mode = -1;
    struct vt_mode probe;
    struct vt_mode controller = { .mode = VT_PROCESS,
                                  .relsig = SIG_RELEASE, .acqsig = SIG_ACQUIRE };
    struct vt_mode automatic = { .mode = VT_AUTO };
    struct vt_mode ack = { .mode = VT_ACKACQ,
                           .relsig = SIG_RELEASE, .acqsig = SIG_ACQUIRE };
    struct vt_mode junk = { .mode = 0x7f };

    owners[0] = (struct task){ .pid = 100, .process_session = 100,
                               .process_group = 100, .euid = 0 };
    owners[1] = (struct task){ .pid = 200, .process_session = 200,
                               .process_group = 200, .euid = 0 };
    current = &owners[0];

    pty_init();
    assert(pty_vt_init() == 0);
    assert(pty_vt_active() == 1);

    /* ---- VT_SETMODE accepts only the two Linux ownership modes. ---- */
    assert(pty_vt_set_mode(1, &ack) == -22);
    assert(pty_vt_set_mode(1, &junk) == -22);

    /* ---- Register controllers: Xorg owns tty1, a second client owns tty2. ---- */
    assert(pty_vt_set_mode(1, &controller) == 0);
    owners[0].controlling_pty_id = 1;
    current = &owners[1];
    assert(pty_vt_set_mode(2, &controller) == 0);
    owners[1].controlling_pty_id = 2;
    current = &owners[0];

    /* ---- Switching away defers the commit and signals the old controller. ---- */
    assert(pty_vt_set_graphics(1, 1) == 0);
    assert(pty_vt_graphical_active());
    signal_reset();
    assert(pty_vt_switch(2) == 0);
    assert(pty_vt_switch(2) == 0); /* duplicate kernel/Xorg request is idempotent */
    assert(pty_vt_active() == 1);
    assert(pty_vt_graphical_active());
    assert(shown_vt == 1);
    assert(signal_hits(SIG_RELEASE, 100) == 1);
    assert(signal_hits(SIG_ACQUIRE, 200) == 0);

    /* ---- VT_RELDISP(0) refuses the release and changes nothing. ---- */
    assert(pty_vt_release_display(1, 0) == 0);
    assert(pty_vt_active() == 1);
    assert(pty_vt_graphical_active());
    assert(shown_vt == 1 && shown_graphical == 1);
    assert(signal_hits(SIG_ACQUIRE, 200) == 0);
    assert(pty_vt_release_display(1, 1) == -22); /* terminal EINVAL, not replayable EAGAIN */
    assert(pty_vt_active() == 1);

    /* ---- Ask again, then grant: only VT_RELDISP(1) commits the target. ---- */
    assert(pty_vt_switch(2) == 0);
    assert(signal_hits(SIG_RELEASE, 100) == 2);
    assert(pty_vt_active() == 1);
    {
        uint64_t generation = pty_vt_generation();
        assert(pty_vt_release_display(1, 1) == 0);
        assert(pty_vt_generation() == generation + 1);
    }
    assert(pty_vt_active() == 2);
    assert(!pty_vt_graphical_active());
    assert(shown_vt == 2 && shown_graphical == 0);
    assert(signal_hits(SIG_ACQUIRE, 200) == 1);

    /* ---- The new controller keeps VT_PROCESS; ACKACQ only clears the pending. ---- */
    assert(pty_vt_get_mode(2, &probe) == 0 && probe.mode == VT_PROCESS);
    assert(pty_vt_release_display(2, 2) == 0);
    assert(pty_vt_get_mode(2, &probe) == 0 && probe.mode == VT_PROCESS);
    assert(pty_vt_release_display(2, 2) == 0); /* no-pending ACK is ignored */

    /* ---- Returning to tty1 runs the same protocol against tty2's controller. ---- */
    signal_reset();
    assert(pty_vt_switch(1) == 0);
    assert(pty_vt_active() == 2);
    assert(signal_hits(SIG_RELEASE, 200) == 1);
    assert(pty_vt_release_display(2, 1) == 0);
    assert(pty_vt_active() == 1);
    assert(signal_hits(SIG_ACQUIRE, 100) == 1);
    assert(pty_vt_release_display(1, 2) == 0);

    /* ---- Keyboard translation modes follow the Linux contract. ---- */
    assert(pty_vt_set_graphics(1, 0) == 0);
    current = &owners[1];
    assert(pty_vt_set_mode(2, &automatic) == 0);
    assert(pty_vt_set_mode(1, &automatic) == 0);
    current = &owners[0];
    use_raw_termios(1);
    assert(pty_vt_get_keyboard_mode(1, &keyboard_mode) == 0 && keyboard_mode == K_XLATE);

    /* K_OFF: nothing reaches the console PTY at all. */
    assert(pty_vt_set_keyboard_mode(1, K_OFF) == 0);
    pty_console_key_event(30, 1);
    pty_console_key_event(30, 0);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 0);

    /* K_OFF leaves VT switching to userspace VT_ACTIVATE. */
    pty_console_key_event(29, 1);
    pty_console_key_event(56, 1);
    pty_console_key_event(60, 1); /* F2 */
    pty_console_key_event(60, 0);
    pty_console_key_event(29, 0);
    pty_console_key_event(56, 0);
    assert(pty_vt_active() == 1);
    assert(pty_vt_switch(1) == 0);
    assert(pty_vt_active() == 1);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 0);

    /* K_RAW delivers the scan code, never a cooked character. */
    assert(pty_vt_set_keyboard_mode(1, K_RAW) == 0);
    pty_console_key_event(30, 1);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && (uint8_t)bytes[0] == 30);
    pty_console_key_event(30, 0);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 &&
           (uint8_t)bytes[0] == (uint8_t)(30 | 0x80));

    /* K_MEDIUMRAW is a raw stream as well. */
    assert(pty_vt_set_keyboard_mode(1, K_MEDIUMRAW) == 0);
    pty_console_key_event(30, 1);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && (uint8_t)bytes[0] == 30);
    pty_console_key_event(30, 0);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 &&
           (uint8_t)bytes[0] == (uint8_t)(30 | 0x80));

    /* K_XLATE keeps the cooked translation. */
    assert(pty_vt_set_keyboard_mode(1, K_XLATE) == 0);
    pty_console_key_event(30, 1);
    pty_console_key_event(30, 0);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a');
    assert(pty_vt_set_keyboard_mode(1, 0x7f) == -22);

    /* ---- Controller death restores a usable text VT. ---- */
    assert(pty_vt_set_graphics(1, 1) == 0);
    assert(pty_vt_set_mode(1, &controller) == 0);
    signal_reset();
    assert(pty_vt_switch(2) == 0); /* leaves a pending release on tty1 */
    assert(pty_vt_active() == 1);
    pty_process_session_exit(100);
    assert(pty_vt_graphical(1) == 0);              /* KD_TEXT restored */
    assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_AUTO);
    assert(!pty_vt_graphical_active());
    assert(shown_graphical == 0);
    assert(pty_vt_release_display(1, 1) == -22);   /* hand-off cancelled: no VT_PROCESS owner */
    /* The console is text-usable again: keystrokes reach tty1. */
    assert(pty_vt_switch(2) == 0);
    assert(pty_vt_active() == 2);
    assert(pty_vt_switch(1) == 0);
    assert(pty_vt_active() == 1);
    pty_console_key_event(30, 1);
    pty_console_key_event(30, 0);
    assert(drain_bytes(1, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a');
    /* And it can be claimed again by a fresh session. */
    assert(pty_vt_set_mode(1, &controller) == 0);
    assert(pty_vt_get_mode(1, &probe) == 0 && probe.mode == VT_PROCESS);

    /* ---- VT_OPENQRY follows open references, writing -1 if all are open. ---- */
    open_refs[1] = 1;
    assert(pty_vt_open_query() == 2); /* 1 is claimed and active, 2 is free */
    for (uint32_t number = 2; number <= 6; ++number) {
        assert(pty_vt_set_mode(number, &controller) == 0);
        open_refs[number] = 1;
    }
    assert(pty_vt_open_query() == -1);
    assert(pty_vt_set_mode(6, &automatic) == 0);
    open_refs[6] = 0;
    assert(pty_vt_open_query() == 6);

    /* ---- VT_GETSTATE reports a real per-console bitmap. ---- */
    {
        assert(pty_vt_state_bitmap() == 0x3fu); /* tty0 and tty1..5 */
    }

    printf("PASS Linux VT_PROCESS hand-off, keyboard modes and controller cleanup\n");
    return 0;
}
