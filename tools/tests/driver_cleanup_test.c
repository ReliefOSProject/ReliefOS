#define main resources_fixture_main
#include "pci_audio_resources_test.c"
#undef main
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/driver_manager_phase.c"
static unsigned allocation_call,fail_allocation;
static uint64_t fixture_alloc_dma_owned(uint32_t owner,uint32_t pages,uint64_t mask)
{
    if(++allocation_call==fail_allocation)return 0;
    return driver_alloc_dma_owned(owner,pages,mask);
}
#define driver_alloc_dma_owned fixture_alloc_dma_owned
#include "../../kernel/reliefnt/kernel/reliefnt/driver_manager.c"
#undef driver_alloc_dma_owned
static unsigned char *module_file;
static size_t module_size;
static char diagnosis[256];
void console_printf(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);vsnprintf(diagnosis,sizeof diagnosis,fmt,ap);va_end(ap); }
void console_write(const char *text) { (void)text; }
void panic(const char *text) { fprintf(stderr,"panic: %s\n",text);abort(); }
uint8_t x86_64_inb(uint16_t port) { (void)port;return 0x20; }
void x86_64_outb(uint8_t value,uint16_t port) { (void)value;(void)port; }
void input_push_mouse(int32_t x,int32_t y,int32_t dx,int32_t dy,uint8_t buttons)
{ (void)x;(void)y;(void)dx;(void)dy;(void)buttons; }
void input_push_mouse_wheel(int32_t x,int32_t y,int32_t wheel,uint8_t buttons)
{ (void)x;(void)y;(void)wheel;(void)buttons; }
const struct framebuffer *framebuffer_get(void) { return NULL; }
void net_driver_detached(void) { }
uint64_t time_ticks(void) { return 100; }
void time_sleep_ms(uint64_t ms) { (void)ms; }
int storage_read_file(const char *path,const void **data,size_t *length)
{
    assert(!strcmp(path,"/usr/lib/reliefos/drivers/cleanup.drv") ||
           !strcmp(path,"/usr/lib/reliefos/drivers/renamed.drv"));
    uint32_t pages=(module_size+4095)/4096;
    uint64_t p=mm_alloc_pages(pages);if(!p)return -12;
    memcpy((void*)(uintptr_t)p,module_file,module_size);*data=(void*)(uintptr_t)p;*length=module_size;return 0;
}
int storage_write_file(const char *path,const void *data,uint32_t len)
{ (void)path;(void)data;(void)len;return -5; }
int storage_list_dir(const char *path,struct reliefos_dir_entry *e,uint32_t cap,uint32_t *n)
{ (void)path;(void)e;(void)cap;*n=0;return 0; }
static void transaction_begin(uint64_t *flags)
{ kernel_execution_lock_irqsave(flags);assert(!driver_manager_phase_try_enter()); }
static void transaction_end(uint64_t flags)
{ driver_manager_phase_leave();kernel_execution_unlock_irqrestore(flags); }
static void setup(unsigned mode,unsigned fail)
{
    assert(!owner_resource_count(0));
    mm_fixture_init(0x30000000,512,0,0);
    /* Only this owned fixture mapping executes the loaded test ET_REL image. */
    assert(!mprotect((void*)0x30000000,512*4096,PROT_READ|PROT_WRITE|PROT_EXEC));
    nx_enabled=true;kernel_pd[3][0]=0xc0000000ULL|0x83;
    memset(cfg,0,sizeof cfg);memset(cfg2,0,sizeof cfg2);
    cfg[0]=0x26688086;cfg[2]=0x04030001;cfg[0x10/4]=0xc0000004;
    bar_masks[0x10/4]=0xffffc004;cfg[1]=1u<<20;cfg[0x34/4]=0x40;
    cfg[0x40/4]=0x01800005;cfg[0x58/4]=mode;
    allocation_call=0;fail_allocation=fail;diagnosis[0]=0;
    memset(driver_slots,0,sizeof driver_slots);
    driver_slots[0].info.id=0;strcpy(driver_slots[0].info.file,"cleanup.drv");
}
static void assert_empty(void)
{
    assert(!owner_resource_count(0));assert(!pci_mmio_reference_count());
    assert(free_page_count==512);
    for(unsigned n=0;n<IRQ_COUNT;n++)assert(!pci_irqs[n].live && !pci_irqs[n].retiring);
    for(unsigned n=0;n<CARD_MAX;n++)assert(!cards[n].live);
}
static void retention_retry(unsigned mode)
{
    setup(mode,0);uint64_t flags;transaction_begin(&flags);
    int ret=driver_load_slot(&driver_slots[0]);
    if(!(mode&1)){assert(!ret);ret=driver_unload_slot(&driver_slots[0],1);}
    assert(ret==-16);assert(driver_slots[0].info.state==RELIEFOS_DRIVER_STATE_LOADED);
    assert(driver_slots[0].module && driver_slots[0].image_phys);
    assert(strstr(driver_slots[0].info.error,"retained"));
    assert(strstr(diagnosis,"status=-16"));
    assert(owner_resource_count(0)==9 && pci_mmio_reference_count()==1);
    assert(!(cfg[0x40/4]&(1u<<16)));
    assert(!driver_manager_service_try_pin(0));
    uint64_t *pt=(void*)(uintptr_t)(kernel_pd[3][0]&RELIEFNT_PHYS_ADDR_MASK);
    assert(pt[0]&RELIEFNT_PAGE_PCD);
    unsigned before=free_page_count;
    assert(driver_unload_slot(&driver_slots[0],1)==-16 && free_page_count==before);
    cfg[0x58/4]=0;assert(!driver_unload_slot(&driver_slots[0],1));
    assert(!driver_slots[0].image_phys && !driver_slots[0].module);
    assert(pt[0]==(0xc0000000ULL|3));
    /* The page-table page is retained by paging, not the driver lease. */
    assert(free_page_count==511);assert(!owner_resource_count(0));
    transaction_end(flags);
    puts(mode&1?"actual_ELF_init_rollback_retains_DMA_MMIO_image_and_retry PASS":
                   "actual_manager_unload_retains_DMA_MMIO_image_and_retry PASS");
}
static void renamed_module_rejected_before_init(void)
{
    setup(0,0);uint64_t flags;transaction_begin(&flags);
    assert(!driver_load_slot(&driver_slots[0]));
    driver_slots[1].info.id=1;strcpy(driver_slots[1].info.file,"renamed.drv");
    unsigned allocations=allocation_call,available=free_page_count;
    uint32_t config_before[64];memcpy(config_before,cfg,sizeof cfg);
    assert(driver_load_slot(&driver_slots[1])==-EBUSY);
    assert(allocation_call==allocations && free_page_count==available);
    assert(!memcmp(config_before,cfg,sizeof cfg));
    assert(!owner_resource_count(1) && !driver_slots[1].module && !driver_slots[1].image_phys);
    assert(strstr(driver_slots[1].info.error,"already loaded"));
    assert(driver_slots[0].info.state==RELIEFOS_DRIVER_STATE_LOADED);
    assert(!driver_unload_slot(&driver_slots[0],0));
    assert(!driver_load_slot(&driver_slots[1]));
    assert(!driver_unload_slot(&driver_slots[1],0));
    assert(!owner_resource_count(0) && !owner_resource_count(1));
    transaction_end(flags);
    puts("renamed_actual_ELF_duplicate_rejected_before_init_and_reload_after_unload PASS");
}
static void legacy_backend_generation_and_exclusive_lease(void)
{
    setup(4,0);uint64_t flags;transaction_begin(&flags);
    assert(!driver_load_slot(&driver_slots[0]));
    assert(audio_card_snapshot(0,NULL,NULL)==-ENOENT);
    uint32_t first,second,other,status;struct reliefos_audio_state state;
    assert(!driver_manager_audio_acquire(&first,&state));
    assert(state.channels==2 && state.bits_per_sample==16 && !state.queued_bytes);
    assert(driver_manager_audio_acquire(&other,&state)==-EBUSY);
    uint32_t frame=1;
    assert(driver_manager_audio_write_bound(first,&frame,4,&status)==4 && !status);
    assert(!driver_manager_audio_state_bound(first,&state) && state.queued_bytes==4);
    assert(!driver_unload_slot(&driver_slots[0],1));
    assert(driver_manager_audio_state_bound(first,&state)==-ENODEV && !state.present);
    assert(!driver_load_slot(&driver_slots[0]));
    assert(!driver_manager_audio_acquire(&second,&state) && second!=first);
    assert(driver_manager_audio_write_bound(first,&frame,4,&status)==-ENODEV);
    assert(status==RELIEFOS_AUDIO_STATUS_NO_DEVICE);
    struct reliefos_audio_format format={44100,2,16,0};
    assert(driver_manager_audio_configure_bound(first,&format)==-ENODEV);
    assert(!driver_manager_audio_state_bound(second,&state) && !state.queued_bytes);
    driver_manager_audio_release(first);
    assert(driver_manager_audio_acquire(&other,&state)==-EBUSY);
    driver_manager_audio_release(second);
    assert(!driver_manager_audio_acquire(&other,&state));
    driver_manager_audio_release(other);
    assert(!driver_unload_slot(&driver_slots[0],1));assert_empty();
    transaction_end(flags);
    puts("actual_ET_REL_v1_lease_generation_stale_write_configure_release_and_no_ALSA_card PASS");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    module_size=ftell(f);rewind(f);module_file=malloc(module_size);assert(module_file);
    assert(fread(module_file,1,module_size,f)==module_size);fclose(f);
    legacy_backend_generation_and_exclusive_lease();
    renamed_module_rejected_before_init();retention_retry(2);retention_retry(3);
    for(unsigned n=1;n<=8;n++){
        setup(0,n);uint64_t flags;transaction_begin(&flags);
        assert(driver_load_slot(&driver_slots[0])==-12);assert_empty();transaction_end(flags);
    }
    puts("each_Nth_DMA_failure_actual_init_rollback_provider_reclaim PASS");
    setup(0,0);uint64_t flags;transaction_begin(&flags);
    assert(!driver_load_slot(&driver_slots[0]));assert(!driver_unload_slot(&driver_slots[0],0));
    assert(!owner_resource_count(0) && !pci_mmio_reference_count() && free_page_count==511);
    transaction_end(flags);free(module_file);
    puts("unextended_successful_fini_still_automatically_reclaims_owner_resources PASS");return 0;
}
