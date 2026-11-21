#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mntent.h>
#include <unistd.h>
#include "../../userland/apps/installer/installer_plan.h"

static int fixture_open(const char *, int, ...);
static int fixture_ioctl(int, unsigned long, ...);
static FILE *fixture_mounts(const char *, const char *);
#define open fixture_open
#define ioctl fixture_ioctl
#define setmntent fixture_mounts
#include "../../userland/runtime/src/blockdev.c"
#undef open
#undef ioctl
#undef setmntent

static const char *image;
static int readonly, mounted;
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    if (!strcmp(path, "/dev/sda")) return fixture_open(path, flags);
    return __real_open(path, flags, mode);
}
static int fixture_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/urandom")) return __real_open(path, flags);
    if (strcmp(path, "/dev/sda")) { errno = ENOENT; return -1; }
    return __real_open(image, flags, 0600);
}
static int fixture_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request);
    if (request == BLKGETSIZE64) {
        struct stat status; assert(!fstat(fd, &status));
        *va_arg(args, uint64_t *) = (uint64_t)status.st_size;
    } else if (request == BLKSSZGET) *va_arg(args, int *) = 512;
    else if (request == BLKROGET) *va_arg(args, int *) = readonly;
    else assert(request == BLKRRPART);
    va_end(args); return 0;
}
static FILE *fixture_mounts(const char *path, const char *mode)
{
    (void)path; (void)mode;
    static char text[] = "/dev/sda1 /target ext4 rw 0 0\n";
    FILE *stream = tmpfile();
    assert(stream);
    if (mounted) assert(fputs(text, stream) >= 0);
    rewind(stream);
    return stream;
}

static void prepare(void)
{
    int initialized = reliefos_block_gpt_initialize("/dev/sda", 1);
    if (initialized) fprintf(stderr, "initialize result=%d errno=%d\n", initialized, errno);
    assert(!initialized);
    unsigned index;
    assert(!reliefos_block_gpt_create("/dev/sda", 0, 1, "BIOS", &index) && index == 0);
    assert(!reliefos_block_gpt_set_type("/dev/sda", 0, RELIEFOS_BLOCK_GPT_BIOS_BOOT));
    assert(!reliefos_block_gpt_create("/dev/sda", 0, 128, "ESP", &index) && index == 1);
    assert(!reliefos_block_gpt_set_type("/dev/sda", 1, RELIEFOS_BLOCK_GPT_ESP));
    assert(!reliefos_block_gpt_create("/dev/sda", RELIEFOS_BLOCK_FILESYSTEM_EXT4, 256, "Root", &index) && index == 2);
    assert(!reliefos_block_gpt_create("/dev/sda", 0, 16, "Data", &index) && index == 3);
    int fd = __real_open(image, O_RDWR);
    assert(fd >= 0);
    /* Existing user data must survive all preview and unrelated apply paths. */
    const char marker[] = "PRESERVE-EXISTING-DATA";
    assert(pwrite(fd, marker, sizeof(marker), 130ULL * 1024 * 1024 + 8192) == sizeof(marker));
    assert(pwrite(fd, marker, sizeof(marker), 386ULL * 1024 * 1024 + 8192) == sizeof(marker));
    assert(!fsync(fd)); close(fd);
}

static void stage(struct installer_disk_plan *plan)
{
    unsigned index = UINT32_MAX;
    assert(!installer_plan_rename(plan, 3, "Keep Data"));
    assert(!installer_plan_create(plan, 0, 16, "New Root", &index) && index == 4);
    assert(!installer_plan_type(plan, 4, RELIEFOS_BLOCK_GPT_LINUX));
    assert(!installer_plan_resize(plan, 4, 24));
    assert(!installer_plan_format(plan, 4, RELIEFOS_BLOCK_FILESYSTEM_EXT4, "ROOT"));
    unsigned operations = plan->operation_count;
    assert(installer_plan_resize(plan, 2, 128) == -ENOTSUP);
    assert(installer_plan_create(plan, 0, UINT32_MAX, "too large", &index) == -ENOSPC);
    assert(installer_plan_rename(plan, 3, "abcdefghijklmnopqrstuvwxyz0123456789-too-long") == -ENAMETOOLONG);
    assert(installer_plan_format(plan, 0, RELIEFOS_BLOCK_FILESYSTEM_EXT4, "bad") == -EPERM);
    assert(plan->operation_count == operations);
    assert(!installer_plan_delete(plan, 2));
    assert(!installer_plan_create(plan, 0, 32, "Temporary", &index) && index == 2);
    assert(!installer_plan_format(plan, 2, RELIEFOS_BLOCK_FILESYSTEM_EXT4, "SCRATCH"));
    assert(!installer_plan_delete(plan, 2));
    assert(installer_plan_requires_format(plan, 4));
    assert(!installer_plan_requires_format(plan, 2));
    char summary[4096];
    assert(installer_plan_describe(plan, summary, sizeof(summary)) > 0);
    assert(strstr(summary, "Keep Data") && strstr(summary, "Format partition 5") && strstr(summary, "Delete partition 3"));
    assert(installer_plan_describe(plan, summary, 2) == -ENOSPC);
}

int main(int argc, char **argv)
{
    assert(argc == 3); image = argv[1];
    if (!strcmp(argv[2], "prepare")) { prepare(); return 0; }
    struct installer_disk_plan plan = {0};
    struct reliefos_block_disk_info disk;
    assert(!reliefos_block_get_info("/dev/sda", &disk));
    if (!strcmp(argv[2], "read-error-load")) {
        int fd = __real_open(image, O_RDWR); assert(fd >= 0);
        unsigned char changed;
        assert(pread(fd, &changed, 1, 512u + 16u) == 1); changed ^= 0xff;
        assert(pwrite(fd, &changed, 1, 512u + 16u) == 1);
        assert(!fsync(fd)); close(fd);
        assert(installer_plan_load(&plan, &disk) == -EIO);
        assert(!plan.loaded && installer_plan_apply(&plan, 4, 1) == -ENODEV);
        installer_plan_dispose(&plan);
        puts("PASS installer read error is not an empty disk");
        return 0;
    }
    assert(!installer_plan_load(&plan, &disk) && plan.count == 4 && !plan.table_error);
    assert(!installer_plan_check(&plan));
    if (!strncmp(argv[2], "bios-", 5)) {
        assert(!installer_plan_type(&plan, 3, RELIEFOS_BLOCK_GPT_BIOS_BOOT));
        int clear = !strcmp(argv[2], "bios-clear-preview") || !strcmp(argv[2], "bios-clear-apply");
        if (!strcmp(argv[2], "bios-then-data-preview"))
            assert(!installer_plan_type(&plan, 3, RELIEFOS_BLOCK_GPT_BASIC_DATA));
        if (!strcmp(argv[2], "bios-delete-preview"))
            assert(!installer_plan_delete(&plan, 3));
        char summary[4096];
        assert(installer_plan_describe(&plan, summary, sizeof(summary)) > 0);
        const char *warning = "Clear all contents of BIOS Boot partition 4 (16777216 bytes).";
        assert(!!strstr(summary, warning) == clear);
        assert(!strstr(summary, "Clear all contents of BIOS Boot partition 1"));
        if (!strcmp(argv[2], "bios-clear-apply")) {
            /* Root/ESP remain unrelated to the newly reserved BIOS extent. */
            assert(!installer_plan_apply(&plan, 2, 1));
            int fd = __real_open(image, O_RDONLY);
            assert(fd >= 0);
            unsigned char block[4096];
            for (uint64_t at = 386ULL * 1024 * 1024; at < 402ULL * 1024 * 1024; at += sizeof(block)) {
                assert(pread(fd, block, sizeof(block), (off_t)at) == sizeof(block));
                for (unsigned i = 0; i < sizeof(block); ++i) assert(!block[i]);
            }
            close(fd);
        }
        installer_plan_dispose(&plan);
        printf("PASS installer BIOS final-layout summary and clear: %s\n", argv[2]);
        return 0;
    }
    stage(&plan);
    if (!strcmp(argv[2], "apply")) {
        assert(!installer_plan_apply(&plan, 4, 1));
        assert(installer_plan_apply(&plan, 4, 1) == -ESTALE);
        struct reliefos_block_partition parts[128]; uint32_t count;
        assert(!reliefos_block_list_partitions("/dev/sda", parts, 128, &count) && count == 4);
        for (unsigned i = 0; i < count; ++i) {
            assert(parts[i].index != 2);
            if (parts[i].index == 3) assert(!strcmp(parts[i].name, "Keep Data"));
            if (parts[i].index == 4) assert(parts[i].sector_count == 24u * 2048u);
        }
    } else if (!strcmp(argv[2], "readonly") || !strcmp(argv[2], "mounted")) {
        readonly = !strcmp(argv[2], "readonly"); mounted = !strcmp(argv[2], "mounted");
        assert(installer_plan_apply(&plan, 4, 1) == (readonly ? -EROFS : -EBUSY));
    } else if (!strcmp(argv[2], "stale-gpt") || !strcmp(argv[2], "stale-fs")) {
        int fd = __real_open(image, O_RDWR); assert(fd >= 0);
        unsigned char changed;
        uint64_t offset = !strcmp(argv[2], "stale-gpt") ? 512u + 56u : plan.base[3].first_lba * 512u + 1024u;
        assert(pread(fd, &changed, 1, (off_t)offset) == 1); changed ^= 0xff;
        assert(pwrite(fd, &changed, 1, (off_t)offset) == 1); assert(!fsync(fd)); close(fd);
        assert(installer_plan_check(&plan) == -ESTALE);
        assert(installer_plan_apply(&plan, 4, 1) == -ESTALE);
    } else if (!strcmp(argv[2], "initialize-preview")) {
        assert(!installer_plan_initialize(&plan) && plan.count == 0);
        unsigned index;
        assert(!installer_plan_create(&plan, 0, 1, "BIOS", &index));
        assert(!installer_plan_type(&plan, index, RELIEFOS_BLOCK_GPT_BIOS_BOOT));
        assert(!installer_plan_check(&plan));
    } else assert(!strcmp(argv[2], "preview"));
    installer_plan_dispose(&plan);
    puts("PASS installer draft transaction");
    return 0;
}
