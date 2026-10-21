#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/fs/locks.c"

static struct task first, second, *current = &first;
static struct task_file file = {.used = 1, .flags = LINUX_O_RDWR,
    .node = {.type = RELIEFOS_FS_TYPE_FILE, .volume_id = 1, .first_cluster = 50}};
static int allocation_fails;
struct task *sched_current_task(void) { return current; }
struct task_file *task_file_for_fd(struct task *task, int fd)
{ assert(task); return fd == 5 ? &file : NULL; }
bool user_range_ok(uint64_t pointer, uint64_t size) { return pointer > 4096 && size == 32; }
bool user_range_writable(uint64_t pointer, uint64_t size) { return user_range_ok(pointer, size); }
void *kernel_malloc(size_t size) { return allocation_fails ? NULL : malloc(size); }
void kernel_free(void *pointer) { free(pointer); }
int file_can_read(const struct task_file *entry) { return (entry->flags & LINUX_O_ACCMODE) != LINUX_O_WRONLY; }
int file_can_write(const struct task_file *entry) { return (entry->flags & LINUX_O_ACCMODE) != LINUX_O_RDONLY; }
int storage_inode_refresh(struct storage_node *node) { node->size = 100; return 0; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue) { (void)queue; return 0; }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ assert(task->waiting_queue == queue); task->waiting_queue = NULL; }
void kernel_wait_queue_block_current(struct kernel_wait_queue *queue) { current->waiting_queue = queue; }
static int request(unsigned command, short type, long start, long size)
{
    struct native_flock lock = {.type = type, .start = start, .length = size};
    return syscall_record_lock(5, command, (uintptr_t)&lock);
}
int main(void)
{
    first.pid = first.tgid = 41;
    second.pid = second.tgid = 42;
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 10, 20) == 0);
    current = &second;
    assert(request(LINUX_F_SETLK, LINUX_F_RDLCK, 20, 1) == -LINUX_EAGAIN);
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 30, 20) == 0);
    current = &first;
    assert(request(LINUX_F_SETLKW, LINUX_F_WRLCK, 30, 1) == KERNEL_SYSCALL_BLOCKED);
    current = &second;
    assert(request(LINUX_F_SETLKW, LINUX_F_WRLCK, 10, 1) == -LINUX_EDEADLK);
    syscall_record_lock_cancel(&first);
    current = &first;
    assert(request(LINUX_F_SETLK, LINUX_F_UNLCK, 15, 5) == 0);
    current = &second;
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 15, 5) == 0);
    allocation_fails = 1;
    assert(request(LINUX_F_SETLK, LINUX_F_UNLCK, 15, 1) == -LINUX_ENOLCK);
    allocation_fails = 0;
    current = &first;
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 15, 1) == -LINUX_EAGAIN);
    syscall_record_locks_close(&second, &file);
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 15, 5) == 0);
    struct native_flock result = {.type = LINUX_F_WRLCK, .start = 20, .length = 1};
    current = &second;
    assert(syscall_record_lock(5, LINUX_F_GETLK, (uintptr_t)&result) == 0);
    assert(result.type == LINUX_F_WRLCK && result.start == 10 && result.length == 20 && result.pid == 41);
    char unaligned[sizeof(result) + 1];
    memcpy(unaligned + 1, &result, sizeof(result));
    assert(syscall_record_lock(5, LINUX_F_GETLK, (uintptr_t)(unaligned + 1)) == 0);
    file.flags = LINUX_O_RDONLY;
    assert(request(LINUX_F_SETLK, LINUX_F_WRLCK, 100, 0) == -LINUX_EBADF);
    file.flags = TASK_FILE_FLAG_PATH;
    assert(request(LINUX_F_GETLK, LINUX_F_RDLCK, 100, 0) == -LINUX_EBADF);
    syscall_record_locks_close(&first, &file);
    puts("PASS production record locks: conflict, split/merge, stable owner, deadlock, close, ENOLCK rollback and unaligned ABI");
}
