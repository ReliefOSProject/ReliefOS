#include <stddef.h>
#include <string.h>
#include "audio_fixture.h"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"

/* The lifetime fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

static unsigned opens, closes, stops, service, module_unloaded, stop_error;
static int caps(void *o,uint32_t d,enum audio_direction dir,struct audio_caps *c){(void)o;(void)d;(void)dir; *c=(struct audio_caps){.formats=AUDIO_FORMAT_S16_LE,.rates=AUDIO_RATE_48000,.channels_min=2,.channels_max=2};return 0;}
static int open_stream(void *o,uint32_t d,enum audio_direction dir,struct audio_hw_stream *s){(void)o;(void)d;(void)dir;++opens;s->id=17;return 0;}
static int prepare(void *o,struct audio_hw_stream *s,const struct audio_params *p,const struct audio_dma *d){(void)o;(void)s;(void)p;(void)d;return 0;}
static int trigger(void *o,struct audio_hw_stream *s,enum audio_trigger t){(void)o;(void)s;assert(!module_unloaded);if(t==AUDIO_STOP){++stops;if(stop_error)return -5;}return 0;}
static int pointer(void *o,struct audio_hw_stream *s,uint64_t *p){(void)o;(void)s;*p=123;return 0;}
static void close_stream(void *o,struct audio_hw_stream *s){(void)o;(void)s;assert(!module_unloaded);++closes;}
static uint32_t controls(void *o){(void)o;return 0;}
static uint32_t tick(void *o,uint32_t budget){(void)o;assert(!module_unloaded);assert(budget<=32 && budget);__atomic_add_fetch(&service,1,__ATOMIC_RELAXED);return 1;}
static struct audio_card_ops ops={.version=1,.size=sizeof ops,.pcm_caps=caps,.open=open_stream,.prepare=prepare,.trigger=trigger,.pointer=pointer,.close=close_stream,.control_count=controls};
static int format_caps(void *o,uint32_t d,enum audio_direction dir,struct audio_format_caps *c)
{(void)o;(void)d;(void)dir;memset(c,0,sizeof(*c));c->pcm.formats=AUDIO_FORMAT_S32_LE;return 0;}
static int prepare_format(void *o,struct audio_hw_stream *s,const struct audio_params_ext *p,const struct audio_dma *d)
{(void)o;(void)s;(void)p;(void)d;return 0;}
static pthread_mutex_t service_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t service_cond=PTHREAD_COND_INITIALIZER;
static unsigned generation_workers_entered,generation_workers_release;
static void *ticks(void *o){
    (void)o;
    audio_service_tick();
    pthread_mutex_lock(&service_mutex);
    ++generation_workers_entered;
    pthread_cond_broadcast(&service_cond);
    while(!generation_workers_release)pthread_cond_wait(&service_cond,&service_mutex);
    pthread_mutex_unlock(&service_mutex);
    for(unsigned n=0;n<1000;n++)audio_service_tick();
    return NULL;
}
static unsigned total_budget, budget_calls;
static uint32_t consume(void *o,uint32_t budget) {(void)o;assert(budget>=8);total_budget+=8;++budget_calls;return 8;}
static unsigned service_entered,service_release,unregistered;
static uint32_t held_service(void *o,uint32_t budget){
    (void)o;assert(budget);pthread_mutex_lock(&service_mutex);service_entered=1;pthread_cond_broadcast(&service_cond);
    while(!service_release)pthread_cond_wait(&service_cond,&service_mutex);pthread_mutex_unlock(&service_mutex);
    assert(!module_unloaded);return 1;
}
static void *one_tick(void *o){(void)o;audio_service_tick();return NULL;}
static void *unregister_thread(void *o){assert(!audio_unregister_card((uintptr_t)o,1));__atomic_store_n(&unregistered,1,__ATOMIC_RELEASE);return NULL;}
static void service_removal_waits_for_module_pin(const struct audio_card_identity *identity){
    service_entered=service_release=unregistered=0;
    uint32_t card,stream;assert(!audio_register_card_owned(identity,&ops,NULL,5,&card));
    assert(!audio_stream_open(card,0,AUDIO_PLAYBACK,&stream));assert(!audio_card_set_service(card,held_service,NULL));
    pthread_t service_thread,removal_thread;pthread_create(&service_thread,NULL,one_tick,NULL);
    pthread_mutex_lock(&service_mutex);while(!service_entered)pthread_cond_wait(&service_cond,&service_mutex);pthread_mutex_unlock(&service_mutex);
    pthread_create(&removal_thread,NULL,unregister_thread,(void*)(uintptr_t)card);
    for(;;){uint64_t flags;kernel_spin_lock_irqsave(&audio_lock,&flags);unsigned closing=cards[card&255].closing;kernel_spin_unlock_irqrestore(&audio_lock,flags);if(closing)break;}
    assert(!__atomic_load_n(&unregistered,__ATOMIC_ACQUIRE));
    pthread_mutex_lock(&service_mutex);service_release=1;pthread_cond_broadcast(&service_cond);pthread_mutex_unlock(&service_mutex);
    pthread_join(service_thread,NULL);pthread_join(removal_thread,NULL);module_unloaded=1;
    uint64_t frame;assert(audio_stream_pointer(stream,&frame)==-19);audio_stream_close(stream);audio_service_tick();module_unloaded=0;
    puts("service_unregister_synchronizes_module_pin PASS");
}
/* Catch IRQ invocation of a waitable callback, truncated ABI copies, lost
 * coalesced work, per-card budget reset, stale generation calls and unpin before
 * task completion. The fake callback is the external hardware wait boundary. */
static unsigned task_calls[5],task_work,task_requeue;
static uint32_t task_callback(void *opaque,uint32_t budget)
{
    unsigned index=(uintptr_t)opaque;
    assert(!module_unloaded && (audio_fixture_irq_flags&(1ULL<<9)));
    assert(index<5 && budget);
    ++task_calls[index];
    if(task_requeue){task_requeue=0;audio_service_tick();}
    unsigned used=budget<8?budget:8;task_work+=used;return used;
}
static void deferred_task_service_contract(const struct audio_card_identity *identity)
{
    struct audio_card_ops extended=ops;extended.task_service=task_callback;
    uint32_t card;
    for(unsigned partial=0;partial<2;++partial){
        extended.size=(uint32_t)(offsetof(struct audio_card_ops,task_service)+
                        (partial?sizeof(extended.task_service)-1:0));
        assert(!audio_register_card_owned(identity,&extended,NULL,11,&card));
        audio_service_tick();assert(!audio_task_work_pending());
        assert(!audio_process_task_work(32) && !task_work);
        assert(!audio_unregister_card(card,0));
    }
    extended.size=sizeof extended;
    assert(!audio_register_card_owned(identity,&extended,NULL,11,&card));
    assert(!audio_task_work_pending());
    uint64_t flags=kernel_irq_save();
    for(unsigned i=0;i<5;++i)audio_service_tick();
    kernel_irq_restore(flags);
    assert(!task_calls[0] && audio_task_work_pending());
    assert(!audio_process_task_work(0) && !task_calls[0] && audio_task_work_pending());
    task_requeue=1;
    assert(audio_process_task_work(3)==3 && task_calls[0]==1 && task_work==3);
    assert(audio_task_work_pending());
    assert(audio_process_task_work(1)==1 && task_calls[0]==2 && task_work==4);
    assert(!audio_task_work_pending() && !audio_process_task_work(32));
    audio_service_tick();assert(!audio_unregister_card(card,0));
    assert(!audio_task_work_pending() && !audio_process_task_work(32));
    memset(task_calls,0,sizeof task_calls);task_work=0;
    uint32_t ids[5];
    for(unsigned i=0;i<5;++i)assert(!audio_register_card_owned(identity,&extended,(void*)(uintptr_t)i,11,&ids[i]));
    audio_service_tick();assert(!task_work);
    assert(audio_process_task_work(32)==32 && task_work==32 && audio_task_work_pending());
    unsigned called=0;for(unsigned i=0;i<5;++i){assert(task_calls[i]<=1);called+=task_calls[i];}
    assert(called==4);
    assert(audio_process_task_work(8)==8 && task_work==40 && !audio_task_work_pending());
    for(unsigned i=0;i<5;++i){assert(task_calls[i]==1);assert(!audio_unregister_card(ids[i],0));}
    puts("task_service_size_gating_IRQ_deferral_coalescing_budget_fairness_generation PASS");
}
static void *one_task_pump(void *opaque){(void)opaque;assert(audio_process_task_work(32)==1);return NULL;}
static void task_removal_waits_for_module_pin(const struct audio_card_identity *identity)
{
    struct audio_card_ops extended=ops;extended.task_service=held_service;
    service_entered=service_release=unregistered=0;
    uint32_t card;assert(!audio_register_card_owned(identity,&extended,NULL,12,&card));
    assert(!audio_card_set_service(card,tick,NULL));audio_service_tick();
    pthread_t callback,removal;
    assert(!pthread_create(&callback,NULL,one_task_pump,NULL));
    pthread_mutex_lock(&service_mutex);while(!service_entered)pthread_cond_wait(&service_cond,&service_mutex);pthread_mutex_unlock(&service_mutex);
    /* The IRQ path must still progress while task hardware waits. */
    unsigned before=service;audio_service_tick();assert(service==before+1);
    assert(!pthread_create(&removal,NULL,unregister_thread,(void*)(uintptr_t)card));
    for(;;){uint64_t flags;kernel_spin_lock_irqsave(&audio_lock,&flags);unsigned closing=cards[card&255].closing;kernel_spin_unlock_irqrestore(&audio_lock,flags);if(closing)break;}
    assert(!__atomic_load_n(&unregistered,__ATOMIC_ACQUIRE));
    pthread_mutex_lock(&service_mutex);service_release=1;pthread_cond_broadcast(&service_cond);pthread_mutex_unlock(&service_mutex);
    pthread_join(callback,NULL);pthread_join(removal,NULL);
    module_unloaded=1;audio_service_tick();assert(!audio_process_task_work(32));module_unloaded=0;
    puts("task_service_unregister_pin_and_independent_IRQ_progress PASS");
}
static void fatal_disconnect_retains_failed_stop_and_retries(void)
{
    struct audio_card_identity identity={.id="fault",.name="fault",.bus=2,.slot=3,.function=1};
    uint32_t a,b,other_owner,other_bdf,stream,sentinel=0x1234;
    assert(!audio_register_card_owned(&identity,&ops,NULL,9,&a));
    identity.codec=1;assert(!audio_register_card_owned(&identity,&ops,NULL,9,&b));
    assert(!audio_register_card_owned(&identity,&ops,NULL,10,&other_owner));
    identity.function=2;assert(!audio_register_card_owned(&identity,&ops,NULL,9,&other_bdf));
    assert(!audio_stream_open(a,0,AUDIO_PLAYBACK,&stream));
    assert(!audio_card_set_service(a,tick,NULL));
    uint32_t epoch=audio_pcm_event_epoch();unsigned before_stops=stops,before_closes=closes;
    uint64_t flags=kernel_irq_save();
    assert(!audio_request_controller_disconnect(b));
    assert(!(audio_fixture_irq_flags&(1ULL<<9)));
    kernel_irq_restore(flags);
    assert(stops==before_stops && closes==before_closes);
    assert(audio_disconnect_work_pending());
    assert(audio_pcm_event_epoch()!=epoch);epoch=audio_pcm_event_epoch();
    assert(!audio_request_controller_disconnect(a));assert(audio_pcm_event_epoch()==epoch);
    assert(audio_request_controller_disconnect(0xdead00)==-ENODEV);
    assert(audio_pcm_event_epoch()==epoch);
    assert(audio_stream_open(b,0,AUDIO_PLAYBACK,&sentinel)==-ENODEV && sentinel==0x1234);
    struct audio_card_identity out;
    assert(audio_card_identity(a,&out)==-ENODEV && audio_card_identity(b,&out)==-ENODEV);
    assert(!audio_card_identity(other_owner,&out) && !audio_card_identity(other_bdf,&out));
    uint64_t frame;int error;
    assert(audio_stream_period_poll(stream,&frame,&error)==1 && error==-ENODEV);
    unsigned before_service=service;audio_service_tick();assert(service==before_service);
    assert(!audio_process_pending_disconnects(0));assert(stops==before_stops);
    stop_error=1;
    assert(audio_process_pending_disconnects(1)==1);
    struct card_entry *c=&cards[a&SLOT_MASK];struct stream_entry *s=find_stream(stream);
    assert(c->live && c->closing && c->disconnect_pending && !c->disconnecting);
    assert(s->live && !s->detached && closes==before_closes);
    assert(audio_stream_pointer(stream,&frame)==-ENODEV);
    /* A final fd close must return a retained-lease error, never spin forever
     * behind a pending fault whose STOP failed. */
    assert(audio_stream_close(stream)==-EIO);
    assert(s->live && !s->detached && closes==before_closes);
    stop_error=0;assert(audio_process_pending_disconnects(16)==2);
    assert(!audio_disconnect_work_pending() && s->live && s->detached);
    assert(!audio_stream_close(stream));
    assert(audio_request_controller_disconnect(a)==-ENODEV);
    assert(!audio_unregister_owner(9,1) && !audio_unregister_owner(10,1));
    opens=closes=stops=0;
    puts("fatal_disconnect_BDF_owner_generation_IRQ_safe_STOP_failure_close_retry PASS");
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    fatal_disconnect_retains_failed_stop_and_retries();
    struct audio_card_identity identity={.id="fixture",.name="card"}; uint32_t card,stream;
    deferred_task_service_contract(&identity);
    task_removal_waits_for_module_pin(&identity);
    struct audio_card_ops legacy=ops;
    legacy.size=(uint32_t)(offsetof(struct audio_card_ops,pcm_format_caps));
    legacy.pcm_format_caps=format_caps; legacy.prepare_format=prepare_format;
    assert(!audio_register_card_owned(&identity,&legacy,NULL,3,&card));
    struct audio_format_caps format_info;
    assert(audio_stream_format_caps(card,0,AUDIO_PLAYBACK,&format_info)==-95);
    assert(!audio_unregister_card(card,0));
    struct audio_card_ops partial=ops;
    partial.size=(uint32_t)(offsetof(struct audio_card_ops,prepare_format)+sizeof(partial.prepare_format)-1u);
    partial.pcm_format_caps=format_caps; partial.prepare_format=prepare_format;
    assert(!audio_register_card_owned(&identity,&partial,NULL,3,&card));
    assert(!audio_stream_format_caps(card,0,AUDIO_PLAYBACK,&format_info));
    assert(format_info.pcm.formats==AUDIO_FORMAT_S32_LE);
    assert(!audio_unregister_card(card,0));
    ops.pcm_format_caps=format_caps; ops.prepare_format=prepare_format;
    struct audio_card_ops bad=ops;bad.size=8;assert(audio_register_card_owned(&identity,&bad,NULL,4,&card)==-22);
    assert(!audio_register_card_owned(&identity,&ops,NULL,4,&card));
    int (*saved_open)(void *,uint32_t,enum audio_direction,struct audio_hw_stream *)=ops.open;
    ops.open=NULL;identity.id[0]='z';assert(cards[card&255].identity.id[0]=='f');identity.id[0]='f';
    assert(!audio_card_set_service(card,tick,NULL));
    assert(!audio_stream_open(card,0,AUDIO_PLAYBACK,&stream));ops.open=saved_open; assert(audio_unregister_card(card,0)==-16);
    uint32_t old=card; assert(!audio_unregister_card(card,1)); assert(stops==1 && closes==1);
    uint64_t frame;assert(audio_stream_pointer(stream,&frame)==-19);audio_stream_close(stream);assert(closes==1);
    assert(!audio_register_card_owned(&identity,&ops,NULL,4,&card));assert(old!=card);
    audio_period_elapsed(old,17,512,0);assert(audio_card_periods(card)==0);
    assert(!audio_card_set_service(card,tick,NULL)); pthread_t t[4];for(unsigned i=0;i<4;i++)pthread_create(&t[i],NULL,ticks,NULL);
    pthread_mutex_lock(&service_mutex);
    while(generation_workers_entered<4)pthread_cond_wait(&service_cond,&service_mutex);
    pthread_mutex_unlock(&service_mutex);
    unsigned admitted_before_unregister=__atomic_load_n(&service,__ATOMIC_ACQUIRE);
    assert(admitted_before_unregister>0);
    pthread_mutex_lock(&service_mutex);generation_workers_release=1;pthread_cond_broadcast(&service_cond);pthread_mutex_unlock(&service_mutex);
    assert(!audio_unregister_owner(4,1));for(unsigned i=0;i<4;i++)pthread_join(t[i],NULL);
    unsigned done=service;audio_service_tick();assert(service==done);
    uint32_t budget_cards[4];for(unsigned i=0;i<4;i++){
        assert(!audio_register_card_owned(&identity,&ops,NULL,6,&budget_cards[i]));
        assert(!audio_card_set_service(budget_cards[i],consume,NULL));
    }
    audio_service_tick();assert(total_budget==32 && budget_calls==4);assert(!audio_unregister_owner(6,1));
    puts("tick_total_budget_32 PASS");
    service_removal_waits_for_module_pin(&identity);
    assert(!audio_register_card_owned(&identity,&ops,NULL,7,&card));
    assert(!audio_stream_open(card,0,AUDIO_PLAYBACK,&stream));stop_error=1;
    assert(audio_unregister_card(card,1)==-5);assert(!audio_stream_pointer(stream,&frame));
    stop_error=0;assert(!audio_unregister_card(card,1));audio_stream_close(stream);
    puts("STOP_failure_keeps_module_alive PASS");
    for(unsigned i=0;i<CARD_MAX;i++)assert(!cards[i].live);for(unsigned i=0;i<STREAM_MAX;i++)assert(!streams[i].live);
    puts("metadata_is_copied PASS\nregistration_failure_rollback PASS\nactive_stream_busy PASS\nforced_disconnect_no_stale_callbacks PASS\nservice_1_4_cpu_generation PASS");return 0;
}
