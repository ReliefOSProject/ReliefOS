/* Backend calls are intercepted; allocation, typed accessors and storage use
 * real libasound. Its pinned local.h aliases these opaque objects to the UAPI
 * structs. Production clients never include or rely on this fixture layout. */
#define snd_ctl_elem_type_t fixture_elem_type_t
#define snd_ctl_elem_iface_t fixture_elem_iface_t
#define snd_pcm_access_t fixture_pcm_access_t
#define snd_pcm_format_t fixture_pcm_format_t
#define snd_pcm_subformat_t fixture_pcm_subformat_t
#define snd_pcm_state_t fixture_pcm_state_t
#define snd_aes_iec958 fixture_iec958
#include <sound/asound.h>
#undef snd_ctl_elem_type_t
#undef snd_ctl_elem_iface_t
#undef snd_pcm_access_t
#undef snd_pcm_format_t
#undef snd_pcm_subformat_t
#undef snd_pcm_state_t
#undef snd_aes_iec958
#include <alsa/asoundlib.h>
#include <reliefos/audio_control.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _snd_ctl { unsigned card; int fds[2]; };
unsigned fake_reads, fake_writes, fake_polls, fake_closes;
int fake_error, fake_poll_error, fake_nonblock;
int fake_watch;
unsigned fake_channels = 2;
int64_t fake_values[2][7][128];
static const char *names[] = { "Master Playback Volume", "Master Playback Switch",
    "Output Source", "Input Source", "Headphone Jack", "Wide Integer", "Unsupported" };
static const char *outputs[] = { "Headphones", "Speakers" };
static const char *inputs[] = { "Line", "Microphone" };
int snd_card_next(int *card) { *card = *card < 0 ? 0 : *card == 0 ? 2 : -1; return 0; }
int snd_card_get_name(int card, char **name) {
    if (card != 0 && card != 2) return -ENOENT;
    *name = strdup(card == 0 ? "Fixture HDA" : "Fixture Legacy");
    return *name ? 0 : -ENOMEM;
}
int snd_ctl_open(snd_ctl_t **out, const char *name, int mode) {
    assert(snd_ctl_elem_info_sizeof() == sizeof(struct snd_ctl_elem_info));
    assert(snd_ctl_elem_value_sizeof() == sizeof(struct snd_ctl_elem_value));
    assert(snd_ctl_elem_list_sizeof() == sizeof(struct snd_ctl_elem_list));
    assert(snd_ctl_event_sizeof() == sizeof(struct snd_ctl_event));
    unsigned card; char tail;
    if (sscanf(name, "hw:%u%c", &card, &tail) != 1 || (card != 0 && card != 2)) return -ENOENT;
    if (fake_error) return -fake_error;
    snd_ctl_t *ctl = calloc(1, sizeof(*ctl));
    if (!ctl) return -ENOMEM;
    if (pipe(ctl->fds)) { free(ctl); return -errno; }
    ctl->card = card == 2; fake_nonblock = !!(mode & SND_CTL_NONBLOCK);
    *out = ctl; return 0;
}
int snd_ctl_close(snd_ctl_t *ctl) { close(ctl->fds[0]); close(ctl->fds[1]); free(ctl); ++fake_closes; return 0; }
int snd_ctl_subscribe_events(snd_ctl_t *ctl, int subscribe) {
    if (subscribe && fake_watch) { char id = 1; if (write(ctl->fds[1], &id, 1) != 1) abort(); }
    return 0;
}
int snd_ctl_elem_list(snd_ctl_t *ctl, snd_ctl_elem_list_t *object) {
    (void)ctl; struct snd_ctl_elem_list *list = (void *)object;
    if (fake_error) return -fake_error;
    list->count = 7; list->used = 0;
    for (unsigned i = list->offset; i < 7 && list->used < list->space; ++i) {
        struct snd_ctl_elem_id *id = &list->pids[list->used++];
        memset(id, 0, sizeof(*id)); id->numid = i + 1; id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
        strcpy((char *)id->name, names[i]);
    }
    return 0;
}
int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *object) {
    (void)ctl; struct snd_ctl_elem_info *info = (void *)object;
    unsigned n = info->id.numid, item = info->value.enumerated.item;
    if (fake_error) return -fake_error;
    if (!n || n > 7) return -ENOENT;
    memset(info, 0, sizeof(*info)); info->id.numid = n; strcpy((char *)info->id.name, names[n - 1]);
    info->access = SNDRV_CTL_ELEM_ACCESS_READWRITE; info->count = 1;
    if (n == 1) { info->type = SNDRV_CTL_ELEM_TYPE_INTEGER; info->count = fake_channels;
        info->value.integer.min = 0; info->value.integer.max = 40; info->value.integer.step = 2;
    } else if (n == 2 || n == 5) { info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        if (n == 5) info->access = SNDRV_CTL_ELEM_ACCESS_READ;
    } else if (n == 3 || n == 4) { info->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
        if (item >= 2) return -EINVAL;
        info->value.enumerated.items = 2; info->value.enumerated.item = item;
        strcpy(info->value.enumerated.name, n == 3 ? outputs[item] : inputs[item]);
    } else if (n == 6) { info->type = SNDRV_CTL_ELEM_TYPE_INTEGER64;
        info->value.integer64.min = INT64_MIN; info->value.integer64.max = INT64_MAX; info->value.integer64.step = 1;
    } else { info->type = SNDRV_CTL_ELEM_TYPE_BYTES; }
    return 0;
}
int snd_ctl_elem_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *object) {
    struct snd_ctl_elem_value *value = (void *)object; unsigned n = value->id.numid;
    ++fake_reads; if (fake_error) return -fake_error;
    if (!n || n > 7) return -ENOENT;
    for (unsigned i = 0; i < (n == 1 ? fake_channels : 1) && i < 128; ++i) {
        if (n == 3 || n == 4) value->value.enumerated.item[i] = fake_values[ctl->card][n - 1][i];
        else value->value.integer.value[i] = fake_values[ctl->card][n - 1][i];
    }
    return 0;
}
int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *object) {
    struct snd_ctl_elem_value *value = (void *)object; unsigned n = value->id.numid;
    ++fake_writes; if (fake_error) return -fake_error;
    if (!n || n > 7) return -ENOENT;
    for (unsigned i = 0; i < (n == 1 ? fake_channels : 1) && i < 128; ++i)
        fake_values[ctl->card][n - 1][i] = n == 3 || n == 4 ? value->value.enumerated.item[i] : value->value.integer.value[i];
    return 1;
}
int snd_ctl_poll_descriptors_count(snd_ctl_t *ctl) { (void)ctl; return fake_poll_error ? -fake_poll_error : 1; }
int snd_ctl_poll_descriptors(snd_ctl_t *ctl, struct pollfd *fds, unsigned count) {
    if (!count) return -EINVAL;
    ++fake_polls; fds[0].fd = ctl->fds[0]; fds[0].events = POLLIN; fds[0].revents = 0; return 1;
}
int snd_ctl_poll_descriptors_revents(snd_ctl_t *ctl, struct pollfd *fds, unsigned count, unsigned short *revents) {
    (void)ctl; if (!count) return -EINVAL; *revents = fds[0].revents; return 0;
}
int snd_ctl_read(snd_ctl_t *ctl, snd_ctl_event_t *object) {
    struct snd_ctl_event *event = (void *)object; char byte;
    if (read(ctl->fds[0], &byte, 1) != 1) return -errno;
    memset(event, 0, sizeof(*event)); event->type = SNDRV_CTL_EVENT_ELEM;
    event->data.elem.id.numid = (unsigned)byte; event->data.elem.mask = SNDRV_CTL_EVENT_MASK_VALUE;
    if (fake_watch) raise(SIGTERM);
    return 1;
}
void fake_external_event(struct reliefos_audio_control *control, unsigned numid);
/* Test-only introspection: the handle starts with its owned snd_ctl_t*. */
void fake_external_event(struct reliefos_audio_control *control, unsigned numid) {
    snd_ctl_t *ctl = *(snd_ctl_t **)(void *)control; char byte = (char)numid;
    fake_values[ctl->card][0][0] = 12; fake_values[ctl->card][0][1] = 14;
    if (write(ctl->fds[1], &byte, 1) != 1) abort();
}
