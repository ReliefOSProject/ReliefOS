#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/arch/x86_64/paging.c"
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"
#include "sysv_shm_fixture_stubs.h"

static unsigned allocated;
static bool fail_alloc;
uint64_t mm_alloc_page(void)
{
    if (fail_alloc) return 0;
    void *p = aligned_alloc(4096, 4096);
    assert(p);
    memset(p, 0, 4096);
    ++allocated;
    return (uintptr_t)p;
}
void mm_free_page(uint64_t p) { assert(allocated); --allocated; free((void *)(uintptr_t)p); }
int page_cache_owns(uint64_t p) { (void)p; return 0; }
void page_cache_release(uint64_t p) { (void)p; abort(); }
void x86_64_invlpg(uint64_t p) { (void)p; }
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task; return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t i)
{ assert(i < SCHED_TASK_VMA_MAX); return &sched_task_mm(task)->vmas[i]; }

int main(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->kind = TASK_KIND_USER;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(task)));
    uint64_t va = RELIEFNT_USER_BASE + 4096;
    struct task_vma *vma = &task->vmas[0];
    *vma = (struct task_vma){.used = 1, .start = va, .end = va + 8192,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE, .flags = TASK_VMA_FLAG_ANON};
    assert(syscall_handle_private_anon_fault(task, va + 17, 6));
    uint64_t phys = address_space_user_page_phys(sched_task_as(task), va);
    assert(phys && address_space_user_page_writable(sched_task_as(task), va));
    for (unsigned i = 0; i < 4096; ++i) assert(!((unsigned char *)(uintptr_t)phys)[i]);
    *(unsigned char *)(uintptr_t)phys = 42;
    assert(!syscall_handle_private_anon_fault(task, va, 7));
    assert(*(unsigned char *)(uintptr_t)phys == 42);

    va += 4096;
    unsigned before = allocated;
    task->shared_mm = &task->address_space;
    assert(!syscall_handle_private_anon_fault(task, va, 6));
    task->shared_mm = NULL;
    vma->flags |= TASK_VMA_FLAG_SHARED;
    assert(!syscall_handle_private_anon_fault(task, va, 6));
    vma->flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY;
    assert(!syscall_handle_private_anon_fault(task, va, 6));
    vma->flags = TASK_VMA_FLAG_ANON;
    vma->prot = LINUX_PROT_NONE;
    assert(!syscall_handle_private_anon_fault(task, va, 4));
    vma->prot = LINUX_PROT_READ;
    assert(!syscall_handle_private_anon_fault(task, va, 6));
    assert(!syscall_handle_private_anon_fault(task, va, 20));
    assert(!syscall_handle_private_anon_fault(task, va, 12));
    assert(!syscall_handle_private_anon_fault(task, va + 4096, 4));
    assert(!syscall_handle_private_anon_fault(NULL, va, 4));
    fail_alloc = true;
    assert(!syscall_handle_private_anon_fault(task, va, 4));
    fail_alloc = false;
    assert(allocated == before);
    assert(!address_space_user_page_phys(sched_task_as(task), va));
    /* Supervisor identity PDE faults may have PRESENT set, but no user PTE. */
    assert(syscall_handle_private_anon_fault(task, va, 5));
    assert(address_space_user_page_readable(sched_task_as(task), va));
    assert(!address_space_user_page_writable(sched_task_as(task), va));
    address_space_destroy(sched_task_as(task));
    assert(!allocated);
    free(task);
    puts("PASS private faults: demand zero, ownership, protections, fallback, allocation failure");
}
