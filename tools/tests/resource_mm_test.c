#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"
#include "sysv_shm_fixture_stubs.h"

uint32_t sched_task_vma_capacity(const struct task *task) { (void)task; return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index) { return &task->vmas[index]; }

int main(void)
{
    struct task task = {0};
    uint64_t base = RELIEFNT_USER_BASE;
    task.vmas[0] = (struct task_vma){.used = 1, .start = base, .end = base + 8192};
    task.stack_top = RELIEFNT_USER_TOP - 4096;
    task.stack_low = task.stack_top - 65536;
    uint64_t used = 8192 + 65536;
    task.limits.as.rlim_cur = used;
    assert(task_vma_total_bytes(&task) == used);
    assert(task_address_space_can_map(&task, base, base + 8192));
    assert(!task_address_space_can_map(&task, base, base + 12288));
    task.limits.as.rlim_cur = used + 4095;
    assert(!task_address_space_can_map(&task, base + 8192, base + 12288));
    task.limits.as.rlim_cur = used + 4096;
    assert(task_address_space_can_map(&task, base + 8192, base + 12288));
    task.address_space.initial_stack_top = task.stack_top;
    task.address_space.initial_stack_low = task.stack_low;
    task.stack_low = base;
    task.stack_top = base + 8192;
    assert(task_vma_total_bytes(&task) == used);
    task.limits.as.rlim_cur = 0;
    assert(!task_address_space_can_map(&task, base, base + 8192));
    task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(task_address_space_can_map(&task, base + 8192, base + 12288));

    /* RLIMIT_STACK is measured in Linux against the whole growable stack
     * span (stack_top - new_start), inclusive of the eager initial pages. */
    struct task stack_task = {0};
    stack_task.stack_top = RELIEFNT_USER_TOP - 4096;
    stack_task.stack_low = stack_task.stack_top - 65536;
    stack_task.address_space.initial_stack_top = stack_task.stack_top;
    stack_task.address_space.initial_stack_low = stack_task.stack_low;
    stack_task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t candidate = stack_task.stack_top - 65536 - 4096;
    uint64_t cross_pde = (stack_task.stack_top & ~(RELIEFNT_USER_PD_BYTES - 1ULL)) - 4096;
    /* The inherited low identity map makes the first fault in the next 2 MiB
     * slot report PRESENT even though no user PTE exists there. */
    assert(task_stack_fault_candidate(&stack_task, cross_pde, 4));
    assert(task_stack_fault_candidate(&stack_task, cross_pde, 6));
    assert(task_stack_fault_candidate(&stack_task, cross_pde, 5));
    assert(task_stack_fault_candidate(&stack_task, cross_pde, 7));
    assert(!task_stack_fault_candidate(&stack_task, cross_pde, 13));
    assert(!task_stack_fault_candidate(&stack_task, cross_pde, 21));
    stack_task.limits.stack.rlim_cur = 65536;
    assert(!task_stack_growth_allowed(&stack_task, candidate));
    stack_task.limits.stack.rlim_cur = 69632;
    assert(task_stack_growth_allowed(&stack_task, candidate));
    stack_task.limits.stack.rlim_cur = LINUX_RLIM_INFINITY;
    assert(task_stack_growth_allowed(&stack_task, stack_task.stack_top - 4096 * 4096));
    /* The address-space limit is still enforced when the stack rlimit allows. */
    stack_task.limits.as.rlim_cur = task_vma_total_bytes(&stack_task);
    assert(!task_stack_growth_allowed(&stack_task, candidate));
    assert(task_stack_growth_allowed(&stack_task, stack_task.stack_top - 4096));
    stack_task.limits.stack.rlim_cur = 0;
    assert(!task_stack_growth_allowed(&stack_task, candidate));
    /* Tightening a limit cannot revoke lazy pages inside the existing VMA. */
    assert(task_stack_growth_allowed(&stack_task, stack_task.stack_top - 32768));
    puts("PASS address-space accounting: shared initial stack, net replacement growth, rounding, zero, infinity and RLIMIT_STACK");
}
