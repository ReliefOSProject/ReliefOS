#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/net/socket/udp.c"

static struct task current;
static struct task_file files[80];
static unsigned allocations;
static uint64_t ticks = 100;
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t bytes) { ++allocations; return calloc(1, bytes); }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
uint64_t time_ticks(void) { return ticks; }
void sched_sleep_current_until(uint64_t tick) { assert(tick == ticks + 1); }
bool user_range_ok(uint64_t address, uint64_t size) { (void)size; return address > 4096; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    (void)task;
    for (unsigned i = minimum; i < 80; ++i) if (!files[i].used) {
        files[i].used = 1; *out = &files[i]; return (int)i;
    }
    return -LINUX_EMFILE;
}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd >= 0 && fd < 80 && files[fd].used ? &files[fd] : NULL; }
int net_get_config(struct reliefos_net_config *config)
{ *config = (struct reliefos_net_config){.local_ip = 0x0a00020f}; return 0; }
void net_poll_packets(void) {}
int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length)
{ (void)source; (void)destination; (void)source_port; (void)destination_port; (void)data; (void)length; return -LINUX_ENETDOWN; }

static long call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ return syscall_udp(nr, a, b, c, d, e, 0); }
static int open_socket(void)
{ return (int)call(__NR_socket, AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, 0, 0); }

int main(void)
{
    current.cap_effective = 1ULL << CAP_NET_BIND_SERVICE;
    int a = open_socket(), b = open_socket();
    assert(a >= 0 && b >= 0 && a != b);
    assert(files[a].fd_flags == 1 && (files[a].flags & RELIEFOS_O_NONBLOCK));
    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = swap16(55000),
                               .sin_addr = {.s_addr = swap32(0x7f000001)}};
    assert(call(__NR_bind, b, (uintptr_t)&dest, sizeof(dest), 0, 0) == 0);
    assert(call(__NR_bind, a, (uintptr_t)&dest, sizeof(dest), 0, 0) == -LINUX_EADDRINUSE);
    assert(call(__NR_connect, a, (uintptr_t)&dest, sizeof(dest), 0, 0) == 0);
    char text[16] = {0};
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    assert(task_udp_send(&files[a], "hello", 5, 0, 0, 0) == 5);
    assert(task_udp_poll(&files[b], POLLIN | POLLOUT) == (POLLIN | POLLOUT));
    assert(task_udp_recv(&files[b], text, 2, MSG_PEEK | MSG_TRUNC, 0, 0) == 5);
    assert(!memcmp(text, "he", 2));
    struct sockaddr_in source;
    uint32_t size = sizeof(source);
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, (uintptr_t)&source, (uintptr_t)&size) == 5);
    assert(source.sin_family == AF_INET && source.sin_addr.s_addr == swap32(0x7f000001));
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    assert(task_udp_send(&files[a], "", 0, 0, 0, 0) == 0);
    assert(task_udp_poll(&files[b], POLLIN) == POLLIN);
    assert(task_udp_recv(&files[b], text, 0, 0, 0, 0) == 0);
    assert(task_udp_poll(&files[b], POLLIN) == 0);
    int64_t timeout[2] = {0, 1000};
    assert(call(__NR_setsockopt, b, SOL_SOCKET, SO_RCVTIMEO, (uintptr_t)timeout, sizeof(timeout)) == 0);
    size = sizeof(timeout);
    assert(call(__NR_getsockopt, b, SOL_SOCKET, SO_RCVTIMEO, (uintptr_t)timeout, (uintptr_t)&size) == 0);
    assert(timeout[0] == 0 && timeout[1] == 1000000 / RELIEFNT_TICK_HZ);
    files[b].flags &= ~RELIEFOS_O_NONBLOCK;
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    ++ticks;
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    files[b].flags |= RELIEFOS_O_NONBLOCK;
    assert(call(__NR_setsockopt, b, SOL_SOCKET, SO_SNDTIMEO, (uintptr_t)timeout, sizeof(timeout)) == -LINUX_ENOPROTOOPT);
    assert(call(__NR_getsockopt, b, SOL_SOCKET, SO_SNDTIMEO, (uintptr_t)timeout, (uintptr_t)&size) == -LINUX_ENOPROTOOPT);
    int value = -1;
    size = 4;
    assert(call(__NR_getsockopt, a, SOL_SOCKET, SO_TYPE, (uintptr_t)&value, (uintptr_t)&size) == 0);
    assert(value == SOCK_DGRAM && size == 4);
    assert(call(__NR_getsockopt, a, SOL_SOCKET, 99999, (uintptr_t)&value, (uintptr_t)&size) == -LINUX_ENOPROTOOPT);
    assert(call(__NR_bind, a, 1, 16, 0, 0) == -LINUX_EFAULT);
    assert(task_udp_send(&files[a], text, 1473, 0, 0, 0) == -LINUX_EMSGSIZE);
    for (unsigned i = 0; i < 100; ++i) assert(task_udp_send(&files[a], "x", 1, 0, 0, 0) == 1);
    assert(udp_from_file(&files[b])->queued == UDP_QUEUE_LENGTH);
    task_udp_release(&files[a]); task_udp_release(&files[b]);
    assert(!allocations);
    puts("PASS UDP bind, connect, datagram boundaries, source address, truncation/peek, zero length, options and cleanup");
}
