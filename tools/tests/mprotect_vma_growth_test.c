/* Reproduce musl pthread_create's PROT_NONE stack followed by mprotect.
 * Link the real growable scheduler VMA store and exercise actual paging under
 * ASan so both retained pointers and lazy-page access are checked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/sched.h>

static struct task *current;
static struct task *test_current_task(void) { return current; }
#define sched_current_task test_current_task
#include "../../kernel/reliefnt/arch/x86_64/paging.c"
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"
#undef sched_current_task

static unsigned pages, allocations;
static int fail_after = -1;
void *kernel_malloc(size_t size)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    ++allocations;
    void *memory = malloc(size);
    assert(memory);
    return memory;
}
void kernel_free(void *memory) { free(memory); }
uint64_t mm_alloc_page(void)
{
    void *memory = aligned_alloc(4096, 4096);
    assert(memory);
    memset(memory, 0, 4096);
    ++pages;
    return (uintptr_t)memory;
}
void mm_free_page(uint64_t page) { assert(pages); --pages; free((void *)(uintptr_t)page); }
int page_cache_owns(uint64_t page) { (void)page; return 0; }
void page_cache_release(uint64_t page) { (void)page; abort(); }
void x86_64_invlpg(uint64_t page) { (void)page; }
void smp_flush_user_tlb(void) {}
void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }

static const uint64_t base = RELIEFNT_USER_BASE + 0x100000;

static void setup(unsigned extra)
{
    current = calloc(1, sizeof(*current));
    assert(current);
    current->kind = TASK_KIND_USER;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(current)));
    unsigned count = SCHED_TASK_VMA_MAX + extra;
    for (unsigned i = 0; i < count; ++i) {
        struct task_vma *vma = sched_task_vma_at(current, i);
        assert(vma);
        *vma = (struct task_vma){.used = 1, .start = base + (i + 1) * 0x10000,
            .end = base + (i + 1) * 0x10000 + 4096,
            .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_NONE,
            .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC};
    }
    struct task_vma *target = sched_task_vma_at(current, count - 1);
    target->start = base;
    target->end = base + 5 * 4096;
}

static void teardown(void)
{
    sched_task_vma_release(current);
    address_space_destroy(sched_task_as(current));
    assert(!pages);
    free(current);
    current = NULL;
}

static void test_split(unsigned extra, unsigned first, unsigned last)
{
    setup(extra);
    uint64_t start = base + first * 4096, end = base + last * 4096;
    /* Leave len unaligned: Linux/POSIX apply protection through the last page. */
    assert(syscall_mm_mprotect(start, end - start - 1,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        assert(vma);
        bool writable = i >= first && i < last;
        assert(vma->prot == (writable ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_NONE));
        assert(syscall_handle_private_anon_fault(current, address + 17, 6) == writable);
        assert(address_space_user_page_writable(sched_task_as(current), address) == writable);
    }
    uint64_t phys = address_space_user_page_phys(sched_task_as(current), start);
    assert(phys);
    *(unsigned char *)(uintptr_t)phys = 42;
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_NONE) == 0);
    assert(!address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_READ) == 0);
    assert(address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(*(unsigned char *)(uintptr_t)phys == 42);
    teardown();
}

static void test_allocation_failure(unsigned extra)
{
    setup(extra);
    uint32_t index = SCHED_TASK_VMA_MAX + extra - 1;
    struct task_vma saved = *sched_task_vma_at(current, index);
    fail_after = 0;
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(!memcmp(&saved, sched_task_vma_at(current, index), sizeof(saved)));
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        assert(sched_task_vma_at(current, i)->used != 0xffffffffu);
    assert(!syscall_handle_private_anon_fault(current, base + 4096, 6));
    assert(syscall_mm_mprotect(base + 1, 4096, LINUX_PROT_READ) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mprotect(base - 4096, 4096, LINUX_PROT_READ) == -RELIEFOS_ENOMEM);
    sched_task_vma_at(current, index)->max_prot = LINUX_PROT_READ;
    assert(syscall_mm_mprotect(base, 5 * 4096, LINUX_PROT_WRITE) == -RELIEFOS_EACCES);
    teardown();
}

static void test_append_with_spare_capacity(void)
{
    setup(1);
    struct task_vma *store = current->vma_extra;
    unsigned before = allocations;
    assert(sched_task_vma_at(current, SCHED_TASK_VMA_MAX + 1));
    assert(current->vma_extra == store && allocations == before);
    assert(current->vma_extra_count == 2);
    teardown();
}

static void test_adjacent_vmas(void)
{
    setup(4);
    unsigned count = SCHED_TASK_VMA_MAX + 4;
    struct task_vma *left = sched_task_vma_at(current, count - 1);
    struct task_vma *right = sched_task_vma_at(current, count - 2);
    assert(left && right);
    left->end = base + 3 * 4096;
    right->start = left->end;
    right->end = base + 5 * 4096;
    right->prot = LINUX_PROT_READ;
    /* Linux accepts one mprotect range spanning two adjacent VMAs. */
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        uint32_t expected = i == 0 ? LINUX_PROT_NONE :
                            i < 4 ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_READ;
        assert(vma && vma->prot == expected);
        bool writable = i > 0 && i < 4;
        assert(syscall_handle_private_anon_fault(current, address, 6) == writable);
    }
    teardown();
}

int main(void)
{
    for (unsigned extra = 1; extra <= 17; ++extra) {
        test_split(extra, 1, 5); /* musl stack guard: prefix split */
        test_split(extra, 0, 4); /* suffix split */
        test_split(extra, 1, 4); /* both sides, including a growth between slots */
    }
    test_allocation_failure(15); /* one spare append before the allocation fails */
    test_allocation_failure(16); /* allocation fails on the first needed slot */
    test_append_with_spare_capacity();
    test_adjacent_vmas();
    puts("PASS mprotect: VMA growth, guard pages, lazy writes, page rounding, revoke/restore and errno");
    assert(allocations);
    return 0;
}
