/* Standalone input-method service client. Window-system integration belongs
 * to each frontend; this client only implements the input-method protocol. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <reliefos/inputm.h>
#include <reliefos/inputmd.h>
#include <reliefos/text_input.h>
#include <reliefos/unix_ipc.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INPUTM_RESULT_QUEUE 32U
#define INPUTM_KEY_QUEUE 32U
#define INPUTM_FRAME_CAP 4096U
/* Short per-call retry plus backoff: a daemon that is still starting is
 * absorbed, while a daemon that never exists (installer images) must not
 * make callers stall for seconds. */
#define INPUTM_CONNECT_ATTEMPT_MS 50u
#define INPUTM_CONNECT_BACKOFF_MS 1000u

static int imd_fd = -1;
static uint32_t imd_retry_after_ms;
static struct reliefos_inputm_result inputm_results[INPUTM_RESULT_QUEUE];
static uint32_t inputm_result_head;
static uint32_t inputm_result_tail;
static struct reliefos_inputm_key_event inputm_keys[INPUTM_KEY_QUEUE];
static uint32_t inputm_key_head;
static uint32_t inputm_key_tail;
static uint32_t imd_role = RELIEFOS_IMD_ROLE_APP;

static uint32_t inputm_now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(1, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static int inputm_copy_id(char *dst, uint32_t capacity, const char *id)
{
    uint32_t i = 0;
    if (!dst || capacity == 0 || !id || !id[0]) {
        return 0;
    }
    while (id[i] && i + 1U < capacity) {
        dst[i] = id[i];
        ++i;
    }
    if (id[i]) {
        return 0;
    }
    dst[i] = 0;
    return 1;
}

static int inputm_wait_frame(uint32_t expected, void *payload, uint32_t capacity,
                             uint32_t *length)
{
    uint32_t deadline = inputm_now_ms() + 3000u;
    for (;;) {
        uint8_t buffer[INPUTM_FRAME_CAP];
        uint32_t type = 0;
        uint32_t got = 0;
        if (reliefos_ipc_recv(imd_fd, &type, buffer, sizeof(buffer), &got) == 0) {
            if (type == expected) {
                if (got > capacity) got = capacity;
                if (got) memcpy(payload, buffer, got);
                if (length) *length = got;
                return 0;
            }
            if (type == RELIEFOS_IMD_MSG_ACK) {
                struct reliefos_imd_ack ack;
                if (got < sizeof(ack)) { errno = EPROTO; return -1; }
                memcpy(&ack, buffer, sizeof(ack));
                errno = ack.code < 0 && ack.code >= -4095 ? -ack.code : EPROTO;
                return -1;
            }
            if (type == RELIEFOS_IMD_MSG_RESULT && got >= sizeof(struct reliefos_inputm_result)) {
                struct reliefos_inputm_result result;
                memcpy(&result, buffer, sizeof(result));
                inputm_results[inputm_result_head] = result;
                inputm_result_head = (inputm_result_head + 1u) % INPUTM_RESULT_QUEUE;
            } else if (type == RELIEFOS_IMD_MSG_KEY_EVENT && got >= sizeof(struct reliefos_inputm_key_event)) {
                struct reliefos_inputm_key_event event;
                memcpy(&event, buffer, sizeof(event));
                inputm_keys[inputm_key_head] = event;
                inputm_key_head = (inputm_key_head + 1u) % INPUTM_KEY_QUEUE;
            }
        }
        if (inputm_now_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        (void)poll(0, 0, 2);
    }
}

static int inputm_pump(void)
{
    int progress = 0;
    if (imd_fd < 0) return 0;
    for (;;) {
        struct pollfd descriptor = {.fd = imd_fd, .events = POLLIN, .revents = 0};
        uint8_t buffer[INPUTM_FRAME_CAP];
        uint32_t type = 0;
        uint32_t got = 0;
        if (poll(&descriptor, 1, 0) <= 0) break;
        if (reliefos_ipc_recv(imd_fd, &type, buffer, sizeof(buffer), &got) < 0) break;
        progress = 1;
        if (type == RELIEFOS_IMD_MSG_RESULT && got >= sizeof(struct reliefos_inputm_result)) {
            struct reliefos_inputm_result result;
            memcpy(&result, buffer, sizeof(result));
            inputm_results[inputm_result_head] = result;
            inputm_result_head = (inputm_result_head + 1u) % INPUTM_RESULT_QUEUE;
        } else if (type == RELIEFOS_IMD_MSG_KEY_EVENT && got >= sizeof(struct reliefos_inputm_key_event)) {
            struct reliefos_inputm_key_event event;
            memcpy(&event, buffer, sizeof(event));
            inputm_keys[inputm_key_head] = event;
            inputm_key_head = (inputm_key_head + 1u) % INPUTM_KEY_QUEUE;
        }
    }
    return progress;
}

static int inputm_open_connection(void)
{
    uint32_t deadline;
    struct reliefos_imd_hello hello;
    struct reliefos_imd_ack ack;
    if (imd_fd >= 0) return imd_fd;
    if (inputm_now_ms() < imd_retry_after_ms) return -1;
    deadline = inputm_now_ms() + INPUTM_CONNECT_ATTEMPT_MS;
    while (imd_fd < 0 && inputm_now_ms() < deadline) {
        imd_fd = reliefos_ipc_connect(RELIEFOS_IPC_SOCK_INPUT_METHOD);
        if (imd_fd < 0) (void)poll(0, 0, 10);
    }
    if (imd_fd < 0) {
        imd_retry_after_ms = inputm_now_ms() + INPUTM_CONNECT_BACKOFF_MS;
        return -1;
    }
    (void)reliefos_ipc_set_nonblock(imd_fd, 1);
    hello.pid = (uint32_t)getpid();
    hello.role = imd_role;
    if (reliefos_ipc_send(imd_fd, RELIEFOS_IMD_MSG_HELLO, &hello, sizeof(hello)) < 0 ||
        inputm_wait_frame(RELIEFOS_IMD_MSG_ACK, &ack, sizeof(ack), 0) < 0 ||
        ack.code < 0) {
        reliefos_ipc_close(imd_fd);
        imd_fd = -1;
        imd_retry_after_ms = inputm_now_ms() + INPUTM_CONNECT_BACKOFF_MS;
        return -1;
    }
    return imd_fd;
}

static int inputm_request_ack(uint32_t type, const void *payload, uint32_t length,
                              int32_t *code)
{
    struct reliefos_imd_ack ack;
    if (inputm_open_connection() < 0) return -1;
    if (reliefos_ipc_send(imd_fd, type, payload, length) < 0) return -1;
    if (inputm_wait_frame(RELIEFOS_IMD_MSG_ACK, &ack, sizeof(ack), 0) < 0) return -1;
    if (code) *code = ack.code;
    return 0;
}

int text_input_register(const struct reliefos_inputm_provider *provider)
{
    int32_t code = -1;
    if (!provider) return -1;
    imd_role = RELIEFOS_IMD_ROLE_PROVIDER;
    if (imd_fd >= 0) reliefos_ipc_close(imd_fd), imd_fd = -1;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_REGISTER, provider, sizeof(*provider), &code) < 0) return -1;
    return code > 0 ? 1 : code;
}

int text_input_unregister(void)
{
    int32_t code = -1;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_UNREGISTER, 0, 0, &code) < 0) return -1;
    return code;
}

int text_input_provider_next(struct reliefos_inputm_key_event *event)
{
    uint32_t deadline;
    if (!event) return -1;
    if (inputm_open_connection() < 0) return -1;
    deadline = inputm_now_ms() + 50u;
    for (;;) {
        (void)inputm_pump();
        if (inputm_key_head != inputm_key_tail) {
            *event = inputm_keys[inputm_key_tail];
            inputm_key_tail = (inputm_key_tail + 1u) % INPUTM_KEY_QUEUE;
            return 1;
        }
        if (inputm_now_ms() >= deadline) return 0;
        (void)poll(0, 0, 2);
    }
}

int text_input_provider_result(const struct reliefos_inputm_result *result)
{
    int32_t code = -1;
    if (!result) return -1;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_RESULT, result, sizeof(*result), &code) < 0) return -1;
    return code;
}

int text_input_submit_key(uint32_t window_id, uint8_t keycode, uint8_t pressed)
{
    struct reliefos_inputm_key_event event = {0};
    int32_t code = -1;
    if (!window_id) return -1;
    event.window_id = window_id;
    event.keycode = keycode;
    event.pressed = pressed ? 1u : 0u;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_SUBMIT_KEY, &event, sizeof(event), &code) < 0) return -1;
    return code;
}

int text_input_poll_result(struct reliefos_inputm_result *result)
{
    if (!result) return -1;
    if (inputm_open_connection() < 0) return 0;
    (void)inputm_pump();
    if (inputm_result_head == inputm_result_tail) return 0;
    *result = inputm_results[inputm_result_tail];
    inputm_result_tail = (inputm_result_tail + 1u) % INPUTM_RESULT_QUEUE;
    return 1;
}

int text_input_set_active(uint32_t uid, const char *id)
{
    struct reliefos_inputm_active_request request = {0};
    int32_t code = -1;
    if (!uid || !inputm_copy_id(request.id, sizeof(request.id), id)) return -1;
    request.uid = uid;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_SET_ACTIVE, &request, sizeof(request), &code) < 0) return -1;
    return code;
}

int text_input_list(uint32_t uid, struct reliefos_inputm_provider *providers,
                    uint32_t capacity, uint32_t *out_count)
{
    struct reliefos_imd_list request = {.uid = uid, .capacity = capacity};
    struct reliefos_imd_list_ack ack;
    uint32_t length = 0;
    uint8_t buffer[INPUTM_FRAME_CAP];
    if (out_count) *out_count = 0;
    if (!uid) return -1;
    if (inputm_open_connection() < 0) return -1;
    if (reliefos_ipc_send(imd_fd, RELIEFOS_IMD_MSG_LIST, &request, sizeof(request)) < 0) return -1;
    if (inputm_wait_frame(RELIEFOS_IMD_MSG_LIST_ACK, buffer, sizeof(buffer), &length) < 0) return -1;
    if (length < sizeof(ack)) return -1;
    memcpy(&ack, buffer, sizeof(ack));
    if (out_count) *out_count = ack.count;
    if (providers && capacity) {
        uint32_t count = ack.count < capacity ? ack.count : capacity;
        uint32_t payload = length - sizeof(ack);
        if (payload < count * sizeof(*providers)) return -1;
        memcpy(providers, buffer + sizeof(ack), count * sizeof(*providers));
    }
    return (int)ack.count;
}

int text_input_set_context(const struct reliefos_inputm_context *context)
{
    int32_t code = -1;
    if (!context) return -1;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_SET_CONTEXT, context, sizeof(*context), &code) < 0) return -1;
    return code;
}

int text_input_get_state(uint32_t uid, struct reliefos_inputm_state *state)
{
    struct reliefos_imd_get_state request = {.uid = uid};
    if (!uid || !state) return -1;
    if (inputm_open_connection() < 0) return -1;
    if (reliefos_ipc_send(imd_fd, RELIEFOS_IMD_MSG_GET_STATE, &request, sizeof(request)) < 0) return -1;
    if (inputm_wait_frame(RELIEFOS_IMD_MSG_STATE_ACK, state, sizeof(*state), 0) < 0) return -1;
    return 1;
}

int text_input_notify_config(uint32_t uid)
{
    struct reliefos_inputm_config_request request = {.uid = uid};
    int32_t code = -1;
    if (!uid) return -1;
    if (inputm_request_ack(RELIEFOS_IMD_MSG_NOTIFY_CONFIG, &request, sizeof(request), &code) < 0) return -1;
    return code;
}

int reliefos_inputm_register(const struct reliefos_inputm_provider *provider) { return text_input_register(provider); }
int reliefos_inputm_unregister(void) { return text_input_unregister(); }
int reliefos_inputm_provider_next(struct reliefos_inputm_key_event *event) { return text_input_provider_next(event); }
int reliefos_inputm_provider_result(const struct reliefos_inputm_result *result) { return text_input_provider_result(result); }
int reliefos_inputm_submit_key(uint32_t window_id, uint8_t keycode, uint8_t pressed) { return text_input_submit_key(window_id, keycode, pressed); }
int reliefos_inputm_poll_result(struct reliefos_inputm_result *result) { return text_input_poll_result(result); }
int reliefos_inputm_set_active(uint32_t uid, const char *id) { return text_input_set_active(uid, id); }
int reliefos_inputm_list(uint32_t uid, struct reliefos_inputm_provider *providers, uint32_t capacity, uint32_t *out_count) { return text_input_list(uid, providers, capacity, out_count); }
int reliefos_inputm_set_context(const struct reliefos_inputm_context *context) { return text_input_set_context(context); }
int reliefos_inputm_get_state(uint32_t uid, struct reliefos_inputm_state *state) { return text_input_get_state(uid, state); }
int reliefos_inputm_notify_config(uint32_t uid) { return text_input_notify_config(uid); }
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_inputm_get_state) leonos_inputm_get_state __attribute__((alias("reliefos_inputm_get_state")));
extern __typeof__(reliefos_inputm_list) leonos_inputm_list __attribute__((alias("reliefos_inputm_list")));
extern __typeof__(reliefos_inputm_notify_config) leonos_inputm_notify_config __attribute__((alias("reliefos_inputm_notify_config")));
extern __typeof__(reliefos_inputm_poll_result) leonos_inputm_poll_result __attribute__((alias("reliefos_inputm_poll_result")));
extern __typeof__(reliefos_inputm_provider_next) leonos_inputm_provider_next __attribute__((alias("reliefos_inputm_provider_next")));
extern __typeof__(reliefos_inputm_provider_result) leonos_inputm_provider_result __attribute__((alias("reliefos_inputm_provider_result")));
extern __typeof__(reliefos_inputm_register) leonos_inputm_register __attribute__((alias("reliefos_inputm_register")));
extern __typeof__(reliefos_inputm_set_active) leonos_inputm_set_active __attribute__((alias("reliefos_inputm_set_active")));
extern __typeof__(reliefos_inputm_set_context) leonos_inputm_set_context __attribute__((alias("reliefos_inputm_set_context")));
extern __typeof__(reliefos_inputm_submit_key) leonos_inputm_submit_key __attribute__((alias("reliefos_inputm_submit_key")));
extern __typeof__(reliefos_inputm_unregister) leonos_inputm_unregister __attribute__((alias("reliefos_inputm_unregister")));
