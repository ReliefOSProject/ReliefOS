/*
 * reliefos-version - render the build identity header.
 *
 * The old system incremented a version-controlled counter and stamped the wall
 * clock on every invocation, which dirtied the tree and forced every kernel to
 * relink. This tool derives its
 * text from reviewed inputs only: the release version file, a fixed source
 * identifier and a timestamp supplied by
 * SOURCE_DATE_EPOCH or a commit time (plan section 7).
 */

/* vasprintf() is a GNU extension; declaring it here keeps -std=c11 honest. */
#define _GNU_SOURCE 1

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tools/host/common/buffer.h"
#include "tools/host/common/io.h"

#define TOOL_NAME "reliefos-version"
#define COPYRIGHT_HOLDER "LeonMMcoset"
#define COPYRIGHT_FIRST_YEAR 2021
#define LINE_LIMIT 256

struct options {
    const char *version_file;
    const char *output;
    const char *source_id;
    const char *epoch;
};

struct identity {
    char *kernel_name;
    char *release_version;
};

#if defined(__GNUC__) || defined(__clang__)
static int report(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
static int report(const char *format, ...)
{
    va_list arguments;

    fprintf(stderr, "%s: ", TOOL_NAME);
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    return 1;
}

static void usage(void)
{
    printf(
        "usage: %s --version-file FILE --output PATH\n"
        "\n"
        "Render the build identity header. Output is content-stable: identical\n"
        "inputs leave an existing file's mtime untouched, so nothing downstream\n"
        "rebuilds. No counter is incremented and no wall clock is read.\n"
        "\n"
        "  --version-file FILE  key=value file with kernel_name\n"
        "                       and release_version\n"
        "  --output PATH        header to publish; its directory must exist\n"
        "  --source-id ID       fixed source identifier, e.g. an abbreviated commit\n"
        "  --epoch SECONDS      Unix time for the build timestamp and copyright year\n"
        "  --help               show this message\n",
        TOOL_NAME);
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int index;

    memset(options, 0, sizeof(*options));
    for (index = 1; index < argc; index++) {
        char *argument = argv[index];

        if (strcmp(argument, "--help") == 0) {
            usage();
            exit(0);
        }
        if (index + 1 >= argc) {
            return report("'%s' requires a value", argument);
        }
        if (strcmp(argument, "--version-file") == 0) {
            options->version_file = argv[++index];
        } else if (strcmp(argument, "--output") == 0) {
            options->output = argv[++index];
        } else if (strcmp(argument, "--source-id") == 0) {
            options->source_id = argv[++index];
        } else if (strcmp(argument, "--epoch") == 0) {
            options->epoch = argv[++index];
        } else {
            return report("unrecognised argument '%s'; try --help", argument);
        }
    }
    if (options->version_file == NULL || options->output == NULL) {
        return report("--version-file and --output are both required");
    }
    return 0;
}

static char *trim(char *text)
{
    size_t length;

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    length = strlen(text);
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n' ||
            text[length - 1] == ' ' || text[length - 1] == '\t')) {
        length--;
        text[length] = '\0';
    }
    return text;
}

static void identity_free(struct identity *identity)
{
    free(identity->kernel_name);
    free(identity->release_version);
}

/* Assigns one reviewed key. A second definition is an error, and so is an
 * unknown key: a typo must not silently revert a product name. */
static int assign_identity(struct identity *identity, const char *key,
                           const char *value, const char *path)
{
    char **slot;

    if (strcmp(key, "kernel_name") == 0) {
        slot = &identity->kernel_name;
    } else if (strcmp(key, "release_version") == 0) {
        slot = &identity->release_version;
    } else {
        return report("%s: unknown key '%s'", path, key);
    }
    if (*slot != NULL) {
        return report("%s: key '%s' is given twice", path, key);
    }
    *slot = strdup(value);
    if (*slot == NULL) {
        return report("out of memory");
    }
    return 0;
}

static int load_identity(const char *path, struct identity *identity)
{
    FILE *stream = fopen(path, "r");
    char line[LINE_LIMIT];
    int status = 0;

    if (stream == NULL) {
        return report("cannot read %s: %s", path, strerror(errno));
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        char *trimmed = trim(line);
        char *separator;
        char *key;
        char *value;

        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }
        if (strlen(trimmed) >= sizeof(line) - 1 && strchr(trimmed, '=') == NULL) {
            status = report("%s: line too long or unterminated", path);
            break;
        }
        separator = strchr(trimmed, '=');
        if (separator == NULL) {
            status = report("%s: '%s' is not key=value", path, trimmed);
            break;
        }
        *separator = '\0';
        key = trim(trimmed);
        value = trim(separator + 1);
        if (*value == '\0') {
            status = report("%s: key '%s' has no value", path, key);
            break;
        }
        status = assign_identity(identity, key, value, path);
        if (status != 0) {
            break;
        }
    }
    if (ferror(stream)) {
        status = report("cannot read %s", path);
    }
    if (fclose(stream) != 0 && status == 0) {
        status = report("cannot finish reading %s: %s", path, strerror(errno));
    }
    if (status == 0 && (identity->kernel_name == NULL ||
            identity->release_version == NULL)) {
        status = report("%s must define kernel_name and release_version",
            path);
    }
    return status;
}

static int parse_decimal(const char *text, unsigned long long *out_value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out_value = parsed;
    return 0;
}

/* Splits "major.minor.patch" with no fixed-size scratch buffer. */
static int split_version(const char *text, long *major, long *minor, long *patch)
{
    const char *cursor = text;
    char *end = NULL;
    long parts[3];
    int index;

    for (index = 0; index < 3; index++) {
        errno = 0;
        parts[index] = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || parts[index] < 0) {
            return -1;
        }
        if (index < 2) {
            if (*end != '.') {
                return -1;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return -1;
        }
    }
    *major = parts[0];
    *minor = parts[1];
    *patch = parts[2];
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
static int append_line(struct byte_buffer *out, const char *format, ...) __attribute__((format(printf, 2, 3)));
#endif
static int append_line(struct byte_buffer *out, const char *format, ...)
{
    va_list arguments;
    char *line = NULL;
    int needed;

    va_start(arguments, format);
    needed = vasprintf(&line, format, arguments);
    va_end(arguments);
    if (needed < 0 || line == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if ((size_t)needed != strlen(line)) {
        free(line);
        errno = EOVERFLOW;
        return -1;
    }
    if (out->len + (size_t)needed < out->len) {
        free(line);
        errno = EOVERFLOW;
        return -1;
    }
    if (buffer_reserve(out, out->len + (size_t)needed) != 0) {
        free(line);
        return -1;
    }
    memcpy(out->data + out->len, line, (size_t)needed);
    out->len += (size_t)needed;
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    struct options options;
    struct identity identity = { NULL, NULL };
    struct byte_buffer out = { NULL, 0, 0 };
    char timestamp[32];
    char copyright[160];
    char version_string[128];
    unsigned long long epoch = 0;
    long major = 0;
    long minor = 0;
    long patch = 0;
    int year = 1970;
    int status;

    status = parse_options(argc, argv, &options);
    if (status != 0) {
        return status;
    }
    status = load_identity(options.version_file, &identity);
    if (status != 0) {
        goto cleanup;
    }
    if (split_version(identity.release_version, &major, &minor, &patch) != 0) {
        status = report("release_version must be major.minor.patch, got '%s'",
            identity.release_version);
        goto cleanup;
    }
    if (options.epoch != NULL && parse_decimal(options.epoch, &epoch) != 0) {
        status = report("--epoch must be a decimal Unix time");
        goto cleanup;
    }

    /* The timestamp comes from the caller-supplied epoch only. gmtime_r keeps
     * the result independent of the developer's TZ setting, which is what makes
     * two clean output directories byte-identical. */
    {
        time_t moment = (time_t)epoch;
        struct tm broken_down_time;

        if (gmtime_r(&moment, &broken_down_time) == NULL) {
            status = report("cannot convert the supplied epoch");
            goto cleanup;
        }
        year = broken_down_time.tm_year + 1900;
        if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                &broken_down_time) == 0) {
            status = report("cannot render the supplied epoch");
            goto cleanup;
        }
    }

    /* Source identity is independent of the public release version. */
    const char *source_id = options.source_id != NULL ? options.source_id : "unknown";
    if (strspn(source_id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(source_id)) {
        status = report("--source-id contains unsafe characters");
        goto cleanup;
    }
    if (snprintf(version_string, sizeof(version_string), "%s",
            identity.release_version) >= (int)sizeof(version_string)) {
        status = report("the derived version string does not fit");
        goto cleanup;
    }
    if (snprintf(copyright, sizeof(copyright),
            "(C) %s %d-%d. Open source as Apache 2.0 license.",
            COPYRIGHT_HOLDER, COPYRIGHT_FIRST_YEAR, year) >= (int)sizeof(copyright)) {
        status = report("the copyright line does not fit");
        goto cleanup;
    }

    status = append_line(&out, "#ifndef RELIEFOS_GENERATED_BUILD_INFO_H\n") != 0 ||
        append_line(&out, "#define RELIEFOS_GENERATED_BUILD_INFO_H\n\n") != 0 ||
        append_line(&out, "/* Generated by %s from the version inputs. Do not edit. */\n\n",
            TOOL_NAME) != 0 ||
        append_line(&out, "#define RELIEFOS_KERNEL_NAME \"%s\"\n", identity.kernel_name) != 0 ||
        append_line(&out, "#define RELIEFOS_KERNEL_VERSION_MAJOR %ld\n", major) != 0 ||
        append_line(&out, "#define RELIEFOS_KERNEL_VERSION_MINOR %ld\n", minor) != 0 ||
        append_line(&out, "#define RELIEFOS_KERNEL_VERSION_PATCH %ld\n", patch) != 0 ||
        append_line(&out, "#define RELIEFOS_SOURCE_ID \"%s\"\n", source_id) != 0 ||
        append_line(&out, "#define RELIEFOS_KERNEL_VERSION \"%s\"\n", version_string) != 0 ||
        append_line(&out, "#define RELIEFOS_BUILD_TIME \"%s\"\n", timestamp) != 0 ||
        append_line(&out, "#define RELIEFOS_COPYRIGHT_YEAR %d\n", year) != 0 ||
        append_line(&out, "#define RELIEFOS_COPYRIGHT \"%s\"\n\n", copyright) != 0 ||
        append_line(&out, "#endif\n") != 0;
    if (status != 0) {
        status = report("cannot render the version header: %s", strerror(errno));
        goto cleanup;
    }

    if (write_file_if_changed(options.output, out.data, out.len, 0644u) != 0) {
        status = report("cannot publish %s: %s", options.output, strerror(errno));
    }

cleanup:
    buffer_destroy(&out);
    identity_free(&identity);
    return status;
}
