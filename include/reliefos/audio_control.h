#ifndef RELIEFOS_AUDIO_CONTROL_H
#define RELIEFOS_AUDIO_CONTROL_H

#include <stdint.h>

struct reliefos_audio_control;

struct reliefos_audio_control_desc {
    uint32_t numid;
    uint32_t type;
    uint32_t count;
    uint32_t items;
    int64_t minimum;
    int64_t maximum;
    int64_t step;
    char name[44];
};

/* Operations return 0, or -1 with POSIX errno. list(NULL, 0, &count)
 * reports the required capacity with ERANGE (0 for an empty card).
 * set accepts one value broadcast to all channels, or exactly count values.
 * wait returns 1 for an event, 0 for timeout/no element event, -1 for error;
 * timeout_ms is -1 (infinite) or nonnegative. Calls on a handle serialize.
 * close removes the handle immediately; in-flight operations retain ownership
 * until they finish. Use finite waits when a worker needs prompt shutdown.
 * NULL/repeated close is harmless; discard closed pointers before opening any
 * new handle (an allocator may reuse their address). */

int reliefos_audio_control_open(uint32_t card,
                                struct reliefos_audio_control **out);
int reliefos_audio_control_list(struct reliefos_audio_control *c,
                                struct reliefos_audio_control_desc *out,
                                uint32_t capacity, uint32_t *count);
int reliefos_audio_control_get(struct reliefos_audio_control *c,
                               uint32_t numid, int64_t *values,
                               uint32_t capacity);
int reliefos_audio_control_set(struct reliefos_audio_control *c,
                               uint32_t numid, const int64_t *values,
                               uint32_t count);
int reliefos_audio_control_wait(struct reliefos_audio_control *c,
                               int timeout_ms, uint32_t *numid,
                               uint32_t *event_mask);
void reliefos_audio_control_close(struct reliefos_audio_control *c);
int reliefos_audio_control_item_name(struct reliefos_audio_control *c,
    uint32_t numid, uint32_t item, char *name, uint32_t capacity);

#endif
