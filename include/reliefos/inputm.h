#ifndef RELIEFOS_INPUTM_H
#define RELIEFOS_INPUTM_H

/*
 * Userland input-method client API. The wire types and constants live in the
 * UAPI (<reliefos/inputm_abi.h>); this shadow copy re-exports them and keeps the
 * libc-side runtime declarations.
 */
#include <reliefos/inputm_abi.h>
#include <stdint.h>

int reliefos_inputm_register(const struct reliefos_inputm_provider *provider);
int reliefos_inputm_unregister(void);
int reliefos_inputm_provider_next(struct reliefos_inputm_key_event *event);
int reliefos_inputm_provider_result(const struct reliefos_inputm_result *result);
int reliefos_inputm_submit_key(uint32_t window_id, uint8_t keycode, uint8_t pressed);
int reliefos_inputm_poll_result(struct reliefos_inputm_result *result);
int reliefos_inputm_set_active(uint32_t uid, const char *id);
int reliefos_inputm_list(uint32_t uid, struct reliefos_inputm_provider *providers, uint32_t capacity, uint32_t *out_count);
int reliefos_inputm_set_context(const struct reliefos_inputm_context *context);
int reliefos_inputm_get_state(uint32_t uid, struct reliefos_inputm_state *state);
int reliefos_inputm_notify_config(uint32_t uid);

#endif
