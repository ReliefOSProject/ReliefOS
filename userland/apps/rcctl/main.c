/* Narrow command adapter. sudo authenticates callers; OpenRC owns all state. */
#define _GNU_SOURCE
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>

#define T(text) gettext(text)

static void init_localization(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("rcctl", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("rcctl", "UTF-8");
    textdomain("rcctl");
}

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int valid_service(const char *name)
{
    static const char *const allowed[] = {"reliefos-audio", "reliefos-device",
        "reliefos-dhcp", "reliefos-ntp"};
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
        if (!strcmp(name, allowed[i])) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    init_localization();
    if (argc != 3 || !valid_service(argv[1])) return 2;
    const char *action = argv[2];
    int query = !strcmp(action, "status");
    const char *execute_error = T("Execute OpenRC");
    int enable = !strcmp(action, "enable"), disable = !strcmp(action, "disable");
    if (!query && !enable && !disable && strcmp(action, "start") &&
        strcmp(action, "stop") && strcmp(action, "restart")) return 2;
    if (!query && (getuid() || geteuid())) { errno = EPERM; perror(T("OpenRC operation")); return 1; }
    if (clearenv() < 0 || setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) < 0 ||
        setenv("HOME", "/root", 1) < 0 || setenv("RC_NOCOLOR", "YES", 1) < 0 || chdir("/") < 0) return 1;
    umask(022);
    if (enable || disable)
        execl("/sbin/rc-update", "rc-update", enable ? "add" : "del", argv[1], "default", (char *)NULL);
    else
        execl("/sbin/rc-service", "rc-service", argv[1], action, (char *)NULL);
    perror(execute_error);
    return 127;
}
