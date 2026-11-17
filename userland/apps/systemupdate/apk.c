/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int update_apk(struct update_context *context, const char *root, int network,
               const char *output, const char *const arguments[])
{
    char repositories[PATH_MAX], keys[PATH_MAX];
    int result = update_path(repositories, context->plan, "sources");
    if (!result) result = update_path(keys, context->plan, "snapshot/etc/apk/keys");
    if (result) return result;
    char *argv[160]; size_t count = 0;
#define ARG(value) do { if (count >= sizeof(argv)/sizeof(argv[0])-1) return -E2BIG; argv[count++] = (char *)(value); } while (0)
    ARG(context->apk); ARG("--root"); ARG(root); ARG("--arch"); ARG("x86_64");
    ARG("--keys-dir"); ARG(keys); ARG("--repositories-file"); ARG(repositories);
    /* apk-tools interprets this value in minutes using a signed int. */
    ARG("--cache-dir"); ARG(context->cache); ARG("--cache-max-age"); ARG("31536000");
    ARG("--timeout"); ARG("30"); ARG("--no-progress"); ARG("--no-interactive");
    ARG("--check-certificate");
    if (context->committing) ARG("--preserve-env");
    if (!network) ARG("--no-network");
    for (size_t i = 0; arguments[i]; ++i) ARG(arguments[i]);
    argv[count] = NULL;
#undef ARG
    return update_run(context, argv, output, context->committing);
}

static int valid_url(const char *url, int isolated)
{
    const char *host;
    if (!strncmp(url, "https://", 8)) host = url + 8;
    else if (isolated && (!strncmp(url, "http://127.0.0.1:", 17) ||
                          !strncmp(url, "http://127.0.0.1/", 17) ||
                          !strncmp(url, "http://[::1]:", 13))) host = url + 7;
    else return 0;
    if (!*host || *host == '/' || strpbrk(url, "\\\"'\t\r\n ?#$`")) return 0;
    const char *end = strchr(host, '/');
    size_t size = end ? (size_t)(end - host) : strlen(host);
    return size && !memchr(host, '@', size);
}

/* APK's explicit repositories-file disables its implicit .d scans. Flatten
 * both supported trees ourselves, retaining origin tags and original URLs. */
static int source_file(struct update_context *context, const char *path, FILE *output)
{
    char *bytes; size_t size; int result = update_read(path, &bytes, &size, 1024u * 1024u);
    if (result) return result;
    if (memchr(bytes, 0, size)) { free(bytes); return -EINVAL; }
    char *save = NULL;
    for (char *line = strtok_r(bytes, "\n", &save); !result && line; line = strtok_r(NULL, "\n", &save)) {
        while (isspace((unsigned char)*line)) ++line;
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
        if (!*line || *line == '#') continue;
        int ndx = 0; char tag[80] = {0};
        if (!strncmp(line, "ndx ", 4) || !strncmp(line, "ndx\t", 4)) {
            ndx = 1; line += 4; while (*line == ' ' || *line == '\t') ++line;
        }
        if (*line == '@') {
            char *separator = strpbrk(line, " \t");
            if (!separator || separator - line < 2 || (size_t)(separator - line) >= sizeof(tag)) { result = -EINVAL; break; }
            memcpy(tag, line, (size_t)(separator - line));
            if (strspn(tag + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(tag + 1)) {
                result = -EINVAL; break;
            }
            line = separator; while (*line == ' ' || *line == '\t') ++line;
        }
        if (!valid_url(line, context->isolated)) { result = -EINVAL; break; }
        if (fprintf(output, "%s%s%s%s\n", ndx ? "ndx " : "", tag, *tag ? " " : "", line) < 0) result = -EIO;
    }
    free(bytes); return result;
}

static int source_directory(struct update_context *context, const char *path, const char *overrides, FILE *output)
{
    int directory = update_directory(path, 0);
    if (directory == -ENOENT) return 0;
    if (directory < 0) return directory;
    close(directory);
    struct dirent **entries; int count = scandir(path, &entries, NULL, alphasort);
    if (count < 0) return -errno;
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name; size_t size = strlen(name);
        if (!result && size > 5 && !strcmp(name + size - 5, ".list")) {
            int hidden = 0;
            if (overrides) {
                char override[PATH_MAX]; result = update_path(override, overrides, name);
                if (!result) {
                    int present = update_regular(override, 1024u * 1024u);
                    if (!present) hidden = 1;
                    else if (present != -ENOENT) result = present;
                }
            }
            char file[PATH_MAX]; if (!result) result = update_path(file, path, name);
            if (!result && !hidden) result = source_file(context, file, output);
        }
        free(entries[i]);
    }
    free(entries); return result;
}

int update_rpr_url(struct update_context *context, char output[2048])
{
    char path[PATH_MAX], *bytes; size_t size;
    int result = update_path(path, context->root, "etc/reliefos/rpr.conf");
    if (!result) result = update_read(path, &bytes, &size, 4096);
    if (result) return result;
    if (memchr(bytes, 0, size)) { free(bytes); return -EINVAL; }
    int found = 0; char *save = NULL;
    for (char *line = strtok_r(bytes, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "RPR_BASE_URL=", 13)) continue;
        const char *value = line + 13;
        if (found++ || strlen(value) >= 2048 || !valid_url(value, context->isolated)) { result = -EINVAL; break; }
        strcpy(output, value);
        size_t length = strlen(output);
        while (length && output[length - 1] == '/') output[--length] = 0;
    }
    free(bytes); return result ? result : found == 1 ? 0 : -EINVAL;
}

int update_sources(struct update_context *context)
{
    char path[PATH_MAX], *buffer = NULL; size_t size = 0;
    FILE *output = open_memstream(&buffer, &size); if (!output) return -errno;
    int result = update_path(path, context->plan, "snapshot/etc/apk/repositories");
    if (!result) result = source_file(context, path, output);
    if (result == -ENOENT) result = 0;
    char overrides[PATH_MAX];
    if (!result) result = update_path(overrides, context->plan, "snapshot/etc/apk/repositories.d");
    if (!result) result = source_directory(context, overrides, NULL, output);
    if (!result) result = update_path(path, context->plan, "snapshot/lib/apk/repositories.d");
    if (!result) result = source_directory(context, path, overrides, output);
    char url[2048]; int rpr = update_rpr_url(context, url);
    if (!result && !rpr && fprintf(output, "ndx %s/apk/packages.adb\n", url) < 0) result = -EIO;
    if (!result && rpr && !(context->isolated && rpr == -ENOENT)) result = rpr;
    if (fclose(output) && !result) result = -errno;
    if (!result && !size) result = -ENOENT;
    if (!result) result = update_path(path, context->plan, "sources");
    if (!result) result = update_write(path, buffer, size, 0600);
    free(buffer); return result;
}

int update_probe(struct update_context *context)
{
    char url[2048], address[2100], output[PATH_MAX];
    int result = update_rpr_url(context, url);
    if (!result) result = update_path(output, context->base, "probe.txt");
    if (result) return result;
    int size = snprintf(address, sizeof(address), "%s/health.txt", url);
    if (size < 0 || (size_t)size >= sizeof(address)) return -ENAMETOOLONG;
    char *args[] = {context->fetch, address, output, "64", NULL};
    result = update_run(context, args, NULL, 0);
    char *bytes = NULL; size_t length = 0;
    if (!result) result = update_read(output, &bytes, &length, 64);
    if (!result && (length != 14 || memcmp(bytes, "leonos-rpr-ok\n", 14))) result = -EINVAL;
    free(bytes);
    if (!result) update_event(context, "repository-reachable");
    return result;
}

/* Registration never downloads, replaces or imports a trust key. The signed
 * index must verify with the administrator's existing APK keyring. */
int update_configure(struct update_context *context)
{
    char url[2048], path[PATH_MAX], index[PATH_MAX], address[2100];
    int result = update_rpr_url(context, url);
    if (!result) result = update_path(index, context->base, "configure-index.adb");
    if (result) return result;
    int length = snprintf(address, sizeof(address), "%s/apk/packages.adb", url);
    if (length < 0 || (size_t)length >= sizeof(address)) return -ENAMETOOLONG;
    char *fetch[] = {context->fetch, address, index, "134217728", NULL};
    result = update_run(context, fetch, NULL, 0);
    char keys[PATH_MAX];
    if (!result) result = update_path(keys, context->root, "etc/apk/keys");
    char *verify[] = {context->apk, "--keys-dir", keys, "verify", index, NULL};
    if (!result) result = update_run(context, verify, NULL, 0);
    char *bytes = NULL; size_t size = 0;
    if (!result) result = update_path(path, context->root, "etc/apk/repositories");
    if (!result) result = update_read(path, &bytes, &size, 1024u * 1024u);
    if (result == -ENOENT) result = 0;
    if (result) { free(bytes); return result; }
    char entry[2100]; length = snprintf(entry, sizeof(entry), "ndx %s/apk/packages.adb", url);
    if (length < 0 || (size_t)length >= sizeof(entry)) { free(bytes); return -ENAMETOOLONG; }
    int exists = 0;
    if (bytes) {
        const char *at = bytes;
        while (*at) {
            const char *end = strchr(at, '\n'); size_t count = end ? (size_t)(end - at) : strlen(at);
            if (count == (size_t)length && !memcmp(at, entry, count)) exists = 1;
            if (!end) break;
            at = end + 1;
        }
    }
    if (!exists) {
        char *next = malloc(size + (size_t)length + 3);
        if (!next) { free(bytes); return -ENOMEM; }
        if (size) memcpy(next, bytes, size);
        if (size && next[size - 1] != '\n') next[size++] = '\n';
        memcpy(next + size, entry, (size_t)length); size += (size_t)length; next[size++] = '\n';
        result = update_write(path, next, size, 0644); free(next);
    }
    free(bytes);
    if (!result) update_event(context, "repository-configured-with-existing-trust");
    return result;
}

int update_cache_verify(struct update_context *context, int snapshot)
{
    char indexes[PATH_MAX]; int result = update_path(indexes, context->plan, "indexes");
    if (result) return result;
    int fd = update_directory(indexes, snapshot); if (fd < 0) return fd; close(fd);
    struct dirent **entries; int count = scandir(context->cache, &entries, NULL, alphasort);
    if (count < 0) return -errno;
    unsigned found = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name; size_t size = strlen(name);
        int index = !strncmp(name, "APKINDEX.", 9);
        int archive = size > 4 && !strcmp(name + size - 4, ".apk");
        if (!result && (index || archive)) {
            char source[PATH_MAX], copy[PATH_MAX];
            result = update_path(source, context->cache, name);
            if (!result) result = update_regular(source, UINT64_MAX);
            if (!result && index) {
                ++found;
                result = update_path(copy, indexes, name);
                if (!result && snapshot) result = update_copy(source, copy, 0600);
                else if (!result) {
                    char a[65], b[65]; result = update_hash(source, a);
                    if (!result) result = update_hash(copy, b);
                    if (!result && strcmp(a, b)) result = -ESTALE;
                }
            }
            if (!result) {
                const char *verify[] = {"verify", source, NULL};
                result = update_apk(context, context->root, 0, NULL, verify);
            }
        }
        free(entries[i]);
    }
    free(entries);
    if (!result && !found) result = -ENOENT;
    if (!result && !snapshot) {
        count = scandir(indexes, &entries, NULL, alphasort);
        if (count < 0) return -errno;
        unsigned expected = 0;
        for (int i = 0; i < count; ++i) { if (strncmp(entries[i]->d_name, "APKINDEX.", 9) == 0) ++expected; free(entries[i]); }
        free(entries);
        if (expected != found) result = -ESTALE;
    }
    return result;
}

int update_plan_seal(struct update_context *context, char output[65])
{
    const char *members[] = {"snapshot", "tools", "cache", "indexes", "sources", "preview", "release.txt"};
    char records[sizeof(members)/sizeof(members[0]) * 66], path[PATH_MAX]; size_t used = 0;
    for (size_t i = 0; i < sizeof(members)/sizeof(members[0]); ++i) {
        char digest[65]; int result = update_path(path, context->plan, members[i]);
        if (!result) result = i < 4 ? update_hash_tree(path, digest) : update_hash(path, digest);
        if (result == -ENOENT && i == 6) strcpy(digest, "absent");
        else if (result) return result;
        size_t size = strlen(digest); memcpy(records + used, digest, size); used += size; records[used++] = '\n';
    }
    /* Use the same streaming SHA implementation without exposing a hash API. */
    int result = update_path(path, context->plan, ".seal-input");
    if (!result) result = update_write(path, records, used, 0600);
    if (!result) result = update_hash(path, output);
    return result;
}
