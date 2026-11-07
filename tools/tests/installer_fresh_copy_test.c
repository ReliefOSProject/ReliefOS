#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW

static int counted_fchown(int fd, uid_t uid, gid_t gid);
static int counted_fstat(int fd, struct stat *st);
static int counted_rename(const char *from, const char *to);
static ssize_t checked_write(int fd, const void *data, size_t count);
static int checked_close(int fd);
static int checked_fchmod(int fd, mode_t mode);
static int counted_chown(const char *path, uid_t uid, gid_t gid);
static int counted_chmod(const char *path, mode_t mode);
static int modeled_open(const char *path, int flags, ...);
static int modeled_lstat(const char *path, struct stat *st);
static unsigned owner_calls, rename_calls;
static unsigned directory_owners, directory_modes;
static int pretend_wrong_owner;
static int fail_mode, fail_close, interrupt_write;
static ssize_t write_budget = -1;
static size_t short_write;
static const char *modeled_target;
static const char *modeled_directory_owner;
static int clear_directory_setid;
#define open modeled_open
#define lstat modeled_lstat
#define fchown counted_fchown
#define fstat counted_fstat
#define rename counted_rename
#define write checked_write
#define close checked_close
#define fchmod checked_fchmod
#define chown counted_chown
#define chmod counted_chmod
#include "../../userland/apps/installer/install_ops.c"
#undef fchown
#undef fstat
#undef rename
#undef write
#undef close
#undef fchmod
#undef chown
#undef chmod
#undef open
#undef lstat

static int modeled_lstat(const char *path, struct stat *st)
{
    int result = lstat(path, st);
    if (!result && modeled_directory_owner && !strcmp(path, modeled_directory_owner)) {
        ++st->st_gid;
        modeled_directory_owner = NULL;
    }
    return result;
}

/** @brief Model native creation followed by descriptor-allocation failure. */
static int modeled_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    if (modeled_target && !strcmp(path, modeled_target) &&
        (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        int fd = open(path, flags, mode);
        assert(fd >= 0 && !close(fd));
        errno = EMFILE;
        return -1;
    }
    return open(path, flags, mode);
}

/** @brief Count real ownership updates, including Linux set-ID clearing. */
static int counted_fchown(int fd, uid_t uid, gid_t gid)
{
    ++owner_calls;
    return fchown(fd, uid, gid);
}
/** @brief Exercise owner mismatch without requiring host privilege changes. */
static int counted_fstat(int fd, struct stat *st)
{
    int result = fstat(fd, st);
    if (!result && pretend_wrong_owner && (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY) {
        st->st_gid = (gid_t)(st->st_gid + 1);
        pretend_wrong_owner = 0;
    }
    return result;
}
static int counted_rename(const char *from, const char *to)
{
    ++rename_calls;
    return rename(from, to);
}
/** @brief Keep incomplete data private and inject real partial I/O failures. */
static ssize_t checked_write(int fd, const void *data, size_t count)
{
    struct stat st;
    assert(!fstat(fd, &st) && (st.st_mode & 07777) == 0600);
    if (interrupt_write) { interrupt_write = 0; errno = EINTR; return -1; }
    if (!write_budget) { errno = EIO; return -1; }
    if (write_budget > 0 && count > (size_t)write_budget) count = (size_t)write_budget;
    if (short_write && count > short_write) count = short_write;
    ssize_t result = write(fd, data, count);
    if (result > 0 && write_budget > 0) write_budget -= result;
    return result;
}
static int checked_close(int fd)
{
    int output = (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
    int result = close(fd);
    if (output && fail_close) { fail_close = 0; errno = EIO; return -1; }
    return result;
}
static int checked_fchmod(int fd, mode_t mode)
{
    if (fail_mode) { fail_mode = 0; errno = EACCES; return -1; }
    return fchmod(fd, mode);
}
static int counted_chown(const char *path, uid_t uid, gid_t gid)
{
    ++directory_owners;
    int result = chown(path, uid, gid);
    if (!result && clear_directory_setid) {
        struct stat st; assert(!stat(path, &st));
        assert(!chmod(path, (st.st_mode & 07777) & ~06000));
        clear_directory_setid = 0;
    }
    return result;
}
static int counted_chmod(const char *path, mode_t mode)
{
    ++directory_modes;
    return chmod(path, mode);
}
void refresh_ui(void) {}
unsigned long reliefos_uptime_ms(void) { return 0; }

/** @brief Adapt the runtime's negative-errno mkdir ABI to a real host directory. */
long syscall2(long number, long argument0, long argument1)
{
    assert(number == SYS_mkdir);
    int result = mkdir((const char *)argument0, (mode_t)argument1);
    return result < 0 ? -errno : result;
}
/** @brief Translate actual host stat data into the existing runtime ABI. */
int reliefos_stat_legacy(const char *path, struct reliefos_stat *out)
{
    struct stat st;
    if (stat(path, &st) < 0) return -errno;
    memset(out, 0, sizeof(*out));
    out->type = S_ISDIR(st.st_mode) ? RELIEFOS_FS_TYPE_DIR :
                S_ISREG(st.st_mode) ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DEVICE;
    out->size = st.st_size;
    return 0;
}

static void contents(const char *path, const char *expected, mode_t mode)
{
    struct stat st;
    assert(!lstat(path, &st) && S_ISREG(st.st_mode));
    assert(st.st_uid == getuid() && st.st_gid == getgid());
    assert((st.st_mode & 07777) == mode);
    FILE *stream = fopen(path, "r"); assert(stream);
    char text[80] = {0}; assert(fread(text, 1, sizeof(text) - 1, stream) == strlen(expected));
    assert(!strcmp(text, expected) && !fclose(stream));
}

/** @brief Copy real data and permissions without journaling unchanged owners. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    umask(0022);
    char source[1024], target[1024];
    snprintf(source, sizeof(source), "%s/source", argv[1]);
    snprintf(target, sizeof(target), "%s/target", argv[1]);
    FILE *stream = fopen(source, "w"); assert(stream);
    assert(fputs("complete-new-file\n", stream) >= 0 && !fclose(stream));
    assert(!chmod(source, 0644));
    installer_tty_mode = 1;
    copy_defer_sync = 1;
    assert(!copy_file_path(source, target));
    contents(target, "complete-new-file\n", 0644);
    assert(owner_calls == 0 && "unchanged owner must not cause a journal transaction");
    assert(rename_calls == 1);

    assert(!chmod(source, 04755));
    pretend_wrong_owner = 1;
    assert(!copy_file_path(source, target));
    assert(owner_calls == 1 && !pretend_wrong_owner);
    contents(target, "complete-new-file\n", 04755);

    char from[1024], to[1024], child[1200], copied[1200], missing[1024];
    snprintf(from, sizeof(from), "%s/tree", argv[1]);
    snprintf(to, sizeof(to), "%s/copied-tree", argv[1]);
    snprintf(child, sizeof(child), "%s/child", from);
    snprintf(copied, sizeof(copied), "%s/child", to);
    snprintf(missing, sizeof(missing), "%s/missing", argv[1]);
    assert(!mkdir(from, 0755) && !mkdir(to, 0755));
    stream = fopen(child, "w"); assert(stream);
    assert(fputs("fresh-tree\n", stream) >= 0 && !fclose(stream));
    assert(!chmod(child, 0644));
    copy_defer_sync = 9; copy_fresh_root = 7;
    assert(copy_root_payload(missing, to, 1) == -ENOENT);
    assert(copy_defer_sync == 9 && copy_fresh_root == 7);
    rename_calls = directory_owners = directory_modes = 0;
    assert(!copy_root_payload(from, to, 1));
    assert(copy_defer_sync == 9 && copy_fresh_root == 7);
    contents(copied, "fresh-tree\n", 0644);
    assert(rename_calls == 1 && "fresh files also publish only after completing the private copy");
    assert(directory_owners == 0 && directory_modes == 0 &&
           "already-correct fresh directory metadata must not create journal transactions");
    assert(!chmod(from, 02755));
    directory_owners = directory_modes = 0;
    assert(!copy_root_payload(from, to, 1));
    struct stat copied_directory;
    assert(!stat(to, &copied_directory) && (copied_directory.st_mode & 07777) == 02755);
    assert(directory_owners == 0 && directory_modes == 1);
    modeled_directory_owner = to; clear_directory_setid = 1;
    directory_owners = directory_modes = 0;
    assert(!copy_root_payload(from, to, 1));
    assert(!modeled_directory_owner && !clear_directory_setid);
    assert(!stat(to, &copied_directory) && (copied_directory.st_mode & 07777) == 02755);
    assert(directory_owners == 1 && directory_modes == 1);
    write_budget = 3;
    assert(copy_root_payload(from, to, 1) == -EIO);
    assert(copy_defer_sync == 9 && copy_fresh_root == 7);
    contents(copied, "fresh-tree\n", 0644);
    write_budget = -1;
    copy_defer_sync = copy_fresh_root = 0;

    char open_failure[1024];
    snprintf(open_failure, sizeof(open_failure), "%s/native-open-failure", argv[1]);
    modeled_target = open_failure;
    copy_defer_sync = copy_fresh_root = 1;
    int copy_result = copy_file_path(child, open_failure);
    modeled_target = NULL;
    if (copy_result < 0) {
        assert(copy_result == -EMFILE);
        assert(access(open_failure, F_OK) < 0 && errno == ENOENT &&
               "failed native open must not leave an incomplete final filename");
    } else {
        contents(open_failure, "fresh-tree\n", 0644);
    }

    char failure[1024], existing[1024], alias[1024], symbolic[1024];
    snprintf(failure, sizeof(failure), "%s/failure", argv[1]);
    snprintf(existing, sizeof(existing), "%s/existing", argv[1]);
    snprintf(alias, sizeof(alias), "%s/alias", argv[1]);
    snprintf(symbolic, sizeof(symbolic), "%s/symbolic", argv[1]);
    copy_defer_sync = copy_fresh_root = 1;
    write_budget = 3;
    assert(copy_file_path(child, failure) == -EIO);
    assert(access(failure, F_OK) < 0 && errno == ENOENT);
    write_budget = -1; fail_mode = 1;
    assert(copy_file_path(child, failure) == -EACCES);
    assert(access(failure, F_OK) < 0 && errno == ENOENT);
    fail_close = 1;
    assert(copy_file_path(child, failure) == -EIO && !fail_close);
    assert(access(failure, F_OK) < 0 && errno == ENOENT);
    short_write = 3; interrupt_write = 1; rename_calls = 0;
    assert(!copy_file_path(child, failure) && !interrupt_write && rename_calls == 1);
    contents(failure, "fresh-tree\n", 0644);
    short_write = 0;

    stream = fopen(existing, "w"); assert(stream);
    assert(fputs("previous-file\n", stream) >= 0 && !fclose(stream));
    assert(!chmod(existing, 0644) && !link(existing, alias));
    write_budget = 3;
    assert(copy_file_path(child, existing) == -EIO);
    contents(existing, "previous-file\n", 0644);
    contents(alias, "previous-file\n", 0644);
    write_budget = -1; rename_calls = 0;
    assert(!copy_file_path(child, existing) && rename_calls == 1);
    contents(existing, "fresh-tree\n", 0644);
    contents(alias, "previous-file\n", 0644);
    assert(!symlink("target", symbolic));
    assert(!copy_file_path(child, symbolic));
    contents(symbolic, "fresh-tree\n", 0644);
    contents(target, "complete-new-file\n", 04755);

    copy_defer_sync = copy_fresh_root = 0; fail_close = 1;
    assert(copy_file_path(source, existing) == -EIO);
    contents(existing, "fresh-tree\n", 0644);
    contents(alias, "previous-file\n", 0644);
    DIR *dir = opendir(argv[1]); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) assert(strncmp(entry->d_name, ".reliefos-copy-", 15));
    assert(!closedir(dir));
    puts("PASS atomic publication, unchanged metadata, set-ID restoration, short I/O and copy failure safety");
    return 0;
}
