/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct module_record {
    char *name, *phase, *autoload;
    const char *path;
    char **dependencies;
    size_t dependency_count;
    unsigned state;
};
struct module_graph {
    struct module_record *modules;
    size_t count;
    char **builtins;
    size_t builtin_count;
    size_t *order;
    size_t order_count;
};

static bool module_name(const char *name)
{
    if (!name || !*name) return false;
    for (; *name; ++name)
        if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') ||
              (*name >= '0' && *name <= '9') || *name == '_')) return false;
    return true;
}

static char *filename_name(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t length = strlen(base);
    if (length <= 3 || strcmp(base + length - 3, ".ko")) return NULL;
    char *name = strndup(base, length - 3);
    if (name) for (char *at = name; *at; ++at) if (*at == '-') *at = '_';
    return name;
}

static bool is_builtin(const struct module_graph *graph, const char *name)
{
    for (size_t i = 0; i < graph->builtin_count; ++i)
        if (!strcmp(graph->builtins[i], name)) return true;
    return false;
}

static int builtins(struct module_graph *graph, const struct kernel_bundle *bundle)
{
    char member[100];
    snprintf(member, sizeof(member), "%smodules.builtin", bundle->prefix);
    int fd = kernel_bundle_open_member(bundle->root_fd, member);
    struct byte_buffer bytes = {0};
    if (fd < 0) return -1;
    int result = kernel_tools_read_all(fd, &bytes); close(fd);
    if (result || memchr(bytes.data, 0, bytes.len)) { buffer_destroy(&bytes); errno = EINVAL; return -1; }
    char *line = (char *)bytes.data;
    while (line && *line) {
        char *next = strchr(line, '\n'); if (next) *next++ = 0;
        char *name = kernel_tools_canonical_member(line) ? filename_name(line) : NULL;
        if (!name || !module_name(name) || is_builtin(graph, name)) { free(name); result = -1; break; }
        char **grown = realloc(graph->builtins, (graph->builtin_count + 1) * sizeof(*grown));
        if (!grown) { free(name); result = -1; break; }
        graph->builtins = grown; grown[graph->builtin_count++] = name; line = next;
    }
    buffer_destroy(&bytes);
    return result;
}

static char *field(const char *busybox, const char *install, const char *path, const char *key)
{
    size_t length = strlen(install) + strlen(path) + 2;
    char *full = malloc(length);
    if (!full) return NULL;
    snprintf(full, length, "%s/%s", install, path);
    char *arguments[] = {(char *)busybox, "modinfo", "-F", (char *)key, full, NULL};
    struct byte_buffer bytes = {0};
    int result = kernel_stage_run(arguments, &bytes);
    free(full);
    if (result || memchr(bytes.data, 0, bytes.len)) { buffer_destroy(&bytes); return NULL; }
    char *first = (char *)bytes.data;
    while (*first && isspace((unsigned char)*first)) ++first;
    size_t size = strlen(first);
    while (size && isspace((unsigned char)first[size - 1])) first[--size] = 0;
    char *value = strdup(first);
    buffer_destroy(&bytes);
    return value;
}

static int compare_modules(const void *a, const void *b)
{
    const struct module_record *first = a, *second = b;
    return strcmp(first->name, second->name);
}

static int read_graph(struct module_graph *graph, const struct kernel_bundle *bundle,
                       const char *install, const char *busybox)
{
    if (builtins(graph, bundle)) return -1;
    graph->modules = calloc(bundle->module_count ? bundle->module_count : 1, sizeof(*graph->modules));
    graph->order = calloc(bundle->module_count ? bundle->module_count : 1, sizeof(*graph->order));
    if (!graph->modules || !graph->order) return -1;
    const json_value *names = json_member(&bundle->document, "modules");
    for (size_t i = 0; i < names->child_count; ++i) {
        struct module_record *record = &graph->modules[graph->count++];
        record->path = json_text(&names->children[i]);
        record->name = field(busybox, install, record->path, "name");
        char *expected = filename_name(record->path);
        if (!module_name(record->name) || !expected || strcmp(expected, record->name) || is_builtin(graph, record->name)) {
            free(expected); fputs("kernel-tools: ambiguous module name\n", stderr); return -1;
        }
        free(expected);
        char magic[160]; snprintf(magic, sizeof(magic), "%s ReliefNT-1 %s", bundle->release, bundle->abi);
        char *actual = field(busybox, install, record->path, "vermagic");
        if (!actual || strcmp(actual, magic)) { free(actual); fputs("kernel-tools: module ABI mismatch\n", stderr); return -1; }
        free(actual);
        record->phase = field(busybox, install, record->path, "reliefnt_phase");
        record->autoload = field(busybox, install, record->path, "reliefnt_autoload");
        char *dependencies = field(busybox, install, record->path, "depends");
        if (!dependencies || !record->phase || !record->autoload ||
            (*record->phase && strcmp(record->phase, "block") && strcmp(record->phase, "filesystem")) ||
            (*record->autoload && strcmp(record->autoload, "platform"))) {
            free(dependencies); fputs("kernel-tools: unsupported module loading policy\n", stderr); return -1;
        }
        char *at = dependencies;
        while (*at) {
            char *next = strchr(at, ','); if (next) *next++ = 0;
            if (*at) {
                for (char *p = at; *p; ++p) if (*p == '-') *p = '_';
                if (!module_name(at)) { free(dependencies); return -1; }
                char **grown = realloc(record->dependencies, (record->dependency_count + 1) * sizeof(*grown));
                if (!grown) { free(dependencies); return -1; }
                record->dependencies = grown;
                grown[record->dependency_count] = strdup(at);
                if (!grown[record->dependency_count]) { free(dependencies); return -1; }
                ++record->dependency_count;
            }
            if (!next) break;
            at = next;
        }
        free(dependencies);
    }
    if (graph->count) qsort(graph->modules, graph->count, sizeof(*graph->modules), compare_modules);
    for (size_t i = 1; i < graph->count; ++i) if (!strcmp(graph->modules[i - 1].name, graph->modules[i].name)) {
        fputs("kernel-tools: ambiguous module name\n", stderr); return -1;
    }
    return 0;
}

/* Iterative DFS avoids consuming the host call stack on long dependency chains.
 * Each frame and result slot is bounded by the actual installed module count. */
static int visit(struct module_graph *graph, size_t first)
{
    struct frame { size_t module, next; };
    struct frame *stack = calloc(graph->count ? graph->count : 1, sizeof(*stack));
    if (!stack) return -1;
    size_t depth = 1; stack[0].module = first;
    int result = 0;
    while (depth) {
        struct frame *frame = &stack[depth - 1];
        struct module_record *record = &graph->modules[frame->module];
        if (record->state == 2) { --depth; continue; }
        record->state = 1;
        if (frame->next == record->dependency_count) {
            record->state = 2; graph->order[graph->order_count++] = frame->module; --depth; continue;
        }
        const char *name = record->dependencies[frame->next++];
        if (is_builtin(graph, name)) continue;
        size_t i;
        for (i = 0; i < graph->count; ++i) if (!strcmp(graph->modules[i].name, name)) break;
        if (i == graph->count) { fprintf(stderr, "kernel-tools: missing module dependency: %s\n", name); result = -1; break; }
        if (graph->modules[i].state == 1) { fprintf(stderr, "kernel-tools: module dependency cycle: %s\n", name); result = -1; break; }
        if (graph->modules[i].state == 2) continue;
        stack[depth++] = (struct frame){.module = i};
    }
    free(stack);
    return result;
}

static void graph_free(struct module_graph *graph)
{
    for (size_t i = 0; i < graph->count; ++i) {
        struct module_record *record = &graph->modules[i];
        free(record->name); free(record->phase); free(record->autoload);
        for (size_t j = 0; j < record->dependency_count; ++j) free(record->dependencies[j]);
        free(record->dependencies);
    }
    for (size_t i = 0; i < graph->builtin_count; ++i) free(graph->builtins[i]);
    free(graph->builtins); free(graph->modules); free(graph->order);
}

static int names_json(FILE *stream, const struct module_graph *graph, const char *which)
{
    fputc('[', stream); bool first = true;
    size_t count = !strcmp(which, "early") ? graph->order_count : graph->count;
    unsigned phases = !strcmp(which, "boot") ? 2 : 1;
    for (unsigned phase = 0; phase < phases; ++phase) for (size_t i = 0; i < count; ++i) {
        const struct module_record *record = &graph->modules[!strcmp(which, "early") ? graph->order[i] : i];
        if (!strcmp(which, "boot") && strcmp(record->phase, phase ? "filesystem" : "block")) continue;
        if (!strcmp(which, "platform") && strcmp(record->autoload, "platform")) continue;
        if (!first) fputc(',', stream);
        first = false;
        if (kernel_stage_quote(stream, record->name)) return -1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int manifest(struct kernel_stage *stage, const struct kernel_bundle *bundle, const struct module_graph *graph)
{
    struct kernel_stage_inventory files = {0};
    if (kernel_stage_inventory(stage->root_fd, &files)) { kernel_stage_inventory_free(&files); return -1; }
    char *encoded = NULL; size_t size = 0;
    FILE *stream = open_memstream(&encoded, &size);
    if (!stream) { kernel_stage_inventory_free(&files); return -1; }
    fprintf(stream, "{\"schema\":1,\"release\":\"%s\",\"module_abi\":\"%s\",\"kernel_sha256\":\"%s\",\"boot_modules\":",
            bundle->release, bundle->abi, kernel_bundle_find(bundle, "kernel.sys")->sha256);
    int result = names_json(stream, graph, "boot");
    fputs(",\"early_modules\":", stream); result |= names_json(stream, graph, "early");
    fputs(",\"platform_modules\":", stream); result |= names_json(stream, graph, "platform");
    fputs(",\"metadata\":{", stream);
    for (size_t i = 0; i < graph->count; ++i) {
        const struct module_record *record = &graph->modules[i];
        if (i) fputc(',', stream);
        result |= kernel_stage_quote(stream, record->name); fputs(":{\"path\":", stream);
        result |= kernel_stage_quote(stream, record->path); fputs(",\"phase\":", stream);
        result |= kernel_stage_quote(stream, record->phase); fputs(",\"autoload\":", stream);
        result |= kernel_stage_quote(stream, record->autoload); fputs(",\"depends\":[", stream);
        for (size_t j = 0; j < record->dependency_count; ++j) {
            if (j) fputc(',', stream);
            result |= kernel_stage_quote(stream, record->dependencies[j]);
        }
        fputs("]}", stream);
    }
    fputs("},\"files\":", stream); result |= kernel_stage_files_json(stream, &files);
    fputs("}\n", stream);
    if (ferror(stream)) result = -1;
    if (fclose(stream)) result = -1;
    if (!result) result = kernel_stage_write(stage->root_fd, "modules-stage.json", encoded, size, 0644);
    free(encoded); kernel_stage_inventory_free(&files);
    return result;
}

int kernel_modules_stage(int argc, char **argv)
{
    if (argc < 3) return 2;
    struct kernel_bundle bundle = {.root_fd = -1};
    struct kernel_stage stage = {.root_fd = -1, .parent_fd = -1};
    struct module_graph graph = {0};
    int result = kernel_bundle_open(&bundle, argv[0], (size_t)(argc - 3), (const char *const *)(argv + 3));
    if (result) { fprintf(stderr, "kernel-tools: %s\n", bundle.error); goto out; }
    result = read_graph(&graph, &bundle, argv[0], argv[1]);
    if (result) goto out;
    for (size_t i = 0; i < graph.count; ++i) if (visit(&graph, i)) { result = -1; goto out; }
    for (size_t i = 0; i < graph.count; ++i) graph.modules[i].state = 0;
    graph.order_count = 0;
    for (unsigned phase = 0; phase < 2; ++phase) for (size_t i = 0; i < graph.count; ++i)
        if (!strcmp(graph.modules[i].phase, phase ? "filesystem" : "block") && visit(&graph, i)) { result = -1; goto out; }
    if (kernel_stage_begin(&stage, argv[2])) { result = -1; goto out; }
    for (size_t i = 0; i < bundle.file_count; ++i) {
        const struct kernel_bundle_file *file = &bundle.files[i];
        if (strncmp(file->name, bundle.prefix, strlen(bundle.prefix))) continue;
        if (kernel_stage_copy(bundle.root_fd, file->name, stage.root_fd, file->name, file->sha256, 0644)) { result = -1; goto out; }
    }
    char *arguments[] = {argv[1], "depmod", "-b", stage.path, (char *)bundle.release, NULL};
    if (kernel_stage_run(arguments, NULL)) { result = -1; goto out; }
    char *platform = NULL; size_t platform_size = 0;
    FILE *stream = open_memstream(&platform, &platform_size);
    if (!stream) { result = -1; goto out; }
    for (size_t i = 0; i < graph.count; ++i)
        if (!strcmp(graph.modules[i].autoload, "platform")) fprintf(stream, "%s\n", graph.modules[i].name);
    result = ferror(stream) ? -1 : 0;
    if (fclose(stream)) result = -1;
    char platform_path[128];
    snprintf(platform_path, sizeof(platform_path), "%smodules-load.d/reliefnt-platform.conf", bundle.prefix);
    if (!result) result = kernel_stage_write(stage.root_fd, platform_path, platform, platform_size, 0644);
    free(platform);
    if (!result) result = manifest(&stage, &bundle, &graph);
    if (!result) result = kernel_stage_publish(&stage, "modules-stage.json");
out:
    if (result) fprintf(stderr, "kernel-tools: module stage failed: %s\n", strerror(errno));
    graph_free(&graph); kernel_stage_end(&stage); kernel_bundle_close(&bundle);
    return result ? 1 : 0;
}
