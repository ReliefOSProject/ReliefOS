#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW
/** @brief Count file durability calls and inject a file-sync failure. */
static int fixture_fsync(int fd);
/** @brief Model the Linux filesystem barrier, refusing injected I/O failure. */
static int fixture_syncfs(int fd);
#define fsync fixture_fsync
#define syncfs fixture_syncfs
#include "../../userland/apps/installer/install_ops.c"
#undef fsync
#undef syncfs
static unsigned file_sync_calls, filesystem_sync_calls, publish_calls;
static int fail_file_sync, fail_filesystem_sync;
static int fixture_fsync(int fd)
{
    ++file_sync_calls;
    if (fail_file_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
static int fixture_syncfs(int fd)
{
    struct stat st;
    assert(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
    ++filesystem_sync_calls;
    if (fail_filesystem_sync) { errno = EIO; return -1; }
    return 0;
}
/** @brief Assert that publication cannot precede a successful filesystem barrier. */
int reliefos_boot_install_at_root(const struct reliefos_boot_install_options *options, const char *root)
{
    (void)options;
    assert(root && *root);
    assert(filesystem_sync_calls && !fail_filesystem_sync);
    ++publish_calls;
    return 0;
}
/** @brief Host-owned copy fixture needs no GUI event or actual guest clock. */
void refresh_ui(void) {}
unsigned long reliefos_uptime_ms(void) { return 0; }
/** @brief Exercise real file replacement and barrier-error publication ordering. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    char source[1024], target[1024], text[64];
    snprintf(source, sizeof(source), "%s/source", argv[1]);
    snprintf(target, sizeof(target), "%s/target", argv[1]);
    FILE *f = fopen(source, "w"); assert(f); fputs("complete-new-file\n", f); assert(!fclose(f));
    f = fopen(target, "w"); assert(f); fputs("previous-file\n", f); assert(!fclose(f));
    installer_tty_mode = 1;
    fail_file_sync = 1;
    assert(copy_file_path(source, target) == -EIO);
    f = fopen(target, "r"); assert(f); assert(fgets(text, sizeof(text), f)); fclose(f);
    assert(!strcmp(text, "previous-file\n"));
    file_sync_calls = 0;
    copy_defer_sync = 1;
    assert(copy_file_path(source, target) == 0);
    copy_defer_sync = 0;
    assert(file_sync_calls == 0);
    f = fopen(target, "r"); assert(f); assert(fgets(text, sizeof(text), f)); fclose(f);
    assert(!strcmp(text, "complete-new-file\n"));
    struct reliefos_boot_install_options options = {0};
    fail_filesystem_sync = 1;
    assert(install_boot_after_root_sync(argv[1], &options) == -EIO);
    assert(publish_calls == 0 && filesystem_sync_calls == 1);
    fail_filesystem_sync = 0;
    assert(install_boot_after_root_sync(argv[1], &options) == 0);
    assert(publish_calls == 1 && filesystem_sync_calls == 2);
    puts("PASS fresh copy batches file sync; failed Linux syncfs blocks boot publication; ordinary durable copy preserves old file on failure");
    return 0;
}
