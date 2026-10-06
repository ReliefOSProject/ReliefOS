#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/lock.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"
#include "sysv_shm_fixture_stubs.h"

static unsigned char file_bytes[8192];
static uint64_t mapped;
static unsigned allocations;
static bool io_async;
static int read_error;
static bool short_read;
static unsigned reads;

uint64_t mm_alloc_page(void)
{
    void *p = aligned_alloc(4096, 4096);
    assert(p);
    memset(p, 0, 4096);
    ++allocations;
    return (uintptr_t)p;
}
void mm_free_page(uint64_t p) { assert(allocations); --allocations; free((void *)(uintptr_t)p); }
int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buffer, uint32_t size, uint32_t *got)
{
    assert(node->size == sizeof(file_bytes) && offset + size <= node->size);
    ++reads;
    *got = 0;
    if (io_async) return -RELIEFOS_EAGAIN;
    if (read_error) return read_error;
    if (short_read) --size;
    memcpy(buffer, file_bytes + offset, size);
    *got = size;
    return 0;
}
void storage_set_io_async_context(bool enabled) { io_async = enabled; }
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; abort(); }
int storage_errno(int status) { return status; }
bool address_space_map_user_page(struct address_space *as, uint64_t va,
                                 uint64_t phys, uint64_t flags)
{ (void)as; (void)va; (void)flags; mapped = phys; return true; }
void kernel_spin_init(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }

int main(void)
{
    struct task task = {0};
    struct task_vma prefix = {
        .used = 1, .prot = LINUX_PROT_READ,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_SHARED_FILE,
        .start = 0x5000000, .end = 0x5001000, .file_offset = 0,
        .file_limit = 0x800, .file_node = {.size = sizeof(file_bytes)}
    };
    struct task_vma text = prefix;
    text.start += 4096;
    text.end += 4096;
    text.prot |= LINUX_PROT_EXEC;
    text.file_limit = sizeof(file_bytes);
    memset(file_bytes, 0x53, sizeof(file_bytes));
    page_cache_init();
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &prefix, prefix.start) == 0);
    uint64_t prefix_page = mapped;
    assert(((unsigned char *)(uintptr_t)prefix_page)[0] == 0x53);
    assert(((unsigned char *)(uintptr_t)prefix_page)[0x900] == 0);
    /* usercopy can fault on a cold ELF page while syscall I/O is async. */
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    uint64_t text_page = mapped;
    assert(((unsigned char *)(uintptr_t)text_page)[0x900] == 0x53);
    assert(text_page != prefix_page);
    unsigned reads_before = reads;
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    assert(mapped == text_page);
    assert(reads == reads_before);

    struct task_vma cold = text;
    cold.file_offset = 4096;
    unsigned allocated_before = allocations;
    read_error = -RELIEFOS_EIO;
    assert(task_map_file_vma_page(&task, &cold, cold.start) == -RELIEFOS_EIO);
    assert(allocations == allocated_before && mapped == text_page);
    read_error = 0;
    short_read = true;
    assert(task_map_file_vma_page(&task, &cold, cold.start) == -RELIEFOS_EIO);
    assert(allocations == allocated_before && mapped == text_page);
    short_read = false;
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &cold, cold.start) == 0);
    assert(memcmp((void *)(uintptr_t)mapped, file_bytes + 4096, 4096) == 0);
    page_cache_release(mapped);
    page_cache_release(text_page);
    page_cache_release(text_page);
    if (page_cache_owns(prefix_page)) page_cache_release(prefix_page);
    else mm_free_page(prefix_page);
    struct storage_node resized = prefix.file_node;
    resized.size += 4096;
    page_cache_invalidate_node(&resized);
    assert(!allocations);
    /* Every cache slot is pinned; another executable page must still load. */
    uint64_t pinned[4096];
    for (unsigned i = 0; i < 4096; ++i) {
        text.file_node.first_cluster = i + 1;
        assert(task_map_file_vma_page(&task, &text, text.start) == 0);
        pinned[i] = mapped;
    }
    text.file_node.first_cluster = 4097;
    struct task_vma shared = text;
    shared.flags |= TASK_VMA_FLAG_SHARED;
    unsigned pinned_allocations = allocations;
    assert(task_map_file_vma_page(&task, &shared, shared.start) == -RELIEFOS_EIO);
    assert(allocations == pinned_allocations);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    assert(!page_cache_owns(mapped));
    assert(memcmp((void *)(uintptr_t)mapped, file_bytes, 4096) == 0);
    mm_free_page(mapped);
    for (unsigned i = 0; i < 4096; ++i) {
        page_cache_release(pinned[i]);
        text.file_node.first_cluster = i + 1;
        page_cache_invalidate_node(&text.file_node);
    }
    assert(!allocations);
    puts("PASS pinned cache exhaustion: executable pages remain loadable without leaks");
    puts("PASS ELF segments sharing a file page: independent zero-fill and intact code cache");
    puts("PASS cold ELF usercopy faults: synchronous I/O, cache hits, errors and short-read cleanup");
}
