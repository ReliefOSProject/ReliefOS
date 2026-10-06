#include <reliefos/audio_control.h>
#include <reliefos/audio_test.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static volatile sig_atomic_t stopped;
static void stop(int signal) { (void)signal; stopped = 1; }
static int cancelled(void *context) { (void)context; return stopped; }
static int number(const char *text, int64_t *out)
{
    if (!text || !*text) return -1;
    char *end;
    errno = 0; intmax_t value = strtoimax(text, &end, 10);
    if (errno || *end) return -1;
    *out = value; return 0;
}
static int failure(const char *operation) { fprintf(stderr, "soundctl: %s: %s\n", operation, strerror(errno)); return 1; }
static int usage(void)
{
    fputs("Usage: soundctl [--card N] cards|controls|get NAME|set NAME VALUE[,VALUE]\n"
          "       soundctl [--card N] route LABEL|capture-source LABEL|watch\n"
          "       soundctl [--pcm NAME] test [--channels 1|2] [--seconds 1..30]\n", stderr);
    return 2;
}
static int list(struct reliefos_audio_control *c, struct reliefos_audio_control_desc **descs, uint32_t *count)
{
    *descs = NULL; *count = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        int r = reliefos_audio_control_list(c, NULL, 0, count);
        if (r == 0 && !*count) return 0;
        if (r != -1 || errno != ERANGE || *count > 65536) return -1;
        struct reliefos_audio_control_desc *items = calloc(*count, sizeof(*items));
        if (!items) { errno = ENOMEM; return -1; }
        r = reliefos_audio_control_list(c, items, *count, count);
        if (!r) { *descs = items; return 0; }
        int error = errno; free(items); errno = error;
        if (error != EAGAIN && error != ERANGE) return -1;
    }
    errno = EAGAIN; return -1;
}
static struct reliefos_audio_control_desc *find(struct reliefos_audio_control_desc *descs, uint32_t count, const char *name)
{
    int64_t id; int numeric = number(name, &id) == 0 && id > 0 && (uint64_t)id <= UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(name, descs[i].name) || (numeric && (uint32_t)id == descs[i].numid)) return &descs[i];
    errno = ENOENT; return NULL;
}
static int parse_value(struct reliefos_audio_control *c, struct reliefos_audio_control_desc *d, const char *text, int64_t *value)
{
    if (d->type == SND_CTL_ELEM_TYPE_BOOLEAN) {
        if (!strcasecmp(text, "on")) { *value = 1; return 0; }
        if (!strcasecmp(text, "off")) { *value = 0; return 0; }
    }
    if (d->type == SND_CTL_ELEM_TYPE_ENUMERATED) {
        char label[64];
        for (uint32_t i = 0; i < d->items; ++i) {
            if (reliefos_audio_control_item_name(c, d->numid, i, label, sizeof(label))) return -1;
            if (!strcasecmp(text, label)) { *value = i; return 0; }
        }
    }
    size_t length = strlen(text);
    if (length && text[length - 1] == '%') {
        if (d->type != SND_CTL_ELEM_TYPE_INTEGER && d->type != SND_CTL_ELEM_TYPE_INTEGER64) goto invalid;
        char percent[16]; int64_t p;
        if (length >= sizeof(percent)) goto invalid;
        memcpy(percent, text, length - 1); percent[length - 1] = 0;
        if (number(percent, &p) || p < 0 || p > 100 || d->minimum > d->maximum) goto invalid;
        __int128 span = (__int128)d->maximum - d->minimum;
        __int128 offset = (span * p + 50) / 100;
        if (d->step > 0) {
            offset = ((offset + d->step / 2) / d->step) * d->step;
            if (offset > span) offset = (span / d->step) * d->step;
        }
        *value = (int64_t)((__int128)d->minimum + offset); return 0;
    }
    if (!number(text, value)) return 0;
invalid:
    errno = EINVAL; return -1;
}
static int show(struct reliefos_audio_control *c, struct reliefos_audio_control_desc *d)
{
    int64_t values[128];
    if (d->count > 128) { errno = EOVERFLOW; return -1; }
    if (reliefos_audio_control_get(c, d->numid, values, 128)) return -1;
    printf("numid=%u name='%s' values=", d->numid, d->name);
    for (uint32_t i = 0; i < d->count; ++i) {
        if (i) putchar(',');
        if (d->type == SND_CTL_ELEM_TYPE_ENUMERATED) {
            char label[64];
            if (reliefos_audio_control_item_name(c, d->numid, (uint32_t)values[i], label, sizeof(label))) return -1;
            printf("%s", label);
        } else printf("%" PRId64, values[i]);
    }
    putchar('\n'); return 0;
}
static int set(struct reliefos_audio_control *c, struct reliefos_audio_control_desc *d, int argc, char **argv)
{
    int64_t values[128]; uint32_t count = 0;
    for (int a = 0; a < argc; ++a) {
        char *copy = strdup(argv[a]);
        if (!copy) { errno = ENOMEM; return -1; }
        char *part = copy;
        for (;;) {
            char *comma = strchr(part, ','); if (comma) *comma = 0;
            if (count == 128) { free(copy); errno = EINVAL; return -1; }
            if (parse_value(c, d, part, &values[count])) {
                int error = errno; free(copy); errno = error; return -1;
            }
            ++count;
            if (!comma) break;
            part = comma + 1;
        }
        free(copy);
    }
    return reliefos_audio_control_set(c, d->numid, values, count);
}
int main(int argc, char **argv)
{
    uint32_t card = 0, channels = 2, seconds = 2;
    const char *pcm = "default"; int a = 1;
    stopped = 0;
    while (a < argc && (!strcmp(argv[a], "--card") || !strcmp(argv[a], "--pcm"))) {
        if (a + 1 == argc) return usage();
        if (!strcmp(argv[a], "--pcm")) pcm = argv[a + 1];
        else { int64_t id; if (number(argv[a + 1], &id) || id < 0 || (uint64_t)id > UINT32_MAX) return usage(); card = (uint32_t)id; }
        a += 2;
    }
    if (a == argc) return usage();
    const char *command = argv[a++];
    if (!strcmp(command, "cards")) {
        if (a != argc) return usage();
        int id = -1, r;
        while ((r = snd_card_next(&id)) >= 0 && id >= 0) {
            char *name;
            r = snd_card_get_name(id, &name); if (r < 0) { errno = -r; return failure("card name"); }
            printf("%d: %s\n", id, name); free(name);
        }
        if (r < 0) { errno = -r; return failure("cards"); }
        return 0;
    }
    struct sigaction action = {0}; action.sa_handler = stop;
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    if (!strcmp(command, "test")) {
        while (a < argc) {
            int64_t v;
            if (a + 1 == argc || number(argv[a + 1], &v)) return usage();
            if (!strcmp(argv[a], "--channels") && v >= 1 && v <= 2) channels = (uint32_t)v;
            else if (!strcmp(argv[a], "--seconds") && v >= 1 && v <= 30) seconds = (uint32_t)v;
            else return usage();
            a += 2;
        }
        if (reliefos_audio_test_tone(pcm, channels, seconds, cancelled, NULL)) return failure("test");
        return 0;
    }
    int controls = !strcmp(command, "controls"), watch = !strcmp(command, "watch");
    int get = !strcmp(command, "get"), write = !strcmp(command, "set");
    int route = !strcmp(command, "route"), source = !strcmp(command, "capture-source");
    if ((!controls && !watch && !get && !write && !route && !source) ||
        ((controls || watch) && a != argc) || ((get || route || source) && argc - a != 1) ||
        (write && argc - a < 2)) return usage();
    struct reliefos_audio_control *c;
    if (reliefos_audio_control_open(card, &c)) return failure("open");
    struct reliefos_audio_control_desc *descs; uint32_t count;
    int status = 0;
    if (list(c, &descs, &count)) { status = failure("controls"); goto close; }
    if (controls) {
        for (uint32_t i = 0; i < count; ++i) {
            printf("numid=%u name='%s' type=%u count=%u min=%" PRId64 " max=%" PRId64 " step=%" PRId64 "\n",
                   descs[i].numid, descs[i].name, descs[i].type, descs[i].count, descs[i].minimum, descs[i].maximum, descs[i].step);
            if (descs[i].type == SND_CTL_ELEM_TYPE_ENUMERATED)
                for (uint32_t item = 0; item < descs[i].items; ++item) {
                    char label[64];
                    if (reliefos_audio_control_item_name(c, descs[i].numid, item, label, sizeof(label))) { status = failure("enum label"); goto free_list; }
                    printf("  %u: %s\n", item, label);
                }
        }
    } else if (watch) {
        while (!stopped) {
            uint32_t id, mask;
            int r = reliefos_audio_control_wait(c, 100, &id, &mask);
            if (r < 0) { if (errno == EINTR) continue; status = failure("watch"); break; }
            if (r) { printf("numid=%u event=0x%x\n", id, mask); fflush(stdout); }
        }
    } else {
        const char *name = route ? "Output Source" : source ? "Input Source" : argv[a++];
        struct reliefos_audio_control_desc *d = find(descs, count, name);
        if (!d) {
            if (route || source) {
                fputs("soundctl: available controls:\n", stderr);
                for (uint32_t i = 0; i < count; ++i) fprintf(stderr, "  %u: %s\n", descs[i].numid, descs[i].name);
                errno = ENOTSUP;
            }
            status = failure(name);
        } else if ((route || source) && d->type != SND_CTL_ELEM_TYPE_ENUMERATED) { errno = ENOTSUP; status = failure(name); }
        else if (get) { if (show(c, d)) status = failure(name); }
        else if (set(c, d, argc - a, &argv[a])) status = failure(name);
    }
free_list:
    free(descs);
close:
    reliefos_audio_control_close(c);
    return status;
}
