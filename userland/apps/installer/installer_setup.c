#include "installer_setup.h"
#include "../../auth/standard_accounts.h"
#include <reliefos/launch.h>
#include <reliefos/layout.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int installer_setup_valid(const struct installer_setup *setup)
{
    if (!reliefos_account_name_valid(setup->username, sizeof(setup->username)) ||
        !strcmp(setup->username, "root") || !strcmp(setup->username, "nobody") ||
        !strcmp(setup->username, "wheel")) return 0;
    const char *passwords[] = {setup->password, setup->password_confirm,
                               setup->root_password, setup->root_password_confirm};
    for (unsigned i = 0; i < 4; ++i)
        if (!reliefos_auth_password_valid(passwords[i], RELIEFOS_AUTH_PASSWORD_LEN)) return 0;
    return !strcmp(setup->password, setup->password_confirm) &&
           !strcmp(setup->root_password, setup->root_password_confirm);
}

/* Fastfetch is optional. Seed its HyFetch defaults without replacing user choices
 * or following links out of the destination home. */
static int prepare_hyfetch_config(const char *target, const struct reliefos_user_info *user)
{
    char path[512], buffer[4096];
    int n = snprintf(path, sizeof(path), "%s/etc/skel/.config/hyfetch.json", target);
    if (n < 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    int source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) return errno == ENOENT ? 0 : -1;
    int home = -1, directory = -1, output = -1, result = -1, created = 0;
    struct stat st;
    if (fstat(source, &st) < 0) goto out;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto out; }
    n = snprintf(path, sizeof(path), "%s%s", target, user->home);
    if (n < 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; goto out; }
    home = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (home < 0) goto out;
    if (mkdirat(home, ".config", 0700) < 0 && errno != EEXIST) goto out;
    directory = openat(home, ".config", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0 || fchown(directory, user->uid, user->uid) < 0) goto out;
    output = openat(directory, "hyfetch.json", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        if (errno == EEXIST) result = 0;
        goto out;
    }
    created = 1;
    for (;;) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto out;
        if (!count) break;
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(output, buffer + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { if (!written) errno = EIO; goto out; }
            offset += written;
        }
    }
    if (fchown(output, user->uid, user->uid) < 0 || fchmod(output, 0600) < 0 || fsync(output) < 0)
        goto out;
    result = 0;
out:;
    int error = errno;
    if (output >= 0 && close(output) < 0 && !result) { result = -1; error = errno; }
    if (created && result < 0) unlinkat(directory, "hyfetch.json", 0);
    if (directory >= 0) close(directory);
    if (home >= 0) close(home);
    close(source);
    errno = error;
    return result;
}

static int prepare_shell_config(const char *target, const struct reliefos_user_info *user,
                                const char *name)
{
    char path[512], buffer[4096];
    int length = snprintf(path, sizeof(path), "%s/etc/skel/%s", target, name);
    if (length < 0 || (size_t)length >= sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    int source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) return -1;
    int home = -1, output = -1, result = -1, created = 0;
    struct stat info;
    if (fstat(source, &info) < 0) goto out;
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; goto out; }
    length = snprintf(path, sizeof(path), "%s%s", target, user->home);
    if (length < 0 || (size_t)length >= sizeof(path)) { errno = ENAMETOOLONG; goto out; }
    home = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (home < 0) goto out;
    output = openat(home, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        if (errno == EEXIST) result = 0;
        goto out;
    }
    created = 1;
    for (;;) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto out;
        if (!count) break;
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(output, buffer + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { if (!written) errno = EIO; goto out; }
            offset += written;
        }
    }
    if (fchown(output, user->uid, user->uid) < 0 || fsync(output) < 0) goto out;
    result = 0;
out:;
    int error = errno;
    if (output >= 0 && close(output) < 0 && !result) { result = -1; error = errno; }
    if (created && result < 0) unlinkat(home, name, 0);
    if (home >= 0) close(home);
    close(source);
    errno = error;
    return result;
}

static int prepare_home(const char *target, const struct reliefos_user_info *user,
                        int skip_xorg)
{
    const char *const directories[] = {"", "/desktop", "/documents", "/downloads"};
    char path[512];
    for (unsigned i = 0; i < 4; ++i) {
        int n = snprintf(path, sizeof(path), "%s%s%s", target, user->home, directories[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; return -1; }
        if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
        struct stat st;
        if (lstat(path, &st) < 0) return -1;
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
        if (chown(path, user->uid, user->uid) < 0 || chmod(path, 0700) < 0) return -1;
    }
    const char *const applications[] = {"fileman", "terminal", "settings", "run", "taskmgr"};
    char desktop[512], executable[256];
    snprintf(desktop, sizeof(desktop), "%s%s/desktop", target, user->home);
    for (unsigned i = 0; !skip_xorg && i < sizeof(applications) / sizeof(applications[0]); ++i) {
        snprintf(executable, sizeof(executable), RELIEFOS_LAYOUT_RELIEFOS_APPS "/%s/%s.elf",
                 applications[i], applications[i]);
        if (reliefos_launch_create_shortcut_in_dir(desktop, executable, path, sizeof(path)) < 0 ||
            chown(path, user->uid, user->uid) < 0 || chmod(path, 0600) < 0) return -1;
    }
    const char *const shell_configs[] = {".bash_profile", ".bashrc", ".inputrc"};
    for (unsigned index = 0; index < sizeof(shell_configs) / sizeof(shell_configs[0]); ++index)
        if (prepare_shell_config(target, user, shell_configs[index]) < 0) return -1;
    return prepare_hyfetch_config(target, user);
}

int installer_setup_write(const struct installer_setup *setup, const char *target)
{
    struct reliefos_user_info records[2] = {
        {.uid = 0, .role = 2, .username = "root", .home = "/root"},
        {.uid = 1000, .role = 1},
    };
    char path[512];
    int result = -1;
    if (!installer_setup_valid(setup)) { errno = EINVAL; return -1; }
    if (strlen(target) > 256) { errno = ENAMETOOLONG; return -1; }
    strcpy(records[1].username, setup->username);
    snprintf(records[1].home, sizeof(records[1].home), "/home/%s", setup->username);
    if (reliefos_account_legacy_check(target) < 0) goto out;
    for (unsigned i = 0; i < 2; ++i)
        if (prepare_home(target, &records[i], setup->skip_xorg) < 0) goto out;
    if (reliefos_account_seed(target, setup->username, setup->password,
                            setup->root_password) < 0) goto out;
    snprintf(path, sizeof(path), "%s/etc/reliefos/installed", target);
    FILE *file = fopen(path, "w");
    if (!file) goto out;
    int failed = fputs("installed=1\n", file) == EOF;
    if (fflush(file) || fchmod(fileno(file), 0644) || fsync(fileno(file))) failed = 1;
    if (fclose(file)) failed = 1;
    if (!failed) result = 0;
out:
    explicit_bzero(records, sizeof(records));
    return result;
}
