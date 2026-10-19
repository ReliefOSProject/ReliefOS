#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sound/asound.h>
#include <linux/soundcard.h>
#include <linux/poll.h>
#include <reliefnt/sched.h>

static struct task *descriptor_dispatch_task;
static unsigned descriptor_dispatch_sleep_count;
static struct task *descriptor_dispatch_current(void)
{ return descriptor_dispatch_task; }
static uint32_t descriptor_dispatch_pid(void)
{ return descriptor_dispatch_task ? descriptor_dispatch_task->pid : 0; }
static void descriptor_dispatch_sleep(uint64_t deadline)
{ (void)deadline; ++descriptor_dispatch_sleep_count; }
static int64_t descriptor_dispatch_regs(struct task *task, uint64_t number,
                                        uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5);
static int64_t descriptor_dispatch_clone(const struct trap_frame *frame,
                                         uint64_t flags, uint64_t stack,
                                         uint64_t parent_tid, uint64_t child_tid,
                                         uint64_t tls);
static int64_t descriptor_dispatch_fork(const struct trap_frame *frame);
static int64_t descriptor_dispatch_vfork(const struct trap_frame *frame);

#define sched_current_task descriptor_dispatch_current
#define sched_current_pid descriptor_dispatch_pid
#define sched_sleep_current_until descriptor_dispatch_sleep
#define sched_clone_current descriptor_dispatch_clone
#define sched_fork_current descriptor_dispatch_fork
#define sched_vfork_current descriptor_dispatch_vfork
#define RELIEFOS_SYSCALL_FRAME_DISPATCH descriptor_dispatch_regs
#include "../../kernel/reliefnt/kernel/reliefnt/syscall.c"
#undef RELIEFOS_SYSCALL_FRAME_DISPATCH
#undef sched_vfork_current
#undef sched_fork_current
#undef sched_clone_current
#undef sched_current_task
#undef sched_current_pid
#undef sched_sleep_current_until

/* This descriptor fixture never registers a DRM node. */
void drm_file_close(struct task_file *file)
{ if (file) file->drm_file = NULL; }
void drm_vmwgfx_cancel_task_continuation(struct task *task) { (void)task; }
uint32_t drm_vmwgfx_process_retirements(uint32_t budget)
{ (void)budget; return 0; }

static unsigned descriptor_drm_read_count;
int drm_file_read(struct task *task, struct task_file *file,
                  void *buffer, uint32_t length)
{
    assert(task == descriptor_dispatch_task && file && file->drm_file);
    assert(buffer && length == 1);
    ++descriptor_drm_read_count;
    return (file->flags & RELIEFOS_O_NONBLOCK) ? -RELIEFOS_EAGAIN
                                                : KERNEL_SYSCALL_BLOCKED;
}
int64_t drm_file_ioctl(struct task *task, struct task_file *file,
                       uint32_t command, uint64_t argument)
{ (void)task; (void)file; (void)command; (void)argument; abort(); }
uint32_t drm_file_poll(struct task_file *file, uint32_t events)
{ (void)file; (void)events; abort(); }


int64_t syscall_clone3(const struct trap_frame *frame, uint64_t arguments,
                       uint64_t size)
{ (void)frame; (void)arguments; (void)size; abort(); }
int64_t kernel_signal_rt_sigreturn(struct task *task, struct trap_frame *frame)
{ (void)task; (void)frame; abort(); }
void net_close_owner_sockets(uint32_t owner_pid) { (void)owner_pid; }

static int64_t descriptor_dispatch_regs(struct task *task, uint64_t number,
                                        uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    assert((number == LINUX_SYS_READ || number == LINUX_SYS_READV) &&
           task == descriptor_dispatch_task);
    struct task_file *file = task_file_for_io(task, (int)a0);
    assert(file && file->drm_file);
    if (number == LINUX_SYS_READV) {
        assert(a2 == 1);
        const struct iovec *vector = (const struct iovec *)(uintptr_t)a1;
        return drm_file_read(task, file, vector[0].iov_base,
                             (uint32_t)vector[0].iov_len);
    }
    return drm_file_read(task, file, (void *)(uintptr_t)a1, (uint32_t)a2);
}
static int64_t descriptor_dispatch_clone(const struct trap_frame *frame,
                                         uint64_t flags, uint64_t stack,
                                         uint64_t parent_tid, uint64_t child_tid,
                                         uint64_t tls)
{ (void)frame; (void)flags; (void)stack; (void)parent_tid; (void)child_tid; (void)tls; abort(); }
static int64_t descriptor_dispatch_fork(const struct trap_frame *frame)
{ (void)frame; abort(); }
static int64_t descriptor_dispatch_vfork(const struct trap_frame *frame)
{ (void)frame; abort(); }

void driver_manager_process_audio_faults(void) { }
void input_process_pending(void) { }
void storage_set_io_async_context(bool enabled) { (void)enabled; }
void storage_release_task_io(uint32_t pid) { (void)pid; }

#define AUDIO_FIXTURE_REAL_WAIT
#define AUDIO_FIXTURE_NO_PTHREAD
#include "audio_fixture.h"
#include "audio_fake_card.h"
#include "audio_device_fixture.h"

/* Socket scheduling selects a host task; OFDs, queues, object handles and
 * sendmsg/recvmsg are production implementations. No case may park or signal. */
static struct task *descriptor_socket_task;
static struct task *descriptor_socket_current(void)
{ assert(descriptor_socket_task); return descriptor_socket_task; }
static void descriptor_socket_park(void) { abort(); }
static void descriptor_socket_sleep(uint64_t deadline) { (void)deadline; abort(); }
static int descriptor_socket_signal(uint32_t pid, int signal)
{ (void)pid; (void)signal; abort(); }
#define sched_current_task descriptor_socket_current
#define sched_block_current descriptor_socket_park
#define sched_sleep_current_until descriptor_socket_sleep
#define sched_signal_user_task descriptor_socket_signal
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_socket.c"
#undef sched_current_task
#undef sched_block_current
#undef sched_sleep_current_until
#undef sched_signal_user_task

static int fail_allocation;
static unsigned pty_references;
void *kernel_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
void kernel_free(void *memory) { free(memory); }
/* Descriptor scheduling fixtures model a single active CPU, the BSP. */
uint32_t smp_cpu_count(void) { return 1; }
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/* OSS tests bind a registered PCM. Legacy discovery reports no hardware;
 * entering a legacy backend after that result is a failure. */
void driver_manager_audio_get_state(struct reliefos_audio_state *state)
{ *state = (struct reliefos_audio_state){0}; }
int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{ (void)format; abort(); }
long driver_manager_audio_write(const void *data, uint32_t count, uint32_t *status)
{ (void)data; (void)count; (void)status; abort(); }
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state)
{ assert(generation && state); return -ENODEV; }
void driver_manager_audio_release(uint32_t generation)
{ (void)generation; abort(); }
int driver_manager_audio_configure_bound(uint32_t generation,
                                         const struct reliefos_audio_format *format)
{ (void)generation; (void)format; abort(); }
long driver_manager_audio_write_bound(uint32_t generation, const void *data,
                                      uint32_t count, uint32_t *status)
{ (void)generation; (void)data; (void)count; (void)status; abort(); }
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *state)
{ (void)generation; (void)state; abort(); }
/* This descriptor test uses anonymous objects, never storage-backed files. */
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }
int fs_permissions_resolve_flags(const struct task *task, const char *base, const char *input,
                                char *out, uint32_t cap, bool real_ids, uint32_t flags)
{ (void)task; (void)base; (void)input; (void)out; (void)cap; (void)real_ids; (void)flags; abort(); }
void task_pipe_release(struct task_file *file) { (void)file; }
void task_shm_release(struct task_file *file) { (void)file; }
/* AF_UNIX audio transfers must not enter storage or IPv4 backends. */
int fs_permissions_check(const struct task *task, const char *path, uint32_t access, bool real_ids)
{ (void)task; (void)path; (void)access; (void)real_ids; abort(); }
void task_packet_release(struct task_file *file) { (void)file; abort(); }
void task_udp_release(struct task_file *file) { (void)file; abort(); }
void net_socket_release_fd(int32_t handle) { (void)handle; abort(); }
int task_packet_recv(struct task_file *file, void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_packet_send(struct task_file *file, const void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_udp_recv(struct task_file *file, void *data, uint32_t length,
                  uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_udp_send(struct task_file *file, const void *data, uint32_t length,
                  uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
short net_socket_poll_fd(int32_t handle, uint32_t owner, short events)
{ (void)handle; (void)owner; (void)events; abort(); }
int net_socket_error(int32_t handle, bool clear) { (void)handle; (void)clear; abort(); }
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner)
{ (void)request; (void)owner; abort(); }
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner)
{ (void)request; (void)owner; abort(); }
uint64_t time_ticks(void) { return 100; }
void input_evdev_release(uint32_t kind, uint64_t token)
{ (void)kind; (void)token; }
void pty_reap_hungup(uint32_t id) { (void)id; }
int pty_is_active(uint32_t id) { return id == 7; }
int pty_slave_open_allowed(uint32_t id) { return id == 7; }
int pty_transfer_get(uint32_t id, uint32_t endpoint)
{ assert(id == 7 && endpoint == TASK_PTY_ENDPOINT_SLAVE); ++pty_references; return 0; }
void pty_transfer_put(uint32_t id, uint32_t endpoint)
{ assert(id == 7 && endpoint == TASK_PTY_ENDPOINT_SLAVE && pty_references); --pty_references; }
bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_range_ok(address, length); }

static int64_t descriptor_control_value;
static uint32_t descriptor_control_count(void *opaque)
{ (void)opaque; return 1; }
static int descriptor_control_info(void *opaque, uint32_t control,
                                    struct audio_control_info *info)
{
    (void)opaque;
    assert(control == 0);
    *info = (struct audio_control_info){.id = 1, .type = AUDIO_CONTROL_INTEGER,
        .count = 1, .min = 0, .max = 40, .step = 1,
        .access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE};
    strcpy(info->name, "Playback Volume");
    return 0;
}
static int descriptor_control_read(void *opaque, uint32_t control,
                                    struct audio_control_value *value)
{
    (void)opaque; assert(control == 0);
    *value = (struct audio_control_value){.values = {descriptor_control_value}};
    return 0;
}
static int descriptor_control_write(void *opaque, uint32_t control,
                                     const struct audio_control_value *value)
{
    (void)opaque; assert(control == 0);
    descriptor_control_value = value->values[0];
    return 0;
}

static struct task *descriptor_audio_task(uint32_t pid)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = pid;
    task->limits.nofile.rlim_cur = 64;
    return task;
}

static int descriptor_audio_open(struct task *task, uint32_t card, int control)
{
    struct task_file *file;
    int fd = task_allocate_fd(task, 0, &file);
    assert(fd == 3);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_RDWR;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | (control ? STORAGE_NODE_FLAG_AUDIO_CONTROL : STORAGE_NODE_FLAG_AUDIO_PCM),
        .first_cluster = STORAGE_DEV_KIND_AUDIO,
        .volume_id = control ? card : AUDIO_DEVICE_VOLUME_ID(card, 0, AUDIO_PLAYBACK)};
    assert((control ? audio_control_open(task, file) : audio_device_open(task, file)) == 0);
    if (!control) audio_test_configure_file(file);
    return fd;
}

/* Missing final close must leave the observer locked out; premature close
 * must break the child/pinned read. These test actual syscall OFD helpers. */
static void test_audio_control_descriptions(void)
{
    struct audio_test_card card = {0};
    struct audio_card_ops ops = audio_test_ops;
    ops.control_count = descriptor_control_count;
    ops.control_info = descriptor_control_info;
    ops.control_read = descriptor_control_read;
    ops.control_write = descriptor_control_write;
    struct audio_card_identity identity = {.id = "ofd", .name = "ofd-test"};
    assert(audio_register_card_owned(&identity, &ops, &card, 0x7a, &card.id) == 0);
    struct task *parent = descriptor_audio_task(1000), *child = descriptor_audio_task(1001);
    struct task *observer = descriptor_audio_task(1002);
    int fd = descriptor_audio_open(parent, card.id, 1);
    int observer_fd = descriptor_audio_open(observer, card.id, 1);
    struct task_file *source = task_file_for_fd(parent, fd);
    struct task_file *independent = task_file_for_fd(observer, observer_fd);
    struct snd_ctl_elem_id id = {.numid = 1, .iface = SNDRV_CTL_ELEM_IFACE_MIXER};
    struct snd_ctl_elem_info info = {.id = id};
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == -1 && !(info.access & (SNDRV_CTL_ELEM_ACCESS_LOCK | SNDRV_CTL_ELEM_ACCESS_OWNER)));
    assert(audio_control_ioctl(parent, source, SNDRV_CTL_IOCTL_ELEM_LOCK, (uintptr_t)&id) == 0);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == 1000 && (info.access & SNDRV_CTL_ELEM_ACCESS_LOCK));
    assert(!(info.access & SNDRV_CTL_ELEM_ACCESS_OWNER));
    int subscribe = 1;
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) == 0);
    struct snd_ctl_elem_value value = {.id = id, .value.integer.value = {25}};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
    fail_allocation = 1;
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == -RELIEFOS_ENOMEM);
    assert(!source->description && !task_file_for_fd(parent, 4));
    fail_allocation = 0;
    int duplicate = task_duplicate_file_fd(parent, fd, 0, 0);
    assert(duplicate == 4);
    source = task_file_for_fd(parent, fd);
    assert(source == task_file_for_fd(parent, duplicate) && source->references == 2);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(source == task_file_for_fd(child, fd) && source->references == 4);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == 1000 && (info.access & SNDRV_CTL_ELEM_ACCESS_OWNER));
    assert(audio_alsa_control_ioctl(task_file_for_fd(child, duplicate)->audio_control_file,
                                    SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
    struct snd_ctl_event event;
    assert(audio_control_read_file(task_file_for_fd(child, fd)->audio_control_file, &event, sizeof(event)) == sizeof(event));
    assert(event.data.elem.id.numid == 1 && event.data.elem.mask == 1);
    assert(audio_control_read_file(source->audio_control_file, &event, sizeof(event)) == -EAGAIN);
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(parent->syscall_file == task_file_description(source));
    assert(source->references == 5);
    clear_task_files(parent);
    clear_task_files(child);
    assert(source->references == 1);
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
    audio_control_changed(card.id, 0);
    assert(audio_control_read_file(parent->syscall_file->audio_control_file, &event, sizeof(event)) == sizeof(event));
    task_release_syscall_file(parent);
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == -1 && !(info.access & (SNDRV_CTL_ELEM_ACCESS_LOCK | SNDRV_CTL_ELEM_ACCESS_OWNER)));
    clear_task_files(observer);
    sched_task_file_release(parent); sched_task_file_release(child); sched_task_file_release(observer);
    free(parent); free(child); free(observer);
    assert(audio_unregister_card(card.id, 0) == 0);
    puts("PASS actual control OFD: dup/fork shared queue/owner, ENOMEM rollback, I/O pin and final unlock");
}

static void test_audio_pcm_descriptions(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *parent = descriptor_audio_task(1100), *child = descriptor_audio_task(1101);
    int fd = descriptor_audio_open(parent, card.id, 0);
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == 4);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    struct task_file *source = task_file_for_fd(parent, fd);
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(parent->syscall_file == task_file_description(source));
    assert(card.open_count == 1 && card.close_count == 0);
    clear_task_files(parent); clear_task_files(child);
    assert(!syscall_ioctl_resolve_fd(parent, fd));
    assert(task_file_for_io(parent, fd) == parent->syscall_file);
    struct task_file *replacement;
    assert(task_allocate_fd(parent, 0, &replacement) == fd);
    replacement->used = 1;
    replacement->flags = TASK_FILE_FLAG_DEV_NULL;
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(task_file_for_io(parent, fd) != task_file_for_fd(parent, fd));
    assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    uint8_t samples[4] = {0};
    assert(audio_device_write(NULL, parent->syscall_file, samples, sizeof(samples)) == 4);
    task_release_syscall_file(parent);
    clear_task_files(parent);
    assert(card.close_count == 1 && audio_unregister_card(card.id, 0) == 0);
    sched_task_file_release(parent); sched_task_file_release(child);
    free(parent); free(child);
    puts("PASS actual PCM OFD: dup/fork/exit retain stream until last in-flight I/O reference");
}

static void descriptor_socket_pair(struct task *sender, struct task *receiver,
                                    int *send_fd, int *receive_fd)
{
    struct unix_socket *left = unix_alloc(sender->pid, SOCK_DGRAM);
    struct unix_socket *right = unix_alloc(receiver->pid, SOCK_DGRAM);
    assert(left && right);
    left->state = right->state = UNIX_SOCKET_CONNECTED;
    left->peer_handle = right->handle;
    right->peer_handle = left->handle;
    *send_fd = unix_alloc_fd(sender, left);
    *receive_fd = unix_alloc_fd(receiver, right);
    assert(*send_fd == 4 && *receive_fd == 3);
    unix_release_handle(left->handle);
    unix_release_handle(right->handle);
}

static void test_audio_oss_descriptions(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *parent = descriptor_audio_task(1200), *child = descriptor_audio_task(1201);
    struct task_file *file;
    int fd = task_allocate_fd(parent, 0, &file);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_WRONLY;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_AUDIO};
    assert(!audio_oss_open(parent, file));
    assert(card.open_count == 1 && audio_unregister_card(card.id, 0) == -EBUSY);
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == 4);
    child->fd_table = parent->fd_table;
    assert(!syscall_clone_task_files(parent, child));
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    struct task_file *held = parent->syscall_file;
    assert(held == task_file_for_fd(child, 4));
    assert(!audio_oss_ioctl(child, task_file_for_fd(child, 4), SNDCTL_DSP_NONBLOCK, 0));
    assert(held->flags & RELIEFOS_O_NONBLOCK);
    clear_task_files(parent); clear_task_files(child);
    assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    uint8_t data[4] = {0};
    assert(audio_oss_write(NULL, held, data, sizeof(data)) == sizeof(data));
    task_release_syscall_file(parent);
    assert(card.close_count == 1 && !audio_unregister_card(card.id, 0));
    sched_task_file_release(parent); sched_task_file_release(child);
    free(parent); free(child);
    puts("PASS actual OSS OFD: open lease, shared NONBLOCK, dup/fork/exit and final ioctl pin release");
}

static void test_audio_write_batch(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *task = descriptor_audio_task(1300);
    struct task_file *file;
    assert(task_allocate_fd(task, 0, &file) >= 0);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_WRONLY | RELIEFOS_O_NONBLOCK;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_AUDIO};
    assert(!audio_oss_open(task, file));
    int fragments = (8 << 16) | 13; /* A 64 KiB ring on the bounded fake card. */
    assert(!audio_oss_ioctl(task, file, SNDCTL_DSP_SETFRAGMENT, (uintptr_t)&fragments));
    uint8_t data[98304]; /* Entire large partial-write request stays readable. */
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 13u);
    /* One real Doom block must reach the OSS core in one bounded syscall
     * request, rather than five forced 4 KiB returns/scheduling boundaries. */
    uint32_t length = task_device_write_length(file, 5120u * 4u);
    assert(length == 5120u * 4u);
    assert(audio_oss_write(NULL, file, data, length) == (int)length);
    assert(card.prepared_dma.bytes == 65536u);
    assert(!memcmp(card.prepared_dma.kernel, data, length));
    assert(task_device_write_length(file, UINT64_MAX) == 32768u);
    assert(task_device_write_length(file, 0) == 0);
    assert(task_device_write_length(file, 3) == 3);
    assert(audio_oss_write(NULL, file, data + length, 32768u) == 32768);
    uint32_t remaining = 65536u - length - 32768u;
    assert(audio_oss_write(NULL, file, data + length + 32768u, 32768u) == (int)remaining);
    assert(!memcmp(card.prepared_dma.kernel, data, 65536u));
    assert(audio_oss_write(NULL, file, data, 4) == -EAGAIN);
    struct task_file other = {.flags = TASK_FILE_FLAG_DEV_NODE,
        .node = {.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_ZERO}};
    assert(task_device_write_length(&other, UINT64_MAX) == RELIEFOS_FS_IO_SLICE_BYTES);
    clear_task_files(task);
    assert(!audio_unregister_card(card.id, 0));
    sched_task_file_release(task);
    free(task);
    puts("PASS actual OSS syscall batching: whole Doom block, bounded large request, exact samples, short/EAGAIN");
}

static void test_nonblocking_drm_read_dispatch(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = task->tgid = 1400;
    sched_task_limits(task)->nofile.rlim_cur = 64;
    struct task_file *file;
    int fd = task_allocate_fd(task, 3, &file);
    assert(fd == 3);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_RDONLY |
                  RELIEFOS_O_NONBLOCK;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = STORAGE_DEV_KIND_DRM_CARD0};
    file->drm_file = (struct drm_file *)(uintptr_t)1;
    descriptor_dispatch_task = task;
    descriptor_dispatch_sleep_count = 0;
    descriptor_drm_read_count = 0;
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_READ, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_READV, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV2, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PWRITE64, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PWRITEV, fd));
    file->flags &= ~RELIEFOS_O_NONBLOCK;
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV, fd));
    file->flags |= RELIEFOS_O_NONBLOCK;
    file->drm_file = NULL;
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    file->drm_file = (struct drm_file *)(uintptr_t)1;
    char byte;
    struct trap_frame frame = {.rax = LINUX_SYS_READ, .rdi = (uint64_t)fd,
        .rsi = (uintptr_t)&byte, .rdx = 1, .rip = 0x4002, .rsp = 0x8000};

    syscall_dispatch_frame(&frame);

    assert(descriptor_drm_read_count == 1);
    assert((int64_t)frame.rax == -RELIEFOS_EAGAIN);
    assert(frame.rip == 0x4002);
    assert(descriptor_dispatch_sleep_count == 0);
    assert(task->syscall_file == NULL);

    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    frame = (struct trap_frame){.rax = LINUX_SYS_READV, .rdi = (uint64_t)fd,
        .rsi = (uintptr_t)&vector, .rdx = 1, .rip = 0x5002, .rsp = 0x8000};
    syscall_dispatch_frame(&frame);
    assert(descriptor_drm_read_count == 2);
    assert((int64_t)frame.rax == -RELIEFOS_EAGAIN);
    assert(frame.rip == 0x5002);
    assert(descriptor_dispatch_sleep_count == 0);
    assert(task->syscall_file == NULL);

    assert(syscall_drm_read(task, file, (uintptr_t)&byte, 1) == -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 3);
    assert(syscall_drm_pread(task, file, (uintptr_t)&byte, 1, 4096) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 4);
    assert(file->offset == 0);
    assert(syscall_drm_pread(task, file, (uintptr_t)&byte, 1, UINT64_MAX) ==
           -RELIEFOS_EINVAL);
    assert(descriptor_drm_read_count == 4);
    assert(syscall_drm_readv(task, file, &vector, 1, false, 0, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 5);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 6);
    assert(file->offset == 0);
    assert(syscall_drm_readv(task, file, &vector, 1, false, UINT64_MAX, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 7);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 1) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 8);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 2) ==
           -RELIEFOS_EOPNOTSUPP);
    assert(syscall_drm_readv(task, file, &vector, 0, true, UINT64_MAX, 0) ==
           -RELIEFOS_EINVAL);
    assert(syscall_drm_readv(task, file, &vector, 0, false, UINT64_MAX, 1) == 0);
    assert(file->offset == 0);
    assert(descriptor_drm_read_count == 8);

    file->offset = 37;
    for (uint64_t whence = 0; whence <= 4; ++whence)
        assert(syscall_drm_llseek(file, whence) == 37);
    assert(syscall_drm_llseek(file, 5) == -RELIEFOS_EINVAL);
    assert(syscall_drm_llseek(file, UINT32_MAX) == -RELIEFOS_EINVAL);
    assert(file->offset == 37);
    task_discard_file_fd(task, fd);
    descriptor_dispatch_task = NULL;
    free(task);
    puts("PASS production syscall frame returns nonblocking DRM read/readv EAGAIN without retry sleep; positional DRM EAGAIN classification is scoped by syscall number");
}

/* mode 0 imports then dup/closes; modes 1/2 truncate by control capacity/fd
 * limit; mode 3 exits with queued rights. Each checks the actual last release. */
static void test_audio_scm_rights(int control, unsigned mode)
{
    struct audio_test_card card = {0};
    if (control) {
        struct audio_card_ops ops = audio_test_ops;
        ops.control_count = descriptor_control_count;
        ops.control_info = descriptor_control_info;
        ops.control_read = descriptor_control_read;
        ops.control_write = descriptor_control_write;
        struct audio_card_identity identity = {.id = "scm", .name = "scm-test"};
        assert(audio_register_card_owned(&identity, &ops, &card, 0x7b, &card.id) == 0);
    } else audio_test_card_init(&card);
    struct task *sender = descriptor_audio_task(1200), *receiver = descriptor_audio_task(1201);
    struct task *observer = control ? descriptor_audio_task(1202) : NULL;
    int audio_fd = descriptor_audio_open(sender, card.id, control);
    struct task_file *independent = NULL;
    struct snd_ctl_elem_id id = {.numid = 1, .iface = SNDRV_CTL_ELEM_IFACE_MIXER};
    struct snd_ctl_elem_value value = {.id = id, .value.integer.value = {30}};
    if (control) {
        independent = task_file_for_fd(observer, descriptor_audio_open(observer, card.id, 1));
        assert(audio_control_ioctl(sender, task_file_for_fd(sender, audio_fd),
                                    SNDRV_CTL_IOCTL_ELEM_LOCK, (uintptr_t)&id) == 0);
        int subscribe = 1;
        assert(audio_alsa_control_ioctl(task_file_for_fd(sender, audio_fd)->audio_control_file,
            SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) == 0);
    }
    int send_fd, receive_fd;
    descriptor_socket_pair(sender, receiver, &send_fd, &receive_fd);
    union { struct cmsghdr alignment; uint8_t bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct cmsghdr *header = (struct cmsghdr *)ancillary.bytes;
    *header = (struct cmsghdr){.cmsg_len = CMSG_LEN(sizeof(int)),
        .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS};
    memcpy(ancillary.bytes + CMSG_LEN(0), &audio_fd, sizeof(audio_fd));
    uint8_t byte = 0x65;
    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct socket_message_result result = {0};
    descriptor_socket_task = sender;
    fail_allocation = 1;
    assert(task_socket_message(sender, task_file_for_fd(sender, send_fd), (uintptr_t)&message,
        MSG_DONTWAIT | MSG_NOSIGNAL, false, false, &result) == -ENOMEM);
    fail_allocation = 0;
    assert(!task_file_for_fd(sender, audio_fd)->description);
    assert(task_socket_message(sender, task_file_for_fd(sender, send_fd), (uintptr_t)&message,
        MSG_DONTWAIT | MSG_NOSIGNAL, false, false, &result) == 1);
    struct task_file *held = task_file_for_fd(sender, audio_fd);
    assert(held->references == 2 && held->scm_references == 1);
    clear_task_file(task_descriptor_for_fd(sender, audio_fd));
    assert(held->references == 1 && held->scm_references == 1);
    if (control) {
        assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
        audio_control_changed(card.id, 0);
    } else assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    descriptor_socket_task = receiver;
    if (mode == 3) {
        clear_task_files(receiver);
    } else {
        memset(&ancillary, 0, sizeof(ancillary));
        byte = 0;
        message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1,
            .msg_control = ancillary.bytes, .msg_controllen = mode == 1 ? 0 : sizeof(ancillary.bytes)};
        if (mode == 2) receiver->limits.nofile.rlim_cur = 4;
        assert(task_socket_message(receiver, task_file_for_fd(receiver, receive_fd), (uintptr_t)&message,
            MSG_DONTWAIT | MSG_CMSG_CLOEXEC, true, false, &result) == 1);
        assert(byte == 0x65);
        if (mode) {
            assert(message.msg_flags & MSG_CTRUNC);
            assert(message.msg_controllen == 0 && !task_file_for_fd(receiver, 4));
        } else {
            assert(!(message.msg_flags & MSG_CTRUNC));
            assert(header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
            int received;
            memcpy(&received, ancillary.bytes + CMSG_LEN(0), sizeof(received));
            assert(received == 4 && task_file_for_fd(receiver, received) == held);
            assert(held->references == 1 && !held->scm_references);
            assert(task_fd_descriptor_flags(receiver, received) == RELIEFOS_FD_CLOEXEC);
            assert(task_duplicate_file_fd(receiver, received, 0, 0) == 5);
            if (control) {
                struct snd_ctl_event event;
                assert(audio_control_read_file(held->audio_control_file, &event, sizeof(event)) == sizeof(event));
                assert(event.data.elem.id.numid == 1 && event.data.elem.mask == 1);
                assert(audio_alsa_control_ioctl(held->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
            } else {
                uint8_t samples[4] = {0};
                assert(audio_device_write(NULL, held, samples, sizeof(samples)) == sizeof(samples));
            }
            clear_task_file(task_descriptor_for_fd(receiver, received));
            assert(held->references == 1);
            if (control)
                assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
            else assert(card.close_count == 0);
            clear_task_file(task_descriptor_for_fd(receiver, 5));
        }
    }
    if (control) {
        assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
        clear_task_files(observer); sched_task_file_release(observer); free(observer);
    } else assert(card.close_count == 1);
    clear_task_files(sender); clear_task_files(receiver);
    sched_task_file_release(sender); sched_task_file_release(receiver);
    free(sender); free(receiver);
    descriptor_socket_task = NULL;
    assert(audio_unregister_card(card.id, 0) == 0);
    for (unsigned i = 0; i < UNIX_SOCKET_MAX; ++i) assert(!unix_sockets[i]);
    printf("PASS actual SCM_RIGHTS %s: %s\n", control ? "control" : "PCM",
        mode == 0 ? "queue survives sender close, CLOEXEC import, shared state, final dup close" :
        mode == 1 ? "missing control buffer discards queued reference" :
        mode == 2 ? "RLIMIT import rollback discards queued reference" : "receiver exit drains queued reference");
}

static void test_flock_promotion(void)
{
    for (unsigned type = RELIEFOS_FLOCK_SH; type <= RELIEFOS_FLOCK_EX; ++type) {
        struct task_file source = {.used = 1, .flock_type = type};
        struct task_file duplicate = {0};
        struct reliefos_flock_entry *entry = &reliefos_flocks[0];
        *entry = (struct reliefos_flock_entry){.used = 1, .type = type};
        kernel_wait_queue_init(&entry->waiters);
        if (type == RELIEFOS_FLOCK_EX) entry->owner = &source;
        else { entry->shared_count = 1; entry->shared_owners[0] = &source; }
        fail_allocation = 1;
        assert(task_file_reference(&duplicate, &source) == -RELIEFOS_ENOMEM);
        assert(!source.description && !duplicate.used);
        assert((type == RELIEFOS_FLOCK_EX ? entry->owner : entry->shared_owners[0]) == &source);
        fail_allocation = 0;
        assert(task_file_reference(&duplicate, &source) == 0);
        assert((type == RELIEFOS_FLOCK_EX ? entry->owner : entry->shared_owners[0]) == source.description);
        clear_task_file(&source);
        assert(entry->used);
        clear_task_file(&duplicate);
        assert(!entry->used);
    }
    puts("PASS flock owner promotion: exclusive/shared, ENOMEM rollback and final close");
}

static void test_pty_descriptions(void)
{
    struct task *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
    assert(parent && child);
    parent->limits.nofile.rlim_cur = child->limits.nofile.rlim_cur = 1024;
    fail_allocation = 1;
    assert(task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR) == -RELIEFOS_ENOMEM);
    assert(!pty_references && !task_pty_fd_for_fd(parent, 3));
    fail_allocation = 0;
    int fd = task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR | RELIEFOS_O_CLOEXEC);
    assert(fd == 3 && pty_references == 1);
    struct task_pty_fd *source = task_pty_fd_for_fd(parent, fd);
    int duplicate = task_pty_duplicate_fd(parent, fd, 0, 0);
    assert(duplicate == 4 && source->description->references == 2 && pty_references == 1);
    source->description->status_flags |= RELIEFOS_O_NONBLOCK;
    assert(task_pty_status(task_pty_fd_for_fd(parent, duplicate)) & RELIEFOS_O_NONBLOCK);
    assert(task_fd_descriptor_flags(parent, fd) == RELIEFOS_FD_CLOEXEC &&
           task_fd_descriptor_flags(parent, duplicate) == 0);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(source->description->references == 4);
    struct task_pty_fd queued;
    assert(task_pty_export_fd(parent, fd, &queued) == 0 && source->description->references == 5);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(parent, duplicate));
    task_pty_release_entry(task_pty_fd_for_fd(child, fd));
    task_pty_release_entry(task_pty_fd_for_fd(child, duplicate));
    assert(queued.description->references == 1 && pty_references == 1);
    fd = task_pty_import_fd(parent, &queued, RELIEFOS_FD_CLOEXEC);
    assert(fd == 3 && queued.description->references == 2);
    task_pty_release_entry(&queued);
    source = task_pty_fd_for_fd(parent, fd);
    parent->syscall_pty = *source;
    ++source->description->references;
    parent->syscall_fd = fd;
    assert(task_pty_dup2_fd(parent, fd, 1) == 1 && parent->pty_id == 0);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(parent, 1));
    assert(parent->syscall_pty.description->references == 1 && pty_references == 1);
    assert(task_pty_for_io(parent, fd) == &parent->syscall_pty);
    task_release_syscall_file(parent);
    assert(!pty_references && !parent->syscall_pty.used);
    fd = task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR);
    assert(fd >= 0);
    struct task_fd_table_state *shared = kernel_malloc(sizeof(*shared));
    assert(shared);
    clear_task_files(child);
    *shared = parent->fd_table;
    shared->references = 2;
    parent->shared_files = child->shared_files = shared;
    fail_allocation = 1;
    assert(syscall_unshare_task_files(parent) == -RELIEFOS_ENOMEM);
    assert(parent->shared_files == shared && shared->references == 2);
    fail_allocation = 0;
    assert(syscall_unshare_task_files(parent) == 0);
    source = task_pty_fd_for_fd(parent, fd);
    assert(parent->shared_files != shared && shared->references == 1);
    assert(source != task_pty_fd_for_fd(child, fd) && source->description->references == 2);
    source->description->status_flags |= RELIEFOS_O_APPEND;
    assert(task_pty_status(task_pty_fd_for_fd(child, fd)) & RELIEFOS_O_APPEND);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(child, fd));
    assert(!pty_references);
    clear_task_files(parent);
    clear_task_files(child);
    kernel_free(parent->shared_files);
    kernel_free(shared);
    free(parent);
    free(child);
}

static void test_console_descriptions(void)
{
    struct task *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
    assert(parent && child);
    parent->limits.nofile.rlim_cur = child->limits.nofile.rlim_cur = 64;
    assert(task_pty_materialize_stdio(parent) == 0);
    assert(task_file_for_fd(parent, 0) && task_file_for_fd(parent, 1) && task_file_for_fd(parent, 2));
    assert(task_device_is(task_file_for_fd(parent, 0), STORAGE_DEV_KIND_NULL));
    assert(task_device_is(task_file_for_fd(parent, 1), STORAGE_DEV_KIND_KMSG));
    assert(task_fd_set_descriptor_flags(parent, 2, RELIEFOS_FD_CLOEXEC) == 0);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(task_file_for_fd(parent, 1) == task_file_for_fd(child, 1));
    int copy = task_duplicate_file_fd(child, 1, 0, 0);
    assert(copy == 3);
    task_file_for_fd(child, copy)->flags |= RELIEFOS_O_NONBLOCK;
    assert(task_file_for_fd(parent, 1)->flags & RELIEFOS_O_NONBLOCK);
    assert(task_fd_descriptor_flags(child, 2) == RELIEFOS_FD_CLOEXEC);
    task_discard_file_fd(child, 1);
    assert(task_pty_materialize_stdio(child) == 0 && !task_file_for_fd(child, 1));
    assert(task_file_for_fd(parent, 1));
    clear_task_files(child);
    clear_task_files(parent);
    free(child);
    free(parent);
}

int main(void)
{
    test_nonblocking_drm_read_dispatch();
    test_audio_write_batch();
    kernel_objects_init();
    struct audio_test_card stat_cards[2];
    audio_test_card_init(&stat_cards[0]);
    audio_test_card_init(&stat_cards[1]);
    struct storage_node sound_node = {.flags = STORAGE_NODE_FLAG_AUDIO_PCM,
        .volume_id = AUDIO_DEVICE_VOLUME_ID(stat_cards[1].id, 7, AUDIO_CAPTURE)};
    assert(linux_audio_rdev(&sound_node) == ((116u << 8) | 63u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL;
    sound_node.volume_id = stat_cards[1].id;
    assert(linux_audio_rdev(&sound_node) == ((116u << 8) | 32u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_MIXER;
    assert(linux_audio_rdev(&sound_node) == ((14u << 8) | 16u));
    sound_node.flags = STORAGE_NODE_FLAG_DEV_NODE;
    sound_node.first_cluster = STORAGE_DEV_KIND_AUDIO;
    assert(linux_audio_rdev(&sound_node) == ((14u << 8) | 3u));
    assert(!audio_unregister_card(stat_cards[0].id, 0));
    assert(!audio_unregister_card(stat_cards[1].id, 0));
    struct task_file pipe = {.flags = TASK_FILE_FLAG_PIPE};
    struct linux_stat_abi pipe_stat = {.st_mode = 0020660, .st_rdev = 1};
    linux_stat_fd_type(&pipe_stat, &pipe);
    assert(pipe_stat.st_mode == (LINUX_S_IFIFO | 0600) && pipe_stat.st_rdev == 0);
    pipe.flags |= TASK_FILE_FLAG_PIPE_WRITE;
    pipe_stat.st_mode = 0100644;
    linux_stat_fd_type(&pipe_stat, &pipe);
    assert(pipe_stat.st_mode == (LINUX_S_IFIFO | 0600));
    struct task_file device = {.flags = TASK_FILE_FLAG_DEV_NULL};
    struct linux_stat_abi device_stat = {.st_mode = 0020660, .st_rdev = 1};
    linux_stat_fd_type(&device_stat, &device);
    linux_stat_fd_type(&device_stat, NULL);
    assert(device_stat.st_mode == 0020660 && device_stat.st_rdev == 1);
    puts("PASS stat pipe endpoints report FIFO; character devices remain unchanged");
    test_flock_promotion();
    test_pty_descriptions();
    test_console_descriptions();
    test_audio_control_descriptions();
    test_audio_pcm_descriptions();
    test_audio_oss_descriptions();
    for (unsigned mode = 0; mode < 4; ++mode) {
        test_audio_scm_rights(1, mode);
        test_audio_scm_rights(0, mode);
    }
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    sched_task_limits(task)->nofile.rlim_cur = 1024;
    struct task_file *slot;
    assert(task_allocate_fd(task, 0, &slot) == 3);
    slot->flags = TASK_FILE_FLAG_PIPE;
    struct task_file *resolved;
    char path[RELIEFOS_FS_PATH_LEN];
    assert(resolve_kernel_path_at_flags(task, 3, "", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == 0);
    assert(resolved == slot && !path[0]);
    assert(resolve_kernel_path_at_flags(task, 3, "entry", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_ENOTDIR);
    assert(resolve_kernel_path_at_flags(task, 3, "", false, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_ENOENT);
    assert(resolve_kernel_path_at_flags(task, -1, "", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_EBADF);
    slot->flags = 0;
    puts("PASS AT_EMPTY_PATH anonymous descriptor resolution and invalid-path errors");
    slot->offset = 123;
    slot->kind = TASK_FILE_KIND_SIGNALFD;
    slot->aux = 1ULL << 35;
    assert(task_duplicate_file_fd(task, 3, 100, 1) == 100);
    assert(task_dup2_fd(task, 100, 700) == 700);
    assert(task_file_for_fd(task, 100) == task_file_for_fd(task, 700));
    assert(task_file_for_fd(task, 700)->offset == 123);
    assert(task_file_for_fd(task, 700)->kind == TASK_FILE_KIND_SIGNALFD);
    task_file_for_fd(task, 100)->aux = 1ULL << 34;
    assert(task_file_for_fd(task, 700)->aux == (1ULL << 34));
    assert(task_descriptor_for_fd(task, 100)->fd_flags == 1);
    assert(task_descriptor_for_fd(task, 700)->fd_flags == 0);
    assert(task_file_for_fd(task, 3)->references == 3);
    /* Reject failed expansion without consuming an OFD reference or fd. */
    fail_allocation = 1;
    assert(task_dup2_fd(task, 100, 900) == -RELIEFOS_ENOMEM);
    assert(task_file_for_fd(task, 900) == NULL);
    assert(task_file_for_fd(task, 3)->references == 3);
    fail_allocation = 0;
    assert(task_dup2_fd(task, 100, 900) == 900);
    assert(task_dup2_fd(task, 999, 900) == -RELIEFOS_EBADF);
    assert(task_file_for_fd(task, 3)->references == 4);
    struct task waiter = {.pid = 777, .state = TASK_BLOCKED};
    reliefos_flocks[0] = (struct reliefos_flock_entry){.used = 1,
        .type = RELIEFOS_FLOCK_EX, .owner = task_file_for_fd(task, 3)};
    kernel_wait_queue_init(&reliefos_flocks[0].waiters);
    assert(kernel_wait_queue_add(&reliefos_flocks[0].waiters, &waiter) == 0);
    sched_task_limits(task)->nofile.rlim_cur = 4;
    assert(task_allocate_fd(task, 0, &slot) == -RELIEFOS_EMFILE);
    assert(task_dup2_fd(task, 900, 900) == 900);
    task_discard_file_fd(task, 0);
    assert(task_allocate_fd(task, 0, &slot) == 0);
    task_discard_file_fd(task, 0);
    assert(task_allocate_fd(task, 0, &slot) == 0);
    task_discard_file_fd(task, 0);
    for (int fd = 3; fd < 1024; ++fd) {
        task_discard_file_fd(task, fd);
        if (fd < 900) assert(reliefos_flocks[0].used && waiter.waiting_queue);
    }
    assert(!reliefos_flocks[0].used && !waiter.waiting_queue && !reliefos_flocks[0].waiters.count);
    assert(task_allocate_fd(task, 0, &slot) == 0 && !slot->kind);
    task_discard_file_fd(task, 0);
    task->signalfd_vectors = kernel_malloc(9 * sizeof(struct iovec));
    assert(task->signalfd_vectors);
    task->signalfd_waiting = true;
    task->signalfd_wait_mask = UINT64_MAX;
    task_release_syscall_file(task);
    assert(!task->signalfd_vectors && !task->signalfd_waiting && !task->signalfd_wait_mask);
    sched_task_file_release(task);
    free(task);
    puts("PASS actual fd table: growth, shared OFD, failure rollback, stdio reuse, limit lowering");
}
