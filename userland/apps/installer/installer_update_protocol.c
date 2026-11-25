/* SPDX-License-Identifier: Apache-2.0 */
#include "installer_update.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *const metadata[] = {
    "disk", "root_device", "esp_device", "root_uuid", "esp_uuid", "boot_file",
    "installed_version", "media_version", "profile", "installed_kernel",
    "media_kernel", "index_sha256", "database_sha256", "world_sha256", "pending",
    "free_bytes", "required_bytes", "boot_modes",
};
#define ALL_METADATA ((UINT32_C(1) << (sizeof(metadata) / sizeof(metadata[0]))) - 1)

static int uuid_valid(const char *text)
{
    if (strlen(text) != 36) return 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return 0;
        } else if (!strchr("0123456789abcdefABCDEF", text[i])) return 0;
    }
    return 1;
}

static int digest_valid(const char *text)
{
    return strlen(text) == 64 && strspn(text, "0123456789abcdef") == 64;
}

static int snapshot_valid(const struct installer_update_info *info)
{
    return !strncmp(info->disk, "/dev/", 5) && info->disk[5] &&
        !strncmp(info->root_device, "/dev/", 5) && info->root_device[5] &&
        !strncmp(info->esp_device, "/dev/", 5) && info->esp_device[5] &&
        uuid_valid(info->root_uuid) && uuid_valid(info->esp_uuid) &&
        digest_valid(info->index_sha256) && digest_valid(info->database_sha256) &&
        digest_valid(info->world_sha256) && *info->installed_version && *info->media_version &&
        (!strcmp(info->profile, "xorg") || !strcmp(info->profile, "console")) && info->boot_modes <= 3;
}

int installer_update_session_line(struct installer_update_session *session,
    struct installer_update_info *info, unsigned *progress,
    char *message, size_t capacity, const char *line)
{
    if (!session || !info || !progress || !message || !capacity || !line) return -EINVAL;
    if (session->error) return -EPROTO;
    int result = -EPROTO;
    if (session->terminal || strlen(line) >= 512 || strpbrk(line, "\r\n")) goto invalid;
    if (!strncmp(line, "result\t", 7)) {
        const char *value = line + 7;
        if (*value < '0' || *value > '9') goto invalid;
        char *end;
        errno = 0;
        unsigned long number = strtoul(value, &end, 10);
        if (errno || number > 4095 || *end != '\t' || !end[1] ||
            strpbrk(end + 1, "\t\r\n") || strlen(end + 1) >= capacity) goto invalid;
        if ((!number && (info->error_number || session->fields != ALL_METADATA || !snapshot_valid(info))) ||
            (info->error_number && info->error_number != (int)number)) goto invalid;
        session->terminal = 1;
        session->terminal_error = (int)number;
        info->error_number = (int)number;
        memcpy(message, end + 1, strlen(end + 1) + 1);
        if (number && !info->failure[0]) {
            size_t length = strlen(end + 1);
            if (length >= sizeof(info->failure)) length = sizeof(info->failure) - 1;
            memcpy(info->failure, end + 1, length);
            info->failure[length] = 0;
        }
        return 5;
    }
    uint32_t field = 0;
    if (!strncmp(line, "info\t", 5)) {
        const char *tab = strchr(line + 5, '\t');
        if (!tab) goto invalid;
        for (unsigned i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i) {
            if ((size_t)(tab - line - 5) == strlen(metadata[i]) &&
                !memcmp(line + 5, metadata[i], strlen(metadata[i]))) field = UINT32_C(1) << i;
        }
        if (!field || (session->fields & field)) goto invalid;
    }
    if (!strncmp(line, "error\t", 6) && info->error_number) goto invalid;
    result = installer_update_decode(info, progress, message, capacity, line);
    if (result <= 0) goto invalid;
    if (result == 1) session->fields |= field;
    return result;
invalid:
    session->error = 1;
    return result < 0 ? result : -EPROTO;
}

int installer_update_session_finish(const struct installer_update_session *session,
                                    const struct installer_update_info *info)
{
    if (!session || !info) return -EINVAL;
    if (session->error || !session->terminal || info->error_number != session->terminal_error ||
        (!session->terminal_error && (session->fields != ALL_METADATA || !snapshot_valid(info)))) return -EPROTO;
    return -session->terminal_error;
}
