#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "format.h"
#include <string.h>
#include <libintl.h>
#define T(text) gettext(text)
#include <wchar.h>

/* Widths are terminal cells, never UTF-8 byte counts. Invalid/control bytes
 * become printable placeholders, including in administrator-edited text. */
static size_t glyph(const char *s, wchar_t *wc, int *width)
{
    mbstate_t state = {0};
    size_t n = mbrtowc(wc, s, strlen(s), &state);
    if (n == (size_t)-1 || n == (size_t)-2 || !n) {
        *wc = L'?'; *width = 1; return 1;
    }
    *width = wcwidth(*wc);
    if (*width < 0) { *wc = L'?'; *width = 1; }
    return n;
}

static size_t cells(const char *s)
{
    size_t count = 0;
    while (*s) { wchar_t wc; int w; s += glyph(s, &wc, &w); count += (size_t)w; }
    return count;
}

void motd_wrap(FILE *output, const char *text, size_t columns)
{
    size_t used = 0;
    if (!columns) columns = 1;
    while (*text) {
        wchar_t wc; int width;
        if (*text == '\n') { fputc('\n', output); used = 0; ++text; continue; }
        size_t n = glyph(text, &wc, &width);
        if (width && used && used + (size_t)width > columns) { fputc('\n', output); used = 0; }
        if ((size_t)width > columns) { fputc('?', output); used = 1; }
        else {
            if (wc == L'?' && (n != 1 || *text != '?')) fputc('?', output);
            else fwrite(text, 1, n, output);
            used += (size_t)width;
        }
        text += n;
    }
    if (used) fputc('\n', output);
}

void motd_render(FILE *output, const struct motd_info *info, int zh, size_t columns)
{
    char line[640], items[5][256];
    const char *labels[2][5] = {
        {T("Uptime"), T("Load"), T("Memory"), T("Root"), T("Address")},
        {"运行", "负载", "内存", "根分区", "地址"}
    };
    const char *values[] = {info->uptime, info->load, info->memory, info->root, info->address};
    snprintf(line, sizeof(line), zh ? "欢迎使用 %s (%s)" : T("Welcome to %s (%s)"), info->system, info->kernel);
    motd_wrap(output, line, columns);
    snprintf(line, sizeof(line), zh ? "系统信息：%s" : T("System information as of %s"), info->timestamp);
    motd_wrap(output, line, columns);
    for (size_t i = 0; i < 5; ++i) snprintf(items[i], sizeof(items[i]), "%s: %s", labels[zh != 0][i], values[i]);
    size_t count, widths[3] = {0};
    for (count = 3; count > 1; --count) {
        memset(widths, 0, sizeof(widths));
        for (size_t i = 0; i < 5; ++i) {
            size_t w = cells(items[i]);
            if (w > widths[i % count]) widths[i % count] = w;
        }
        size_t total = (count - 1) * 4;
        for (size_t i = 0; i < count; ++i) total += widths[i];
        if (total <= columns) break;
    }
    for (size_t i = 0; i < 5; i += count) {
        if (count == 1) { motd_wrap(output, items[i], columns); continue; }
        for (size_t j = 0; j < count && i + j < 5; ++j) {
            fputs(items[i + j], output);
            if (j + 1 < count && i + j + 1 < 5)
                for (size_t pad = cells(items[i + j]); pad < widths[j] + 4; ++pad) fputc(' ', output);
        }
        fputc('\n', output);
    }
}
