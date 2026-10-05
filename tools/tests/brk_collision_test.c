/* A Linux brk extension must never replace a VMA already in the process. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <linux/mman.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall_internal.h>

int64_t syscall_mm_brk(uint64_t requested);

static struct task *current_task;
struct task *sched_current_task(void) { return current_task; }

uint32_t sched_task_vma_capacity(const struct task *task)
{
    (void)task;
    return SCHED_TASK_VMA_MAX;
}

struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    return index < SCHED_TASK_VMA_MAX ? &sched_task_mm(task)->vmas[index] : NULL;
}

uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t address)
{
    (void)as;
    (void)address;
    return 0;
}

bool address_space_user_page_is_device(const struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return 0; }
bool address_space_prepare_user_range(struct address_space *as, uint64_t start, uint64_t end)
{ (void)as; (void)start; (void)end; return true; }
bool address_space_map_user_page(struct address_space *as, uint64_t address, uint64_t phys, uint64_t flags)
{ (void)as; (void)address; (void)phys; (void)flags; return true; }
void smp_flush_user_tlb(void) {}
int page_cache_owns(uint64_t phys) { (void)phys; return 0; }
void page_cache_release(uint64_t phys) { (void)phys; }
void mm_free_page(uint64_t phys) { (void)phys; }
uint64_t mm_alloc_page(void) { return 0; }
uint64_t mm_free_memory_kib(void) { return UINT64_MAX; }
int storage_inode_put(struct storage_inode_ref *reference) { (void)reference; return 0; }
void storage_inode_retain(struct storage_inode_ref *reference) { (void)reference; }
int storage_inode_get(const struct storage_node *node, struct storage_inode_ref **out)
{ (void)node; *out = NULL; return 0; }
int storage_inode_refresh(struct storage_node *node) { (void)node; return 0; }
int storage_node_mount_flags(const struct storage_node *node, uint64_t *flags)
{ (void)node; *flags = 0; return 0; }
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }
int file_can_read(const struct task_file *file) { (void)file; return 0; }
int file_can_write(const struct task_file *file) { (void)file; return 0; }
int task_shm_map(const struct task_file *file, uint64_t offset, uint64_t length, uint64_t *phys)
{ (void)file; (void)offset; (void)length; (void)phys; return -1; }
const struct framebuffer *framebuffer_get(void) { return NULL; }
void console_printf(const char *fmt, ...) { (void)fmt; }
void *kernel_malloc(size_t size) { return __builtin_malloc(size); }
void kernel_free(void *ptr) { __builtin_free(ptr); }

int main(void)
{
    const uint64_t page = 4096;
    const uint64_t start = RELIEFNT_USER_HEAP_BASE + 0x20000;
    struct task task = {0};
    task.kind = TASK_KIND_USER;
    task.stack_top = RELIEFNT_USER_TOP - page;
    task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    task.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    task.program_break_base = task.program_break = start;
    task.address_space.vmas[0] = (struct task_vma){
        .used = 1, .start = start + page, .end = start + 2 * page,
        .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_READ,
    };
    current_task = &task;

    /* Linux brk(2) returns the old break when an existing VMA blocks growth. */
    int64_t result = syscall_mm_brk(start + 2 * page);
    if (result != (int64_t)start) {
        fprintf(stderr, "brk collision returned %#llx, expected %#llx\n",
                (unsigned long long)result, (unsigned long long)start);
    }
    assert(result == (int64_t)start);
    assert(task.program_break == start);
    assert(task.address_space.vmas[0].used);
    assert(task.address_space.vmas[0].start == start + page);
    assert(task.address_space.vmas[0].end == start + 2 * page);
    assert(task.address_space.vmas[0].prot == LINUX_PROT_READ);

    /* CLONE_VM threads must observe the same process break, just as they
     * observe the same mappings. The local copy models the task state made
     * by clone before both tasks point at the promoted shared mm. */
    struct task process = {0};
    struct task thread;
    struct task_address_space_state shared_mm;
    process.kind = TASK_KIND_USER;
    process.stack_top = RELIEFNT_USER_TOP - page;
    process.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    process.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    process.program_break_base = process.program_break = start;
    thread = process;
    shared_mm = process.address_space;
    process.shared_mm = thread.shared_mm = &shared_mm;

    current_task = &process;
    result = syscall_mm_brk(start + page);
    if (result != (int64_t)(start + page)) {
        fprintf(stderr, "shared brk growth returned %#llx, current=%#llx base=%#llx\n",
                (unsigned long long)result,
                (unsigned long long)process.program_break,
                (unsigned long long)process.program_break_base);
    }
    assert(result == (int64_t)(start + page));
    current_task = &thread;
    assert(syscall_mm_brk(0) == (int64_t)(start + page));
    puts("PASS brk: collision protection and CLONE_VM shared break");
    return 0;
}
