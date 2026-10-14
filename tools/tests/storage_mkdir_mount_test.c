#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The storage modules are independent translation units; this fixture keeps
 * them in one TU so a plain host build can exercise them together.  The list
 * mirrors the former drivers/bootstrap/storage.c aggregation, in its order. */
#include "../../kernel/reliefnt/block/storage_internal.h"
#include <reliefnt/tmpfs.h>
#include "../../kernel/reliefnt/block/storage_state.c"
#include "../../kernel/reliefnt/block/storage_state_data.c"
#include "../../kernel/reliefnt/block/storage_devlinks.c"
#include "../../kernel/reliefnt/fs/storage_ext2_cache.c"
#include "../../kernel/reliefnt/drivers/storage/ide/storage_ide.c"
#include "../../kernel/reliefnt/drivers/storage/ahci/storage_ahci.c"
#include "../../kernel/reliefnt/drivers/storage/nvme/storage_nvme.c"
#include "../../kernel/reliefnt/block/storage_block.c"
#include "../../kernel/reliefnt/fs/storage_ext4_format.c"
#include "../../kernel/reliefnt/fs/storage_ext4_checksum.c"
#include "../../kernel/reliefnt/fs/storage_ext4_cache.c"
#include "../../kernel/reliefnt/fs/storage_ext4_alloc.c"
#include "../../kernel/reliefnt/fs/storage_ext4_extent.c"
#include "../../kernel/reliefnt/fs/storage_ext4_ops.c"
#include "../../kernel/reliefnt/fs/storage_ext4_xattr.c"
#include "../../kernel/reliefnt/fs/storage_ext4_journal.c"
#include "../../kernel/reliefnt/fs/storage_ext4_dir.c"
#include "../../kernel/reliefnt/block/storage_sync.c"
#include "../../kernel/reliefnt/fs/storage_iso.c"
#include "../../kernel/reliefnt/fs/storage_fat32.c"
#include "../../kernel/reliefnt/fs/storage_inode.c"
#include "../../kernel/reliefnt/fs/storage_ext4_vfs.c"
#include "../../kernel/reliefnt/fs/storage_ext2.c"
#include "../../kernel/reliefnt/fs/storage_exfat.c"
#include "../../kernel/reliefnt/fs/storage_ext4_mount.c"
#include "../../kernel/reliefnt/fs/storage_mount.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/storage_audio.c"
#include "../../kernel/reliefnt/fs/storage_vfs.c"
#include "../../kernel/reliefnt/fs/storage_mounts.c"
#include "../../kernel/reliefnt/fs/storage_tmpfs.c"
#include "../../kernel/reliefnt/fs/storage_permissions.c"
#include "../../kernel/reliefnt/fs/storage_sidecar.c"
#include "../../kernel/reliefnt/fs/storage_statfs.c"
#include "../../kernel/reliefnt/block/storage_installer.c"
#include "../../kernel/reliefnt/block/storage_disk.c"
#include "../../kernel/reliefnt/block/storage_identity.c"

static int test_fail_cache_alloc;
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *p) { free(p); }
uint64_t mm_alloc_page(void) { return (uintptr_t)aligned_alloc(4096, 4096); }
void mm_free_page(uint64_t p) { free((void *)(uintptr_t)p); }
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{ (void)node; (void)size; abort(); }
uint64_t mm_alloc_pages(uint32_t pages)
{ return test_fail_cache_alloc ? 0 : (uint64_t)(uintptr_t)calloc(pages, 4096); }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
uint32_t sched_current_pid(void) { return 1; }
struct task *sched_current_task(void) { return NULL; }
bool task_in_group(const struct task *task, uint32_t gid, bool real)
{ (void)task; (void)gid; (void)real; abort(); }
void page_cache_invalidate_node(const struct storage_node *node)
{ assert(node && (node->flags & STORAGE_NODE_FLAG_EXT2)); }
uint32_t smp_cpu_count(void) { return 1; }
int pty_lookup_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; abort(); }
int pty_lookup_vt_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; abort(); }
/* RAM-backed ext2 must never touch the physical controller or other codecs. */
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; assert(0); }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; assert(0); }
uint8_t x86_64_inb(uint16_t port) { (void)port; abort(); }
uint16_t x86_64_inw(uint16_t port) { (void)port; abort(); }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; abort(); }
uint64_t time_ticks(void) { abort(); }
int time_wall_clock(struct reliefos_time_info *info)
{ *info = (struct reliefos_time_info){.unix_seconds = 1800000000}; return 0; }
static void load_volume(const char *image, unsigned id, const char *mount_path)
{
    FILE *file = fopen(image, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    struct storage_volume *volume = &g_volumes[id];
    *volume = (struct storage_volume){.volume_id = id, .kind = STORAGE_VOLUME_RAM,
        .ram_bytes = size, .ext2_sector_count = size / 512};
    volume->ram_base = malloc(size);
    assert(volume->ram_base && fread(volume->ram_base, 1, size, file) == (size_t)size);
    assert(!fclose(file));
    strcpy(volume->mount_path, mount_path);
    g_active_volume = volume;
    assert(ext2_mount() == 0);
    volume->ready = true;
    storage_cache_invalidate();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    load_volume(argv[1], 0, "/");
    assert(storage_mkdir("/target") == 0);
    assert(storage_mkdir("/target") == -17);
    load_volume(argv[1], 1, "/target");
    /* The update wizard prepares the same target twice: scan, then update. */
    int ret = storage_mkdir("/target");
    printf("mkdir mounted /target: %d (expected -17/EEXIST)\n", ret);
    fflush(stdout);
    assert(ret == -17);
    assert(storage_mkdir("/") == -17);
    storage_task_io_owner = 2;
    storage_set_io_async_context(true);
    assert(storage_mkdir("/target") == -RELIEFOS_EAGAIN);
    storage_release_task_io(2);
    storage_set_io_async_context(false);
    assert(storage_mkdir("/target/boot") == 0);
    load_volume(argv[1], 2, "/target/boot");
    for (unsigned i = 0; i < 3; ++i) {
        assert(storage_mkdir("/target") == -17);
        assert(storage_mkdir("/target/boot") == -17);
        assert(storage_mkdir("/target/boot/") == -17);
    }
    assert(storage_mkdir("/target/new") == 0);
    assert(storage_mkdir("/target/new") == -17);
    assert(storage_mkdir("/target/missing/child") == -2);
    assert(storage_write_file("/target/file", "data", 4) == 0);
    assert(storage_mkdir("/target/file") == -17);
    assert(storage_mkdir("/target/file/child") == -20);
    assert(storage_symlink("missing", "/target/link") == 0);
    assert(storage_mkdir("/target/link") == -17);
    g_active_volume = &g_volumes[0];
    struct storage_node node;
    assert(ext2_lookup_path("/target/new", &node) == -2);
    for (unsigned i = 0; i < 3; ++i) free(g_volumes[i].ram_base);
    puts("PASS storage mkdir: mounted roots, nested mounts, existing objects and target volume isolation");
}
