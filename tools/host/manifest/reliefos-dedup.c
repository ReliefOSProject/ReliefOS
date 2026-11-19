#define _XOPEN_SOURCE 700
/* Share an identical file at the same path in separate installer trees.
 * A single installed tree must never acquire new hard-link aliases. */
#include <errno.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUCKETS 4096
struct entry {
    char *path;
    const char *relative;
    struct stat st;
    struct entry *next;
};
static struct entry *buckets[BUCKETS];
static size_t root_length;
static unsigned long linked;

static int same_bytes(const char *a, const char *b)
{
    FILE *left = fopen(a, "rb"), *right = fopen(b, "rb");
    unsigned char x[65536], y[65536];
    int result = 1;
    if (!left || !right) {
        if (left) fclose(left);
        if (right) fclose(right);
        return -1;
    }
    for (;;) {
        size_t nx = fread(x, 1, sizeof x, left);
        size_t ny = fread(y, 1, sizeof y, right);
        if (ferror(left) || ferror(right)) { result = -1; break; }
        if (nx != ny || memcmp(x, y, nx)) { result = 0; break; }
        if (!nx) break;
    }
    if (fclose(left)) result = -1;
    if (fclose(right)) result = -1;
    return result;
}

static int examine(const char *path, const struct stat *st, int type,
                   struct FTW *walk)
{
    static const char *const embedded_roots[] = {
        "install/root/",
        "install/root-console/",
        "install/root-xfce/",
    };
    (void)walk;
    if (type == FTW_NS || type == FTW_DNR) return -1;
    /* A hard link supplied by a package already has intentional aliases. */
    if (type != FTW_F || !S_ISREG(st->st_mode) || st->st_nlink != 1) return 0;
    const char *relative = path + root_length;
    if (*relative == '/') relative++;
    for (size_t i = 0; i < sizeof embedded_roots / sizeof embedded_roots[0]; i++) {
        size_t length = strlen(embedded_roots[i]);
        if (!strncmp(relative, embedded_roots[i], length)) {
            relative += length;
            break;
        }
    }
    if (strncmp(relative, "usr/", 4) && strncmp(relative, "bin/", 4) &&
        strncmp(relative, "sbin/", 5) && strncmp(relative, "opt/", 4)) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = (const unsigned char *)relative; *byte; ++byte)
        hash = (hash ^ *byte) * UINT64_C(1099511628211);
    size_t bucket = hash % BUCKETS;
    int bad;
    for (struct entry *e = buckets[bucket]; e; e = e->next) {
        if (strcmp(e->relative, relative) || e->st.st_size != st->st_size ||
            e->st.st_mode != st->st_mode || e->st.st_uid != st->st_uid ||
            e->st.st_gid != st->st_gid || e->st.st_dev != st->st_dev) continue;
        int same = same_bytes(e->path, path);
        if (same < 0) return -1;
        if (!same) continue;
        /* Create the replacement before touching the original file. */
        size_t length = strlen(path) + sizeof ".dedup.XXXXXX";
        char *temporary = malloc(length);
        if (!temporary) return -1;
        snprintf(temporary, length, "%s.dedup.XXXXXX", path);
        int fd = mkstemp(temporary);
        if (fd < 0) { free(temporary); return -1; }
        bad = close(fd);
        if (unlink(temporary)) bad = 1;
        if (!bad && link(e->path, temporary)) bad = 1;
        if (!bad && rename(temporary, path)) bad = 1;
        if (bad) unlink(temporary);
        free(temporary);
        if (bad) return -1;
        linked++;
        return 0;
    }
    struct entry *e = malloc(sizeof *e);
    if (!e) return -1;
    e->path = strdup(path);
    if (!e->path) { free(e); return -1; }
    e->st = *st;
    e->relative = e->path + (relative - path);
    e->next = buckets[bucket];
    buckets[bucket] = e;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: reliefos-dedup ROOT\n"); return 2; }
    char *root = realpath(argv[1], NULL);
    if (!root) { perror(argv[1]); return 1; }
    struct stat st;
    if (stat(root, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "dedup root must be a directory: %s\n", root);
        free(root);
        return 1;
    }
    root_length = strlen(root);
    int result = nftw(root, examine, 32, FTW_PHYS);
    if (result) fprintf(stderr, "dedup failed under %s: %s\n", root, strerror(errno));
    else printf("  DEDUP    %lu identical payload files\n", linked);
    for (size_t i = 0; i < BUCKETS; i++) {
        while (buckets[i]) {
            struct entry *e = buckets[i];
            buckets[i] = e->next;
            free(e->path);
            free(e);
        }
    }
    free(root);
    return result ? 1 : 0;
}
