#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/futex.h>

static struct task task;
static int64_t poll_result;
struct task *sched_current_task(void) { return &task; }
static bool user_range_ok(uint64_t address, uint64_t length)
{ return address && length; }
static int64_t syscall_select_common(uint64_t nfds, uint64_t rd, uint64_t wr,
                                    uint64_t ex, uint64_t timeout, bool pselect)
{
    (void)nfds;(void)rd;(void)wr;(void)ex;(void)timeout;
    assert(pselect && task.blocked_signals == 0);
    return poll_result;
}
static int64_t dispatch(uint64_t a5)
{
    uint64_t number = LINUX_SYS_PSELECT6, a0=0, a1=0, a2=0, a3=0, a4=0;
#include "pselect_dispatch_body.inc"
    return -RELIEFOS_ENOSYS;
}
int main(void)
{
    uint64_t empty=0, argument[2]={(uintptr_t)&empty,sizeof(empty)};
    const uint64_t original=(1ULL<<16)|(1ULL<<9);
    const int64_t parked[]={KERNEL_SYSCALL_BLOCKED,-RELIEFOS_EAGAIN};
    for (unsigned i=0;i<2;++i) {
        memset(&task,0,sizeof(task));task.blocked_signals=original;
        poll_result=parked[i];
        assert(dispatch((uintptr_t)argument)==poll_result);
        if (!task.sigsuspend_active || task.blocked_signals)
            fputs("FAIL: pselect restored SIGCHLD mask while still parked in the kernel\n",stderr);
        assert(task.sigsuspend_active && !task.blocked_signals);
        assert(task.sigsuspend_saved_mask==original);
        /* Replaying a parked syscall must retain the original saved mask. */
        assert(dispatch((uintptr_t)argument)==poll_result);
        assert(task.sigsuspend_active && !task.blocked_signals && task.sigsuspend_saved_mask==original);
        poll_result=1;
        assert(dispatch((uintptr_t)argument)==1);
        assert(!task.sigsuspend_active && task.blocked_signals==original);
    }
    const int64_t terminal[]={0,-RELIEFOS_EBADF,-RELIEFOS_EINTR};
    for (unsigned i=0;i<3;++i) {
        memset(&task,0,sizeof(task));task.blocked_signals=original;
        poll_result=terminal[i];
        assert(dispatch((uintptr_t)argument)==poll_result);
        assert(!task.sigsuspend_active && task.blocked_signals==original);
    }
    puts("PASS pselect: temporary mask survives both kernel park forms/replay and restores on final return");
}
