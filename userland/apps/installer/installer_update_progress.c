#include "installer_update.h"
#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_(s) (s)
static const struct {
    const char *key, *message;
    unsigned checking, applying;
} steps[] = {
    {"", "", 0, 0},
    {"repository", N_("Checking signed repository and package archives"), 22, 38},
    {"fetch", N_("Preparing verified package archives"), 23, 39},
    {"verify", N_("Verifying package signatures"), 24, 40},
    {"plan", N_("Calculating the offline package transaction"), 26, 42},
    {"retain", N_("Preserving the installed kernel and configuration"), 28, 44},
    {"repair", N_("Repairing signed system packages"), 28, 46},
    {"upgrade", N_("Applying signed system packages"), 28, 50},
    {"shell", N_("Updating shell configuration"), 28, 60},
    {"cache", N_("Publishing the offline package cache"), 28, 62},
    {"prune", N_("Removing retired package archives"), 28, 75},
    {"sync", N_("Synchronizing the offline package cache"), 28, 76},
    {"restore", N_("Restoring retained kernel files"), 28, 77},
};
static const char *const actions[] = {"", "step", "checking", "copying", "reused", "complete", "failed", "package"};

/** @brief Admit only APK names or archive basenames, never paths/control text. */
static int package_valid(const char *name)
{
    return *name && strlen(name) < 256 && strcmp(name, ".") && strcmp(name, "..") &&
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_.-") == strlen(name);
}

/** @brief Parse a bounded unsigned scalar without accepting signs or trailing data. */
static int number(const char *text, uint64_t *value)
{
    if (!*text || strspn(text, "0123456789") != strlen(text)) return -EINVAL;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || *end) return -EINVAL;
    *value = parsed;
    return 0;
}

int installer_update_activity_decode(struct installer_update_activity *activity, const char *line)
{
    if (!activity || !line) return -EINVAL;
    if (strncmp(line, "activity\t", 9)) return 0;
    if (strlen(line) >= 512 || strpbrk(line, "\r\n")) return -EINVAL;
    char copy[512], *fields[9];
    strcpy(copy, line + 9);
    fields[0] = copy;
    for (unsigned i = 1; i < 9; ++i) {
        char *tab = strchr(fields[i - 1], '\t');
        if (!tab) return -EINVAL;
        *tab = 0; fields[i] = tab + 1;
    }
    if (strchr(fields[8], '\t') || !package_valid(fields[2])) return -EINVAL;
    struct installer_update_activity parsed = {0};
    for (unsigned i = 1; i < sizeof(steps) / sizeof(steps[0]); ++i)
        if (!strcmp(fields[0], steps[i].key)) parsed.step = i;
    for (unsigned i = 1; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (!strcmp(fields[1], actions[i])) parsed.action = i;
    uint64_t values[6];
    for (unsigned i = 0; i < 6; ++i) if (number(fields[i + 3], &values[i])) return -EINVAL;
    if (!parsed.step || !parsed.action || values[0] > values[1] || values[1] > UINT32_MAX ||
        values[2] > values[3] || values[4] > values[2] || values[5] > 255 ||
        (parsed.action == UPDATE_ACTION_FAILED) != (values[5] != 0)) return -EINVAL;
    strcpy(parsed.package, fields[2]);
    parsed.files_done = (uint32_t)values[0]; parsed.files_total = (uint32_t)values[1];
    parsed.bytes_done = values[2]; parsed.bytes_total = values[3]; parsed.bytes_reused = values[4];
    parsed.status = (unsigned)values[5];
    *activity = parsed;
    return 1;
}

int installer_update_activity_encode(const struct installer_update_activity *activity, char *line, size_t capacity)
{
    if (!activity || !line || activity->step == 0 || activity->step >= sizeof(steps) / sizeof(steps[0]) ||
        activity->action == 0 || activity->action >= sizeof(actions) / sizeof(actions[0])) return -EINVAL;
    int length = snprintf(line, capacity, "activity\t%s\t%s\t%s\t%u\t%u\t%llu\t%llu\t%llu\t%u",
        steps[activity->step].key, actions[activity->action], activity->package,
        activity->files_done, activity->files_total, (unsigned long long)activity->bytes_done,
        (unsigned long long)activity->bytes_total, (unsigned long long)activity->bytes_reused, activity->status);
    return length < 0 || (size_t)length >= capacity ? -EOVERFLOW : 0;
}

int installer_update_activity_apk(struct installer_update_activity *activity, const char *line)
{
    if (!activity || !line) return -EINVAL;
    if ((activity->step != UPDATE_STEP_UPGRADE && activity->step != UPDATE_STEP_REPAIR &&
         activity->step != UPDATE_STEP_PLAN) || *line != '(') return 0;
    const char *slash = strchr(line, '/'), *close = strchr(line, ')');
    if (!slash || !close || slash > close || slash - line > 11 || close - slash > 11 || close[1] != ' ') return 0;
    char current[12], total[12], operation[24], package[256];
    memcpy(current, line + 1, (size_t)(slash - line - 1)); current[slash - line - 1] = 0;
    memcpy(total, slash + 1, (size_t)(close - slash - 1)); total[close - slash - 1] = 0;
    uint64_t position, count; int consumed = 0;
    if (number(current, &position) || number(total, &count) || !position || position > count || count > UINT32_MAX ||
        sscanf(close + 2, "%23s %255s%n", operation, package, &consumed) != 2 || !package_valid(package) ||
        (close[2 + consumed] && close[2 + consumed] != ' ' && close[2 + consumed] != '\t')) return 0;
    if (strcmp(operation, "Installing") && strcmp(operation, "Upgrading") && strcmp(operation, "Downgrading") &&
        strcmp(operation, "Reinstalling") && strcmp(operation, "Purging")) return 0;
    activity->action = UPDATE_ACTION_PACKAGE;
    strcpy(activity->package, package);
    activity->files_done = (uint32_t)position - 1; activity->files_total = (uint32_t)count;
    activity->bytes_done = activity->bytes_total = activity->bytes_reused = 0;
    activity->status = 0;
    return 1;
}

const char *installer_update_activity_stage(unsigned step)
{
    return step < sizeof(steps) / sizeof(steps[0]) ? steps[step].message : "";
}

unsigned installer_update_activity_percent(const struct installer_update_activity *activity, int applying)
{
    if (activity->step >= sizeof(steps) / sizeof(steps[0])) return 0;
    unsigned value = applying ? steps[activity->step].applying : steps[activity->step].checking;
    if (applying && activity->step == UPDATE_STEP_CACHE && activity->bytes_total)
        value += (unsigned)(12.0 * ((double)activity->bytes_done / (double)activity->bytes_total));
    if (applying && activity->step == UPDATE_STEP_UPGRADE && activity->files_total)
        value += (unsigned)(8.0 * ((double)activity->files_done / activity->files_total));
    return value;
}

int installer_update_activity_format(const struct installer_update_activity *activity, char *text, size_t capacity)
{
    if (!activity || !text || !capacity) return -EINVAL;
    text[0] = 0;
    if (!activity->step) return 0;
    const char *label = N_("Package");
    switch (activity->action) {
    case UPDATE_ACTION_CHECKING: label = N_("Comparing contents"); break;
    case UPDATE_ACTION_COPYING: label = N_("Copying"); break;
    case UPDATE_ACTION_REUSED: label = N_("Reused"); break;
    case UPDATE_ACTION_COMPLETE: label = N_("Completed"); break;
    case UPDATE_ACTION_FAILED: label = N_("Failed"); break;
    default: break;
    }
    int length = 0;
    if (strcmp(activity->package, "-")) length = snprintf(text, capacity, "%s: %s", gettext(label), activity->package);
    if (length < 0 || (size_t)length >= capacity) return -EOVERFLOW;
    if (activity->bytes_total) {
        int added = snprintf(text + length, capacity - (size_t)length,
            gettext("\n%u/%u files; %.1f/%.1f MiB processed; %.1f MiB reused"),
            activity->files_done, activity->files_total, activity->bytes_done / 1048576.0,
            activity->bytes_total / 1048576.0, activity->bytes_reused / 1048576.0);
        if (added < 0 || (size_t)added >= capacity - (size_t)length) return -EOVERFLOW;
        length += added;
    } else if (activity->files_total) {
        int added = snprintf(text + length, capacity - (size_t)length, gettext("\n%u/%u packages processed"),
                             activity->files_done, activity->files_total);
        if (added < 0 || (size_t)added >= capacity - (size_t)length) return -EOVERFLOW;
        length += added;
    }
    if (activity->status) {
        int added = snprintf(text + length, capacity - (size_t)length, gettext("\nCommand exit status: %u"), activity->status);
        if (added < 0 || (size_t)added >= capacity - (size_t)length) return -EOVERFLOW;
    }
    return 0;
}
