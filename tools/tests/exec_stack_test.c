/* Exercise the production initial-stack admission path with controlled accounting. */
#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/mm/user/fault.c"
static uint64_t used_bytes;
static unsigned pages;
uint64_t task_vma_total_bytes(const struct task *task) { (void)task; return used_bytes; }
bool address_space_map_user_stack_page(struct address_space *as, uint64_t page)
{ (void)as; (void)page; ++pages; return true; }
int main(void)
{
    struct task task = {0};
    task.stack_top = RELIEFNT_USER_TOP - 4096;
    task.stack_low = task.stack_top - 65536;
    task.limits.stack.rlim_cur = 8u << 20;
    task.limits.as.rlim_cur = 1u << 20;
    used_bytes = 2u << 20;
    assert(syscall_mm_prepare_exec_stack(&task, task.stack_top - 4096) == -RELIEFOS_ENOMEM);
    assert(!pages);
    used_bytes = 1u << 20;
    assert(!syscall_mm_prepare_exec_stack(&task, task.stack_top - 4096));
    assert(syscall_mm_prepare_exec_stack(&task, task.stack_low - 4096) == -RELIEFOS_ENOMEM);
    task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_prepare_exec_stack(&task, task.stack_low - 8192));
    assert(pages == 2 && task.address_space.initial_stack_low == task.stack_low);
    task.limits.stack.rlim_cur = 65536;
    assert(syscall_mm_prepare_exec_stack(&task, task.stack_low - 4096) == -RELIEFOS_E2BIG);
    puts("PASS exec stack: existing mappings honor RLIMIT_AS, growth accounting and stack limits");
}
