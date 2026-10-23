#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <reliefos/pam_session.h>
#include <security/pam_misc.h>
#include <reliefos/rootfs.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../../auth/account_store.h"
#include "../../auth/standard_accounts.h"

#define SESSION "/run/reliefos/session-user"
#define STATE "/run/reliefos/session-state"
struct session_state {
    uint32_t version, uid, mask, resources;
    uint32_t gid, group_count;
    struct rlimit limits[RLIM_NLIMITS];
};
struct conversation { char *secret; int cancelled; };
static struct conversation login_conversation;
static pam_handle_t *login_pam;
static int session_lock = -1;
static int session_marker = -1;
static struct stat session_identity;

static int full_io(int fd, void *buffer, size_t size, int writing)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = writing ? write(fd, (char *)buffer + done, size - done) :
                              read(fd, (char *)buffer + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int trusted_open(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0022)) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

int reliefos_session_current(struct reliefos_user_info *user)
{
    int fd = trusted_open(SESSION);
    if (fd < 0) return -1;
    /* The private marker's OFD lock identifies this exact live session. */
    struct stat marker;
    if (fstat(fd, &marker) < 0 || (marker.st_mode & 0077)) { close(fd); errno = EACCES; return -1; }
    int locked = flock(fd, LOCK_EX | LOCK_NB), lock_error = errno;
    if (locked == 0) { close(fd); errno = ENOENT; return -1; }
    if (lock_error != EWOULDBLOCK && lock_error != EAGAIN) { close(fd); errno = lock_error; return -1; }
    char text[16] = {0};
    ssize_t size = read(fd, text, sizeof(text) - 1);
    close(fd);
    char *end;
    errno = 0;
    unsigned long uid = strtoul(text, &end, 10);
    if (size <= 0 || errno || end == text || *end != '\n' || uid >= UINT32_MAX) { errno = EIO; return -1; }
    if (reliefos_account_info(getpwuid((uid_t)uid), user) < 0) return -1;
    if (user->flags & RELIEFOS_AUTH_USER_DISABLED) { errno = EACCES; return -1; }
    return 0;
}

static int session_mutex(void)
{
    int fd = open("/run/reliefos/.session-lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid || !S_ISREG(st.st_mode) || st.st_mode & 0077) {
        close(fd); errno = EACCES; return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int error = errno; close(fd); errno = error; return -1; }
    return fd;
}

int reliefos_session_initialize(void)
{
    if (geteuid() != 0) { errno = EPERM; return -1; }
    if (reliefos_account_legacy_check("") < 0) {
        fputs("Legacy AUS2 accounts require controlled recovery; refusing to reset them.\n", stderr);
        return -1;
    }
    int fd = open("/etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = reliefos_account_store_recover(fd), error = errno;
    close(fd);
    if (result < 0) { errno = error; return -1; }
    fd = session_mutex();
    if (fd < 0) return errno == EWOULDBLOCK || errno == EAGAIN ? 0 : -1;
    result = unlink(SESSION) < 0 && errno != ENOENT ? -1 : 0;
    if (!result && unlink(STATE) < 0 && errno != ENOENT) result = -1;
    error = errno;
    close(fd); errno = error;
    return result;
}

int reliefos_auth_logout(void)
{
    if (geteuid() != 0) { errno = EPERM; return -1; }
    return unlink(SESSION) == 0 || errno == ENOENT ? 0 : -1;
}

static int conversation(int count, const struct pam_message **messages,
                         struct pam_response **out, void *opaque)
{
    if (!out) return PAM_CONV_ERR;
    *out = NULL;
    if (count <= 0 || count > PAM_MAX_NUM_MSG || !messages || !opaque) return PAM_CONV_ERR;
    struct conversation *context = opaque;
    struct pam_response *responses = calloc((size_t)count, sizeof(*responses));
    if (!responses) return PAM_BUF_ERR;
    for (int i = 0; i < count; ++i) {
        if (!messages[i] || !messages[i]->msg) goto failed;
        int style = messages[i]->msg_style;
        if (style == PAM_PROMPT_ECHO_OFF) {
            if (context->secret) {
                responses[i].resp = strdup(context->secret);
                explicit_bzero(context->secret, strlen(context->secret));
                free(context->secret); context->secret = NULL;
            } else {
                struct pam_response *response = NULL;
                const struct pam_message *message = messages[i];
                int result = misc_conv(1, &message, &response, NULL);
                if (result != PAM_SUCCESS || !response) {
                    context->cancelled = result == PAM_CONV_ERR;
                    goto failed;
                }
                responses[i] = response[0];
                free(response);
            }
        } else if (style == PAM_PROMPT_ECHO_ON || style == PAM_ERROR_MSG ||
                   style == PAM_TEXT_INFO) {
            struct pam_response *response = NULL;
            const struct pam_message *message = messages[i];
            int result = misc_conv(1, &message, &response, NULL);
            if (result != PAM_SUCCESS || !response) {
                context->cancelled = result == PAM_CONV_ERR;
                goto failed;
            }
            responses[i] = response[0];
            free(response);
        } else goto failed;
        if ((style == PAM_PROMPT_ECHO_ON || style == PAM_PROMPT_ECHO_OFF) && !responses[i].resp) goto failed;
    }
    *out = responses;
    return PAM_SUCCESS;
failed:
    for (int i = 0; i < count; ++i) if (responses[i].resp) {
        explicit_bzero(responses[i].resp, strlen(responses[i].resp)); free(responses[i].resp);
    }
    free(responses);
    return PAM_CONV_ERR;
}

static int publish(pam_handle_t *pam, const struct reliefos_user_info *user)
{
    char temporary[] = "/run/reliefos/.session-state.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    struct session_state state = {.version = 2, .uid = user->uid};
    state.mask = umask(0); umask(state.mask);
    int result = -1;
    char **environment = NULL;
    gid_t *groups = NULL;
    struct passwd *account = getpwuid(user->uid);
    if (!account) goto out;
    state.gid = account->pw_gid;
    int count = getgroups(0, NULL);
    if (count < 0) goto out;
    groups = calloc(count ? (size_t)count : 1, sizeof(*groups));
    if (!groups || getgroups(count, groups) != count) goto out;
    state.group_count = (uint32_t)count;
    if (fchown(fd, 0, 0) < 0 || fchmod(fd, 0600) < 0) goto out;
    for (unsigned i = 0; i < RLIM_NLIMITS; ++i) {
        if (getrlimit(i, &state.limits[i]) == 0) state.resources |= 1u << i;
        else if (errno != EINVAL && errno != ENOSYS) goto out;
    }
    if (full_io(fd, &state, sizeof(state), 1) < 0) goto out;
    if (full_io(fd, groups, (size_t)count * sizeof(*groups), 1) < 0) goto out;
    environment = pam_getenvlist(pam);
    if (!environment) goto out;
    for (size_t i = 0; environment[i]; ++i) {
        size_t size = strlen(environment[i]) + 1;
        if (size > UINT32_MAX) { errno = EOVERFLOW; goto out; }
        uint32_t length = (uint32_t)size;
        if (full_io(fd, &length, sizeof(length), 1) < 0 || full_io(fd, environment[i], size, 1) < 0) goto out;
    }
    uint32_t end = 0;
    if (full_io(fd, &end, sizeof(end), 1) < 0 || fsync(fd) < 0 || rename(temporary, STATE) < 0) goto out;
    char identity[] = "/run/reliefos/.session-user.XXXXXX";
    int marker = mkstemp(identity);
    if (marker < 0) goto out;
    char *text = NULL;
    int length = asprintf(&text, "%u\n%u\n%s\n%s\n", user->uid, user->role, user->username, user->home);
    int error = length < 0 || fchown(marker, 0, 0) < 0 || fchmod(marker, 0600) < 0 ||
                fcntl(marker, F_SETFD, FD_CLOEXEC) < 0 || flock(marker, LOCK_EX | LOCK_NB) < 0 ||
                full_io(marker, text, length, 1) < 0 || fsync(marker) < 0;
    free(text);
    if (!error && rename(identity, SESSION) == 0) {
        session_marker = marker;
        result = 0;
    } else close(marker);
    unlink(identity);
out:;
    int saved = errno;
    free(groups);
    if (environment) { for (size_t i = 0; environment[i]; ++i) free(environment[i]); free(environment); }
    close(fd); unlink(temporary); errno = saved;
    return result;
}

int reliefos_session_apply(void)
{
    struct reliefos_user_info user;
    if (reliefos_session_current(&user) < 0) return -1;
    int fd = trusted_open(STATE);
    if (fd < 0) return -1;
    struct session_state state;
    int result = -1;
    gid_t *groups = NULL;
    if (full_io(fd, &state, sizeof(state), 0) < 0 || state.version != 2 ||
        state.uid != user.uid || state.resources & ~((1u << RLIM_NLIMITS) - 1) || state.mask > 0777) goto out;
    struct stat file;
    if (fstat(fd, &file) < 0 || (uint64_t)state.group_count * sizeof(*groups) >
        (uint64_t)file.st_size - sizeof(state)) goto out;
    groups = calloc(state.group_count ? state.group_count : 1, sizeof(*groups));
    if (!groups || full_io(fd, groups, (size_t)state.group_count * sizeof(*groups), 0) < 0) goto out;
    for (unsigned i = 0; i < RLIM_NLIMITS; ++i)
        if ((state.resources & (1u << i)) && setrlimit(i, &state.limits[i]) < 0) goto out;
    umask(state.mask);
    if (clearenv() < 0) goto out;
    for (;;) {
        uint32_t length;
        if (full_io(fd, &length, sizeof(length), 0) < 0) goto out;
        if (!length) break;
        struct stat st;
        off_t position = lseek(fd, 0, SEEK_CUR);
        if (position < 0 || fstat(fd, &st) < 0 || length > st.st_size - position) goto out;
        char *entry = malloc(length);
        if (!entry) goto out;
        if (full_io(fd, entry, length, 0) < 0 || entry[length - 1] || !strchr(entry, '=')) { free(entry); goto out; }
        if (putenv(entry) < 0) { free(entry); goto out; }
    }
    struct passwd *account = getpwuid(user.uid);
    if (!account) goto out;
    if (setenv("HOME", account->pw_dir, 1) < 0 || setenv("USER", account->pw_name, 1) < 0 ||
        setenv("LOGNAME", account->pw_name, 1) < 0 || setenv("SHELL", account->pw_shell, 1) < 0 ||
        (!getenv("PATH") && setenv("PATH", RELIEFOS_DEFAULT_PATH, 1) < 0) ||
        account->pw_gid != state.gid || setgroups(state.group_count, groups) < 0 || setgid(state.gid) < 0 ||
        setuid(account->pw_uid) < 0 || chdir(account->pw_dir) < 0) goto out;
    result = 0;
out:;
    int saved = errno ? errno : EIO;
    free(groups);
    close(fd); errno = saved;
    return result;
}

static volatile sig_atomic_t session_ending;
static void session_signal(int number) { (void)number; session_ending = 1; }

struct runtime_snapshot {
    struct rlimit limits[RLIM_NLIMITS];
    uint32_t resources;
    mode_t mask;
    int count;
    gid_t *groups;
};

static int snapshot_runtime(struct runtime_snapshot *state)
{
    memset(state, 0, sizeof(*state));
    state->mask = umask(0); umask(state->mask);
    state->count = getgroups(0, NULL);
    if (state->count < 0) return -1;
    state->groups = calloc(state->count ? (size_t)state->count : 1, sizeof(*state->groups));
    if (!state->groups || getgroups(state->count, state->groups) != state->count) goto failed;
    for (unsigned i = 0; i < RLIM_NLIMITS; ++i) {
        if (getrlimit(i, &state->limits[i]) == 0) state->resources |= 1u << i;
        else if (errno != EINVAL && errno != ENOSYS) goto failed;
    }
    return 0;
failed:
    free(state->groups);
    return -1;
}

static int restore_runtime(const struct runtime_snapshot *state)
{
    for (unsigned i = 0; i < RLIM_NLIMITS; ++i)
        if ((state->resources & (1u << i)) && setrlimit(i, &state->limits[i]) < 0) return -1;
    umask(state->mask);
    return setgroups((size_t)state->count, state->groups);
}

int reliefos_pam_login(const char *name, char *password, struct reliefos_user_info *user)
{
    if (geteuid() != 0 || getuid() != 0) { errno = EPERM; return -1; }
    if (!name || !password || !user) { errno = EINVAL; return -1; }
    int lock = session_mutex();
    if (lock < 0) return -1;
    if (login_pam) { close(lock); errno = EALREADY; return -1; }
    struct runtime_snapshot previous;
    if (snapshot_runtime(&previous) < 0) { close(lock); return -1; }
    login_conversation = (struct conversation){.secret = strdup(password)};
    struct conversation *context = &login_conversation;
    explicit_bzero(password, strlen(password));
    if (!context->secret) { free(previous.groups); close(lock); return -1; }
    struct pam_conv conv = {.conv = conversation, .appdata_ptr = context};
    pam_handle_t *pam = NULL;
    int cred = 0, session = 0;
    const char *stage = "start";
    int code = pam_start("reliefos-gui", name, &conv, &pam);
    if (code == PAM_SUCCESS) code = pam_set_item(pam, PAM_TTY, "reliefos-gui");
    if (code == PAM_SUCCESS) { stage = "authenticate"; code = pam_authenticate(pam, PAM_DISALLOW_NULL_AUTHTOK); }
    if (code == PAM_SUCCESS) { fprintf(stderr, "[pam-login] authentication accepted\n"); stage = "account"; code = pam_acct_mgmt(pam, 0); }
    if (code == PAM_NEW_AUTHTOK_REQD) {
        stage = "password-change";
        code = pam_chauthtok(pam, PAM_CHANGE_EXPIRED_AUTHTOK);
        if (code == PAM_SUCCESS) code = pam_acct_mgmt(pam, 0);
    }
    const void *canonical = NULL;
    if (code == PAM_SUCCESS) code = pam_get_item(pam, PAM_USER, &canonical);
    if (code == PAM_SUCCESS && (!canonical || reliefos_account_info(getpwnam(canonical), user) < 0)) code = PAM_USER_UNKNOWN;
    if (code == PAM_SUCCESS) {
        stage = "groups";
        fprintf(stderr, "[pam-login] applying groups\n");
        struct passwd *account = getpwuid(user->uid);
        if (!account || initgroups(account->pw_name, account->pw_gid) < 0) code = PAM_CRED_ERR;
    }
    if (code == PAM_SUCCESS) { fprintf(stderr, "[pam-login] establishing credentials\n"); stage = "credentials"; code = pam_setcred(pam, PAM_ESTABLISH_CRED); cred = code == PAM_SUCCESS; }
    if (code == PAM_SUCCESS) { fprintf(stderr, "[pam-login] opening session\n"); stage = "open-session"; code = pam_open_session(pam, 0); session = code == PAM_SUCCESS; }
    if (context->secret) { explicit_bzero(context->secret, strlen(context->secret)); free(context->secret); context->secret = NULL; }
    if (code == PAM_SUCCESS) { fprintf(stderr, "[pam-login] publishing session\n"); stage = "publish-session"; if (publish(pam, user) < 0) code = PAM_SYSTEM_ERR; }
    struct stat identity;
    if (code == PAM_SUCCESS && lstat(SESSION, &identity) < 0) code = PAM_SYSTEM_ERR;
    if (code == PAM_SUCCESS) {
        login_pam = pam;
        session_lock = lock;
        session_identity = identity;
        fprintf(stderr, "[pam-login] session ready\n");
        free(previous.groups);
        return 0;
    }
    fprintf(stderr, "[pam-login] stage=%s status=%d\n", stage, code);
    if (session) pam_close_session(pam, 0);
    if (cred) pam_setcred(pam, PAM_DELETE_CRED);
    if (pam) pam_end(pam, code);
    unlink(SESSION); unlink(STATE);
    if (session_marker >= 0) { close(session_marker); session_marker = -1; }
    close(lock);
    int restored = restore_runtime(&previous);
    free(previous.groups);
    if (restored < 0) _exit(126);
    switch (code) {
    case PAM_USER_UNKNOWN: case PAM_AUTH_ERR: case PAM_PERM_DENIED:
    case PAM_CRED_INSUFFICIENT: errno = EACCES; break;
    case PAM_ACCT_EXPIRED: case PAM_NEW_AUTHTOK_REQD: errno = EKEYEXPIRED; break;
    case PAM_MAXTRIES: errno = EAGAIN; break;
    case PAM_CONV_ERR: errno = context->cancelled ? ECANCELED : EIO; break;
    case PAM_AUTHINFO_UNAVAIL: errno = ENODATA; break;
    default: errno = EIO; break;
    }
    return -1;
}

int reliefos_pam_session_wait(void)
{
    if (!login_pam) { errno = EINVAL; return -1; }
    struct sigaction action = {.sa_handler = session_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL); sigaction(SIGHUP, &action, NULL);
    for (;;) {
        struct stat current;
        if (session_ending || lstat(SESSION, &current) < 0 ||
            current.st_ino != session_identity.st_ino || current.st_dev != session_identity.st_dev) break;
        usleep(100000);
    }
    int status = pam_close_session(login_pam, 0);
    int deleted = pam_setcred(login_pam, PAM_DELETE_CRED);
    pam_end(login_pam, status != PAM_SUCCESS ? status : deleted);
    login_pam = NULL;
    unlink(SESSION); unlink(STATE);
    close(session_marker); session_marker = -1;
    close(session_lock); session_lock = -1;
    return status == PAM_SUCCESS && deleted == PAM_SUCCESS ? 0 : -1;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_auth_logout) leonos_auth_logout __attribute__((alias("reliefos_auth_logout")));
extern __typeof__(reliefos_pam_login) leonos_pam_login __attribute__((alias("reliefos_pam_login")));
extern __typeof__(reliefos_pam_session_wait) leonos_pam_session_wait __attribute__((alias("reliefos_pam_session_wait")));
extern __typeof__(reliefos_session_apply) leonos_session_apply __attribute__((alias("reliefos_session_apply")));
extern __typeof__(reliefos_session_current) leonos_session_current __attribute__((alias("reliefos_session_current")));
extern __typeof__(reliefos_session_initialize) leonos_session_initialize __attribute__((alias("reliefos_session_initialize")));
