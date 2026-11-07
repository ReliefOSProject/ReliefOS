#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <syslog.h>
#include <pwd.h>
#include <shadow.h>
#ifdef PAM_TEST_NONROOT_HELPER
#include <grp.h>
#include <sys/wait.h>
#endif
#ifdef PAM_TEST_ENTROPY_FAILURE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

static unsigned failures, conversations;
static int cancel;
static const char *password = "r";
#define CHECK(c) do { if (!(c)) { \
    printf("[pam-runtime] FAIL line=%d %s\n", __LINE__, #c); ++failures; } } while (0)

static int conversation(int count, const struct pam_message **messages,
                         struct pam_response **out, void *data)
{
    (void)data;
    ++conversations;
    *out = NULL;
    if (cancel) return PAM_CONV_ERR;
    if (count <= 0 || count > PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
    struct pam_response *responses = calloc((size_t)count, sizeof(*responses));
    if (!responses) return PAM_BUF_ERR;
    for (int i = 0; i < count; ++i) {
        if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            messages[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            responses[i].resp = strdup(password);
            if (!responses[i].resp) {
                for (int j = 0; j < i; ++j) {
                    if (responses[j].resp) {
                        explicit_bzero(responses[j].resp, strlen(responses[j].resp));
                        free(responses[j].resp);
                    }
                }
                free(responses);
                return PAM_BUF_ERR;
            }
        } else if (messages[i]->msg_style != PAM_TEXT_INFO &&
                   messages[i]->msg_style != PAM_ERROR_MSG) {
            for (int j = 0; j < i; ++j) {
                if (responses[j].resp) {
                    explicit_bzero(responses[j].resp, strlen(responses[j].resp));
                    free(responses[j].resp);
                }
            }
            free(responses);
            return PAM_CONV_ERR;
        }
    }
    *out = responses;
    return PAM_SUCCESS;
}

static pam_handle_t *start(const char *service, const char *user, const char *directory)
{
    const struct pam_conv conv = {conversation, NULL};
    pam_handle_t *handle = NULL;
    int ret = pam_start_confdir(service, user, &conv, directory, &handle);
    CHECK(ret == PAM_SUCCESS);
    return ret == PAM_SUCCESS ? handle : NULL;
}

static int descriptor_count(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) < 0 || limit.rlim_cur > 1048576) return -1;
    int count = 0;
    for (rlim_t fd = 0; fd < limit.rlim_cur; ++fd) {
        int flags = fcntl((int)fd, F_GETFD);
        if (flags >= 0) ++count;
        else if (errno != EBADF) return -1;
    }
    return count;
}

/* Only the disposable diagnostic image contains these explicit fixture files.
 * Wait for authd's initial account export before replacing scratch /etc data. */
static int install_guest_fixture(void)
{
    const char *names[] = {"passwd", "group", "shadow"};
    for (unsigned i = 0; i < 200 && access("/run/leonos/authd.sock", F_OK) < 0; ++i)
        usleep(100000);
    if (access("/run/leonos/authd.sock", F_OK) < 0 || geteuid() != 0) return -1;
    for (unsigned i = 0; i < 3; ++i) {
        char source[160], destination[64], temporary[80];
        snprintf(source, sizeof(source), "/usr/lib/reliefos/tests/pam-fixture/%s", names[i]);
        snprintf(destination, sizeof(destination), "/etc/%s", names[i]);
        snprintf(temporary, sizeof(temporary), "/etc/.pam-fixture-%s", names[i]);
        int in = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (in < 0) return -1;
        int out = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, i == 2 ? 0600 : 0644);
        if (out < 0) { close(in); return -1; }
        char buffer[1024];
        ssize_t size;
        int result = 0;
        while ((size = read(in, buffer, sizeof(buffer))) > 0) {
            ssize_t offset = 0;
            while (offset < size) {
                ssize_t n = write(out, buffer + offset, (size_t)(size - offset));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { result = -1; break; }
                offset += n;
            }
            if (result) break;
        }
        if (size < 0 || fsync(out) < 0) result = -1;
        if (close(in) < 0) result = -1;
        if (close(out) < 0) result = -1;
        if (!result && rename(temporary, destination) < 0) result = -1;
        if (result) { unlink(temporary); return -1; }
    }
    return 0;
}

#ifdef PAM_TEST_NONROOT_HELPER
static void nonroot_helper(const char *directory)
{
    pid_t child = fork();
    if (!child) {
        failures = 0;
        if (setgroups(0, NULL) || setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000)) _exit(1);
        CHECK(getspnam("pam_fixture") == NULL);
        int secret = open("/etc/shadow", O_RDONLY);
        CHECK(secret == -1 && errno == EACCES);
        if (secret >= 0) close(secret);
        const char *names[] = {"pam_fixture", "pam_fixture", "pam_other"};
        const char *answers[] = {"r", "incorrect", "r"};
        for (unsigned i = 0; i < 3; ++i) {
            password = answers[i];
            pam_handle_t *pamh = start("unix", names[i], directory);
            if (!pamh) continue;
            int result = pam_authenticate(pamh, PAM_SILENT);
            printf("[pam-runtime] nonroot helper case=%u ret=%d\n", i, result);
            CHECK(i ? result != PAM_SUCCESS : result == PAM_SUCCESS);
            CHECK(pam_end(pamh, result) == PAM_SUCCESS);
        }
        _exit(failures != 0);
    }
    int status;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}
#endif

int main(int argc, char **argv)
{
#ifdef PAM_TEST_NONROOT_HELPER
    const char *directory = "/usr/lib/reliefos/tests/pam.d";
#else
    const char *directory = argc > 1 ? argv[1] : "/usr/lib/reliefos/tests/pam.d";
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[pam-runtime] BEGIN Linux-PAM 1.7.3 / pinned musl, production DSOs");
    struct { const char *name; int expected; } cases[] = {
        {"permit", PAM_SUCCESS}, {"deny", PAM_AUTH_ERR},
        {"required", PAM_AUTH_ERR}, {"sufficient", PAM_SUCCESS},
        {"jump", PAM_SUCCESS}, {"include", PAM_AUTH_ERR},
        {"substack", PAM_AUTH_ERR}, {"missing-module", PAM_MODULE_UNKNOWN},
        {"nonexistent-service", PAM_AUTH_ERR},
    };
    int before = descriptor_count();
    for (unsigned repeat = 0; repeat < 16; ++repeat) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            pam_handle_t *pamh = start(cases[i].name, "alice", directory);
            if (!pamh) continue;
            int ret = pam_authenticate(pamh, 0);
            if (ret != cases[i].expected)
                printf("[pam-runtime] service=%s expected=%d got=%d\n", cases[i].name, cases[i].expected, ret);
            CHECK(ret == cases[i].expected);
            CHECK(pam_end(pamh, ret) == PAM_SUCCESS);
        }
    }
    /* musl syslog retains its process-global socket until closelog(). */
    closelog();
    int after = descriptor_count();
    printf("[pam-runtime] descriptor counts before=%d after=%d\n", before, after);
    CHECK(before >= 0 && after == before);
    pam_handle_t *first = start("permit", "alice", directory);
    pam_handle_t *second = start("permit", "root", directory);
    if (first && second) {
        CHECK(pam_putenv(first, "LEONOS_PAM_TEST=one") == PAM_SUCCESS);
        CHECK(pam_getenv(second, "LEONOS_PAM_TEST") == NULL);
        CHECK(!strcmp(pam_getenv(first, "LEONOS_PAM_TEST"), "one"));
        CHECK(pam_acct_mgmt(first, 0) == PAM_SUCCESS);
        CHECK(pam_setcred(first, PAM_ESTABLISH_CRED) == PAM_SUCCESS);
        CHECK(pam_open_session(first, 0) == PAM_SUCCESS);
        CHECK(pam_close_session(first, 0) == PAM_SUCCESS);
        CHECK(pam_setcred(first, PAM_DELETE_CRED) == PAM_SUCCESS);
        CHECK(pam_chauthtok(first, 0) == PAM_SUCCESS);
        CHECK(pam_putenv(first, "LEONOS_PAM_TEST") == PAM_SUCCESS);
        CHECK(pam_getenv(first, "LEONOS_PAM_TEST") == NULL);
    }
    if (first) CHECK(pam_end(first, PAM_SUCCESS) == PAM_SUCCESS);
    if (second) CHECK(pam_end(second, PAM_SUCCESS) == PAM_SUCCESS);
    first = start("phase-failure", "alice", directory);
    if (first) {
        CHECK(pam_authenticate(first, 0) == PAM_SUCCESS);
        CHECK(pam_acct_mgmt(first, 0) == PAM_ACCT_EXPIRED);
        CHECK(pam_setcred(first, PAM_ESTABLISH_CRED) == PAM_CRED_ERR);
        CHECK(pam_open_session(first, 0) == PAM_SESSION_ERR);
        CHECK(pam_end(first, PAM_SESSION_ERR) == PAM_SUCCESS);
    }
    /* Only the dedicated fixture namespace has this account name. */
    int entropy_failure = argc > 2 && !strcmp(argv[2], "unix-no-entropy");
    int unix_fixture = argc > 2 && (!strcmp(argv[2], "unix") || entropy_failure);
#ifdef PAM_TEST_NONROOT_HELPER
    unix_fixture = 1;
#endif
    if (argc == 1 && access("/usr/lib/reliefos/tests/pam-fixture/shadow", F_OK) == 0) {
        int result = install_guest_fixture();
        CHECK(result == 0);
        unix_fixture = result == 0;
    }
    if (unix_fixture) {
        struct passwd *fixture_user = getpwnam("pam_fixture");
        struct spwd *fixture_shadow = getspnam("pam_fixture");
        printf("[pam-runtime] fixture euid=%u user=%d shadow=%d errno=%d\n",
            (unsigned)geteuid(), fixture_user != NULL, fixture_shadow != NULL, errno);
        const char *answers[] = {"r", "incorrect", "", "r"};
        for (unsigned i = 0; i < 4; ++i) {
            first = start("unix", "pam_fixture", directory);
            if (!first) continue;
            password = answers[i];
            cancel = i == 3;
            unsigned previous = conversations;
            int ret = pam_authenticate(first, PAM_SILENT);
            printf("[pam-runtime] unix authentication case=%u ret=%d\n", i, ret);
            CHECK(i == 0 ? ret == PAM_SUCCESS : ret != PAM_SUCCESS);
            CHECK(conversations > previous);
            CHECK(pam_end(first, ret) == PAM_SUCCESS);
        }
        cancel = 0;
#ifdef PAM_TEST_NONROOT_HELPER
        nonroot_helper(directory);
#endif
        password = "z";
        char previous_hash[256] = {0};
        if (entropy_failure) {
            fixture_shadow = getspnam("pam_fixture");
            CHECK(fixture_shadow && strlen(fixture_shadow->sp_pwdp) < sizeof(previous_hash));
            if (fixture_shadow) snprintf(previous_hash, sizeof(previous_hash), "%s", fixture_shadow->sp_pwdp);
#ifdef PAM_TEST_ENTROPY_FAILURE
            struct sock_filter filters[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getrandom, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EIO),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog program = {sizeof(filters) / sizeof(filters[0]), filters};
            CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
            CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0);
#else
            CHECK(!entropy_failure);
#endif
        }
        first = start("unix", "pam_fixture", directory);
        if (first) {
            int account = pam_acct_mgmt(first, 0);
            printf("[pam-runtime] unix account ret=%d\n", account);
            CHECK(account == PAM_SUCCESS);
            int ret = pam_chauthtok(first, 0);
            if (ret != PAM_SUCCESS) printf("[pam-runtime] password update returned=%d\n", ret);
            /* pam_unix_passwd.c maps a NULL create_password_hash to PAM_BUF_ERR. */
            CHECK(entropy_failure ? ret == PAM_BUF_ERR : ret == PAM_SUCCESS);
            CHECK(pam_end(first, ret) == PAM_SUCCESS);
        }
        if (entropy_failure) {
            fixture_shadow = getspnam("pam_fixture");
            CHECK(fixture_shadow && !strcmp(previous_hash, fixture_shadow->sp_pwdp));
            password = "r";
            puts("[pam-runtime] entropy failure refused password update; shadow unchanged");
        }
        first = start("unix", "pam_fixture", directory);
        if (first) {
            int authenticated = pam_authenticate(first, PAM_SILENT);
            printf("[pam-runtime] updated password authentication ret=%d\n", authenticated);
            CHECK(authenticated == PAM_SUCCESS);
            CHECK(pam_end(first, PAM_SUCCESS) == PAM_SUCCESS);
        }
        puts("[pam-runtime] pam_unix exercised=1");
    }
    closelog();
    CHECK(descriptor_count() == before);
    printf("[pam-runtime] DONE failures=%u\n", failures);
    return failures != 0;
}
