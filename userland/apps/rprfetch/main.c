#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>

#define T(text) gettext(text)

static void init_localization(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("rprfetch", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("rprfetch", "UTF-8");
    textdomain("rprfetch");
}

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <reliefos/http.h>

static void usage(const char *program)
{
    fprintf(stderr, T("Usage: %s HTTPS_URL OUTPUT [MAX_BYTES]\n"), program);
}

struct fetch_bound { uint32_t maximum; struct timespec start; };

static int bounded_progress(uint32_t received, uint32_t total, void *opaque)
{
    struct fetch_bound *bound = opaque;
    struct timespec now;
    if (received > bound->maximum || total > bound->maximum || clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return now.tv_sec - bound->start.tv_sec > 90 ? -1 : 0;
}

int main(int argc, char **argv)
{
    init_localization();
    struct reliefos_http_response response;

    if (argc != 3 && argc != 4) {
        usage(argv[0]);
        return 2;
    }
    if (strncmp(argv[1], "https://", 8) != 0) {
        fprintf(stderr, T("rprfetch: refusing non-HTTPS URL\n"));
        return 2;
    }
    if (!strcmp(argv[2], "-") || !argv[2][0]) {
        fprintf(stderr, T("rprfetch: OUTPUT must be a file path\n"));
        return 2;
    }
    struct fetch_bound bound = {.maximum = UINT32_MAX};
    if (argc == 4) {
        char *end; errno = 0;
        unsigned long maximum = strtoul(argv[3], &end, 10);
        if (errno || !*argv[3] || *end || argv[3][0] == '-' || !maximum || maximum > UINT32_MAX ||
            clock_gettime(CLOCK_MONOTONIC, &bound.start)) { usage(argv[0]); return 2; }
        bound.maximum = (uint32_t)maximum;
    }
    memset(&response, 0, sizeof(response));
    if (reliefos_http_download(argv[1], argv[2], 30000, argc == 4 ? bounded_progress : NULL, &bound,
                             &response) < 0) {
        fprintf(stderr,
                T("rprfetch: download failed (network status %u, HTTP %u)\n"),
                response.net_status, response.http_status);
        (void)unlink(argv[2]);
        return 1;
    }
    if (response.http_status < 200 || response.http_status >= 300) {
        fprintf(stderr, T("rprfetch: server returned HTTP %u\n"),
                response.http_status);
        (void)unlink(argv[2]);
        return 1;
    }
    return 0;
}
