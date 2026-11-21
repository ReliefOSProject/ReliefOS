#ifndef RELIEFOS_INSTALLER_LABEL_WRAP_H
#define RELIEFOS_INSTALLER_LABEL_WRAP_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Invalid UTF-8 advances one byte; valid codepoints are never split. Testing
 * the terminating NUL before each continuation also bounds truncated input. */
static inline size_t installer_label_character_bytes(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;
    unsigned char lead = bytes[0];
    size_t count = lead >= 0xc2 && lead <= 0xdf ? 2 :
        lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
    for (size_t index = 1; index < count; ++index)
        if ((bytes[index] & 0xc0) != 0x80) return 1;
    if ((lead == 0xe0 && bytes[1] < 0xa0) ||
        (lead == 0xed && bytes[1] >= 0xa0) ||
        (lead == 0xf0 && bytes[1] < 0x90) ||
        (lead == 0xf4 && bytes[1] >= 0x90)) return 1;
    return count;
}

static inline int installer_label_space(char character)
{
    return character == ' ' || character == '\t';
}

/* Return an owned display-only string; the caller frees it. Preserve explicit
 * newlines and prefer ordinary word boundaries. A word wider than the label
 * is split at whole UTF-8 characters, including unspaced CJK and long paths.
 * The callback measures complete candidate substrings in the current font. */
static inline char *installer_label_wrap(const char *text, double max_width,
    double (*measure)(const char *, size_t, void *), void *context)
{
    if (!measure || max_width <= 0) return NULL;
    if (!text) text = "";
    size_t length = strlen(text);
    if (length > INT_MAX || length > (SIZE_MAX - 1) / 2) return NULL;
    char *wrapped = (char *)malloc(length * 2 + 1);
    if (!wrapped) return NULL;
    char *output = wrapped;
    const char *start = text;
    while (*start) {
        const char *scan = start, *word_break = NULL;
        size_t character_bytes = 1;
        while (*scan && *scan != '\n') {
            character_bytes = installer_label_character_bytes(scan);
            if (measure(start, (size_t)(scan + character_bytes - start), context) > max_width)
                break;
            if (installer_label_space(*scan) && scan > start &&
                !installer_label_space(scan[-1])) word_break = scan;
            scan += character_bytes;
        }
        const char *end = scan, *next = scan;
        int explicit_newline = *scan == '\n';
        if (*scan && !explicit_newline) {
            if (scan > start && installer_label_space(*scan)) {
                while (installer_label_space(*next)) ++next;
                if (*next == '\n') {
                    ++next;
                    explicit_newline = 1;
                }
            } else if (word_break) {
                end = word_break;
                next = word_break;
                while (installer_label_space(*next)) ++next;
            } else if (scan == start) {
                /* An individual glyph may exceed a very narrow box. Preserve
                 * it and still advance, instead of looping or deleting text. */
                end = next = scan + character_bytes;
            }
        } else if (explicit_newline) {
            ++next;
        }
        size_t bytes = (size_t)(end - start);
        memcpy(output, start, bytes);
        output += bytes;
        if (*next || explicit_newline) *output++ = '\n';
        start = next;
    }
    *output = 0;
    return wrapped;
}

#endif
