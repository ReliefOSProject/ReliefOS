#include "audio_fixture.h"
#include <sys/mman.h>
#include <reliefos/driver.h>
#include "../../kernel/reliefnt/mm/physical/mm.c"
#include "../../kernel/reliefnt/arch/x86_64/platform/pci_irq.c"
#include "../../kernel/reliefnt/arch/x86_64/platform/pci.c"
#define align_down paging_align_down
#include "../../kernel/reliefnt/arch/x86_64/mm/paging.c"
#undef align_down
#include "../../kernel/reliefnt/sound/core/core.c"
#include "../../kernel/reliefnt/kernel/device/driver_resources.c"

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

/* Resource/manager fixtures link the registry without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

static unsigned shootdowns;
void x86_64_invlpg(uint64_t a) {(void)a;}
void smp_flush_user_tlb(void) {++shootdowns;}
static uint32_t cfg[64], cfg2[64], bar_masks[64], config_address;
static uint32_t *selected_config(void) {
    if ((config_address & 0x00ffff00u)==0) return cfg;
    if ((config_address & 0x00ffff00u)==0x100 && cfg2[0]) return cfg2;
    return NULL;
}
uint32_t x86_64_inl(uint16_t port) {
    assert(port==0xcfc);uint32_t *p=selected_config();if(!p)return UINT32_MAX;
    uint32_t i=(config_address & 0xfc)/4;
    uint32_t v=__atomic_load_n(&p[i],__ATOMIC_ACQUIRE);
    return v==UINT32_MAX && bar_masks[i] ? bar_masks[i] : v;
}
void x86_64_outl(uint32_t value,uint16_t port) {
    if(port==0xcf8){config_address=value;return;}assert(port==0xcfc);
    uint32_t *p=selected_config();if(p)__atomic_store_n(&p[(config_address & 0xfc)/4],value,__ATOMIC_RELEASE);
}
void x86_64_outw(uint16_t value,uint16_t port) {
    assert(port==0xcfc || port==0xcfe);uint32_t *p=selected_config();assert(p);
    uint32_t i=(config_address & 0xfc)/4,shift=(port & 2)*8;
    uint32_t old=__atomic_load_n(&p[i],__ATOMIC_ACQUIRE);
    __atomic_store_n(&p[i],(old&~(0xffffu<<shift))|((uint32_t)value<<shift),__ATOMIC_RELEASE);
}
bool apic_enabled(void) { return true; }
uint32_t apic_bsp_id(void) { return 3; }
static uint64_t bitmap[MM_PAGE_COUNT/64];
static void mm_fixture_init(uint64_t low,uint32_t ln,uint64_t high,uint32_t hn) {
    free_pages=bitmap; free_page_count=ln+hn; allocation_hint=high/4096; reserved_range_count=0;
    __builtin_memset(bitmap,0,sizeof bitmap); __builtin_memset(page_refs,0,sizeof page_refs);
    for(uint32_t n=0;n<ln;n++) bitmap[(low/4096+n)/64]|=1ULL<<((low/4096+n)%64);
    for(uint32_t n=0;n<hn;n++) bitmap[(high/4096+n)/64]|=1ULL<<((high/4096+n)%64);
    if(ln) assert(mmap((void*)(uintptr_t)low,ln*4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(void*)-1);
    if(hn) assert(mmap((void*)(uintptr_t)high,hn*4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(void*)-1);
}
static void handler(void *p) { ++*(unsigned*)p; }
static pthread_mutex_t irq_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t irq_cond=PTHREAD_COND_INITIALIZER;
static unsigned irq_entered,irq_release,irq_freed;
static void held_handler(void *p) {
    (void)p;pthread_mutex_lock(&irq_mutex);irq_entered=1;pthread_cond_broadcast(&irq_cond);
    while(!irq_release)pthread_cond_wait(&irq_cond,&irq_mutex);pthread_mutex_unlock(&irq_mutex);
}
static void *dispatch_thread(void *p){pci_irq_dispatch((uintptr_t)p);return NULL;}
static void *free_thread(void *p){pci_free_irq((uintptr_t)p);__atomic_store_n(&irq_freed,1,__ATOMIC_RELEASE);return NULL;}
static void msi_disable_synchronizes_inflight(void) {
    struct reliefos_driver_pci_device d={0};uint32_t handle;
    assert(!pci_request_irq(&d,held_handler,NULL,&handle));uint32_t vector=0x50+(handle&255);
    pthread_t isr,release;pthread_create(&isr,NULL,dispatch_thread,(void*)(uintptr_t)vector);
    pthread_mutex_lock(&irq_mutex);while(!irq_entered)pthread_cond_wait(&irq_cond,&irq_mutex);pthread_mutex_unlock(&irq_mutex);
    pthread_create(&release,NULL,free_thread,(void*)(uintptr_t)handle);
    while(__atomic_load_n(&cfg[0x40/4],__ATOMIC_ACQUIRE)&(1u<<16)){}
    assert(!__atomic_load_n(&irq_freed,__ATOMIC_ACQUIRE));
    pthread_mutex_lock(&irq_mutex);irq_release=1;pthread_cond_broadcast(&irq_cond);pthread_mutex_unlock(&irq_mutex);
    pthread_join(isr,NULL);pthread_join(release,NULL);assert(irq_freed);
    pci_irq_dispatch(vector);assert(pci_irqs[handle&255].active==0 && !pci_irqs[handle&255].retiring);
    puts("msi_disable_waits_for_inflight_ISR PASS");
}
static void dma_mask_is_enforced(void) {
    mm_fixture_init(0x100000,32,0x100000000ULL,32);
    uint64_t p=mm_alloc_pages_below(2,0xffffffffULL); assert(p && p+8191<=0xffffffffULL);
    mm_free_pages(p,2); assert(free_page_count==64);
    mm_fixture_init(0,0,0x100000000ULL,32); assert(!mm_alloc_pages_below(2,0xffffffffULL));
    assert(!mm_alloc_pages_below(0xffffffffu,UINT64_MAX));
}
static void mmio_pages_are_precise_and_owned(void) {
    mm_fixture_init(0x30000000,32,0,0); nx_enabled=true;
    kernel_pd[3][0]=0xc0000000ULL|0x83;
    assert(paging_acquire_mmio(0xc0002000,8192));
    uint64_t *pt=(void*)(uintptr_t)(kernel_pd[3][0]&RELIEFNT_PHYS_ADDR_MASK);
    assert(pt[1]==(0xc0001000ULL|3));
    assert(pt[2]==(0xc0002000ULL|3|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_NOEXEC));
    assert(paging_acquire_mmio(0xc0002000,4096));
    paging_release_mmio(0xc0002000,8192); assert(pt[3]==(0xc0003000ULL|3));
    assert(pt[2]&RELIEFNT_PAGE_PCD); paging_release_mmio(0xc0002000,4096); assert(pt[2]==(0xc0002000ULL|3));
    assert(shootdowns>=4); assert(!paging_acquire_mmio(0xc0002001,4096));
    assert(!paging_acquire_mmio(UINT64_MAX-4095,8192)); puts("mmio_exact_uc_nx_refcounts PASS");
}
static void mmio_large_aperture_is_precise_and_owned(void) {
    const uint64_t base=0xc0000000ULL, bytes=128ULL*1024*1024;
    const uint64_t fifo=0xf0000000ULL;
    mm_fixture_init(0x30000000,128,0,0); nx_enabled=true;
    for(unsigned i=0;i<64;i++)kernel_pd[3][i]=(base+(uint64_t)i*0x200000)|0x83;
    kernel_pd[2][511]=0xbfe00000ULL|0x83;
    kernel_pd[3][384]=fifo|0x83;
    uint64_t variant_phys=base+0x123000, neighbor_phys=base-4096;
    uint64_t *variant=mmio_leaf(variant_phys), *neighbor=mmio_leaf(neighbor_phys);
    assert(variant && neighbor);
    *variant=variant_phys|0x800000000000000bULL;
    *neighbor=neighbor_phys|0x8000000000000001ULL;

    assert(paging_acquire_mmio(base,bytes));
    uint64_t *first=mmio_leaf(base), *last=mmio_leaf(base+bytes-4096);
    assert(first && last);
    assert(*first==(base|0x800000000000001bULL));
    assert(*last==((base+bytes-4096)|0x800000000000001bULL));
    assert(*variant==(variant_phys|0x800000000000001bULL));
    assert(*neighbor==(neighbor_phys|0x8000000000000001ULL));

    assert(paging_acquire_mmio(fifo,4096));
    uint64_t *fifo_pte=mmio_leaf(fifo); assert(fifo_pte);
    assert(*fifo_pte==(fifo|0x800000000000001bULL));
    paging_release_mmio(fifo,4096);
    assert(*fifo_pte==(fifo|3));

    assert(paging_acquire_mmio(base+0x2000,0x6000));
    paging_release_mmio(base,bytes);
    assert(first[2]==((base+0x2000)|0x800000000000001bULL));
    assert(first[8]==((base+0x8000)|3));
    assert(*variant==(variant_phys|0x800000000000000bULL));
    assert(*neighbor==(neighbor_phys|0x8000000000000001ULL));
    paging_release_mmio(base+0x2000,0x6000);
    assert(first[2]==((base+0x2000)|3));
    assert(first[8]==((base+0x8000)|3));
    assert(*last==((base+bytes-4096)|3));
    puts("mmio_128MiB_aperture_fifo_overlap_and_neighbors PASS");
}
static void mmio_large_page_pat_is_converted_and_restored(void) {
    const uint64_t base=0xdc000000ULL;
    const uint64_t pat=RELIEFNT_PAGE_PAT;
    const uint64_t nx=0x8000000000000000ULL;
    mm_fixture_init(0x30000000,8,0,0); nx_enabled=true;
    kernel_pd[3][224]=base|0x83|(1ULL<<12)|nx;

    uint64_t *pat_page=mmio_leaf(base+4096); assert(pat_page);
    uint64_t pat_original=*pat_page;
    assert(!paging_mmio_writeback_nx(base+4096,4096));
    assert(*pat_page==pat_original);
    assert(paging_acquire_mmio(base+4096,4096));
    assert(!(kernel_pd[3][224]&0x80));
    uint64_t *pt=(void *)(uintptr_t)(kernel_pd[3][224]&RELIEFNT_PHYS_ADDR_MASK);
    assert(pt[0]==(base|nx|0x83|pat));
    assert(pt[1]==(base+4096|nx|0x1b));
    assert(pt[2]==(base+8192|nx|0x83|pat));

    paging_release_mmio(base+4096,4096);
    assert(pt[0]==(base|nx|0x83|pat));
    assert(pt[1]==(base+4096|nx|0x83|pat));
    assert(pt[2]==(base+8192|nx|0x83|pat));
    puts("mmio_large_page_pat_conversion_uc_and_restore PASS");
}
static void mmio_writeback_policy_is_default_only_and_exclusive(void) {
    const uint64_t base=0xe0000000ULL;
    const uint64_t wb=base+2*4096;
    mm_fixture_init(0x30000000,32,0,0); nx_enabled=true;
    kernel_pd[3][256]=base|0x83;
    uint64_t *first=mmio_leaf(base+4096), *wb_page=mmio_leaf(wb);
    uint64_t *after=mmio_leaf(wb+4096), *uc_page=mmio_leaf(base+6*4096);
    assert(first && wb_page && after && uc_page);
    const uint64_t original_first=*first, original_wb=*wb_page,
                   original_after=*after, original_uc=*uc_page;
    unsigned before_shootdowns=shootdowns;

    nx_enabled=false;
    assert(!paging_mmio_writeback_nx(wb,4096));
    assert(*wb_page==original_wb);
    nx_enabled=true;
    for (uint64_t cache=RELIEFNT_PAGE_PWT; cache<=RELIEFNT_PAGE_PAT; cache<<=1) {
        if (cache != RELIEFNT_PAGE_PWT && cache != RELIEFNT_PAGE_PCD &&
            cache != RELIEFNT_PAGE_PAT) continue;
        *wb_page=wb|3|cache;
        uint64_t nondefault=*wb_page;
        assert(!paging_mmio_writeback_nx(wb,4096));
        assert(*wb_page==nondefault);
    }
    *wb_page=original_wb;

    assert(paging_mmio_writeback_nx(wb,4096));
    assert(*wb_page==(wb|3|RELIEFNT_PAGE_NOEXEC));
    assert(paging_mmio_writeback_nx(wb,4096));
    assert(*wb_page==(wb|3|RELIEFNT_PAGE_NOEXEC));
    assert(shootdowns>before_shootdowns);

    /* UC may touch either exact page neighbor, but no page-edge or partial
     * range that rounds into the permanent WB extent. */
    assert(paging_mmio_uncached(base+4096,4096));
    assert(paging_mmio_uncached(wb+4096,4096));
    assert(!paging_mmio_uncached(wb+1,1));
    assert(!paging_mmio_uncached(wb,4096));
    assert(!paging_acquire_mmio(wb,4096));
    assert(!paging_mmio_uncached(wb-4096,8192));
    assert(*first==(base+4096|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|
                    RELIEFNT_PAGE_NOEXEC|3));
    assert(*after==(wb+4096|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|
                    RELIEFNT_PAGE_NOEXEC|3));
    assert(*wb_page==(wb|3|RELIEFNT_PAGE_NOEXEC));

    assert(paging_mmio_uncached(base+6*4096,4096));
    assert(!paging_mmio_writeback_nx(base+6*4096,4096));
    assert(*uc_page==(base+6*4096|3|RELIEFNT_PAGE_PWT|
                      RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_NOEXEC));
    uint64_t lease_page_phys=base+7*4096;
    assert(paging_acquire_mmio(lease_page_phys,4096));
    uint64_t *lease_page=mmio_leaf(lease_page_phys); assert(lease_page);
    uint64_t leased_uc=*lease_page;
    assert(!paging_mmio_writeback_nx(lease_page_phys,4096));
    assert(*lease_page==leased_uc);
    paging_release_mmio(lease_page_phys,4096);
    assert(*lease_page==(lease_page_phys|3));
    assert(*wb_page==(wb|3|RELIEFNT_PAGE_NOEXEC));
    puts("mmio_writeback_default_cache_nx_and_bidirectional_policy_conflicts PASS");
    (void)original_first; (void)original_after; (void)original_uc;
}
static void mmio_writeback_registry_exhaustion_is_atomic(void) {
    const uint64_t base=0xd8000000ULL;
    mm_fixture_init(0x30000000,8,0,0); nx_enabled=true;
    kernel_pd[3][192]=base|0x83;
    for (unsigned i=0;i<257;i++) {
        uint64_t phys=base+(uint64_t)i*4096;
        uint64_t *pte=mmio_leaf(phys); assert(pte);
        *pte=phys|3|((i&1)?RELIEFNT_PAGE_DEVICE:0);
    }
    assert(!paging_mmio_writeback_nx(base,257ULL*4096));
    for (unsigned i=0;i<257;i++) {
        uint64_t phys=base+(uint64_t)i*4096;
        assert(*mmio_leaf(phys)==(phys|3|((i&1)?RELIEFNT_PAGE_DEVICE:0)));
    }
    puts("mmio_writeback_registry_exhaustion_no_partial_flags_or_ownership PASS");
}
static void mmio_writeback_rejects_holes_aliases_and_user_leaves(void) {
    const uint64_t base=0xd9000000ULL;
    mm_fixture_init(0x30000000,8,0,0); nx_enabled=true;
    kernel_pd[3][200]=base|0x83;
    uint64_t *first=mmio_leaf(base), *second=mmio_leaf(base+4096);
    assert(first && second);
    uint64_t original_first=*first, original_second=*second;
    *second=0;
    assert(!paging_mmio_writeback_nx(base,8192));
    assert(*first==original_first && *second==0);
    *second=(base+0x3000)|3;
    uint64_t alias=*second;
    assert(!paging_mmio_writeback_nx(base,8192));
    assert(*first==original_first && *second==alias);
    *second=(base+4096)|3|RELIEFNT_PAGE_USER;
    uint64_t user=*second;
    assert(!paging_mmio_writeback_nx(base,8192));
    assert(*first==original_first && *second==user);
    *second=original_second;
    puts("mmio_writeback_rejects_hole_alias_and_user_leaf_without_partial_change PASS");
}
static void mmio_writeback_second_large_page_split_oom_is_atomic(void) {
    const uint64_t first_large=0xe0000000ULL;
    const uint64_t second_large=first_large+0x200000ULL;
    const uint64_t first_page=second_large-4096;
    const uint32_t first_index=(uint32_t)((first_page>>21)&511u);
    const uint32_t second_index=first_index+1;
    mm_fixture_init(0x30000000,1,0,0); nx_enabled=true;
    kernel_pd[3][first_index]=first_large|0x83;
    kernel_pd[3][second_index]=second_large|0x83;

    /* One free page splits the first 2 MiB mapping; the second split then
     * fails. The documented preparatory split may remain, but WB attributes
     * and registry ownership must stay wholly unpublished. */
    assert(!paging_mmio_writeback_nx(first_page,8192));
    assert(!(kernel_pd[3][first_index]&0x80));
    uint64_t *first_pt=(uint64_t *)(uintptr_t)
        (kernel_pd[3][first_index]&RELIEFNT_PHYS_ADDR_MASK);
    assert(first_pt[511]==(first_page|3));
    assert(kernel_pd[3][second_index]==(second_large|0x83));

    /* A conflicting UC claim on the first half would reveal a partial WB
     * registry entry even if the PTE itself had not been changed. */
    assert(paging_mmio_uncached(first_page,4096));
    assert(first_pt[511]==(first_page|3|RELIEFNT_PAGE_PWT|
                           RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_NOEXEC));
    puts("mmio_writeback_second_2MiB_split_oom_no_partial_policy_or_claim PASS");
}
static void pci_vmware_svga_bar2_identity_size_and_restore(void) {
    struct pci_device d={.bus=0,.slot=0,.function=0,
                         .vendor_id=0x15ad,.device_id=0x0405};
    uint32_t saved_id=cfg[0],saved_command=cfg[1],saved_bar0=cfg[0x10/4],
             saved_bar1=cfg[0x14/4],saved_bar2=cfg[0x18/4],
             saved_bar3=cfg[0x1c/4],saved_mask2=bar_masks[0x18/4],
             saved_mask3=bar_masks[0x1c/4],saved_mask0=bar_masks[0x10/4],
             saved_mask1=bar_masks[0x14/4];
    cfg[0]=0x040515ad; cfg[1]=0x00000007;
    cfg[0x10/4]=0xc0000000; cfg[0x14/4]=0;
    cfg[0x18/4]=0xf0000000; cfg[0x1c/4]=0;
    bar_masks[0x10/4]=0; bar_masks[0x14/4]=0;
    bar_masks[0x18/4]=0xffff0000; bar_masks[0x1c/4]=0;
    assert(pci_device_bar_contains(&d,2,0xf0000000,0x10000));
    assert(pci_device_bar_contains(&d,2,0xf0001000,0xf000));
    assert(!pci_device_bar_contains(&d,2,0xf000f000,0x2000));
    assert(cfg[1]==0x00000007 && cfg[0x18/4]==0xf0000000 && cfg[0x1c/4]==0);
    struct pci_device wrong=d; wrong.device_id=0x0406;
    assert(!pci_device_bar_contains(&wrong,2,0xf0000000,4096));
    cfg[0x14/4]=0x00000004; /* BAR2 is the high dword, not an independent BAR. */
    assert(!pci_device_bar_contains(&d,2,0xf0000000,4096));
    cfg[0x14/4]=0;
    assert(!pci_device_bar_contains(&d,6,0xf0000000,4096));
    cfg[0x18/4]=0xf0000001;
    assert(!pci_device_bar_contains(&d,2,0xf0000000,4096));
    cfg[0x18/4]=0xf0000002;
    assert(!pci_device_bar_contains(&d,2,0xf0000000,4096));

    cfg[0x18/4]=0x00000004; cfg[0x1c/4]=1;
    bar_masks[0x18/4]=0xffff0004; bar_masks[0x1c/4]=0xffffffff;
    assert(pci_device_bar_contains(&d,2,0x100000000ULL,0x10000));
    assert(!pci_device_bar_contains(&d,2,0x10000f000ULL,0x2000));
    assert(cfg[0x18/4]==0x00000004 && cfg[0x1c/4]==1 && cfg[1]==0x00000007);

    cfg[0]=saved_id; cfg[1]=saved_command; cfg[0x10/4]=saved_bar0;
    cfg[0x14/4]=saved_bar1; cfg[0x18/4]=saved_bar2;
    cfg[0x1c/4]=saved_bar3; bar_masks[0x18/4]=saved_mask2;
    bar_masks[0x1c/4]=saved_mask3; bar_masks[0x10/4]=saved_mask0;
    bar_masks[0x14/4]=saved_mask1;
    puts("pci_exact_device_BAR2_identity_size_32_64_and_restore PASS");
}
static void mmio_exact_lease_capacity_rejects_atomically(void) {
    const uint64_t base=0xd4000000ULL;
    const uint64_t original=0x3ULL;
    const uint64_t uc=0x800000000000001bULL;
    mm_fixture_init(0x30000000,8,0,0); nx_enabled=true;
    kernel_pd[3][160]=base|0x83;

    for(unsigned i=0;i<128;i++)
        assert(paging_acquire_mmio(base+(uint64_t)i*4096,4096));
    uint64_t *pt=(void *)(uintptr_t)(kernel_pd[3][160]&RELIEFNT_PHYS_ADDR_MASK);
    for(unsigned i=0;i<128;i++)assert(pt[i]==(base+(uint64_t)i*4096|uc));

    assert(!paging_acquire_mmio(base+128ULL*4096,4096));
    assert(pt[128]==(base+128ULL*4096|original));
    for(unsigned i=0;i<128;i++)assert(pt[i]==(base+(uint64_t)i*4096|uc));

    for(unsigned i=0;i<128;i++)
        paging_release_mmio(base+(uint64_t)i*4096,4096);
    for(unsigned i=0;i<129;i++)assert(pt[i]==(base+(uint64_t)i*4096|original));
    puts("mmio_exact_lease_capacity_rejection_and_releases PASS");
}
static void mmio_interval_capacity_failure_rolls_back(void) {
    const uint64_t base=0xd0000000ULL;
    mm_fixture_init(0x30000000,8,0,0); nx_enabled=true;
    kernel_pd[3][128]=base|0x83;
    for(unsigned i=0;i<257;i++) {
        uint64_t phys=base+(uint64_t)i*4096;
        uint64_t *pte=mmio_leaf(phys); assert(pte);
        *pte=phys|((i&1)?0xbULL:3ULL);
    }
    assert(!paging_acquire_mmio(base,257ULL*4096));
    for(unsigned i=0;i<257;i++) {
        uint64_t phys=base+(uint64_t)i*4096;
        uint64_t *pte=mmio_leaf(phys); assert(pte);
        assert(*pte==(phys|((i&1)?0xbULL:3ULL)));
    }
    assert(paging_acquire_mmio(base,4096));
    paging_release_mmio(base,4096);
    assert(*(uint64_t *)(uintptr_t)mmio_leaf(base)==(base|3));
    puts("mmio_interval_capacity_failure_is_atomic PASS");
}
static void mmio_permanent_claims_are_idempotent(void) {
    const uint64_t base=0xc8000000ULL, phys=base+4096;
    mm_fixture_init(0x30000000,4,0,0); nx_enabled=true;
    kernel_pd[3][64]=base|0x83;
    uint64_t *pte=mmio_leaf(phys); assert(pte);
    *pte=phys|0x800000000000000bULL;
    assert(paging_mmio_uncached(phys,1));
    for(unsigned i=0;i<100;i++)assert(paging_mmio_uncached(phys,1));
    assert(*pte==(phys|0x800000000000001bULL));
    assert(paging_acquire_mmio(phys,4096));
    paging_release_mmio(phys,4096);
    assert(*pte==(phys|0x800000000000001bULL));
    puts("mmio_permanent_claims_are_idempotent_and_persistent PASS");
}
static void mmio_validation_and_page_table_oom_roll_back(void) {
    const uint64_t base=0xda000000ULL;
    mm_fixture_init(0x30000000,1,0,0);
    nx_enabled=false;
    kernel_pd[3][208]=base|0x83;
    assert(!paging_acquire_mmio(base,4096));
    assert(!paging_mmio_uncached(base,4096));
    nx_enabled=true;
    assert(!paging_acquire_mmio(RELIEFNT_KERNEL_DIRECT_MAP_SIZE-4096,8192));
    assert(!paging_acquire_mmio(UINT64_MAX-4095,8192));
    assert(!paging_mmio_uncached(UINT64_MAX-7,16));
    assert(!paging_acquire_mmio(base+1,4096));

    kernel_pd[3][209]=(base+0x200000)|0x83;
    assert(!paging_mmio_writeback_nx(base,0x400000));
    assert(!paging_acquire_mmio(base,0x400000));
    assert(!(kernel_pd[3][208]&0x80));
    uint64_t *first_pt=(uint64_t *)(uintptr_t)(kernel_pd[3][208]&RELIEFNT_PHYS_ADDR_MASK);
    assert(first_pt[0]==(base|3));
    assert(first_pt[511]==((base+0x1ff000)|3));
    assert(kernel_pd[3][209]==((base+0x200000)|0x83));
    assert(paging_acquire_mmio(base,4096));
    paging_release_mmio(base,4096);
    assert(first_pt[0]==(base|3));
    mm_fixture_init(0x30000000,8,0,0);
    kernel_pd[3][0]=0xc0000000ULL|0x83;
    kernel_pd[3][208]=base|0x83;
    kernel_pd[3][209]=(base+0x200000)|0x83;
    puts("mmio_nx_bounds_alignment_overflow_and_page_table_oom PASS");
}
static void module_resource_failure_rolls_back(void) {
    mm_fixture_init(0x50000000,300,0,0);
    for(unsigned i=0;i<DRIVER_RESOURCE_MAX;i++)assert(driver_alloc_dma_owned(9,1,0xffffffff));
    uint32_t available=free_page_count;assert(!driver_alloc_dma_owned(9,1,0xffffffff));assert(free_page_count==available);
    assert(!driver_map_mmio_owned(9,0xc0000000,4096));for(unsigned i=0;i<PCI_MMIO_MAX;i++)assert(!pci_mappings[i].refs);
    struct reliefos_driver_pci_device d={0};unsigned count=0;uint32_t handle;
    assert(driver_request_pci_irq_owned(9,&d,handler,&count,&handle)==-28);
    assert(!(cfg[0x40/4]&(1u<<16)));for(unsigned i=0;i<IRQ_COUNT;i++)assert(!pci_irqs[i].live && !pci_irqs[i].retiring);
    driver_resources_release(9,false);assert(free_page_count==300);
    for(unsigned i=0;i<DRIVER_RESOURCE_MAX;i++)assert(!driver_resources[i].live);
    void *lease=driver_map_mmio_owned(9,0xc0000000,4096);assert(lease);
    assert(!driver_map_mmio_owned(10,0xc0000000,8192));
    driver_unmap_mmio(lease,4096);
    puts("module_resource_registration_failure_no_leaks PASS\nMMIO_owner_is_exclusive PASS");
}
static uint32_t pci_mmio_reference_count(void) {
    uint32_t refs=0;
    for(unsigned i=0;i<PCI_MMIO_MAX;i++)refs+=pci_mappings[i].refs;
    return refs;
}
static uint32_t owner_resource_count(uint32_t owner) {
    uint32_t count=0;
    for(unsigned i=0;i<DRIVER_RESOURCE_MAX;i++)
        if(driver_resources[i].live && driver_resources[i].owner==owner)++count;
    return count;
}
static void automatic_mmio_cleanup_releases_provider(void) {
    mm_fixture_init(0x30000000,32,0,0);nx_enabled=true;
    kernel_pd[3][0]=0xc0000000ULL|0x83;
    void *first=driver_map_mmio_owned(11,0xc0000000,4096);
    void *second=driver_map_mmio_owned(11,0xc0000000,4096);
    assert(first && second==first && pci_mmio_reference_count()==2);
    uint64_t *pt=(void*)(uintptr_t)(kernel_pd[3][0]&RELIEFNT_PHYS_ADDR_MASK);
    const uint64_t original=0xc0000000ULL|3;
    assert(pt[0]==(original|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_NOEXEC));
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    driver_resources_release(11,false);
    kernel_execution_unlock_irqrestore(flags);
    assert(!owner_resource_count(11));
    assert(!pci_mmio_reference_count());
    assert(pt[0]==original);

    void *explicit=driver_map_mmio_owned(12,0xc0000000,4096);
    assert(explicit && pci_mmio_reference_count()==1);
    driver_unmap_mmio(explicit,4096);
    assert(!pci_mmio_reference_count() && pt[0]==original);
    kernel_execution_lock_irqsave(&flags);
    driver_resources_release(12,false);
    kernel_execution_unlock_irqrestore(flags);
    assert(!owner_resource_count(12) && !pci_mmio_reference_count());
    assert(pt[0]==original);
    puts("automatic_mmio_owner_cleanup_releases_provider_and_restores_pte PASS");
}
static void retired_msi_cannot_reach_new_owner(void) {
    struct reliefos_driver_pci_device d={0};unsigned old_count=0,new_count=0;
    uint32_t old_handle,new_handle;
    assert(!pci_request_irq(&d,handler,&old_count,&old_handle));
    uint32_t old_vector=0x50+(old_handle&255);
    pci_free_irq(old_handle);
    assert(!pci_request_irq(&d,handler,&new_count,&new_handle));
    assert((new_handle&255)!=(old_handle&255));
    /* This interrupt was pending before free, but enters dispatch only after
     * the new owner has registered. Software handle generations cannot tag it. */
    pci_irq_dispatch(old_vector);
    assert(old_count==0 && new_count==0);
    pci_irq_dispatch(0x50+(new_handle&255));assert(new_count==1);
    pci_free_irq(old_handle); /* Stale free cannot disable the current owner. */
    assert(cfg[0x40/4]&(1u<<16));
    pci_free_irq(new_handle);
    unsigned allocations=0;
    while(!pci_request_irq(&d,handler,&new_count,&new_handle)){
        assert(++allocations<=IRQ_COUNT);pci_free_irq(new_handle);
    }
    assert(pci_request_irq(&d,handler,&new_count,&new_handle)==-ENOSPC);
    assert(!(cfg[0x40/4]&(1u<<16)));
    for(unsigned v=0x50;v<=0x5f;v++)pci_irq_dispatch(v);
    assert(old_count==0 && new_count==1);
    puts("delayed_old_MSI_never_reaches_new_owner_bounded_exhaustion PASS");
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    _Static_assert(offsetof(struct reliefos_driver_kernel_api,register_audio)==160,"v1 prefix changed");
    _Static_assert(offsetof(struct reliefos_driver_kernel_api,pci_enumerate)==168,"extension must follow v1");
    _Static_assert(sizeof(struct reliefos_driver_audio_ops)==32,"v1 ops changed");
    cfg[0]=0x26688086;cfg[2]=0x04030001;cfg[3]=0x00800000;
    cfg2[0]=0x12348086;cfg2[2]=0x02000001;
    struct reliefos_driver_pci_device enum_device;
    assert(!pci_enumerate(0,&enum_device) && enum_device.function==0 && enum_device.subclass==3);
    assert(!pci_enumerate(1,&enum_device) && enum_device.function==1);
    assert(pci_enumerate(2,&enum_device)==-19); puts("pci_enumerates_multifunction PASS");
    mmio_pages_are_precise_and_owned();
    mmio_large_aperture_is_precise_and_owned();
    mmio_large_page_pat_is_converted_and_restored();
    mmio_writeback_policy_is_default_only_and_exclusive();
    mmio_writeback_registry_exhaustion_is_atomic();
    mmio_writeback_rejects_holes_aliases_and_user_leaves();
    mmio_writeback_second_large_page_split_oom_is_atomic();
    mmio_exact_lease_capacity_rejects_atomically();
    mmio_interval_capacity_failure_rolls_back();
    mmio_validation_and_page_table_oom_roll_back();
    kernel_pd[3][0]=0xc0000000ULL|0x83;
    cfg[0x10/4]=0xc0000004;cfg[0x14/4]=0;bar_masks[0x10/4]=0xffffc004;
    void *mmio=pci_map_mmio(0xc0000000,8192); assert(mmio);
    assert(cfg[0x10/4]==0xc0000004 && cfg[0x14/4]==0);
    assert(!pci_map_mmio(0xc0000000,32768));
    assert(!pci_map_mmio(0xc0001000,4096));
    pci_unmap_mmio(mmio,8192); for(unsigned i=0;i<PCI_MMIO_MAX;i++)assert(!pci_mappings[i].refs);
    puts("pci_bar_range_and_mapping_ledger PASS");
    pci_vmware_svga_bar2_identity_size_and_restore();
    puts("module_v1_prefix_is_unchanged PASS");
    dma_mask_is_enforced(); puts("dma_mask_is_enforced PASS");
    struct reliefos_driver_pci_device d={0}; uint32_t h; unsigned count=0;
    cfg[1]=1u<<20; cfg[0x34/4]=0x40; cfg[0x40/4]=0x4001;
    assert(pci_request_irq(&d,handler,&count,&h)==-95); puts("capability_cycle_terminates PASS");
    cfg[0x40/4]=0x01800005;cfg[0x50/4]=0xffffffff; assert(!pci_request_irq(&d,handler,&count,&h));
    assert(!(cfg[0x50/4]&1));
    assert(cfg[0x44/4]==0xfee03000 && cfg[0x48/4]==0); assert((cfg[0x4c/4]&0xffff)>=0x50);
    pci_irq_dispatch(cfg[0x4c/4]&0xff); assert(count==1); pci_free_irq(h);
    assert(!(cfg[0x40/4]&(1u<<16))); pci_irq_dispatch(cfg[0x4c/4]&0xff); assert(count==1);
    puts("msi_64_address_and_disable PASS");msi_disable_synchronizes_inflight();module_resource_failure_rolls_back();
    automatic_mmio_cleanup_releases_provider();retired_msi_cannot_reach_new_owner();
    mmio_permanent_claims_are_idempotent();return 0;
}
