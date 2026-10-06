#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the real page-table implementation; only physical allocation and
 * privileged CPU operations are replaced by host fixtures. */
#include "../../kernel/reliefnt/arch/x86_64/paging.c"

static struct { uint64_t address; unsigned refs; } pages[1024];

uint64_t mm_alloc_page(void)
{
#ifdef RELIEFNT_TEST_PAGE_ALLOC_GATE
    if (!RELIEFNT_TEST_PAGE_ALLOC_GATE()) return 0;
#endif
    for (unsigned i = 0; i < 1024; ++i) if (!pages[i].refs) {
        void *p = aligned_alloc(4096, 4096);
        assert(p);
        memset(p, 0, 4096);
        pages[i].address = (uint64_t)(uintptr_t)p;
        pages[i].refs = 1;
        return pages[i].address;
    }
    abort();
}

void mm_retain_page(uint64_t p)
{
    for (unsigned i = 0; i < 1024; ++i) if (pages[i].address == p && pages[i].refs) {
        ++pages[i].refs;
        return;
    }
    abort();
}

void mm_free_page(uint64_t p)
{
    for (unsigned i = 0; i < 1024; ++i) if (pages[i].address == p && pages[i].refs) {
        if (!--pages[i].refs) free((void *)(uintptr_t)p);
        return;
    }
    abort();
}

int page_cache_retain(uint64_t p) { (void)p; return -1; }
int page_cache_owns(uint64_t p) { (void)p; return 0; }
void page_cache_release(uint64_t p) { (void)p; abort(); }
void x86_64_load_cr3(uint64_t p) { (void)p; }
void smp_flush_user_tlb(void) { }
void x86_64_invlpg(uint64_t p) { (void)p; }

int main(void)
{
    struct address_space parent = {0}, child = {0};
    const uint64_t va = RELIEFNT_USER_BASE;
    nx_enabled = true;
    assert(address_space_create(&parent));
    uint64_t backing = mm_alloc_page();
    *(unsigned char *)(uintptr_t)backing = 42;
    assert(address_space_map_user_page(&parent, va, backing,
                                      RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE));
    assert(address_space_user_page_phys(&parent, va) == backing);
    assert(!address_space_user_page_readable(&parent, va));
    assert(!address_space_user_page_writable(&parent, va));
    assert(!address_space_handle_cow_fault(&parent, va));
    assert(!address_space_map_user_page(&parent, va, backing, RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_resident_kib(&parent) == 4);
    uint64_t device = mm_alloc_page();
    assert(address_space_map_user_page(&parent, va + 4096, device,
                                      RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_resident_kib(&parent) == 4);
    assert(address_space_unmap_user_page(&parent, va + 4096) == device);
    mm_free_page(device);
    assert(address_space_clone_cow(&parent, &child));
    assert(child.user_page_count == 1);
    assert(!address_space_user_page_readable(&child, va));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_readable(&child, va));
    assert(address_space_handle_cow_fault(&child, va));
    uint64_t private_page = address_space_user_page_phys(&child, va);
    assert(private_page != backing && *(unsigned char *)(uintptr_t)private_page == 42);
    *(unsigned char *)(uintptr_t)private_page = 17;
    assert(*(unsigned char *)(uintptr_t)backing == 42);
    assert(address_space_protect_user_page(&child, va, RELIEFNT_PAGE_NOEXEC));
    assert(!address_space_handle_cow_fault(&child, va));
    assert(!address_space_user_page_writable(&child, va));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE));
    assert(!address_space_user_page_readable(&child, va));
    assert(address_space_unmap_user_page(&child, va) == private_page);
    mm_free_page(private_page);
    assert(!child.user_page_count);
    assert(address_space_user_resident_kib(&child) == 0);
    address_space_destroy(&child);
    address_space_destroy(&parent);
    assert(address_space_create(&parent));
    backing = mm_alloc_page();
    assert(address_space_map_user_page(&parent, va, backing,
                                      RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE |
                                      RELIEFNT_PAGE_SHARED));
    assert(address_space_clone_cow(&parent, &child));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_writable(&child, va));
    assert(!address_space_handle_cow_fault(&child, va));
    assert(address_space_user_page_phys(&child, va) == backing);
    *(unsigned char *)(uintptr_t)backing = 61;
    assert(address_space_protect_user_page(&parent, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_writable(&parent, va));
    assert(*(unsigned char *)(uintptr_t)address_space_user_page_phys(&parent, va) == 61);
    address_space_destroy(&parent);
    assert(*(unsigned char *)(uintptr_t)address_space_user_page_phys(&child, va) == 61);
    address_space_destroy(&child);
    for (unsigned i = 0; i < 1024; ++i) assert(!pages[i].refs);
    puts("PASS page protection: PROT_NONE, private COW, shared fork, restore, unmap, ownership");
}
