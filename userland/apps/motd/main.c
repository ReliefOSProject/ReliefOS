#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "format.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <locale.h>
#include <libintl.h>
#include <reliefos/layout.h>

#define T(text) gettext(text)
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static const char *message_locale(void)
{
    const char *s = getenv("LC_ALL");
    if (!s || !*s) s = getenv("LC_MESSAGES");
    if (!s || !*s) s = getenv("LANG");
    return s ? s : "C";
}

static unsigned fd_columns(int fd)
{
    struct winsize size = {0};
    return fd >= 0 && ioctl(fd, TIOCGWINSZ, &size) == 0 ? size.ws_col : 0;
}

static size_t terminal_columns(void)
{
    unsigned width = fd_columns(STDOUT_FILENO);
    /* pam_exec stdout is a pipe and its child calls setsid(). PAM_TTY is
     * therefore required; /dev/tty alone cannot find the login terminal. */
    const char *tty = getenv("PAM_TTY");
    if (!width && tty && *tty) {
        char name[256];
        int n = snprintf(name, sizeof(name), "%s%s", tty[0] == '/' ? "" : "/dev/", tty);
        if (n > 0 && (size_t)n < sizeof(name) && !strncmp(name, "/dev/", 5)) {
            int fd = open(name, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            width = fd_columns(fd);
            if (fd >= 0) close(fd);
        }
    }
    if (!width) width = fd_columns(STDIN_FILENO);
    if (!width) {
        int fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        width = fd_columns(fd);
        if (fd >= 0) close(fd);
    }
    if (!width) {
        const char *value = getenv("COLUMNS");
        if (value && *value) {
            char *end;
            unsigned long parsed = strtoul(value, &end, 10);
            if (!*end && parsed > 0 && parsed <= 65535) width = (unsigned)parsed;
        }
    }
    if (!width) width = 80; /* Unknown/non-terminal output only. */
    /* Reserve the last cell to avoid a terminal's automatic line wrap. */
    return width > 1 ? width - 1 : 1;
}

static void system_name(char *out, size_t size)
{
    FILE *file = fopen("/etc/os-release", "r");
    if (!file) return;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *value = line + 12;
        value[strcspn(value, "\r\n")] = 0;
        size_t length = strlen(value);
        if (length >= 2 && (*value == '"' || *value == '\'') && value[length - 1] == *value) {
            value[length - 1] = 0; ++value;
        }
        if (*value) snprintf(out, size, "%s", value);
        break;
    }
    fclose(file);
}

static void address(char *out, size_t size, int zh)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq interfaces[64];
    struct ifconf config = {.ifc_len = sizeof(interfaces), .ifc_req = interfaces};
    if (ioctl(fd, SIOCGIFCONF, &config) == 0) {
        snprintf(out, size, "%s", zh ? "未连接" : T("Not connected"));
        size_t count = config.ifc_len > 0 ? (size_t)config.ifc_len / sizeof(struct ifreq) : 0;
        if (count > 64) count = 64;
        for (size_t i = 0; i < count; ++i) {
            struct ifreq flags = interfaces[i];
            if (interfaces[i].ifr_addr.sa_family != AF_INET ||
                ioctl(fd, SIOCGIFFLAGS, &flags) != 0 ||
                !(flags.ifr_flags & IFF_UP) || (flags.ifr_flags & IFF_LOOPBACK)) continue;
            const struct sockaddr_in *in = (const void *)&interfaces[i].ifr_addr;
            if (in->sin_addr.s_addr && inet_ntop(AF_INET, &in->sin_addr, out, size)) break;
        }
    }
    close(fd);
}

static void collect(struct motd_info *info, int zh)
{
    const char *missing = zh ? "不可用" : T("N/A");
    snprintf(info->system, sizeof(info->system), "%s", missing);
    snprintf(info->kernel, sizeof(info->kernel), "%s", missing);
    snprintf(info->timestamp, sizeof(info->timestamp), "%s", missing);
    snprintf(info->uptime, sizeof(info->uptime), "%s", missing);
    snprintf(info->load, sizeof(info->load), "%s", missing);
    snprintf(info->memory, sizeof(info->memory), "%s", missing);
    snprintf(info->root, sizeof(info->root), "%s", missing);
    snprintf(info->address, sizeof(info->address), "%s", missing);
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(info->system, sizeof(info->system), "%s", uts.sysname);
        snprintf(info->kernel, sizeof(info->kernel), "%s %s %s", uts.sysname, uts.release, uts.machine);
    }
    system_name(info->system, sizeof(info->system));
    time_t now = time(NULL);
    struct tm local;
    if (now != (time_t)-1 && localtime_r(&now, &local))
        strftime(info->timestamp, sizeof(info->timestamp), "%Y-%m-%d %H:%M:%S %Z", &local);
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        if (si.uptime >= 0) {
            unsigned long seconds = (unsigned long)si.uptime;
            if (seconds >= 86400)
                snprintf(info->uptime, sizeof(info->uptime), zh ? "%lu天 %lu小时" : T("%lud %luh"), seconds / 86400, seconds / 3600 % 24);
            else if (seconds >= 3600)
                snprintf(info->uptime, sizeof(info->uptime), zh ? "%lu小时 %lu分" : T("%luh %lum"), seconds / 3600, seconds / 60 % 60);
            else snprintf(info->uptime, sizeof(info->uptime), zh ? "%lu分 %lu秒" : T("%lum %lus"), seconds / 60, seconds % 60);
        }
        snprintf(info->load, sizeof(info->load), "%.2f", (double)si.loads[0] / (1UL << SI_LOAD_SHIFT));
        if (si.totalram && si.freeram <= si.totalram)
            snprintf(info->memory, sizeof(info->memory), "%.0f%%", 100.0 * (double)(si.totalram - si.freeram) / (double)si.totalram);
    }
    struct statvfs fs;
    if (statvfs("/", &fs) == 0 && fs.f_blocks >= fs.f_bfree) {
        double used = (double)(fs.f_blocks - fs.f_bfree);
        /* Linux can report a negative available count through this unsigned
         * field when reserved blocks exceed free space. */
        double available = fs.f_bavail <= fs.f_bfree ? (double)fs.f_bavail : 0;
        if (used + available > 0) snprintf(info->root, sizeof(info->root), "%.0f%%", 100.0 * used / (used + available));
    }
    address(info->address, sizeof(info->address), zh);
}

int main(void)
{
    const char *locale = message_locale();
    int zh = !strcmp(locale, "zh") || !strncmp(locale, "zh_", 3) || !strncmp(locale, "zh-", 3);
    int ja = !strcmp(locale, "ja") || !strncmp(locale, "ja_", 3) || !strncmp(locale, "ja-", 3);
    setlocale(LC_ALL, "");
    bindtextdomain("motd", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("motd", "UTF-8");
    textdomain("motd");
    if (MB_CUR_MAX == 1) setlocale(LC_CTYPE, "C.UTF-8");
    struct motd_info info = {0};
    size_t columns = terminal_columns();
    collect(&info, zh);
    motd_render(stdout, &info, zh, columns);
    FILE *links = fopen(zh ? "/etc/motd.zh_CN" : ja ? "/etc/motd.ja_JP" : "/etc/motd", "r");
    if (links) {
        char *line = NULL; size_t capacity = 0;
        while (getline(&line, &capacity, links) >= 0) motd_wrap(stdout, line, columns);
        free(line); fclose(links);
    }
    return ferror(stdout) ? 1 : 0;
}
