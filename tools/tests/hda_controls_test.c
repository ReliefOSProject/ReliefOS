#define HDA_TESTING 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel/reliefnt/drivers/hda/hda.h"

static struct {
    uint16_t amp[2];
    uint32_t pin_sense;
    uint32_t command_count;
    uint32_t fail_at;
    uint32_t route_verb_count;
    uint32_t route_fail_at;
    uint8_t headphone_present;
    uint8_t amp_nid;
    uint16_t last_verb;
    uint32_t service_calls;
} fake;
static void (*control_read_hook)(void);

int hda_exec_verb(struct hda_controller *controller, uint8_t cad, uint8_t nid,
                  uint16_t verb, uint16_t payload, bool short_verb,
                  uint32_t *out_response)
{
    (void)controller;
    (void)cad;
    (void)nid;
    (void)short_verb;
    ++fake.command_count;
    fake.last_verb=verb;
    if (fake.fail_at && fake.command_count == fake.fail_at) return -EIO;
    if (verb == 0xf09u) {
        *out_response = fake.pin_sense;
        return 0;
    }
    if (verb == 0x0bu) {
        *out_response = fake.amp[(payload & (1u << 13)) ? 0u : 1u];
        return 0;
    }
    if (verb == 0x03u) {
        uint32_t channel = (payload & (1u << 12)) ? 1u : 0u;
        fake.amp[channel] = (uint16_t)(payload & 0x7fu);
        return 0;
    }
    *out_response = 0u;
    return 0;
}

int hda_codec_control_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t *out)
{
    (void)codec; (void)input; (void)index;
    fake.amp_nid=nid;
    if(control_read_hook){void (*hook)(void)=control_read_hook;control_read_hook=NULL;hook();}
    *out = fake.amp[right ? 1u : 0u];
    return 0;
}

int hda_codec_control_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                                uint8_t index, bool right, uint16_t value,
                                uint32_t caps)
{
    (void)codec; (void)nid; (void)input; (void)index; (void)caps;
    ++fake.command_count;
    if (fake.fail_at && fake.command_count == fake.fail_at) return -EIO;
    fake.amp[right ? 1u : 0u] = value;
    return 0;
}

int hda_codec_control_gate_enter(struct hda_codec *codec)
{ (void)codec; return 0; }
void hda_codec_control_gate_leave(struct hda_codec *codec)
{ (void)codec; }

int hda_apply_route(struct hda_codec *codec, struct hda_route *route,
                    bool enable)
{
    (void)codec;
    ++fake.route_verb_count;
    if (enable) {
        ++fake.route_verb_count;
        if (fake.route_fail_at &&
            fake.route_verb_count == fake.route_fail_at) {
            fake.route_fail_at = 0u;
            return -EIO;
        }
    }
    route->active = enable ? 1u : 0u;
    return 0;
}

/* Transport boundaries are exercised with the real ring code in the
 * controller suite; this fixture isolates waitable policy failure/retry. */
uint32_t hda_controller_service_budget(struct hda_controller *c,uint32_t budget)
{ (void)c;assert(budget);++fake.service_calls;return 0; }
int hda_unsolicited_pop_for_codec(struct hda_controller *c,uint8_t cad,
                                struct hda_unsolicited_response *event)
{ (void)c;(void)cad;(void)event;return -EAGAIN; }
uint32_t hda_unsolicited_dropped_count(struct hda_controller *c)
{ (void)c;return 0; }
int hda_verb_poll(struct hda_controller *c,uint64_t ticket,uint32_t *response)
{ (void)c;(void)ticket;(void)response;return -EAGAIN; }
int hda_verb_submit(struct hda_controller *c,uint8_t cad,uint8_t nid,
                   uint16_t verb,uint16_t payload,bool short_verb,uint64_t *ticket)
{ (void)c;(void)cad;(void)nid;(void)verb;(void)payload;(void)short_verb;*ticket=1;return 0; }

#include "../../kernel/reliefnt/drivers/hda/controls.c"

static void test_amp_units(void)
{
    uint32_t caps = 20u | (40u << 8) | (1u << 16) | (1u << 31);
    assert(hda_amp_db(caps, 20u) == 0);
    assert(hda_amp_db(caps, 0u) == -1000);
}

static void test_controls_and_atomic_rollback(void)
{
    struct hda_codec codec = {0};
    struct hda_codec_node nodes[4] = {0};
    codec.nodes = nodes;
    codec.cad = 2u;
    nodes[2].present = 1u;
    nodes[2].type = HDA_WIDGET_PIN;
    nodes[2].widget_caps = HDA_WCAP_OUT_AMP | HDA_WCAP_STEREO;
    nodes[2].amp_output_caps = 20u | (40u << 8) | (1u << 16) | (1u << 31);
    struct hda_route speaker = {.codec = &codec, .pin_nid = 2u,
                                .direction = HDA_ROUTE_PLAYBACK};
    struct hda_controls controls;
    memset(&fake, 0, sizeof(fake));
    fake.amp[0] = fake.amp[1] = 20u;
    assert(!hda_controls_init(&controls, &codec, &speaker, NULL, 2u, 0u,
                              NULL, 0u, 1u));
    assert(hda_controls_count(&controls) == 5u);
    struct audio_control_info info;
    assert(!hda_controls_info(&controls, HDA_CONTROL_PLAYBACK_VOLUME, &info));
    assert(info.type == AUDIO_CONTROL_INTEGER && info.count == 2u);
    assert(info.min == 0 && info.max == 40 && info.db_min == -1000 &&
           info.db_step == 50);
    struct audio_control_value value = {.values = {22, 23}};
    assert(!hda_controls_write(&controls, HDA_CONTROL_PLAYBACK_VOLUME, &value));
    assert(fake.amp[0] == 22u && fake.amp[1] == 23u);
    fake.fail_at = fake.command_count + 2u;
    value.values[0] = 24; value.values[1] = 25;
    assert(hda_controls_write(&controls, HDA_CONTROL_PLAYBACK_VOLUME, &value) ==
           -EIO);
    assert(fake.amp[0] == 22u && fake.amp[1] == 23u);
    fake.fail_at = 0u;
    assert(!hda_controls_info(&controls, HDA_CONTROL_PLAYBACK_MUTE, &info));
    assert(info.access & AUDIO_CONTROL_WRITE);
}

static void test_converter_amp_without_pin_amp(void)
{
    struct hda_codec codec={0};struct hda_codec_node nodes[4]={0};codec.nodes=nodes;
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN};
    nodes[1]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_AUDIO_OUTPUT,
        .widget_caps=HDA_WCAP_OUT_AMP,.amp_output_caps=(30u<<8)|(1u<<31)};
    struct hda_route route={.codec=&codec,.pin_nid=2,.path={2,1},.path_count=2};
    struct hda_controls controls;memset(&fake,0,sizeof fake);fake.amp[0]=11;fake.amp[1]=17;
    assert(!hda_controls_init(&controls,&codec,&route,NULL,2,0,NULL,0,false));
    assert(fake.amp_nid==1);
    struct audio_control_info info;assert(!hda_controls_info(&controls,0,&info));
    assert(info.count==1 && info.max==30);
    struct audio_control_value value;assert(!hda_controls_read(&controls,0,&value));
    assert(value.values[0]==11 && fake.amp_nid==1);
    value.values[0]=12;assert(!hda_controls_write(&controls,0,&value));
    assert(fake.amp[0]==12 && fake.amp[1]==17);
}

static void test_jack_deduplicates(void)
{
    struct hda_codec codec = {0};
    struct hda_codec_node nodes[4] = {0};
    codec.nodes = nodes;
    codec.cad = 2u;
    nodes[2].present = 1u;
    nodes[2].type = HDA_WIDGET_PIN;
    nodes[2].pin_caps = HDA_PINCAP_OUTPUT;
    nodes[2].widget_caps = HDA_WCAP_OUT_AMP;
    nodes[2].amp_output_caps = 20u | (40u << 8) | (1u << 16);
    struct hda_route speaker = {.codec = &codec, .pin_nid = 2u,
                                .direction = HDA_ROUTE_PLAYBACK};
    struct hda_controls controls;
    struct audio_control_info info;
    memset(&fake, 0, sizeof(fake));
    assert(!hda_controls_init(&controls, &codec, &speaker, NULL, 2u, 0u,
                              NULL, 0u, 0u));
    fake.headphone_present = 1u;
    fake.pin_sense = 1u << 31;
    assert(hda_controls_process_jack_sense(&controls, fake.pin_sense) == 1);
    assert(controls.jack_events == 1u);
    assert(hda_controls_process_jack_sense(&controls, fake.pin_sense) == 0);
    assert(controls.jack_events == 1u);
    assert(!hda_controls_info(&controls, HDA_CONTROL_HEADPHONE_JACK, &info));
    assert(info.access & AUDIO_CONTROL_VOLATILE);
}

static void test_capture_source_and_mute_capability(void)
{
    struct hda_codec codec = {0};
    struct hda_codec_node nodes[4] = {0};
    codec.nodes = nodes;
    nodes[2].present = 1u;
    nodes[2].type = HDA_WIDGET_PIN;
    nodes[2].widget_caps = HDA_WCAP_OUT_AMP;
    nodes[2].amp_output_caps = 20u | (40u << 8) | (1u << 16);
    struct hda_route speaker = {.codec = &codec, .pin_nid = 2u,
                                .direction = HDA_ROUTE_PLAYBACK};
    struct hda_controls controls;
    struct audio_control_info info;
    struct audio_control_value value = {.values = {1}};
    memset(&fake, 0, sizeof(fake));
    fake.amp[0] = fake.amp[1] = 20u;
    assert(!hda_controls_init(&controls, &codec, &speaker, NULL, 2u, 0u,
                              NULL, 2u, 0u));
    assert(!hda_controls_info(&controls, HDA_CONTROL_PLAYBACK_MUTE, &info));
    assert(!(info.access & AUDIO_CONTROL_WRITE));
    assert(hda_controls_write(&controls, HDA_CONTROL_PLAYBACK_MUTE, &value) ==
           -EOPNOTSUPP);
    assert(!hda_controls_write(&controls, HDA_CONTROL_CAPTURE_SOURCE, &value));
    assert(controls.capture_source == 1u);
    assert(controls.playback_gain[0] == 20u && controls.playback_gain[1] == 20u);
}

static void test_route_rollback_on_third_verb(void)
{
    struct hda_codec codec = {0};
    struct hda_codec_node nodes[4] = {0};
    codec.nodes = nodes;
    nodes[2].present = 1u;
    nodes[2].type = HDA_WIDGET_PIN;
    nodes[2].widget_caps = HDA_WCAP_OUT_AMP;
    nodes[2].amp_output_caps = 20u | (40u << 8) | (1u << 16);
    nodes[3].present = 1u;
    nodes[3].type = HDA_WIDGET_PIN;
    nodes[3].widget_caps = HDA_WCAP_OUT_AMP;
    nodes[3].amp_output_caps = nodes[2].amp_output_caps;
    struct hda_route speaker = {.codec = &codec, .pin_nid = 2u,
                                .direction = HDA_ROUTE_PLAYBACK,
                                .path_count = 1u, .active = 1u};
    struct hda_route headphone = {.codec = &codec, .pin_nid = 3u,
                                  .direction = HDA_ROUTE_PLAYBACK,
                                  .path_count = 1u, .active = 0u};
    struct hda_controls controls;
    struct audio_control_value value = {.values = {1}};
    memset(&fake, 0, sizeof(fake));
    fake.amp[0] = fake.amp[1] = 20u;
    assert(!hda_controls_init(&controls, &codec, &speaker, &headphone,
                              2u, 3u, NULL, 0u, 0u));
    controls.headphone_present = 1u;
    fake.route_fail_at = 3u;
    assert(hda_controls_write(&controls, HDA_CONTROL_AUTO_MUTE, &value) ==
           -EIO);
    assert(fake.route_verb_count >= 3u);
    assert(speaker.active == 1u && headphone.active == 0u);
    assert(controls.auto_mute == 0u);
}

static void test_worker_preserves_failed_policy_and_zero_budget(void)
{
    struct hda_codec codec={0};struct hda_codec_node nodes[4]={0};codec.nodes=nodes;
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP,.amp_output_caps=(40u<<8)|(1u<<31)};
    nodes[3]=nodes[2];
    struct hda_route speaker={.codec=&codec,.pin_nid=2,.path_count=1,.active=1};
    struct hda_route headphone={.codec=&codec,.pin_nid=3,.path_count=1};
    struct hda_controls controls;memset(&fake,0,sizeof fake);fake.amp[0]=fake.amp[1]=20;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    controls.headphone_present=controls.auto_mute=controls.auto_mute_pending=1;
    assert(!hda_controls_worker(&controls,0));
    assert(!fake.route_verb_count && controls.auto_mute_pending);
    fake.route_fail_at=3;
    assert(hda_controls_worker(&controls,8)==-EIO);
    assert(controls.auto_mute_pending && speaker.active && !headphone.active);
    assert(!hda_controls_worker(&controls,8));
    assert(!controls.auto_mute_pending && !speaker.active && headphone.active);
    assert((fake.amp[0]&0x7f)==20);
}

static void test_unsolicited_requires_real_widget_capability(void)
{
    struct hda_controller c={.transport_mode=HDA_TRANSPORT_RINGS,.rings_started=1};
    struct hda_codec codec={.controller=&c};struct hda_codec_node nodes[4]={0};codec.nodes=nodes;
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP,.amp_output_caps=40u<<8};
    nodes[3]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .pin_caps=HDA_PINCAP_OUTPUT|(1u<<2)};
    struct hda_route speaker={.codec=&codec,.pin_nid=2};struct hda_controls controls;
    memset(&fake,0,sizeof fake);
    assert(!hda_controls_init(&controls,&codec,&speaker,NULL,2,3,NULL,0,false));
    assert(!fake.command_count && !controls.unsolicited_enabled && controls.poll_only);
    assert(controls.sense_pending);
    nodes[3].widget_caps=1u<<7;
    assert(!hda_controls_init(&controls,&codec,&speaker,NULL,2,3,NULL,0,false));
    assert(fake.command_count==1 && fake.last_verb==0x708u && controls.unsolicited_enabled);
    assert(controls.sense_pending && !controls.poll_only);
    nodes[3].pin_caps=HDA_PINCAP_OUTPUT;
    assert(!hda_controls_init(&controls,&codec,&speaker,NULL,2,3,NULL,0,false));
    assert(!controls.headphone_pin && !controls.unsolicited_enabled);
    unsigned calls=fake.service_calls;
    assert(!hda_controls_service(&controls,8) && calls==fake.service_calls);
}

static struct hda_controls *held_controls;
static void control_transaction_reentry(void)
{
    struct audio_control_value value={.values={1,1}};
    unsigned calls=fake.service_calls;
    assert(hda_controls_read(held_controls,HDA_CONTROL_HEADPHONE_JACK,&value)==-EBUSY);
    assert(hda_controls_write(held_controls,HDA_CONTROL_PLAYBACK_VOLUME,&value)==-EBUSY);
    assert(hda_controls_process_jack_sense(held_controls,1u<<31)==-EBUSY);
    assert(!hda_controls_service(held_controls,8));
    assert(hda_controls_worker(held_controls,8)==-EBUSY);
    assert(calls==fake.service_calls && !held_controls->headphone_present);
}

static void test_state_gate_does_not_wait_or_lose_pending_work(void)
{
    struct hda_controller c={.transport_mode=HDA_TRANSPORT_IMMEDIATE};
    struct hda_codec codec={.controller=&c};struct hda_codec_node nodes[4]={0};codec.nodes=nodes;
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP,.amp_output_caps=40u<<8};
    nodes[3]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .pin_caps=HDA_PINCAP_OUTPUT|HDA_PINCAP_PRESENCE};
    struct hda_route speaker={.codec=&codec,.pin_nid=2};struct hda_controls controls;
    memset(&fake,0,sizeof fake);
    assert(!hda_controls_init(&controls,&codec,&speaker,NULL,2,3,NULL,0,false));
    held_controls=&controls;control_read_hook=control_transaction_reentry;
    struct audio_control_value value;
    assert(!hda_controls_read(&controls,HDA_CONTROL_PLAYBACK_VOLUME,&value));
    assert(controls.sense_pending && !controls.headphone_present);
    assert(hda_controls_service(&controls,8)==1 && !controls.sense_pending && controls.sense_inflight);
    assert(hda_controls_process_jack_sense(&controls,1u<<31)==1);
    assert(!hda_controls_read(&controls,HDA_CONTROL_HEADPHONE_JACK,&value) && value.values[0]==1);
}

static void test_auto_mute_disable_restores_speaker(void)
{
    struct hda_codec_node nodes[4]={0};struct hda_codec codec={.nodes=nodes};
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP|HDA_WCAP_STEREO,
        .amp_output_caps=(40u<<8)|(1u<<31)};nodes[3]=nodes[2];
    struct hda_route speaker={.codec=&codec,.pin_nid=2,.path={2},.path_count=1,.active=1};
    struct hda_route headphone={.codec=&codec,.pin_nid=3,.path={3},.path_count=1};
    struct hda_controls controls;memset(&fake,0,sizeof fake);
    fake.amp[0]=23;fake.amp[1]=19;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    controls.headphone_present=1;
    struct audio_control_value value={.values={1}};
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value));
    assert(!speaker.active && headphone.active);
    value.values[0]=0;
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value));
    assert(speaker.active && !headphone.active);
    assert(fake.amp[0]==23 && fake.amp[1]==19 && !controls.auto_mute);
}

static void test_auto_mute_shared_dac_keeps_headphones_audible(void)
{
    struct hda_codec_node nodes[4]={0};struct hda_codec codec={.nodes=nodes};
    nodes[1]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_AUDIO_OUTPUT,
        .widget_caps=HDA_WCAP_OUT_AMP|HDA_WCAP_STEREO,
        .amp_output_caps=(40u<<8)|(1u<<31)};
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN};nodes[3]=nodes[2];
    struct hda_route speaker={.codec=&codec,.pin_nid=2,.path={2,1},.path_count=2,.active=1};
    struct hda_route headphone={.codec=&codec,.pin_nid=3,.path={3,1},.path_count=2};
    struct hda_controls controls;memset(&fake,0,sizeof fake);fake.amp[0]=23;fake.amp[1]=19;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    controls.headphone_present=1;
    struct audio_control_value value={.values={1}};
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value));
    assert(!speaker.active && headphone.active && fake.amp[0]==23 && fake.amp[1]==19);
    assert(controls.playback_amp_nid==1 && !controls.playback_mute);
}

static void test_auto_mute_rejects_bound_route_without_owner(void)
{
    struct hda_codec_node nodes[4]={0};struct hda_codec codec={.nodes=nodes};
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP,.amp_output_caps=(40u<<8)|(1u<<31)};nodes[3]=nodes[2];
    struct hda_route speaker={.codec=&codec,.pin_nid=2,.path={2},.path_count=1,
        .active=1,.stream_bound=1};
    struct hda_route headphone={.codec=&codec,.pin_nid=3,.path={3},.path_count=1};
    struct hda_controls controls;memset(&fake,0,sizeof fake);fake.amp[0]=20;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    controls.headphone_present=1;
    struct audio_control_value value={.values={1}};
    assert(hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value)==-EBUSY);
    assert(speaker.active && !headphone.active && !fake.route_verb_count && !fake.command_count);
    assert(!controls.auto_mute && fake.amp[0]==20);
    speaker.stream_bound=0;speaker.stream_bind_dirty=1;
    assert(hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value)==-EBUSY);
    assert(!fake.route_verb_count && !fake.command_count);
}

static void test_auto_mute_amp_failure_restores_routes(void)
{
    struct hda_codec_node nodes[4]={0};struct hda_codec codec={.nodes=nodes};
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP|HDA_WCAP_STEREO,
        .amp_output_caps=(40u<<8)|(1u<<31)};nodes[3]=nodes[2];
    struct hda_route speaker={.codec=&codec,.pin_nid=2,.path={2},.path_count=1,.active=1};
    struct hda_route headphone={.codec=&codec,.pin_nid=3,.path={3},.path_count=1};
    struct hda_controls controls;memset(&fake,0,sizeof fake);fake.amp[0]=23;fake.amp[1]=19;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    controls.headphone_present=1;fake.fail_at=1;
    struct audio_control_value value={.values={1}};
    assert(hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&value)==-EIO);
    assert(speaker.active && !headphone.active && !controls.auto_mute);
    assert(fake.amp[0]==23 && fake.amp[1]==19);
}

static void test_amp_policy_does_not_replace_user_mute(void)
{
    struct hda_codec_node nodes[4]={0};struct hda_codec codec={.nodes=nodes};
    nodes[2]=(struct hda_codec_node){.present=1,.type=HDA_WIDGET_PIN,
        .widget_caps=HDA_WCAP_OUT_AMP|HDA_WCAP_STEREO,
        .amp_output_caps=(40u<<8)|(1u<<31)};
    struct hda_route speaker={.codec=&codec,.pin_nid=2};struct hda_controls controls;
    memset(&fake,0,sizeof fake);fake.amp[0]=23;fake.amp[1]=19;
    assert(!hda_controls_init(&controls,&codec,&speaker,NULL,2,0,NULL,0,false));
    controls.headphone_present=1;
    struct audio_control_value policy={.values={1}},value;
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&policy));
    assert(fake.amp[0]==(23|0x80) && fake.amp[1]==(19|0x80));
    assert(!hda_controls_read(&controls,HDA_CONTROL_PLAYBACK_MUTE,&value) && !value.values[0]);
    value.values[0]=25;value.values[1]=26;
    assert(!hda_controls_write(&controls,HDA_CONTROL_PLAYBACK_VOLUME,&value));
    assert(fake.amp[0]==(25|0x80) && fake.amp[1]==(26|0x80));
    value.values[0]=0;
    assert(!hda_controls_write(&controls,HDA_CONTROL_PLAYBACK_MUTE,&value));
    assert(fake.amp[0]==(25|0x80) && fake.amp[1]==(26|0x80));
    value.values[0]=1;
    assert(!hda_controls_write(&controls,HDA_CONTROL_PLAYBACK_MUTE,&value));
    policy.values[0]=0;
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&policy));
    assert(fake.amp[0]==(25|0x80) && fake.amp[1]==(26|0x80) && controls.playback_mute);
    value.values[0]=0;
    assert(!hda_controls_write(&controls,HDA_CONTROL_PLAYBACK_MUTE,&value));
    assert(fake.amp[0]==25 && fake.amp[1]==26);
}

int main(int argc,char **argv)
{
    if(argc==2){
        if(!strcmp(argv[1],"disable"))test_auto_mute_disable_restores_speaker();
        else if(!strcmp(argv[1],"shared"))test_auto_mute_shared_dac_keeps_headphones_audible();
        else if(!strcmp(argv[1],"bound"))test_auto_mute_rejects_bound_route_without_owner();
        else if(!strcmp(argv[1],"amp-failure"))test_auto_mute_amp_failure_restores_routes();
        else assert(!"unknown policy test");
        return 0;
    }
    test_amp_units();
    test_controls_and_atomic_rollback();
    test_converter_amp_without_pin_amp();
    test_jack_deduplicates();
    test_capture_source_and_mute_capability();
    test_route_rollback_on_third_verb();
    test_worker_preserves_failed_policy_and_zero_budget();
    test_unsolicited_requires_real_widget_capability();
    test_state_gate_does_not_wait_or_lose_pending_work();
    test_auto_mute_disable_restores_speaker();
    test_auto_mute_shared_dac_keeps_headphones_audible();
    test_auto_mute_rejects_bound_route_without_owner();
    test_auto_mute_amp_failure_restores_routes();
    test_amp_policy_does_not_replace_user_mute();
    return 0;
}
