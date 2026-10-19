#define _GNU_SOURCE
#include <assert.h>
#include <crypt.h>
#include <sys/wait.h>
#include "../../userland/runtime/src/pam_session.c"

static int fault = ENOSYS;
static unsigned dialogs;
static unsigned resource_calls;
static gid_t effective_groups[32];
static size_t effective_group_count;

/* Capture initgroups' real musl group selection where user namespaces deny setgroups. */
int setgroups(size_t count, const gid_t *groups)
{
    assert(count <= sizeof(effective_groups) / sizeof(effective_groups[0]));
    memcpy(effective_groups, groups, count * sizeof(*groups));
    effective_group_count = count;
    return 0;
}

int getgroups(int size, gid_t *groups)
{
    if (size < 0 || (size && (size_t)size < effective_group_count)) {
        errno = EINVAL;
        return -1;
    }
    if (size) memcpy(groups, effective_groups, effective_group_count * sizeof(*groups));
    return (int)effective_group_count;
}

/* The user namespace denies the kernel setgroups syscall. Preserve musl's
 * actual /etc/group resolution while capturing the resulting group vector. */
int initgroups(const char *user, gid_t primary)
{
    gid_t groups[32];
    int count = (int)(sizeof(groups) / sizeof(groups[0]));
    int result = getgrouplist(user, primary, groups, &count);
    if (result < 0) return -1;
    return setgroups((size_t)result, groups);
}

/* Reproduce the current kernel's missing resources at the real DSO boundary. */
int getrlimit(int resource, struct rlimit *limit)
{
    ++resource_calls;
    if (resource != RLIMIT_NOFILE && resource != RLIMIT_STACK && resource != RLIMIT_AS &&
        resource != RLIMIT_NPROC && resource != RLIMIT_SIGPENDING) {
        errno = resource_calls <= RLIM_NLIMITS ? ENOSYS : fault;
        return -1;
    }
    return syscall(SYS_prlimit64, 0, resource, NULL, limit);
}

int reliefos_ui_show_password_dialog(const char *title, const char *label, char *value, uint32_t size)
{ (void)title; (void)label; (void)value; (void)size; ++dialogs; return 0; }
int reliefos_ui_show_input_dialog(const char *title, const char *label, char *value, uint32_t size)
{ (void)title; (void)label; (void)value; (void)size; ++dialogs; return 0; }
int reliefos_ui_show_message_box(const char *title, const char *message, const char *button)
{ (void)title; (void)message; (void)button; ++dialogs; return 1; }

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void run_case(const char *label, const char *secret, const char *limits, int error, int resource_error)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        fault = resource_error;
        resource_calls = 0;
        write_file("/etc/security/limits.conf", limits);
        struct reliefos_user_info user;
        char password[64];
        snprintf(password, sizeof(password), "%s", secret);
        int result = reliefos_pam_login("root", password, &user);
        int actual = result < 0 ? errno : 0;
        fprintf(stderr, "[pam-login-case] %s result=%d errno=%d expected=%d\n", label, result, actual, error);
        assert(actual == error && password[0] == 0 && dialogs == 0);
        if (!error) {
            assert(user.uid == 0);
            if (*limits) {
                struct rlimit current;
                assert(getrlimit(RLIMIT_NOFILE, &current) == 0 && current.rlim_cur == 64);
            }
            assert(reliefos_auth_logout() == 0);
            assert(reliefos_pam_session_wait() == 0);
        } else assert(access(SESSION, F_OK) == -1 && errno == ENOENT);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) fprintf(stderr, "case %s wait status=%d\n", label, status);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void run_video_group_case(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        fault = ENOSYS;
        resource_calls = 0;
        effective_group_count = 0;
        write_file("/etc/security/limits.conf", "");
        struct reliefos_user_info user;
        char password[] = "AbC123!";
        int result = reliefos_pam_login("test", password, &user);
        assert(result == 0 && user.uid == 1000 && password[0] == 0);
        gid_t groups[32];
        int count = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);
        assert(count >= 2);
        int has_personal = 0, has_video = 0;
        for (int i = 0; i < count; ++i) {
            if (groups[i] == 1000) has_personal = 1;
            if (groups[i] == 44) has_video = 1;
        }
        assert(has_personal && has_video);
        assert(reliefos_auth_logout() == 0);
        assert(reliefos_pam_session_wait() == 0);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr, "case video group wait status=%d\n", status);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    struct conversation context = {0};
    struct pam_message prompt = {.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password:"};
    const struct pam_message *messages[] = {&prompt};
    struct pam_response *response = NULL;
    assert(conversation(1, messages, &response, &context) == PAM_CONV_ERR && context.cancelled);
    context.cancelled = 0;
    messages[0] = NULL;
    assert(conversation(1, messages, &response, &context) == PAM_CONV_ERR && !context.cancelled);
    dialogs = 0;
    assert(mkdir("/etc/pam.d", 0755) == 0);
    assert(mkdir("/etc/security", 0755) == 0);
    assert(mkdir("/run/reliefos", 0755) == 0);
    write_file("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n"
               "test:x:1000:1000:Test user:/home/test:/bin/sh\n");
    char salt[CRYPT_GENSALT_OUTPUT_SIZE], text[512];
    struct crypt_data data = {0};
    assert(crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt)));
    char *hash = crypt_rn("AbC123!", salt, &data, sizeof(data));
    assert(hash && *hash == '$');
    snprintf(text, sizeof(text), "root:%s:20000:0:99999:7:::\ntest:%s:20000:0:99999:7:::\n",
             hash, hash);
    write_file("/etc/shadow", text);
    write_file("/etc/pam.d/reliefos-gui",
               "auth required pam_unix.so\naccount required pam_unix.so\n"
               "session required pam_limits.so\n");
    run_case("wrong lowercase", "abc123!", "", EACCES, ENOSYS);
    run_case("correct mixed case", "AbC123!", "", 0, ENOSYS);
    run_case("supported nofile enforced", "AbC123!", "root soft nofile 64\n", 0, ENOSYS);
    run_case("unsupported cpu denied", "AbC123!", "root hard cpu 1\n", EACCES, ENOSYS);
    run_case("unexpected resource failure", "AbC123!", "", EIO, EIO);
    run_video_group_case();
    puts("[pam-login-case] PASS");
    return 0;
}
