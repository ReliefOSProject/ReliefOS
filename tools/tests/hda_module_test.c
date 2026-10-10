/* Real module/core/stream integration. Codec/controller discovery is the
 * hardware boundary; their production algorithms have separate fixtures. */
#define HDA_TESTING 1
#define HDA_STREAM_TESTING 1
#include "audio_fixture.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../../kernel/reliefnt/drivers/hda/hda.h"
static uint8_t registers[2][0x4000];
static uint8_t hda_test_mmio_read8(void *p,uint32_t n){return ((uint8_t*)p)[n];}
static uint16_t hda_test_mmio_read16(void *p,uint32_t n){uint16_t v;memcpy(&v,(char*)p+n,2);return v;}
static uint32_t hda_test_mmio_read32(void *p,uint32_t n){uint32_t v;memcpy(&v,(char*)p+n,4);return v;}
static void hda_test_mmio_write8(void *p,uint32_t n,uint8_t v){((uint8_t*)p)[n]=v;}
static void hda_test_mmio_write16(void *p,uint32_t n,uint16_t v){memcpy((char*)p+n,&v,2);}
static void hda_test_mmio_write32(void *p,uint32_t n,uint32_t v){memcpy((char*)p+n,&v,4);}
uint8_t hda_mmio_read8(const struct hda_controller *c,uint32_t n){return hda_test_mmio_read8(c->mmio,n);}
uint16_t hda_mmio_read16(const struct hda_controller *c,uint32_t n){return hda_test_mmio_read16(c->mmio,n);}
uint32_t hda_mmio_read32(const struct hda_controller *c,uint32_t n){return hda_test_mmio_read32(c->mmio,n);}
void hda_mmio_write8(struct hda_controller *c,uint32_t n,uint8_t v){hda_test_mmio_write8(c->mmio,n,v);}
void hda_mmio_write16(struct hda_controller *c,uint32_t n,uint16_t v){hda_test_mmio_write16(c->mmio,n,v);}
void hda_mmio_write32(struct hda_controller *c,uint32_t n,uint32_t v){hda_test_mmio_write32(c->mmio,n,v);}
static void *hda_test_dma_map(uint64_t p){return (void*)(uintptr_t)p;}
static uint64_t hda_test_irq_save(void){return kernel_irq_save();}
static void hda_test_irq_restore(uint64_t f){kernel_irq_restore(f);}
static void hda_test_before_sleep(const struct hda_controller *c){(void)c;}
/* Preserve the actual core notification implementation separately from the
 * module's API forwarding symbol; the contract header is already included. */
#define audio_period_elapsed core_audio_period_elapsed
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#undef audio_period_elapsed
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#include "../../kernel/reliefnt/kernel/reliefnt/driver_manager_phase.c"
#define memset module_memset
#include "../../kernel/reliefnt/drivers/hda/hda.c"
#undef memset
#include "../../kernel/reliefnt/drivers/hda/stream.c"
#include "../../kernel/reliefnt/drivers/hda/controls.c"

/* The module fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

static struct {uint64_t phys;uint32_t pages;} allocations[128];
static unsigned allocation_count,allocation_calls,fail_nth,card_count;
static uint32_t registered[16];
static struct audio_card_identity identities[16];
static unsigned destroy_stuck;
static int teardown_error;
static unsigned jack_fixture,sense_submits;
static uint64_t fixture_ticks;
static uint32_t senses[2][2];
static uint16_t gains[2][2][2];
/* Format changes preserve amp readback but recreate QEMU's output voice. */
static uint16_t effective_gains[2][2][2];
static unsigned policy_route_calls,policy_route_fail_at,policy_bind_fail_once;
static unsigned policy_route_fail_count,policy_amp_calls,policy_amp_fail_at;
static uint64_t alloc_pages(uint32_t pages)
{
    if(++allocation_calls==fail_nth)return 0;
    void *p=mmap(NULL,(size_t)pages*4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(p!=MAP_FAILED);
    for(unsigned n=0;n<128;n++)if(!allocations[n].phys){
        allocations[n].phys=(uintptr_t)p;allocations[n].pages=pages;allocation_count++;return (uintptr_t)p;
    }
    abort();
}
static void free_pages(uint64_t phys,uint32_t pages)
{
    for(unsigned n=0;n<128;n++)if(allocations[n].phys==phys){
        assert(allocations[n].pages==pages);assert(!munmap((void*)(uintptr_t)phys,(size_t)pages*4096));
        allocations[n].phys=0;allocation_count--;return;
    }
    assert(!"unknown/double free");
}
static uint64_t alloc_dma(uint32_t pages,uint64_t mask)
{ uint64_t p=alloc_pages(pages);assert(!p || p+(uint64_t)pages*4096-1<=mask);return p; }
void *hda_phys_to_direct_map_page(uint64_t phys){return phys && !(phys&4095)?(void*)(uintptr_t)phys:NULL;}
static int enumerate(uint32_t index,struct reliefos_driver_pci_device *out)
{
    if(index>=3)return -ENODEV;
    *out=(struct reliefos_driver_pci_device){.slot=index,.class_code=index?4:2,
        .subclass=index?3:0,.vendor_id=0x8086,.device_id=0x2668};return 0;
}
int hda_controller_init(struct hda_controller *c,const struct reliefos_driver_kernel_api *api,
                         const struct reliefos_driver_pci_device *dev)
{
    *c=(struct hda_controller){.api=api,.dev=*dev,.gcap=0x1100,.codec_mask=3,
        .initialized=1,.transport_mode=HDA_TRANSPORT_IMMEDIATE,
        .mmio=registers[dev->slot-1]};return 0;
}
int hda_controller_destroy(struct hda_controller *c)
{ if(destroy_stuck && c->dev.slot==1)return -EIO;assert(!c->active_streams);memset(c,0,sizeof *c);return 0; }
int hda_controller_stream_acquire(struct hda_controller *c)
{ if(c->transport_poisoned)return -EIO;c->active_streams++;return 0; }
int hda_controller_stream_release(struct hda_controller *c)
{ if(!c->active_streams)return -EINVAL;c->active_streams--;return 0; }
int hda_controller_set_card_id(struct hda_controller *c,uint32_t id){c->card_id=id;return 0;}
int hda_controller_take_disconnect_request(struct hda_controller *c,uint32_t *id)
{
    if(!c || !id || !c->initialized)return -ENODEV;
    if(!c->disconnect_requested || !c->card_id)return -EAGAIN;
    *id=c->card_id;c->disconnect_requested=0;return 0;
}
uint32_t hda_controller_service_budget(struct hda_controller *c,uint32_t budget)
{ (void)c;(void)budget;return 0; }
int hda_codec_probe(struct hda_controller *c,uint8_t cad,struct hda_codec *codec)
{
    uint32_t pages=(sizeof(struct hda_codec_node)*256+4095)/4096;
    uint64_t phys=c->api->alloc_pages(pages);if(!phys)return -ENOMEM;
    *codec=(struct hda_codec){.controller=c,.api=c->api,.nodes=(void*)(uintptr_t)phys,
        .nodes_phys=phys,.nodes_pages=pages,.probed=1,.cad=cad,.vendor_id=0x12345678};
    codec->nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_STEREO,.pin_caps=HDA_PINCAP_OUTPUT,.pin_default=0x00100010};
    codec->nodes[3]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_AUDIO_OUTPUT,
        .widget_caps=HDA_WCAP_STEREO,.pcm_rates=(1u<<6)|(1u<<17)|(cad?((1u<<5)|(1u<<19)):0),.stream_formats=1};
    codec->nodes[5]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_STEREO,.pin_caps=HDA_PINCAP_INPUT,.pin_default=0x00a00020};
    codec->nodes[6]=codec->nodes[3];codec->nodes[6].type=HDA_WIDGET_AUDIO_INPUT;
    if(jack_fixture){
        codec->nodes[4]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
            .widget_caps=HDA_WCAP_STEREO,.pin_caps=HDA_PINCAP_OUTPUT|HDA_PINCAP_PRESENCE,
            .pin_default=0x00200030};
        if(jack_fixture==1 || jack_fixture==3){codec->nodes[3].widget_caps|=HDA_WCAP_OUT_AMP;
            codec->nodes[3].amp_output_caps=(40u<<8)|(jack_fixture==3?(1u<<31):0);}
    }
    return 0;
}
int hda_codec_destroy(struct hda_codec *c)
{ assert(!c->route_groups && !c->active_routes);c->api->free_pages(c->nodes_phys,c->nodes_pages);memset(c,0,sizeof *c);return 0; }
void hda_codec_dump(const struct hda_codec *c){(void)c;}
int hda_find_route(struct hda_codec *c,uint8_t pin,enum hda_route_direction dir,struct hda_route *r)
{
    *r=(struct hda_route){.codec=c,.pin_nid=pin,.converter_nid=dir==HDA_ROUTE_CAPTURE?6:3,
        .direction=dir,.path={pin,dir==HDA_ROUTE_CAPTURE?6:3},.path_count=2,.channel_count=2};return 0;
}
int hda_find_route_group(struct hda_codec *c,uint8_t pin,struct hda_route_group *g)
{
    uint32_t pages=(sizeof(struct hda_route)+4095)/4096;uint64_t p=c->api->alloc_pages(pages);if(!p)return -ENOMEM;
    *g=(struct hda_route_group){.codec=c,.members=(void*)(uintptr_t)p,.members_phys=p,
        .members_pages=pages,.member_count=1,.total_channels=2,.association=1};
    hda_find_route(c,pin,HDA_ROUTE_PLAYBACK,g->members);c->route_groups++;return 0;
}
int hda_apply_route(struct hda_codec *c,struct hda_route *r,bool enable)
{ if(enable && !r->active){c->active_routes++;
    if(r->direction==HDA_ROUTE_PLAYBACK && (c->nodes[3].widget_caps&HDA_WCAP_OUT_AMP)){
        /* Hardware route activation installs its safe gain; module prepare
         * must then restore the user mixer state, also without Auto-Mute. */
        gains[c->controller->dev.slot-1][c->cad][0]=0;
        gains[c->controller->dev.slot-1][c->cad][1]=0;
    }}
  if(!enable && r->active)c->active_routes--;r->active=enable;return 0; }
int hda_apply_route_group(struct hda_codec *c,struct hda_route_group *g,bool enable)
{
    if(jack_fixture==3){
        assert(!(hda_mmio_read32(c->controller,0xa0)&2));
        if(++policy_route_calls==policy_route_fail_at){
            policy_route_fail_at=policy_route_fail_count && --policy_route_fail_count?
                                  policy_route_calls+1:0;
            if(!enable){g->rollback_failed=1;g->rollback_error=-EIO;}
            return -EIO;
        }
        if(enable && !g->active)assert(!c->active_routes);
    }
    g->active=enable;g->rollback_failed=0;g->rollback_error=0;
    return hda_apply_route(c,g->members,enable);
}
int hda_route_group_destroy(struct hda_route_group *g)
{ assert(!g->active);g->codec->route_groups--;g->codec->api->free_pages(g->members_phys,g->members_pages);memset(g,0,sizeof *g);return 0; }
int hda_stream_test_route_bind(struct hda_route_group *g,uint8_t tag,uint16_t fmt)
{
    if (g->members[0].direction == HDA_ROUTE_PLAYBACK) {
        effective_gains[g->codec->controller->dev.slot-1][g->codec->cad][0]=40;
        effective_gains[g->codec->controller->dev.slot-1][g->codec->cad][1]=40;
    }
    if(jack_fixture==3){
        assert(!(hda_mmio_read32(g->codec->controller,0xa0)&2));
        if(tag){assert(g->active);if(policy_bind_fail_once){policy_bind_fail_once=0;return -EIO;}}
    }
    for(unsigned n=0;n<g->member_count;n++){
        /* Production codec binding verifies hardware; stream.c/card owner
         * publishes nonzero metadata after the rest of prepare succeeds. */
        if(!tag){g->members[n].stream_bound=0;g->members[n].stream_tag=0;g->members[n].stream_format=0;}
    }
    return 0;
}
/* Codec verbs/transport are the hardware boundary. Production controls, module
 * publication, core event queues and the execution-phase pump remain real. */
int hda_codec_control_gate_enter(struct hda_codec *c)
{ uint32_t old=0;return __atomic_compare_exchange_n(&c->route_busy,&old,1,false,
    __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)?0:-EBUSY; }
void hda_codec_control_gate_leave(struct hda_codec *c)
{ __atomic_store_n(&c->route_busy,0,__ATOMIC_RELEASE); }
int hda_codec_control_read_amp(struct hda_codec *c,uint8_t nid,bool input,
    uint8_t index,bool right,uint16_t *value)
{ (void)input;(void)index;assert(nid==3);*value=gains[c->controller->dev.slot-1][c->cad][right];return 0; }
int hda_codec_control_write_amp(struct hda_codec *c,uint8_t nid,bool input,
    uint8_t index,bool right,uint16_t value,uint32_t caps)
{ (void)input;(void)index;(void)caps;assert(nid==3);
  if(++policy_amp_calls==policy_amp_fail_at){policy_amp_fail_at=0;return -EIO;}
  gains[c->controller->dev.slot-1][c->cad][right]=value;
  effective_gains[c->controller->dev.slot-1][c->cad][right]=value;return 0; }
int hda_exec_verb(struct hda_controller *c,uint8_t cad,uint8_t nid,uint16_t verb,
    uint16_t payload,bool short_verb,uint32_t *response)
{ (void)c;(void)cad;(void)nid;(void)verb;(void)payload;(void)short_verb;(void)response;
  assert(!"unexpected synchronous verb in polling-only jack fixture");return -EIO; }
int hda_unsolicited_pop_for_codec(struct hda_controller *c,uint8_t cad,
    struct hda_unsolicited_response *event)
{ (void)c;(void)cad;(void)event;return -EAGAIN; }
uint32_t hda_unsolicited_dropped_count(struct hda_controller *c){(void)c;return 0;}
int hda_verb_submit(struct hda_controller *c,uint8_t cad,uint8_t nid,uint16_t verb,
    uint16_t payload,bool short_verb,uint64_t *ticket)
{
    (void)payload;(void)short_verb;assert(nid==4 && verb==0xf09u && cad<2);
    assert(audio_fixture_irq_flags&(1ULL<<9));assert(!execution_depth[0]);
    *ticket=(uint64_t)cad+1;++sense_submits;(void)c;return 0;
}
int hda_verb_poll(struct hda_controller *c,uint64_t ticket,uint32_t *response)
{ assert(ticket>=1 && ticket<=2);*response=senses[c->dev.slot-1][ticket-1];return 0; }
static uint64_t ticks(void){return fixture_ticks;}
static int register_card(const struct audio_card_identity *id,const struct audio_card_ops *ops,
                          void *p,uint32_t *token)
{ int ret=audio_register_card_owned(id,ops,p,1,token);if(!ret){identities[card_count]=*id;registered[card_count++]=*token;}return ret; }
static void console(const char *text){(void)text;}
static void report(int error){if(!teardown_error)teardown_error=error;}
static struct reliefos_driver_kernel_api api={.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_kernel_api),.alloc_pages=alloc_pages,.free_pages=free_pages,
    .alloc_dma=alloc_dma,.free_dma=free_pages,.pci_enumerate=enumerate,.console_write=console,
    .audio_register_card=register_card,.audio_unregister_card=audio_unregister_card,
    .audio_period_elapsed=core_audio_period_elapsed,.audio_control_changed=audio_control_changed,
    .audio_set_service=audio_card_set_service,.report_teardown_failure=report,
    .audio_request_disconnect=audio_request_controller_disconnect,.ticks=ticks};
static void reset(void)
{
    assert(!controllers && !allocation_count);memset(registers,0,sizeof registers);
    allocation_calls=0;card_count=0;teardown_error=0;destroy_stuck=0;
    jack_fixture=0;sense_submits=0;fixture_ticks=0;memset(senses,0,sizeof senses);memset(gains,0,sizeof gains);
    policy_route_calls=policy_route_fail_at=policy_bind_fail_once=0;
    policy_route_fail_count=policy_amp_calls=policy_amp_fail_at=0;
}

static void jack_task_pump(void)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    driver_manager_process_audio_faults();
    assert(!(audio_fixture_irq_flags&(1ULL<<9)));
    kernel_execution_unlock_irqrestore(flags);
}
static void actual_module_jack_publication_and_sparse_events(unsigned mode)
{
    reset();jack_fixture=mode;senses[0][1]=1u<<31;
    assert(!hda_driver_module.init(&api));assert(card_count==4);
    uint32_t count,index=mode==1?2:1;
    assert(!audio_card_control_count(registered[1],&count) && count==index+1);
    struct audio_control_info info;
    assert(!audio_card_control_info(registered[1],index,&info));
    assert(info.id==index && !strcmp(info.name,"Headphone Jack"));
    assert(info.access==(AUDIO_CONTROL_READ|AUDIO_CONTROL_VOLATILE));
    struct audio_control_event_queue queues[2]={{0}};
    for(unsigned n=0;n<2;n++){
        assert(!audio_control_queue_open(registered[n],&queues[n]));
        assert(!audio_control_queue_subscribe(&queues[n],1));
    }
    audio_service_tick();assert(!sense_submits && audio_task_work_pending());
    jack_task_pump();assert(sense_submits==4);
    audio_service_tick();jack_task_pump();
    struct audio_control_value value;
    assert(!audio_card_control_read(registered[1],index,&value) && value.values[0]==1);
    uint32_t element,mask;
    assert(!audio_control_queue_next(&queues[1],&element,&mask,1) && element==index && mask);
    assert(audio_control_queue_next(&queues[0],&element,&mask,1)==-EAGAIN);
    assert(audio_control_queue_next(&queues[1],&element,&mask,1)==-EAGAIN);
    assert(audio_card_control_write(registered[1],index,&value)==-EACCES);
    senses[0][1]=0;fixture_ticks+=10;
    audio_service_tick();jack_task_pump();audio_service_tick();jack_task_pump();
    assert(!audio_card_control_read(registered[1],index,&value) && !value.values[0]);
    assert(!audio_control_queue_next(&queues[1],&element,&mask,1) && element==index);
    for(unsigned n=0;n<2;n++)assert(!audio_control_queue_close(&queues[n]));
    hda_driver_module.fini();assert(!controllers && !allocation_count && !audio_task_work_pending());
    printf("actual_module_jack_mode%u_task_only_sparse_id_and_CAD_isolation PASS\n",mode);
}
static void fatal_controller_hides_all_codecs_without_irq_teardown(void)
{
    reset();assert(!hda_driver_module.init(&api));
    int error=0;
    struct audio_pcm *pcm=audio_pcm_open(registered[0],0,AUDIO_PLAYBACK,&error);
    assert(pcm && !error);uint32_t stream=pcm->stream;
    struct stream_entry *s=find_stream(stream);
    struct hda_stream *hw=s->hw.driver;
    unsigned resources=allocation_count;
    hw->controller->disconnect_requested=1;
    audio_service_tick();
    struct audio_card_identity identity;
    assert(audio_card_identity(registered[0],&identity)==-ENODEV);
    assert(audio_card_identity(registered[1],&identity)==-ENODEV);
    assert(!audio_card_identity(registered[2],&identity));
    assert(!audio_card_identity(registered[3],&identity));
    assert(s->live && !s->detached && hw->controller->active_streams==1);
    assert(pcm->state==AUDIO_PCM_DISCONNECTED);
    assert(allocation_count==resources);
    assert(audio_disconnect_work_pending());
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    audio_pcm_release(pcm);
    assert(pcm->live && !pcm->file_refs && pcm->close_pending);
    assert(!driver_manager_phase_try_enter());
    driver_manager_process_audio_faults();
    assert(s->live && !s->detached && allocation_count==resources);
    driver_manager_phase_leave();
    driver_manager_process_audio_faults();
    assert(!s->live && !pcm->live && !audio_disconnect_work_pending());
    assert(!audio_pcm_release_work_pending());
    assert(!cards[registered[0]&SLOT_MASK].live && !cards[registered[1]&SLOT_MASK].live);
    assert(!audio_card_identity(registered[2],&identity));
    assert(!audio_card_identity(registered[3],&identity));
    assert(!(audio_fixture_irq_flags&(1ULL<<9)));
    kernel_execution_unlock_irqrestore(flags);
    assert(!audio_stream_close(stream));
    hda_driver_module.fini();assert(!controllers && !allocation_count);
    puts("fatal_controller_all_CAD_hidden_other_BDF_live_no_IRQ_teardown PASS");
}

static void actual_module_auto_mute_running_group_transaction(void)
{
    reset();jack_fixture=3;gains[0][0][0]=23;gains[0][0][1]=19;
    assert(!hda_driver_module.init(&api));
    uint32_t count;assert(!audio_card_control_count(registered[0],&count) && count==5);
    struct audio_control_info info;assert(!audio_card_control_info(registered[0],4,&info));
    assert(!strcmp(info.name,"Auto-Mute") && (info.access&AUDIO_CONTROL_WRITE));
    uint32_t stream;assert(!audio_stream_open(registered[0],0,AUDIO_PLAYBACK,&stream));
    struct stream_entry *entry=find_stream(stream);struct hda_stream *hw=entry->hw.driver;
    struct hda_module_card *card=find_card(registered[0])->opaque;
    uint64_t memory=alloc_dma(1,UINT32_MAX);
    struct audio_dma dma={.kernel=(void*)(uintptr_t)memory,.bus=memory,.bytes=4096};
    struct audio_params_ext params={.pcm={.rate=48000,.channels=2,.sample_bits=16,
        .frame_bytes=4,.period_frames=32,.buffer_frames=128},.format=AUDIO_FORMAT_S16_LE,
        .subformat=AUDIO_SUBFORMAT_STD,.significant_bits=16};
    assert(!audio_stream_prepare_format(stream,&params,&dma));
    assert(!audio_stream_trigger(stream,AUDIO_START));
    uint8_t tag=hw->tag;uint64_t generation=hw->generation;struct hda_route_group *speaker=hw->group;
    struct hda_route_group *headphone=&card->playback[1].group;
    struct audio_control_value policy={.values={1}};
    assert(!audio_card_control_write(registered[0],4,&policy));
    /* Sensing and waitable routing both run through the actual task pump. */
    senses[0][0]=1u<<31;
    audio_service_tick();jack_task_pump();audio_service_tick();jack_task_pump();
    assert(hw->state==HDA_STREAM_RUNNING && hw->group==headphone && hw->tag==tag);
    assert(hw->generation==generation && !speaker->active && headphone->active);
    assert(!speaker->members[0].stream_bound && headphone->members[0].stream_tag==tag);
    assert(headphone->members[0].stream_format==0x11);
    assert(gains[0][0][0]==23 && gains[0][0][1]==19);
    policy_route_fail_at=policy_route_calls+1;policy.values[0]=0;
    assert(audio_card_control_write(registered[0],4,&policy)==-EIO);
    assert(card->controls.auto_mute && hw->group==headphone && hw->state==HDA_STREAM_RUNNING);
    assert(!speaker->active && headphone->active && !headphone->rollback_failed);
    assert(headphone->members[0].stream_tag==tag);
    policy_route_fail_at=policy_route_calls+2;policy.values[0]=0;
    assert(audio_card_control_write(registered[0],4,&policy)==-EIO);
    assert(card->controls.auto_mute && hw->group==headphone && hw->state==HDA_STREAM_RUNNING);
    assert(!speaker->active && headphone->active && headphone->members[0].stream_tag==tag);
    policy_bind_fail_once=1;
    assert(audio_card_control_write(registered[0],4,&policy)==-EIO);
    assert(card->controls.auto_mute && hw->group==headphone && hw->state==HDA_STREAM_RUNNING);
    assert(!speaker->active && headphone->active && headphone->members[0].stream_tag==tag);
    policy_amp_fail_at=policy_amp_calls+4;
    assert(audio_card_control_write(registered[0],4,&policy)==-EIO);
    assert(card->controls.auto_mute && hw->group==headphone && hw->state==HDA_STREAM_RUNNING);
    assert(!speaker->active && headphone->active && headphone->members[0].stream_tag==tag);
    assert(gains[0][0][0]==23 && gains[0][0][1]==19);
    assert(!audio_card_control_write(registered[0],4,&policy));
    assert(!card->controls.auto_mute && hw->group==speaker && hw->state==HDA_STREAM_RUNNING);
    assert(speaker->active && !headphone->active && speaker->members[0].stream_tag==tag);
    assert(gains[0][0][0]==23 && gains[0][0][1]==19);
    assert(!audio_stream_trigger(stream,AUDIO_STOP));assert(!audio_stream_close(stream));
    free_pages(memory,1);hda_driver_module.fini();assert(!controllers && !allocation_count);
    puts("auto_mute_actual_module_RUNNING_shared_DAC_group_tag_gain_and_failure_rollback PASS");
}

static void actual_module_auto_mute_pause_contention_and_fail_closed(void)
{
    reset();jack_fixture=3;gains[0][0][0]=11;gains[0][0][1]=9;
    assert(!hda_driver_module.init(&api));
    struct hda_module_card *card=find_card(registered[0])->opaque;
    uint32_t stream;assert(!audio_stream_open(registered[0],0,AUDIO_PLAYBACK,&stream));
    struct stream_entry *entry=find_stream(stream);struct hda_stream *hw=entry->hw.driver;
    uint64_t memory=alloc_dma(1,UINT32_MAX);
    struct audio_dma dma={.kernel=(void*)(uintptr_t)memory,.bus=memory,.bytes=4096};
    struct audio_params_ext params={.pcm={.rate=48000,.channels=2,.sample_bits=16,
        .frame_bytes=4,.period_frames=32,.buffer_frames=128},.format=AUDIO_FORMAT_S16_LE,
        .subformat=AUDIO_SUBFORMAT_STD,.significant_bits=16};
    assert(!audio_stream_prepare_format(stream,&params,&dma));
    uint64_t generation=hw->generation;uint8_t tag=hw->tag;
    card->controls.headphone_present=1;
    struct audio_control_value policy={.values={1}};
    assert(!audio_card_control_write(registered[0],4,&policy));
    assert(hw->state==HDA_STREAM_PREPARED && hw->group==&card->playback[1].group);
    assert(hw->generation==generation && hw->tag==tag && !(hda_mmio_read32(hw->controller,0xa0)&2));
    assert(!audio_stream_trigger(stream,AUDIO_START));assert(!audio_stream_trigger(stream,AUDIO_PAUSE));
    policy.values[0]=0;assert(!audio_card_control_write(registered[0],4,&policy));
    assert(hw->state==HDA_STREAM_PAUSED && hw->group==&card->playback[0].group);
    assert(!(hda_mmio_read32(hw->controller,0xa0)&2));
    assert(!audio_stream_trigger(stream,AUDIO_UNPAUSE));
    card->operation_busy=1;policy.values[0]=1;
    assert(audio_card_control_write(registered[0],4,&policy)==-EBUSY);
    assert(audio_stream_trigger(stream,AUDIO_STOP)==-EBUSY);
    unsigned route_calls=policy_route_calls;card->controls.auto_mute_pending=1;
    assert(!hda_card_task_service(card,8) && card->controls.auto_mute_pending);
    assert(policy_route_calls==route_calls && hw->state==HDA_STREAM_RUNNING);
    card->operation_busy=0;card->controls.auto_mute_pending=0;
    /* Independent headphone ownership blocks policy even with an idle SD. */
    assert(!audio_stream_trigger(stream,AUDIO_STOP));assert(!audio_stream_close(stream));
    assert(!audio_stream_open(registered[0],1,AUDIO_PLAYBACK,&stream));
    assert(audio_card_control_write(registered[0],4,&policy)==-EBUSY);
    assert(!audio_stream_close(stream));
    assert(!audio_stream_open(registered[0],0,AUDIO_PLAYBACK,&stream));
    entry=find_stream(stream);hw=entry->hw.driver;
    assert(!audio_stream_prepare_format(stream,&params,&dma));assert(!audio_stream_trigger(stream,AUDIO_START));
    unsigned resources=allocation_count;
    policy_route_fail_at=policy_route_calls+1;policy_route_fail_count=2;
    assert(audio_card_control_write(registered[0],4,&policy)==-EIO);
    assert(hw->state==HDA_STREAM_STOPPED && !(hda_mmio_read32(hw->controller,0xa0)&2));
    assert(card->playback[0].group.rollback_failed && allocation_count==resources);
    struct audio_card_identity identity;
    assert(audio_card_identity(registered[0],&identity)==-ENODEV);
    assert(audio_card_identity(registered[1],&identity)==-ENODEV);
    assert(!audio_card_identity(registered[2],&identity));
    assert(!audio_card_identity(registered[3],&identity));
    jack_task_pump();assert(!audio_stream_close(stream));free_pages(memory,1);
    hda_driver_module.fini();assert(!controllers && !allocation_count && !teardown_error);
    puts("auto_mute_PREPARED_PAUSED_gate_contention_exclusive_HP_and_rollback_failure_BDF_retention PASS");
}

static void actual_module_prepare_preserves_volume_without_auto_mute(void)
{
    reset();jack_fixture=1;gains[0][0][0]=23;gains[0][0][1]=19;
    assert(!hda_driver_module.init(&api));
    uint32_t count;assert(!audio_card_control_count(registered[0],&count) && count==3);
    uint32_t stream;assert(!audio_stream_open(registered[0],0,AUDIO_PLAYBACK,&stream));
    uint64_t memory=alloc_dma(1,UINT32_MAX);
    struct audio_dma dma={.kernel=(void*)(uintptr_t)memory,.bus=memory,.bytes=4096};
    struct audio_params_ext params={.pcm={.rate=48000,.channels=2,.sample_bits=16,
        .frame_bytes=4,.period_frames=32,.buffer_frames=128},.format=AUDIO_FORMAT_S16_LE,
        .subformat=AUDIO_SUBFORMAT_STD,.significant_bits=16};
    assert(!audio_stream_prepare_format(stream,&params,&dma));
    struct audio_control_value volume;
    assert(!audio_card_control_read(registered[0],0,&volume));
    assert(volume.values[0]==23 && volume.values[1]==19);
    assert(effective_gains[0][0][0]==23 && effective_gains[0][0][1]==19);
    assert(!audio_stream_trigger(stream,AUDIO_START));
    assert(!audio_stream_trigger(stream,AUDIO_STOP));
    assert(!audio_stream_prepare_format(stream,&params,&dma));
    assert(!audio_card_control_read(registered[0],0,&volume));
    assert(volume.values[0]==23 && volume.values[1]==19);
    assert(effective_gains[0][0][0]==23 && effective_gains[0][0][1]==19);
    assert(!audio_stream_trigger(stream,AUDIO_STOP));
    policy_amp_fail_at=policy_amp_calls+1;
    assert(audio_stream_prepare_format(stream,&params,&dma)==-EIO);
    struct hda_stream *prepared=find_stream(stream)->hw.driver;
    assert(prepared->state==HDA_STREAM_STOPPED);
    assert(!(hda_mmio_read32(prepared->controller,0xa0)&2));
    assert(!audio_stream_prepare_format(stream,&params,&dma));
    assert(effective_gains[0][0][0]==23 && effective_gains[0][0][1]==19);
    assert(!audio_stream_trigger(stream,AUDIO_STOP));
    uint64_t large_memory=alloc_dma(32,UINT64_MAX);
    struct audio_dma large_dma={.kernel=(void*)(uintptr_t)large_memory,.bus=large_memory,.bytes=131072};
    params.pcm.period_frames=4096;params.pcm.buffer_frames=32768;
    assert(!audio_stream_prepare_format(stream,&params,&large_dma));
    struct hda_stream *hw=find_stream(stream)->hw.driver;
    assert(hw->period_count==8 && hw->cbl==131072 && hw->period_bytes==16384);
    assert(!audio_stream_trigger(stream,AUDIO_STOP));
    uint64_t larger_memory=alloc_dma(64,UINT64_MAX);
    struct audio_dma larger_dma={.kernel=(void*)(uintptr_t)larger_memory,.bus=larger_memory,.bytes=262144};
    params.pcm.period_frames=8192;params.pcm.buffer_frames=65536;
    assert(!audio_stream_prepare_format(stream,&params,&larger_dma));
    assert(hw->period_count==8 && hw->cbl==262144 && hw->period_bytes==32768);
    assert(!audio_stream_trigger(stream,AUDIO_STOP));assert(!audio_stream_close(stream));
    free_pages(larger_memory,64);
    free_pages(large_memory,32);
    free_pages(memory,1);hda_driver_module.fini();assert(!controllers && !allocation_count);
    puts("actual_module_prepare_preserves_user_volume_without_auto_mute_capability PASS");
}

int main(void)
{
    actual_module_auto_mute_running_group_transaction();
    actual_module_auto_mute_pause_contention_and_fail_closed();
    actual_module_prepare_preserves_volume_without_auto_mute();
    actual_module_jack_publication_and_sparse_events(1);
    actual_module_jack_publication_and_sparse_events(2);
    fatal_controller_hides_all_codecs_without_irq_teardown();
    reset();assert(!hda_driver_module.init(&api));assert(card_count==4);
    assert(identities[0].slot==1 && identities[0].codec==0);
    assert(identities[1].slot==1 && identities[1].codec==1);
    assert(strcmp(identities[0].id,identities[1].id));
    unsigned total=allocation_calls;
    struct audio_format_caps caps;assert(!audio_stream_format_caps(registered[0],0,AUDIO_PLAYBACK,&caps));
    assert(caps.pcm.formats==AUDIO_FORMAT_S16_LE && caps.pcm.rates==AUDIO_RATE_48000);
    assert(audio_stream_format_caps(registered[0],1,AUDIO_PLAYBACK,&caps)==-ENODEV);
    assert(!audio_stream_format_caps(registered[1],0,AUDIO_PLAYBACK,&caps));
    assert(caps.subformats[1]==(1u<<AUDIO_SUBFORMAT_MSBITS_24));
    uint32_t p,c,other;assert(!audio_stream_open(registered[0],0,AUDIO_PLAYBACK,&p));
    assert(audio_stream_open(registered[1],0,AUDIO_PLAYBACK,&other)==-EBUSY);
    assert(!audio_stream_open(registered[0],0,AUDIO_CAPTURE,&c));
    assert(!audio_stream_open(registered[2],0,AUDIO_PLAYBACK,&other));
    struct stream_entry *pe=find_stream(p),*ce=find_stream(c);
    struct hda_stream *phw=pe->hw.driver,*chw=ce->hw.driver;
    assert(phw->index==1 && chw->index==0 && phw->card_id==registered[0]);
    assert(phw->controller==chw->controller && phw->controller->active_streams==2);
    assert(!audio_stream_trigger(p,AUDIO_STOP));assert(phw->controller->active_streams==1);
    assert(!audio_stream_trigger(p,AUDIO_STOP));assert(phw->controller->active_streams==1);
    assert(!audio_stream_close(p));assert(!audio_stream_close(c));assert(!audio_stream_close(other));
    puts("actual_module_multicodec_BDF_direction_slot_caps_and_idempotent_STOP PASS");
    destroy_stuck=1;hda_driver_module.fini();assert(teardown_error==-EIO && controllers);
    destroy_stuck=0;teardown_error=0;hda_driver_module.fini();assert(!controllers && !allocation_count);
    puts("module_fini_reports_retained_controller_and_retries PASS");
    for(unsigned nth=1;nth<=total;nth++){
        reset();fail_nth=nth;assert(hda_driver_module.init(&api)==-ENOMEM);
        hda_driver_module.fini();assert(!controllers && !allocation_count && !teardown_error);
        for(unsigned n=0;n<CARD_MAX;n++)assert(!cards[n].live);
    }
    fail_nth=0;printf("module_each_Nth_metadata_DMA_allocation_failure_reclaimed %u PASS\n",total);
    return 0;
}
