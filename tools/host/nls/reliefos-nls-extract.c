/* Explicit maintenance tool: extract single-argument message markers, retain
 * PO translations, and remove literal legacy translation arguments once. */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct text { char *data; size_t len; };
struct message { char *id; const char *file; size_t line; };
struct catalog { char *id; char *block; bool used; };
static struct message *messages;
static size_t count;
static struct catalog *catalog;
static size_t catalog_count;

static void fail(const char *what)
{
    fprintf(stderr, "nls-extract: %s\n", what);
    exit(1);
}

static void *resize(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1U);
    if (!p) fail("out of memory");
    return p;
}

static void append(struct text *out, const char *s, size_t n)
{
    out->data = resize(out->data, out->len + n + 1U);
    memcpy(out->data + out->len, s, n);
    out->len += n;
    out->data[out->len] = 0;
}

static struct text read_file(const char *path)
{
    struct text out = {0};
    char buf[8192];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) fail(path);
    while ((n = fread(buf, 1, sizeof buf, f)) != 0) append(&out, buf, n);
    if (ferror(f) || fclose(f)) fail("read failed");
    if (!out.data) append(&out, "", 0);
    return out;
}

static void write_file(const char *path, const struct text *out)
{
    FILE *f = fopen(path, "wb");
    if (!f) fail(path);
    if (fwrite(out->data, 1, out->len, f) != out->len || fclose(f)) fail("write failed");
}

static size_t trivia(const char *s, size_t p)
{
    for (;;) {
        while (isspace((unsigned char)s[p])) p++;
        if (s[p] == '/' && s[p + 1] == '/') {
            while (s[p] && s[p] != '\n') p++;
        } else if (s[p] == '/' && s[p + 1] == '*') {
            p += 2;
            while (s[p] && !(s[p] == '*' && s[p + 1] == '/')) p++;
            if (s[p]) p += 2;
        } else return p;
    }
}

static size_t quoted_end(const char *s, size_t p)
{
    char quote = s[p++];
    while (s[p] && s[p] != quote) {
        if (s[p] == '\\' && s[p + 1]) p++;
        p++;
    }
    if (!s[p]) fail("unterminated literal");
    return p + 1;
}

static char *literal(const char *s, size_t *p)
{
    struct text out = {0};
    *p = trivia(s, *p);
    if (s[*p] != '"') return NULL;
    do {
        size_t end = quoted_end(s, *p);
        append(&out, s + *p + 1, end - *p - 2);
        *p = trivia(s, end);
    } while (s[*p] == '"');
    return out.data;
}

static void record(char *id, const char *file, size_t line)
{
    if (!*id) { free(id); return; }
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(messages[i].id, id)) { free(id); return; }
    }
    messages = resize(messages, (count + 1) * sizeof *messages);
    messages[count++] = (struct message){id, file, line};
}

static bool marker(const char *s, size_t n)
{
    return (n == 1 && !memcmp(s, "T", 1)) ||
           (n == 4 && !memcmp(s, "UI_T", 4)) ||
           (n == 2 && !memcmp(s, "N_", 2)) ||
           (n == 7 && !memcmp(s, "gettext", 7));
}

static void scan(const char *file, bool rewrite)
{
    struct text input = read_file(file), output = {0};
    const char *s = input.data;
    size_t p = 0, copied = 0;
    while (s[p]) {
        p = trivia(s, p);
        if (!s[p]) break;
        if (s[p] == '#') {
            do {
                while (s[p] && s[p] != '\n') p++;
                if (!p || s[p - 1] != '\\' || !s[p]) break;
                p++;
            } while (s[p]);
            continue;
        }
        if (s[p] == '"' || s[p] == '\'') { p = quoted_end(s, p); continue; }
        if (isalpha((unsigned char)s[p]) || s[p] == '_') {
            size_t start = p++;
            while (isalnum((unsigned char)s[p]) || s[p] == '_') p++;
            if (!marker(s + start, p - start)) continue;
            size_t arg = trivia(s, p);
            if (s[arg] != '(') continue;
            arg++;
            char *id = literal(s, &arg);
            if (!id) continue;
            if (s[arg] == ',' && rewrite) {
                size_t comma = arg++;
                char *translation = literal(s, &arg);
                if (translation && s[arg] == ')') {
                    append(&output, s + copied, comma - copied);
                    copied = arg;
                    p = arg;
                }
                free(translation);
            } else if (s[arg] == ')' && !rewrite) {
                size_t line = 1;
                for (size_t i = 0; i < start; i++) if (s[i] == '\n') line++;
                record(id, file, line);
                id = NULL;
                p = arg;
            }
            free(id);
        } else p++;
    }
    if (rewrite && copied) {
        append(&output, s + copied, input.len - copied);
        write_file(file, &output);
    }
    free(output.data);
    free(input.data);
}

static int compare(const void *a, const void *b)
{
    const struct message *x = a, *y = b;
    return strcmp(x->id, y->id);
}

static void load_catalog(const char *path)
{
    struct text input = read_file(path), block = {0};
    size_t p = 0;
    while (p < input.len) {
        size_t end = p;
        while (input.data[end] && input.data[end] != '\n') end++;
        const char *line = input.data + p;
        size_t n = end - p;
        if (n >= 3 && !memcmp(line, "#~ ", 3)) { line += 3; n -= 3; }
        if (n) { append(&block, line, n); append(&block, "\n", 1); }
        if ((!n || end == input.len) && block.len) {
            char *at = block.data;
            while (*at && strncmp(at, "msgid ", 6)) {
                char *next = strchr(at, '\n');
                at = next ? next + 1 : at + strlen(at);
            }
            if (*at) {
                size_t offset = (size_t)(at - block.data) + 6;
                char *id = literal(block.data, &offset);
                if (!id) fail("invalid PO msgid");
                catalog = resize(catalog, (catalog_count + 1) * sizeof *catalog);
                catalog[catalog_count++] = (struct catalog){id, block.data, false};
                block = (struct text){0};
            } else { free(block.data); block = (struct text){0}; }
        }
        p = end + (end < input.len ? 1U : 0U);
    }
    /* A file without a terminating blank line has one final block. */
    if (block.len) {
        char *at = strstr(block.data, "msgid ");
        if (!at) fail("invalid final PO entry");
        size_t offset = (size_t)(at - block.data) + 6;
        char *id = literal(block.data, &offset);
        if (!id) fail("invalid final PO msgid");
        catalog = resize(catalog, (catalog_count + 1) * sizeof *catalog);
        catalog[catalog_count++] = (struct catalog){id, block.data, false};
    }
    free(input.data);
}

static struct catalog *lookup(const char *id)
{
    for (size_t i = 0; i < catalog_count; i++)
        if (!strcmp(catalog[i].id, id)) { catalog[i].used = true; return &catalog[i]; }
    return NULL;
}

static void emit_block(FILE *f, const char *block, bool obsolete)
{
    while (*block) {
        const char *end = strchr(block, '\n');
        size_t n = end ? (size_t)(end - block) : strlen(block);
        if (strncmp(block, "#:", 2)) {
            if (obsolete && *block != '#') fputs("#~ ", f);
            if (fwrite(block, 1, n, f) != n) fail("write failed");
            fputc('\n', f);
        }
        block += n + (end ? 1U : 0U);
    }
}

int main(int argc, char **argv)
{
    bool rewrite = argc > 1 && !strcmp(argv[1], "--rewrite");
    bool merge = argc > 1 && !strcmp(argv[1], "--merge");
    int first = merge ? 4 : 2;
    if (argc <= first) fail("usage: nls-extract OUT SOURCE... | --merge OLD OUT SOURCE... | --rewrite SOURCE...");
    if (rewrite) {
        for (int i = 2; i < argc; i++) scan(argv[i], true);
        return 0;
    }
    if (merge) load_catalog(argv[2]);
    for (int i = first; i < argc; i++) scan(argv[i], false);
    qsort(messages, count, sizeof *messages, compare);
    FILE *f = fopen(argv[merge ? 3 : 1], "wb");
    if (!f) fail("cannot open output");
    struct catalog *header = lookup("");
    if (header) emit_block(f, header->block, false);
    else fputs("msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n", f);
    for (size_t i = 0; i < count; i++) {
        struct catalog *old = lookup(messages[i].id);
        fputc('\n', f);
        fprintf(f, "#: %s:%zu\n", messages[i].file, messages[i].line);
        if (old) emit_block(f, old->block, false);
        else fprintf(f, "msgid \"%s\"\nmsgstr \"\"\n", messages[i].id);
    }
    for (size_t i = 0; i < catalog_count; i++) {
        if (!catalog[i].used) {
            fputc('\n', f);
            emit_block(f, catalog[i].block, true);
        }
        free(catalog[i].id);
        free(catalog[i].block);
    }
    if (fclose(f)) fail("close failed");
    for (size_t i = 0; i < count; i++) free(messages[i].id);
    free(messages);
    free(catalog);
    return 0;
}
