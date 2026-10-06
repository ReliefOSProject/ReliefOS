/*
 * Query and validate configs/dependencies.lock.json.
 *
 * The lock file is the single authority for which upstream source a build
 * unpacks, so this tool is the only place its grammar is interpreted: `make
 * fetch` reads its download list here, and the build adapters ask it for a
 * pinned commit or digest here. Nothing else may parse the file, and nothing
 * may re-derive a URL by string concatenation.
 *
 * The fetch list carries the id as well as the digest, url and cache name, so
 * the downloader can name the dependency it could not satisfy.
 *
 * Usage:
 *   reliefos-deps --lock PATH [--root DIR] --check
 *   reliefos-deps --lock PATH --list
 *   reliefos-deps --lock PATH --id ID --print FIELD
 *   reliefos-deps --lock PATH --fetch-list
 *
 * Exit status: 0 success, 1 usage, I/O or syntax failure, 2 the lock file's
 * content is invalid. The two are separated so `make doctor` can tell a broken
 * file from an unreadable one.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tools/host/common/io.h"
#include "tools/host/manifest/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCK_SCHEMA_VERSION 1L

#if defined(__GNUC__) || defined(__clang__)
static int report(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
static int report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
    return 2;
}

static void usage(void)
{
    fputs("usage: reliefos-deps --lock PATH [--root DIR]"
        " (--check | --list | --fetch-list | --id ID --print FIELD)\n", stderr);
    fputs("       --print feature returns base when an entry omits feature\n", stderr);
}

/* Relative paths in the lock file are joined onto the repository root, so an
 * escaping or absolute path would let a lock entry read or unpack outside the
 * tree it claims to describe. */
static int is_safe_relative_path(const char *value)
{
    const char *cursor;

    if (value == NULL || value[0] == '\0' || value[0] == '/') {
        return 0;
    }
    cursor = value;
    while (*cursor != '\0') {
        const char *segment = cursor;

        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        if (cursor == segment) {
            return 0; /* empty component: "//" or a trailing slash */
        }
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') {
            return 0;
        }
        if (cursor - segment == 1 && segment[0] == '.') {
            return 0;
        }
        while (segment < cursor) {
            if ((unsigned char)*segment < 0x20u || (unsigned char)*segment == 0x7Fu) {
                return 0;
            }
            if (*segment == ' ' || *segment == '\\') {
                return 0;
            }
            segment++;
        }
        if (*cursor == '/') {
            cursor++;
        }
    }
    return 1;
}

static int is_lowercase_hex(const char *value, size_t width)
{
    size_t index;

    if (value == NULL || strlen(value) != width) {
        return 0;
    }
    for (index = 0; index < width; index++) {
        char byte = value[index];

        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int is_valid_id(const char *value)
{
    size_t index;
    size_t length = strlen(value);

    if (length == 0 || length > 64) {
        return 0;
    }
    for (index = 0; index < length; index++) {
        char byte = value[index];
        int ok = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || (index > 0 && (byte == '.' || byte == '_' || byte == '+' || byte == '-'));

        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static const char *field(const json_value *entry, const char *name)
{
    return json_text(json_member(entry, name));
}

static const char *const known_fields[] = {
    "id", "kind", "version", "commit", "url", "sha256", "directory", "patches",
    "script", "license", "license_in_source", "target", "checksum_source", "note",
    "feature",
};

static int is_known_field(const char *name)
{
    size_t index;

    for (index = 0; index < sizeof(known_fields) / sizeof(known_fields[0]); index++) {
        if (strcmp(known_fields[index], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_downloaded(const char *kind)
{
    return kind != NULL && strcmp(kind, "submodule") != 0;
}

static int kind_is_known(const char *kind)
{
    if (kind == NULL) {
        return 0;
    }
    return strcmp(kind, "submodule") == 0 || strcmp(kind, "tarball") == 0
        || strcmp(kind, "apk") == 0 || strcmp(kind, "binary") == 0;
}

/* The cache file name is the last URL path component. It is stored in the fetch
 * list so the downloader never has to invent one. */
static const char *url_basename(const char *url)
{
    const char *slash = strrchr(url, '/');

    return (slash != NULL && slash[1] != '\0') ? slash + 1 : NULL;
}

static int has_query_or_fragment(const char *url)
{
    return strchr(url, '?') != NULL || strchr(url, '#') != NULL;
}

static int validate_entry(const json_value *entry, const char *root_dir, int index)
{
    const char *id = field(entry, "id");
    const char *kind = field(entry, "kind");
    size_t child;

    if (id == NULL || !is_valid_id(id)) {
        return report("dependency %d: \"id\" must be a lowercase, filename-safe name",
            index);
    }
    if (!kind_is_known(kind)) {
        return report("%s: \"kind\" must be submodule, tarball, apk or binary", id);
    }
    {
        /* Wherever the source lands, the name comes from the lock file, so an
         * escaping or absolute directory would let one entry unpack outside the
         * tree the build owns (plan section 9). */
        const char *directory = field(entry, "directory");

        if (directory != NULL && !is_safe_relative_path(directory)) {
            return report("%s: \"directory\" must be relative and inside the tree", id);
        }
    }
    for (child = 0; child < json_child_count(entry); child++) {
        const char *key = entry->keys[child];

        if (!is_known_field(key)) {
            return report("%s: unknown field \"%s\"", id, key);
        }
    }
    {
        const json_value *feature = json_member(entry, "feature");

        if (feature != NULL && (feature->type != JSON_STRING
                || (strcmp(feature->text, "base") != 0
                    && strcmp(feature->text, "xorg") != 0))) {
            return report("%s: \"feature\" must be base or xorg", id);
        }
    }
    if (is_downloaded(kind)) {
        const char *url = field(entry, "url");

        if (field(entry, "version") == NULL) {
            return report("%s: a downloaded dependency needs the \"version\" its URL"
                " serves", id);
        }
        const char *digest = field(entry, "sha256");

        if (url == NULL || strncmp(url, "https://", 8) != 0) {
            return report("%s: downloaded dependencies need an https \"url\"", id);
        }
        if (has_query_or_fragment(url) || url_basename(url) == NULL) {
            return report("%s: \"url\" must end in a file name and carry no query", id);
        }
        if (strpbrk(url, " \t\r\n") != NULL) {
            /* The fetch list is tab separated one-field-per-column text, and
             * curl is invoked with the url as a single argument. */
            return report("%s: \"url\" must contain no whitespace", id);
        }
        if (!is_lowercase_hex(digest, 64)) {
            return report("%s: \"sha256\" must be 64 lowercase hex digits", id);
        }
    } else {
        const char *commit = field(entry, "commit");
        const char *directory = field(entry, "directory");

        if (!is_lowercase_hex(commit, 40)) {
            return report("%s: submodules pin \"commit\" with 40 lowercase hex digits",
                id);
        }
        if (directory == NULL || !is_safe_relative_path(directory)) {
            return report("%s: submodules need a relative \"directory\"", id);
        }
    }
    {
        const json_value *patches = json_member(entry, "patches");
        size_t patch;

        if (patches != NULL && patches->type != JSON_ARRAY) {
            return report("%s: \"patches\" must be a list", id);
        }
        for (patch = 0; patches != NULL && patch < json_child_count(patches); patch++) {
            const json_value *item = json_child(patches, patch);
            const char *path;
            const char *digest;
            char *full;
            int present;

            if (item == NULL || item->type != JSON_OBJECT || json_child_count(item) != 2) {
                return report("%s: each patch is an object holding \"path\" and"
                    " \"sha256\"", id);
            }
            path = field(item, "path");
            digest = field(item, "sha256");
            if (path == NULL || !is_safe_relative_path(path)) {
                return report("%s: patch paths must be relative and safe", id);
            }
            if (!is_lowercase_hex(digest, 64)) {
                return report("%s: patch \"%s\" needs a 64 hex digit sha256", id, path);
            }
            if (root_dir == NULL) {
                continue;
            }
            /* The digest itself is compared by whoever consumes the bytes: the
             * adapter before applying a patch, `make fetch` before promoting a
             * download out of its temporary. Checking it twice here would need
             * a second SHA-256 implementation. */
            if (asprintf(&full, "%s/%s", root_dir, path) < 0) {
                return report("%s: out of memory", id);
            }
            present = access(full, R_OK) == 0;
            free(full);
            if (!present) {
                return report("%s: patch \"%s\" is not present in the tree", id, path);
            }
        }
    }
    {
        const char *script = field(entry, "script");
        const char *license = field(entry, "license");
        const char *inside = field(entry, "license_in_source");

        if (script != NULL && !is_safe_relative_path(script)) {
            return report("%s: \"script\" must be a relative path", id);
        }
        if (license == NULL && inside == NULL) {
            return report("%s: needs \"license\" or \"license_in_source\"", id);
        }
        if (license != NULL && !is_safe_relative_path(license)) {
            return report("%s: \"license\" must be a relative path", id);
        }
        if (inside != NULL && !is_safe_relative_path(inside)) {
            return report("%s: \"license_in_source\" must be a relative path", id);
        }
        /* "license" names a file that is already in this tree; the other form
         * names one inside an archive nobody has unpacked yet. */
        if (root_dir != NULL && license != NULL) {
            char *full;
            int present;

            if (asprintf(&full, "%s/%s", root_dir, license) < 0) {
                return report("%s: out of memory", id);
            }
            present = access(full, R_OK) == 0;
            free(full);
            if (!present) {
                return report("%s: license file \"%s\" is not in the tree", id, license);
            }
        }
    }
    return 0;
}

static int validate(const json_value *lock, const char *root_dir, int require_entries)
{
    const json_value *schema = json_member(lock, "schema_version");
    const json_value *dependencies = json_member(lock, "dependencies");
    long version;
    size_t index;

    if (lock->type != JSON_OBJECT) {
        return report("the lock file must contain one object");
    }
    if (schema == NULL || json_int(schema, &version) != 0) {
        return report("\"schema_version\" must be the integer %ld",
            LOCK_SCHEMA_VERSION);
    }
    if (version != LOCK_SCHEMA_VERSION) {
        return report("unsupported \"schema_version\" %ld, this build reads %ld",
            version, LOCK_SCHEMA_VERSION);
    }
    if (json_child_count(lock) != 2 || dependencies == NULL) {
        return report("the document must hold exactly \"schema_version\" and"
            " \"dependencies\"");
    }
    if (dependencies->type != JSON_ARRAY) {
        return report("\"dependencies\" must be a list");
    }
    if (require_entries && json_child_count(dependencies) == 0) {
        return report("\"dependencies\" must not be empty");
    }
    for (index = 0; index < json_child_count(dependencies); index++) {
        const json_value *entry = json_child(dependencies, index);
        size_t other;

        if (entry->type != JSON_OBJECT) {
            return report("dependency %zu must be an object", index);
        }
        if (validate_entry(entry, root_dir, (int)index) != 0) {
            return 2;
        }
        for (other = 0; other < index; other++) {
            const char *first = field(json_child(dependencies, other), "id");
            const char *second = field(entry, "id");

            if (first != NULL && second != NULL && strcmp(first, second) == 0) {
                return report("id \"%s\" is recorded twice", second);
            }
        }
    }
    return 0;
}

static const json_value *find_entry(const json_value *lock, const char *id)
{
    const json_value *dependencies = json_member(lock, "dependencies");
    size_t index;

    for (index = 0; index < json_child_count(dependencies); index++) {
        const json_value *entry = json_child(dependencies, index);

        if (strcmp(field(entry, "id") ? field(entry, "id") : "", id) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* Emits `<sha256>\t<path>` for every patch, so the adapter that applies them
 * can compare the digest of the bytes it is about to use. */
static int list_patches(const json_value *lock, const char *id)
{
    const json_value *entry = find_entry(lock, id);
    const json_value *patches;
    size_t index;

    if (entry == NULL) {
        (void)fprintf(stderr, "no dependency named \"%s\"\n", id);
        return 2;
    }
    patches = json_member(entry, "patches");
    if (patches == NULL) {
        return 0;
    }
    if (patches->type != JSON_ARRAY) {
        (void)fprintf(stderr, "%s: \"patches\" is not a list\n", id);
        return 2;
    }
    for (index = 0; index < json_child_count(patches); index++) {
        const json_value *item = json_child(patches, index);

        printf("%s\t%s\n", field(item, "sha256"), field(item, "path"));
    }
    return 0;
}

static int print_field(const json_value *lock, const char *id, const char *name)
{
    const json_value *entry = find_entry(lock, id);
    const json_value *value;

    if (entry == NULL) {
        (void)fprintf(stderr, "no dependency named \"%s\"\n", id);
        return 2;
    }
    if (!is_known_field(name)) {
        (void)fprintf(stderr, "%s: no \"%s\" field is recorded\n", id, name);
        return 2;
    }
    value = json_member(entry, name);
    if (value == NULL && strcmp(name, "feature") == 0) {
        puts("base");
        return 0;
    }
    if (value == NULL) {
        (void)fprintf(stderr, "%s: no \"%s\" field is recorded\n", id, name);
        return 2;
    }
    if (value->type == JSON_STRING) {
        printf("%s\n", value->text);
        return 0;
    }
    if (value->type != JSON_ARRAY) {
        (void)fprintf(stderr, "%s: \"%s\" is neither text nor a list\n", id, name);
        return 2;
    }
    /* Lists such as "patches" print one element per line. An empty list is a
     * valid answer and prints nothing. */
    {
        size_t index;

        for (index = 0; index < json_child_count(value); index++) {
            const char *text = json_text(json_child(value, index));

            if (text == NULL) {
                (void)fprintf(stderr, "%s: \"%s\" holds a non-text element\n", id,
                    name);
                return 2;
            }
            printf("%s\n", text);
        }
    }
    return 0;
}

static int compare_ids(const void *first, const void *second)
{
    const json_value *a = *(const json_value *const *)first;
    const json_value *b = *(const json_value *const *)second;

    return strcmp(field(a, "id"), field(b, "id"));
}

/* Collects the entries sorted by id, so the fetch list and the id list are the
 * same on every host regardless of how the file happens to be ordered. */
static const json_value **sorted_entries(const json_value *lock, size_t *out_count)
{
    const json_value *dependencies = json_member(lock, "dependencies");
    size_t total = json_child_count(dependencies);
    const json_value **entries = calloc(total == 0 ? 1u : total, sizeof(*entries));

    *out_count = 0;
    if (entries == NULL) {
        return NULL;
    }
    {
        size_t index;
        size_t filled = 0;

        for (index = 0; index < total; index++) {
            entries[filled++] = json_child(dependencies, index);
        }
        if (filled > 1) {
            qsort(entries, filled, sizeof(*entries), compare_ids);
        }
        *out_count = filled;
    }
    return entries;
}

static int list_entries(const json_value *lock)
{
    const json_value **entries;
    size_t count;
    size_t index;

    entries = sorted_entries(lock, &count);
    if (entries == NULL) {
        (void)fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (index = 0; index < count; index++) {
        printf("%s\n", field(entries[index], "id"));
    }
    free((void *)entries);
    return 0;
}

static int fetch_list(const json_value *lock)
{
    const json_value **entries;
    size_t count;
    size_t index;

    entries = sorted_entries(lock, &count);
    if (entries == NULL) {
        (void)fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (index = 0; index < count; index++) {
        const json_value *entry = entries[index];
        const char *url;

        if (!is_downloaded(field(entry, "kind"))) {
            continue;
        }
        url = field(entry, "url");
        printf("%s\t%s\t%s\t%s\n", field(entry, "id"), field(entry, "sha256"), url,
            url_basename(url));
    }
    free((void *)entries);
    return 0;
}

int main(int argc, char **argv)
{
    const char *lock_path = NULL;
    const char *root_dir = NULL;
    const char *id = NULL;
    const char *print_name = NULL;
    enum { MODE_NONE, MODE_CHECK, MODE_LIST, MODE_FETCH, MODE_PRINT, MODE_PATCHES }
        mode = MODE_NONE;
    struct byte_buffer text = {NULL, 0, 0};
    json_value lock;
    int index;
    int status;

    for (index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (strcmp(argument, "--lock") == 0 && ++index < argc) {
            lock_path = argv[index];
        } else if (strcmp(argument, "--root") == 0 && ++index < argc) {
            root_dir = argv[index];
        } else if (strcmp(argument, "--id") == 0 && ++index < argc) {
            id = argv[index];
        } else if (strcmp(argument, "--print") == 0 && ++index < argc) {
            print_name = argv[index];
        } else if (strcmp(argument, "--check") == 0) {
            mode = MODE_CHECK;
        } else if (strcmp(argument, "--list") == 0) {
            mode = MODE_LIST;
        } else if (strcmp(argument, "--fetch-list") == 0) {
            mode = MODE_FETCH;
        } else if (strcmp(argument, "--list-patches") == 0) {
            mode = MODE_PATCHES;
        } else if (strcmp(argument, "--help") == 0) {
            usage();
            return 0;
        } else {
            (void)fprintf(stderr, "unrecognised argument: %s\n", argument);
            usage();
            return 1;
        }
    }
    if (lock_path == NULL) {
        usage();
        return 1;
    }
    if (print_name != NULL && mode == MODE_NONE && id != NULL) {
        mode = MODE_PRINT;
    }
    if (mode == MODE_NONE) {
        usage();
        return 1;
    }
    if (mode == MODE_PRINT && id == NULL) {
        (void)fprintf(stderr, "--print needs --id\n");
        return 1;
    }
    if (mode == MODE_PATCHES && id == NULL) {
        (void)fprintf(stderr, "--list-patches needs --id\n");
        return 1;
    }
    if ((mode == MODE_CHECK || mode == MODE_LIST || mode == MODE_FETCH)
        && (id != NULL || print_name != NULL)) {
        (void)fprintf(stderr,
            "--check, --list and --fetch-list describe the whole file\n");
        return 1;
    }
    if (read_file_all(lock_path, &text) != 0) {
        (void)fprintf(stderr, "%s: cannot be read\n", lock_path);
        return 1;
    }
    if (json_parse((const char *)text.data, text.len, &lock, NULL, 0) != 0) {
        (void)fprintf(stderr, "%s: is not well-formed JSON\n", lock_path);
        buffer_destroy(&text);
        return 1;
    }
    buffer_destroy(&text);

    switch (mode) {
    case MODE_CHECK:
        status = validate(&lock, root_dir, 1);
        if (status == 0) {
            printf("%s is valid\n", lock_path);
        }
        break;
    case MODE_LIST:
        status = validate(&lock, NULL, 0) != 0 ? 2 : list_entries(&lock);
        break;
    case MODE_FETCH:
        status = validate(&lock, NULL, 0) != 0 ? 2 : fetch_list(&lock);
        break;
    case MODE_PRINT:
        status = validate(&lock, NULL, 0) != 0
            ? 2 : print_field(&lock, id, print_name);
        break;
    case MODE_PATCHES:
        status = validate(&lock, NULL, 0) != 0 ? 2 : list_patches(&lock, id);
        break;
    default:
        status = 1;
        break;
    }
    json_free(&lock);
    return status;
}
