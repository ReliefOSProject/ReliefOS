#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/sched.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>
#include <linux/capability.h>
#undef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0
#include "../../kernel/reliefnt/arch/x86_64/mm/paging.c"
#include "../../kernel/reliefnt/kernel/syscall/process/process_vm.c"

static struct task caller, target;
static unsigned allocations, faults;
static int fail_allocations;
static struct { uint64_t address; unsigned refs; } pages[2048];
void *kernel_malloc(size_t n)
{ if (fail_allocations) return NULL; void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
uint64_t mm_alloc_page(void)
{
    for (unsigned i = 0; i < 2048; ++i) if (!pages[i].refs) {
        void *p = aligned_alloc(4096, 4096);
        assert(p); memset(p, 0, 4096);
        pages[i].address = (uintptr_t)p; pages[i].refs = 1;
        return (uintptr_t)p;
    }
    abort();
}
void mm_retain_page(uint64_t p)
{
    for (unsigned i = 0; i < 2048; ++i) if (pages[i].address == p && pages[i].refs) {
        ++pages[i].refs; return;
    }
    abort();
}
void mm_free_page(uint64_t p)
{
    for (unsigned i = 0; i < 2048; ++i) if (pages[i].address == p && pages[i].refs) {
        if (!--pages[i].refs) free((void *)(uintptr_t)p);
        return;
    }
    abort();
}
int page_cache_retain(uint64_t p) { (void)p; return -1; }
int page_cache_owns(uint64_t p) { (void)p; return 0; }
void page_cache_release(uint64_t p) { (void)p; abort(); }
void x86_64_load_cr3(uint64_t p) { (void)p; }
void x86_64_invlpg(uint64_t p) { (void)p; }
struct task *sched_current_task(void) { return &caller; }
struct task *sched_find(uint32_t pid) { return pid == 10 ? &caller : pid == 20 ? &target : NULL; }
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task; return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t i) { return &sched_task_mm(task)->vmas[i]; }
bool user_range_ok(uint64_t p, uint64_t n) { return p >= 4096 && n <= UINT64_MAX - p; }
int syscall_handle_task_page_fault(struct task *task, uint64_t address, uint64_t error)
{
    ++faults;
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX; ++i) {
        struct task_vma *vma = &sched_task_mm(task)->vmas[i];
        if (vma->used && address >= vma->start && address < vma->end && vma->prot &&
            !address_space_user_page_phys(sched_task_as(task), address)) {
            if ((error & 2) && !(vma->prot & TASK_VMA_PROT_WRITE)) return 0;
            return address_space_map_user_page(sched_task_as(task), address & ~4095ULL,
                mm_alloc_page(), RELIEFNT_PAGE_NOEXEC | (vma->prot & TASK_VMA_PROT_WRITE ? RELIEFNT_PAGE_WRITABLE : 0));
        }
    }
    return 0;
}
static uint64_t add_page(struct task *task, uint64_t va, bool writable)
{
    uint64_t p = mm_alloc_page();
    assert(address_space_map_user_page(sched_task_as(task), va, p, RELIEFNT_PAGE_NOEXEC | (writable ? RELIEFNT_PAGE_WRITABLE : 0)));
    return p;
}
static int64_t transfer(int pid, struct iovec *local, uint64_t ln,
                        struct iovec *remote, uint64_t rn, uint64_t flags, bool write)
{ return syscall_process_vm(pid, (uintptr_t)local, ln, (uintptr_t)remote, rn, flags, write); }

int main(void)
{
    nx_enabled = true;
    caller.pid = caller.tgid = 10; target.pid = target.tgid = 20;
    caller.kind = target.kind = TASK_KIND_USER;
    caller.uid = caller.euid = caller.suid = target.uid = target.euid = target.suid = 1000;
    caller.gid = caller.egid = caller.sgid = target.gid = target.egid = target.sgid = 100;
    assert(address_space_create(&caller.as) && address_space_create(&target.as));
    const uint64_t va = RELIEFNT_USER_BASE + 0x20000;
    uint64_t lpage = add_page(&caller, va, true), rpage = add_page(&target, va, true);
    memcpy((void *)rpage, "0123456789abcdef", 16);
    struct iovec local[2] = {{(void *)va, 3}, {(void *)(va + 3), 13}};
    struct iovec remote[2] = {{(void *)va, 7}, {(void *)(va + 7), 9}};
    assert(transfer(20, local, 2, remote, 2, 0, false) == 16);
    assert(!memcmp((void *)lpage, (void *)rpage, 16));
    memcpy((void *)lpage, "abcdefghijklmnop", 16);
    assert(transfer(20, local, 2, remote, 2, 0, true) == 16);
    assert(!memcmp((void *)lpage, (void *)rpage, 16));
    assert(transfer(-1, NULL, 0, NULL, UINT64_MAX, 1, false) == -LINUX_EINVAL);
    assert(transfer(-1, NULL, 0, (void *)1, UINT64_MAX, 0, false) == 0);
    assert(transfer(-1, local, 2, NULL, 0, 0, false) == 0);
    assert(transfer(-1, local, 2, remote, 2, 0, false) == -LINUX_ESRCH);
    assert(transfer(20, local, 1025, remote, 2, 0, false) == -LINUX_EINVAL);
    assert(transfer(20, local, 2, remote, 1ULL << 32, 0, false) == -LINUX_EINVAL);
    assert(transfer(20, local, (1ULL << 32) | 2, remote, 2, 0, false) == 16);
    remote[0].iov_len = UINT64_MAX;
    assert(transfer(20, local, 2, remote, 2, 0, false) == -LINUX_EINVAL);
    remote[0].iov_len = 7;
    target.euid = 1001;
    assert(transfer(20, local, 2, remote, 2, 0, false) == -LINUX_EPERM);
    target.euid = 1000; target.cap_permitted = 1ULL << CAP_KILL;
    assert(transfer(20, local, 2, remote, 2, 0, false) == -LINUX_EPERM);
    caller.cap_permitted = target.cap_permitted;
    assert(transfer(20, local, 2, remote, 2, 0, false) == 16);
    target.address_space.nondumpable = true;
    assert(transfer(20, local, 2, remote, 2, 0, false) == -LINUX_EPERM);
    caller.cap_effective = 1ULL << CAP_SYS_PTRACE;
    assert(transfer(20, local, 2, remote, 2, 0, false) == 16);
    caller.cap_effective = 0; target.address_space.nondumpable = false;
    caller.address_space.nondumpable = true;
    assert(transfer(10, local, 2, remote, 2, 0, false) == 16);
    caller.address_space.nondumpable = false;
    target.state = TASK_EXITED;
    assert(transfer(20, local, 2, remote, 2, 0, false) == -LINUX_ESRCH);
    target.state = TASK_READY;
    local[0] = (struct iovec){(void *)va, 32};
    remote[0] = (struct iovec){(void *)(va + 4096 - 16), 32};
    memset((void *)(rpage + 4096 - 16), 0x53, 16);
    assert(transfer(20, local, 1, remote, 1, 0, false) == 16);
    assert(((unsigned char *)lpage)[0] == 0x53 && ((unsigned char *)lpage)[15] == 0x53);
    remote[0] = (struct iovec){(void *)va, 32};
    local[0] = (struct iovec){(void *)(va + 4096 - 16), 32};
    assert(transfer(20, local, 1, remote, 1, 0, false) == 16);
    assert(transfer(20, local, 1, remote, 1, 0, true) == 16);
    target.vmas[0] = (struct task_vma){.used = 1, .prot = TASK_VMA_PROT_READ,
        .start = va, .end = va + 4096};
    assert(transfer(20, local, 1, remote, 1, 0, true) == -LINUX_EFAULT);
    target.vmas[0].prot = TASK_VMA_PROT_EXEC;
    assert(transfer(20, local, 1, remote, 1, 0, false) == -LINUX_EFAULT);
    target.vmas[0].prot = TASK_VMA_PROT_READ | TASK_VMA_PROT_WRITE;
    target.vmas[0].flags = TASK_VMA_FLAG_DEVICE;
    assert(transfer(20, local, 1, remote, 1, 0, false) == -LINUX_EFAULT);
    target.vmas[0].flags = TASK_VMA_FLAG_ANON;
    target.vmas[0].end = va + 18 * 4096;
    remote[0].iov_len = 18 * 4096;
    fail_allocations = 1;
    assert(transfer(-1, local, 1, remote, 1, 0, false) == -LINUX_ENOMEM);
    fail_allocations = 0;
    local[0] = (struct iovec){(void *)1, 1};
    assert(transfer(20, local, 1, remote, 1, 0, false) == -LINUX_EFAULT);
    assert(address_space_user_page_phys(&target.as, va + 17 * 4096) && faults >= 17);
    assert(!allocations);
    address_space_destroy(&target.as);
    memset(target.vmas, 0, sizeof(target.vmas));
    assert(address_space_clone_cow(&caller.as, &target.as));
    local[0] = (struct iovec){(void *)1, 1};
    remote[0] = (struct iovec){(void *)va, 1};
    assert(transfer(20, local, 1, remote, 1, 0, true) == -LINUX_EFAULT);
    uint64_t private_page = address_space_user_page_phys(&target.as, va);
    assert(private_page != lpage && address_space_user_page_writable(&target.as, va));
    local[0] = (struct iovec){(void *)va, 1};
    *(unsigned char *)lpage = 0x62;
    assert(transfer(20, local, 1, remote, 1, 0, true) == 1 && *(unsigned char *)private_page == 0x62);
    address_space_destroy(&target.as); address_space_destroy(&caller.as);
    for (unsigned i = 0; i < 2048; ++i) assert(!pages[i].refs);
    assert(!allocations);
    puts("PASS real process_vm: vector import, credentials, partial copies, pinned pages, protection and COW");
}
