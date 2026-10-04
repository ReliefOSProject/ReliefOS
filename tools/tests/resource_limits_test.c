#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_process.c"

static struct task caller, target;
static uint64_t readonly;
struct task *sched_current_task(void) { return &caller; }
struct task *sched_find(uint32_t pid) { return pid == 7 ? &target : NULL; }
bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && address + length >= address; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_range_ok(address, length) && address != readonly; }

int main(void)
{
    struct task_address_space_state shared = {0};
    caller.shared_mm = &shared;
    caller.euid = caller.fsuid = 1000;
    caller.egid = caller.fsgid = 100;
    caller.cap_permitted = 3;
    task_credentials_prepare(&caller, 1000, 100, 1000, 100, 1);
    assert(!shared.nondumpable);
    task_credentials_prepare(&caller, 1000, 100, 1001, 100, 3);
    assert(shared.nondumpable && !caller.address_space.nondumpable);
    shared.nondumpable = false;
    task_credentials_prepare(&caller, 1000, 100, 1000, 100, 7);
    assert(shared.nondumpable);
    caller = (struct task){0};
    struct linux_rlimit64 core = {0, 0}, core_old;
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core_old, 0, 0) == 0);
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core, 0, 0) == 0);
    core.rlim_max = 1;
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core, 0, 0) == -LINUX_EPERM);
    caller.cap_effective = 1ULL << CAP_SYS_RESOURCE;
    core = (struct linux_rlimit64){4096, LINUX_RLIM_INFINITY};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core, 0, 0) == 0);
    target = caller;
    target.shared_limits = &caller.limits;
    core = (struct linux_rlimit64){2048, 4096};
    readonly = (uintptr_t)&core_old;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, LINUX_RLIMIT_CORE,
        (uintptr_t)&core, readonly) == -LINUX_EFAULT);
    readonly = 0;
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core_old, 0, 0) == 0);
    assert(core_old.rlim_cur == 2048 && core_old.rlim_max == 4096);
    core = (struct linux_rlimit64){4097, 4096};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_CORE,
        (uintptr_t)&core, 0, 0) == -LINUX_EINVAL);
    caller.cap_effective = 0;
    struct linux_rlimit64 initial_nproc;
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, LINUX_RLIMIT_NPROC,
        (uintptr_t)&initial_nproc, 0, 0) == 0);
    caller.limits.nproc = (struct linux_rlimit64){2, 4};
    initial_nproc = (struct linux_rlimit64){1, 4};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_NPROC,
        (uintptr_t)&initial_nproc, 0, 0) == 0 && caller.limits.nproc.rlim_cur == 1);
    initial_nproc.rlim_max = 5;
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_NPROC,
        (uintptr_t)&initial_nproc, 0, 0) == -LINUX_EPERM);
    caller.limits.nofile = (struct linux_rlimit64){1024,1048576};
    target = caller;
    target.shared_limits = &caller.limits;
    struct linux_rlimit64 value = {128,256}, old;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, 7, (uintptr_t)&value, (uintptr_t)&old) == 0);
    assert(old.rlim_cur == 1024 && old.rlim_max == 1048576);
    assert(caller.limits.nofile.rlim_cur == 128 && caller.limits.nofile.rlim_max == 256);
    value = (struct linux_rlimit64){64,256};
    readonly = (uintptr_t)&old;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, 7, (uintptr_t)&value, readonly) == -LINUX_EFAULT);
    assert(caller.limits.nofile.rlim_cur == 64);
    readonly = 0;
    value = (struct linux_rlimit64){32,256};
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 0x100000007ULL, 0x100000007ULL,
                                  (uintptr_t)&value, (uintptr_t)&value) == 0);
    assert(value.rlim_cur == 64 && caller.limits.nofile.rlim_cur == 32);
    caller.uid = caller.euid = caller.suid = target.uid = target.euid = target.suid = 1000;
    caller.gid = caller.egid = caller.sgid = target.gid = target.egid = target.sgid = 2000;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, 7, 0, (uintptr_t)&old) == 0);
    ++target.sgid;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, 7, 0, (uintptr_t)&old) == -LINUX_EPERM);
    value = (struct linux_rlimit64){32,257};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, 7, (uintptr_t)&value, 0, 0) == -LINUX_EPERM);
    value = (struct linux_rlimit64){257,256};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, 7, (uintptr_t)&value, 0, 0) == -LINUX_EINVAL);
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, 16, 1, 0, 0) == -LINUX_EINVAL);
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, 16, 1, 0, 0) == -LINUX_EFAULT);
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, -1, 16, 0, 1) == -LINUX_ESRCH);
    --target.sgid;
    caller.limits.sigpending = (struct linux_rlimit64){128, 256};
    value = (struct linux_rlimit64){0, 256};
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, LINUX_RLIMIT_SIGPENDING,
        (uintptr_t)&value, (uintptr_t)&old) == 0);
    assert(old.rlim_cur == 128 && caller.limits.sigpending.rlim_cur == 0);
    value = (struct linux_rlimit64){128, 256};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_SIGPENDING,
        (uintptr_t)&value, 0, 0) == 0);
    value.rlim_max = 257;
    caller.euid = 0; /* A numeric root UID alone does not grant CAP_SYS_RESOURCE. */
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_SIGPENDING,
        (uintptr_t)&value, 0, 0) == -LINUX_EPERM);
    caller.cap_effective = 1ULL << CAP_SYS_RESOURCE;
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_SIGPENDING,
        (uintptr_t)&value, 0, 0) == 0);

    /* RLIMIT_STACK is a real process-wide limit, not a fixed query result. */
    caller.limits.stack = (struct linux_rlimit64){8192ULL * 1024ULL, LINUX_RLIM_INFINITY};
    value = (struct linux_rlimit64){4096ULL * 1024ULL, LINUX_RLIM_INFINITY};
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, LINUX_RLIMIT_STACK,
        (uintptr_t)&value, (uintptr_t)&old) == 0);
    assert(old.rlim_cur == 8192ULL * 1024ULL && old.rlim_max == LINUX_RLIM_INFINITY);
    assert(caller.limits.stack.rlim_cur == 4096ULL * 1024ULL);
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, LINUX_RLIMIT_STACK,
        (uintptr_t)&old, 0, 0) == 0 && old.rlim_cur == 4096ULL * 1024ULL);
    value = (struct linux_rlimit64){2048ULL * 1024ULL, 1024ULL * 1024ULL};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_STACK,
        (uintptr_t)&value, 0, 0) == -LINUX_EINVAL);
    caller.cap_effective = 0;
    value = (struct linux_rlimit64){1024ULL * 1024ULL, LINUX_RLIM_INFINITY};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_STACK,
        (uintptr_t)&value, 0, 0) == 0);
    value = (struct linux_rlimit64){512ULL * 1024ULL, 512ULL * 1024ULL};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_STACK,
        (uintptr_t)&value, 0, 0) == 0);
    caller.cap_effective = 1ULL << CAP_SYS_RESOURCE;
    value = (struct linux_rlimit64){768ULL * 1024ULL, 768ULL * 1024ULL};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_STACK,
        (uintptr_t)&value, 0, 0) == 0);
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, -1, LINUX_RLIMIT_STACK, 0, 1) == -LINUX_ESRCH);
    assert(process_resource_limit(LINUX_SYS_GETRLIMIT, -1, 1, 0, 0) == -LINUX_EINVAL);
    caller.cap_effective = 0;
    caller.limits.memlock = (struct linux_rlimit64){65536, 65536};
    value = (struct linux_rlimit64){4096, 65536};
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64, 7, LINUX_RLIMIT_MEMLOCK,
        (uintptr_t)&value, (uintptr_t)&old) == 0);
    assert(old.rlim_cur == 65536 && target.shared_limits->memlock.rlim_cur == 4096);
    value.rlim_max = 65537;
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_MEMLOCK,
        (uintptr_t)&value, 0, 0) == -LINUX_EPERM);
    value = (struct linux_rlimit64){0, 65536};
    assert(process_resource_limit(LINUX_SYS_SETRLIMIT, LINUX_RLIMIT_MEMLOCK,
        (uintptr_t)&value, 0, 0) == 0 && !target.shared_limits->memlock.rlim_cur);
    puts("PASS native rlimit helpers: shared state, hard limits, widths, ID checks, alias, stack/memlock and output-fault commit");
}
