#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;
static const char *marker = "/run/sudo-runtime-executed";
static uid_t command_uid;
static const char *command_password;
#define CHECK(c) do { if (!(c)) { \
    printf("[sudo-runtime] FAIL line=%d errno=%d %s\n", __LINE__, errno, #c); ++failures; } } while (0)

static int write_config(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    size_t size = strlen(text), offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, text + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        offset += (size_t)n;
    }
    int result = fchmod(fd, mode);
    if (fsync(fd) < 0) result = -1;
    if (close(fd) < 0) result = -1;
    return result;
}

static void command(const char *name, char *const argv[], int expected, const char *required)
{
    int descriptors[2];
    int result = pipe2(descriptors, O_CLOEXEC);
    CHECK(result == 0);
    if (result < 0) return;
    int password_pipe[2] = {-1, -1};
    if (command_password) {
        CHECK(pipe2(password_pipe, O_CLOEXEC) == 0);
        if (password_pipe[0] < 0) { close(descriptors[0]); close(descriptors[1]); return; }
        size_t length = strlen(command_password);
        CHECK(write(password_pipe[1], command_password, length) == (ssize_t)length);
        close(password_pipe[1]);
    }
    pid_t child = fork();
    if (!child) {
        int input = command_password ? password_pipe[0] : open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, 0) < 0 || dup2(descriptors[1], 1) < 0 ||
            dup2(descriptors[1], 2) < 0) _exit(125);
        close(input);
        close(descriptors[0]);
        close(descriptors[1]);
        clearenv();
        setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("HOME", command_uid ? "/tmp" : "/root", 1);
        setenv("LC_ALL", "C", 1);
        if (command_uid && (setgroups(0, NULL) || setgid(command_uid) || setuid(command_uid))) _exit(124);
        execv(argv[0], argv);
        _exit(126);
    }
    close(descriptors[1]);
    if (password_pipe[0] >= 0) close(password_pipe[0]);
    char output[32768];
    size_t offset = 0;
    ssize_t size;
    while ((size = read(descriptors[0], output + offset, sizeof(output) - 1 - offset)) != 0) {
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) break;
        offset += (size_t)size;
        if (offset == sizeof(output) - 1) break;
    }
    output[offset] = 0;
    close(descriptors[0]);
    int status = 0;
    int waited = child > 0 && waitpid(child, &status, 0) == child;
    int code = waited && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    printf("[sudo-runtime] case=%s exit=%d output-begin\n%s\n[sudo-runtime] output-end\n", name, code, output);
    CHECK(offset < sizeof(output) - 1 && size >= 0);
    CHECK(code == expected);
    CHECK(!required || strstr(output, required));
    CHECK(expected == 0 || !strstr(output, "[su-identity]"));
}

static void test_su(const char *self)
{
    static const char allowed[] = "auth sufficient /lib/security/pam_rootok.so\n"
        "auth required /lib/security/pam_unix.so nodelay\n"
        "account required /lib/security/pam_unix.so\n"
        "session required /lib/security/pam_permit.so\n";
    static const char denied[] = "auth required /lib/security/pam_deny.so\n"
        "account required /lib/security/pam_permit.so\n"
        "session required /lib/security/pam_permit.so\n";
    CHECK(write_config("/etc/passwd", "root:x:0:0::/root:/bin/sh\n"
        "sudo_fixture:x:1000:1000::/tmp:/bin/sh\n"
        "su_target:x:1001:2001::/tmp:/bin/sh\n", 0644) == 0);
    CHECK(write_config("/etc/group", "root:x:0:\nsudo_fixture:x:1000:\n"
        "su_target:x:2001:\nsu_extra:x:2002:su_target\n", 0644) == 0);
    CHECK(write_config("/etc/shadow", "root:!:1:0:99999:7:::\n"
        "sudo_fixture:$6$sudoFixture$qC4cGMKkZTvWDv0hq3DDyRLbNwQKHsQWAs.TXBcBD.KZzJfTAedJGdtYAxqMrAePQKxTyrEKJfc/hchzhQp81.:1:0:99999:7:::\n"
        "su_target:$6$rootFixture$.5Ftj8BydtUP.YM5MDO8csbfGYGug4uXFRbgsXydsoe2fSoW50CIzlowxvyccYjrcBS9Y0zvDJsEXZjCEAnC9/:1:0:99999:7:::\n", 0600) == 0);
    CHECK(write_config("/etc/pam.d/su", allowed, 0644) == 0);
    CHECK(write_config("/etc/pam.d/su-l", denied, 0644) == 0);
    CHECK(write_config("/etc/login.defs", "FAIL_DELAY 0\n", 0644) == 0);
    CHECK(write_config("/etc/shells", "/bin/sh\n", 0644) == 0);
    char shell_command[4096];
    int length = snprintf(shell_command, sizeof(shell_command), "%s --print-identity", self);
    CHECK(length > 0 && (size_t)length < sizeof(shell_command));
    char *normal[] = {"/bin/su", "-c", shell_command, "su_target", NULL};
    char *login[] = {"/bin/su", "-l", "-c", shell_command, "su_target", NULL};
    command_uid = 0;
    command_password = NULL;
    command("official-su-version", (char *[]){"/bin/su", "--version", NULL}, 0, "util-linux 2.42.4");
    command("su-root-target-groups", normal, 0, "[su-identity] uid=1001/1001/1001 gid=2001/2001/2001 extra=1");
    command_uid = 1000;
    command_password = "r\n";
    command("su-caller-password-rejected", normal, 1, "Authentication failure");
    command_password = "z\n";
    command("su-target-password", normal, 0, "user=su_target home=/tmp shell=/bin/sh");
    command("su-login-service-denied", login, 1, "Authentication failure");
    CHECK(write_config("/etc/pam.d/su-l", allowed, 0644) == 0);
    command("su-login-environment", login, 0, "user=su_target home=/tmp shell=/bin/sh cwd=/tmp");
    CHECK(write_config("/etc/pam.d/su", "auth required /lib/security/pam_unix.so nodelay\n"
        "account required /lib/security/pam_deny.so\n"
        "session required /lib/security/pam_permit.so\n", 0644) == 0);
    command("su-correct-password-account-denied", normal, 1, NULL);
    CHECK(write_config("/etc/pam.d/su", "auth required /lib/security/pam_unix.so nodelay\n"
        "account required /lib/security/pam_permit.so\n"
        "session required /lib/security/pam_debug.so open_session=perm_denied\n", 0644) == 0);
    command("su-correct-password-session-denied", normal, 1, "cannot open session");
    CHECK(write_config("/etc/pam.d/su", "auth required /lib/security/pam_debug.so auth=success cred=perm_denied\n"
        "account required /lib/security/pam_permit.so\n"
        "session required /lib/security/pam_permit.so\n", 0644) == 0);
    command("su-setcred-denied", normal, 1, NULL);
    command_uid = 0;
    command_password = NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--print-identity")) {
        uid_t real, effective, saved;
        gid_t greal, geffective, gsaved, groups[16];
        char cwd[4096];
        if (getresuid(&real, &effective, &saved) || getresgid(&greal, &geffective, &gsaved) ||
            !getcwd(cwd, sizeof(cwd))) return 125;
        int count = getgroups(16, groups), extra = 0;
        if (count < 0) return 125;
        for (int i = 0; i < count; ++i) extra |= groups[i] == 2002;
        printf("[su-identity] uid=%u/%u/%u gid=%u/%u/%u extra=%d user=%s home=%s shell=%s cwd=%s\n",
               real, effective, saved, greal, geffective, gsaved, extra,
               getenv("USER"), getenv("HOME"), getenv("SHELL"), cwd);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--print-euid")) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || close(fd) < 0) return 125;
        printf("uid=%u\n", (unsigned)geteuid());
        return 0;
    }
    puts("[sudo-runtime] BEGIN official sudo 1.9.17p2 root and nonroot set-ID entry");
    CHECK(getuid() == 0 && geteuid() == 0);
    /* These paths are writable only inside this test's private namespace/disk. */
    const char *directories[] = {"/etc", "/run", "/var", "/var/lib", "/var/lib/sudo"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        CHECK(mkdir(directories[i], 0755) == 0 || errno == EEXIST);
    CHECK(write_config("/etc/passwd", "root:x:0:0::/root:/bin/sh\n", 0644) == 0);
    CHECK(write_config("/etc/group", "root:x:0:\n", 0644) == 0);
    CHECK(write_config("/etc/sudo.conf", "Plugin sudoers_policy sudoers.so\nPlugin sudoers_io sudoers.so\n", 0644) == 0);
    CHECK(write_config("/etc/sudoers", "root ALL=(ALL:ALL) ALL\n", 0440) == 0);
    CHECK(mkdir("/etc/pam.d", 0755) == 0 || errno == EEXIST);
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_deny.so\n"
        "account required /lib/security/pam_permit.so\nsession required /lib/security/pam_permit.so\n", 0644) == 0);
    command("version", (char *[]){"/usr/bin/sudo", "-V", NULL}, 0, "Sudo version 1.9.17p2");
    command("visudo-valid", (char *[]){"/usr/sbin/visudo", "-c", NULL}, 0, "parsed OK");
    command("root-command", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 0, "uid=0\n");
    CHECK(unlink(marker) == 0);
    char policy[4096];
    int length = snprintf(policy, sizeof(policy), "root ALL=(ALL:ALL) !%s\n", argv[0]);
    CHECK(length > 0 && (size_t)length < sizeof(policy));
    CHECK(write_config("/etc/sudoers", policy, 0440) == 0);
    command("revoked-command", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 1, "not allowed");
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    CHECK(write_config("/etc/sudoers", "root ALL=(ALL:ALL) ALL\n", 0440) == 0);
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_permit.so\n"
        "account required /lib/security/pam_deny.so\nsession required /lib/security/pam_permit.so\n", 0644) == 0);
    command("account-denied", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 1, NULL);
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_permit.so\n"
        "account required /lib/security/pam_permit.so\n"
        "session required /lib/security/pam_debug.so open_session=perm_denied\n", 0644) == 0);
    command("fatal-session-error", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 1, NULL);
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    /* sudo 1.9.17p2 auth/pam.c explicitly treats PAM_SESSION_ERR as nonfatal. */
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_permit.so\n"
        "account required /lib/security/pam_permit.so\nsession required /lib/security/pam_deny.so\n", 0644) == 0);
    command("upstream-nonfatal-session-err", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 0, "uid=0\n");
    CHECK(unlink(marker) == 0);
    CHECK(write_config("/etc/sudoers", "not valid sudoers syntax @\n", 0440) == 0);
    command("visudo-invalid", (char *[]){"/usr/sbin/visudo", "-c", NULL}, 1, "syntax error");
    if (argc == 2 && !strcmp(argv[1], "--root-only")) {
        printf("[sudo-runtime] DONE root-only failures=%u\n", failures);
        return failures != 0;
    }
    CHECK(write_config("/etc/passwd", "root:x:0:0::/root:/bin/sh\n"
        "sudo_fixture:x:1000:1000::/tmp:/bin/sh\n", 0644) == 0);
    CHECK(write_config("/etc/group", "root:x:0:\nsudo_fixture:x:1000:\n", 0644) == 0);
    CHECK(write_config("/etc/shadow", "root:$6$rootFixture$.5Ftj8BydtUP.YM5MDO8csbfGYGug4uXFRbgsXydsoe2fSoW50CIzlowxvyccYjrcBS9Y0zvDJsEXZjCEAnC9/:1:0:99999:7:::\n"
        "sudo_fixture:$6$sudoFixture$qC4cGMKkZTvWDv0hq3DDyRLbNwQKHsQWAs.TXBcBD.KZzJfTAedJGdtYAxqMrAePQKxTyrEKJfc/hchzhQp81.:1:0:99999:7:::\n", 0600) == 0);
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_unix.so nodelay\n"
        "account required /lib/security/pam_unix.so\nsession required /lib/security/pam_permit.so\n", 0644) == 0);
    CHECK(write_config("/etc/sudoers", "Defaults passwd_tries=1,timestamp_timeout=0\nroot ALL=(ALL:ALL) ALL\n", 0440) == 0);
    command_uid = 1000;
    command_password = "r\n";
    command("no-policy-correct-password", (char *[]){"/usr/bin/sudo", "-S", "-k", argv[0], "--print-euid", NULL}, 1, "not in the sudoers");
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    length = snprintf(policy, sizeof(policy), "Defaults passwd_tries=1,timestamp_timeout=0\n"
        "sudo_fixture ALL=(root:root) %s --print-euid\n", argv[0]);
    CHECK(length > 0 && (size_t)length < sizeof(policy) && write_config("/etc/sudoers", policy, 0440) == 0);
    command_password = "z\n";
    command("root-password-is-not-caller-password", (char *[]){"/usr/bin/sudo", "-S", "-k", argv[0], "--print-euid", NULL}, 1, "incorrect password");
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    command_password = "r\n";
    command("caller-password-authorized-command", (char *[]){"/usr/bin/sudo", "-S", "-k", argv[0], "--print-euid", NULL}, 0, "uid=0\n");
    CHECK(unlink(marker) == 0);
    command("caller-password-wrong-arguments", (char *[]){"/usr/bin/sudo", "-S", "-k", argv[0], "--print-euid", "extra", NULL}, 1, "not allowed");
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    length = snprintf(policy, sizeof(policy), "sudo_fixture ALL=(root:root) NOPASSWD: %s --print-euid\n", argv[0]);
    CHECK(length > 0 && (size_t)length < sizeof(policy) && write_config("/etc/sudoers", policy, 0440) == 0);
    command_password = NULL;
    command("nonroot-nopasswd", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 0, "uid=0\n");
    CHECK(unlink(marker) == 0);
    CHECK(write_config("/etc/pam.d/sudo", "auth required /lib/security/pam_permit.so\n"
        "account required /lib/security/pam_deny.so\nsession required /lib/security/pam_permit.so\n", 0644) == 0);
    command("nonroot-nopasswd-account-denied", (char *[]){"/usr/bin/sudo", "-n", argv[0], "--print-euid", NULL}, 1, NULL);
    CHECK(access(marker, F_OK) == -1 && errno == ENOENT);
    test_su(argv[0]);
    printf("[sudo-runtime] DONE failures=%u\n", failures);
    return failures != 0;
}
