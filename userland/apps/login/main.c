#include <reliefos/pam_session.h>
#include <reliefos/stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

static int take_console_terminal(void)
{
    if ((getsid(0) != getpid() && setsid() < 0) ||
        ioctl(STDIN_FILENO, TIOCSCTTY, 1) < 0 ||
        tcsetpgrp(STDIN_FILENO, getpgrp()) < 0) {
        perror("Initialize login terminal");
        return -1;
    }
    return 0;
}

static int tty_login_main(void)
{
    if (reliefos_session_initialize() < 0) { perror("Initialize accounts"); return 1; }
    if (take_console_terminal() < 0) return 1;
    execl("/bin/login", "login", (char *)0);
    perror("Start login");
    return 1;
}

static int installer_shell_main(void)
{
    if (geteuid() != 0 || access("/etc/reliefos/installer-runtime", F_OK) < 0) {
        errno = EPERM;
        perror("Start installer shell");
        return 1;
    }
    if (take_console_terminal() < 0) return 1;
    if (setenv("SHELL", "/bin/bash", 1) < 0) {
        perror("Set installer shell");
        return 1;
    }
    execl("/bin/bash", "bash", "-l", (char *)0);
    perror("Start installer shell");
    return 1;
}

int main(int argc, char **argv)
{
    int installer_shell = argc == 2 && strcmp(argv[1], "--installer-shell") == 0;
    int getty_login = argc >= 2 && strcmp(argv[1], "--") == 0;
    if (argc != 1 && !installer_shell && !getty_login) {
        fputs("usage: login.elf [--installer-shell]\n", stderr);
        return 2;
    }
    if (installer_shell) {
        if (!isatty(STDIN_FILENO)) {
            fputs("Installer shell requires a TTY\n", stderr);
            return 1;
        }
        return installer_shell_main();
    }
    return tty_login_main();
}
