#define _GNU_SOURCE
#include <assert.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "userland/apps/motd/format.h"

static void check_width(const char *text, size_t limit)
{
    mbstate_t state = {0};
    size_t columns = 0;
    while (*text) {
        wchar_t wc;
        size_t bytes = mbrtowc(&wc, text, strlen(text), &state);
        assert(bytes != (size_t)-1 && bytes != (size_t)-2 && bytes);
        if (wc == '\n') { assert(columns <= limit); columns = 0; }
        else { int width = wcwidth(wc); assert(width >= 0); columns += (size_t)width; }
        text += bytes;
    }
    assert(columns == 0);
}

int main(void)
{
    assert(setlocale(LC_ALL, "C.UTF-8"));
    struct motd_info info = {
        .system = "Example OS 19", .kernel = "TestKernel 7.8.9 test64",
        .timestamp = "2026-09-22 21:50:12 CST", .uptime = "2d 4h",
        .load = "0.08", .memory = "31%", .root = "42%", .address = "192.168.1.10"
    };
    for (int zh = 0; zh < 2; ++zh) {
        for (size_t width = 1; width <= 161; width += 1) {
            char *text = NULL; size_t size = 0;
            FILE *output = open_memstream(&text, &size); assert(output);
            motd_render(output, &info, zh, width);
            motd_wrap(output, "Project: https://github.com/ReliefOSProject/ReliefOS", width);
            assert(fclose(output) == 0);
            check_width(text, width);
            if (width >= 80) {
                assert(strstr(text, "Example OS 19 (TestKernel 7.8.9 test64)"));
                assert(strstr(text, "31%") && strstr(text, "192.168.1.10"));
                assert(strstr(text, zh ? "内存" : "Memory"));
            }
            free(text);
        }
    }
    puts("motd: multilingual wrapping, live-value rendering passed");
    return 0;
}
