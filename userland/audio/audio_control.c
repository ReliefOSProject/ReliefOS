#include <reliefos/audio_control.h>

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct reliefos_audio_control {
    snd_ctl_t *ctl;
    uint32_t card;
    unsigned refs;
    pthread_mutex_t operation;
    struct reliefos_audio_control *next;
};

/* Live-object lookup never dereferences the caller's pointer. Pins keep an
 * in-flight operation alive after close; separate cards can wait concurrently. */
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct reliefos_audio_control *handles;

static struct reliefos_audio_control *acquire(struct reliefos_audio_control *c)
{
    pthread_mutex_lock(&handles_lock);
    struct reliefos_audio_control *p = handles;
    while (p && p != c) p = p->next;
    if (p) ++p->refs;
    pthread_mutex_unlock(&handles_lock);
    if (!p) errno = EINVAL;
    else pthread_mutex_lock(&p->operation);
    return p;
}

static void release(struct reliefos_audio_control *c)
{
    pthread_mutex_lock(&handles_lock);
    unsigned remaining = --c->refs;
    pthread_mutex_unlock(&handles_lock);
    if (remaining) return;
    snd_ctl_subscribe_events(c->ctl, 0);
    snd_ctl_close(c->ctl);
    pthread_mutex_destroy(&c->operation);
    free(c);
}

static int finish(struct reliefos_audio_control *c, int result)
{
    int saved = errno;
    pthread_mutex_unlock(&c->operation);
    release(c);
    errno = saved;
    return result;
}

static void bounds(snd_ctl_elem_info_t *info, int64_t *minimum,
                   int64_t *maximum, int64_t *step)
{
    *minimum = *maximum = *step = 0;
    switch (snd_ctl_elem_info_get_type(info)) {
    case SND_CTL_ELEM_TYPE_BOOLEAN: *maximum = 1; *step = 1; break;
    case SND_CTL_ELEM_TYPE_INTEGER:
        *minimum = snd_ctl_elem_info_get_min(info);
        *maximum = snd_ctl_elem_info_get_max(info);
        *step = snd_ctl_elem_info_get_step(info); break;
    case SND_CTL_ELEM_TYPE_INTEGER64:
        *minimum = snd_ctl_elem_info_get_min64(info);
        *maximum = snd_ctl_elem_info_get_max64(info);
        *step = snd_ctl_elem_info_get_step64(info); break;
    default: break;
    }
}

static int valid_shape(snd_ctl_elem_info_t *info)
{
    unsigned limit;
    switch (snd_ctl_elem_info_get_type(info)) {
    case SND_CTL_ELEM_TYPE_BOOLEAN:
    case SND_CTL_ELEM_TYPE_INTEGER:
    case SND_CTL_ELEM_TYPE_ENUMERATED: limit = 128; break;
    case SND_CTL_ELEM_TYPE_INTEGER64: limit = 64; break;
    default: errno = ENOTSUP; return 0;
    }
    unsigned count = snd_ctl_elem_info_get_count(info);
    if (!count || count > limit) { errno = EOVERFLOW; return 0; }
    if (snd_ctl_elem_info_is_inactive(info)) { errno = ENXIO; return 0; }
    return 1;
}

static int alsa_error(int result)
{
    if (result >= 0) {
        return result;
    }
    errno = -result;
    return -1;
}

static int valid_handle(struct reliefos_audio_control *c)
{
    if (!c || !c->ctl) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    if (!dst) return;
    while (src && src[i] && i + 1 < 44) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int get_info(struct reliefos_audio_control *c, uint32_t numid,
                    snd_ctl_elem_info_t **info_out)
{
    snd_ctl_elem_info_t *info = NULL;
    int result;
    result = snd_ctl_elem_info_malloc(&info);
    if (result < 0) return alsa_error(result);
    snd_ctl_elem_info_clear(info);
    snd_ctl_elem_info_set_numid(info, numid);
    result = snd_ctl_elem_info(c->ctl, info);
    if (result < 0) {
        snd_ctl_elem_info_free(info);
        alsa_error(result);
        return -1;
    }
    *info_out = info;
    return 0;
}

int reliefos_audio_control_open(uint32_t card,
                                struct reliefos_audio_control **out)
{
    struct reliefos_audio_control *control;
    char name[32];
    int result;
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    control = calloc(1, sizeof(*control));
    if (!control) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(name, sizeof(name), "hw:%u", card);
    result = pthread_mutex_init(&control->operation, NULL);
    if (result) { free(control); errno = result; return -1; }
    result = snd_ctl_open(&control->ctl, name, SND_CTL_NONBLOCK);
    if (result < 0) {
        pthread_mutex_destroy(&control->operation);
        free(control);
        alsa_error(result);
        return -1;
    }
    control->card = card;
    result = snd_ctl_subscribe_events(control->ctl, 1);
    if (result < 0) {
        snd_ctl_close(control->ctl);
        pthread_mutex_destroy(&control->operation);
        free(control);
        alsa_error(result);
        return -1;
    }
    control->refs = 1;
    pthread_mutex_lock(&handles_lock);
    control->next = handles;
    handles = control;
    pthread_mutex_unlock(&handles_lock);
    *out = control;
    return 0;
}

static int control_list(struct reliefos_audio_control *c,
                                struct reliefos_audio_control_desc *out,
                                uint32_t capacity, uint32_t *count)
{
    snd_ctl_elem_list_t *list = NULL;
    unsigned int total;
    unsigned int used;
    unsigned int i;
    int result;
    if (!valid_handle(c) || !count || (!out && capacity)) { errno = EINVAL; return -1; }
    result = snd_ctl_elem_list_malloc(&list);
    if (result < 0) return alsa_error(result);
    snd_ctl_elem_list_clear(list);
    snd_ctl_elem_list_set_offset(list, 0);
    result = snd_ctl_elem_list(c->ctl, list);
    if (result < 0) {
        snd_ctl_elem_list_free(list);
        return alsa_error(result);
    }
    total = snd_ctl_elem_list_get_count(list);
    *count = total;
    if (!out && capacity != 0) {
        snd_ctl_elem_list_free(list);
        errno = EINVAL;
        return -1;
    }
    if (total > capacity) {
        snd_ctl_elem_list_free(list);
        errno = ERANGE;
        return -1;
    }
    if (total == 0) {
        snd_ctl_elem_list_free(list);
        return 0;
    }
    result = snd_ctl_elem_list_alloc_space(list, total);
    if (result < 0) {
        snd_ctl_elem_list_free(list);
        return alsa_error(result);
    }
    snd_ctl_elem_list_set_offset(list, 0);
    result = snd_ctl_elem_list(c->ctl, list);
    if (result < 0) {
        snd_ctl_elem_list_free_space(list);
        snd_ctl_elem_list_free(list);
        return alsa_error(result);
    }
    used = snd_ctl_elem_list_get_used(list);
    if (used != total || snd_ctl_elem_list_get_count(list) != total) {
        *count = snd_ctl_elem_list_get_count(list);
        snd_ctl_elem_list_free_space(list);
        snd_ctl_elem_list_free(list);
        errno = EAGAIN;
        return -1;
    }
    for (i = 0; i < used; ++i) {
        snd_ctl_elem_info_t *info;
        uint32_t numid = snd_ctl_elem_list_get_numid(list, i);
        memset(&out[i], 0, sizeof(out[i]));
        out[i].numid = numid;
        copy_name(out[i].name, snd_ctl_elem_list_get_name(list, i));
        if (get_info(c, numid, &info) < 0) {
            snd_ctl_elem_list_free_space(list);
            snd_ctl_elem_list_free(list);
            return -1;
        }
        out[i].type = (uint32_t)snd_ctl_elem_info_get_type(info);
        out[i].count = snd_ctl_elem_info_get_count(info);
        if (out[i].type == SND_CTL_ELEM_TYPE_ENUMERATED)
            out[i].items = snd_ctl_elem_info_get_items(info);
        bounds(info, &out[i].minimum, &out[i].maximum, &out[i].step);
        snd_ctl_elem_info_free(info);
    }
    *count = used;
    snd_ctl_elem_list_free_space(list);
    snd_ctl_elem_list_free(list);
    return 0;
}

static int fill_value(snd_ctl_elem_value_t *value,
                      snd_ctl_elem_type_t type, const int64_t *values,
                      uint32_t count, uint32_t index)
{
    int64_t input = values[index < count ? index : count - 1];
    switch (type) {
    case SND_CTL_ELEM_TYPE_BOOLEAN:
        snd_ctl_elem_value_set_boolean(value, index, input);
        return 0;
    case SND_CTL_ELEM_TYPE_INTEGER:
        snd_ctl_elem_value_set_integer(value, index, (long)input);
        return 0;
    case SND_CTL_ELEM_TYPE_INTEGER64:
        snd_ctl_elem_value_set_integer64(value, index, (long long)input);
        return 0;
    case SND_CTL_ELEM_TYPE_ENUMERATED:
        snd_ctl_elem_value_set_enumerated(value, index, (unsigned int)input);
        return 0;
    default:
        errno = ENOTSUP;
        return -1;
    }
}

static int read_value(snd_ctl_elem_value_t *value,
                      snd_ctl_elem_type_t type, int64_t *values,
                      uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        switch (type) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            values[i] = snd_ctl_elem_value_get_boolean(value, i);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            values[i] = snd_ctl_elem_value_get_integer(value, i);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER64:
            values[i] = snd_ctl_elem_value_get_integer64(value, i);
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            values[i] = snd_ctl_elem_value_get_enumerated(value, i);
            break;
        default:
            errno = ENOTSUP;
            return -1;
        }
    }
    return 0;
}

static int control_get(struct reliefos_audio_control *c,
                               uint32_t numid, int64_t *values,
                               uint32_t capacity)
{
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value = NULL;
    unsigned int count;
    int result;
    if (!valid_handle(c) || !values) { errno = EINVAL; return -1; }
    if (get_info(c, numid, &info) < 0) return -1;
    if (!valid_shape(info)) { snd_ctl_elem_info_free(info); return -1; }
    if (!snd_ctl_elem_info_is_readable(info)) {
        snd_ctl_elem_info_free(info); errno = EACCES; return -1;
    }
    count = snd_ctl_elem_info_get_count(info);
    if (count == 0 || count > capacity) {
        snd_ctl_elem_info_free(info);
        errno = ERANGE;
        return -1;
    }
    result = snd_ctl_elem_value_malloc(&value);
    if (result < 0) {
        snd_ctl_elem_info_free(info);
        return alsa_error(result);
    }
    snd_ctl_elem_value_clear(value);
    snd_ctl_elem_value_set_numid(value, numid);
    result = snd_ctl_elem_read(c->ctl, value);
    if (result < 0) {
        snd_ctl_elem_value_free(value);
        snd_ctl_elem_info_free(info);
        return alsa_error(result);
    }
    if (read_value(value, snd_ctl_elem_info_get_type(info), values, count) < 0) {
        snd_ctl_elem_value_free(value);
        snd_ctl_elem_info_free(info);
        return -1;
    }
    snd_ctl_elem_value_free(value);
    snd_ctl_elem_info_free(info);
    return 0;
}

static int control_set(struct reliefos_audio_control *c,
                               uint32_t numid, const int64_t *values,
                               uint32_t count)
{
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value = NULL;
    snd_ctl_elem_type_t type;
    unsigned int members;
    int64_t minimum, maximum, step;
    unsigned int i;
    int result;
    if (!valid_handle(c) || !values || count == 0) {
        errno = EINVAL;
        return -1;
    }
    if (get_info(c, numid, &info) < 0) return -1;
    if (!valid_shape(info)) { snd_ctl_elem_info_free(info); return -1; }
    if (!snd_ctl_elem_info_is_writable(info)) {
        snd_ctl_elem_info_free(info);
        errno = EROFS;
        return -1;
    }
    members = snd_ctl_elem_info_get_count(info);
    if (count != 1 && count != members) {
        snd_ctl_elem_info_free(info);
        errno = ERANGE;
        return -1;
    }
    type = snd_ctl_elem_info_get_type(info);
    bounds(info, &minimum, &maximum, &step);
    for (i = 0; i < members; ++i) {
        int64_t current = values[i < count ? i : count - 1];
        if (type == SND_CTL_ELEM_TYPE_BOOLEAN && (current < 0 || current > 1)) {
            errno = EINVAL;
            snd_ctl_elem_info_free(info);
            return -1;
        }
        if (type == SND_CTL_ELEM_TYPE_INTEGER ||
            type == SND_CTL_ELEM_TYPE_INTEGER64) {
            if (current < minimum || current > maximum ||
                (step > 0 && (((uint64_t)current - (uint64_t)minimum) % (uint64_t)step) != 0)) {
                errno = EINVAL;
                snd_ctl_elem_info_free(info);
                return -1;
            }
        }
        if (type == SND_CTL_ELEM_TYPE_ENUMERATED &&
            (current < 0 || (uint64_t)current >= snd_ctl_elem_info_get_items(info))) {
            errno = EINVAL;
            snd_ctl_elem_info_free(info);
            return -1;
        }
    }
    result = snd_ctl_elem_value_malloc(&value);
    if (result < 0) {
        snd_ctl_elem_info_free(info);
        return alsa_error(result);
    }
    snd_ctl_elem_value_clear(value);
    snd_ctl_elem_value_set_numid(value, numid);
    for (i = 0; i < members; ++i) {
        if (fill_value(value, type, values, count, i) < 0) {
            snd_ctl_elem_value_free(value);
            snd_ctl_elem_info_free(info);
            return -1;
        }
    }
    result = snd_ctl_elem_write(c->ctl, value);
    snd_ctl_elem_value_free(value);
    snd_ctl_elem_info_free(info);
    if (result < 0) return alsa_error(result);
    return 0;
}

static int control_wait(struct reliefos_audio_control *c,
                               int timeout_ms, uint32_t *numid,
                               uint32_t *event_mask)
{
    struct pollfd *fds;
    snd_ctl_event_t *event = NULL;
    unsigned int count;
    unsigned short revents = 0;
    int result;
    if (!valid_handle(c) || !numid || !event_mask || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    *numid = 0;
    *event_mask = 0;
    result = snd_ctl_poll_descriptors_count(c->ctl);
    if (result < 0) return alsa_error(result);
    count = (unsigned)result;
    if (!count) {
        errno = ENODEV;
        return -1;
    }
    fds = calloc(count, sizeof(*fds));
    if (!fds) {
        errno = ENOMEM;
        return -1;
    }
    result = snd_ctl_poll_descriptors(c->ctl, fds, count);
    if (result < 0) {
        free(fds);
        return alsa_error(result);
    }
    if (result == 0 || (unsigned)result > count) { free(fds); errno = EIO; return -1; }
    count = (unsigned)result;
    result = poll(fds, count, timeout_ms);
    if (result <= 0) {
        free(fds);
        if (result < 0) return -1;
        return 0;
    }
    result = snd_ctl_poll_descriptors_revents(c->ctl, fds, count, &revents);
    free(fds);
    if (result < 0) return alsa_error(result);
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENODEV; return -1; }
    if (!(revents & POLLIN)) return 0;
    result = snd_ctl_event_malloc(&event);
    if (result < 0) return alsa_error(result);
    result = snd_ctl_read(c->ctl, event);
    if (result == -EAGAIN) { snd_ctl_event_free(event); return 0; }
    if (result < 0) {
        snd_ctl_event_free(event);
        return alsa_error(result);
    }
    if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) {
        snd_ctl_event_free(event);
        return 0;
    }
    *numid = snd_ctl_event_elem_get_numid(event);
    *event_mask = snd_ctl_event_elem_get_mask(event);
    snd_ctl_event_free(event);
    return 1;
}

void reliefos_audio_control_close(struct reliefos_audio_control *c)
{
    int saved = errno;
    pthread_mutex_lock(&handles_lock);
    struct reliefos_audio_control **p = &handles;
    while (*p && *p != c) p = &(*p)->next;
    struct reliefos_audio_control *owned = *p;
    if (owned) *p = owned->next;
    pthread_mutex_unlock(&handles_lock);
    if (owned) release(owned);
    errno = saved;
}

int reliefos_audio_control_list(struct reliefos_audio_control *c,
    struct reliefos_audio_control_desc *out, uint32_t capacity, uint32_t *count)
{
    if (!(c = acquire(c))) return -1;
    return finish(c, control_list(c, out, capacity, count));
}

int reliefos_audio_control_get(struct reliefos_audio_control *c,
    uint32_t numid, int64_t *values, uint32_t capacity)
{
    if (!(c = acquire(c))) return -1;
    return finish(c, control_get(c, numid, values, capacity));
}

int reliefos_audio_control_set(struct reliefos_audio_control *c,
    uint32_t numid, const int64_t *values, uint32_t count)
{
    if (!(c = acquire(c))) return -1;
    return finish(c, control_set(c, numid, values, count));
}

int reliefos_audio_control_wait(struct reliefos_audio_control *c,
    int timeout_ms, uint32_t *numid, uint32_t *event_mask)
{
    if (!(c = acquire(c))) return -1;
    return finish(c, control_wait(c, timeout_ms, numid, event_mask));
}

int reliefos_audio_control_item_name(struct reliefos_audio_control *c,
    uint32_t numid, uint32_t item, char *name, uint32_t capacity)
{
    if (!name || !capacity) { errno = EINVAL; return -1; }
    if (!(c = acquire(c))) return -1;
    snd_ctl_elem_info_t *info;
    if (get_info(c, numid, &info) < 0) return finish(c, -1);
    int result = -1;
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_ENUMERATED) errno = ENOTSUP;
    else if (item >= snd_ctl_elem_info_get_items(info)) errno = EINVAL;
    else {
        snd_ctl_elem_info_set_item(info, item);
        int r = snd_ctl_elem_info(c->ctl, info);
        if (r < 0) alsa_error(r);
        else {
            const char *label = snd_ctl_elem_info_get_item_name(info);
            if (strlen(label) >= capacity) errno = ERANGE;
            else { strcpy(name, label); result = 0; }
        }
    }
    snd_ctl_elem_info_free(info);
    return finish(c, result);
}
