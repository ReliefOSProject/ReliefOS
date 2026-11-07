/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int document_fd(int fd, const char *name, json_value *document)
{
    struct byte_buffer bytes = {0};
    int result = kernel_tools_read_all(fd, &bytes);
    close(fd);
    char error[256];
    if (!result && !kernel_tools_c_string_document(&bytes)) { errno = EINVAL; result = -1; }
    if (!result) {
        result = json_parse((char *)bytes.data, bytes.len, document, error, sizeof(error));
        if (result) fprintf(stderr, "kernel-tools: %s: %s\n", name, error);
    }
    buffer_destroy(&bytes);
    return result;
}

int kernel_stage_document(int root_fd, const char *name, json_value *document)
{
    int fd = kernel_bundle_open_member(root_fd, name);
    return fd < 0 ? -1 : document_fd(fd, name, document);
}

int kernel_stage_document_path(const char *path, json_value *document)
{
    int fd = kernel_stage_input(path);
    return fd < 0 ? -1 : document_fd(fd, path, document);
}

int kernel_stage_identity(const json_value *document, const char **release,
                          const char **abi, const char **kernel_sha256)
{
    *release = json_text(json_member(document, "release"));
    *abi = json_text(json_member(document, "module_abi"));
    *kernel_sha256 = json_text(json_member(document, "kernel_sha256"));
    if (!*release || !kernel_tools_digest_text(*abi) || !kernel_tools_digest_text(*kernel_sha256)) goto invalid;
    size_t length = strlen(*release);
    if (length < 12 || length > 31 || strncmp(*release + length - 12, *abi, 12)) goto invalid;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)(*release)[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (i && strchr("._+-", c)))) goto invalid;
    }
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

int kernel_stage_verify(int root_fd, const json_value *files)
{
    if (!files || files->type != JSON_OBJECT) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < files->child_count; ++i) {
        const char *name = files->keys[i];
        const char *expected = json_text(json_member(&files->children[i], "sha256"));
        long size;
        if (!kernel_tools_canonical_member(name) || !kernel_tools_digest_text(expected) ||
            json_int(json_member(&files->children[i], "size"), &size) || size < 0) { errno = EINVAL; return -1; }
        int fd = kernel_bundle_open_member(root_fd, name);
        if (fd < 0) return -1;
        struct stat status; char actual[65];
        int result = fstat(fd, &status) || status.st_size != (off_t)size ||
                     kernel_tools_sha256_fd(fd, actual) || strcmp(actual, expected);
        close(fd);
        if (result) { fprintf(stderr, "kernel-tools: checksum mismatch: %s\n", name); errno = EIO; return -1; }
    }
    return 0;
}

int kernel_stage_quote(FILE *stream, const char *text)
{
    if (!text || fputc('"', stream) == EOF) return -1;
    for (const unsigned char *at = (const unsigned char *)text; *at; ++at) {
        if (*at == '"' || *at == '\\') { if (fputc('\\', stream) == EOF || fputc(*at, stream) == EOF) return -1; }
        else if (*at < 32) { if (fprintf(stream, "\\u%04x", *at) < 0) return -1; }
        else if (fputc(*at, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

int kernel_stage_json(FILE *stream, const json_value *value)
{
    if (!value) return -1;
    switch (value->type) {
    case JSON_NULL: return fputs("null", stream) == EOF ? -1 : 0;
    case JSON_FALSE: return fputs("false", stream) == EOF ? -1 : 0;
    case JSON_TRUE: return fputs("true", stream) == EOF ? -1 : 0;
    case JSON_NUMBER: return fprintf(stream, "%.17g", value->number) < 0 ? -1 : 0;
    case JSON_STRING: return kernel_stage_quote(stream, value->text);
    case JSON_ARRAY:
    case JSON_OBJECT:
        if (fputc(value->type == JSON_ARRAY ? '[' : '{', stream) == EOF) return -1;
        for (size_t i = 0; i < value->child_count; ++i) {
            if (i && fputc(',', stream) == EOF) return -1;
            if (value->type == JSON_OBJECT &&
                (kernel_stage_quote(stream, value->keys[i]) || fputc(':', stream) == EOF)) return -1;
            if (kernel_stage_json(stream, &value->children[i])) return -1;
        }
        return fputc(value->type == JSON_ARRAY ? ']' : '}', stream) == EOF ? -1 : 0;
    }
    return -1;
}

int kernel_stage_files_json(FILE *stream, const struct kernel_stage_inventory *inventory)
{
    if (fputc('{', stream) == EOF) return -1;
    for (size_t i = 0; i < inventory->count; ++i) {
        const struct kernel_stage_file *file = &inventory->files[i];
        if ((i && fputc(',', stream) == EOF) || kernel_stage_quote(stream, file->name) ||
            fprintf(stream, ":{\"sha256\":\"%s\",\"size\":%lld}", file->sha256, (long long)file->size) < 0) return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}
