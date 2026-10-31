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

int reliefos_account_seed(const char *target, const char *name,
                        const char *password, const char *root_password)
{
    if (!reliefos_account_name_valid(name, RELIEFOS_AUTH_USERNAME_LEN)) { errno = EINVAL; return -1; }
    if (reliefos_account_legacy_check(target) < 0) return -1;
    char *hash = password_hash(password), *root_hash = password_hash(root_password);
    char *contents[4] = {0}, *directory = NULL;
    int fd = -1, result = -1;
    if (!hash || !root_hash) goto out;
    if (asprintf(&directory, "%s/etc", target) < 0 ||
        asprintf(&contents[0], "root:x:0:0:root:/root:/bin/bash\nnobody:x:65534:65534:nobody:/:/sbin/nologin\n%s:x:1000:1000::/home/%s:/bin/bash\n", name, name) < 0 ||
        asprintf(&contents[1], "root:%s:%ld:0:99999:7:::\nnobody:!:::::::\n%s:%s:%ld:0:99999:7:::\n", root_hash, (long)(time(NULL) / 86400), name, hash, (long)(time(NULL) / 86400)) < 0 ||
        asprintf(&contents[2], "root:x:0:\nwheel:x:10:%s\nvideo:x:44:%s\nnobody:x:65534:\n%s:x:1000:\n", name, name, name) < 0 ||
        asprintf(&contents[3], "root:!::\nwheel:!::%s\nvideo:!::%s\nnobody:!::\n%s:!::\n", name, name, name) < 0) goto out;
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) goto out;
    result = reliefos_account_store_commit(fd, (const char *const *)contents);
out:;
    int error = errno;
    if (fd >= 0 && close(fd) < 0 && !result) { error = errno; result = -1; }
    for (unsigned i = 0; i < 4; ++i) if (contents[i]) { explicit_bzero(contents[i], strlen(contents[i])); free(contents[i]); }
    if (hash) { explicit_bzero(hash, strlen(hash)); free(hash); }
    if (root_hash) { explicit_bzero(root_hash, strlen(root_hash)); free(root_hash); }
    free(directory);
    errno = error;
    return result;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_account_info) leonos_account_info __attribute__((alias("reliefos_account_info")));
extern __typeof__(reliefos_account_legacy_check) leonos_account_legacy_check __attribute__((alias("reliefos_account_legacy_check")));
extern __typeof__(reliefos_account_name_valid) leonos_account_name_valid __attribute__((alias("reliefos_account_name_valid")));
extern __typeof__(reliefos_account_seed) leonos_account_seed __attribute__((alias("reliefos_account_seed")));
