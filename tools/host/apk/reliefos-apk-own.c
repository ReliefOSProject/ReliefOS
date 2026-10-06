/* Convert the APK ownership policy and a raw root into a stable package list. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "tools/host/common/buffer.h"
#include "tools/host/common/io.h"
#include "tools/host/manifest/json.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rule { char *path; const char *group; };
struct entry { char *path; char *line; int elf; };
struct state {
    const char *root;
    const char *force_group;
    struct rule *rules;
    size_t rule_count;
    size_t rule_capacity;
    struct entry *entries;
    size_t entry_count;
    size_t entry_capacity;
};

#if defined(__GNUC__) || defined(__clang__)
static int fail(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif

static void usage(void)
{
    fputs("usage: reliefos-apk-own --policy FILE --root DIR --output FILE\n", stderr);
}

static int fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
    return -1;
}

static char *join2(const char *left, const char *right)
{
    size_t a = strlen(left), b = strlen(right);
    int slash = a != 0u && left[a - 1u] != '/';
    char *result;
    if (a > (size_t)-1 - b - (size_t)slash - 1u) { errno = ENOMEM; return NULL; }
    result = malloc(a + b + (size_t)slash + 1u);
    if (result == NULL) return NULL;
    memcpy(result, left, a);
    if (slash != 0) result[a++] = '/';
    memcpy(result + a, right, b + 1u);
    return result;
}

static int valid_guest_path(const char *path)
{
    const char *p = path;
    if (*p == '\0' || *p == '/') return 0;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            unsigned char c = (unsigned char)*p++;
            if (c < 0x20u || c == 0x7fu || c == '\\') return 0;
        }
        if (p == start || (p - start == 1 && start[0] == '.') ||
            (p - start == 2 && start[0] == '.' && start[1] == '.')) return 0;
        if (*p == '/') p++;
    }
    return 1;
}

static int add_rule(struct state *state, const char *path, const char *group)
{
    size_t i;
    struct rule *grown;
    if (!valid_guest_path(path)) return fail("invalid ownership path: %s", path);
    for (i = 0; i < state->rule_count; i++) {
        if (strcmp(state->rules[i].path, path) == 0) {
            if (strcmp(state->rules[i].group, group) != 0)
                return fail("ambiguous ownership for %s", path);
            return 0;
        }
    }
    if (state->rule_count == state->rule_capacity) {
        size_t capacity = state->rule_capacity == 0u ? 64u : state->rule_capacity * 2u;
        grown = realloc(state->rules, capacity * sizeof(*grown));
        if (grown == NULL) return fail("out of memory");
        state->rules = grown;
        state->rule_capacity = capacity;
    }
    state->rules[state->rule_count].path = strdup(path);
    if (state->rules[state->rule_count].path == NULL) return fail("out of memory");
    state->rules[state->rule_count].group = group;
    state->rule_count++;
    return 0;
}

static int add_string_rules(struct state *state, const json_value *array,
    const char *group, const char *prefix, const char *suffix)
{
    size_t i;
    if (array == NULL) return 0;
    if (array->type != JSON_ARRAY) return fail("%s must be an array", prefix);
    for (i = 0; i < json_child_count(array); i++) {
        const char *value = json_text(json_child(array, i));
        char *first;
        char *path;
        if (value == NULL) return fail("ownership arrays must contain strings");
        first = prefix[0] == '\0' ? strdup(value) : join2(prefix, value);
        if (first == NULL) return fail("out of memory");
        if (suffix[0] != '\0') {
            size_t n = strlen(first) + strlen(suffix) + 1u;
            path = malloc(n);
            if (path != NULL) (void)snprintf(path, n, "%s%s", first, suffix);
            free(first);
        } else path = first;
        if (path == NULL || add_rule(state, path, group) != 0) { free(path); return -1; }
        free(path);
    }
    return 0;
}

static int load_rules(struct state *state, const char *policy_path, json_value *policy)
{
    struct byte_buffer input = {NULL, 0, 0};
    char error[256];
    const json_value *groups;
    long version;
    size_t i;
    if (read_file_all(policy_path, &input) != 0) return fail("cannot read %s: %s", policy_path, strerror(errno));
    if (json_parse((const char *)input.data, input.len, policy, error, sizeof(error)) != 0) {
        buffer_destroy(&input); return fail("%s: %s", policy_path, error);
    }
    buffer_destroy(&input);
    if (json_int(json_member(policy, "version"), &version) != 0 || version != 1L ||
        strcmp(json_text(json_member(policy, "apk_registration")) != NULL ?
        json_text(json_member(policy, "apk_registration")) : "", "not-installed") != 0)
        return fail("unsupported ownership policy");
    groups = json_member(policy, "groups");
    if (groups == NULL || groups->type != JSON_OBJECT) return fail("groups must be an object");
    for (i = 0; i < json_child_count(groups); i++) {
        const json_value *group = json_child(groups, i);
        const char *name = groups->keys[i];
        const json_value *stems = json_member(group, "library_stems");
        if (group->type != JSON_OBJECT) return fail("group %s must be an object", name);
        if (add_string_rules(state, json_member(group, "paths"), name, "", "") != 0 ||
            add_string_rules(state, json_member(group, "components"), name,
                "usr/lib/reliefos/apps", "") != 0 ||
            add_string_rules(state, stems, name, "", ".so") != 0 ||
            add_string_rules(state, stems, name, "", ".a") != 0 ||
            add_string_rules(state, stems, name, "", ".la") != 0) return -1;
    }
    {
        static const struct { const char *group; const char *path; } tool_paths[] = {
            {"reliefos-fastfetch", "usr/bin/fastfetch"}, {"reliefos-fastfetch", "usr/share/licenses/fastfetch"},
            {"reliefos-fastfetch", "etc/fastfetch"}, {"reliefos-fastfetch", "usr/share/fastfetch/leonos-ascii.txt"},
            {"reliefos-fastfetch", "etc/skel/.config/hyfetch.json"},
            {"reliefos-apps", "usr/share/licenses/pleditor"},
            {"busybox", "bin/busybox"}, {"busybox", "bin/sh"}, {"busybox", "usr/share/licenses/busybox"},
            {"ncurses", "usr/share/terminfo"}, {"ncurses", "etc/terminfo"},
            {"ncurses", "usr/share/licenses/ncurses"}, {"sl", "usr/bin/sl"}, {"sl", "usr/share/licenses/sl"}
        };
        static const char *const ncurses_commands[] = {
            "clear", "infocmp", "infotocap", "captoinfo", "reset", "tabs", "tic",
            "toe", "tput", "tset", "ncursesw6-config"
        };
        static const char *const util_commands[] = {
            "usr/sbin/fdisk", "usr/sbin/sfdisk", "usr/sbin/runuser", "usr/sbin/fsck",
            "usr/sbin/blkid", "bin/mount", "bin/umount", "bin/lsblk"
        };
        static const char *const util_libraries[] = {
            "usr/lib/libfdisk.so.1", "usr/lib/libsmartcols.so.1", "usr/lib/libuuid.so.1",
            "usr/lib/libblkid.so.1", "usr/lib/libmount.so.1"
        };
        static const char *const filesystem_commands[] = {
            "usr/sbin/mkfs.ext4", "usr/sbin/fsck.ext4", "usr/sbin/mkfs.ext2", "usr/sbin/fsck.ext2", "usr/sbin/mkfs.fat",
            "usr/sbin/fsck.fat", "usr/sbin/mkfs.exfat", "usr/sbin/fsck.exfat"
        };
        size_t item;
        for (item = 0; item < sizeof(tool_paths) / sizeof(tool_paths[0]); item++)
            if (add_rule(state, tool_paths[item].path, tool_paths[item].group) != 0) return -1;
        for (item = 0; item < sizeof(ncurses_commands) / sizeof(ncurses_commands[0]); item++) {
            char *path = join2("usr/bin", ncurses_commands[item]);
            int result = path == NULL ? -1 : add_rule(state, path, "ncurses");
            free(path);
            if (result != 0) return -1;
        }
        for (item = 0; item < sizeof(util_commands) / sizeof(util_commands[0]); item++)
            if (add_rule(state, util_commands[item], "storage-util-linux") != 0) return -1;
        for (item = 0; item < sizeof(util_libraries) / sizeof(util_libraries[0]); item++)
            if (add_rule(state, util_libraries[item], "storage-util-linux") != 0) return -1;
        for (item = 0; item < sizeof(filesystem_commands) / sizeof(filesystem_commands[0]); item++)
            if (add_rule(state, filesystem_commands[item], "storage-filesystems") != 0) return -1;
    }
    return 0;
}

static const char *classify(const struct state *state, const char *path)
{
    size_t i, best = 0u;
    const char *group = "reliefos-base";
    for (i = 0; i < state->rule_count; i++) {
        size_t n = strlen(state->rules[i].path);
        int boundary = strncmp(path, state->rules[i].path, n) == 0 &&
            (path[n] == '\0' || path[n] == '/');
        int versioned_so = strncmp(path, state->rules[i].path, n) == 0 &&
            n >= 3u && strcmp(state->rules[i].path + n - 3u, ".so") == 0 && path[n] == '.';
        if ((boundary != 0 || versioned_so != 0) && n > best) {
            best = n; group = state->rules[i].group;
        }
    }
    return group;
}

/* Resolve a link lexically in guest / without ever dereferencing a host path. */
static char *guest_link_target(const char *relative, const char *target)
{
    char *input;
    char *copy;
    char **parts;
    size_t used = 0u, capacity, i, length = 1u;
    char *token;
    char *save = NULL;
    char *result;
    if (target[0] == '/') input = strdup(target + 1);
    else {
        const char *slash = strrchr(relative, '/');
        size_t directory = slash == NULL ? 0u : (size_t)(slash - relative);
        input = malloc(directory + (directory != 0u ? 1u : 0u) + strlen(target) + 1u);
        if (input != NULL) {
            if (directory != 0u) { memcpy(input, relative, directory); input[directory++] = '/'; }
            strcpy(input + directory, target);
        }
    }
    if (input == NULL) return NULL;
    copy = input;
    capacity = strlen(input) / 2u + 2u;
    parts = calloc(capacity, sizeof(*parts));
    if (parts == NULL) { free(input); return NULL; }
    token = strtok_r(copy, "/", &save);
    while (token != NULL) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') { }
        else if (strcmp(token, "..") == 0) { if (used != 0u) used--; }
        else parts[used++] = token;
        token = strtok_r(NULL, "/", &save);
    }
    for (i = 0; i < used; i++) length += strlen(parts[i]) + (i != 0u ? 1u : 0u);
    result = malloc(length);
    if (result != NULL) {
        result[0] = '\0';
        for (i = 0; i < used; i++) { if (i != 0u) strcat(result, "/"); strcat(result, parts[i]); }
    }
    free(parts); free(input);
    return result;
}

static int append_entry(struct state *state, const char *relative, const struct stat *info,
    const char *target)
{
    const char *type = S_ISDIR(info->st_mode) ? "dir" : S_ISLNK(info->st_mode) ? "symlink" : "file";
    const char *group = S_ISDIR(info->st_mode) ? "reliefos-base" : classify(state, relative);
    char *resolved = NULL;
    size_t length = strlen(group) + strlen(type) + strlen(relative) + strlen(target) + 32u;
    char *line;
    struct entry *grown;
    if (S_ISLNK(info->st_mode)) {
        resolved = guest_link_target(relative, target);
        if (resolved == NULL) return fail("out of memory");
        if (strcmp(classify(state, resolved), "reliefos-base") != 0)
            group = classify(state, resolved);
    }
    if (state->force_group != NULL) group = state->force_group;
    length = strlen(group) + strlen(type) + strlen(relative) + strlen(target) + 32u;
    line = malloc(length);
    free(resolved);
    if (line == NULL) return fail("out of memory");
    (void)snprintf(line, length, "%s\t%s\t%04o\t%s\t%s", group, type,
        (unsigned int)(info->st_mode & 07777u), relative, target);
    if (state->entry_count == state->entry_capacity) {
        size_t capacity = state->entry_capacity == 0u ? 128u : state->entry_capacity * 2u;
        grown = realloc(state->entries, capacity * sizeof(*grown));
        if (grown == NULL) { free(line); return fail("out of memory"); }
        state->entries = grown; state->entry_capacity = capacity;
    }
    state->entries[state->entry_count].path = strdup(relative);
    state->entries[state->entry_count].line = line;
    state->entries[state->entry_count].elf = 0;
    if (state->entries[state->entry_count].path == NULL) { free(line); return fail("out of memory"); }
    state->entry_count++;
    return 0;
}

static int walk_tree(struct state *state, const char *relative)
{
    char *path = relative[0] == '\0' ? strdup(state->root) : join2(state->root, relative);
    struct stat info;
    DIR *directory;
    struct dirent *item;
    int result = 0;
    if (path == NULL) return fail("out of memory");
    if (lstat(path, &info) != 0) { result = fail("cannot inspect %s: %s", path, strerror(errno)); goto done; }
    if (S_ISLNK(info.st_mode)) {
        size_t capacity = info.st_size > 0 ? (size_t)info.st_size + 2u : 4097u;
        char *target = malloc(capacity);
        ssize_t got;
        if (target == NULL) { result = fail("out of memory"); goto done; }
        got = readlink(path, target, capacity - 1u);
        if (got < 0 || (size_t)got == capacity - 1u) { free(target); result = fail("cannot read link %s", path); goto done; }
        target[got] = '\0';
        result = append_entry(state, relative, &info, target);
        free(target); goto done;
    }
    if (S_ISREG(info.st_mode)) {
        unsigned char magic[4];
        FILE *input = fopen(path, "rb");
        if (!input) { result = fail("cannot read %s", path); goto done; }
        size_t got = fread(magic, 1, sizeof magic, input);
        int failed = ferror(input);
        if (fclose(input)) failed = 1;
        if (failed) { result = fail("cannot read %s", path); goto done; }
        result = append_entry(state, relative, &info, "-");
        if (!result) state->entries[state->entry_count - 1u].elf =
            got == sizeof magic && !memcmp(magic, "\177ELF", sizeof magic);
        goto done;
    }
    if (!S_ISDIR(info.st_mode)) { result = fail("special file is not packageable: %s", path); goto done; }
    if (*relative && append_entry(state, relative, &info, "-") != 0) { result = -1; goto done; }
    directory = opendir(path);
    if (directory == NULL) { result = fail("cannot open %s: %s", path, strerror(errno)); goto done; }
    while ((item = readdir(directory)) != NULL && result == 0) {
        char *child;
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
        child = relative[0] == '\0' ? strdup(item->d_name) : join2(relative, item->d_name);
        if (child == NULL) { result = fail("out of memory"); break; }
        if (!valid_guest_path(child)) result = fail("unsafe package path: %s", child);
        else result = walk_tree(state, child);
        free(child);
    }
    if (closedir(directory) != 0 && result == 0) result = fail("cannot close %s", path);
done:
    free(path);
    return result;
}

static int compare_entries(const void *left, const void *right)
{
    const struct entry *a = left, *b = right;
    return strcmp(a->path, b->path);
}

static void quote_json(FILE *stream, const char *text)
{
    fputc('"', stream);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', stream); fputc(*p, stream); }
        else if (*p < 0x20u) fprintf(stream, "\\u%04x", (unsigned)*p);
        else fputc(*p, stream);
    }
    fputc('"', stream);
}

static void emit_json(FILE *stream, const json_value *value, int policy_root)
{
    switch (value->type) {
    case JSON_NULL: fputs("null", stream); break;
    case JSON_FALSE: fputs("false", stream); break;
    case JSON_TRUE: fputs("true", stream); break;
    case JSON_NUMBER: fprintf(stream, "%.17g", value->number); break;
    case JSON_STRING: quote_json(stream, value->text); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        fputc(value->type == JSON_ARRAY ? '[' : '{', stream);
        for (size_t i = 0; i < value->child_count; ++i) {
            if (i) fputc(',', stream);
            if (value->type == JSON_OBJECT) {
                quote_json(stream, value->keys[i]); fputc(':', stream);
                if (policy_root && !strcmp(value->keys[i], "apk_registration")) {
                    quote_json(stream, "installed-by-upstream-apk"); continue;
                }
                if (policy_root && !strcmp(value->keys[i], "current_distributor")) {
                    quote_json(stream, "reliefos-apk"); continue;
                }
            }
            emit_json(stream, &value->children[i], 0);
        }
        fputc(value->type == JSON_ARRAY ? ']' : '}', stream);
        break;
    }
}

static int installed_policy(const json_value *policy, const char *path)
{
    char *data = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&data, &length);
    if (!stream) return -1;
    emit_json(stream, policy, 1);
    fputc('\n', stream);
    int failed = ferror(stream);
    if (fclose(stream)) failed = 1;
    int result = failed ? -1 : write_file_if_changed(path, data, length, 0644u);
    free(data);
    return result;
}

int main(int argc, char **argv)
{
    const char *policy_path = NULL, *root = NULL, *output = NULL, *installed = NULL, *elf_list = NULL;
    struct state state = {0};
    json_value policy = {0};
    struct byte_buffer result = {NULL, 0, 0};
    struct byte_buffer elf_result = {NULL, 0, 0};
    int i, status = 1;
    size_t n;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        if (i + 1 >= argc) { usage(); return 2; }
        if (strcmp(argv[i], "--policy") == 0) policy_path = argv[++i];
        else if (strcmp(argv[i], "--root") == 0) root = argv[++i];
        else if (strcmp(argv[i], "--output") == 0) output = argv[++i];
        else if (strcmp(argv[i], "--installed-policy") == 0) installed = argv[++i];
        else if (strcmp(argv[i], "--elf-list") == 0) elf_list = argv[++i];
        else if (strcmp(argv[i], "--group") == 0) state.force_group = argv[++i];
        else { usage(); return 2; }
    }
    if (policy_path == NULL || root == NULL || output == NULL) { usage(); return 2; }
    state.root = root;
    if (load_rules(&state, policy_path, &policy) != 0 || walk_tree(&state, "") != 0) goto done;
    qsort(state.entries, state.entry_count, sizeof(*state.entries), compare_entries);
    for (n = 0; n < state.entry_count; n++) {
        size_t length = strlen(state.entries[n].line);
        if (buffer_reserve(&result, result.len + length + 1u) != 0) { fail("out of memory"); goto done; }
        memcpy(result.data + result.len, state.entries[n].line, length);
        result.len += length; result.data[result.len++] = '\n';
        if (state.entries[n].elf) {
            if (buffer_reserve(&elf_result, elf_result.len + length + 1u) != 0) { fail("out of memory"); goto done; }
            memcpy(elf_result.data + elf_result.len, state.entries[n].line, length);
            elf_result.len += length;
            elf_result.data[elf_result.len++] = '\n';
        }
    }
    if (write_file_if_changed(output, result.data, result.len, 0644u) != 0) {
        fail("cannot write %s: %s", output, strerror(errno)); goto done;
    }
    if (installed != NULL && installed_policy(&policy, installed) != 0) { fail("cannot write installed policy"); goto done; }
    if (elf_list != NULL && write_file_if_changed(elf_list, elf_result.data, elf_result.len, 0644u) != 0) { fail("cannot write ELF inventory"); goto done; }
    status = 0;
done:
    for (n = 0; n < state.rule_count; n++) free(state.rules[n].path);
    for (n = 0; n < state.entry_count; n++) { free(state.entries[n].path); free(state.entries[n].line); }
    free(state.rules); free(state.entries); buffer_destroy(&result); buffer_destroy(&elf_result); json_free(&policy);
    return status;
}
