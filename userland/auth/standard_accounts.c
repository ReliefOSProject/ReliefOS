#define _GNU_SOURCE
#include "standard_accounts.h"
#include "account_store.h"
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int reliefos_account_name_valid(const char *name, unsigned capacity)
{
    if (!name || !*name || strnlen(name, capacity) == capacity ||
        !strcmp(name, ".") || !strcmp(name, "..") || name[0] == '-') return 0;
    if (!strcmp(name, "root") || !strcmp(name, "nobody") ||
        !strcmp(name, "wheel") || !strcmp(name, "video")) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p <= ' ' || *p == 127 || *p == ':' || *p == ',' || *p == '/' || *p == '\\') return 0;
    return 1;
}

int reliefos_account_info(const struct passwd *account, struct reliefos_user_info *out)
{
    if (!account || !out) { errno = ENOENT; return -1; }
    if (strlen(account->pw_name) >= sizeof(out->username) ||
        strlen(account->pw_dir) >= sizeof(out->home)) { errno = EOVERFLOW; return -1; }
    *out = (struct reliefos_user_info){.uid = account->pw_uid,
        .role = account->pw_uid ? RELIEFOS_AUTH_ROLE_USER : RELIEFOS_AUTH_ROLE_ADMIN};
    strcpy(out->username, account->pw_name);
    strcpy(out->home, account->pw_dir);
    if (geteuid() == 0) {
        errno = 0;
        struct spwd *shadow = getspnam(account->pw_name);
        if (!shadow) { if (!errno) errno = ENODATA; return -1; }
        time_t now = time(NULL);
        if (now == (time_t)-1) return -1;
        if (shadow->sp_pwdp[0] == '!' || shadow->sp_pwdp[0] == '*' ||
            (shadow->sp_expire >= 0 && shadow->sp_expire <= now / 86400))
            out->flags |= RELIEFOS_AUTH_USER_DISABLED;
    }
    return 0;
}

int reliefos_account_legacy_check(const char *target)
{
    char *path = NULL;
    if (asprintf(&path, "%s/var/lib/reliefos/accounts.db", target) < 0) return -1;
    int old = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    free(path); path = NULL;
    if (old >= 0) {
        struct stat st;
        int result = fstat(old, &st), error = errno;
        close(old);
        if (result < 0) { errno = error; return -1; }
        if (!S_ISREG(st.st_mode) || st.st_size) { errno = ENOTSUP; return -1; }
    } else if (errno != ENOENT) return -1;
    if (asprintf(&path, "%s/var/lib/reliefos/users.db", target) < 0) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    free(path);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { int error = errno; close(fd); errno = error; return -1; }
    if (!S_ISREG(st.st_mode) || st.st_size != 8) { close(fd); errno = ENOTSUP; return -1; }
    uint32_t header[2];
    ssize_t size = read(fd, header, sizeof(header));
    int error = errno;
    close(fd);
    if (size < 0) { errno = error; return -1; }
    if (size != sizeof(header) || header[0] != 0x41555332u || header[1]) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static char *password_hash(const char *password)
{
    if (!reliefos_auth_password_valid(password, RELIEFOS_AUTH_PASSWORD_LEN)) { errno = EINVAL; return NULL; }
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data *data = calloc(1, sizeof(*data));
    if (!data) return NULL;
    char *result = NULL;
    if (crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt))) {
        char *hash = crypt_rn(password, salt, data, sizeof(*data));
        if (hash && hash[0] != '*') result = strdup(hash);
    }
    int error = errno;
    explicit_bzero(data, sizeof(*data));
    explicit_bzero(salt, sizeof(salt));
    free(data);
    errno = error;
    return result;
}

enum seed_file { SEED_PASSWD, SEED_SHADOW, SEED_GROUP, SEED_GSHADOW, SEED_FILES };
static const char *const account_files[SEED_FILES] = {"passwd", "shadow", "group", "gshadow"};
static const struct {
    const char *name;
    unsigned id;
    const char *row[SEED_FILES];
} seed_defaults[] = {
    {"root", 0, {"root:x:0:0:root:/root:/bin/bash", NULL, "root:x:0:", "root:!::"}},
    {"nobody", 65534, {"nobody:x:65534:65534:nobody:/:/sbin/nologin", "nobody:!:::::::",
                       "nobody:x:65534:", "nobody:!::"}},
    {"wheel", 10, {NULL, NULL, "wheel:x:10:", "wheel:!::"}},
    {"video", 44, {NULL, NULL, "video:x:44:", "video:!::"}},
};

static int has_group_member(const char *members, const char *name)
{
    size_t length = strlen(name);
    while (*members) {
        size_t size = strcspn(members, ",");
        if (size == length && !memcmp(members, name, size)) return 1;
        members += size;
        if (*members) ++members;
    }
    return 0;
}

static int write_root_login(FILE *output, enum seed_file kind, const char *hash, long day)
{
    if (kind == SEED_SHADOW)
        return fprintf(output, "root:%s:%ld:0:99999:7:::\n", hash, day);
    return fprintf(output, "%s\n", seed_defaults[0].row[SEED_PASSWD]);
}

/* Preserve packaged entries verbatim, including shadow and gshadow records
 * that musl does not expose through one common parsing API. Read only the
 * fields needed to detect collisions or extend wheel/video membership. */
static int merge_seed_file(int directory, enum seed_file kind, const char *name,
                           const char *root_hash, const char *user_hash, long day, FILE *output)
{
    FILE *input = NULL;
    char *line = NULL;
    size_t capacity = 0, name_size = strlen(name);
    unsigned groups = kind >= SEED_GROUP;
    unsigned numeric = kind == SEED_PASSWD || kind == SEED_GROUP;
    unsigned entries = groups ? 4 : 2;
    unsigned seen = 0, other_ids = 0;
    int result = -1;
    int fd = openat(directory, account_files[kind], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT) return -1;
    if (fd >= 0) {
        struct stat info;
        if (fstat(fd, &info) < 0) goto out;
        if (!S_ISREG(info.st_mode) || info.st_uid || (info.st_mode & 0022)) {
            errno = EACCES;
            goto out;
        }
        input = fdopen(fd, "r");
        if (!input) goto out;
        fd = -1;
    }
    ssize_t size;
    while (input && (size = getline(&line, &capacity, input)) >= 0) {
        if (memchr(line, '\0', (size_t)size)) { errno = EINVAL; goto out; }
        if (size && line[size - 1] == '\n') line[size - 1] = '\0';
        const char *first = strchr(line, ':');
        unsigned entry = entries;
        const char *members = NULL;
        if (*line && *line != '#') {
            if (!first || first == line) { errno = EINVAL; goto out; }
            size_t length = (size_t)(first - line);
            if (length == name_size && !memcmp(line, name, length)) { errno = EEXIST; goto out; }
            for (entry = 0; entry < entries; ++entry)
                if (strlen(seed_defaults[entry].name) == length &&
                    !memcmp(line, seed_defaults[entry].name, length)) break;
            if (entry < entries) {
                if (seen & (1u << entry)) { errno = EINVAL; goto out; }
                seen |= 1u << entry;
            }
            if (numeric || (groups && entry >= 2 && entry < entries)) {
                const char *second = strchr(first + 1, ':');
                const char *third = second ? strchr(second + 1, ':') : NULL;
                if (!third) { errno = EINVAL; goto out; }
                if (groups) members = third + 1;
                if (numeric) {
                    char *end;
                    errno = 0;
                    unsigned long id = strtoul(second + 1, &end, 10);
                    if (second[1] < '0' || second[1] > '9' || end != third || errno || id >= UINT32_MAX) {
                        errno = EINVAL;
                        goto out;
                    }
                    if (id == 1000 || (!id && entry != 0)) { errno = EEXIST; goto out; }
                    if (entry == 0 && id != 0) { errno = EINVAL; goto out; }
                    for (unsigned i = 1; i < entries; ++i)
                        if (id == seed_defaults[i].id && entry != i) other_ids |= 1u << i;
                }
            }
        }
        /* Replace the root login and its password; leave root's groups intact. */
        if (!output) continue;
        if (!groups && entry == 0) {
            if (write_root_login(output, kind, root_hash, day) < 0) goto out;
        } else if (groups && entry >= 2 && entry < entries && !has_group_member(members, name)) {
            if (fprintf(output, "%s%s%s\n", line, *members ? "," : "", name) < 0) goto out;
        } else if (fprintf(output, "%s\n", line) < 0) goto out;
    }
    if (input && !feof(input)) goto out;
    if (other_ids & ~seen) { errno = EEXIST; goto out; }
    if (output) {
        for (unsigned i = 0; i < entries; ++i) {
            if (seen & (1u << i)) continue;
            if (!i && !groups) {
                if (write_root_login(output, kind, root_hash, day) < 0) goto out;
            } else if (fprintf(output, "%s%s\n", seed_defaults[i].row[kind], i >= 2 ? name : "") < 0) goto out;
        }
        switch (kind) {
        case SEED_PASSWD:
            result = fprintf(output, "%s:x:1000:1000::/home/%s:/bin/bash\n", name, name);
            break;
        case SEED_SHADOW:
            result = fprintf(output, "%s:%s:%ld:0:99999:7:::\n", name, user_hash, day);
            break;
        case SEED_GROUP: result = fprintf(output, "%s:x:1000:\n", name); break;
        case SEED_GSHADOW: result = fprintf(output, "%s:!::\n", name); break;
        default: errno = EINVAL; break;
        }
        if (result < 0) goto out;
    }
    result = 0;
out:;
    int error = errno;
    if (input && fclose(input) < 0 && !result) { error = errno; result = -1; }
    if (fd >= 0) close(fd);
    if (line) { explicit_bzero(line, capacity); free(line); }
    errno = error;
    return result;
}

static int open_account_etc(const char *target)
{
    char *directory = NULL;
    if (asprintf(&directory, "%s/etc", target) < 0) return -1;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int error = errno;
    free(directory);
    errno = error;
    return fd;
}

int reliefos_account_seed_check(const char *target, const char *name)
{
    if (!reliefos_account_name_valid(name, RELIEFOS_AUTH_USERNAME_LEN)) { errno = EINVAL; return -1; }
    int fd = open_account_etc(target);
    if (fd < 0) return -1;
    int result = 0;
    for (enum seed_file kind = 0; kind < SEED_FILES && !result; ++kind)
        result = merge_seed_file(fd, kind, name, NULL, NULL, 0, NULL);
    int error = errno;
    if (close(fd) < 0 && !result) return -1;
    errno = error;
    return result;
}

int reliefos_account_seed(const char *target, const char *name,
                        const char *password, const char *root_password)
{
    if (!reliefos_account_name_valid(name, RELIEFOS_AUTH_USERNAME_LEN)) { errno = EINVAL; return -1; }
    if (reliefos_account_legacy_check(target) < 0) return -1;
    int fd = open_account_etc(target), result = -1;
    char *contents[SEED_FILES] = {0}, *hash = NULL, *root_hash = NULL;
    if (fd < 0 || reliefos_account_store_recover(fd) < 0) goto out;
    time_t now = time(NULL);
    if (now == (time_t)-1 || !(hash = password_hash(password)) ||
        !(root_hash = password_hash(root_password))) goto out;
    /* The target is a freshly copied, exclusively owned installation root. */
    for (enum seed_file kind = 0; kind < SEED_FILES; ++kind) {
        size_t size;
        FILE *output = open_memstream(&contents[kind], &size);
        if (!output) goto out;
        int merged = merge_seed_file(fd, kind, name, root_hash, hash, (long)(now / 86400), output);
        int error = errno;
        if (fclose(output) < 0 && !merged) goto out;
        if (merged < 0) { errno = error; goto out; }
    }
    result = reliefos_account_store_commit(fd, (const char *const *)contents);
out:;
    int error = errno;
    if (fd >= 0 && close(fd) < 0 && !result) { error = errno; result = -1; }
    for (unsigned i = 0; i < SEED_FILES; ++i) if (contents[i]) { explicit_bzero(contents[i], strlen(contents[i])); free(contents[i]); }
    if (hash) { explicit_bzero(hash, strlen(hash)); free(hash); }
    if (root_hash) { explicit_bzero(root_hash, strlen(root_hash)); free(root_hash); }
    errno = error;
    return result;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_account_info) leonos_account_info __attribute__((alias("reliefos_account_info")));
extern __typeof__(reliefos_account_legacy_check) leonos_account_legacy_check __attribute__((alias("reliefos_account_legacy_check")));
extern __typeof__(reliefos_account_name_valid) leonos_account_name_valid __attribute__((alias("reliefos_account_name_valid")));
extern __typeof__(reliefos_account_seed) leonos_account_seed __attribute__((alias("reliefos_account_seed")));
