#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    unsigned count = argc > 1 ? static_cast<unsigned>(strtoul(argv[1], nullptr, 10)) : 20000;
    if (!count || count > 100000)
        return 2;
    size_t capacity = static_cast<size_t>(count) * 400 + 4;
    char *json = static_cast<char *>(malloc(capacity));
    if (!json)
        return 2;
    size_t at = 0;
    json[at++] = '[';
    for (unsigned i = count; i > 0; --i) {
        int n = snprintf(json + at, capacity - at,
                         "%s{\"name\":\"package-%06u\",\"version\":\"1.2.3-r0\",\"description\":\"Synthetic "
                         "application package, 中文描述 and ordinary package "
                         "metadata\",\"installed-size\":1048576,\"status\":[\"installed\"]}",
                         i == count ? "" : ",", i - 1);
        if (n < 0 || static_cast<size_t>(n) >= capacity - at) {
            free(json);
            return 2;
        }
        at += static_cast<size_t>(n);
    }
    json[at++] = ']';
    json[at] = 0;
    ra::PackageParser parser;
    double started = now(), longest = 0;
    unsigned steps = 0;
    if (!parser.begin(json, at)) {
        free(json);
        return 1;
    }
    while (!parser.done()) {
        double before = now();
        if (!parser.step(96)) {
            fprintf(stderr, "%s\n", parser.error().c_str());
            free(json);
            return 1;
        }
        double took = now() - before;
        if (took > longest)
            longest = took;
        ++steps;
    }
    double total = now() - started;
    auto result = parser.take();
    char last[64];
    snprintf(last, sizeof(last), "package-%06u", count - 1);
    bool ok = result.size() == count && !strcmp(result[0].name.c_str(), "package-000000") &&
              !strcmp(result[count - 1].name.c_str(), last) && steps > 1;
    printf("RELIEFAPK_CATALOG_BENCH rows=%u bytes=%zu steps=%u total_ms=%.3f max_step_ms=%.3f verified=%d\n",
           count, at, steps, total * 1000, longest * 1000, ok ? 1 : 0);
    free(json);
    return ok ? 0 : 1;
}
