/* Detect shared vvar lifetime bugs, permission mistakes and failed-exec leaks.
 * Run the real mapper and page tables; replace physical allocation and CPU
 * privileged operations with a reference-counted host physical-memory pool. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define address_space_map_user_page real_map_user_page
#include "../../kernel/reliefnt/arch/x86_64/mm/paging.c"
#undef address_space_map_user_page
bool address_space_map_user_page(struct address_space *, uint64_t, uint64_t, uint64_t);
#include "../../kernel/reliefnt/kernel/exec/vdso.c"

__asm__(".pushsection .rodata\n"
        ".global reliefnt_vdso_image_start\n"
        "reliefnt_vdso_image_start:\n"
        ".ascii \"vdso-mapping-fixture\"\n"
        ".global reliefnt_vdso_image_end\n"
        "reliefnt_vdso_image_end:\n.popsection\n");

static struct { uint64_t address; unsigned refs; } pool[2048];
static int allocation_budget = -1;
static unsigned bind_count;
static unsigned entropy;
static int map_budget = -1;
static bool fail_metadata;

bool address_space_map_user_page(struct address_space *as, uint64_t address,
                                 uint64_t physical, uint64_t flags)
{
    if (!map_budget) return false;
    if (map_budget > 0) --map_budget;
    return real_map_user_page(as, address, physical, flags);
}

uint64_t mm_alloc_page(void)
{
    if (!allocation_budget) return 0;
    if (allocation_budget > 0) --allocation_budget;
    for (unsigned i = 0; i < 2048; ++i) if (!pool[i].refs) {
        void *page = aligned_alloc(4096, 4096);
        assert(page);
        /* mm_alloc_page's real contract includes zero-filled pages. */
        memset(page, 0, 4096);
        pool[i].address = (uint64_t)(uintptr_t)page;
        pool[i].refs = 1;
        return pool[i].address;
    }
    abort();
}

void mm_retain_page(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i)
        if (pool[i].address == physical && pool[i].refs) { ++pool[i].refs; return; }
    abort();
}

void mm_free_page(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i)
        if (pool[i].address == physical && pool[i].refs) {
            if (!--pool[i].refs) free((void *)(uintptr_t)physical);
            return;
        }
    abort();
}

static unsigned references(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i)
        if (pool[i].address == physical && pool[i].refs) return pool[i].refs;
    return 0;
}

static unsigned live_pages(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 2048; ++i) count += pool[i].refs != 0;
    return count;
}

int page_cache_retain(uint64_t physical) { (void)physical; return -1; }
int page_cache_owns(uint64_t physical) { (void)physical; return 0; }
void page_cache_release(uint64_t physical) { (void)physical; abort(); }
void x86_64_load_cr3(uint64_t physical) { (void)physical; }
void smp_flush_user_tlb(void) { }
void x86_64_invlpg(uint64_t address) { (void)address; }
int kernel_random_fill(void *buffer, size_t size)
{ memset(buffer, (int)entropy++, size); return 0; }
void time_vdso_bind(struct reliefnt_vdso_data *data)
{
    ++bind_count;
    const unsigned char *bytes = (const void *)data;
    for (unsigned i = 0; i < 4096; ++i) assert(bytes[i] == 0);
    data->clock_mask = (1u << 5) | (1u << 6);
    data->basetime[6].sec = 17;
}

/* The real sched VMA accessors only allocate metadata, not pages. The fixture
 * moves the extra store on growth, catching retained-pointer mistakes. */
uint32_t sched_task_vma_capacity(const struct task *task)
{ return SCHED_TASK_VMA_MAX + sched_task_mm(task)->vma_extra_count; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    if (index < SCHED_TASK_VMA_MAX) return &task->vmas[index];
    index -= SCHED_TASK_VMA_MAX;
    struct task_address_space_state *mm = sched_task_mm(task);
    if (index >= mm->vma_extra_count) {
        if (fail_metadata) return NULL;
        unsigned wanted = index + 1;
        struct task_vma *next = calloc(wanted, sizeof(*next));
        assert(next);
        if (mm->vma_extra_count)
            memcpy(next, mm->vma_extra, mm->vma_extra_count * sizeof(*next));
        free(mm->vma_extra);
        mm->vma_extra = next;
        mm->vma_extra_count = wanted;
    }
    return &mm->vma_extra[index];
}

static struct task *new_task(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->kind = TASK_KIND_USER;
    task->stack_top = RELIEFNT_USER_TOP - 4096;
    assert(address_space_create(sched_task_as(task)));
    return task;
}

static void destroy_task(struct task *task)
{
    address_space_destroy(sched_task_as(task));
    free(task->vma_extra);
    free(task);
}

int main(void)
{
    nx_enabled = true;
    struct task *task = new_task();
    uint64_t ehdr = 123;
    unsigned before = live_pages();
    allocation_budget = 1;
    assert(vdso_map_task(task, &ehdr) == -12 && ehdr == 0);
    allocation_budget = -1;
    assert(live_pages() == before && !bind_count);

    /* Force allocation of two VMA slots across a moving extra-store boundary. */
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX - 1; ++i)
        task->vmas[i] = (struct task_vma){.used = 1, .start = 0x400000, .end = 0x401000};
    assert(vdso_map_task(task, &ehdr) == 0 && ehdr && bind_count == 1);
    assert(ehdr > RELIEFNT_KERNEL_HOLE_END && !(ehdr & 4095));
    const uint64_t text = address_space_user_page_phys(sched_task_as(task), ehdr);
    const uint64_t data = address_space_user_page_phys(sched_task_as(task), ehdr - 4096);
    assert(text && data && references(text) == 2 && references(data) == 2);
    assert(memcmp((void *)(uintptr_t)text, "vdso-mapping-fixture", 20) == 0);
    assert(!address_space_user_page_writable(sched_task_as(task), ehdr));
    assert(!address_space_user_page_writable(sched_task_as(task), ehdr - 4096));
    unsigned table = (unsigned)((ehdr - RELIEFNT_USER_BASE) / 4096 / 512);
    unsigned slot = (unsigned)((ehdr - RELIEFNT_USER_BASE) / 4096 % 512);
    assert(!(task->as.user_pt[table][slot] & RELIEFNT_PAGE_NOEXEC));
    table = (unsigned)((ehdr - 4096 - RELIEFNT_USER_BASE) / 4096 / 512);
    slot = (unsigned)((ehdr - 4096 - RELIEFNT_USER_BASE) / 4096 % 512);
    assert(task->as.user_pt[table][slot] & RELIEFNT_PAGE_NOEXEC);
    unsigned found = 0;
    for (unsigned i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma->start == ehdr || vma->start == ehdr - 4096) {
            assert(!(vma->max_prot & TASK_VMA_PROT_WRITE));
            assert(!(vma->flags & TASK_VMA_FLAG_ANON));
            if (vma->start == ehdr - 4096) assert(!(vma->max_prot & TASK_VMA_PROT_EXEC));
            ++found;
        }
    }
    assert(found == 2);
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(task), &child));
    assert(references(data) == 3 && references(text) == 3);
    assert(!address_space_handle_cow_fault(&child, ehdr - 4096));
    ((struct reliefnt_vdso_data *)(uintptr_t)data)->basetime[6].sec = 18;
    assert(((struct reliefnt_vdso_data *)(uintptr_t)
            address_space_user_page_phys(&child, ehdr - 4096))->basetime[6].sec == 18);
    address_space_destroy(&child);
    destroy_task(task);
    assert(references(data) == 1 && references(text) == 1);

    struct task *other = new_task();
    /* Occupied VMA at the randomly selected address must never be overwritten. */
    other->vmas[0] = (struct task_vma){.used = 1, .start = ehdr - 4096, .end = ehdr + 4096};
    uint64_t next;
    entropy = 0;
    assert(vdso_map_task(other, &next) == 0 && next && bind_count == 1);
    assert(next + 4096 <= ehdr - 4096 || next - 4096 >= ehdr + 4096);
    destroy_task(other);

    struct task *failed = new_task();
    before = live_pages();
    allocation_budget = 0;
    ehdr = 123;
    assert(vdso_map_task(failed, &ehdr) == -12 && !ehdr);
    assert(failed->as.user_page_count == 0 && live_pages() == before);
    for (unsigned i = 0; i < sched_task_vma_capacity(failed); ++i)
        assert(!sched_task_vma_at(failed, i)->used);
    allocation_budget = -1;
    destroy_task(failed);

    failed = new_task();
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX - 1; ++i)
        failed->vmas[i] = (struct task_vma){.used = 1, .start = 0x400000, .end = 0x401000};
    fail_metadata = true;
    assert(vdso_map_task(failed, &next) == -12 && !next);
    assert(!failed->vmas[SCHED_TASK_VMA_MAX - 1].used && !failed->as.user_page_count);
    fail_metadata = false;
    destroy_task(failed);

    /* An already mapped page need not have a VMA (legacy loader). */
    other = new_task();
    uint64_t occupied = mm_alloc_page();
    *(unsigned char *)(uintptr_t)occupied = 42;
    assert(real_map_user_page(sched_task_as(other), RELIEFNT_KERNEL_HOLE_END,
                              occupied, RELIEFNT_PAGE_NOEXEC));
    entropy = 0;
    assert(vdso_map_task(other, &next) == 0 && next > RELIEFNT_KERNEL_HOLE_END + 4096);
    assert(address_space_user_page_phys(sched_task_as(other), RELIEFNT_KERNEL_HOLE_END) == occupied);
    assert(*(unsigned char *)(uintptr_t)occupied == 42);
    destroy_task(other);

    /* Fail after the data PTE is installed: release it and both reserved VMAs. */
    failed = new_task();
    map_budget = 1;
    assert(vdso_map_task(failed, &next) == -12 && !next);
    map_budget = -1;
    assert(!failed->as.user_page_count && references(data) == 1 && references(text) == 1);
    for (unsigned i = 0; i < sched_task_vma_capacity(failed); ++i)
        assert(!sched_task_vma_at(failed, i)->used);
    destroy_task(failed);
    assert(live_pages() == 2); /* The kernel retains only the global image/data. */
    puts("PASS vDSO mapping: RX/R-NX, sharing, fork, collision, slot growth and rollback");
    return 0;
}
