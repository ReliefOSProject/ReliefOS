/* Real ET_REL input to the production manager loader; no unresolved symbols. */
#include <reliefos/driver.h>
#include <reliefos/audio_abi.h>
static const struct reliefos_driver_kernel_api *api;
static uint32_t card;
static int caps(void *p,uint32_t d,enum audio_direction dir,struct audio_caps *out)
{ (void)p;(void)d;(void)dir;(void)out;return -19; }
static int open_stream(void *p,uint32_t d,enum audio_direction dir,struct audio_hw_stream *s)
{ (void)p;(void)d;(void)dir;(void)s;return -19; }
static int prepare(void *p,struct audio_hw_stream *s,const struct audio_params *a,const struct audio_dma *d)
{ (void)p;(void)s;(void)a;(void)d;return -19; }
static int trigger(void *p,struct audio_hw_stream *s,enum audio_trigger t)
{ (void)p;(void)s;(void)t;return 0; }
static int pointer(void *p,struct audio_hw_stream *s,uint64_t *n)
{ (void)p;(void)s;*n=0;return 0; }
static void close_stream(void *p,struct audio_hw_stream *s) { (void)p;(void)s; }
static uint32_t count(void *p) { (void)p;return 0; }
static void irq(void *p) { (void)p; }
static const struct audio_card_ops ops={.version=AUDIO_CARD_OPS_VERSION,.size=sizeof(ops),
    .pcm_caps=caps,.open=open_stream,.prepare=prepare,.trigger=trigger,.pointer=pointer,
    .close=close_stream,.control_count=count};
static struct reliefos_audio_state legacy_state;
static int legacy_ready(void) { return 1; }
static int legacy_configure(const struct reliefos_audio_format *format)
{
    if(!format || format->channels!=2 || format->bits_per_sample!=16 ||
       format->sample_rate<8000 || format->sample_rate>48000)return -22;
    legacy_state.sample_rate=format->sample_rate;return 0;
}
static long legacy_write(const void *data,uint32_t length,uint32_t *status)
{ (void)data;legacy_state.queued_bytes+=length;*status=0;return length; }
static void legacy_get_state(struct reliefos_audio_state *out) { *out=legacy_state; }
static const struct reliefos_driver_audio_ops legacy_ops={.is_ready=legacy_ready,
    .configure=legacy_configure,.write=legacy_write,.get_state=legacy_get_state};
static int init(const struct reliefos_driver_kernel_api *a)
{
    api=a;
    if(api->pci_read32(0,0,0,0x58)&4){
        legacy_state=(struct reliefos_audio_state){.present=1,.active=1,.sample_rate=48000,
                                                  .channels=2,.bits_per_sample=16};
        return api->register_audio(&legacy_ops);
    }
    for(unsigned n=0;n<8;n++)if(!api->alloc_dma(1,0xffffffff))return -12;
    if(!api->map_mmio(0xc0000000,4096))return -12;
    struct reliefos_driver_pci_device dev={0};uint32_t handle;
    if(api->request_pci_irq(&dev,irq,0,&handle))return -5;
    const struct audio_card_identity id={.id="cleanup",.name="cleanup probe"};
    if(api->audio_register_card(&id,&ops,0,&card))return -5;
    return (api->pci_read32(0,0,0,0x58)&1)?-5:0;
}
static void fini(void)
{
    /* Read the append-only tail by its frozen byte offset, including on the
     * pre-extension RED build. The successful branch deliberately leaves
     * resources for the manager's real automatic owner cleanup. */
    if(!(api->pci_read32(0,0,0,0x58)&2))return;
    const unsigned offset=RELIEFOS_DRIVER_AUDIO_API_SIZE;
    if(api->struct_size>=offset+sizeof(void (*)(int))){
        void (*report)(int)=*(void (*const *)(int))((const char*)api+offset);
        report(-16);
        report(-5); /* First negative result wins. */
    }
}
const struct reliefos_driver_module reliefos_driver_module={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_AUDIO,
    .name="cleanup-probe",.version=1,.init=init,.fini=fini};
