/* Ordinary Linux/musl executable: no ReliefOS SDK headers or adapted Fastfetch. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;
static void check(int condition, const char *what)
{
    printf("[inventory] %s %s\n", condition ? "PASS" : "FAIL", what);
    failures += !condition;
}
static int read_file(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t used = 0;
    ssize_t n;
    while (used + 1 < capacity && (n = read(fd, buffer + used, capacity - used - 1)) > 0)
        used += n;
    close(fd);
    if (n < 0 || used + 1 == capacity) return -1;
    buffer[used] = 0;
    return used;
}
static void report(const char *path, const char *label)
{
    char buffer[32768];
    int n = read_file(path, buffer, sizeof(buffer));
    check(n > 0, label);
    if (n > 0) {
        printf("[inventory] BEGIN %s\n", label);
        fflush(stdout);
        write(1, buffer, n);
        printf("\n[inventory] END %s\n", label);
    }
}
static void run_fastfetch(int defaults)
{
    pid_t pid = fork();
    if (!pid) {
        int fd = open(defaults ? "/tmp/fastfetch-default.txt" : "/tmp/fastfetch.json", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, 1) < 0)
            _exit(111);
        close(fd);
        if (defaults) execl("/usr/bin/fastfetch", "fastfetch", NULL);
        execl("/usr/bin/fastfetch", "fastfetch", "--format", "json", "--structure",
              "OS:Host:Kernel:Uptime:CPU:Memory:Swap:Disk:Display:GPU:Shell:Terminal", "--pipe", NULL);
        perror("exec Fastfetch");
        _exit(127);
    }
    int status = 0;
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Fastfetch process exit");
    report(defaults ? "/tmp/fastfetch-default.txt" : "/tmp/fastfetch.json", defaults ? "default-output" : "console-json");
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[inventory] START");
    struct utsname uts;
    if (uname(&uts) == 0) {
        const char *paths[] = {"/proc/sys/kernel/ostype", "/proc/sys/kernel/osrelease",
                               "/proc/sys/kernel/version"};
        const char *fields[] = {uts.sysname, uts.release, uts.version};
        check(!strcmp(uts.sysname, "ReliefNT"), "uname identifies the ReliefNT kernel");
        for (unsigned i = 0; i < 3; ++i) {
            char actual[128], expected[128];
            snprintf(expected, sizeof(expected), "%s\n", fields[i]);
            check(read_file(paths[i], actual, sizeof(actual)) > 0 && !strcmp(actual, expected), paths[i]);
        }
        printf("[inventory] uname release=%s version=%s\n", uts.release, uts.version);
    } else {
        check(0, "uname");
    }
    struct statfs fs;
    check(statfs("/sys", &fs) == 0 && (unsigned long)fs.f_type == 0x62656572 && (fs.f_flags & 1), "sysfs statfs magic");
    check(statfs("/proc", &fs) == 0 && (unsigned long)fs.f_type == 0x9fa0, "procfs statfs magic");
    errno=0;
    int writable=open("/sys/devices/system/cpu/online",O_WRONLY);
    check(writable<0 && errno==EROFS,"sysfs rejects unsupported writes");
    if(writable>=0) close(writable);
    report("/proc/cpuinfo", "cpuinfo");
    report("/proc/self/stat", "stat");
    char command[4096];
    int n = read_file("/proc/self/cmdline", command, sizeof(command));
    check(n > 0 && !strcmp(command, "linux-inventory") && command[n - 1] == 0,
          "actual NUL-terminated argv");
    report("/sys/devices/virtual/dmi/id/product_uuid", "uuid");
    run_fastfetch(0);
    run_fastfetch(1);
    pid_t terminal = fork();
    if (!terminal) {
        execl("/usr/bin/st", "st", "-e", "/bin/sh", "-c",
              "/usr/bin/fastfetch --format json --structure "
              "OS:Host:Kernel:Uptime:CPU:Memory:Swap:Disk:Display:GPU:Shell:Terminal --pipe > "
              "/tmp/fastfetch-terminal.json; echo $? > /tmp/fastfetch-terminal.done",
              NULL);
        perror("exec Terminal");
        _exit(127);
    }
    check(terminal > 0, "launch actual desktop Terminal");
    int ready = 0;
    for (unsigned i = 0; i < 1200; ++i) {
        if (read_file("/tmp/fastfetch-terminal.done", command, sizeof(command)) > 0) {
            ready = 1;
            break;
        }
        usleep(100000);
    }
    check(ready && !strcmp(command, "0\n"), "Fastfetch through Terminal and BusyBox shell");
    if (ready)
        report("/tmp/fastfetch-terminal.json", "terminal-json");
    printf("[inventory] DONE failures=%u\n", failures);
    return failures ? 1 : 0;
}
