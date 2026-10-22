#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/fs/filesystem.c"

void copy_text(char *dst, uint32_t cap, const char *src) { snprintf(dst, cap, "%s", src); }
static struct task_file file;
static unsigned lookups, updates;
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd == 3 ? &file : NULL; }
int resolve_user_path_at_flags(struct task *task, int32_t dirfd, uint64_t ptr, bool flags,
                              char *path, struct task_file **empty, bool create, uint32_t lookup)
{
    (void)task; (void)dirfd; (void)flags; (void)empty; (void)create; (void)lookup;
    ++lookups;
    if (!ptr) return -RELIEFOS_EFAULT;
    strcpy(path, (const char *)(uintptr_t)ptr);
    return 0;
}
int storage_lookup_path(const char *path, struct storage_node *node)
{ (void)path; ++lookups; *node = file.node; return 0; }
int storage_inode_refresh(struct storage_node *node) { (void)node; return 0; }
int fs_permissions_get(const char *path, const struct storage_node *node, struct reliefos_permissions *perms)
{ (void)path; (void)node; *perms = (struct reliefos_permissions){0600, 1000, 1000}; return 0; }
int fs_permissions_check(const struct task *task, const char *path, uint32_t access, bool elevate)
{ (void)task; (void)path; (void)access; (void)elevate; return -RELIEFOS_EACCES; }
int fs_permissions_check_node(const struct task *task, const char *path, const struct storage_node *node, uint32_t access, bool elevate)
{ (void)task; (void)path; (void)node; (void)access; (void)elevate; return -RELIEFOS_EACCES; }
int time_wall_clock(struct reliefos_time_info *info) { info->unix_seconds = 1700000000; return 0; }
int storage_inode_utimensat(const struct storage_node *node, int64_t atime, int64_t mtime, bool a, bool m)
{ assert(!memcmp(node, &file.node, sizeof(*node))); assert(atime == 123 && mtime == 456 && a && m); ++updates; return 0; }

int main(void)
{
    struct task task = {.euid = 1000};
    strcpy(file.path, "/renamed-or-unlinked-file");
    struct linux_timespec times[2] = {{123, 0}, {456, 0}};
    assert(!syscall_update_path_times(&task, 3, 0, times, 0, true));
    assert(updates == 1 && !lookups);
    assert(syscall_update_path_times(&task, 3, 0, times, LINUX_AT_SYMLINK_NOFOLLOW, true) == -RELIEFOS_EINVAL);
    assert(syscall_update_path_times(&task, 4, 0, times, 0, true) == -RELIEFOS_EBADF);
    file.flags = TASK_FILE_FLAG_PATH;
    assert(syscall_update_path_times(&task, 3, 0, times, 0, true) == -RELIEFOS_EBADF);
    file.flags = 0;
    task.euid = 2000;
    assert(syscall_update_path_times(&task, 3, 0, times, 0, true) == -RELIEFOS_EPERM);
    assert(updates == 1 && !lookups);
    assert(syscall_update_path_times(&task, LINUX_AT_FDCWD, 0, times, 0, true) == -RELIEFOS_EFAULT);
    puts("PASS futimens: fd identity, no pathname lookup, flags, O_PATH, invalid fd and ownership");
}
