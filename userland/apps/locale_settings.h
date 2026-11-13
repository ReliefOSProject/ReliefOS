#ifndef RELIEFOS_APPS_LOCALE_SETTINGS_H
#define RELIEFOS_APPS_LOCALE_SETTINGS_H

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Language choices shared by the installer and system settings. */
static const struct {
    const char *name;
    const char *locale;
} language_options[] = {
    {"English", "en_US.UTF-8"},
    {"中文", "zh_CN.UTF-8"},
    {"日本語", "ja_JP.UTF-8"},
};

static inline unsigned language_selection(void)
{
    const char *locale = setlocale(LC_MESSAGES, NULL);
    if (locale) {
        for (unsigned i = 0; i < sizeof(language_options) / sizeof(language_options[0]); ++i) {
            if (strncmp(locale, language_options[i].locale, 2) == 0 &&
                (locale[2] == 0 || locale[2] == '_' || locale[2] == '.' || locale[2] == '@'))
                return i;
        }
    }
    return 0U;
}

static inline int write_locale_setting(const char *path, unsigned selection)
{
    char input[65536], temporary[512];
    size_t length = 0, position = 0;
    struct stat st;
    mode_t mode = 0644;
    int fd, result = -1, saved;
    FILE *output = NULL;
    if (selection >= sizeof(language_options) / sizeof(language_options[0])) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0) mode = st.st_mode & 0777;
        for (;;) {
            ssize_t got;
            if (length == sizeof(input)) {
                close(fd);
                errno = EFBIG;
                return -1;
            }
            got = read(fd, input + length, sizeof(input) - length);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) { saved = errno; close(fd); errno = saved; return -1; }
            if (!got) break;
            length += (size_t)got;
        }
        if (close(fd) < 0) return -1;
    } else if (errno != ENOENT) return -1;
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (fchmod(fd, mode) < 0) goto cleanup;
    output = fdopen(fd, "w");
    if (!output) goto cleanup;
    /* Remove every previous LANG assignment; preserve all other bytes. */
    while (position < length) {
        size_t start = position, key, end;
        while (position < length && input[position] != '\n') position++;
        if (position < length) position++;
        key = start;
        while (key < position && (input[key] == ' ' || input[key] == '\t')) key++;
        end = key + 4;
        if (end <= position && memcmp(input + key, "LANG", 4) == 0) {
            while (end < position && (input[end] == ' ' || input[end] == '\t')) end++;
            if (end < position && input[end] == '=') continue;
        }
        if (fwrite(input + start, 1, position - start, output) != position - start) goto cleanup;
    }
    if (length && input[length - 1] != '\n' && fputc('\n', output) == EOF) goto cleanup;
    if (fprintf(output, "LANG=%s\n", language_options[selection].locale) < 0) goto cleanup;
    if (fflush(output) != 0 || fsync(fd) != 0) goto cleanup;
    if (fclose(output) != 0) { output = NULL; fd = -1; goto cleanup; }
    output = NULL;
    fd = -1;
    result = rename(temporary, path);
cleanup:
    saved = errno;
    if (output) fclose(output);
    else if (fd >= 0) close(fd);
    if (result < 0) unlink(temporary);
    errno = saved;
    return result;
}

#endif
