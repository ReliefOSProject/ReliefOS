#define _POSIX_C_SOURCE 200809L
/* Expand explicit staging rules, reject collisions/escaping parents, and emit
 * the complete rootfs manifest. All writes target a caller-owned private tree.
 */
#include "tools/host/common/io.h"
#include "tools/host/manifest/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct entry {
    char *source, *path, *owner, *target;
    mode_t mode;
    /* Guest gid for the staged inode (plan column 7, default 0). The rootfs
     * image build re-applies it after its blanket chown (tools/build/images.sh);
     * the M1 service role gids live there and in the staged group files
     * (system/rootfs/etc/group, system/test-accounts/group). */
    unsigned gid;
    char type;
    int override;
    size_t order;
};
static struct entry *entries;
static size_t count, capacity;
#if defined(__GNUC__) || defined(__clang__)
static void die(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
static void die(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    fputs("reliefos-stage: ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
static char *copy(const char *s) {
    char *p = strdup(s);
    if (!p)
        die("allocation failed");
    return p;
}
static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na + nb > 65535)
        die("path too long");
    char *p = malloc(na + nb + 2);
    if (!p)
        die("allocation failed");
    memcpy(p, a, na);
    p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}
static void valid_guest(const char *p) {
    if (*p != '/' || !p[1])
        die("guest path must be absolute and non-root: %s", p);
    for (const char *s = p + 1; *s;) {
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        if (!n || (n == 1 && *s == '.') || (n == 2 && !memcmp(s, "..", 2)))
            die("unsafe guest path: %s", p);
        if (!end)
            return;
        s = end + 1;
        if (!*s)
            die("trailing slash: %s", p);
    }
}
static char *link_text(const char *source) {
    size_t size = 256;
    while (size <= 65536) {
        char *p = malloc(size + 1);
        if (!p)
            die("allocation failed");
        ssize_t n = readlink(source, p, size);
        if (n < 0)
            die("readlink %s: %s", source, strerror(errno));
        if ((size_t)n < size) {
            p[n] = 0;
            return p;
        }
        free(p);
        size *= 2;
    }
    die("symlink too long: %s", source);
    return NULL;
}
static void add(char type, const char *source, const char *path, mode_t mode,
                unsigned gid, const char *owner, int override,
                const char *target) {
    valid_guest(path);
    if (count == 1000000)
        die("manifest entry limit");
    if (count == capacity) {
        capacity = capacity ? capacity * 2 : 256;
        struct entry *p = realloc(entries, capacity * sizeof(*p));
        if (!p)
            die("allocation failed");
        entries = p;
    }
    entries[count] = (struct entry){
        .source = copy(source), .path = copy(path), .owner = copy(owner),
        .target = target ? copy(target) : NULL,
        .mode = mode, .gid = gid, .type = type, .override = override,
        .order = count};
    ++count;
}
static void expand(const char *source, const char *path, const char *owner,
                   int override, unsigned depth) {
    if (depth > 128)
        die("source tree nesting limit: %s", source);
    struct stat st;
    if (lstat(source, &st))
        die("stat %s: %s", source, strerror(errno));
    if (S_ISDIR(st.st_mode)) {
        if (strcmp(path, "/"))
            add('d', source, path, st.st_mode & 07777, 0, owner, override, NULL);
        struct dirent **names;
        int n = scandir(source, &names, NULL, alphasort);
        if (n < 0)
            die("scan %s: %s", source, strerror(errno));
        for (int i = 0; i < n; ++i) {
            const char *name = names[i]->d_name;
            if (strcmp(name, ".") && strcmp(name, "..")) {
                char *s = join(source, name);
                char *p = join(!strcmp(path, "/") ? "" : path, name);
                expand(s, p, owner, override, depth + 1);
                free(s);
                free(p);
            }
            free(names[i]);
        }
        free(names);
    } else if (S_ISREG(st.st_mode)) {
        add('f', source, path, st.st_mode & 07777, 0, owner, override, NULL);
    } else if (S_ISLNK(st.st_mode)) {
        char *target = link_text(source);
        add('l', source, path, 0777, 0, owner, override, target);
        free(target);
    } else
        die("unsupported source file type: %s", source);
}
static void read_plan(const char *name) {
    FILE *f = fopen(name, "r");
    if (!f)
        die("open plan: %s", strerror(errno));
    char *line = NULL;
    size_t allocated = 0;
    ssize_t length;
    while ((length = getline(&line, &allocated, f)) >= 0) {
        if (memchr(line, 0, (size_t)length))
            die("NUL in plan");
        if (length && line[length - 1] == '\n')
            line[--length] = 0;
        if (!length || line[0] == '#')
            continue;
        char *fields[7], *p = line;
        size_t nfields = 0;
        for (;;) {
            if (nfields == 7)
                die("extra plan fields");
            fields[nfields++] = p;
            char *tab = strchr(p, '\t');
            if (!tab)
                break;
            *tab = 0;
            p = tab + 1;
        }
        if (nfields < 6)
            die("plan requires six tab-separated fields");
        char type = fields[0][0];
        if (fields[0][1] || !strchr("tfdlx", type))
            die("invalid plan type");
        char *end;
        errno = 0;
        unsigned long mode = strtoul(fields[3], &end, 8);
        if (errno || !*fields[3] || *end || mode > 07777)
            die("invalid mode");
        /* Column 7 is the guest gid (decimal), optional; an absent or
         * explicitly zero column keeps the historic root-owned staging. */
        unsigned gid = 0;
        if (nfields == 7) {
            errno = 0;
            unsigned long value = strtoul(fields[6], &end, 10);
            if (errno || !*fields[6] || *end || value > 4294967295UL)
                die("invalid gid");
            gid = (unsigned)value;
        }
        int override = !strcmp(fields[5], "override");
        if (!override && strcmp(fields[5], "unique"))
            die("invalid overlay policy");
        if (type == 't')
            expand(fields[1], fields[2], fields[4], override, 0);
        else {
            if (type == 'f') {
                struct stat st;
                if (lstat(fields[1], &st) || !S_ISREG(st.st_mode))
                    die("regular source required: %s", fields[1]);
            }
            add(type, type == 'l' ? "" : fields[1], fields[2], (mode_t)mode,
                gid, fields[4], override, type == 'l' ? fields[1] : NULL);
        }
    }
    free(line);
    if (ferror(f) || fclose(f))
        die("plan read failed");
}
static int compare(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    int n = strcmp(x->path, y->path);
    return n ? n : (x->order > y->order) - (x->order < y->order);
}
static void parent_dirs(const char *path) {
    char *p = copy(path);
    for (char *s = p + 1; *s; ++s)
        if (*s == '/') {
            *s = 0;
            struct stat st;
            if (lstat(p, &st)) {
                if (errno != ENOENT || mkdir(p, 0755) || chmod(p, 0755))
                    die("mkdir %s: %s", p, strerror(errno));
            } else if (!S_ISDIR(st.st_mode))
                die("non-directory parent: %s", p);
            *s = '/';
        }
    free(p);
}
static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 32)
            fprintf(f, "\\u%04x", *p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}
static void apply(const char *root, const char *manifest) {
    struct stat st;
    if (lstat(root, &st) || !S_ISDIR(st.st_mode))
        die("root must be a real directory");
    qsort(entries, count, sizeof(*entries), compare);
    /* Validate all duplicates before creating anything. */
    for (size_t i = 1; i < count; ++i)
        if (!strcmp(entries[i - 1].path, entries[i].path) &&
            !entries[i].override &&
            !(entries[i - 1].type == 'd' && entries[i].type == 'd'))
            die("duplicate destination %s (%s and %s)", entries[i].path,
                entries[i - 1].owner, entries[i].owner);
    char *json = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&json, &size);
    if (!f)
        die("manifest allocation failed");
    fputs("{\"schema_version\":1,\"entries\":[", f);
    int comma = 0;
    for (size_t i = 0; i < count; ++i) {
        struct entry *e = &entries[i];
        if (i + 1 < count && !strcmp(e->path, entries[i + 1].path))
            continue;
        if (e->type == 'x')
            continue;
        char *dest = join(root, e->path + 1);
        parent_dirs(dest);
        if (e->type == 'd') {
            if (mkdir(dest, 0755) && errno != EEXIST)
                die("mkdir %s", dest);
            if (lstat(dest, &st) || !S_ISDIR(st.st_mode))
                die("directory collision %s", dest);
        } else {
            if (!lstat(dest, &st))
                die("existing destination %s", dest);
            if (errno != ENOENT)
                die("stat destination %s", dest);
            if (e->type == 'l') {
                if (symlink(e->target, dest))
                    die("symlink %s", dest);
            } else {
                int in = open(e->source, O_RDONLY | O_NOFOLLOW);
                if (in < 0)
                    die("open %s", e->source);
                int out = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (out < 0)
                    die("create %s", dest);
                char data[65536];
                ssize_t n;
                while ((n = read(in, data, sizeof data)) != 0) {
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        die("read %s", e->source);
                    }
                    ssize_t done = 0;
                    while (done < n) {
                        ssize_t wrote =
                            write(out, data + done, (size_t)(n - done));
                        if (wrote < 0 && errno == EINTR)
                            continue;
                        if (wrote <= 0)
                            die("write %s", dest);
                        done += wrote;
                    }
                }
                if (close(in) || close(out))
                    die("close payload %s", dest);
            }
        }
        if (e->type != 'l' && chmod(dest, e->mode))
            die("chmod %s", dest);
        if (comma++)
            fputc(',', f);
        fputs("\n{\"type\":", f);
        json_string(f, e->type == 'd'   ? "dir"
                       : e->type == 'l' ? "symlink"
                                        : "file");
        fputs(",\"source\":", f);
        json_string(f, e->source);
        fputs(",\"path\":", f);
        json_string(f, e->path);
        fprintf(f, ",\"mode\":\"%04o\",\"uid\":0,\"gid\":%u,\"owner\":",
                (unsigned)e->mode, e->gid);
        json_string(f, e->owner);
        if (e->target) {
            fputs(",\"target\":", f);
            json_string(f, e->target);
        }
        fputc('}', f);
        free(dest);
    }
    fputs("\n]}\n", f);
    if (ferror(f) || fclose(f))
        die("manifest generation failed");
    if (write_file_if_changed(manifest, json, size, 0644))
        die("manifest publication failed");
    free(json);
}
static int verify(const char *manifest, const char *root) {
    struct byte_buffer input = {0};
    json_value document = {0};
    char error[256];
    int status = 1;
    if (read_file_all(manifest, &input) ||
        json_parse((char *)input.data, input.len, &document, error,
                   sizeof error))
        goto done;
    const json_value *items = json_member(&document, "entries");
    if (!items || items->type != JSON_ARRAY)
        goto done;
    for (size_t i = 0; i < items->child_count; ++i) {
        const json_value *item = &items->children[i];
        const char *path = json_text(json_member(item, "path"));
        const char *type = json_text(json_member(item, "type"));
        const char *mode = json_text(json_member(item, "mode"));
        if (!path || !type || !mode)
            goto done;
        char *end;
        unsigned long expected_mode = strtoul(mode, &end, 8);
        if (*end || expected_mode > 07777)
            goto done;
        valid_guest(path);
        char *full = join(root, path + 1);
        struct stat st;
        int exists = lstat(full, &st) == 0;
        if (!exists || (!strcmp(type, "file") && !S_ISREG(st.st_mode)) ||
            (!strcmp(type, "dir") && !S_ISDIR(st.st_mode)) ||
            (!strcmp(type, "symlink") && !S_ISLNK(st.st_mode)) ||
            (!S_ISLNK(st.st_mode) && (st.st_mode & 07777) != expected_mode)) {
            free(full);
            goto done;
        }
        if (S_ISLNK(st.st_mode)) {
            const char *target = json_text(json_member(item, "target"));
            char *actual = link_text(full);
            int match = target && !strcmp(target, actual);
            free(actual);
            if (!match) {
                free(full);
                goto done;
            }
        }
        free(full);
    }
    status = 0;
done:
    json_free(&document);
    buffer_destroy(&input);
    return status;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--check"))
        return verify(argv[2], argv[3]);
    if (argc != 4) {
        fprintf(stderr, "usage: reliefos-stage PLAN EMPTY_ROOT MANIFEST\n");
        return 2;
    }
    read_plan(argv[1]);
    apply(argv[2], argv[3]);
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].source);
        free(entries[i].path);
        free(entries[i].owner);
        free(entries[i].target);
    }
    free(entries);
    return 0;
}
