/* POSIX-shaped file helpers backed by the ReliefOS userland ABI. */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <reliefos/auth.h>
#include <reliefos/app.h>
#include <reliefos/pty.h>
#include <reliefos/system.h>
#include <sys/reboot.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <linux/syscall.h>
#include <reliefos/layout.h>

extern long syscall0(long number);
extern long syscall1(long number, long a0);
extern long syscall2(long number, long a0, long a1);
extern long syscall3(long number, long a0, long a1, long a2);

extern int sleep_ms(unsigned long milliseconds);
extern unsigned long reliefos_uptime_ms(void);
extern char **environ;

/* BusyBox's env applet normally gets this helper from libbb/executable.o.
 * That object is intentionally omitted from the ReliefOS minimal libbb set;
 * keep the same execvp and SUSv3 exit-code behavior in the port shim. */
extern unsigned char xfunc_error_retval;
void bb_perror_msg_and_die(const char *message, ...);

/* Ash and libbb/lineedit share this latch when an input wait is interrupted.
 * The rest of BusyBox's signals.c is intentionally replaced by the ReliefOS
 * signal shim below, so keep this small state definition here as well. */


/* POSIX requires environ to remain a valid, NULL-terminated vector.  Ash
 * calls clearenv() while preparing noexec applets, so never leave it NULL. */


const char *reliefos_shell_command_path(const char *name);

/* Login updates the kernel task identity of the shell's session, but the
 * shell environment was created before login and therefore may not contain a
 * user-specific HOME variable. Ash calls this on demand for ~ expansion. */
const char *reliefos_shell_home(void)
{
    static char home[RELIEFOS_AUTH_HOME_LEN];
    struct reliefos_user_info user;
    uint32_t index;
    if (reliefos_auth_current(&user) < 0 || !user.uid || !user.home[0]) {
        return 0;
    }
    for (index = 0; index + 1U < sizeof(home) && user.home[index]; ++index) {
        home[index] = user.home[index];
    }
    home[index] = 0;
    return home[0] ? home : 0;
}

/* BusyBox whoami normally resolves the effective UID through /etc/passwd.
 * ReliefOS keeps accounts in the authentication service instead, so expose the
 * session username directly. The unauthenticated installer shell runs as the
 * system administrator context and uses the conventional root name. */
const char *reliefos_shell_user_name(void)
{
    static char username[RELIEFOS_AUTH_USERNAME_LEN];
    struct reliefos_user_info user;
    uint32_t index;
    if (reliefos_auth_current(&user) == 0 && user.username[0]) {
        for (index = 0; index + 1U < sizeof(username) && user.username[index]; ++index) {
            username[index] = user.username[index];
        }
        username[index] = 0;
        return username;
    }
    strncpy(username, "root", sizeof(username) - 1U);
    username[sizeof(username) - 1U] = 0;
    return username;
}

/*
 * ReliefOS terminals are inherited standard streams, not reopenable named tty
 * nodes.  BusyBox less accepts this condition and safely falls back to its
 * stdout descriptor for keyboard input.
 */
/* Signal handlers remain a compatibility stub; kill() termination is provided
 * by the kernel syscall ABI below. */
const char *reliefos_shell_command_path(const char *name)
{
    static char resolved[RELIEFOS_APP_PATH_LEN];
    if (!name || !name[0]) return 0;
    if (strchr(name, '/') || strchr(name, ':')) return name;
    if (strcmp(name, "fdisk") == 0 || strcmp(name, "mkfs.fat") == 0 ||
        strcmp(name, "mkfs.ext4") == 0 || strcmp(name, "fsck.ext4") == 0 ||
        strcmp(name, "mkfs.fat32") == 0 || strcmp(name, "mkfs.vfat") == 0 ||
        strcmp(name, "mkfs.ext2") == 0 || strcmp(name, "mkfs.exfat") == 0 ||
        strcmp(name, "mount") == 0 || strcmp(name, "umount") == 0 ||
        strcmp(name, "fsck") == 0 || strcmp(name, "fsck.fat") == 0 ||
        strcmp(name, "fsck.fat32") == 0 || strcmp(name, "fsck.vfat") == 0 ||
        strcmp(name, "fsck.ext2") == 0 || strcmp(name, "fsck.exfat") == 0 ||
        strcmp(name, "blkid") == 0 || strcmp(name, "lsblk") == 0 ||
        strcmp(name, "sync") == 0)
        return "/bin/busybox";
    if (reliefos_app_registry_resolve(name, resolved, sizeof(resolved)) == 0)
        return resolved;
    return 0;
}

static int reliefos_exec_busybox_applet(char *const argv[])
{
    size_t argc = 0;
    size_t index;
    char **exec_argv;
    int result;
    int saved_errno;

    if (!argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }
    while (argv[argc]) {
        ++argc;
    }
    if (argc > (((size_t)-1) / sizeof(*exec_argv)) - 2U) {
        errno = E2BIG;
        return -1;
    }
    exec_argv = malloc((argc + 2U) * sizeof(*exec_argv));
    if (!exec_argv) {
        errno = ENOMEM;
        return -1;
    }
    exec_argv[0] = "busybox";
    for (index = 0; index < argc; ++index) {
        exec_argv[index + 1U] = argv[index];
    }
    exec_argv[argc + 1U] = 0;
    result = execve("/bin/busybox", exec_argv, environ);
    saved_errno = errno;
    free(exec_argv);
    errno = saved_errno;
    return result;
}

static int reliefos_busybox_execvp(const char *file, char *const argv[])
{
    const char *path = reliefos_shell_command_path(file);
    if (path) {
        return execve(path, argv, environ);
    }

    /* Other BusyBox callers may use execvp directly. Ash uses its dedicated
     * image resolver patch, while this fallback still expresses a bare
     * applet through BusyBox's documented process form. */
    if (!argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }
    return reliefos_exec_busybox_applet(argv);
}

__attribute__((__noreturn__)) void BB_EXECVP_or_die(char **argv)
{
    int saved_errno;

    if (!argv || !argv[0]) {
        errno = EINVAL;
        bb_perror_msg_and_die("can't execute");
    }
    reliefos_busybox_execvp(argv[0], argv);
    saved_errno = errno;
    xfunc_error_retval = (saved_errno == ENOENT) ? 127 : 126;
    errno = saved_errno;
    bb_perror_msg_and_die("can't execute '%s'", argv[0]);
}
