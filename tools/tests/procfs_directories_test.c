#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/include/reliefnt/audio.h"
int audio_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity)
{ (void)index; (void)out_id; (void)identity; return -2; }
#include "../../kernel/reliefnt/fs/procfs.c"

static struct task current = {.pid = 42, .name = "test", .uid = 1000};
static struct task foreign = {.pid = 43, .name = "foreign", .uid = 2000};
static bool expose_foreign;
struct task_pty_fd *task_pty_fd_for_fd(struct task *t, int fd)
{ for (unsigned i = 0; i < SCHED_TASK_PTY_FD_MAX; ++i)
    if (t->pty_fds[i].used && t->pty_fds[i].fd == fd) return &t->pty_fds[i];
  return NULL; }
struct task_file *task_file_for_fd(struct task *t, int fd) { (void)t; (void)fd; return NULL; }
uint32_t pty_vt_number(uint32_t id) { return id >= 1 && id <= 6 ? id : 0; }
uint32_t address_space_user_resident_kib(const struct address_space *as)
{ assert(as == sched_task_as(&current)); return 12; }
struct task *sched_current_task(void) { return &current; }
uint32_t sched_current_pid(void) { return current.pid; }
struct task *sched_find(uint32_t pid) { return pid == current.pid ? &current : expose_foreign && pid == foreign.pid ? &foreign : NULL; }
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick)
{
    if (tick) *tick = 123;
    if (out && capacity) out[0] = (struct task_snapshot_info){.pid = current.pid};
    return 1;
}
uint64_t time_uptime_ms(void) { return 1234; }
uint64_t mm_total_memory_kib(void) { return 1024; }
uint64_t mm_free_memory_kib(void) { return 512; }
#ifndef TEST_REAL_INVENTORY
uint32_t smp_cpu_count(void) { return 1; }
#endif
void sched_cpu_ticks(uint64_t *busy, uint64_t *idle) { *busy = 10; *idle = 20; }
void sched_cpu_ticks_per_cpu(uint64_t *busy, uint64_t *idle, uint32_t capacity)
{ if (capacity) sched_cpu_ticks(busy, idle); }
/* All release interfaces must use this record, including its suffix. */
static const struct reliefos_system_info fixture_system = {
    .kernel_name = "ReliefNT", .kernel_version = "9.8.7-0123",
    .build_time = "2026-09-11 01:02:03",
};
const struct reliefos_system_info *reliefnt_system_info(void) { return &fixture_system; }

const char *userland_boot_cmdline(void) { return ""; }
void linux_uts_names(char host[65], char domain[65])
{ strcpy(host, "fixture-host"); strcpy(domain, "(none)"); }
int storage_read_mounts(uint64_t offset, void *buffer, uint32_t capacity, uint32_t *out)
{ (void)offset; (void)buffer; (void)capacity; *out = 0; return 0; }
int storage_read_mountinfo(uint64_t offset, void *buffer, uint32_t capacity, uint32_t *out)
{ (void)offset; assert(capacity); ((char *)buffer)[0] = '1'; *out = 1; return 0; }

#ifndef TEST_REAL_INVENTORY
int cpu_inventory_read(uint64_t o,void *b,uint32_t n,uint32_t *r) { (void)o;(void)b;(void)n;*r=0;return 0; }
int sysfs_lookup(const char *p,struct storage_node *n) { (void)p;(void)n;return -2; }
int sysfs_read(const char *p,uint64_t o,void *b,uint32_t n,uint32_t *r) { (void)p;(void)o;(void)b;(void)n;*r=0;return -2; }
int sysfs_readlink(const char *p,char *b,uint32_t n) { (void)p;(void)b;(void)n;return -2; }
int sysfs_readdir(const char *p,uint64_t *o,struct reliefos_dir_entry *e) { (void)p;(void)o;(void)e;return -2; }
#endif
struct task_vma *sched_task_vma_at(struct task *t,uint32_t i) { return i<SCHED_TASK_VMA_MAX? &sched_task_mm(t)->vmas[i]:NULL; }
int pty_get_foreground_pgid(uint32_t p,uint32_t *g) { (void)p;*g=0;return -2; }
int driver_manager_list(struct reliefos_driver_list *query) { query->count = 0; return 0; }
static char argument_pages[8192];
bool address_space_user_page_readable(const struct address_space *a,uint64_t v) { (void)a;return v>=0x400000 && v<0x402000; }
uint64_t address_space_user_page_phys(const struct address_space *a,uint64_t v) { (void)a;return (v&~4095ULL)-0x400000+0x1000; }
void *paging_kernel_direct_map(uint64_t p) { return p>=0x1000 && p<0x3000?argument_pages+p-0x1000:NULL; }

int main(void)
{
    strcpy(current.path, "/usr/lib/reliefos/apps/procsys/procsys.elf");
    struct storage_node node;
    assert(proc_lookup("/proc/42", &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_lookup("/proc/self", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_lookup("/proc/43", &node) == -2);
    assert(proc_lookup("/proc/4294967338", &node) == -2);
    assert(proc_lookup("/proc/42unknown/stat", &node) == -2);
    assert(proc_lookup("/proc/42/stat", &node) == 0 && node.type == RELIEFOS_FS_TYPE_FILE);
    char contents[512];
    uint32_t got;
    assert(proc_read("/proc/sys/kernel/osrelease", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "9.8.7-0123\n"));
    assert(proc_read("/proc/version", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "ReliefNT version 9.8.7-0123 (2026-09-11 01:02:03)\n"));
    assert(proc_read("/proc/42/stat", 0, contents, sizeof(contents) - 1, &got) == 0 && got);
    contents[got] = 0;
    assert(strstr(contents, "42 (test)"));
    assert(proc_lookup("/proc/42/status", &node) == 0 && node.type == RELIEFOS_FS_TYPE_FILE);
    char link[64] = {0};
    assert(proc_readlink("/proc/self", link, sizeof(link)) == 2);
    assert(!memcmp(link, "42", 2));
    memset(link, 0, sizeof(link));
    assert(proc_readlink("/proc/42/exe", link, sizeof(link)) == (int)strlen(current.path));
    assert(!memcmp(link, current.path, strlen(current.path)));
    assert(proc_readlink("/proc/42/stat", link, sizeof(link)) == -22);
    current.pty_fds[0] = (struct task_pty_fd){.used = 1, .fd = 0,
        .pty_id = 2, .endpoint = TASK_PTY_ENDPOINT_SLAVE};
    assert(proc_readlink("/proc/self/fd/0", link, sizeof(link)) == 9);
    assert(!memcmp(link, "/dev/tty2", 9));
    assert(proc_lookup("/proc/42/fd", &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_lookup("/proc/42/fd/0", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_readlink("/proc/self/fd/99", link, sizeof(link)) == -2);
    assert(proc_readlink("/proc/self/fd/4294967296", link, sizeof(link)) == -2);
    current.euid = current.suid = current.fsuid = 1001;
    current.as.cr3 = 4096;
    current.gid = 4000000000U; current.egid = current.sgid = current.fsgid = 4000000001U;
    assert(proc_read("/proc/42/status", 0, contents, sizeof(contents) - 1, &got) == 0 && got);
    contents[got] = 0;
    assert(strstr(contents, "Name:\ttest\n"));
    assert(strstr(contents, "Uid:\t1000\t1001\t1001\t1001\n"));
    assert(strstr(contents, "Gid:\t4000000000\t4000000001\t4000000001\t4000000001\n"));
    assert(strstr(contents, "VmRSS:\t12 kB\n"));
    sched_task_mm(&current)->env_start = 0x400ffc;
    sched_task_mm(&current)->env_end = 0x401008;
    memcpy(argument_pages + 4092, "VAR=a\0X=two\0", 12);
    assert(proc_lookup("/proc/self/environ", &node) == 0);
    assert(proc_read("/proc/self/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    assert(!memcmp(contents, "VAR=a\0X=two\0", 12));
    argument_pages[4096] = 'b';
    assert(proc_read("/proc/self/environ", 4, contents, 3, &got) == 0 && got == 3);
    assert(!memcmp(contents, "b\0X", 3));
    assert(proc_read("/proc/self/environ", 12, contents, sizeof(contents), &got) == 0 && got == 0);
    expose_foreign = true;
    sched_task_mm(&foreign)->as.cr3 = 4096;
    sched_task_mm(&foreign)->env_start = sched_task_mm(&current)->env_start;
    sched_task_mm(&foreign)->env_end = sched_task_mm(&current)->env_end;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == -RELIEFOS_EACCES && got == 0);
    foreign.uid = foreign.euid = foreign.suid = current.fsuid;
    foreign.gid = foreign.egid = foreign.sgid = current.fsgid;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    sched_task_mm(&foreign)->nondumpable = true;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == -RELIEFOS_EACCES && got == 0);
    current.cap_effective |= 1ULL << CAP_SYS_PTRACE;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    current.cap_effective = 0;
    expose_foreign = false;
    uint64_t offset = 0;
    struct reliefos_dir_entry entry;
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "stat"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "cmdline"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "status"));
    const char *extra[] = {"mounts", "mountinfo", "exe", "cwd", "root"};
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, extra[i]));
        assert(entry.type == (i >= 2 ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_FILE));
    }
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "comm"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "environ"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "fd") && entry.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_readdir("/proc/42", &offset, &entry) == 0);
    offset = 0;
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 1 && !strcmp(entry.name, "0"));
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 0);
    assert(proc_lookup("/proc/mounts", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    memset(link, '#', sizeof(link));
    assert(proc_readlink("/proc/mounts", link, 4) == 4 && !memcmp(link, "self", 4) && link[4] == '#');
    strcpy(sched_task_cwd(&current), "/home/test");
    assert(proc_readlink("/proc/self/cwd", link, sizeof(link)) == 10 && !memcmp(link, "/home/test", 10));
    assert(proc_readlink("/proc/self/root", link, sizeof(link)) == 1 && link[0] == '/');
    assert(proc_read("/proc/self/mountinfo", 0, contents, sizeof(contents), &got) == 0 && got == 1);
    assert(proc_read("/proc/filesystems", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    /* tmpfs now has a real mount backend, also exercised by the FIFO VM probe.
     * ext4 is listed because the ext-family backend serves both revisions. */
    assert(strstr(contents, "\text2\n") && strstr(contents, "\text4\n") &&
           strstr(contents, "\ttmpfs\n"));
    assert(proc_read("/proc/sys/kernel/hostname", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "fixture-host\n"));
    offset = 0;
    int found = 0;
    while (proc_readdir("/proc", &offset, &entry) > 0) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s", entry.name);
        assert(proc_lookup(path, &node) == 0 && entry.type == node.type);
        if (!strcmp(entry.name, "42")) found = 1;
    }
    assert(found);
    puts("PASS procfs task directory lookup, traversal, enumeration and overflow rejection");
}
