#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/ipc/socket.c"
#include "../../kernel/reliefnt/kernel/syscall/ipc/socket_batch.c"

static struct task current;
static struct unix_socket peers[2];
static struct task_file endpoints[2], passed;
static uint64_t readonly;
static unsigned allocations;
static unsigned pty_holds;
/* AF_UNIX cases must not cross into the IPv4 backend. */
int task_udp_recv(struct task_file *f, void *b, uint32_t n, uint32_t flags, uint64_t a, uint64_t l)
{ (void)f; (void)b; (void)n; (void)flags; (void)a; (void)l; abort(); }
int task_udp_send(struct task_file *f, const void *b, uint32_t n, uint32_t flags, uint64_t a, uint32_t l)
{ (void)f; (void)b; (void)n; (void)flags; (void)a; (void)l; abort(); }
short net_socket_poll_fd(int32_t h, uint32_t p, short e)
{ (void)h; (void)p; (void)e; abort(); }
int task_packet_recv(struct task_file *file, void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_packet_send(struct task_file *file, const void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int net_socket_error(int32_t h, bool c) { (void)h; (void)c; abort(); }
int net_socket_recv(struct reliefos_net_socket_io *r, uint32_t p) { (void)r; (void)p; abort(); }
int net_socket_send(struct reliefos_net_socket_io *r, uint32_t p) { (void)r; (void)p; abort(); }
int task_pty_export_fd(struct task *task, int fd, struct task_pty_fd *out)
{
    (void)task;
    if (fd != 0) return -LINUX_EBADF;
    *out = (struct task_pty_fd){.used = 1, .pty_id = 7, .endpoint = TASK_PTY_ENDPOINT_SLAVE};
    ++pty_holds;
    return 0;
}
void task_pty_release_entry(struct task_pty_fd *entry)
{
    assert(entry->pty_id == 7 && entry->endpoint == TASK_PTY_ENDPOINT_SLAVE && pty_holds);
    --pty_holds;
    *entry = (struct task_pty_fd){0};
}
int task_pty_import_fd(struct task *task, const struct task_pty_fd *source, uint32_t flags)
{
    (void)task; (void)flags;
    assert(source->pty_id == 7 && pty_holds);
    return 7;
}
void *kernel_malloc(size_t size) { void *p = malloc(size); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
struct task *sched_current_task(void) { return &current; }
uint32_t sched_current_pid(void) { return current.pid; }
struct task *sched_find(uint32_t pid) { return pid == current.pid ? &current : NULL; }
uint64_t time_ticks(void) { return 100; }
int time_clock_get(int32_t id, struct linux_timespec *out)
{ assert(id == LINUX_CLOCK_MONOTONIC); *out = (struct linux_timespec){1, 0}; return 0; }
bool user_range_ok(uint64_t p, uint64_t size) { return !size || (p >= 4096 && size <= UINT64_MAX - p); }
bool user_range_writable(uint64_t p, uint64_t size) { return user_range_ok(p, size) && p != readonly; }
int user_copy_to_task(struct task *task, uint64_t p, const void *source, uint64_t size)
{ assert(task == &current); if (!user_range_writable(p, size)) return -LINUX_EFAULT; memcpy((void *)p, source, size); return 0; }
struct kernel_object_table *kernel_objects(void) { return NULL; }
void *kernel_object_lookup(struct kernel_object_table *table, uint32_t id, enum kernel_object_type type)
{ (void)table; assert(type == KERNEL_OBJECT_SOCKET); return id >= 1 && id <= 2 ? &peers[id - 1] : NULL; }
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd == 3 ? &endpoints[0] : fd == 4 ? &endpoints[1] : fd == 5 ? &passed : NULL; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ return task->syscall_file ? task->syscall_file : task_file_for_fd(task, fd); }
struct task_file *task_file_get(struct task_file *file) { ++file->references; return file; }
void task_file_put(struct task_file *file) { assert(file->references); --file->references; }
int task_file_reference(struct task_file *destination, struct task_file *source)
{ *destination = (struct task_file){.used = 1, .description = task_file_get(source)}; return 0; }
int task_allocate_fd(struct task *task, int minimum, struct task_file **slot)
{ assert(minimum == 0); *slot = &task->files[0]; return 6; }
struct task_file *task_descriptor_for_fd(struct task *task, int fd) { assert(fd == 6); return &task->files[0]; }
void clear_task_file(struct task_file *file) { task_file_put(file->description); *file = (struct task_file){0}; }
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{ task->waiting_queue = queue; return 0; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ if (current.waiting_queue == queue) { current.waiting_queue = NULL; current.state = TASK_READY; return 1; } return 0; }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ if (task->waiting_queue == queue) task->waiting_queue = NULL; }
/* This fixture exercises socket batching, not global poll waiters. */
void kernel_poll_readiness_notify(void) { }
int fs_permissions_resolve_flags(const struct task *task, const char *base, const char *path,
    char *out, uint32_t capacity, bool real_ids, uint32_t flags)
{ (void)task; (void)base; (void)path; (void)out; (void)capacity; (void)real_ids; (void)flags; assert(0); return -1; }
int fs_permissions_check(const struct task *task, const char *path, uint32_t access, bool real_ids)
{ (void)task; (void)path; (void)access; (void)real_ids; assert(0); return -1; }
void sched_block_current(void) { current.state = TASK_BLOCKED; }
void sched_sleep_current_until(uint64_t deadline) { current.wake_tick = deadline; sched_block_current(); }
int sched_signal_user_task(uint32_t pid, int sig) { (void)pid; assert(sig == 13); return 0; }

static int batch(bool receive, struct mmsghdr *messages, unsigned count, unsigned flags)
{
    current.syscall_file = &endpoints[receive];
    return syscall_socket_mmsg(receive, receive ? 4 : 3, (uintptr_t)messages, count, flags, 0);
}

int main(void)
{
    current.kind = TASK_KIND_USER;
    current.pid = current.tgid = 10;
    current.uid = current.gid = 100;
    current.state = TASK_READY;
    for (unsigned i = 0; i < 2; ++i) {
        peers[i] = (struct unix_socket){.used = 1, .refs = 1, .handle = i + 1,
            .type = SOCK_DGRAM, .state = UNIX_SOCKET_CONNECTED, .peer_handle = 2 - i,
            .receive_timeout = UINT64_MAX, .send_timeout = UINT64_MAX};
        endpoints[i] = (struct task_file){.used = 1, .references = 1, .aux = i + 1,
            .flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX | RELIEFOS_O_RDWR};
    }
    char payload[3][4] = {"one", "two", "end"}, buffer[3][4] = {{0}};
    struct iovec tx[3], rx[3];
    struct mmsghdr sends[3] = {0}, receives[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        tx[i] = (struct iovec){payload[i], 3}; rx[i] = (struct iovec){buffer[i], 4};
        sends[i].msg_hdr.msg_iov = &tx[i]; sends[i].msg_hdr.msg_iovlen = 1;
        receives[i].msg_hdr.msg_iov = &rx[i]; receives[i].msg_hdr.msg_iovlen = 1;
    }
    assert(batch(false, sends, 3, MSG_NOSIGNAL) == 3 && peers[1].packet_count == 3);
    assert(batch(true, receives, 3, MSG_DONTWAIT) == 3 && !peers[1].packet_count);
    for (unsigned i = 0; i < 3; ++i) assert(receives[i].msg_len == 3 && !memcmp(payload[i], buffer[i], 3));
    assert(batch(true, receives, 3, MSG_DONTWAIT) == -LINUX_EAGAIN);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 3, MSG_WAITFORONE) == 1 && !current.mmsg.active);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 3, 0) == KERNEL_SYSCALL_BLOCKED && current.mmsg.count == 1);
    /* Another task sends two packets while this receive batch is suspended. */
    struct socket_message_result result;
    assert(task_socket_message(&current, &endpoints[0], (uintptr_t)&sends[1], MSG_NOSIGNAL, false, false, &result) == 3);
    assert(task_socket_message(&current, &endpoints[0], (uintptr_t)&sends[2], MSG_NOSIGNAL, false, false, &result) == 3);
    assert(batch(true, receives, 3, 0) == 3 && !current.mmsg.active);
    readonly = (uintptr_t)&receives[0].msg_len;
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 1, MSG_DONTWAIT) == -LINUX_EFAULT && !peers[1].packet_count);
    readonly = 0;
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    readonly = (uintptr_t)&receives[0].msg_hdr.msg_flags;
    assert(batch(true, receives, 1, MSG_DONTWAIT) == -LINUX_EFAULT && !peers[1].packet_count);
    readonly = 0;
    /* Data copy faults consume datagrams, but MSG_PEEK keeps them queued. */
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    readonly = (uintptr_t)rx[0].iov_base;
    assert(batch(true, receives, 1, MSG_DONTWAIT | MSG_PEEK) == -LINUX_EFAULT && peers[1].packet_count == 1);
    assert(batch(true, receives, 1, MSG_DONTWAIT) == -LINUX_EFAULT && !peers[1].packet_count);
    readonly = 0;
    void *good_buffer = rx[0].iov_base;
    rx[0].iov_base = (void *)1;
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 1, MSG_DONTWAIT) == -LINUX_EFAULT && !peers[1].packet_count);
    rx[0].iov_base = good_buffer;
    passed = (struct task_file){.used = 1, .references = 1};
    unsigned char ancillary[CMSG_SPACE(sizeof(int))] __attribute__((aligned(8))) = {0};
    struct cmsghdr *header = (void *)ancillary;
    *header = (struct cmsghdr){CMSG_LEN(sizeof(int)), SOL_SOCKET, SCM_RIGHTS};
    int fd = 5;
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    sends[0].msg_hdr.msg_control = ancillary; sends[0].msg_hdr.msg_controllen = sizeof(ancillary);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1 && passed.references == 2 && passed.scm_references == 1);
    unsigned char control[32] __attribute__((aligned(8)));
    receives[0].msg_hdr.msg_control = control; receives[0].msg_hdr.msg_controllen = sizeof(control);
    assert(batch(true, receives, 1, MSG_CMSG_CLOEXEC) == 1);
    header = (void *)control;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    assert(fd == 6 && current.files[0].description == &passed && current.files[0].fd_flags == RELIEFOS_FD_CLOEXEC);
    assert(receives[0].msg_hdr.msg_flags & MSG_CMSG_CLOEXEC);
    assert(passed.references == 2 && !passed.scm_references);
    clear_task_file(&current.files[0]);
    assert(passed.references == 1 && !allocations);
    /* A stream batch stops after a short send and retains the unsent slots. */
    peers[0].type = peers[1].type = SOCK_STREAM;
    peers[1].receive_lowwater = 1;
    peers[0].rx_read = peers[0].rx_written = peers[1].rx_read = peers[1].rx_written = 0;
    char large[UNIX_SOCKET_RX_CAP + 64] = {0};
    tx[0] = (struct iovec){large, sizeof(large)};
    sends[0].msg_hdr.msg_control = NULL; sends[0].msg_hdr.msg_controllen = 0;
    assert(batch(false, sends, 3, MSG_NOSIGNAL) == 1);
    assert(sends[0].msg_len == UNIX_SOCKET_RX_CAP - 1);
    char drained[UNIX_SOCKET_RX_CAP];
    rx[0] = (struct iovec){drained, sizeof(drained)};
    receives[0].msg_hdr.msg_control = NULL; receives[0].msg_hdr.msg_controllen = 0;
    assert(batch(true, receives, 1, MSG_DONTWAIT) == 1);
    assert(receives[0].msg_len == UNIX_SOCKET_RX_CAP - 1 && !allocations);
    tx[0] = (struct iovec){payload[0], 3};
    rx[0] = (struct iovec){buffer[0], sizeof(buffer[0])};
    receives[0].msg_hdr.msg_control = control;
    receives[0].msg_hdr.msg_controllen = sizeof(control);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 1, MSG_WAITALL | MSG_CMSG_CLOEXEC) == KERNEL_SYSCALL_BLOCKED);
    assert(current.socket_receive_done == 3 && receives[0].msg_hdr.msg_controllen == sizeof(control));
    peers[0].shutdown_write = 1;
    assert(batch(true, receives, 1, MSG_WAITALL | MSG_CMSG_CLOEXEC) == 1);
    assert(receives[0].msg_len == 3 && receives[0].msg_hdr.msg_controllen == 0);
    assert(receives[0].msg_hdr.msg_flags == MSG_CMSG_CLOEXEC && !allocations);
    assert(!current.socket_receive_file && endpoints[1].references == 1);
    peers[0].shutdown_write = 0;
    peers[1].receive_timeout = 1;
    current.socket_io_deadline = 0;
    receives[0].msg_hdr.msg_controllen = sizeof(control);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1);
    assert(batch(true, receives, 1, MSG_WAITALL | MSG_CMSG_CLOEXEC) == KERNEL_SYSCALL_BLOCKED);
    current.socket_io_deadline = time_ticks();
    assert(batch(true, receives, 1, MSG_WAITALL | MSG_CMSG_CLOEXEC) == 1);
    assert(receives[0].msg_len == 3 && receives[0].msg_hdr.msg_controllen == 0);
    assert(receives[0].msg_hdr.msg_flags == MSG_CMSG_CLOEXEC && !allocations);
    /* PTY rights use the same queue ownership as ordinary files. */
    unsigned char pty_control[CMSG_SPACE(sizeof(int))] = {0};
    struct cmsghdr *pty_header = (void *)pty_control;
    *pty_header = (struct cmsghdr){.cmsg_len = CMSG_LEN(sizeof(int)),
        .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS};
    int input_fd = 0;
    memcpy(CMSG_DATA(pty_header), &input_fd, sizeof(input_fd));
    struct msghdr pty_message = {.msg_control = pty_control, .msg_controllen = sizeof(pty_control)};
    struct unix_rights *held = NULL;
    struct ucred credential;
    bool explicit;
    assert(unix_message_rights(&current, &pty_message, &held, &credential, &explicit) == 0);
    assert(held && held->count == 1 && !held->files[0] && pty_holds == 1);
    unix_rights_free(held);
    assert(!pty_holds && !allocations);
    input_fd = 12345;
    memcpy(CMSG_DATA(pty_header), &input_fd, sizeof(input_fd));
    assert(unix_message_rights(&current, &pty_message, &held, &credential, &explicit) == -LINUX_EBADF);
    assert(!held && !pty_holds && !allocations);
    input_fd = 0;
    memcpy(CMSG_DATA(pty_header), &input_fd, sizeof(input_fd));
    sends[0].msg_hdr.msg_control = pty_control;
    sends[0].msg_hdr.msg_controllen = sizeof(pty_control);
    receives[0].msg_hdr.msg_control = control;
    receives[0].msg_hdr.msg_controllen = sizeof(control);
    assert(batch(false, sends, 1, MSG_NOSIGNAL) == 1 && pty_holds == 1);
    assert(batch(true, receives, 1, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) == 1);
    int received_pty;
    memcpy(&received_pty, CMSG_DATA((struct cmsghdr *)control), sizeof(received_pty));
    assert(received_pty == 7 && !pty_holds && !allocations);
    current.uid = current.euid = current.suid = 0;
    current.gid = current.egid = current.sgid = 0;
    const struct { uint64_t caps; struct ucred sent; int error; } credential_cases[] = {
        {0, {10, 1234, 0}, -LINUX_EPERM},
        {0, {10, 0, 1234}, -LINUX_EPERM},
        {0, {1234, 0, 0}, -LINUX_EPERM},
        {1ULL << CAP_SETUID, {10, 1234, 0}, 0},
        {1ULL << CAP_SETGID, {10, 0, 1234}, 0},
        {1ULL << CAP_SETUID, {10, 0, 1234}, -LINUX_EPERM},
        {1ULL << CAP_SETGID, {10, 1234, 0}, -LINUX_EPERM},
        {1ULL << CAP_SYS_ADMIN, {1234, 0, 0}, -LINUX_ESRCH},
        {UINT64_MAX, {10, UINT32_MAX, 0}, -LINUX_EINVAL},
    };
    for (unsigned i = 0; i < sizeof(credential_cases) / sizeof(credential_cases[0]); ++i) {
        char cmsg[CMSG_SPACE(sizeof(struct ucred))] = {0};
        struct cmsghdr *h = (void *)cmsg;
        *h = (struct cmsghdr){.cmsg_len = CMSG_LEN(sizeof(struct ucred)),
            .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_CREDENTIALS};
        memcpy(CMSG_DATA(h), &credential_cases[i].sent, sizeof(struct ucred));
        struct msghdr m = {.msg_control = cmsg, .msg_controllen = sizeof(cmsg)};
        current.cap_effective = credential_cases[i].caps;
        assert(unix_message_rights(&current, &m, &held, &credential, &explicit) == credential_cases[i].error);
        assert(!held && !allocations);
    }
    puts("PASS actual Unix mmsg: packet boundaries, WAITFORONE, blocking resume, length faults, SCM_RIGHTS references/CLOEXEC");
}
