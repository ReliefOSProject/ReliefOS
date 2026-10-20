#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/page_cache.h>

/* A 24 MiB simultaneously pinned file working set, independent of the
 * implementation's retention budget. Never substitute private pages: two
 * mappings of the same file page must receive the same physical backing. */
#define WORKING_PAGES 6000u
static atomic_uint live_pages, live_metadata;
static atomic_bool fail_metadata;
static _Thread_local uint64_t irq_flags=1ULL<<9;
static pthread_barrier_t barrier;
struct load { uint64_t offset; bool rendezvous; };
struct result { struct storage_node *node; uint64_t offset, phys; };

uint32_t smp_current_cpu(void) { return 0; }
void smp_membarrier_poll(void) { }
uint64_t kernel_irq_save(void) { uint64_t saved=irq_flags;irq_flags=0;return saved; }
void kernel_irq_restore(uint64_t flags) { irq_flags=flags; }
void console_log(enum console_log_level level,const char *format,...)
{ (void)level;(void)format; }
uint64_t mm_alloc_page(void)
{
    void *page=aligned_alloc(4096,4096);assert(page);
    atomic_fetch_add(&live_pages,1);return (uintptr_t)page;
}
void mm_free_page(uint64_t phys)
{ assert(phys && atomic_load(&live_pages));atomic_fetch_sub(&live_pages,1);free((void *)(uintptr_t)phys); }
void *kernel_malloc(size_t size)
{
    assert(irq_flags == (1ULL << 9)); /* Metadata growth must run unlocked. */
    if (atomic_load(&fail_metadata))return NULL;
    void *memory=malloc(size);assert(memory);atomic_fetch_add(&live_metadata,1);return memory;
}
void kernel_free(void *memory)
{ if (memory){assert(irq_flags == (1ULL << 9));assert(atomic_load(&live_metadata));atomic_fetch_sub(&live_metadata,1);free(memory);} }
static int loader(uint64_t phys,void *opaque)
{
    struct load *load=opaque;
    memset((void *)(uintptr_t)phys,(int)(load->offset/4096%251),4096);
    if (load->rendezvous) {
        int status=pthread_barrier_wait(&barrier);
        assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return 0;
}
static void *concurrent_load(void *opaque)
{
    struct result *result=opaque;
    struct load load={.offset=result->offset,.rendezvous=true};
    assert(!page_cache_load(result->node,load.offset,loader,&load,&result->phys));
    assert(irq_flags==(1ULL<<9));return NULL;
}
int main(void)
{
    page_cache_init();
    struct storage_node node={.type=1,.first_cluster=71,.volume_id=3,
                             .size=(WORKING_PAGES*2+2)*4096ULL};
    uint64_t *pages=calloc(WORKING_PAGES,sizeof(*pages));assert(pages);
    for (unsigned i=0;i<WORKING_PAGES;++i) {
        struct load load={.offset=i*4096ULL};
        int status=page_cache_load(&node,load.offset,loader,&load,&pages[i]);
        if (status)fprintf(stderr,"FAIL: valid pinned file page %u rejected (status=%d), physical memory available\n",i,status);
        assert(!status && pages[i]);
    }
    for (unsigned i=0;i<WORKING_PAGES;i+=WORKING_PAGES-1) {
        uint64_t alias=0;
        assert(!page_cache_lookup(&node,i*4096ULL,&alias) && alias==pages[i]);
        assert(page_cache_owns(alias) && !page_cache_retain(alias));
        assert(*(unsigned char *)(uintptr_t)alias==(unsigned char)(i%251));
        page_cache_release(alias);page_cache_release(alias);
    }
    /* Reach a metadata-growth boundary without depending on chunk size. */
    uint64_t extra[WORKING_PAGES];unsigned extra_count=0;
    atomic_store(&fail_metadata,true);
    struct load load={.offset=WORKING_PAGES*4096ULL};uint64_t rejected=0;
    for (;;) {
        unsigned before=atomic_load(&live_pages);
        int status=page_cache_load(&node,load.offset,loader,&load,&rejected);
        if (status==PAGE_CACHE_FULL) {
            assert(!rejected && atomic_load(&live_pages)==before);
            break;
        }
        assert(!status && extra_count<WORKING_PAGES);
        extra[extra_count++]=rejected;rejected=0;load.offset+=4096;
    }
    atomic_store(&fail_metadata,false);
    /* Duplicate loaders must publish one shared page, with balanced refs,
     * including when metadata has to grow outside the spinlock. */
    assert(!pthread_barrier_init(&barrier,NULL,2));
    struct result result[2]={{.node=&node,.offset=load.offset},
                             {.node=&node,.offset=load.offset}};pthread_t threads[2];
    for (unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],NULL,concurrent_load,&result[i]));
    for (unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],NULL));
    assert(result[0].phys && result[0].phys==result[1].phys);
    page_cache_release(result[0].phys);page_cache_release(result[1].phys);
    assert(!pthread_barrier_destroy(&barrier));

    for (unsigned i=0;i<extra_count;++i)page_cache_release(extra[i]);
    page_cache_invalidate_node(&node); /* Pinned pages cannot be invalidated. */
    uint64_t alias;
    assert(!page_cache_lookup(&node,0,&alias) && alias==pages[0]);
    page_cache_release(alias);
    for (unsigned i=0;i<WORKING_PAGES;++i)page_cache_release(pages[i]);
    page_cache_invalidate_node(&node);
    uint32_t count=1;page_cache_stats(&count,NULL);assert(!count);
    assert(page_cache_lookup(&node,0,&alias) != 0);
    assert(!atomic_load(&live_pages) && !atomic_load(&live_metadata));
    assert(irq_flags==(1ULL<<9));free(pages);
    puts("PASS page cache: large pinned shared working set, aliasing, duplicate loaders, allocation failure, invalidation and reclamation");
}
