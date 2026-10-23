#ifndef RELIEFOS_TEXT_INPUT_H
#define RELIEFOS_TEXT_INPUT_H

/* Versioned ReliefOS text-input service SDK.
 *
 * Consumers talk to the imd daemon over /run/reliefos/input-method.sock through
 * this library; the pre-migration /dev/input-method device and fd 3 channel
 * are gone. Wire structures are the versioned ReliefOS input-method protocol.
 */
#include <reliefos/inputm.h>
#include <stdint.h>

#define TEXT_INPUT_SERVICE_ABI_VERSION 1U

typedef struct reliefos_inputm_provider text_input_provider_t;
typedef struct reliefos_inputm_key_event text_input_key_event_t;
typedef struct reliefos_inputm_result text_input_result_t;
typedef struct reliefos_inputm_active_request text_input_active_request_t;
typedef struct reliefos_inputm_config_request text_input_config_request_t;
typedef struct reliefos_inputm_provider_list text_input_provider_list_t;
typedef struct reliefos_inputm_context text_input_context_t;
typedef struct reliefos_inputm_state text_input_state_t;

#define TEXT_INPUT_MAX_PROVIDERS RELIEFOS_INPUTM_MAX_PROVIDERS
#define TEXT_INPUT_MAX_CANDIDATES RELIEFOS_INPUTM_MAX_CANDIDATES
#define TEXT_INPUT_ID_LEN RELIEFOS_INPUTM_ID_LEN
#define TEXT_INPUT_NAME_LEN RELIEFOS_INPUTM_NAME_LEN
#define TEXT_INPUT_ABBREV_LEN RELIEFOS_INPUTM_ABBREV_LEN
#define TEXT_INPUT_TEXT_LEN RELIEFOS_INPUTM_TEXT_LEN

#define TEXT_INPUT_START_MANUAL RELIEFOS_INPUTM_START_MANUAL
#define TEXT_INPUT_START_LOGIN RELIEFOS_INPUTM_START_LOGIN
#define TEXT_INPUT_START_ON_DEMAND RELIEFOS_INPUTM_START_ON_DEMAND

#define TEXT_INPUT_CONTEXT_FOCUSED RELIEFOS_INPUTM_CONTEXT_FOCUSED
#define TEXT_INPUT_CONTEXT_SECURE RELIEFOS_INPUTM_CONTEXT_SECURE

#define TEXT_INPUT_RESULT_COMPOSITION RELIEFOS_INPUTM_RESULT_COMPOSITION
#define TEXT_INPUT_RESULT_COMMIT RELIEFOS_INPUTM_RESULT_COMMIT
#define TEXT_INPUT_RESULT_CANCEL RELIEFOS_INPUTM_RESULT_CANCEL
#define TEXT_INPUT_RESULT_PASSTHROUGH RELIEFOS_INPUTM_RESULT_PASSTHROUGH

#define TEXT_INPUT_RENDER_CONTROLS RELIEFOS_INPUTM_RENDER_CONTROLS
#define TEXT_INPUT_RENDER_PIXELS RELIEFOS_INPUTM_RENDER_PIXELS

int text_input_register(const text_input_provider_t *provider);
int text_input_unregister(void);
int text_input_provider_next(text_input_key_event_t *event);
int text_input_provider_result(const text_input_result_t *result);
int text_input_submit_key(uint32_t window_id, uint8_t keycode, uint8_t pressed);
int text_input_poll_result(text_input_result_t *result);
int text_input_set_active(uint32_t uid, const char *id);
int text_input_list(uint32_t uid, text_input_provider_t *providers,
                    uint32_t capacity, uint32_t *out_count);
int text_input_set_context(const text_input_context_t *context);
int text_input_get_state(uint32_t uid, text_input_state_t *state);
int text_input_notify_config(uint32_t uid);

#endif
