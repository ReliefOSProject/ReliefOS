#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_process.c"

static struct task caller, target;
static uint64_t readonly;
struct task *sched_current_task(void) { return &caller; }
struct task *sched_find(uint32_t pid) { return pid == 17 ? &target : NULL; }
uint64_t sched_user_task_count(uint32_t uid) { (void)uid; return 0; }
bool user_range_ok(uint64_t p, uint64_t n) { return p >= 4096 && n <= UINT64_MAX - p; }
bool user_range_writable(uint64_t p, uint64_t n) { return p != readonly && user_range_ok(p, n); }
/* Standard ID syscalls must not consult a userspace service or mutate peers. */
int storage_read_file(const char *p, const void **d, size_t *n)
{ (void)p; (void)d; (void)n; assert(0); return -1; }
void sched_set_task_identity(uint32_t p, const struct reliefos_user_info *u, uint32_t s)
{ (void)p; (void)u; (void)s; assert(0); }
void sched_set_session_identity(uint32_t p, const struct reliefos_user_info *u, uint32_t s)
{ (void)p; (void)u; (void)s; assert(0); }
uint32_t sched_next_session_id(void) { assert(0); return 0; }
void mm_free_pages(uint64_t p, uint32_t n) { (void)p; (void)n; assert(0); }
void power_reboot(void) { assert(0); __builtin_unreachable(); }
void power_shutdown(void) { assert(0); __builtin_unreachable(); }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}

int main(void)
{
    caller = (struct task){.uid = 0, .euid = 0, .cap_effective = 0};
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0, 0, 0, 0) == -LINUX_EPERM);
    caller = (struct task){.uid = 1000, .euid = 1000, .cap_effective = 1ULL << CAP_SYS_BOOT};
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0, 0, 0, 0) == -LINUX_EINVAL);
    caller = (struct task){0};
    assert(syscall_process_prctl(LINUX_PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0);
    assert(syscall_process_prctl(LINUX_PR_SET_NO_NEW_PRIVS, 1, 0, 0, 1) == -LINUX_EINVAL);
    assert(!caller.no_new_privs);
    assert(syscall_process_prctl(LINUX_PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(syscall_process_prctl(LINUX_PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    assert(syscall_process_prctl(LINUX_PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) == -LINUX_EINVAL);
    assert(caller.no_new_privs);
    caller.uid = 100;
    caller.euid = caller.fsuid = 101;
    caller.suid = 102;
    assert(syscall_process_control(LINUX_SYS_SETREUID, UINT32_MAX, 101, 0, 0) == 0);
    assert(caller.uid == 100 && caller.euid == 101 && caller.suid == 101);
    caller.gid = 200;
    caller.egid = caller.fsgid = 201;
    caller.sgid = 202;
    assert(syscall_process_control(LINUX_SYS_SETREGID, UINT32_MAX, 201, 0, 0) == 0);
    assert(caller.gid == 200 && caller.egid == 201 && caller.sgid == 201);
    caller.euid = 0;
    assert(syscall_process_control(LINUX_SYS_SETFSUID, UINT32_MAX, 0, 0, 0) == 101);
    assert(caller.fsuid == 101);
    assert(syscall_process_control(LINUX_SYS_SETFSGID, UINT32_MAX, 0, 0, 0) == 201);
    assert(caller.fsgid == 201);
    caller = (struct task){.uid = 100, .euid = 0, .suid = 0,
        .gid = 300, .egid = 301, .sgid = 302, .fsgid = 303,
        .cap_effective = 1u << CAP_SETUID, .cap_permitted = 1u << CAP_SETUID};
    assert(syscall_process_control(LINUX_SYS_SETUID, 400, 0, 0, 0) == 0);
    assert(caller.uid == 400 && caller.euid == 400 && caller.suid == 400 && caller.fsuid == 400);
    assert(caller.gid == 300 && caller.egid == 301 && caller.sgid == 302 && caller.fsgid == 303);
    assert(!caller.cap_effective && !caller.cap_permitted);
    assert(syscall_process_control(LINUX_SYS_SETUID, 0, 0, 0, 0) == -LINUX_EPERM);
    assert(syscall_process_control(LINUX_SYS_SETUID, UINT32_MAX, 0, 0, 0) == -LINUX_EINVAL);
    caller = (struct task){.uid = 0, .euid = 100, .suid = 200, .fsuid = 100};
    assert(syscall_process_control(LINUX_SYS_SETUID, 300, 0, 0, 0) == -LINUX_EPERM);
    assert(syscall_process_control(LINUX_SYS_SETUID, 200, 0, 0, 0) == 0);
    assert(caller.uid == 0 && caller.euid == 200 && caller.suid == 200);
    caller = (struct task){.uid = 100, .euid = 100, .suid = 100,
        .gid = 200, .egid = 201, .sgid = 202, .cap_effective = 1u << CAP_SETGID};
    assert(syscall_process_control(LINUX_SYS_SETGID, 400, 0, 0, 0) == 0);
    assert(caller.gid == 400 && caller.egid == 400 && caller.sgid == 400 && caller.fsgid == 400);
    caller.cap_effective = 0;
    caller.euid = 0;
    assert(syscall_process_control(LINUX_SYS_SETGID, 300, 0, 0, 0) == -LINUX_EPERM);
    caller = (struct task){.uid = 100, .euid = 100, .suid = 0, .fsuid = 101,
        .cap_permitted = 1u << CAP_KILL};
    assert(syscall_process_control(LINUX_SYS_SETRESUID, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0) == 0);
    assert(caller.fsuid == 101);
    assert(syscall_process_control(LINUX_SYS_SETRESUID, UINT32_MAX, 0, UINT32_MAX, 0) == 0);
    assert(caller.cap_effective == (1u << CAP_KILL));
    assert(syscall_process_control(LINUX_SYS_SETRESUID, 100, 100, 100, 0) == 0);
    assert(!caller.cap_effective && !caller.cap_permitted);
    caller = (struct task){0};
    caller.pid = 13;
    caller.tgid = 12;
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2] = {{0}};
    const uint32_t kill = 1u << CAP_KILL;
    caller.cap_permitted = caller.cap_effective = kill;
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == 0);
    data[0].permitted = data[0].effective = kill;
    /* Root cannot restore capabilities removed from its permitted set. */
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == -LINUX_EPERM);
    assert(!caller.cap_permitted && !caller.cap_effective);
    data[0].permitted = 0;
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == -LINUX_EPERM);
    assert(!caller.cap_effective);
    data[0].effective = 0;
    readonly = (uintptr_t)data;
    header.pid = caller.pid;
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == 0);
    readonly = 0;
    header.pid = caller.tgid;
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == -LINUX_EPERM);
    header.version = 0;
    assert(syscall_process_control(LINUX_SYS_CAPGET, (uintptr_t)&header, 0, 0, 0) == 0);
    assert(header.version == _LINUX_CAPABILITY_VERSION_3);
    header.pid = 17;
    target.cap_effective = target.cap_permitted = kill;
    assert(syscall_process_control(LINUX_SYS_CAPGET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == 0 && data[0].effective == kill);
    header.pid = -1;
    assert(syscall_process_control(LINUX_SYS_CAPGET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == -LINUX_EINVAL);
    header.pid = 18;
    assert(syscall_process_control(LINUX_SYS_CAPGET, (uintptr_t)&header,
        (uintptr_t)data, 0, 0) == -LINUX_ESRCH);
    _Alignas(8) unsigned char unaligned_header[sizeof(header) + 1];
    _Alignas(8) unsigned char unaligned_data[sizeof(data) + 1];
    header.pid = 0;
    __builtin_memcpy(unaligned_header + 1, &header, sizeof(header));
    assert(syscall_process_control(LINUX_SYS_CAPGET, (uintptr_t)(unaligned_header + 1),
        (uintptr_t)(unaligned_data + 1), 0, 0) == 0);
    assert(syscall_process_control(LINUX_SYS_CAPSET, (uintptr_t)(unaligned_header + 1),
        (uintptr_t)(unaligned_data + 1), 0, 0) == 0);
    puts("PASS production capget/capset: root drop, subsets, read-only input, thread ID, version query, target lookup");
}
