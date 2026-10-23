#define _GNU_SOURCE
#include <reliefnt/console.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../kernel/reliefnt/mm/physical/mm.c"

char __kernel_start[1], __kernel_end[1];

void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
const struct framebuffer *framebuffer_get(void) { static struct framebuffer fb; return &fb; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
static uint64_t private_zero_base;
static unsigned private_zero_checks;
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(lock->state);
    lock->state = 0;
    if (lock == &mm_lock && private_zero_base) {
        uint64_t base = private_zero_base;
        private_zero_base = 0;
        /* Clearing a large private allocation must not hold mm_lock. At
         * unlock its old contents remain, but every page is already owned. */
        assert(*(const unsigned char *)(uintptr_t)base == 0xa5);
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t page = page_index(base) + i;
            assert(page_refs[page] == 1);
            assert(!(free_pages[page / 64] & (1ULL << (page % 64))));
        }
        /* Model a second allocator entering before the first caller has
         * received its zeroed run: the private pages must not be reused. */
        uint64_t other = mm_alloc_page();
        assert(other && (other < base || other >= base + 8 * PAGE_SIZE));
        mm_free_page(other);
        ++private_zero_checks;
    }
}

int main(void)
{
    const uint64_t base = 0x60000000, count = 2048, bytes = count * PAGE_SIZE;
    assert(mmap((void *)base, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)base);
    struct multiboot2_mmap_entry entry = {.addr = base, .len = bytes, .type = 1};
    struct boot_info boot = {.mmap_addr = (uintptr_t)&entry, .mmap_entry_count = 1,
                             .mmap_entry_size = sizeof(entry)};
    mm_init(&boot);
    const uint64_t usable_base = base + MM_PAGE_COUNT / 8;
    const uint64_t usable_count = count - (MM_PAGE_COUNT / 8) / PAGE_SIZE;
    const uint64_t usable_bytes = usable_count * PAGE_SIZE;
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    for (unsigned round = 0; round < 3; ++round) {
        uint64_t all = mm_alloc_pages(usable_count);
        assert(all == usable_base && !mm_free_memory_kib());
        for (unsigned i = 0; i < usable_count; i += 2) mm_free_page(usable_base + i * PAGE_SIZE);
        assert(mm_free_memory_kib() == usable_bytes / 2048);
        for (unsigned i = 1; i < usable_count; i += 2) mm_free_page(usable_base + i * PAGE_SIZE);
        assert(mm_free_memory_kib() == usable_bytes / 1024);
    }
    uint64_t page = mm_alloc_page();
    mm_retain_page(page);
    mm_free_page(page);
    assert(mm_free_memory_kib() == usable_bytes / 1024 - 4);
    mm_free_page(page);
    mm_free_page(page);
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    assert(!mm_alloc_pages(count + 1));
    uint64_t block = mm_alloc_pages(8);
    assert(block == usable_base);
    memset((void *)block, 0xa5, 8 * PAGE_SIZE);
    mm_free_page(block);
    mm_free_pages(block + 3 * PAGE_SIZE, 3);
    assert(mm_alloc_pages(3) == block + 3 * PAGE_SIZE);
    assert(mm_alloc_page() == block);
    for (unsigned i = 0; i < PAGE_SIZE; ++i) assert(!((unsigned char *)block)[i]);
    mm_free_pages(block, 8);
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    block = mm_alloc_pages(8);
    memset((void *)(uintptr_t)block, 0xa5, 8 * PAGE_SIZE);
    mm_free_pages(block, 8);
    private_zero_base = block;
    assert(mm_alloc_pages(8) == block);
    assert(!private_zero_base && private_zero_checks == 1);
    for (unsigned i = 0; i < 8 * PAGE_SIZE; ++i)
        assert(!((const unsigned char *)(uintptr_t)block)[i]);
    mm_free_pages(block, 8);
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    mm_free_page(base); /* Bitmap storage remains reserved. */
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    munmap((void *)base, bytes);
    puts("PASS physical pages: >256 free fragments, repeated full reclamation, contiguous allocation and refs");
}
