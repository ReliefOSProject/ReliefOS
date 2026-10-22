#define _GNU_SOURCE
#include <assert.h>
/* glibc declares these as enum members after including linux/mount.h. */
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW
#define installer_capacity_measure fake_capacity_measure
#define mkdir fake_mkdir
#define lstat fake_lstat
#define mount fake_mount
#define umount2 fake_umount2
#define posix_spawnp fake_posix_spawnp
#define waitpid fake_waitpid
#include "../../userland/apps/installer/install_ops.c"
#undef posix_spawnp
#undef waitpid
#include <sys/mman.h>
#include <stdarg.h>
int __real_open(const char *path, int flags, ...);

/** @brief Give the default planner an owned RAM file instead of any host disk. */
int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/sda")) {
        int fd = memfd_create("installer-mount-fixture", MFD_CLOEXEC);
        assert(fd >= 0 && ftruncate(fd, 4 * 1024 * 1024) == 0);
        return fd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

static int root_mounted, esp_mounted, busy_root, busy_esp, mutations;
static int manual_layout, manual_bad_role, format_calls, type_calls;
static uint64_t capacity_bytes, capacity_inodes;
int fake_capacity_measure(const char *root, const char *boot, int bios, int uefi,
                          struct installer_capacity *out)
{
    (void)root; (void)boot; (void)bios; (void)uefi;
    *out = (struct installer_capacity){.root_bytes = capacity_bytes, .root_inodes = capacity_inodes};
    return 0;
}

/* The format step is the reason this test exists: capture the spelling of the
 * mkfs.ext4 invocation so a silently-degrading default (the mke2fs revision-0
 * fallback) cannot creep back in unnoticed. */
static char spawn_file[64];
static char spawn_args[16][128];
static int spawn_argc;
static int spawn_calls;

int fake_posix_spawnp(pid_t *pid, const char *file,
                      const posix_spawn_file_actions_t *actions,
                      const posix_spawnattr_t *attr,
                      char *const argv[], char *const envp[])
{
    (void)actions;
    (void)attr;
    (void)envp;
    ++spawn_calls;
    snprintf(spawn_file, sizeof(spawn_file), "%s", file ? file : "");
    spawn_argc = 0;
    for (int i = 0; argv && argv[i] && spawn_argc < 16; ++i) {
        snprintf(spawn_args[spawn_argc], sizeof(spawn_args[0]), "%s", argv[i]);
        ++spawn_argc;
    }
    *pid = 1;
    return 0;
}

pid_t fake_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status) *status = 0;
    return pid;
}

int fake_mkdir(const char *path, mode_t mode) { return 0; }
int fake_lstat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0755;
    return 0;
}
int fake_umount2(const char *path, int flags)
{
    int esp = !strcmp(path, INSTALL_ESP_MOUNT);
    int *mounted = esp ? &esp_mounted : &root_mounted;
    if ((esp ? busy_esp : busy_root) || (!esp && esp_mounted)) {
        errno = EBUSY;
        return -1;
    }
    if (!*mounted) { errno = EINVAL; return -1; }
    *mounted = 0;
    return 0;
}
int fake_mount(const char *source, const char *target, const char *type,
               unsigned long flags, const void *data)
{
    int *mounted = !strcmp(target, INSTALL_ESP_MOUNT) ? &esp_mounted : &root_mounted;
    assert(!*mounted);
    *mounted = 1;
    return 0;
}
int reliefos_block_get_info(const char *path, struct reliefos_block_disk_info *out)
{
    out->sector_size = 512;
    out->sector_count = 8 * 1024 * 1024;
    return 0;
}
int reliefos_block_gpt_initialize(const char *path, int force)
{
    ++mutations;
    /* GPT rereads and filesystem formatting require all target mounts gone. */
    return root_mounted || esp_mounted ? -EBUSY : 0;
}
int reliefos_block_gpt_create(const char *path, uint32_t fs, uint32_t size,
                            const char *name, uint32_t *index)
{ *index = fs == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN ? 0 : fs == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ? 1 : 2; return 0; }
int reliefos_block_gpt_set_type(const char *path, uint32_t index, uint32_t type) { ++type_calls; return 0; }
int reliefos_block_partition_path(const char *path, uint32_t index, char *out, uint32_t cap)
{ snprintf(out, cap, "%sp%u", path, index + 1); return 0; }
int reliefos_block_format(const char *path, uint32_t fs, const char *label)
{ assert(!root_mounted && !esp_mounted); ++format_calls; return 0; }
int reliefos_block_partition_uuid(const char *path, uint32_t index, char uuid[37])
{ strcpy(uuid, "00000000-0000-0000-0000-000000000001"); return 0; }
int reliefos_block_list_partitions(const char *path, struct reliefos_block_partition *parts,
                                 uint32_t cap, uint32_t *count)
{
    if (manual_layout) {
        assert(cap >= 3);
        memset(parts, 0, 3 * sizeof(*parts));
        parts[0].index = 5; parts[0].gpt_type = RELIEFOS_BLOCK_GPT_BIOS_BOOT;
        parts[0].sector_count = 2048;
        parts[1].index = 3; parts[1].gpt_type = RELIEFOS_BLOCK_GPT_ESP;
        if (manual_bad_role) parts[1].gpt_type = RELIEFOS_BLOCK_GPT_BIOS_BOOT;
        parts[2].index = 8; parts[2].gpt_type = RELIEFOS_BLOCK_GPT_LINUX;
        parts[1].sector_count = parts[2].sector_count = 128 * 2048;
        strcpy(parts[0].path, "/dev/sda6");
        strcpy(parts[1].path, "/dev/sda4");
        strcpy(parts[2].path, "/dev/sda9");
        *count = 3;
        return 0;
    }
    assert(cap >= 2);
    memset(parts, 0, 2 * sizeof(*parts));
    parts[0].filesystem = RELIEFOS_BLOCK_FILESYSTEM_FAT32;
    parts[0].gpt_type = RELIEFOS_BLOCK_GPT_ESP;
    parts[1].gpt_type = RELIEFOS_BLOCK_GPT_LINUX;
    parts[1].filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    strcpy(parts[0].path, "/dev/sda1");
    strcpy(parts[1].path, "/dev/sda2");
    *count = 2;
    return 0;
}

/* The fresh path must hand mkfs.ext4 an explicit ext4 profile: an inherited
 * default can degrade to revision 0 (ext2) whenever uname() reports a release
 * the tool considers pre-2.2, which is exactly the mount EINVAL this guards. */
static void assert_format_args(void)
{
    assert(spawn_calls == 1);
    assert(!strcmp(spawn_file, "mkfs.ext4"));
    int seen_f = 0, seen_b = 0, seen_i = 0, seen_o = 0;
    for (int i = 1; i < spawn_argc; ++i) {
        if (!strcmp(spawn_args[i], "-F")) seen_f = 1;
        else if (!strcmp(spawn_args[i], "-b")) { seen_b = 1; assert(!strcmp(spawn_args[i + 1], "4096")); }
        else if (!strcmp(spawn_args[i], "-I")) { seen_i = 1; assert(!strcmp(spawn_args[i + 1], "256")); }
        else if (!strcmp(spawn_args[i], "-O")) {
            seen_o = 1;
            const char *features = spawn_args[i + 1];
            assert(strstr(features, "extents"));
            assert(strstr(features, "64bit"));
            assert(strstr(features, "metadata_csum"));
            assert(strstr(features, "has_journal"));
        }
    }
    assert(seen_f && seen_b && seen_i && seen_o);
    /* The device is the last argument. Check it is a partition of the target
     * disk rather than pinning this test to the stub's naming scheme. */
    assert(!strncmp(spawn_args[spawn_argc - 1], "/dev/sda", 8));
}

int main(void)
{
    assert(installer_mount_targets("/dev/sda", 0) == 0);
    assert(root_mounted && esp_mounted);
    assert(spawn_calls == 0);   /* an update never formats */
    assert(installer_mount_targets("/dev/sda", 1) == 0);
    assert(root_mounted && esp_mounted && mutations == 1);
    assert_format_args();
    assert(installer_mount_targets("/dev/sda", 0) == 0);
    assert(spawn_calls == 1);   /* returning to update does not re-format */
    assert(installer_mount_targets("/dev/sda", 0) == 0);
    busy_esp = 1;
    assert(installer_mount_targets("/dev/sda", 1) == -EBUSY);
    assert(root_mounted && esp_mounted && mutations == 1);
    busy_esp = 0;
    busy_root = 1;
    assert(installer_mount_targets("/dev/sda", 1) == -EBUSY);
    assert(root_mounted && !esp_mounted && mutations == 1);
    busy_root = 0;
    assert(installer_mount_targets("/dev/sda", 1) == 0);
    assert(root_mounted && esp_mounted && mutations == 2);
    manual_layout = 1;
    installer_partition_auto = 0;
    installer_root_partition = 8;
    installer_esp_partition = 3;
    assert(installer_mount_targets("/dev/sda", 1) == 0);
    assert(!strcmp(spawn_args[spawn_argc - 1], "/dev/sda9"));
    assert(mutations == 2); /* A manual install never replaces the GPT. */
    manual_bad_role = 1;
    int unchanged_formats = format_calls;
    assert(installer_mount_targets("/dev/sda", 1) == -EINVAL);
    assert(format_calls == unchanged_formats && mutations == 2);
    selected_partition = 0;
    partition_count = 1;
    partitions[0].gpt_type = RELIEFOS_BLOCK_GPT_BIOS_BOOT;
    int previous_formats = format_calls;
    assert(installer_partition_format(RELIEFOS_BLOCK_FILESYSTEM_EXT4, "bad") == -EPERM);
    assert(format_calls == previous_formats);
    selected_disk = 0; disk_count = 1;
    strcpy(disks[0].path, "/dev/sda");
    partitions[0].filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    int previous_types = type_calls;
    assert(installer_partition_set_type(RELIEFOS_BLOCK_GPT_BIOS_BOOT) == -EINVAL);
    assert(type_calls == previous_types);
    manual_bad_role = 0;
    capacity_bytes = 256ULL * 1024 * 1024;
    assert(installer_mount_targets("/dev/sda", 1) == -ENOSPC);
    assert(format_calls == previous_formats && mutations == 2);
    installer_partition_auto = 1;
    capacity_bytes = UINT64_MAX;
    assert(installer_mount_targets("/dev/sda", 1) < 0);
    assert(format_calls == previous_formats && mutations == 2);
    capacity_bytes = 0; capacity_inodes = 10000000;
    assert(installer_mount_targets("/dev/sda", 1) == -ENOSPC);
    assert(format_calls == previous_formats && mutations == 2);
    puts("PASS installer mount transition, raw BIOS role and capacity refusal before formatting");
    return 0;
}
