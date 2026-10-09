/* Narrow command adapter. sudo authenticates callers; OpenRC owns all state. */
#define _GNU_SOURCE
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
    if (argc != 3 || !valid_service(argv[1])) return 2;
    const char *action = argv[2];
    int query = !strcmp(action, "status");
    int enable = !strcmp(action, "enable"), disable = !strcmp(action, "disable");
    if (!query && !enable && !disable && strcmp(action, "start") &&
        strcmp(action, "stop") && strcmp(action, "restart")) return 2;
    if (!query && (getuid() || geteuid())) { errno = EPERM; perror("OpenRC operation"); return 1; }
    if (clearenv() < 0 || setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) < 0 ||
        setenv("HOME", "/root", 1) < 0 || setenv("RC_NOCOLOR", "YES", 1) < 0 || chdir("/") < 0) return 1;
    umask(022);
    if (enable || disable)
        execl("/sbin/rc-update", "rc-update", enable ? "add" : "del", argv[1], "default", (char *)NULL);
    else
        execl("/sbin/rc-service", "rc-service", argv[1], action, (char *)NULL);
    perror("Execute OpenRC");
    return 127;
}
