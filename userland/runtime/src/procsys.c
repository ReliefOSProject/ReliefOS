/* procfs-backed system/task compatibility exports. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <reliefos/fs.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>
#include <errno.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void ps_copy(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1u < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int ps_read_file(const char *path, char *buffer, uint32_t capacity)
{
    int fd = open(path, RELIEFOS_O_RDONLY, 0);
    uint32_t len = 0;
    if (fd < 0) return -1;
    buffer[0] = 0;
    while (len + 1u < capacity) {
        long got = read(fd, buffer + len, capacity - len - 1u);
        if (got < 0) { close(fd); return -1; }
        if (!got) break;
        len += (uint32_t)got;
    }
    close(fd);
    buffer[len] = 0;
    return 0;
}

static uint64_t ps_parse_number(const char *text, uint32_t *position)
{
    uint64_t value = 0;
    if (!text || !position) return 0;
    while (text[*position] == ' ' || text[*position] == '\t') ++(*position);
    while (text[*position] >= '0' && text[*position] <= '9') {
        value = value * 10u + (uint64_t)(text[*position] - '0');
        ++(*position);
    }
    return value;
}

int reliefos_system_info(struct reliefos_system_info *info)
{
    struct utsname uts;
    if (!info) return -1;
    memset(info, 0, sizeof(*info));
    if (uname(&uts) == 0) {
        ps_copy(info->kernel_name, sizeof(info->kernel_name), uts.sysname);
        ps_copy(info->kernel_version, sizeof(info->kernel_version), uts.release);
        ps_copy(info->architecture, sizeof(info->architecture), uts.machine);
    }
    info->version_major = 4;
    info->version_minor = 0;
    info->version_patch = 0;
    info->copyright_year = 2026;
    return 0;
}

static uint32_t ps_count_proc_tasks(void)
{
    int fd = open("/proc", RELIEFOS_O_RDONLY, 0);
    struct reliefos_dir_entry entry;
    uint32_t count = 0;
    if (fd < 0) return 0;
    while (reliefos_readdir(fd, &entry) > 0) {
        if (entry.name[0] >= '0' && entry.name[0] <= '9') ++count;
    }
    close(fd);
    return count;
}

int reliefos_perf_info(struct reliefos_perf_info *info)
{
    char text[256];
    uint32_t pos = 0;
    if (!info) return -1;
    memset(info, 0, sizeof(*info));
    info->uptime_ms = reliefos_uptime_ms();
    if (ps_read_file("/proc/meminfo", text, sizeof(text)) == 0) {
        /* Minimal parser: first number is MemTotal, second is MemFree. */
        while (text[pos] && (text[pos] < '0' || text[pos] > '9')) ++pos;
        info->total_memory_kib = ps_parse_number(text, &pos);
        while (text[pos] && (text[pos] < '0' || text[pos] > '9')) ++pos;
        info->free_memory_kib = ps_parse_number(text, &pos);
    }
    if (ps_read_file("/proc/stat", text, sizeof(text)) == 0) {
        uint32_t line_pos = 0;
        pos = 0;
        while (text[line_pos] && text[line_pos] != ' ') ++line_pos;
        if (text[line_pos] == ' ') {
            ++line_pos;
            info->busy_ticks = ps_parse_number(text, &line_pos);
            (void)ps_parse_number(text, &line_pos);
            (void)ps_parse_number(text, &line_pos);
            info->idle_ticks = ps_parse_number(text, &line_pos);
        }
        while (text[pos]) {
            uint32_t line_start = pos;
            uint32_t cpu_index = 0;
            uint32_t digits = 0;
            while (text[pos] && text[pos] != '\n') ++pos;
            if (text[line_start] == 'c' && text[line_start + 1u] == 'p' &&
                text[line_start + 2u] == 'u' &&
                text[line_start + 3u] >= '0' && text[line_start + 3u] <= '9') {
                uint32_t cursor = line_start + 3u;
                while (text[cursor] >= '0' && text[cursor] <= '9') {
                    cpu_index = cpu_index * 10u + (uint32_t)(text[cursor] - '0');
                    ++cursor;
                    ++digits;
                }
                if (digits && cpu_index < RELIEFOS_PERF_MAX_CPUS && text[cursor] == ' ') {
                    ++cursor;
                    info->cpus[cpu_index].busy_ticks = ps_parse_number(text, &cursor);
                    (void)ps_parse_number(text, &cursor);
                    (void)ps_parse_number(text, &cursor);
                    info->cpus[cpu_index].idle_ticks = ps_parse_number(text, &cursor);
                    info->cpus[cpu_index].online = 1;
                    if (cpu_index + 1u > info->cpu_count) info->cpu_count = cpu_index + 1u;
                }
            }
            if (text[pos] == '\n') ++pos;
        }
    }
    if (!info->cpu_count) {
        info->cpu_count = 1;
        info->cpus[0].busy_ticks = info->busy_ticks;
        info->cpus[0].idle_ticks = info->idle_ticks;
        info->cpus[0].online = 1;
    }
    info->online_cpu_count = info->cpu_count;
    info->task_count = ps_count_proc_tasks();
    info->running_tasks = 0;
    info->ready_tasks = 0;
    info->sleeping_tasks = 0;
    return 0;
}

int reliefos_time_info(struct reliefos_time_info *info)
{
    struct timeval tv;
    struct tm calendar;
    if (!info) { errno = EINVAL; return -1; }
    memset(info, 0, sizeof(*info));
    info->uptime_ms = reliefos_uptime_ms();
    if (gettimeofday(&tv, 0) < 0) return -1;
    if (tv.tv_sec < 0) { errno = EOVERFLOW; return -1; }
    if (!gmtime_r(&tv.tv_sec, &calendar)) return -1;
    info->unix_seconds = (uint64_t)tv.tv_sec;
    info->year = (uint32_t)calendar.tm_year + 1900u;
    info->month = (uint32_t)calendar.tm_mon + 1u;
    info->day = (uint32_t)calendar.tm_mday;
    info->hour = (uint32_t)calendar.tm_hour;
    info->minute = (uint32_t)calendar.tm_min;
    info->second = (uint32_t)calendar.tm_sec;
    info->valid = 1;
    return 0;
}

int reliefos_machine_identity(struct reliefos_machine_identity *identity)
{
    char id[40];
    if (!identity) { errno = EINVAL; return -1; }
    memset(identity, 0, sizeof(*identity));
    identity->version = 1;
    ps_copy(identity->source, sizeof(identity->source), "sysfs-dmi");
    if (ps_read_file("/sys/class/dmi/id/product_uuid", id, sizeof(id)) < 0) return -1;
    id[strcspn(id, "\r\n")] = 0;
    if (strlen(id) != 36) { errno = EIO; return -1; }
    ps_copy(identity->platform_uuid, sizeof(identity->platform_uuid), id);
    identity->flags |= RELIEFOS_MACHINE_IDENTITY_FLAG_PLATFORM_UUID;
    return 0;
}

/* Linux stat's comm may contain spaces and closing parentheses. The final
 * closing parenthesis delimits it; token positions after state are fixed. */
static int ps_parse_stat(const char *path, struct reliefos_task_info *task)
{
    char text[1024];
    uint32_t pos = 0;
    if (ps_read_file(path, text, sizeof(text)) < 0) return -1;
    const char *start = strchr(text, '('), *end = strrchr(text, ')');
    if (!start || !end || end <= start || end[1] != ' ' || !end[2] || end[3] != ' ') return -1;
    memset(task, 0, sizeof(*task));
    task->uid = UINT32_MAX; /* status may disappear during process exit */
    task->pid = (uint32_t)ps_parse_number(text, &pos);
    size_t len = (size_t)(end - start - 1);
    if (len >= sizeof(task->name)) len = sizeof(task->name) - 1;
    memcpy(task->name, start + 1, len);
    task->state = end[2] == 'Z' || end[2] == 'X' ? 3 :
                  end[2] == 'T' || end[2] == 't' ? 4 : end[2] == 'R' ? 1 : 2;
    pos = (uint32_t)(end + 4 - text);
    for (unsigned field = 4; field <= 15; ++field) {
        while (text[pos] == ' ') ++pos;
        if (!text[pos] || text[pos] == '\n') return -1;
        uint32_t number = pos;
        if (field == 4) task->parent_pid = (uint32_t)ps_parse_number(text, &number);
        if (field == 14 || field == 15) task->cpu_ticks += ps_parse_number(text, &number);
        while (text[pos] && text[pos] != ' ' && text[pos] != '\n') ++pos;
    }
    task->kind = 1;
    return 0;
}

int reliefos_task_snapshot(struct reliefos_task_info *tasks, uint32_t capacity,
                         uint64_t *tick)
{
    int fd;
    struct reliefos_dir_entry entry;
    uint32_t count = 0;
    if (tick) *tick = reliefos_uptime_ms();
    if (!tasks || !capacity) return 0;
    fd = open("/proc", RELIEFOS_O_RDONLY, 0);
    if (fd < 0) return -1;
    while (reliefos_readdir(fd, &entry) > 0 && count < capacity) {
        char path[RELIEFOS_FS_PATH_LEN];
        if (entry.name[0] < '0' || entry.name[0] > '9') continue;
        {
            uint32_t pos = 0;
            ps_copy(path, sizeof(path), "/proc/");
            while (path[pos]) ++pos;
            ps_copy(path + pos, sizeof(path) - pos, entry.name);
            pos += (uint32_t)strlen(entry.name);
            ps_copy(path + pos, sizeof(path) - pos, "/stat");
        }
        if (ps_parse_stat(path, &tasks[count]) == 0) {
            char status[512], password_buffer[1024];
            struct passwd record, *account = NULL;
            /* Identity and ReliefOS-specific metadata are named status fields,
             * never extra columns in the Linux stat ABI. */
            char *suffix = strrchr(path, '/');
            ps_copy(suffix + 1, (uint32_t)(sizeof(path) - (suffix + 1 - path)), "status");
            if (ps_read_file(path, status, sizeof(status)) == 0) {
                const char *value = strstr(status, "\nVmRSS:\t");
                if (value) {
                    uint32_t pos = 8;
                    tasks[count].memory_kib = (uint32_t)ps_parse_number(value, &pos);
                }
                value = strstr(status, "\nUid:\t");
                if (value) {
                    uint32_t pos = 6;
                    tasks[count].uid = (uint32_t)ps_parse_number(value, &pos);
                }
                const char *keys[] = {"\nLeonOSRole:\t", "\nLeonOSFlags:\t"};
                uint32_t *fields[] = {&tasks[count].role, &tasks[count].flags};
                for (unsigned i = 0; i < 2; ++i) {
                    const char *value = strstr(status, keys[i]);
                    if (value) { uint32_t pos = (uint32_t)strlen(keys[i]); *fields[i] = (uint32_t)ps_parse_number(value, &pos); }
                }
            }
            if (tasks[count].uid != UINT32_MAX && getpwuid_r(tasks[count].uid, &record, password_buffer,
                           sizeof(password_buffer), &account) == 0 && account && account->pw_name)
                ps_copy(tasks[count].username, sizeof(tasks[count].username), account->pw_name);
            ++count;
        }
    }
    close(fd);
    return (int)count;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_machine_identity) leonos_machine_identity __attribute__((alias("reliefos_machine_identity")));
extern __typeof__(reliefos_perf_info) leonos_perf_info __attribute__((alias("reliefos_perf_info")));
extern __typeof__(reliefos_system_info) leonos_system_info __attribute__((alias("reliefos_system_info")));
extern __typeof__(reliefos_task_snapshot) leonos_task_snapshot __attribute__((alias("reliefos_task_snapshot")));
extern __typeof__(reliefos_time_info) leonos_time_info __attribute__((alias("reliefos_time_info")));

unsigned long reliefos_uptime_ms(void)
{
    struct timespec ts;
    if (clock_gettime(1, &ts) < 0) return 0;
    return (unsigned long)((uint64_t)ts.tv_sec * 1000ULL +
                           (uint64_t)ts.tv_nsec / 1000000ULL);
}

int reliefos_task_affinity_get(uint32_t pid, uint64_t *mask)
{
    return sched_getaffinity((pid_t)pid, sizeof(*mask), (cpu_set_t *)mask);
}

int reliefos_task_affinity_set(uint32_t pid, uint64_t mask)
{
    return sched_setaffinity((pid_t)pid, sizeof(mask), (const cpu_set_t *)&mask);
}

int reliefos_task_kill(uint32_t pid)
{
    return kill((pid_t)pid, SIGTERM);
}
extern __typeof__(reliefos_uptime_ms) leonos_uptime_ms __attribute__((alias("reliefos_uptime_ms")));
extern __typeof__(reliefos_task_affinity_get) leonos_task_affinity_get __attribute__((alias("reliefos_task_affinity_get")));
extern __typeof__(reliefos_task_affinity_set) leonos_task_affinity_set __attribute__((alias("reliefos_task_affinity_set")));
extern __typeof__(reliefos_task_kill) leonos_task_kill __attribute__((alias("reliefos_task_kill")));
