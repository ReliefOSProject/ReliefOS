#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/block/core/storage_internal.h"
#include "../../kernel/reliefnt/block/core/storage_state_data.c"

static FILE *disk;
static struct task *writer;
int time_wall_clock(struct reliefos_time_info *value)
{ value->unix_seconds = 1800000000; return 0; }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
void mm_free_page(uint64_t p) { free((void *)(uintptr_t)p); }
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{ (void)node; (void)size; abort(); }
struct task *sched_current_task(void) { return writer; }
bool task_in_group(const struct task *task, uint32_t gid, bool real)
{ assert(task && !real); return task->fsgid == gid; }
uint64_t mm_alloc_pages(uint32_t pages)
{ return (uint64_t)(uintptr_t)calloc(pages, 4096); }
static uint64_t test_start_lba = 1;
static uint64_t test_read_commands, test_write_commands, test_read_bytes, test_write_bytes;
static unsigned test_fail_write_at, test_partial_write;
int storage_read_sectors(uint64_t lba, uint32_t count, void *out)
{
    ++test_read_commands;
    test_read_bytes += (uint64_t)count * 512;
    assert(lba >= test_start_lba && !fseek(disk, (lba - test_start_lba) * 512, SEEK_SET));
    return fread(out, 512, count, disk) == count ? 0 : -5;
}
int storage_write_sectors(uint64_t lba, uint32_t count, const void *in)
{
    ++test_write_commands;
    test_write_bytes += (uint64_t)count * 512;
    ext2_cache_invalidate_range(lba, count);
    /* The real storage_block.c write invalidates the shared sector cache. */
    storage_read_cache.valid = 0;
    assert(lba >= test_start_lba && !fseek(disk, (lba - test_start_lba) * 512, SEEK_SET));
    if (test_fail_write_at && !--test_fail_write_at) {
        if (test_partial_write) {
            assert(fwrite(in, 1, 512, disk) == 512 && !fflush(disk));
        }
        return -5;
    }
    return fwrite(in, 512, count, disk) == count && !fflush(disk) ? 0 : -5;
}
void storage_memzero(void *out, size_t n) { memset(out, 0, n); }
void storage_memcpy(void *out, const void *in, size_t n) { memcpy(out, in, n); }
size_t storage_strlen(const char *text) { return strlen(text); }
int storage_text_eq_ci(const char *a, const char *b) { return !strcasecmp(a, b); }
int storage_text_eq(const char *a, const char *b) { return !strcmp(a, b); }
void storage_copy_text(char *out, uint32_t cap, const char *text) { snprintf(out, cap, "%s", text); }
void storage_cache_invalidate(void) { storage_read_cache.valid = 0; }
void storage_begin_mutation(void) {}
int storage_dir_index_lookup(uint32_t directory, const char *name, struct storage_node *node)
{ (void)directory; (void)name; (void)node; return 0; }
void storage_dir_index_store(uint32_t directory, const char *name, const struct storage_node *node)
{ (void)directory; (void)name; (void)node; }
uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
int storage_parent_path(const char *path, char *parent, uint32_t parent_cap, char *name, uint32_t name_cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1]) return -22;
    snprintf(name, name_cap, "%s", slash + 1);
    if (slash == path) snprintf(parent, parent_cap, "/");
    else snprintf(parent, parent_cap, "%.*s", (int)(slash - path), path);
    return 0;
}
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
void page_cache_invalidate_node(const struct storage_node *node)
{ assert(node && (node->flags & STORAGE_NODE_FLAG_EXT2)); }
int storage_select_node_volume(const struct storage_node *node, struct storage_volume **previous)
{ *previous = g_active_volume; return node->volume_id == g_storage.volume_id ? 0 : -2; }
void storage_restore_volume(struct storage_volume *previous) { g_active_volume = previous; }

#include "../../kernel/reliefnt/fs/ext2/ext2_cache.c"
int storage_read_device(const struct storage_volume *v,uint64_t lba,uint32_t n,void *out)
{ (void)v; return storage_read_sectors(lba,n,out); }
int storage_write_device(const struct storage_volume *v,uint64_t lba,uint32_t n,const void *in)
{ storage_ext4_cache_invalidate_write(v,lba,n); return storage_write_sectors(lba,n,in); }
int storage_ext4_device_flush(const struct storage_volume *v) { (void)v; return fflush(disk)?-5:0; }
#include "../../kernel/reliefnt/fs/ext4/format.c"
#include "../../kernel/reliefnt/fs/ext4/checksum.c"
#include "../../kernel/reliefnt/fs/ext4/cache.c"
#include "../../kernel/reliefnt/fs/ext4/alloc.c"
#include "../../kernel/reliefnt/fs/ext4/extent.c"
#include "../../kernel/reliefnt/fs/ext4/ops.c"
#include "../../kernel/reliefnt/fs/ext4/xattr.c"
#include "../../kernel/reliefnt/fs/ext4/journal.c"
#include "../../kernel/reliefnt/fs/ext4/dir.c"
#include "../../kernel/reliefnt/fs/ext4/mount.c"
#include "../../kernel/reliefnt/fs/vfs/inode.c"
#include "../../kernel/reliefnt/fs/ext4/vfs.c"
#include "../../kernel/reliefnt/fs/ext2/ext2.c"

static void test_held_inode(void)
{
    struct storage_node node, replacement;
    struct storage_inode_ref *held;
    struct ext2_inode inode;
    char data[4];
    uint32_t got;
    assert(ext2_write_file("/held-inode", "old", 3) == 0);
    assert(ext2_lookup_path("/held-inode", &node) == 0);
    assert(storage_inode_get(&node, &held) == 0 && held);
    storage_inode_retain(held);
    assert(ext2_unlink("/held-inode") == 0);
    assert(ext2_write_file("/held-inode", "new", 3) == 0);
    assert(ext2_lookup_path("/held-inode", &replacement) == 0 && replacement.first_cluster != node.first_cluster);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && !inode.links_count);
    assert(ext2_read_node(&node, 0, data, 3, &got) == 0 && got == 3 && !memcmp(data, "old", 3));
    assert(storage_inode_put(held) == 0 && held->references == 1);
    assert(storage_write_held_node(&node, 0, "held", 4, &got) == 0 && got == 4);
    assert(storage_truncate_held_node(&node, 2) == 0);
    assert(ext2_read_node(&node, 0, data, 4, &got) == 0 && got == 2 && !memcmp(data, "he", 2));
    assert(storage_inode_put(held) == 0);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && !inode.mode);
    assert(ext2_unlink("/held-inode") == 0 && !storage_inode_refs);
    puts("PASS production inode references: unlink retention, distinct replacement, write/truncate, final reclamation");
}

static void test_write_privileges(void)
{
    struct task actor = {.fsuid = 1000, .fsgid = 200};
    struct storage_node node;
    struct ext2_inode inode;
    uint32_t wrote;
    assert(ext2_write_file("/write-privileges", "a", 1) == 0);
    assert(ext2_lookup_path("/write-privileges", &node) == 0);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0);
    inode.mode = EXT2_S_IFREG | 06770;
    inode.uid = 1000;
    inode.gid = 200;
    assert(ext2_write_inode(node.first_cluster, &inode) == 0);
    writer = &actor;
    assert(ext2_write_node(&node, 0, "b", 1, &wrote) == 0 && wrote == 1);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && !(inode.mode & 06000));
    inode.mode = EXT2_S_IFREG | 02660;
    assert(ext2_write_inode(node.first_cluster, &inode) == 0);
    assert(ext2_truncate_file(&node, 0) == 0);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && (inode.mode & 02000));
    actor.fsgid = 300;
    assert(ext2_truncate_file(&node, 0) == 0);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && !(inode.mode & 02000));
    actor.cap_effective = 1ULL << CAP_FSETID;
    inode.mode = EXT2_S_IFREG | 06770;
    assert(ext2_write_inode(node.first_cluster, &inode) == 0);
    assert(ext2_write_file("/write-privileges", "a", 1) == 0);
    assert(ext2_read_inode(node.first_cluster, &inode) == 0 && (inode.mode & 06000) == 06000);
    writer = NULL;
    assert(ext2_unlink("/write-privileges") == 0);
    puts("PASS production ext2 write/truncate drops set-ID bits according to CAP_FSETID and group membership");
}

static void test_symlinks(void)
{
    struct storage_node node, alias;
    struct ext2_inode inode;
    char target[4097], result[4097];
    const unsigned lengths[] = {59, 60, 61, 255, g_storage.ext2_block_size - 1};
    assert(ext2_symlink("", "/empty-link") == -2);
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        unsigned length = lengths[i];
        memset(target, 't', length);
        target[length] = 0;
        assert(ext2_symlink(target, "/link") == 0);
        assert(ext2_symlink("other", "/link") == -17);
        assert(ext2_lookup_path("/link", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
        assert(ext2_read_inode(node.first_cluster, &inode) == 0);
        assert(inode.mode == 0120777 && inode.size_lo == length);
        assert(inode.blocks_512 == (length < 60 ? 0 : g_storage.ext2_block_size / 512));
        uint32_t got;
        memset(result, '#', sizeof(result));
        assert(ext2_symlink_read(&node, result, sizeof(result), &got) == 0 && got == length);
        assert(!memcmp(result, target, length) && result[length] == '#');
        assert(ext2_symlink_read(&node, result, 3, &got) == 0 && got == 3);
        assert(ext2_link("/link", "/link-hard") == 0);
        assert(ext2_lookup_path("/link-hard", &alias) == 0 && alias.first_cluster == node.first_cluster);
        assert(ext2_symlink("replace", "/replacement") == 0);
        assert(ext2_rename("/replacement", "/link") == 0);
        assert(ext2_symlink_read(&alias, result, sizeof(result), &got) == 0 && got == length);
        assert(ext2_rename("/link-hard", "/link-renamed") == 0);
        assert(ext2_unlink("/link") == 0 && ext2_unlink("/link-renamed") == 0);
    }
    memset(target, 't', g_storage.ext2_block_size);
    target[g_storage.ext2_block_size] = 0;
    assert(ext2_symlink(target, "/too-long") == -36);
    assert(ext2_symlink("dangling-target", "/symlink-persisted") == 0);
    assert(ext2_symlink("upper", "/CaseLink") == 0);
    assert(ext2_lookup_path("/caselink", &node) == -2);
    assert(ext2_symlink("lower", "/caselink") == 0);
    assert(ext2_unlink("/CaseLink") == 0);
    assert(ext2_lookup_path("/caselink", &node) == 0);
    uint32_t got;
    assert(ext2_symlink_read(&node, result, sizeof(result), &got) == 0 && got == 5);
    assert(!memcmp(result, "lower", 5));
    assert(ext2_unlink("/caselink") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    if (argc == 3) {
        assert(!strcmp(argv[2], "--ram-root"));
        test_start_lba = 0;
        g_storage.kind = STORAGE_VOLUME_RAM;
    }
    disk = fopen(argv[1], "r+b");
    assert(disk);
    g_storage.ext2_start_lba = test_start_lba;
    assert(!fseek(disk, 0, SEEK_END));
    g_storage.ext2_sector_count = ftell(disk) / 512;
    assert(ext2_mount() == 0);
    test_symlinks();
    test_held_inode();
    test_write_privileges();
    struct storage_node source, target;
    assert(ext2_lookup_path("/source", &source) == 0);
    assert(ext2_link("/source", "/source-hard") == 0);
    assert(ext2_lookup_path("/source-hard", &target) == 0 &&
           target.first_cluster == source.first_cluster);
    struct ext2_inode linked_inode;
    assert(ext2_read_inode(source.first_cluster, &linked_inode) == 0 &&
           linked_inode.links_count == 2);
    assert(ext2_unlink("/source") == 0);
    assert(ext2_lookup_path("/source-hard", &target) == 0);
    assert(ext2_read_inode(target.first_cluster, &linked_inode) == 0 &&
           linked_inode.links_count == 1);
    assert(ext2_unlink("/source-hard") == 0 && ext2_lookup_path("/source-hard", &target) == -2);
    assert(ext2_write_file("/source", "new", 3) == 0);
    assert(ext2_lookup_path("/source", &source) == 0);
    assert(ext2_rename("/source", "/target") == 0);
    assert(ext2_lookup_path("/source", &target) == -2);
    assert(ext2_lookup_path("/target", &target) == 0 && target.first_cluster == source.first_cluster);
    assert(ext2_rename("/target", "/target") == 0);
    assert(ext2_rename("/missing", "/missing") == -2);
    assert(ext2_rename("/target", "/left") == -21);
    assert(ext2_rename("/left", "/target") == -20);
    assert(ext2_rename("/left", "/right") == -39);
    assert(ext2_unlink("/right/child") == 0);
    assert(ext2_rename("/left", "/right") == 0);
    assert(ext2_lookup_path("/left", &source) == -2);
    assert(ext2_lookup_path("/right", &target) == 0 && target.type == RELIEFOS_FS_TYPE_DIR);
    assert(ext2_write_file("/socket", "", 0) == 0);
    assert(ext2_lookup_path("/socket", &source) == 0);
    assert(ext2_mark_special("/socket", &source, LINUX_S_IFSOCK) == 0);
    assert(ext2_lookup_path("/socket", &source) == 0 && source.type == RELIEFOS_FS_TYPE_SOCKET);
    assert(ext2_write_file("/replaced", "old", 3) == 0);
    assert(ext2_rename("/socket", "/replaced") == 0);
    assert(ext2_lookup_path("/replaced", &target) == 0 && target.type == RELIEFOS_FS_TYPE_SOCKET);
    assert(ext2_rename("/replaced", "/socket-persisted") == 0);
    assert(ext2_lookup_path("/socket-persisted", &target) == 0 && target.type == RELIEFOS_FS_TYPE_SOCKET);
    assert(ext2_write_file("/deleted-socket", "", 0) == 0);
    assert(ext2_lookup_path("/deleted-socket", &source) == 0);
    assert(ext2_mark_special("/deleted-socket", &source, LINUX_S_IFSOCK) == 0 && ext2_unlink("/deleted-socket") == 0);
    assert(storage_ext4_ops.fsync(&g_storage)==0);
    assert(fclose(disk) == 0);
    puts("ext2 rename: replacement, inode identity, type errors, empty-directory rules PASS");
    return 0;
}
