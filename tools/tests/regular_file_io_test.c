#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/io/file_io.c"

static unsigned char disk[1048576], data[1048576];
static uint64_t disk_size = sizeof(disk), error_offset = UINT64_MAX;
static int injected_error;
static uint64_t clock_step, clock_us;
static unsigned disk_syncs, volume_syncs;
static int sync_error;
int storage_sync_disk(uint32_t id) { (void)id; ++disk_syncs; return sync_error; }
int storage_sync_volume(uint32_t id) { (void)id; ++volume_syncs; return sync_error; }
uint64_t time_uptime_us(void) { clock_us += clock_step; return clock_us; }
void storage_release_task_io(uint32_t pid) { assert(pid == 10); }
static int transfer(uint64_t offset, void *buffer, uint32_t length, uint32_t *done, bool writing)
{
    *done = 0;
    if (offset == error_offset) {
        int ret = injected_error;
        error_offset = UINT64_MAX;
        return ret;
    }
    if (offset >= disk_size)
        return 0;
    if (length > disk_size - offset)
        length = disk_size - offset;
    if (writing)
        memcpy(disk + offset, buffer, length);
    else
        memcpy(buffer, disk + offset, length);
    *done = length;
    return 0;
}
int storage_read_node_cursor(const struct storage_node *node, uint64_t offset, void *buffer, uint32_t length,
                             uint32_t *done, struct storage_read_cursor *cursor)
{
    (void)node;
    (void)cursor;
    return transfer(offset, buffer, length, done, false);
}
int storage_write_held_node(struct storage_node *node, uint64_t offset, const void *buffer, uint32_t length,
                            uint32_t *done)
{
    (void)node;
    return transfer(offset, (void *)buffer, length, done, true);
}
int storage_write_node(const char *path, uint64_t offset, const void *buffer, uint32_t length, uint32_t *done)
{
    (void)path;
    return transfer(offset, (void *)buffer, length, done, true);
}
int storage_inode_refresh(struct storage_node *node)
{
    node->size = disk_size;
    return 0;
}
int storage_disk_block_read(uint32_t disk_id, int32_t part, uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *done)
{
    (void)disk_id;
    (void)part;
    return transfer(offset, buffer, length, done, false);
}
int storage_disk_block_write(uint32_t disk_id, int32_t part, uint64_t offset, const void *buffer,
                             uint32_t length, uint32_t *done)
{
    (void)disk_id;
    (void)part;
    return transfer(offset, (void *)buffer, length, done, true);
}
int storage_lookup_path(const char *path, struct storage_node *node)
{
    (void)path;
    (void)node;
    return -2;
}

int main(void)
{
    struct task *task = calloc(1, sizeof(*task)), *other = calloc(1, sizeof(*other));
    struct task_file file = {.used = 1, .flags = RELIEFOS_O_RDWR, .node.first_cluster = 2};
    assert(task && other);
    task->pid = 10;
    other->pid = 11;
    task->syscall_file = other->syscall_file = &file;
    task->syscall_file_number = other->syscall_file_number = LINUX_SYS_READ;
    for (unsigned i = 0; i < sizeof(disk); ++i)
        disk[i] = i * 13 + i / 257;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(data)) && file.offset == sizeof(data));
    file.offset = 0;
    memset(data, 0, sizeof(data));
    error_offset = 65536;
    injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == -LINUX_EAGAIN);
    assert(file.offset == 65536 && task->regular_io.done == 65536 && file.io_owner == 10);
    assert(syscall_regular_io(other, &file, (uintptr_t)data, 1, 0, false, false) == -LINUX_EAGAIN);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(data)) && !file.io_owner);
    file.offset = 0;
    error_offset = 98304;
    injected_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == 98304);
    assert(file.offset == 98304 && !file.io_owner);
    error_offset = file.offset;
    injected_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 10, 0, false, false) == -LINUX_EIO);
    file.offset = disk_size - 17;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 100, 0, false, false) == 17);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 100, 0, false, false) == 0);
    file.offset = 123;
    task->syscall_file_number = LINUX_SYS_PWRITE64;
    memset(data, 0xa5, sizeof(data));
    error_offset = 32768;
    injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true) == -LINUX_EAGAIN);
    assert(file.offset == 123);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(disk)) && file.offset == 123);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, INT64_MAX, true, true) == -LINUX_EINVAL);
    clock_step = 2000;
    unsigned yields = 0;
    int64_t result;
    do {
        result = syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true);
        if (result == -LINUX_EAGAIN) ++yields;
    } while (result == -LINUX_EAGAIN && yields < 40);
    assert(result == sizeof(data) && yields == 31);
    assert(!memcmp(data, disk, sizeof(data)) && file.offset == 123);
    assert(!task->regular_io.active);
    clock_step = 0;
    task->syscall_file_number = LINUX_SYS_PREAD64;
    file.flags |= TASK_FILE_FLAG_DEV_BLOCK | TASK_FILE_FLAG_DEV_NODE;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, 510, false, true) == -LINUX_EBADF);
    file.node.flags = STORAGE_NODE_FLAG_DEV_BLOCK | STORAGE_NODE_FLAG_DEV_NODE;
    file.node.first_cluster = STORAGE_DEV_KIND_DISK;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, 510, false, true) == 2);
    assert(!memcmp(data, disk + 510, 2));
    task->syscall_file_number = LINUX_SYS_PWRITE64;
    file.flags |= LINUX_O_SYNC;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == 512);
    assert(disk_syncs == 1 && volume_syncs == 0);
    file.flags &= ~(TASK_FILE_FLAG_DEV_BLOCK | TASK_FILE_FLAG_DEV_NODE);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == 512);
    assert(disk_syncs == 1 && volume_syncs == 1);
    sync_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == -LINUX_EIO);
    free(task);
    free(other);
    puts("PASS full file transfers, async continuation, shared offsets, EOF, error progress and genuine block-node validation");
}
