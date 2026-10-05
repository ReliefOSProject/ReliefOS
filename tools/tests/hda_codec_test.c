#define HDA_TESTING 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/drivers/hda/hda.h"

#define PAGE_BYTES 4096u
#define FAKE_MAX_CODECS 2u
#define FAKE_MAX_NODES 48u
#define FAKE_MAX_RAW_CONNECTIONS 256u
#define FAKE_MAX_PAGE_RUNS 8u
#define FAKE_MAX_COMMANDS 8192u
#define FAKE_AMP_INDEX_COUNT 16u

#define VERB_GET_PARAMETER 0xf00u
#define VERB_GET_CONNECTION 0xf02u
#define VERB_GET_POWER 0xf05u
#define VERB_GET_CONNECTION_SELECT 0xf01u
#define VERB_GET_PIN_CONTROL 0xf07u
#define VERB_GET_EAPD 0xf0cu
#define VERB_GET_AMP 0x0bu
#define VERB_GET_CONFIG_DEFAULT 0xf1cu
#define VERB_GET_IMPLEMENTATION_ID 0xf20u
#define VERB_SET_POWER 0x705u
#define VERB_SET_CONNECTION_SELECT 0x701u
#define VERB_SET_PIN_CONTROL 0x707u
#define VERB_SET_EAPD 0x70cu
#define VERB_SET_AMP 0x03u
#define VERB_GET_STREAM_CHANNEL 0xf06u
#define VERB_SET_STREAM_CHANNEL 0x706u
#define VERB_GET_CONVERTER_FORMAT 0xa00u
#define VERB_SET_CONVERTER_FORMAT 0x02u

#define PARAM_VENDOR_ID 0x00u
#define PARAM_REVISION_ID 0x02u
#define PARAM_NODE_COUNT 0x04u
#define PARAM_FUNCTION_TYPE 0x05u
#define PARAM_AFG_CAPS 0x08u
#define PARAM_WIDGET_CAPS 0x09u
#define PARAM_PCM_RATES 0x0au
#define PARAM_STREAM_FORMATS 0x0bu
#define PARAM_PIN_CAPS 0x0cu
#define PARAM_AMP_INPUT_CAPS 0x0du
#define PARAM_CONNECTION_LENGTH 0x0eu
#define PARAM_AMP_OUTPUT_CAPS 0x12u

#define AMP_GET_OUTPUT (1u << 15)
#define AMP_SET_OUTPUT (1u << 15)
#define AMP_SET_INPUT (1u << 14)
#define AMP_GET_LEFT (1u << 13)
#define AMP_SET_LEFT (1u << 13)
#define AMP_SET_RIGHT (1u << 12)
#define AMP_SET_INDEX_SHIFT 8u
#define AMP_GET_INDEX_MASK 0x0fu
#define AMP_SET_MUTE (1u << 7)
#define AMP_GAIN_MASK 0x7fu
#define AMPCAP_MUTE (1u << 31)
#define AMPCAP_NUM_STEPS_SHIFT 8u
#define AMPCAP_NUM_STEPS_MASK (0x7fu << AMPCAP_NUM_STEPS_SHIFT)

struct fake_hda_node {
    uint8_t nid;
    uint8_t is_function_group;
    uint8_t type;
    uint8_t long_form;
    uint8_t raw_count;
    uint8_t connection_select;
    uint8_t pin_control;
    uint8_t pin_readonly;
    uint8_t eapd;
    uint8_t power_state;
    uint8_t power_act;
    uint8_t power_error;
    uint8_t power_delay_reads;
    uint8_t power_pending;
    uint8_t power_pending_act;
    uint8_t power_pending_reads;
    uint8_t stream_channel;
    uint8_t reserved_stream;
    uint16_t converter_format;
    uint16_t sub_start;
    uint16_t sub_count;
    uint16_t reserved2;
    uint32_t widget_caps;
    uint32_t pin_caps;
    uint32_t pin_default;
    uint32_t pcm_rates;
    uint32_t stream_formats;
    uint32_t afg_caps;
    uint32_t amp_input_caps;
    uint32_t amp_output_caps;
    uint32_t implementation_id;
    uint16_t raw_connections[FAKE_MAX_RAW_CONNECTIONS];
    uint16_t amp[2][FAKE_AMP_INDEX_COUNT][2];
};

struct fake_codec {
    uint8_t cad;
    uint8_t node_count;
    uint16_t root_start;
    uint16_t root_count;
    uint32_t vendor_id;
    uint32_t revision_id;
    struct fake_hda_node nodes[FAKE_MAX_NODES];
};

struct fake_page_run {
    uint64_t phys;
    uint32_t pages;
    uint8_t *memory;
    uint8_t live;
};

struct fake_command {
    uint8_t cad;
    uint8_t nid;
    uint8_t short_verb;
    uint8_t reserved;
    uint16_t verb;
    uint16_t payload;
    int32_t result;
};

static struct fake_codec fake_codecs[FAKE_MAX_CODECS];
static struct fake_page_run fake_runs[FAKE_MAX_PAGE_RUNS];
static struct fake_command fake_commands[FAKE_MAX_COMMANDS];
static struct reliefos_driver_kernel_api fake_api;
static struct hda_controller fake_controller;
static uint32_t fake_command_count;
static uint32_t fake_alloc_count;
static uint32_t fake_free_count;
static uint32_t fake_live_runs;
static uint32_t fake_alias_calls;
static uint64_t fake_next_phys;
static uint64_t fake_alias_fail_phys;
static uint64_t fake_alias_discontiguous_phys;
static uint32_t fake_fail_alloc_at;
static uint8_t fake_dead;
static uint8_t fake_drop_after_set;
static uint8_t fake_drop_nid;
static uint16_t fake_drop_verb;
static uint8_t fake_power_error_after_set;
static uint8_t fake_power_error_nid;
static uint8_t fake_fail_once;
static uint8_t fake_fail_nid;
static uint16_t fake_fail_verb;
static uint16_t fake_fail_payload_mask;
static uint16_t fake_fail_payload_value;
static uint32_t fake_fail_occurrence;
static uint32_t fake_fail_seen;
static int32_t fake_fail_error;
static uint8_t fake_cleanup_fail_once;
static uint8_t fake_cleanup_fail_nid;
static uint16_t fake_cleanup_fail_verb;
static uint16_t fake_cleanup_fail_payload_mask;
static uint16_t fake_cleanup_fail_payload_value;
static int32_t fake_cleanup_fail_error;
static uint8_t fake_override_get;
static uint8_t fake_override_nid;
static uint16_t fake_override_verb;
static uint8_t fake_override_value;
static struct hda_codec *fake_nested_codec;
static struct hda_route *fake_nested_route;
static int fake_nested_result;
static uint8_t fake_nested_once;
static char fake_console_text[32768];
static uint32_t fake_console_length;
static uint64_t fake_tick_count;
static uint32_t fake_sleep_count;
static uint32_t fake_afg_d0_ready_at;
static uint32_t fake_pin_d0_ready_at;

static void fake_reset(void);
static void fake_build_primary(void);
static void fake_build_secondary(void);
static struct fake_hda_node *fake_add_node(struct fake_codec *codec,
                                           uint8_t nid, uint8_t type,
                                           uint32_t caps);
static struct fake_hda_node *fake_node(struct fake_codec *codec, uint8_t nid);
static void fake_add_connection(struct fake_hda_node *node, uint16_t raw);
static uint32_t fake_pin_default(uint8_t device, uint8_t association,
                                 uint8_t sequence);
static struct fake_codec *fake_codec_by_cad(uint8_t cad);
static uint64_t fake_api_alloc_pages(uint32_t page_count);
static void fake_api_free_pages(uint64_t phys, uint32_t page_count);
static void fake_api_console(const char *text);
static uint64_t fake_api_ticks(void);
static void fake_api_sleep_ms(uint64_t milliseconds);
static uint32_t fake_param(struct fake_codec *codec, struct fake_hda_node *node,
                           uint8_t parameter);
static int fake_command_fail(struct fake_command *command);
static int fake_has_command(uint32_t start, uint8_t nid, uint16_t verb,
                            uint16_t payload);
static int fake_amp(struct fake_hda_node *node, uint16_t payload,
                    uint8_t is_set, uint32_t *out_value);
static void fake_test_connections(void);
static void fake_test_two_codecs_and_routes(void);
static void fake_test_probe_failures(void);
static void fake_test_route_rollback_and_serialization(void);
static void fake_test_amp_restore_channel_failures(void);
static void fake_test_power_error_is_rejected(void);
static void fake_test_power_delay_timeout_and_restore(void);
static void fake_test_power_fresh_get_after_set(void);
static void fake_test_invalid_amp_zero_db_offset(void);
static void fake_test_amp_zero_db_and_mono(void);
static void fake_test_shared_afg_lease_and_conflicts(void);
static void fake_test_route_groups(void);
static void fake_test_stream_binding(void);
static void fake_test_pin_gate_without_mute_and_retention(void);
static void fake_test_diagnostic(void);
static void fake_test_diagnostic_256_connections(void);

/** @brief Supply a CPU alias only for pages allocated by this host fixture. */
void *hda_phys_to_direct_map_page(uint64_t phys)
{
    ++fake_alias_calls;
    for (uint32_t i = 0; i < FAKE_MAX_PAGE_RUNS; ++i) {
        struct fake_page_run *run = &fake_runs[i];
        uint64_t bytes = (uint64_t)run->pages * PAGE_BYTES;
        if (run->live && phys >= run->phys && phys - run->phys < bytes &&
            ((phys - run->phys) & (PAGE_BYTES - 1u)) == 0u) {
            if (phys == fake_alias_fail_phys) return NULL;
            uint8_t *alias = run->memory + (size_t)(phys - run->phys);
            if (phys == fake_alias_discontiguous_phys) return alias + 1u;
            return alias;
        }
    }
    return NULL;
}

/** @brief Model the controller's task-context sleep boundary with fake H1 time. */
void hda_controller_sleep_ms(struct hda_controller *controller,
                             uint64_t milliseconds)
{
    assert(controller && controller->api && controller->api->sleep_ms);
    controller->api->sleep_ms(milliseconds);
}

/** @brief Implement only the accepted serialized-verb hardware boundary. */
int hda_exec_verb(struct hda_controller *controller, uint8_t cad, uint8_t nid,
                  uint16_t verb, uint16_t payload, bool short_verb,
                  uint32_t *out_response)
{
    (void)controller;
    assert(out_response != NULL);
    struct fake_command command = {
        .cad = cad, .nid = nid, .short_verb = short_verb ? 1u : 0u,
        .verb = verb, .payload = payload,
    };
    if (fake_command_count < FAKE_MAX_COMMANDS)
        fake_commands[fake_command_count++] = command;
    int fail = fake_command_fail(&command);
    if (fail) return fail;
    if (fake_cleanup_fail_once && command.nid == fake_cleanup_fail_nid &&
        command.verb == fake_cleanup_fail_verb &&
        (command.payload & fake_cleanup_fail_payload_mask) ==
            fake_cleanup_fail_payload_value) {
        fake_cleanup_fail_once = 0u;
        return fake_cleanup_fail_error;
    }
    if (fake_dead) return -ETIMEDOUT;

    struct fake_codec *codec = fake_codec_by_cad(cad);
    if (!codec) return -ENODEV;
    struct fake_hda_node *node = nid ? fake_node(codec, nid) : NULL;
    if (verb == VERB_GET_PARAMETER && !short_verb) {
        if (nid && !node) return -ENODEV;
        *out_response = fake_param(codec, node, (uint8_t)payload);
        return *out_response == UINT32_MAX ? -EINVAL : 0;
    }
    if (verb == VERB_GET_IMPLEMENTATION_ID && !short_verb && node &&
        node->is_function_group) {
        *out_response = node->implementation_id;
        return 0;
    }
    if (verb == VERB_GET_CONNECTION && !short_verb && node) {
        uint32_t result = 0;
        uint32_t entries = node->long_form ? 2u : 4u;
        for (uint32_t i = 0; i < entries; ++i) {
            uint32_t index = (uint32_t)payload + i;
            uint32_t shift = i * (node->long_form ? 16u : 8u);
            if (index < node->raw_count)
                result |= (uint32_t)node->raw_connections[index] << shift;
        }
        *out_response = result;
        return 0;
    }
    if (!node) return -ENODEV;
    if (verb == VERB_GET_POWER && !short_verb) {
        if (node->power_pending) {
            if (node->power_pending_reads) {
                --node->power_pending_reads;
            } else {
                node->power_act = node->power_pending_act;
                node->power_pending = 0u;
            }
        }
        *out_response = ((uint32_t)(node->power_error != 0u) << 8) |
                        ((uint32_t)(node->power_act & 0x0fu) << 4) |
                        (node->power_state & 0x0fu);
        if (cad == 2u && node->power_state == 0u && node->power_act == 0u &&
            !node->power_error && fake_command_count) {
            if (nid == 1u) fake_afg_d0_ready_at = fake_command_count - 1u;
            if (nid == 2u) fake_pin_d0_ready_at = fake_command_count - 1u;
        }
    } else if (verb == VERB_GET_CONFIG_DEFAULT && !short_verb) {
        *out_response = node->pin_default;
    } else if (verb == VERB_GET_CONNECTION_SELECT && !short_verb) {
        *out_response = node->connection_select;
    } else if (verb == VERB_GET_PIN_CONTROL && !short_verb) {
        *out_response = node->pin_control;
    } else if (verb == VERB_GET_EAPD && !short_verb) {
        *out_response = node->eapd;
    } else if (verb == VERB_GET_AMP && short_verb) {
        int ret = fake_amp(node, payload, 0u, out_response);
        if (ret) return ret;
    } else if (verb == VERB_SET_POWER && !short_verb) {
        node->power_state = (uint8_t)(payload & 0x0fu);
        if (node->power_delay_reads) {
            node->power_pending = 1u;
            node->power_pending_act = node->power_state;
            node->power_pending_reads = node->power_delay_reads;
        } else {
            node->power_act = node->power_state;
            node->power_pending = 0u;
        }
        if (fake_power_error_after_set && nid == fake_power_error_nid) {
            node->power_error = 1u;
            fake_power_error_after_set = 0u;
        }
        *out_response = 0u;
    } else if (verb == VERB_SET_CONNECTION_SELECT && !short_verb) {
        node->connection_select = (uint8_t)payload;
        *out_response = 0u;
    } else if (verb == VERB_SET_PIN_CONTROL && !short_verb) {
        if (!node->pin_readonly) node->pin_control = (uint8_t)payload;
        *out_response = 0u;
    } else if (verb == VERB_SET_EAPD && !short_verb) {
        node->eapd = (uint8_t)payload;
        *out_response = 0u;
    } else if (verb == VERB_GET_STREAM_CHANNEL && !short_verb) {
        *out_response = node->stream_channel;
    } else if (verb == VERB_SET_STREAM_CHANNEL && !short_verb) {
        node->stream_channel = (uint8_t)payload;
        *out_response = 0u;
    } else if (verb == VERB_GET_CONVERTER_FORMAT && !short_verb) {
        *out_response = node->converter_format;
    } else if (verb == VERB_SET_CONVERTER_FORMAT && short_verb) {
        node->converter_format = payload;
        *out_response = 0u;
    } else if (verb == VERB_SET_AMP && short_verb) {
        int ret = fake_amp(node, payload, 1u, out_response);
        if (ret) return ret;
    } else {
        return -EINVAL;
    }

    if (fake_override_get && cad == fake_codec_by_cad(cad)->cad &&
        nid == fake_override_nid && verb == fake_override_verb) {
        *out_response = fake_override_value;
        fake_override_get = 0u;
    }
    if (fake_nested_once && fake_nested_codec && fake_nested_route &&
        ((verb == VERB_SET_AMP && short_verb) ||
         verb == VERB_SET_POWER || verb == VERB_SET_CONNECTION_SELECT ||
         verb == VERB_SET_PIN_CONTROL || verb == VERB_SET_EAPD)) {
        fake_nested_once = 0u;
        fake_nested_result = hda_apply_route(fake_nested_codec,
                                             fake_nested_route, true);
    }
    if (fake_drop_after_set && nid == fake_drop_nid && verb == fake_drop_verb) {
        fake_drop_after_set = 0u;
        fake_dead = 1u;
        return -ETIMEDOUT;
    }
    return 0;
}

static void fake_reset(void)
{
    for (uint32_t i = 0; i < FAKE_MAX_PAGE_RUNS; ++i) {
        if (fake_runs[i].live) free(fake_runs[i].memory);
    }
    memset(fake_codecs, 0, sizeof(fake_codecs));
    memset(fake_runs, 0, sizeof(fake_runs));
    memset(fake_commands, 0, sizeof(fake_commands));
    memset(&fake_controller, 0, sizeof(fake_controller));
    memset(&fake_api, 0, sizeof(fake_api));
    memset(fake_console_text, 0, sizeof(fake_console_text));
    fake_command_count = 0u;
    fake_alloc_count = 0u;
    fake_free_count = 0u;
    fake_live_runs = 0u;
    fake_alias_calls = 0u;
    fake_next_phys = 0x00100000u;
    fake_alias_fail_phys = 0u;
    fake_alias_discontiguous_phys = 0u;
    fake_fail_alloc_at = 0u;
    fake_dead = 0u;
    fake_drop_after_set = 0u;
    fake_drop_nid = 0u;
    fake_drop_verb = 0u;
    fake_power_error_after_set = 0u;
    fake_power_error_nid = 0u;
    fake_fail_once = 0u;
    fake_fail_nid = 0u;
    fake_fail_verb = 0u;
    fake_fail_payload_mask = 0u;
    fake_fail_payload_value = 0u;
    fake_fail_occurrence = 0u;
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    fake_cleanup_fail_once = 0u;
    fake_cleanup_fail_nid = 0u;
    fake_cleanup_fail_verb = 0u;
    fake_cleanup_fail_payload_mask = 0u;
    fake_cleanup_fail_payload_value = 0u;
    fake_cleanup_fail_error = -ETIMEDOUT;
    fake_override_get = 0u;
    fake_override_nid = 0u;
    fake_override_verb = 0u;
    fake_override_value = 0u;
    fake_nested_codec = NULL;
    fake_nested_route = NULL;
    fake_nested_result = 0;
    fake_nested_once = 0u;
    fake_console_length = 0u;
    fake_tick_count = 0u;
    fake_sleep_count = 0u;
    fake_afg_d0_ready_at = UINT32_MAX;
    fake_pin_d0_ready_at = UINT32_MAX;
    fake_controller.codec_mask = (1u << 2) | (1u << 7);
    fake_controller.api = &fake_api;
    fake_controller.initialized = 1u;
    fake_controller.transport_mode = HDA_TRANSPORT_RINGS;
    fake_api.abi_version = RELIEFOS_DRIVER_ABI_VERSION;
    fake_api.struct_size = sizeof(fake_api);
    fake_api.alloc_pages = fake_api_alloc_pages;
    fake_api.free_pages = fake_api_free_pages;
    fake_api.console_write = fake_api_console;
    fake_api.ticks = fake_api_ticks;
    fake_api.sleep_ms = fake_api_sleep_ms;
    fake_build_primary();
    fake_build_secondary();
}

static struct fake_hda_node *fake_add_node(struct fake_codec *codec,
                                           uint8_t nid, uint8_t type,
                                           uint32_t caps)
{
    assert(codec->node_count < FAKE_MAX_NODES);
    struct fake_hda_node *node = &codec->nodes[codec->node_count++];
    memset(node, 0, sizeof(*node));
    node->nid = nid;
    node->type = type;
    node->widget_caps = caps;
    /* Wire WidgetCaps bit 0: model two real channels independently. */
    node->widget_caps |= 1u;
    node->power_state = 3u; /* Non-D0 wire state: routes must explicitly power it. */
    node->power_act = 3u;
    node->amp[0][0][0] = 0x10u;
    node->amp[0][0][1] = 0x12u;
    node->amp[1][0][0] = 0x18u;
    node->amp[1][0][1] = 0x1au;
    return node;
}

static struct fake_hda_node *fake_node(struct fake_codec *codec, uint8_t nid)
{
    if (!codec) return NULL;
    for (uint32_t i = 0; i < codec->node_count; ++i)
        if (codec->nodes[i].nid == nid) return &codec->nodes[i];
    return NULL;
}

static void fake_add_connection(struct fake_hda_node *node, uint16_t raw)
{
    assert(node->raw_count < FAKE_MAX_RAW_CONNECTIONS);
    node->raw_connections[node->raw_count++] = raw;
}

static uint32_t fake_pin_default(uint8_t device, uint8_t association,
                                 uint8_t sequence)
{
    return ((uint32_t)(device & 0x0fu) << 20) |
           ((uint32_t)(association & 0x0fu) << 4) | (sequence & 0x0fu);
}

static void fake_build_primary(void)
{
    struct fake_codec *codec = &fake_codecs[0];
    codec->cad = 2u;
    codec->root_start = 1u;
    codec->root_count = 1u;
    codec->vendor_id = 0x10ec0897u;
    codec->revision_id = 0x00100402u;

    struct fake_hda_node *fg = fake_add_node(codec, 1u,
        HDA_WIDGET_FUNCTION_GROUP, 0u);
    fg->is_function_group = 1u;
    fg->implementation_id = 0x12345678u;
    fg->sub_start = 2u;
    fg->sub_count = 46u;
    fg->afg_caps = 0x00020403u;
    fg->pcm_rates = (1u << 6) | (1u << 17); /* 48 kHz, 16-bit. */
    fg->stream_formats = 1u;
    fg->amp_input_caps = AMPCAP_MUTE | (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 1u;
    fg->amp_output_caps = AMPCAP_MUTE | (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 2u;

    struct fake_hda_node *pin2 = fake_add_node(codec, 2u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_OUT_AMP | HDA_WCAP_POWER);
    pin2->pin_caps = HDA_PINCAP_OUTPUT | HDA_PINCAP_EAPD;
    pin2->pin_default = fake_pin_default(1u, 1u, 0u);
    pin2->connection_select = 0u;
    fake_add_connection(pin2, 13u); /* Alternate dead cycle, prior selection. */
    fake_add_connection(pin2, 4u);

    struct fake_hda_node *pin3 = fake_add_node(codec, 3u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_OUT_AMP | HDA_WCAP_POWER);
    pin3->pin_caps = HDA_PINCAP_OUTPUT;
    pin3->pin_default = fake_pin_default(1u, 2u, 1u);
    fake_add_connection(pin3, 4u);

    struct fake_hda_node *mixer4 = fake_add_node(codec, 4u, HDA_WIDGET_MIXER,
        HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP | HDA_WCAP_POWER);
    fake_add_connection(mixer4, 5u);

    struct fake_hda_node *selector5 = fake_add_node(codec, 5u, HDA_WIDGET_SELECTOR,
        HDA_WCAP_CONN_LIST | HDA_WCAP_AMP_OVERRIDE | HDA_WCAP_OUT_AMP |
        HDA_WCAP_POWER);
    selector5->connection_select = 1u;
    selector5->amp_output_caps = AMPCAP_MUTE | (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 3u;
    selector5->long_form = 1u;
    fake_add_connection(selector5, 6u);
    fake_add_connection(selector5, 0x8007u);

    struct fake_hda_node *dac6 = fake_add_node(codec, 6u,
        HDA_WIDGET_AUDIO_OUTPUT, HDA_WCAP_OUT_AMP | HDA_WCAP_POWER);
    (void)dac6;
    (void)fake_add_node(codec, 7u, HDA_WIDGET_AUDIO_OUTPUT, HDA_WCAP_POWER);

    struct fake_hda_node *adc8 = fake_add_node(codec, 8u,
        1u, HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP | HDA_WCAP_POWER);
    adc8->connection_select = 0u;
    fake_add_connection(adc8, 11u); /* Alternate dead capture input. */
    fake_add_connection(adc8, 9u);

    struct fake_hda_node *selector9 = fake_add_node(codec, 9u,
        HDA_WIDGET_SELECTOR, HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP | HDA_WCAP_POWER);
    fake_add_connection(selector9, 11u);
    fake_add_connection(selector9, 10u);

    struct fake_hda_node *mic10 = fake_add_node(codec, 10u, HDA_WIDGET_PIN,
        HDA_WCAP_IN_AMP | HDA_WCAP_POWER);
    mic10->pin_caps = HDA_PINCAP_INPUT | HDA_PINCAP_VREF_80;
    mic10->pin_default = fake_pin_default(10u, 2u, 0u);

    struct fake_hda_node *dead_input11 = fake_add_node(codec, 11u,
        HDA_WIDGET_PIN, HDA_WCAP_POWER);
    dead_input11->pin_caps = HDA_PINCAP_INPUT;
    dead_input11->pin_default = fake_pin_default(10u, 0u, 0u);

    struct fake_hda_node *cycle_pin12 = fake_add_node(codec, 12u,
        HDA_WIDGET_PIN, HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    cycle_pin12->pin_caps = HDA_PINCAP_OUTPUT;
    cycle_pin12->pin_default = fake_pin_default(1u, 3u, 0u);
    fake_add_connection(cycle_pin12, 13u);
    struct fake_hda_node *cycle_mixer13 = fake_add_node(codec, 13u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    fake_add_connection(cycle_mixer13, 14u);
    struct fake_hda_node *cycle_selector14 = fake_add_node(codec, 14u,
        HDA_WIDGET_SELECTOR, HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    fake_add_connection(cycle_selector14, 13u);
    (void)fake_add_node(codec, 15u, 0x0fu, 0u);

    struct fake_hda_node *pin16 = fake_add_node(codec, 16u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    pin16->pin_caps = HDA_PINCAP_OUTPUT;
    pin16->pin_default = fake_pin_default(1u, 1u, 2u);
    fake_add_connection(pin16, 17u);
    struct fake_hda_node *mixer17 = fake_add_node(codec, 17u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP |
        HDA_WCAP_POWER);
    fake_add_connection(mixer17, 6u);
    fake_add_connection(mixer17, 18u);
    struct fake_hda_node *dac18 = fake_add_node(codec, 18u,
        HDA_WIDGET_AUDIO_OUTPUT, HDA_WCAP_POWER);
    dac18->widget_caps &= ~HDA_WCAP_STEREO;

    struct fake_hda_node *pin19 = fake_add_node(codec, 19u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    pin19->pin_caps = HDA_PINCAP_OUTPUT;
    pin19->pin_default = fake_pin_default(1u, 2u, 3u);
    fake_add_connection(pin19, 20u);
    struct fake_hda_node *mixer20 = fake_add_node(codec, 20u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP |
        HDA_WCAP_POWER);
    fake_add_connection(mixer20, 6u);

    for (uint32_t member = 0u; member < 9u; ++member) {
        uint8_t pin_nid = (uint8_t)(21u + member * 3u);
        struct fake_hda_node *pin = fake_add_node(codec, pin_nid,
            HDA_WIDGET_PIN, HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
        pin->pin_caps = HDA_PINCAP_OUTPUT;
        pin->pin_default = fake_pin_default(1u,
            member == 8u ? 5u : 4u,
            member == 8u ? 1u : (uint8_t)(member * 2u));
        fake_add_connection(pin, (uint16_t)(pin_nid + 1u));
        struct fake_hda_node *mixer = fake_add_node(codec,
            (uint8_t)(pin_nid + 1u), HDA_WIDGET_MIXER,
            HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP | HDA_WCAP_POWER);
        fake_add_connection(mixer, (uint16_t)(pin_nid + 2u));
        (void)fake_add_node(codec, (uint8_t)(pin_nid + 2u),
                            HDA_WIDGET_AUDIO_OUTPUT, HDA_WCAP_POWER);
    }
}

static void fake_build_secondary(void)
{
    struct fake_codec *codec = &fake_codecs[1];
    codec->cad = 7u;
    codec->root_start = 4u;
    codec->root_count = 2u;
    codec->vendor_id = 0x1af40001u;
    codec->revision_id = 0x00010001u;

    struct fake_hda_node *fg = fake_add_node(codec, 4u,
        HDA_WIDGET_FUNCTION_GROUP, 0u);
    fg->is_function_group = 1u;
    fg->implementation_id = 0x87654321u;
    fg->sub_start = 30u;
    fg->sub_count = 23u;
    fg->pcm_rates = (1u << 6) | (1u << 17);
    fg->stream_formats = 1u;
    fg->amp_input_caps = AMPCAP_MUTE | (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 1u;
    fg->amp_output_caps = AMPCAP_MUTE | (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 1u;

    struct fake_hda_node *fg_second = fake_add_node(codec, 5u,
        HDA_WIDGET_FUNCTION_GROUP, 0u);
    fg_second->is_function_group = 1u;
    fg_second->implementation_id = 0xabcdef01u;
    fg_second->sub_start = 60u;
    fg_second->sub_count = 0u;

    struct fake_hda_node *pin30 = fake_add_node(codec, 30u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    pin30->pin_caps = HDA_PINCAP_OUTPUT;
    pin30->pin_default = fake_pin_default(1u, 4u, 0u);
    fake_add_connection(pin30, 31u);
    struct fake_hda_node *mixer31 = fake_add_node(codec, 31u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_IN_AMP |
        HDA_WCAP_POWER);
    /* Vendor and digital branches precede the analog DAC and must not become
     * analog routes. The direct DAC edge is index 16, outside amp encoding. */
    struct fake_hda_node *vendor32 = fake_add_node(codec, 32u, 0x0fu,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    fake_add_connection(mixer31, 32u);
    fake_add_connection(vendor32, 48u);
    struct fake_hda_node *digital33 = fake_add_node(codec, 33u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_DIGITAL |
        HDA_WCAP_POWER);
    fake_add_connection(mixer31, 33u);
    fake_add_connection(digital33, 48u);
    for (uint8_t nid = 34u; nid < 48u; ++nid) {
        (void)fake_add_node(codec, nid, 0x0fu, 0u);
        fake_add_connection(mixer31, nid);
    }
    (void)fake_add_node(codec, 48u, HDA_WIDGET_AUDIO_OUTPUT, HDA_WCAP_POWER);
    fake_add_connection(mixer31, 48u);
    struct fake_hda_node *pin49 = fake_add_node(codec, 49u, HDA_WIDGET_PIN,
        HDA_WCAP_POWER);
    pin49->pin_caps = HDA_PINCAP_INPUT;
    pin49->pin_default = fake_pin_default(10u, 5u, 0u);
    struct fake_hda_node *pin50 = fake_add_node(codec, 50u, HDA_WIDGET_PIN,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    pin50->pin_caps = HDA_PINCAP_OUTPUT;
    pin50->pin_default = fake_pin_default(1u, 6u, 0u);
    fake_add_connection(pin50, 51u);
    struct fake_hda_node *vendor51 = fake_add_node(codec, 51u, 0x0fu,
        HDA_WCAP_CONN_LIST | HDA_WCAP_POWER);
    fake_add_connection(vendor51, 52u);
    struct fake_hda_node *digital52 = fake_add_node(codec, 52u,
        HDA_WIDGET_MIXER, HDA_WCAP_CONN_LIST | HDA_WCAP_DIGITAL |
        HDA_WCAP_POWER);
    fake_add_connection(digital52, 48u);
}

static struct fake_codec *fake_codec_by_cad(uint8_t cad)
{
    for (uint32_t i = 0; i < FAKE_MAX_CODECS; ++i)
        if (fake_codecs[i].cad == cad) return &fake_codecs[i];
    return NULL;
}

static uint64_t fake_api_alloc_pages(uint32_t page_count)
{
    ++fake_alloc_count;
    if (!page_count || (fake_fail_alloc_at && fake_alloc_count == fake_fail_alloc_at))
        return 0u;
    for (uint32_t i = 0; i < FAKE_MAX_PAGE_RUNS; ++i) {
        if (!fake_runs[i].live) {
            uint64_t bytes = (uint64_t)page_count * PAGE_BYTES;
            fake_runs[i].memory = calloc(1u, (size_t)bytes);
            if (!fake_runs[i].memory) return 0u;
            fake_runs[i].phys = fake_next_phys;
            fake_runs[i].pages = page_count;
            fake_runs[i].live = 1u;
            fake_next_phys += bytes;
            ++fake_live_runs;
            return fake_runs[i].phys;
        }
    }
    return 0u;
}

static void fake_api_free_pages(uint64_t phys, uint32_t page_count)
{
    for (uint32_t i = 0; i < FAKE_MAX_PAGE_RUNS; ++i) {
        struct fake_page_run *run = &fake_runs[i];
        if (run->live && run->phys == phys && run->pages == page_count) {
            free(run->memory);
            memset(run, 0, sizeof(*run));
            ++fake_free_count;
            --fake_live_runs;
            return;
        }
    }
    assert(!"free_pages must receive the original physical base and page count");
}

static void fake_api_console(const char *text)
{
    size_t length = strlen(text);
    if (length > sizeof(fake_console_text) - fake_console_length - 1u)
        length = sizeof(fake_console_text) - fake_console_length - 1u;
    memcpy(fake_console_text + fake_console_length, text, length);
    fake_console_length += (uint32_t)length;
    fake_console_text[fake_console_length] = '\0';
}

static uint32_t fake_param(struct fake_codec *codec, struct fake_hda_node *node,
                           uint8_t parameter)
{
    if (!node) {
        switch (parameter) {
        case PARAM_VENDOR_ID: return codec->vendor_id;
        case PARAM_REVISION_ID: return codec->revision_id;
        case PARAM_NODE_COUNT:
            return ((uint32_t)codec->root_start << 16) | codec->root_count;
        default: return UINT32_MAX;
        }
    }
    if (node->is_function_group) {
        switch (parameter) {
        case PARAM_NODE_COUNT:
            return ((uint32_t)node->sub_start << 16) | node->sub_count;
        case PARAM_FUNCTION_TYPE: return 1u;
        case PARAM_AFG_CAPS: return node->afg_caps;
        case PARAM_PCM_RATES: return node->pcm_rates;
        case PARAM_STREAM_FORMATS: return node->stream_formats;
        case PARAM_AMP_INPUT_CAPS: return node->amp_input_caps;
        case PARAM_AMP_OUTPUT_CAPS: return node->amp_output_caps;
        default: return UINT32_MAX;
        }
    }
    switch (parameter) {
    case PARAM_WIDGET_CAPS:
        return node->widget_caps | ((uint32_t)node->type << 20);
    case PARAM_PIN_CAPS: return node->type == HDA_WIDGET_PIN ? node->pin_caps : 0u;
    case PARAM_PCM_RATES: return node->pcm_rates;
    case PARAM_STREAM_FORMATS: return node->stream_formats;
    case PARAM_AMP_INPUT_CAPS: return node->amp_input_caps;
    case PARAM_AMP_OUTPUT_CAPS: return node->amp_output_caps;
    case PARAM_CONNECTION_LENGTH:
        return (node->long_form ? 0x80u : 0u) | node->raw_count;
    default: return UINT32_MAX;
    }
}

static uint64_t fake_api_ticks(void)
{
    return fake_tick_count;
}

static void fake_api_sleep_ms(uint64_t milliseconds)
{
    uint64_t ticks = (milliseconds * 100u + 999u) / 1000u;
    ++fake_sleep_count;
    fake_tick_count += ticks ? ticks : 1u;
}

static int fake_command_fail(struct fake_command *command)
{
    if (!fake_fail_once || command->nid != fake_fail_nid ||
        command->verb != fake_fail_verb) return 0;
    if ((command->payload & fake_fail_payload_mask) !=
        fake_fail_payload_value) return 0;
    if (++fake_fail_seen != fake_fail_occurrence) return 0;
    fake_fail_once = 0u;
    return fake_fail_error;
}

static int fake_has_command(uint32_t start, uint8_t nid, uint16_t verb,
                            uint16_t payload)
{
    for (uint32_t i = start; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid == nid && command->verb == verb &&
            command->payload == payload)
            return 1;
    }
    return 0;
}

static int fake_amp(struct fake_hda_node *node, uint16_t payload,
                    uint8_t is_set, uint32_t *out_value)
{
    uint8_t input;
    uint8_t output;
    uint8_t left;
    uint8_t right;
    uint32_t index;
    if (is_set) {
        uint16_t direction = payload & 0xc000u;
        if (direction != AMP_SET_INPUT && direction != AMP_SET_OUTPUT)
            return -EINVAL;
        input = direction == AMP_SET_INPUT;
        output = direction == AMP_SET_OUTPUT;
        index = (payload >> AMP_SET_INDEX_SHIFT) & 0x0fu;
        if (output && index != 0u) return -EINVAL;
        left = (payload & AMP_SET_LEFT) != 0u;
        right = (payload & AMP_SET_RIGHT) != 0u;
    } else {
        /* GET has bit 15=output, bit 14 reserved, bit 13=left, bit 12
         * reserved, bits 11:4 reserved, and a four-bit input index. */
        if (payload & 0x5ff0u) return -EINVAL;
        output = (payload & AMP_GET_OUTPUT) != 0u;
        input = !output;
        index = payload & AMP_GET_INDEX_MASK;
        left = (payload & 0x2000u) != 0u;
        right = !left;
    }
    if (index >= FAKE_AMP_INDEX_COUNT) return -ERANGE;
    if (is_set) {
        uint32_t caps = input ? node->amp_input_caps : node->amp_output_caps;
        uint16_t value = (uint16_t)(payload & AMP_GAIN_MASK);
        if (!caps || (caps & AMPCAP_MUTE))
            value |= (uint16_t)(payload & AMP_SET_MUTE);
        if (!(node->widget_caps & 1u)) {
            /* A mono widget ignores SET channel flags and has only LEFT. */
            node->amp[input][index][0] = value;
        } else {
            if (left) node->amp[input][index][0] = value;
            if (right) node->amp[input][index][1] = value;
            if (!left && !right) return -EINVAL;
        }
        *out_value = 0u;
        return 0;
    }
    if (right && !(node->widget_caps & 1u)) *out_value = 0u;
    else *out_value = node->amp[input][index][left ? 0u : 1u];
    return 0;
}

static void fake_test_connections(void)
{
    uint16_t short_range[] = {2u, 0x85u};
    uint16_t long_range[] = {6u, 0x8008u};
    uint16_t first_is_range[] = {0x83u};
    uint16_t out_of_nid[] = {0x0100u};
    uint8_t out[8];

    int count = hda_expand_connections(short_range, 2u, false, 1u, 15u,
                                       out, 8u);
    assert(count == 4 && out[0] == 2u && out[1] == 3u &&
           out[2] == 4u && out[3] == 5u);
    assert(hda_expand_connections(short_range, 2u, false, 1u, 3u,
                                  out, 8u) == -EINVAL);
    count = hda_expand_connections(long_range, 2u, true, 6u, 4u, out, 8u);
    assert(count == 3 && out[0] == 6u && out[1] == 7u && out[2] == 8u);
    assert(hda_expand_connections(first_is_range, 1u, false, 0u, 16u,
                                  out, 8u) == -EINVAL);
    assert(hda_expand_connections(out_of_nid, 1u, true, 0u, 256u,
                                  out, 8u) == -EINVAL);
    assert(hda_expand_connections(long_range, 2u, true, 6u, 4u, out, 2u) ==
           -EOVERFLOW);
}

static void fake_test_two_codecs_and_routes(void)
{
    fake_reset();
    struct hda_codec primary = {0};
    struct hda_codec secondary = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &primary) == 0);
    assert(hda_codec_probe(&fake_controller, 7u, &secondary) == 0);
    assert(primary.vendor_id == 0x10ec0897u &&
           primary.revision_id == 0x00100402u &&
           primary.subsystem_id == 0x12345678u);
    assert(secondary.vendor_id == 0x1af40001u && secondary.cad == 7u);
    assert(secondary.subsystem_id == 0x87654321u &&
           secondary.nodes[4].implementation_id == 0x87654321u &&
           secondary.nodes[5].implementation_id == 0xabcdef01u);
    uint8_t primary_iid_query = 0u;
    uint8_t secondary_first_iid_query = 0u;
    uint8_t secondary_second_iid_query = 0u;
    uint8_t root_subsystem_query = 0u;
    for (uint32_t i = 0u; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->verb == VERB_GET_IMPLEMENTATION_ID &&
            !command->short_verb) {
            primary_iid_query |= command->cad == 2u && command->nid == 1u;
            secondary_first_iid_query |= command->cad == 7u &&
                                         command->nid == 4u;
            secondary_second_iid_query |= command->cad == 7u &&
                                          command->nid == 5u;
        }
        if (command->cad == 7u && command->nid == 0u &&
            command->verb == VERB_GET_PARAMETER && command->payload == 1u)
            root_subsystem_query = 1u;
    }
    assert(primary_iid_query && secondary_first_iid_query &&
           secondary_second_iid_query && !root_subsystem_query);
    assert(primary.has_playback && primary.has_capture);
    assert(secondary.has_playback && !secondary.has_capture);
    assert(secondary.capture_pcm_rates == 0u &&
           secondary.capture_stream_formats == 0u);
    struct hda_route digital_or_vendor = {0};
    assert(hda_find_route(&secondary, 50u, HDA_ROUTE_PLAYBACK,
                          &digital_or_vendor) == -ENODEV);
    struct hda_route wide_amp_route = {0};
    assert(hda_find_route(&secondary, 30u, HDA_ROUTE_PLAYBACK,
                          &wide_amp_route) == 0);
    assert(wide_amp_route.path_count == 3u &&
           wide_amp_route.path[0] == 30u && wide_amp_route.path[1] == 31u &&
           wide_amp_route.path[2] == 48u &&
           wide_amp_route.connection_index[1] == 16u);
    uint32_t commands_before_wide_apply = fake_command_count;
    assert(hda_apply_route(&secondary, &wide_amp_route, true) == -ERANGE);
    for (uint32_t i = commands_before_wide_apply; i < fake_command_count; ++i) {
        assert(fake_commands[i].verb != VERB_SET_POWER &&
               fake_commands[i].verb != VERB_SET_CONNECTION_SELECT &&
               fake_commands[i].verb != VERB_SET_PIN_CONTROL &&
               fake_commands[i].verb != VERB_SET_EAPD &&
               fake_commands[i].verb != VERB_SET_AMP);
    }
    assert(primary.nodes[6].pcm_rates == primary.afg_pcm_rates);
    assert(primary.nodes[6].stream_formats == primary.afg_stream_formats);
    assert(primary.nodes[8].type == 1u); /* Intel wire widget type AUDIO_INPUT. */
    assert(primary.nodes[4].amp_input_caps == primary.afg_amp_input_caps);
    assert(primary.nodes[5].amp_output_caps ==
           fake_node(&fake_codecs[0], 5u)->amp_output_caps);
    assert(primary.nodes[5].connection_count == 2u &&
           primary.connections[primary.nodes[5].connection_offset] == 6u &&
           primary.connections[primary.nodes[5].connection_offset + 1u] == 7u);

    struct hda_route playback = {0};
    assert(hda_find_route(&primary, 2u, HDA_ROUTE_PLAYBACK, &playback) == 0);
    assert(playback.path_count == 4u && playback.path[0] == 2u &&
           playback.path[1] == 4u && playback.path[2] == 5u &&
           playback.path[3] == 6u && playback.converter_nid == 6u);
    assert(playback.association == 1u && playback.sequence == 0u);
    struct hda_route sibling = {0};
    assert(hda_find_route(&primary, 3u, HDA_ROUTE_PLAYBACK, &sibling) == 0);
    assert(sibling.association == 2u && sibling.sequence == 1u);

    struct hda_route capture = {0};
    assert(hda_find_route(&primary, 10u, HDA_ROUTE_CAPTURE, &capture) == 0);
    assert(capture.path_count == 3u && capture.path[0] == 8u &&
           capture.path[1] == 9u && capture.path[2] == 10u &&
           capture.converter_nid == 8u);
    struct hda_route cycle = {0};
    assert(hda_find_route(&primary, 12u, HDA_ROUTE_PLAYBACK, &cycle) == -ENODEV);
    struct hda_route no_adc = {0};
    assert(hda_find_route(&secondary, 49u, HDA_ROUTE_CAPTURE, &no_adc) == -ENODEV);

    assert(hda_codec_destroy(&primary) == 0);
    assert(hda_codec_destroy(&secondary) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_probe_failures(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 15u, &codec) == -EINVAL);
    assert(fake_alloc_count == 0u);

    fake_reset();
    fake_fail_alloc_at = 2u;
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -ENOMEM);
    assert(fake_live_runs == 0u && fake_free_count == 1u);

    fake_reset();
    fake_alias_fail_phys = fake_next_phys + PAGE_BYTES;
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -ERANGE);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    uint64_t node_bytes = 256u * sizeof(struct hda_codec_node);
    uint64_t node_pages = (node_bytes + PAGE_BYTES - 1u) / PAGE_BYTES;
    fake_alias_discontiguous_phys = fake_next_phys +
                                   node_pages * PAGE_BYTES + PAGE_BYTES;
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -ERANGE);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    struct fake_hda_node *fg = fake_node(&fake_codecs[0], 1u);
    fg->sub_start = 250u;
    fg->sub_count = 10u;
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -EINVAL);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    struct fake_hda_node *pin = fake_node(&fake_codecs[0], 2u);
    pin->raw_count = 0u;
    fake_add_connection(pin, 0x81u);
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -EINVAL);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    fake_fail_once = 1u;
    fake_fail_nid = 4u;
    fake_fail_verb = VERB_GET_PARAMETER;
    fake_fail_occurrence = 1u;
    fake_fail_error = -ETIMEDOUT;
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -ETIMEDOUT);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static int fake_find_write(uint8_t nid, uint16_t verb, uint8_t short_verb,
                           int mute_state, uint32_t start)
{
    for (uint32_t i = start; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid != nid || command->verb != verb ||
            command->short_verb != short_verb) continue;
        if (verb == VERB_SET_AMP && mute_state >= 0 &&
            (((command->payload & AMP_SET_MUTE) != 0u) != (mute_state != 0)))
            continue;
        return (int)i;
    }
    return -1;
}

static int fake_find_control_write(uint8_t nid, uint16_t verb,
                                   uint16_t payload, uint32_t start)
{
    for (uint32_t i = start; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid == nid && command->verb == verb &&
            !command->short_verb && command->payload == payload)
            return (int)i;
    }
    return -1;
}

static void fake_test_route_rollback_and_serialization(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_route route = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    assert(route.connection_index[0] == 1u);
    struct hda_route malformed_path = route;
    malformed_path.path[1] = 6u; /* NID 2 has no direct edge to this DAC. */
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &malformed_path, true) == -EINVAL);
    assert(fake_command_count == 0u);
    struct hda_route nested = {0};
    assert(hda_find_route(&codec, 3u, HDA_ROUTE_PLAYBACK, &nested) == 0);
    fake_command_count = 0u;
    fake_nested_codec = &codec;
    fake_nested_route = &nested;
    fake_nested_once = 1u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    assert(fake_node(&fake_codecs[0], 2u)->connection_select == 1u);
    assert(fake_nested_result == -EBUSY);
    assert(route.active && codec.active_routes == 1u);
    uint32_t active_command_count = fake_command_count;
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == -EBUSY);
    assert(route.active && fake_command_count == active_command_count);
    int mute = fake_find_write(2u, VERB_SET_AMP, 1u, 1, 0u);
    int function_group_power = fake_find_write(1u, VERB_SET_POWER, 0u, -1, 0u);
    int power = fake_find_write(4u, VERB_SET_POWER, 0u, -1, 0u);
    int select = fake_find_write(5u, VERB_SET_CONNECTION_SELECT, 0u, -1, 0u);
    int pin_gate = fake_find_control_write(2u, VERB_SET_PIN_CONTROL, 0u, 0u);
    int pin_control = fake_find_control_write(2u, VERB_SET_PIN_CONTROL,
                                              HDA_PINCTL_OUTPUT_ENABLE, 0u);
    int eapd = fake_find_write(2u, VERB_SET_EAPD, 0u, -1, 0u);
    int unmute = fake_find_write(2u, VERB_SET_AMP, 1u, 0, 0u);
    assert(pin_gate >= 0 && mute > pin_gate &&
           function_group_power > mute && power > function_group_power &&
           select > power && pin_control > select && eapd > pin_control &&
           unmute > eapd);
    assert((fake_node(&fake_codecs[0], 2u)->pin_control &
            HDA_PINCTL_OUTPUT_ENABLE) != 0u);
    assert(fake_node(&fake_codecs[0], 2u)->eapd & 2u);
    assert(fake_node(&fake_codecs[0], 5u)->connection_select == 0u);
    assert(hda_codec_destroy(&codec) == -EBUSY);
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(!route.active && codec.active_routes == 0u);
    assert(fake_node(&fake_codecs[0], 2u)->power_state == 3u);
    assert(fake_node(&fake_codecs[0], 1u)->power_state == 3u);
    assert(fake_node(&fake_codecs[0], 2u)->pin_control == 0u);
    assert(fake_node(&fake_codecs[0], 2u)->eapd == 0u);
    assert(fake_node(&fake_codecs[0], 2u)->connection_select == 0u);
    assert(fake_node(&fake_codecs[0], 5u)->connection_select == 1u);

    struct hda_route pin_select_readback_failure = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK,
                          &pin_select_readback_failure) == 0);
    fake_command_count = 0u;
    fake_fail_once = 1u;
    fake_fail_nid = 2u;
    fake_fail_verb = VERB_GET_CONNECTION_SELECT;
    fake_fail_occurrence = 2u; /* Snapshot then post-SET verification. */
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    assert(hda_apply_route(&codec, &pin_select_readback_failure, true) ==
           -EIO);
    assert(!pin_select_readback_failure.active &&
           !pin_select_readback_failure.snapshots_valid);
    assert(fake_node(&fake_codecs[0], 2u)->connection_select == 0u);
    assert(fake_find_control_write(2u, VERB_SET_CONNECTION_SELECT, 1u, 0u) >=
           0);
    assert(fake_find_control_write(2u, VERB_SET_CONNECTION_SELECT, 0u, 0u) >
           fake_find_control_write(2u, VERB_SET_CONNECTION_SELECT, 1u, 0u));

    struct hda_route capture = {0};
    assert(hda_find_route(&codec, 10u, HDA_ROUTE_CAPTURE, &capture) == 0);
    assert(capture.connection_index[0] == 1u);
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &capture, true) == 0);
    assert(fake_node(&fake_codecs[0], 8u)->connection_select == 1u);
    assert((fake_node(&fake_codecs[0], 10u)->pin_control & 0x27u) == 0x24u);
    uint8_t get_left_index_one = 0u;
    uint8_t get_right_index_one = 0u;
    uint8_t set_left_index_one = 0u;
    uint8_t set_right_index_one = 0u;
    for (uint32_t i = 0u; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid != 9u) continue;
        if (command->verb == VERB_GET_AMP && command->short_verb) {
            get_left_index_one |= command->payload == 0x2001u;
            get_right_index_one |= command->payload == 0x0001u;
        } else if (command->verb == VERB_SET_AMP && command->short_verb) {
            set_left_index_one |= (command->payload & 0xff00u) == 0x6100u;
            set_right_index_one |= (command->payload & 0xff00u) == 0x5100u;
        }
    }
    assert(get_left_index_one && get_right_index_one &&
           set_left_index_one && set_right_index_one);
    assert(hda_apply_route(&codec, &capture, false) == 0);
    assert(fake_node(&fake_codecs[0], 8u)->connection_select == 0u);

    struct hda_route adc_select_readback_failure = {0};
    assert(hda_find_route(&codec, 10u, HDA_ROUTE_CAPTURE,
                          &adc_select_readback_failure) == 0);
    fake_command_count = 0u;
    fake_fail_once = 1u;
    fake_fail_nid = 8u;
    fake_fail_verb = VERB_GET_CONNECTION_SELECT;
    fake_fail_occurrence = 2u;
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    assert(hda_apply_route(&codec, &adc_select_readback_failure, true) ==
           -EIO);
    assert(!adc_select_readback_failure.active &&
           !adc_select_readback_failure.snapshots_valid);
    assert(fake_node(&fake_codecs[0], 8u)->connection_select == 0u);
    int adc_select_one = fake_find_control_write(
        8u, VERB_SET_CONNECTION_SELECT, 1u, 0u);
    int adc_select_zero = fake_find_control_write(
        8u, VERB_SET_CONNECTION_SELECT, 0u, 0u);
    assert(adc_select_one >= 0 && adc_select_zero > adc_select_one);

    struct hda_route readback_failure = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK,
                          &readback_failure) == 0);
    fake_command_count = 0u;
    fake_fail_once = 1u;
    fake_fail_nid = 2u;
    fake_fail_verb = VERB_GET_PIN_CONTROL;
    fake_fail_occurrence = 2u; /* First GET snapshots; second verifies SET. */
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    assert(hda_apply_route(&codec, &readback_failure, true) == -EIO);
    assert(!readback_failure.active && !readback_failure.rollback_failed);
    assert(fake_node(&fake_codecs[0], 2u)->pin_control == 0u);
    assert(fake_node(&fake_codecs[0], 2u)->eapd == 0u);
    assert(fake_node(&fake_codecs[0], 1u)->power_state == 3u);

    struct hda_route set_failure = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &set_failure) == 0);
    fake_command_count = 0u;
    fake_fail_once = 1u;
    fake_fail_nid = 5u;
    fake_fail_verb = VERB_SET_CONNECTION_SELECT;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_error = -ETIMEDOUT;
    assert(hda_apply_route(&codec, &set_failure, true) == -ETIMEDOUT);
    assert(!set_failure.active && !set_failure.rollback_failed);
    assert(fake_node(&fake_codecs[0], 5u)->connection_select == 1u);
    assert(fake_node(&fake_codecs[0], 1u)->power_state == 3u);

    struct hda_route uncertain = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &uncertain) == 0);
    fake_command_count = 0u;
    fake_drop_after_set = 1u;
    fake_drop_nid = 1u;
    fake_drop_verb = VERB_SET_POWER;
    assert(hda_apply_route(&codec, &uncertain, true) == -ETIMEDOUT);
    assert(uncertain.active && uncertain.rollback_failed &&
           codec.active_routes == 1u);
    assert(fake_node(&fake_codecs[0], 1u)->power_state == 0u);
    assert(hda_codec_destroy(&codec) == -EBUSY);
    fake_dead = 0u;
    assert(hda_apply_route(&codec, &uncertain, false) == 0);
    assert(!uncertain.active && codec.active_routes == 0u);
    assert(fake_node(&fake_codecs[0], 1u)->power_state == 3u);

    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

}

static void fake_test_amp_restore_channel_failures(void)
{
    for (uint32_t failed_channel = 0u; failed_channel < 2u;
         ++failed_channel) {
        fake_reset();
        struct hda_codec codec = {0};
        assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
        struct hda_route route = {0};
        assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
        struct fake_hda_node *amp = fake_node(&fake_codecs[0], 5u);
        const uint16_t saved_left = amp->amp[0][0][0];
        const uint16_t saved_right = amp->amp[0][0][1];
        assert(saved_left == 0x10u && saved_right == 0x12u);
        assert(hda_apply_route(&codec, &route, true) == 0);

        fake_fail_once = 1u;
        fake_fail_nid = 5u;
        fake_fail_verb = VERB_SET_AMP;
        fake_fail_payload_mask = 0xffffu;
        fake_fail_payload_value = failed_channel ? 0x9012u : 0xa010u;
        fake_fail_occurrence = 1u;
        fake_fail_seen = 0u;
        fake_fail_error = -EIO;
        assert(hda_apply_route(&codec, &route, false) == -EIO);
        assert(route.active && codec.active_routes == 1u);
        if (!failed_channel) {
            assert(amp->amp[0][0][0] != saved_left);
            assert(amp->amp[0][0][1] == saved_right);
        } else {
            assert(amp->amp[0][0][0] == saved_left);
            assert(amp->amp[0][0][1] != saved_right);
        }

        assert(hda_apply_route(&codec, &route, false) == 0);
        assert(amp->amp[0][0][0] == saved_left &&
               amp->amp[0][0][1] == saved_right);
        assert(!route.active && codec.active_routes == 0u);
        assert(hda_codec_destroy(&codec) == 0);
        assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
    }
}

static void fake_test_power_error_is_rejected(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_route route = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    struct fake_hda_node *afg = fake_node(&fake_codecs[0], 1u);
    /* PS_Set is already D0, but PS_Act has not entered D0 and PS_Error marks
     * the requested transition rejected. The route must not open its pins. */
    afg->power_state = 0u;
    afg->power_act = 3u;
    afg->power_error = 1u;
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &route, true) == -EIO);
    assert(!route.active && !route.snapshots_valid && codec.active_routes == 0u);
    for (uint32_t i = 0u; i < fake_command_count; ++i) {
        uint16_t verb = fake_commands[i].verb;
        assert(verb != VERB_SET_POWER && verb != VERB_SET_PIN_CONTROL &&
               verb != VERB_SET_EAPD && verb != VERB_SET_AMP &&
               verb != VERB_SET_CONNECTION_SELECT);
    }
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_power_fresh_get_after_set(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    struct hda_route route = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    struct fake_hda_node *afg = fake_node(&fake_codecs[0], 1u);
    afg->power_state = 3u;
    afg->power_act = 0u;
    fake_power_error_nid = 1u;
    fake_power_error_after_set = 1u;
    assert(hda_apply_route(&codec, &route, true) == -EIO);
    assert(route.active && route.snapshots_valid && codec.active_routes == 1u &&
           route.afg_lease_acquired && codec.afg_lease_count[1u] == 1u);
    assert(afg->power_error == 1u);
    afg->power_error = 0u;
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(!route.active && !route.snapshots_valid && codec.active_routes == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&route, 0, sizeof(route));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    afg = fake_node(&fake_codecs[0], 1u);
    assert(hda_apply_route(&codec, &route, true) == 0);
    afg->power_state = 0u;
    afg->power_act = 3u;
    fake_power_error_nid = 1u;
    fake_power_error_after_set = 1u;
    assert(hda_apply_route(&codec, &route, false) == -EIO);
    assert(route.active && codec.active_routes == 1u &&
           route.afg_lease_acquired && codec.afg_lease_count[1u] == 1u);
    afg->power_error = 0u;
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(!route.active && codec.active_routes == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_power_delay_timeout_and_restore(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_route route = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    fake_node(&fake_codecs[0], 1u)->power_delay_reads = 4u;
    fake_node(&fake_codecs[0], 2u)->power_delay_reads = 3u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    assert(fake_sleep_count >= 3u && fake_tick_count >= 3u);
    assert(fake_node(&fake_codecs[0], 1u)->power_act == 0u &&
           fake_node(&fake_codecs[0], 2u)->power_act == 0u);
    int afg_ready = (int)fake_afg_d0_ready_at;
    int pin_power_write = fake_find_control_write(
        2u, VERB_SET_POWER, 0u, 0u);
    int pin_ready = (int)fake_pin_d0_ready_at;
    int pin_enable = fake_find_control_write(
        2u, VERB_SET_PIN_CONTROL, 0x40u, 0u);
    assert(afg_ready >= 0 && pin_power_write > afg_ready);
    assert(pin_ready >= 0 && pin_enable > pin_ready);
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(fake_node(&fake_codecs[0], 1u)->power_act == 3u &&
           fake_node(&fake_codecs[0], 2u)->power_act == 3u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&route, 0, sizeof(route));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    struct fake_hda_node *afg = fake_node(&fake_codecs[0], 1u);
    afg->power_delay_reads = 255u;
    assert(hda_apply_route(&codec, &route, true) == -ETIMEDOUT);
    assert(!route.active && !route.snapshots_valid && codec.active_routes == 0u);
    assert(fake_sleep_count == 10u && fake_tick_count == 10u);
    bool saw_pin_gate = false;
    bool saw_pin_enable = false;
    for (uint32_t i = 0u; i < fake_command_count; ++i)
        if (fake_commands[i].verb == VERB_SET_PIN_CONTROL) {
            if (fake_commands[i].payload == 0u) saw_pin_gate = true;
            if (fake_commands[i].payload & HDA_PINCTL_OUTPUT_ENABLE)
                saw_pin_enable = true;
        }
    assert(saw_pin_gate && !saw_pin_enable);
    assert(afg->power_state == 3u && afg->power_act == 3u);
    afg->power_delay_reads = 0u;
    afg->power_pending = 0u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&route, 0, sizeof(route));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    afg = fake_node(&fake_codecs[0], 1u);
    assert(hda_apply_route(&codec, &route, true) == 0);
    afg->power_error = 1u;
    assert(hda_apply_route(&codec, &route, false) == -EIO);
    assert(route.active && codec.active_routes == 1u &&
           route.afg_lease_acquired && codec.afg_lease_count[1u] == 1u);
    assert(hda_codec_destroy(&codec) == -EBUSY);
    afg->power_error = 0u;
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(!route.active && !route.afg_lease_acquired &&
           codec.afg_lease_count[1u] == 0u &&
           afg->power_state == 3u && afg->power_act == 3u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_invalid_amp_zero_db_offset(void)
{
    fake_reset();
    struct fake_hda_node *selector = fake_node(&fake_codecs[0], 5u);
    selector->amp_output_caps = AMPCAP_MUTE | (2u << 8) | 3u;
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == -ERANGE);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_amp_zero_db_and_mono(void)
{
    fake_reset();
    struct fake_hda_node *selector = fake_node(&fake_codecs[0], 5u);
    selector->amp_output_caps = (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 3u;
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_route route = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    const uint16_t saved_left = selector->amp[0][0][0];
    const uint16_t saved_right = selector->amp[0][0][1];
    assert(saved_left == 0x10u && saved_right == 0x12u);
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    assert(selector->amp[0][0][0] == 3u && selector->amp[0][0][1] == 3u);
    uint8_t wrote_left_0db = 0u;
    uint8_t wrote_right_0db = 0u;
    for (uint32_t i = 0u; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid != 5u || command->verb != VERB_SET_AMP) continue;
        assert((command->payload & AMP_SET_MUTE) == 0u);
        wrote_left_0db |= command->payload == 0xa003u;
        wrote_right_0db |= command->payload == 0x9003u;
    }
    assert(wrote_left_0db && wrote_right_0db);
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(selector->amp[0][0][0] == saved_left &&
           selector->amp[0][0][1] == saved_right);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    selector = fake_node(&fake_codecs[0], 5u);
    selector->widget_caps &= ~1u; /* Wire WidgetCaps says mono. */
    selector->amp_output_caps = AMPCAP_MUTE |
        (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 3u;
    selector->amp[0][0][1] = 0u; /* Mono has no RIGHT channel. */
    uint32_t ignored = 0u;
    assert(hda_exec_verb(&fake_controller, 2u, 5u, VERB_SET_AMP,
                         0x902au, true, &ignored) == 0);
    assert(selector->amp[0][0][0] == 0x2au &&
           selector->amp[0][0][1] == 0u);
    memset(&codec, 0, sizeof(codec));
    memset(&route, 0, sizeof(route));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    const uint16_t mono_saved_left = selector->amp[0][0][0];
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    assert(selector->amp[0][0][0] == 3u && selector->amp[0][0][1] == 0u);
    for (uint32_t i = 0u; i < fake_command_count; ++i) {
        const struct fake_command *command = &fake_commands[i];
        if (command->nid != 5u || !command->short_verb) continue;
        if (command->verb == VERB_GET_AMP)
            assert(command->payload == 0xa000u);
        if (command->verb == VERB_SET_AMP)
            assert((command->payload & 0x1000u) == 0u);
    }
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(selector->amp[0][0][0] == mono_saved_left &&
           selector->amp[0][0][1] == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_shared_afg_lease_and_conflicts(void)
{
    for (uint32_t stop_capture_first = 0u; stop_capture_first < 2u;
         ++stop_capture_first) {
        fake_reset();
        struct hda_codec codec = {0};
        assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
        struct hda_route playback = {0};
        struct hda_route capture = {0};
        assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK,
                              &playback) == 0);
        assert(hda_find_route(&codec, 10u, HDA_ROUTE_CAPTURE, &capture) == 0);
        assert(hda_apply_route(&codec, &playback, true) == 0);
        assert(hda_apply_route(&codec, &capture, true) == 0);
        struct fake_hda_node *afg = fake_node(&fake_codecs[0], 1u);
        assert(afg->power_state == 0u && afg->power_act == 0u);
        if (stop_capture_first) {
            assert(hda_apply_route(&codec, &capture, false) == 0);
            assert(playback.active && afg->power_state == 0u &&
                   afg->power_act == 0u);
            assert(hda_apply_route(&codec, &playback, false) == 0);
        } else {
            assert(hda_apply_route(&codec, &playback, false) == 0);
            assert(capture.active && afg->power_state == 0u &&
                   afg->power_act == 0u);
            assert(hda_apply_route(&codec, &capture, false) == 0);
        }
        assert(afg->power_state == 3u && afg->power_act == 3u);
        assert(codec.active_routes == 0u);
        assert(hda_codec_destroy(&codec) == 0);
        assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
    }

    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_route first = {0};
    struct hda_route conflicting = {0};
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &first) == 0);
    assert(hda_find_route(&codec, 3u, HDA_ROUTE_PLAYBACK,
                          &conflicting) == 0);
    assert(hda_apply_route(&codec, &first, true) == 0);
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &conflicting, true) == -EBUSY);
    assert(fake_command_count == 0u);
    assert(hda_apply_route(&codec, &first, false) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_route_groups(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    struct hda_route_group group = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.association == 1u && group.member_count == 2u &&
           group.total_channels == 3u);
    assert(group.members[0].pin_nid == 2u &&
           group.members[0].converter_nid == 6u &&
           group.members[0].channel_start == 0u &&
           group.members[0].channel_count == 2u);
    assert(group.members[1].pin_nid == 16u &&
           group.members[1].converter_nid == 18u &&
           group.members[1].channel_start == 2u &&
           group.members[1].channel_count == 1u);

    assert(hda_apply_route_group(&codec, &group, true) == 0);
    assert(group.active && codec.active_routes == 2u &&
           codec.afg_lease_count[1u] == 2u);
    assert(hda_codec_destroy(&codec) == -EBUSY);
    assert(hda_apply_route_group(&codec, &group, false) == 0);
    assert(!group.active && codec.active_routes == 0u &&
           codec.afg_lease_count[1u] == 0u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    codec.nodes[16u].pin_default = fake_pin_default(1u, 1u, 0u);
    assert(hda_find_route_group(&codec, 2u, &group) == -EINVAL);
    codec.nodes[16u].pin_default = fake_pin_default(1u, 0u, 2u);
    assert(hda_find_route_group(&codec, 16u, &group) == -ENODEV);
    codec.nodes[16u].pin_default = fake_pin_default(1u, 15u, 2u);
    assert(hda_find_route_group(&codec, 16u, &group) == 0);
    assert(group.member_count == 1u && group.association == 15u);
    assert(hda_route_group_destroy(&group) == 0);
    codec.nodes[16u].pin_default = fake_pin_default(1u, 1u, 2u);
    assert(hda_find_route_group(&codec, 21u, &group) == 0);
    assert(group.member_count == 8u && group.total_channels == 16u &&
           group.members[7].pin_nid == 42u);
    assert(hda_route_group_destroy(&group) == 0);
    codec.nodes[45u].pin_default = fake_pin_default(1u, 1u, 3u);
    codec.nodes[45u].widget_caps |= HDA_WCAP_DIGITAL;
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.member_count == 2u);
    assert(hda_route_group_destroy(&group) == 0);
    codec.nodes[45u].widget_caps &= ~HDA_WCAP_DIGITAL;
    codec.nodes[45u].pin_default = fake_pin_default(1u, 1u, 3u) |
                                   (1u << 30);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.member_count == 2u);
    assert(hda_route_group_destroy(&group) == 0);
    codec.nodes[45u].pin_default = fake_pin_default(1u, 1u, 3u);
    codec.nodes[45u].pin_caps = HDA_PINCAP_INPUT;
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.member_count == 2u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_find_route_group(&codec, 3u, &group) == 0);
    assert(group.members[0].converter_nid == 7u &&
           group.members[1].converter_nid == 6u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_find_route_group(&codec, 12u, &group) == -ENODEV);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    fake_node(&fake_codecs[0], 45u)->pin_default =
        fake_pin_default(1u, 4u, 1u);
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    int overflow_result = hda_find_route_group(&codec, 21u, &group);
    assert(overflow_result == -EOVERFLOW);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    fake_fail_alloc_at = fake_alloc_count + 1u;
    assert(hda_find_route_group(&codec, 2u, &group) == -ENOMEM);
    assert(!group.members && codec.route_groups == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count + 1u == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    fake_alias_fail_phys = fake_next_phys;
    assert(hda_find_route_group(&codec, 2u, &group) == -ERANGE);
    assert(!group.members && codec.route_groups == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(hda_apply_route_group(&codec, &group, true) == 0);
    fake_fail_once = 1u;
    fake_fail_nid = 16u;
    fake_fail_verb = VERB_SET_PIN_CONTROL;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    int group_stop_result = hda_apply_route_group(&codec, &group, false);
    assert(group_stop_result == -EIO);
    assert(group.active && codec.active_routes == 1u &&
           codec.afg_lease_count[1u] == 1u);
    assert(hda_route_group_destroy(&group) == -EBUSY);
    assert(hda_apply_route_group(&codec, &group, false) == 0);
    assert(!group.active && codec.active_routes == 0u &&
           codec.afg_lease_count[1u] == 0u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    fake_fail_once = 1u;
    fake_fail_nid = 16u;
    fake_fail_verb = VERB_SET_PIN_CONTROL;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    assert(hda_apply_route_group(&codec, &group, true) == -EIO);
    assert(!group.active && codec.active_routes == 0u &&
           codec.afg_lease_count[1u] == 0u);
    assert(fake_node(&fake_codecs[0], 2u)->pin_control == 0u &&
           fake_node(&fake_codecs[0], 16u)->pin_control == 0u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    struct hda_route direct = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &direct) == 0);
    assert(hda_apply_route(&codec, &direct, true) == 0);
    fake_command_count = 0u;
    assert(hda_apply_route_group(&codec, &group, true) == -EBUSY);
    assert(fake_command_count == 0u);
    assert(hda_apply_route(&codec, &direct, false) == 0);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_stream_binding(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    struct hda_route_group group = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(hda_apply_route_group(&codec, &group, true) == 0);
    codec.nodes[6u].pcm_rates = (1u << 5) | (1u << 6) | (1u << 17) | (1u << 20);
    codec.nodes[18u].pcm_rates = codec.nodes[6u].pcm_rates;
    codec.nodes[6u].stream_formats = 1u; /* PCM type; precision is in PCM rates. */
    codec.nodes[18u].stream_formats = 1u;
    const uint16_t format = 0x0012u; /* 48 kHz, S16, three channels. */
    assert(hda_route_group_stream_bind(&group, 3u, format) == 0);
    assert(fake_node(&fake_codecs[0], 6u)->stream_channel == 0x30u);
    assert(fake_node(&fake_codecs[0], 18u)->stream_channel == 0x32u);
    assert(fake_node(&fake_codecs[0], 6u)->converter_format == format);
    assert(fake_node(&fake_codecs[0], 18u)->converter_format == format);
    assert(hda_route_group_stream_bind(&group, 0u, 0u) == 0);
    assert(fake_node(&fake_codecs[0], 6u)->stream_channel == 0u);
    assert(fake_node(&fake_codecs[0], 18u)->stream_channel == 0u);
    assert(fake_node(&fake_codecs[0], 6u)->converter_format == 0u);
    assert(fake_node(&fake_codecs[0], 18u)->converter_format == 0u);

    assert(!hda_route_group_stream_bind(&group, 3u, 0x4012u));
    assert(fake_node(&fake_codecs[0], 6u)->converter_format == 0x4012u);
    assert(fake_node(&fake_codecs[0], 18u)->converter_format == 0x4012u);
    assert(!hda_route_group_stream_bind(&group, 0u, 0u));
    codec.nodes[18u].pcm_rates &= ~(1u << 17);
    assert(hda_route_group_stream_bind(&group, 3u, format) == -EOPNOTSUPP);
    assert(fake_node(&fake_codecs[0], 6u)->stream_channel == 0u);
    codec.nodes[18u].pcm_rates |= 1u << 17;

    codec.nodes[18u].stream_formats = 1u << 4;
    assert(hda_route_group_stream_bind(&group, 3u, format) == -EOPNOTSUPP);
    assert(fake_node(&fake_codecs[0], 6u)->stream_channel == 0u);
    assert(fake_node(&fake_codecs[0], 18u)->stream_channel == 0u);

    codec.nodes[18u].stream_formats = 1u;
    fake_fail_once = 1u;
    fake_fail_nid = 18u;
    fake_fail_verb = VERB_SET_CONVERTER_FORMAT;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_payload_mask = UINT16_MAX;
    fake_fail_payload_value = format;
    fake_fail_error = -EIO;
    assert(hda_route_group_stream_bind(&group, 3u, format) == -EIO);
    assert(fake_node(&fake_codecs[0], 6u)->stream_channel == 0u);
    assert(fake_node(&fake_codecs[0], 18u)->stream_channel == 0u);
    assert(fake_node(&fake_codecs[0], 6u)->converter_format == 0u);
    assert(fake_node(&fake_codecs[0], 18u)->converter_format == 0u);

    /* A cleanup verb can fail independently.  The production helper must
     * still issue both GET readbacks for every converter it touched and keep
     * the cleanup error available for a retry instead of returning software
     * unbound while hardware state is unknown. */
    fake_fail_once = 1u;
    fake_fail_nid = 18u;
    fake_fail_verb = VERB_SET_CONVERTER_FORMAT;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_payload_mask = UINT16_MAX;
    fake_fail_payload_value = format;
    fake_fail_error = -EIO;
    fake_cleanup_fail_once = 1u;
    fake_cleanup_fail_nid = 18u;
    fake_cleanup_fail_verb = VERB_GET_STREAM_CHANNEL;
    fake_cleanup_fail_payload_mask = UINT16_MAX;
    fake_cleanup_fail_payload_value = 0u;
    fake_cleanup_fail_error = -ETIMEDOUT;
    uint32_t cleanup_start = fake_command_count;
    assert(hda_route_group_stream_bind(&group, 3u, format) == -EIO);
    assert(fake_has_command(cleanup_start, 6u, VERB_GET_STREAM_CHANNEL, 0u));
    assert(fake_has_command(cleanup_start, 6u, VERB_GET_CONVERTER_FORMAT, 0u));
    assert(fake_has_command(cleanup_start, 18u, VERB_GET_STREAM_CHANNEL, 0u));
    assert(fake_has_command(cleanup_start, 18u, VERB_GET_CONVERTER_FORMAT, 0u));
    bool found_dirty = false;
    for (uint32_t i = 0; i < group.member_count; ++i)
        found_dirty |= group.members[i].stream_bind_dirty != 0u;
    assert(found_dirty);
    assert(hda_route_group_stream_bind(&group, 0u, 0u) == 0);
    for (uint32_t i = 0; i < group.member_count; ++i)
        assert(!group.members[i].stream_bind_dirty);

    assert(hda_apply_route_group(&codec, &group, false) == 0);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_pin_gate_without_mute_and_retention(void)
{
    fake_reset();
    struct fake_hda_node *pin = fake_node(&fake_codecs[0], 2u);
    struct fake_hda_node *afg_node = fake_node(&fake_codecs[0], 1u);
    struct fake_hda_node *selector = fake_node(&fake_codecs[0], 5u);
    pin->pin_control = HDA_PINCTL_OUTPUT_ENABLE;
    afg_node->amp_output_caps = (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 3u;
    pin->amp_output_caps = afg_node->amp_output_caps;
    selector->amp_output_caps = (0x7fu << AMPCAP_NUM_STEPS_SHIFT) | 3u;
    struct hda_codec codec = {0};
    struct hda_route route = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    assert(hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route) == 0);
    fake_command_count = 0u;
    assert(hda_apply_route(&codec, &route, true) == 0);
    int gate = fake_find_control_write(2u, VERB_SET_PIN_CONTROL, 0u, 0u);
    int power = fake_find_write(1u, VERB_SET_POWER, 0u, -1, 0u);
    int enable = fake_find_control_write(2u, VERB_SET_PIN_CONTROL,
                                         HDA_PINCTL_OUTPUT_ENABLE, 0u);
    assert(gate >= 0 && power > gate && enable > power);
    assert(pin->pin_control == HDA_PINCTL_OUTPUT_ENABLE);

    fake_command_count = 0u;
    fake_fail_once = 1u;
    fake_fail_nid = 2u;
    fake_fail_verb = VERB_SET_PIN_CONTROL;
    fake_fail_payload_mask = 0xffu;
    fake_fail_payload_value = 0u;
    fake_fail_occurrence = 1u;
    fake_fail_seen = 0u;
    fake_fail_error = -EIO;
    assert(hda_apply_route(&codec, &route, false) == -EIO);
    assert(route.active && codec.active_routes == 1u &&
           route.changed_flags[0] != 0u);
    fake_fail_once = 0u;
    assert(hda_apply_route(&codec, &route, false) == 0);
    assert(pin->pin_control == HDA_PINCTL_OUTPUT_ENABLE);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_group_solver_regressions(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    struct hda_route_group group = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    codec.nodes[17u].connection_count = 1u;
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.members[0].converter_nid == 7u &&
           group.members[1].converter_nid == 6u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 7u, &codec) == 0);
    /* mixer31 -> DAC48 uses connection index 16, which cannot be encoded by
     * the four-bit input amplifier GET/SET index field. */
    assert(hda_find_route_group(&codec, 30u, &group) == -ERANGE);
    assert(!group.members && codec.route_groups == 0u);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);

    fake_reset();
    memset(&codec, 0, sizeof(codec));
    memset(&group, 0, sizeof(group));
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    codec.nodes[5u].widget_caps &= ~HDA_WCAP_STEREO;
    assert(hda_find_route_group(&codec, 2u, &group) == 0);
    assert(group.members[0].channel_count == 1u);
    assert(hda_route_group_destroy(&group) == 0);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_diagnostic(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    hda_codec_dump(&codec);
    assert(strstr(fake_console_text, "vendor=0x10ec0897") != NULL);
    assert(strstr(fake_console_text, "revision=0x00100402") != NULL);
    assert(strstr(fake_console_text,
                  "implementation_id(first_afg)=0x12345678") != NULL);
    assert(strstr(fake_console_text,
                  "implementation_id=0x12345678") != NULL);
    assert(strstr(fake_console_text, "nid=0x02") != NULL);
    assert(strstr(fake_console_text, "connections=") != NULL);
    assert(strstr(fake_console_text, "nodes_phys") == NULL);
    assert(strstr(fake_console_text, "connections_phys") == NULL);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u);
}

static void fake_test_diagnostic_256_connections(void)
{
    fake_reset();
    struct hda_codec codec = {0};
    assert(hda_codec_probe(&fake_controller, 2u, &codec) == 0);
    struct hda_codec_node *node = &codec.nodes[2u];
    node->connection_offset = 0u;
    node->connection_count = HDA_CODEC_MAX_CONNECTIONS;
    for (uint32_t i = 0u; i < HDA_CODEC_MAX_CONNECTIONS; ++i)
        codec.connections[i] = (uint8_t)i;
    hda_codec_dump(&codec);
    assert(strstr(fake_console_text, "connections=0x100\n") != NULL);
    assert(hda_codec_destroy(&codec) == 0);
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_qemu_fixed_pin(void)
{
    fake_reset();
    struct fake_hda_node *pin = fake_node(&fake_codecs[0], 2u);
    pin->pin_control = HDA_PINCTL_OUTPUT_ENABLE;
    pin->pin_caps = HDA_PINCAP_OUTPUT;
    pin->pin_readonly = 1u;
    struct hda_codec codec = {0};
    struct hda_route route = {0};
    assert(!hda_codec_probe(&fake_controller, 2u, &codec));
    assert(!hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route));
    /* An unknown codec still requires a mutable pin to verify gating. */
    assert(hda_apply_route(&codec, &route, true) == -EIO);
    pin->pin_readonly = 0u;
    assert(!hda_apply_route(&codec, &route, false));
    assert(!hda_codec_destroy(&codec));

    fake_reset();
    fake_codecs[0].vendor_id = 0x1af40022u;
    pin = fake_node(&fake_codecs[0], 2u);
    pin->pin_control = HDA_PINCTL_OUTPUT_ENABLE;
    pin->pin_caps = HDA_PINCAP_OUTPUT;
    pin->pin_readonly = 1u;
    memset(&codec, 0, sizeof(codec)); memset(&route, 0, sizeof(route));
    assert(!hda_codec_probe(&fake_controller, 2u, &codec));
    assert(!hda_find_route(&codec, 2u, HDA_ROUTE_PLAYBACK, &route));
    fake_command_count = 0u;
    assert(!hda_apply_route(&codec, &route, true));
    assert(!hda_apply_route(&codec, &route, false));
    for (unsigned i=0; i<fake_command_count; ++i)
        assert(fake_commands[i].verb != VERB_SET_PIN_CONTROL);
    assert(!hda_codec_destroy(&codec));
    assert(fake_live_runs == 0u && fake_free_count == fake_alloc_count);
}

static void fake_test_auto_mute_real_shared_route_transaction(void)
{
    fake_reset();
    struct fake_hda_node *speaker_pin=fake_node(&fake_codecs[0],2u);
    struct fake_hda_node *headphone_pin=fake_node(&fake_codecs[0],3u);
    struct fake_hda_node *dac=fake_node(&fake_codecs[0],6u);
    speaker_pin->widget_caps&=~HDA_WCAP_OUT_AMP;
    headphone_pin->widget_caps&=~HDA_WCAP_OUT_AMP;
    fake_node(&fake_codecs[0],5u)->widget_caps&=~HDA_WCAP_OUT_AMP;
    headphone_pin->pin_default=fake_pin_default(2u,2u,0u);
    headphone_pin->pin_caps|=HDA_PINCAP_PRESENCE;
    struct hda_codec codec={0};struct hda_route speaker={0},headphone={0};
    assert(!hda_codec_probe(&fake_controller,2u,&codec));
    assert(!hda_find_route(&codec,2u,HDA_ROUTE_PLAYBACK,&speaker));
    assert(!hda_find_route(&codec,3u,HDA_ROUTE_PLAYBACK,&headphone));
    assert(speaker.converter_nid==headphone.converter_nid && speaker.converter_nid==6u);
    assert(!hda_apply_route(&codec,&speaker,true));
    struct hda_controls controls;
    assert(!hda_controls_init(&controls,&codec,&speaker,&headphone,2,3,NULL,0,false));
    struct audio_control_value gain={.values={23,19}},policy={.values={1}};
    assert(!hda_controls_write(&controls,HDA_CONTROL_PLAYBACK_VOLUME,&gain));
    controls.headphone_present=1;
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&policy));
    assert(!speaker.active && headphone.active && codec.active_routes==1);
    assert(!speaker_pin->pin_control && (headphone_pin->pin_control&HDA_PINCTL_OUTPUT_ENABLE));
    assert(dac->amp[0][0][0]==23 && dac->amp[0][0][1]==19);
    /* The selected paths share their DAC, mixer and selector. Rollback must
     * release the replacement's leases before reacquiring the original. */
    fake_fail_once=1;fake_fail_nid=6;fake_fail_verb=VERB_SET_AMP;
    fake_fail_payload_mask=0xffff;fake_fail_payload_value=0xa017;
    fake_fail_occurrence=1;fake_fail_seen=0;fake_fail_error=-EIO;
    policy.values[0]=0;
    assert(hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&policy)==-EIO);
    assert(controls.auto_mute && !speaker.active && headphone.active && codec.active_routes==1);
    assert(!speaker_pin->pin_control && (headphone_pin->pin_control&HDA_PINCTL_OUTPUT_ENABLE));
    assert(dac->amp[0][0][0]==23 && dac->amp[0][0][1]==19);
    assert(!hda_controls_write(&controls,HDA_CONTROL_AUTO_MUTE,&policy));
    assert(!controls.auto_mute && speaker.active && !headphone.active && codec.active_routes==1);
    assert((speaker_pin->pin_control&HDA_PINCTL_OUTPUT_ENABLE) && !headphone_pin->pin_control);
    assert(dac->amp[0][0][0]==23 && dac->amp[0][0][1]==19);
    assert(!hda_apply_route(&codec,&speaker,false));
    hda_controls_destroy(&controls);assert(!hda_codec_destroy(&codec));
    assert(!fake_live_runs && fake_free_count==fake_alloc_count);
    puts("auto_mute_actual_codec_shared_DAC_pin_gain_rollback_and_leases PASS");
}

int main(void)
{
    fake_test_auto_mute_real_shared_route_transaction();
    fake_test_qemu_fixed_pin();
    fake_test_connections();
    fake_test_invalid_amp_zero_db_offset();
    fake_test_amp_zero_db_and_mono();
    fake_test_shared_afg_lease_and_conflicts();
    fake_test_route_groups();
    fake_test_stream_binding();
    fake_test_group_solver_regressions();
    fake_test_pin_gate_without_mute_and_retention();
    fake_test_two_codecs_and_routes();
    fake_test_probe_failures();
    fake_test_route_rollback_and_serialization();
    fake_test_amp_restore_channel_failures();
    fake_test_power_error_is_rejected();
    fake_test_power_fresh_get_after_set();
    fake_test_power_delay_timeout_and_restore();
    fake_test_diagnostic();
    fake_test_diagnostic_256_connections();
    puts("hda codec fixture: ok");
    return 0;
}
