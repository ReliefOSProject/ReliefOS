#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/device/device.c"
static struct task current;
static int calls;
struct task *sched_current_task(void) { return &current; }
bool user_range_ok(uint64_t p, uint64_t n) { return p >= 4096 && n == sizeof(struct reliefos_driver_control); }
bool user_range_writable(uint64_t p, uint64_t n) { return user_range_ok(p,n); }
int driver_manager_control(struct reliefos_driver_control *request)
{ ++calls; assert(!strcmp(request->file,"missing.drv")); request->status=-LINUX_ENOENT; return -LINUX_ENOENT; }
int main(void)
{
    struct reliefos_driver_control request={.action=RELIEFOS_DRIVER_CONTROL_LOAD,.file="missing.drv"};
    assert(syscall_driver_control(0,(uintptr_t)&request)==-LINUX_ENOTTY);
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==-LINUX_EPERM && !calls);
    current.cap_effective=1ULL<<CAP_SYS_MODULE;
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,0)==-LINUX_EFAULT && !calls);
    request.flags=1;
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==-LINUX_EINVAL && !calls);
    request.flags=0;
    memset(request.file,'a',sizeof(request.file));
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==-LINUX_EINVAL && !calls);
    strcpy(request.file,"missing.drv");
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==-LINUX_ENOENT);
    assert(calls==1 && request.status==-LINUX_ENOENT);
    puts("PASS real driver dispatch: CAP_SYS_MODULE, pointer/string/flags validation, actual backend error");
}
