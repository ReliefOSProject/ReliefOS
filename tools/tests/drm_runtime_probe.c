/* Runtime contract probe for the ReliefOS primary DRM node.
 *
 * Usage: drm_runtime_probe [expect-absent|expect-present|expect-bo|expect-kms|expect-prime|expect-resources]
 * The first mode captures the pre-implementation behavior by following the
 * actual open/stat syscall path. The second mode checks the registered node,
 * core query ABI, unsupported capabilities, and empty nonblocking event read.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <reliefos/gpu_abi.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINUX_RWF_HIPRI 0x00000001u
#define DRM_IOCTL_VMW_ALLOC_BO \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO, union drm_vmw_alloc_bo_arg)
#define DRM_IOCTL_VMW_UNREF_BO \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_DMABUF, struct drm_vmw_handle_close_arg)
#define DRM_IOCTL_VMW_SYNCCPU \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_SYNCCPU, struct drm_vmw_synccpu_arg)
#define DRM_IOCTL_VMW_GET_PARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM, struct drm_vmw_getparam_arg)
#define DRM_IOCTL_VMW_CREATE_CONTEXT \
    DRM_IOR(DRM_COMMAND_BASE + DRM_VMW_CREATE_CONTEXT, struct drm_vmw_context_arg)
#define DRM_IOCTL_VMW_UNREF_CONTEXT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_CONTEXT, struct drm_vmw_context_arg)
#define DRM_IOCTL_VMW_CREATE_SURFACE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_CREATE_SURFACE, union drm_vmw_surface_create_arg)
#define DRM_IOCTL_VMW_UNREF_SURFACE \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_SURFACE, struct drm_vmw_surface_arg)
#define DRM_IOCTL_VMW_REF_SURFACE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_REF_SURFACE, union drm_vmw_surface_reference_arg)
#define DRM_IOCTL_VMW_GET_3D_CAP \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_GET_3D_CAP, struct drm_vmw_get_3d_cap_arg)
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_RESOURCE_HANDLE_MIN 0x01000000u
#define DRM_DMA_BUF_IOCTL_SYNC 0x40086200u
#define DRM_DMA_BUF_SYNC_READ (1u << 0)
#define DRM_DMA_BUF_SYNC_WRITE (1u << 1)
#define DRM_DMA_BUF_SYNC_RW (DRM_DMA_BUF_SYNC_READ | DRM_DMA_BUF_SYNC_WRITE)
#define DRM_DMA_BUF_SYNC_END (1u << 2)
#define PROBE_SVGA3D_SURFACE_CUBEMAP (1u << 0)
#define PROBE_SVGA3D_SURFACE_HINT_VERTEXBUFFER (1u << 4)
#define PROBE_SVGA3D_SURFACE_HINT_TEXTURE (1u << 5)
#define PROBE_SVGA3D_A8R8G8B8 2u
#define PROBE_SVGA3D_BUFFER 37u

static uint32_t linux_major(uint64_t dev)
{
    return (uint32_t)((dev >> 8) & 0xfffU) |
           (uint32_t)((dev >> 32) & 0xfffff000U);
}

static uint32_t linux_minor(uint64_t dev)
{
    return (uint32_t)(dev & 0xffU) |
           (uint32_t)((dev >> 12) & 0xffffff00U);
}

static volatile sig_atomic_t interrupt_alarm_received;

static void interrupt_alarm_handler(int signal_number)
{
    (void)signal_number;
    interrupt_alarm_received = 1;
}

static void sysfs_step(const char *name)
{
    printf("DRM_SYSFS_STEP %s\n", name);
    fflush(stdout);
}

static int read_attribute(const char *name, const char *path,
                          char *buffer, size_t capacity)
{
    int fd;
    ssize_t count;
    char step[64];
    if (!capacity) return -1;
    snprintf(step, sizeof(step), "%s.open.begin", name);
    sysfs_step(step);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(step, sizeof(step), "%s.open.returned", name);
    sysfs_step(step);
    if (fd < 0) return -1;
    snprintf(step, sizeof(step), "%s.read.begin", name);
    sysfs_step(step);
    count = read(fd, buffer, capacity - 1);
    snprintf(step, sizeof(step), "%s.read.returned", name);
    sysfs_step(step);
    int saved_errno = errno;
    close(fd);
    snprintf(step, sizeof(step), "%s.close.returned", name);
    sysfs_step(step);
    if (count < 0) {
        errno = saved_errno;
        return -1;
    }
    buffer[count] = 0;
    return (int)count;
}

static int check_sysfs_identity(void)
{
    struct stat st;
    char link[32] = {0};
    char subsystem[128] = {0};
    char uevent[512] = {0};
    char device_number[32] = {0};
    sysfs_step("class-device.readlink.begin");
    int link_length = readlink("/sys/class/drm/card0/device", link,
                               sizeof(link) - 1);
    sysfs_step("class-device.readlink.returned");
    if (link_length < 0) return 1;
    link[link_length] = 0;
    if (strcmp(link, "../..")) return 2;
    sysfs_step("pci-subsystem.readlink.begin");
    int subsystem_length = readlink("/sys/dev/char/226:0/device/subsystem",
                                    subsystem, sizeof(subsystem) - 1);
    sysfs_step("pci-subsystem.readlink.returned");
    if (subsystem_length < 0) return 8;
    subsystem[subsystem_length] = 0;
    const char *subsystem_name = strrchr(subsystem, '/');
    if (!subsystem_name || strcmp(subsystem_name + 1, "pci")) return 9;
    sysfs_step("class-card.stat.begin");
    if (stat("/sys/class/drm/card0", &st) < 0 || !S_ISDIR(st.st_mode))
        return 3;
    sysfs_step("class-card.stat.returned");
    if (read_attribute("uevent", "/sys/dev/char/226:0/device/uevent",
                       uevent, sizeof(uevent)) < 0)
        return 4;
    if (!strstr(uevent, "PCI_ID=15AD:0405\n") ||
        !strstr(uevent, "PCI_SLOT_NAME=0000:") ||
        !strstr(uevent, "MODALIAS=pci:"))
        return 5;
    if (read_attribute("drm-dev", "/sys/dev/char/226:0/device/drm/card0/dev",
                       device_number, sizeof(device_number)) < 0 ||
        strcmp(device_number, "226:0\n"))
        return 6;
    errno = 0;
    sysfs_step("render-stat.begin");
    if (stat("/dev/dri/renderD128", &st) == 0 || errno != ENOENT)
        return 7;
    sysfs_step("render-stat.returned");
    printf("DRM_SYSFS device=%s PCI_ID=15AD:0405 primary=226:0 render=absent\n",
           link);
    return 0;
}

static int check_open_file_lifetime(int fd)
{
    struct drm_auth original_magic = {0}, duplicate_magic = {0};
    struct drm_auth independent_magic = {0};
    struct drm_version version = {0};
    char name[16] = {0};
    int duplicate;
    int independent = -1;
    int result = 0;

    sysfs_step("lifetime.dup.begin");
    duplicate = dup(fd);
    sysfs_step("lifetime.dup.returned");
    if (duplicate < 0) {
        sysfs_step("lifetime.close-original.begin");
        close(fd);
        sysfs_step("lifetime.close-original.returned");
        return 1;
    }
    sysfs_step("lifetime.independent-open.begin");
    independent = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    sysfs_step("lifetime.independent-open.returned");
    if (independent < 0) {
        result = 2;
        goto out;
    }
    sysfs_step("lifetime.magic-original.begin");
    int original_result = ioctl(fd, DRM_IOCTL_GET_MAGIC, &original_magic);
    sysfs_step("lifetime.magic-original.returned");
    sysfs_step("lifetime.magic-duplicate.begin");
    int duplicate_result = ioctl(duplicate, DRM_IOCTL_GET_MAGIC, &duplicate_magic);
    sysfs_step("lifetime.magic-duplicate.returned");
    sysfs_step("lifetime.magic-independent.begin");
    int independent_result = ioctl(independent, DRM_IOCTL_GET_MAGIC, &independent_magic);
    sysfs_step("lifetime.magic-independent.returned");
    if (original_result < 0 || duplicate_result < 0 || independent_result < 0) {
        result = 3;
        goto out;
    }
    if (!original_magic.magic || original_magic.magic != duplicate_magic.magic ||
        original_magic.magic == independent_magic.magic) {
        result = 4;
        goto out;
    }
    sysfs_step("lifetime.close-original.begin");
    close(fd);
    sysfs_step("lifetime.close-original.returned");
    fd = -1;
    version.name = name;
    version.name_len = sizeof(name);
    sysfs_step("lifetime.survivor-version.begin");
    int survivor_version_result = ioctl(duplicate, DRM_IOCTL_VERSION, &version);
    sysfs_step("lifetime.survivor-version.returned");
    if (survivor_version_result < 0 || strcmp(name, "vmwgfx"))
        result = 5;
    printf("DRM_OFD duplicate_magic=%u independent_magic=%u survivor=%s\n",
           original_magic.magic, independent_magic.magic,
           result ? "failed" : "usable");
out:
    if (fd >= 0) {
        sysfs_step("lifetime.final-close-original.begin");
        close(fd);
        sysfs_step("lifetime.final-close-original.returned");
    }
    if (duplicate >= 0) {
        sysfs_step("lifetime.final-close-duplicate.begin");
        close(duplicate);
        sysfs_step("lifetime.final-close-duplicate.returned");
    }
    if (independent >= 0) {
        sysfs_step("lifetime.final-close-independent.begin");
        close(independent);
        sysfs_step("lifetime.final-close-independent.returned");
    }
    return result;
}

static int check_positional_read(int fd, const char *operation)
{
    char event[sizeof(struct drm_event)] = {0};
    struct iovec iov = {.iov_base = event, .iov_len = sizeof(event)};
    ssize_t result;
    int saved_errno;
    int expected_errno = EAGAIN;
    char marker[64];

    snprintf(marker, sizeof(marker), "positional.%s.begin", operation);
    sysfs_step(marker);
    errno = 0;
    if (!strcmp(operation, "pread64")) {
        result = pread(fd, event, sizeof(event), 4096);
    } else if (!strcmp(operation, "preadv")) {
        result = preadv(fd, &iov, 1, 4096);
    } else if (!strcmp(operation, "preadv2")) {
        result = preadv2(fd, &iov, 1, 4096, 0);
    } else if (!strcmp(operation, "preadv2-current")) {
        result = preadv2(fd, &iov, 1, -1, 0);
    } else if (!strcmp(operation, "preadv2-hipri")) {
        result = preadv2(fd, &iov, 1, 4096, LINUX_RWF_HIPRI);
    } else if (!strcmp(operation, "preadv2-current-hipri")) {
        result = preadv2(fd, &iov, 1, -1, LINUX_RWF_HIPRI);
    } else if (!strcmp(operation, "preadv-zero-negative")) {
        expected_errno = EINVAL;
        result = preadv(fd, &iov, 0, -1);
    } else if (!strcmp(operation, "preadv2-zero-negative")) {
        expected_errno = EINVAL;
        result = preadv2(fd, &iov, 0, -2, 0);
    } else {
        return 64;
    }
    saved_errno = errno;
    snprintf(marker, sizeof(marker), "positional.%s.returned", operation);
    sysfs_step(marker);
    printf("DRM_POSITIONAL operation=%s result=%ld errno=%d\n",
           operation, (long)result, saved_errno);
    return result == -1 && saved_errno == expected_errno ? 0 : 1;
}

static int check_noop_lseek(int fd)
{
    errno = 0;
    sysfs_step("noop-lseek-set.begin");
    off_t set_result = lseek(fd, 1234, SEEK_SET);
    int set_errno = errno;
    sysfs_step("noop-lseek-set.returned");
    if (set_result != 0) return 1;

    errno = 0;
    sysfs_step("noop-lseek-hole.begin");
    off_t hole_result = lseek(fd, -1, 4); /* Linux SEEK_HOLE / SEEK_MAX. */
    int hole_errno = errno;
    sysfs_step("noop-lseek-hole.returned");
    if (hole_result != 0) return 2;

    errno = 0;
    sysfs_step("noop-lseek-invalid.begin");
    off_t invalid_result = lseek(fd, 0, 5);
    int invalid_errno = errno;
    sysfs_step("noop-lseek-invalid.returned");
    if (invalid_result != -1 || invalid_errno != EINVAL) return 3;
    printf("DRM_LSEEK set=%ld hole=%ld invalid=%d errno=%d\n",
           (long)set_result, (long)hole_result, (int)invalid_result,
           invalid_errno);
    (void)set_errno;
    (void)hole_errno;
    return 0;
}

static int check_interruptible_read(int fd)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    char event[sizeof(struct drm_event)];
    int original_flags;
    int changed_flags = 0;
    int installed_action = 0;
    int result_code = 1;

    sysfs_step("interruptible-read.getfl.begin");
    original_flags = fcntl(fd, F_GETFL);
    sysfs_step("interruptible-read.getfl.returned");
    if (original_flags < 0) return 1;
    action.sa_handler = interrupt_alarm_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* In particular, do not request SA_RESTART. */
    sysfs_step("interruptible-read.sigaction.begin");
    if (sigaction(SIGALRM, &action, &previous_action) < 0) {
        sysfs_step("interruptible-read.sigaction.failed");
        return 2;
    }
    installed_action = 1;
    sysfs_step("interruptible-read.sigaction.returned");
    sysfs_step("interruptible-read.blocking-flags.begin");
    if (fcntl(fd, F_SETFL, original_flags & ~O_NONBLOCK) < 0) {
        sysfs_step("interruptible-read.blocking-flags.failed");
        result_code = 3;
        goto cleanup;
    }
    changed_flags = 1;
    interrupt_alarm_received = 0;
    sysfs_step("interruptible-read.read.begin");
    alarm(1);
    errno = 0;
    ssize_t read_result = read(fd, event, sizeof(event));
    int read_error = errno;
    sysfs_step("interruptible-read.read.returned");
    alarm(0);
    printf("DRM_INTERRUPTIBLE_READ result=%ld errno=%d alarm=%d\n",
           (long)read_result, read_error, (int)interrupt_alarm_received);
    result_code = read_result == -1 && read_error == EINTR &&
                  interrupt_alarm_received ? 0 : 4;

cleanup:
    alarm(0);
    if (changed_flags && fcntl(fd, F_SETFL, original_flags) < 0)
        result_code = 5;
    if (installed_action && sigaction(SIGALRM, &previous_action, NULL) < 0)
        result_code = 6;
    return result_code;
}

static int run_positional_probe(const char *operation)
{
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    int result;
    if (fd < 0) {
        printf("DRM_POSITIONAL open failed errno=%d\n", errno);
        return 10;
    }
    if (!strcmp(operation, "lseek")) result = check_noop_lseek(fd);
    else if (!strcmp(operation, "interruptible"))
        result = check_interruptible_read(fd);
    else result = check_positional_read(fd, operation);
    close(fd);
    return result;
}

static int expect_absent(void)
{
    struct stat st;
    int stat_result = stat("/dev/dri/card0", &st);
    int stat_error = errno;
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    int open_error = errno;

    printf("DRM_RED stat=%d errno=%d open=%d errno=%d\n",
           stat_result, stat_error, fd, open_error);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    if (stat_result == 0 || (stat_error != ENOENT && stat_error != ENODEV) ||
        (open_error != ENOENT && open_error != ENODEV))
        return 2;
    return 0;
}

static int expect_present(void)
{
    struct stat st;
    struct drm_version version = {0};
    struct drm_get_cap cap = {0};
    char name[32] = {0};
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        printf("DRM_PRESENT open failed errno=%d\n", errno);
        return 10;
    }
    if (fstat(fd, &st) < 0) {
        printf("DRM_PRESENT fstat failed errno=%d\n", errno);
        close(fd);
        return 11;
    }
    printf("DRM_PRESENT mode=%o major=%u minor=%u\n",
           (unsigned)(st.st_mode & S_IFMT), linux_major(st.st_rdev),
           linux_minor(st.st_rdev));
    if (!S_ISCHR(st.st_mode) || linux_major(st.st_rdev) != 226 ||
        linux_minor(st.st_rdev) != 0) {
        close(fd);
        return 12;
    }
    int sysfs_result = check_sysfs_identity();
    if (sysfs_result) {
        printf("DRM_PRESENT sysfs identity check failed step=%d errno=%d\n",
               sysfs_result, errno);
        close(fd);
        return 19;
    }

    version.name = name;
    version.name_len = sizeof(name);
    sysfs_step("version.ioctl.begin");
    int version_result = ioctl(fd, DRM_IOCTL_VERSION, &version);
    sysfs_step("version.ioctl.returned");
    if (version_result < 0) {
        printf("DRM_PRESENT version failed errno=%d\n", errno);
        close(fd);
        return 13;
    }
    printf("DRM_VERSION name=%s len=%lu version=%d.%d.%d\n", name,
           (unsigned long)version.name_len, version.version_major,
           version.version_minor, version.version_patchlevel);
    if (strcmp(name, "vmwgfx") || version.version_major != 2 ||
        version.version_minor != 1) {
        close(fd);
        return 14;
    }

    /* PRIME is advertised after the opaque same-device surface path is
     * implemented. KMS-only capabilities remain unsupported. */
    cap.capability = DRM_CAP_PRIME;
    sysfs_step("cap-prime.ioctl.begin");
    int prime_result = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    sysfs_step("cap-prime.ioctl.returned");
    if (prime_result < 0 ||
        cap.value != (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
        printf("DRM_PRESENT PRIME query errno=%d value=%llu\n", errno,
               (unsigned long long)cap.value);
        close(fd);
        return 15;
    }
    cap.capability = DRM_CAP_DUMB_BUFFER;
    sysfs_step("cap-dumb.ioctl.begin");
    int dumb_result = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    sysfs_step("cap-dumb.ioctl.returned");
    printf("DRM_CAP dumb=%d value=%llu errno=%d\n", dumb_result,
           (unsigned long long)cap.value, errno);
    if (dumb_result < 0 || cap.value != 1) {
        close(fd);
        return 16;
    }

    struct pollfd pollfd = {.fd = fd, .events = POLLIN};
    sysfs_step("empty-poll.begin");
    int poll_result = poll(&pollfd, 1, 0);
    sysfs_step("empty-poll.returned");
    if (poll_result != 0 || pollfd.revents != 0) {
        printf("DRM_PRESENT empty poll returned revents=%x\n",
               (unsigned)pollfd.revents);
        close(fd);
        return 17;
    }
    char event[sizeof(struct drm_event)];
    errno = 0;
    sysfs_step("empty-read.begin");
    ssize_t read_result = read(fd, event, sizeof(event));
    sysfs_step("empty-read.returned");
    int read_error = errno;
    printf("DRM_EMPTY_READ result=%ld errno=%d\n",
           (long)read_result, read_error);
    if (read_result != -1 || read_error != EAGAIN) {
        close(fd);
        return 18;
    }
    int lseek_result = check_noop_lseek(fd);
    if (lseek_result) {
        close(fd);
        return 21;
    }
    int interruptible_result = check_interruptible_read(fd);
    if (interruptible_result) {
        close(fd);
        return 22;
    }
    int positional_result = check_positional_read(fd, "pread64");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-current");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-hipri");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-current-hipri");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv-zero-negative");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-zero-negative");
    if (positional_result) {
        close(fd);
        return 23;
    }
    sysfs_step("open-lifetime.begin");
    int lifetime_result = check_open_file_lifetime(fd);
    sysfs_step("open-lifetime.returned");
    if (lifetime_result) {
        printf("DRM_PRESENT open-file lifetime check failed step=%d errno=%d\n",
               lifetime_result, errno);
        return 20;
    }
    return 0;
}

static int bo_failure(const char *stage, int status)
{
    printf("DRM_BO_FAIL stage=%s status=%d errno=%d\n", stage, status, errno);
    return status;
}

static int secondary_primary_bo_child(void)
{
    if (geteuid() == 0) {
        if (setgid(44) < 0 || setuid(1000) < 0) return 81;
    } else if (geteuid() != 1000) {
        return 82;
    }

    int first = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (first < 0) return 83;
    int second = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (second < 0) {
        close(first);
        return 84;
    }

    union drm_vmw_alloc_bo_arg alloc = {0};
    alloc.req.size = 4096;
    if (ioctl(second, DRM_IOCTL_VMW_ALLOC_BO, &alloc) < 0 ||
        !alloc.rep.handle || !alloc.rep.map_handle) {
        close(second);
        close(first);
        return 85;
    }
    struct drm_vmw_synccpu_arg sync = {
        .op = drm_vmw_synccpu_grab,
        .flags = drm_vmw_synccpu_write,
        .handle = alloc.rep.handle,
    };
    if (ioctl(second, DRM_IOCTL_VMW_SYNCCPU, &sync) < 0) {
        close(second);
        close(first);
        return 86;
    }
    sync.op = drm_vmw_synccpu_release;
    sync.flags = drm_vmw_synccpu_read;
    if (ioctl(second, DRM_IOCTL_VMW_SYNCCPU, &sync) < 0 ||
        ioctl(second, DRM_IOCTL_VMW_UNREF_BO,
              &(struct drm_vmw_handle_close_arg){
                  .handle = alloc.rep.handle,
              }) < 0) {
        close(second);
        close(first);
        return 87;
    }
    if (close(second) < 0 || close(first) < 0) return 88;
    return 0;
}

static int check_secondary_primary_bo(void)
{
    pid_t child = fork();
    if (child < 0) return bo_failure("secondary-primary-fork", 80);
    if (child == 0) _exit(secondary_primary_bo_child());
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("DRM_BO_FAIL stage=secondary-primary status=%d errno=%d\n",
               status, errno);
        return 89;
    }
    puts("DRM_BO_SECONDARY_PRIMARY PASS uid=1000 secondary=unauthenticated");
    return 0;
}

struct drm_smp_protect_state {
    volatile uint32_t ready;
    volatile uint32_t release;
    volatile unsigned char *address;
    uint32_t cpu;
};

static int drm_smp_protect_child(void *opaque)
{
    struct drm_smp_protect_state *state = opaque;
    uint64_t mask = UINT64_C(1) << state->cpu;
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(mask), &mask) != 0)
        _exit(91);
    bool on_target_cpu = false;
    for (unsigned poll = 0; poll < 10000; ++poll) {
        unsigned cpu = UINT32_MAX;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0) _exit(92);
        if (cpu == state->cpu) {
            on_target_cpu = true;
            break;
        }
        syscall(SYS_sched_yield);
    }
    if (!on_target_cpu) _exit(93);

    unsigned char observed = state->address[0];
    /* Exercise writable translation and dirty permission before mprotect. */
    state->address[0] = observed;
    __atomic_store_n(&state->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->release, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" : : : "memory");

    /* No syscall or CR3 transition occurs between priming this writable TLB
     * entry, the parent's mprotect, and this store. A stale entry succeeds. */
    state->address[0] = (unsigned char)(observed + 1u);
    _exit(0);
}

static int check_remote_drm_mprotect(int fd)
{
    uint64_t original_mask = 0;
    long mask_size = syscall(SYS_sched_getaffinity, 0,
                             (long)sizeof(original_mask), &original_mask);
    if (mask_size != (long)sizeof(original_mask))
        return bo_failure("mprotect-smp-getaffinity", 94);
    uint32_t first_cpu = UINT32_MAX, second_cpu = UINT32_MAX;
    for (uint32_t cpu = 0; cpu < 64; ++cpu) {
        if (!(original_mask & (UINT64_C(1) << cpu))) continue;
        if (first_cpu == UINT32_MAX) first_cpu = cpu;
        else { second_cpu = cpu; break; }
    }
    if (second_cpu == UINT32_MAX)
        return bo_failure("mprotect-smp-needs-two-cpus", 95);

    uint64_t first_mask = UINT64_C(1) << first_cpu;
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(first_mask),
                &first_mask) != 0)
        return bo_failure("mprotect-smp-pin-parent", 96);

    struct drm_mode_create_dumb create = {
        .width = 1, .height = 1, .bpp = 32,
    };
    struct drm_mode_map_dumb map = {0};
    void *mapping = MAP_FAILED;
    void *child_stack = NULL;
    pid_t child = -1;
    int result = 0, child_status = 0;
    struct drm_smp_protect_state state = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        !create.handle || create.size != 4096) {
        result = bo_failure("mprotect-smp-create", 97);
        goto out;
    }
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        result = bo_failure("mprotect-smp-map-token", 98);
        goto out;
    }
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)map.offset);
    if (mapping == MAP_FAILED) {
        result = bo_failure("mprotect-smp-mmap", 99);
        goto out;
    }
    ((volatile unsigned char *)mapping)[0] = 0x39;
    child_stack = malloc(64 * 1024);
    if (!child_stack) {
        result = bo_failure("mprotect-smp-stack", 100);
        goto out;
    }
    state = (struct drm_smp_protect_state){
        .address = mapping,
        .cpu = second_cpu,
    };
    child = clone(drm_smp_protect_child,
                  (unsigned char *)child_stack + 64 * 1024,
                  CLONE_VM | SIGCHLD, &state);
    if (child < 0) {
        result = bo_failure("mprotect-smp-clone", 101);
        goto out;
    }

    bool child_ready = false;
    for (uint32_t poll = 0; poll < 50000000; ++poll) {
        if (__atomic_load_n(&state.ready, __ATOMIC_ACQUIRE)) {
            child_ready = true;
            break;
        }
        __asm__ volatile("pause" : : : "memory");
    }
    if (!child_ready) {
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        kill(child, SIGKILL);
        waitpid(child, &child_status, 0);
        child = -1;
        result = bo_failure("mprotect-smp-child-ready-timeout", 102);
        goto out;
    }
    if (mprotect(mapping, 4096, PROT_READ) < 0) {
        result = bo_failure("mprotect-smp-revoke", 103);
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        goto out;
    }
    __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
    pid_t waited;
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        child = -1;
        result = bo_failure("mprotect-smp-wait", 104);
        goto out;
    }
    child = -1;
    if (!WIFSIGNALED(child_status) || WTERMSIG(child_status) != SIGSEGV) {
        printf("DRM_BO_FAIL stage=mprotect-smp-stale-write status=%d errno=%d\n",
               child_status, errno);
        result = 105;
        goto out;
    }
    puts("DRM_BO_MPROTECT_SMP PASS shared_mm=1 remote_cpu=1 stale_write=SIGSEGV");

out:
    if (child > 0) {
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (mapping != MAP_FAILED) {
        (void)mprotect(mapping, 4096, PROT_READ | PROT_WRITE);
        if (munmap(mapping, 4096) < 0 && !result)
            result = bo_failure("mprotect-smp-munmap", 106);
    }
    if (create.handle &&
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB,
              &(struct drm_mode_destroy_dumb){.handle = create.handle}) < 0 &&
        !result)
        result = bo_failure("mprotect-smp-destroy", 107);
    free(child_stack);
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(original_mask),
                &original_mask) != 0 && !result)
        result = bo_failure("mprotect-smp-restore-affinity", 108);
    return result;
}

static int expect_bo(void)
{
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    struct drm_gem_close close_gem = {0};
    struct drm_set_version set_version = {
        .drm_di_major = 1, .drm_di_minor = 4,
        .drm_dd_major = 2, .drm_dd_minor = 1,
    };
    int fd = -1, foreign = -1, candidate = -1, reopened = -1;
    void *mapping = MAP_FAILED;
    size_t map_size;
    pid_t child;
    int child_status = 0;

    int secondary = check_secondary_primary_bo();
    if (secondary) return secondary;

    sysfs_step("bo.open.begin");
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    sysfs_step("bo.open.returned");
    if (fd < 0) return bo_failure("open", 30);
    foreign = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (foreign < 0) return bo_failure("foreign-open", 31);

    sysfs_step("bo.cap-dumb.begin");
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 1)
        return bo_failure("cap-dumb", 32);
    cap.capability = DRM_CAP_DUMB_PREFERRED_DEPTH;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 32)
        return bo_failure("cap-depth", 33);
    cap.capability = DRM_CAP_DUMB_PREFER_SHADOW;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 0)
        return bo_failure("cap-shadow", 34);
    sysfs_step("bo.cap-dumb.returned");

    int smp_protect = check_remote_drm_mprotect(fd);
    if (smp_protect) return smp_protect;

    struct drm_mode_create_dumb invalid = {
        .height = 0, .width = 32, .bpp = 32,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &invalid) != -1 ||
        errno != EINVAL || invalid.handle || invalid.pitch || invalid.size)
        return bo_failure("invalid-create", 35);

    /* A read-only output buffer makes Linux copy-to-user fail after the
     * production handler has attempted an allocation. The handle must roll
     * back when that copy fails. */
    void *output_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (output_page == MAP_FAILED) return bo_failure("copyout-map", 36);
    struct drm_mode_create_dumb *faulting = output_page;
    *faulting = (struct drm_mode_create_dumb){
        .height = 4, .width = 64, .bpp = 32,
    };
    if (mprotect(output_page, 4096, PROT_READ) < 0)
        return bo_failure("copyout-protect", 37);
    errno = 0;
    int copyout_result = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, faulting);
    int copyout_errno = errno;
    if (mprotect(output_page, 4096, PROT_READ | PROT_WRITE) < 0)
        return bo_failure("copyout-unprotect", 38);
    if (munmap(output_page, 4096) < 0) return bo_failure("copyout-unmap", 39);
    if (copyout_result != -1 || copyout_errno != EFAULT)
        return bo_failure("copyout-rollback", 40);

    create.width = 1024;
    create.height = 4;
    create.bpp = 32;
    create.flags = 0;
    sysfs_step("bo.create-dumb.begin");
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return bo_failure("create-dumb", 41);
    sysfs_step("bo.create-dumb.returned");
    if (!create.handle || create.pitch != create.width * 4u ||
        create.size < (uint64_t)create.pitch * create.height ||
        (create.size & 4095u))
        return bo_failure("create-layout", 42);
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0 ||
        !map.offset || (map.offset & 4095u))
        return bo_failure("map-dumb", 43);
    int write_only = open("/dev/dri/card0", O_WRONLY | O_CLOEXEC);
    if (write_only < 0) return bo_failure("open-write-only", 75);
    struct drm_mode_create_dumb write_only_create = {
        .width = 1, .height = 1, .bpp = 32,
    };
    if (ioctl(write_only, DRM_IOCTL_MODE_CREATE_DUMB, &write_only_create) < 0)
        return bo_failure("create-write-only", 76);
    struct drm_mode_map_dumb write_only_map = {
        .handle = write_only_create.handle,
    };
    if (ioctl(write_only, DRM_IOCTL_MODE_MAP_DUMB, &write_only_map) < 0)
        return bo_failure("map-write-only", 77);
    errno = 0;
    void *no_read_mapping = mmap(NULL, 4096, PROT_NONE, MAP_SHARED,
                                 write_only, (off_t)write_only_map.offset);
    if (no_read_mapping != MAP_FAILED) {
        munmap(no_read_mapping, 4096);
        return bo_failure("mmap-write-only-accepted", 78);
    }
    if (errno != EACCES ||
        ioctl(write_only, DRM_IOCTL_MODE_DESTROY_DUMB,
              &(struct drm_mode_destroy_dumb){
                  .handle = write_only_create.handle,
              }) < 0 || close(write_only) < 0)
        return bo_failure("mmap-write-only-errno", 79);
    errno = 0;
    void *foreign_map = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, foreign, (off_t)map.offset);
    if (foreign_map != MAP_FAILED) {
        munmap(foreign_map, (size_t)create.size);
        return bo_failure("foreign-map-authorized", 44);
    }
    errno = 0;
    void *private_map = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE, fd, (off_t)map.offset);
    if (private_map != MAP_FAILED) {
        munmap(private_map, (size_t)create.size);
        return bo_failure("private-cow-map-accepted", 45);
    }
    errno = 0;
    void *unaligned_map = mmap(NULL, (size_t)create.size, PROT_READ,
                               MAP_SHARED, fd, (off_t)(map.offset + 1));
    if (unaligned_map != MAP_FAILED) {
        munmap(unaligned_map, (size_t)create.size);
        return bo_failure("unaligned-map-offset", 46);
    }
    map_size = (size_t)create.size;
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)map.offset);
    if (mapping == MAP_FAILED) return bo_failure("mmap-dumb", 47);
    for (size_t i = 0; i < map_size; ++i)
        if (((volatile unsigned char *)mapping)[i] != 0)
            return bo_failure("zero-backing", 48);
    ((volatile unsigned char *)mapping)[0] = 0x31;
    ((volatile unsigned char *)mapping)[2 * 4096] = 0x42;
    void *move_target = mmap(NULL, map_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (move_target == MAP_FAILED || munmap(move_target, map_size) < 0)
        return bo_failure("mremap-target", 49);
    errno = 0;
    void *bad_move = mremap(mapping, map_size, map_size,
                            MREMAP_MAYMOVE | MREMAP_FIXED,
                            (unsigned char *)move_target + 1);
    if (bad_move != MAP_FAILED || errno != EINVAL ||
        ((volatile unsigned char *)mapping)[0] != 0x31)
        return bo_failure("mremap-invalid-fixed-preserves-source", 50);
    void *moved = mremap(mapping, map_size, map_size,
                         MREMAP_MAYMOVE | MREMAP_FIXED, move_target);
    if (moved == MAP_FAILED || moved != move_target ||
        ((volatile unsigned char *)moved)[0] != 0x31 ||
        ((volatile unsigned char *)moved)[2 * 4096] != 0x42)
        return bo_failure("mremap-fixed-move", 51);
    mapping = moved;
    if (mprotect((unsigned char *)mapping + 4096, 4096, PROT_READ) < 0)
        return bo_failure("mprotect-split", 52);
    if (munmap((unsigned char *)mapping + 4096, 4096) < 0)
        return bo_failure("munmap-middle", 53);
    errno = 0;
    void *expanded = mremap(mapping, 4096, 8192, MREMAP_MAYMOVE);
    if (expanded != MAP_FAILED) {
        munmap(expanded, 8192);
        return bo_failure("mremap-expanded-device", 54);
    }
    if (errno != EFAULT) return bo_failure("mremap-error", 55);

    sysfs_step("bo.fork.begin");
    child = fork();
    if (child < 0) return bo_failure("fork", 56);
    if (child == 0) {
        if (((volatile unsigned char *)mapping)[0] != 0x31 ||
            ((volatile unsigned char *)mapping)[2 * 4096] != 0x42)
            _exit(57);
        ((volatile unsigned char *)mapping)[0] = 0x53;
        ((volatile unsigned char *)mapping)[2 * 4096] = 0x64;
        if (close(fd) < 0 || munmap(mapping, 4096) < 0 ||
            munmap((unsigned char *)mapping + 8192, 8192) < 0)
            _exit(58);
        _exit(0);
    }
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        return bo_failure("fork-map-lifetime", 59);
    sysfs_step("bo.fork.returned");
    if (((volatile unsigned char *)mapping)[0] != 0x53 ||
        ((volatile unsigned char *)mapping)[2 * 4096] != 0x64)
        return bo_failure("fork-shared-backing", 60);

    close_gem.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_gem) < 0)
        return bo_failure("gem-close", 61);
    if (((volatile unsigned char *)mapping)[0] != 0x53)
        return bo_failure("map-after-gem-close", 62);
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != -1 || errno != EINVAL)
        return bo_failure("map-handle-after-close", 63);
    if (close(fd) < 0) return bo_failure("close-last-fd", 64);
    fd = -1;
    candidate = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (candidate < 0) return bo_failure("master-lifetime-open", 65);
    errno = 0;
    if (ioctl(candidate, DRM_IOCTL_SET_VERSION, &set_version) != -1 ||
        errno != EACCES)
        return bo_failure("mapped-master-released-early", 66);
    if (munmap(mapping, 4096) < 0 ||
        munmap((unsigned char *)mapping + 8192, 8192) < 0)
        return bo_failure("final-munmap", 67);
    mapping = MAP_FAILED;
    close(candidate);
    candidate = -1;
    reopened = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (reopened < 0 || ioctl(reopened, DRM_IOCTL_SET_VERSION, &set_version) < 0)
        return bo_failure("master-after-munmap", 68);
    close(foreign);
    foreign = -1;

    const uint32_t requested_bo_size = 8193;
    union drm_vmw_alloc_bo_arg alloc = {0};
    alloc.req.size = requested_bo_size;
    sysfs_step("bo.vmwgfx-alloc.begin");
    if (ioctl(reopened, DRM_IOCTL_VMW_ALLOC_BO, &alloc) < 0)
        return bo_failure("vmwgfx-alloc", 66);
    sysfs_step("bo.vmwgfx-alloc.returned");
    if (!alloc.rep.handle || !alloc.rep.map_handle ||
        (alloc.rep.map_handle & 4095u) ||
        alloc.rep.cur_gmr_id != alloc.rep.handle ||
        alloc.rep.cur_gmr_offset != 0)
        return bo_failure("vmwgfx-alloc-identity", 67);
    struct drm_vmw_synccpu_arg sync = {
        .op = drm_vmw_synccpu_grab,
        .flags = drm_vmw_synccpu_read,
        .handle = alloc.rep.handle,
    };
    if (ioctl(reopened, DRM_IOCTL_VMW_SYNCCPU, &sync) < 0)
        return bo_failure("synccpu-grab", 68);
    sync.op = drm_vmw_synccpu_release;
    if (ioctl(reopened, DRM_IOCTL_VMW_SYNCCPU, &sync) < 0 ||
        ioctl(reopened, DRM_IOCTL_VMW_SYNCCPU, &sync) != -1 || errno != EINVAL)
        return bo_failure("synccpu-owner-release", 69);
    sync.op = drm_vmw_synccpu_grab;
    sync.flags = 0;
    if (ioctl(reopened, DRM_IOCTL_VMW_SYNCCPU, &sync) != -1 || errno != EINVAL)
        return bo_failure("synccpu-flags", 70);

    /* The ioctl reply overlays alloc.req, so retain the request size before
     * submission instead of reading the union's now-overwritten request. */
    size_t alloc_size = (size_t)((requested_bo_size + 4095u) & ~4095u);
    void *vmw_mapping = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, reopened, (off_t)alloc.rep.map_handle);
    if (vmw_mapping == MAP_FAILED) return bo_failure("vmwgfx-mmap", 71);
    for (size_t i = 0; i < alloc_size; ++i)
        if (((volatile unsigned char *)vmw_mapping)[i] != 0)
            return bo_failure("vmwgfx-zero-backing", 72);
    ((volatile unsigned char *)vmw_mapping)[0] = 0x75;
    struct drm_vmw_handle_close_arg unref = {.handle = alloc.rep.handle};
    if (ioctl(reopened, DRM_IOCTL_VMW_UNREF_BO, &unref) < 0 ||
        ((volatile unsigned char *)vmw_mapping)[0] != 0x75)
        return bo_failure("vmwgfx-unref-map-lifetime", 73);
    if (munmap(vmw_mapping, alloc_size) < 0 || close(reopened) < 0)
        return bo_failure("vmwgfx-final-release", 74);
    printf("DRM_BO PASS dumb_handle=%u vmw_handle=%u mapped_bytes=%zu\n",
           create.handle, alloc.rep.handle, map_size);
    return 0;
}

static int kms_failure(const char *stage, int status)
{
    printf("DRM_KMS_FAIL stage=%s status=%d errno=%d\n", stage, status, errno);
    return status;
}

static int expect_kms(void)
{
    int fd = -1, foreign = -1, result = 0;
    void *mapping = MAP_FAILED;
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    struct drm_mode_fb_cmd2 fb = {0};
    uint32_t fb_id = 0;
    bool handle_destroyed = false;
    const char *stage = "open";

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return kms_failure(stage, 110);
    struct drm_mode_card_res resources = {0};
    stage = "resources-counts";
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
        resources.count_crtcs != 1 || resources.count_connectors != 1 ||
        resources.count_encoders != 1 || resources.count_fbs != 0)
        { result = kms_failure(stage, 111); goto out; }
    uint32_t crtc_ids[1] = {0}, connector_ids[1] = {0}, encoder_ids[1] = {0};
    resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    resources.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids;
    resources.encoder_id_ptr = (uint64_t)(uintptr_t)encoder_ids;
    resources.count_crtcs = resources.count_connectors = resources.count_encoders = 1;
    stage = "resources-ids";
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
        !crtc_ids[0] || !connector_ids[0] || !encoder_ids[0])
        { result = kms_failure(stage, 112); goto out; }

    struct drm_mode_get_connector connector = {
        .connector_id = connector_ids[0],
    };
    stage = "connector-counts";
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
        connector.count_modes != 1 || connector.count_encoders != 1 ||
        connector.count_props != 0 ||
        connector.connector_type != DRM_MODE_CONNECTOR_VIRTUAL ||
        connector.connection != 1 || !connector.encoder_id)
        { result = kms_failure(stage, 113); goto out; }
    struct drm_mode_modeinfo modes[1] = {{0}};
    uint32_t connector_encoder_ids[1] = {0};
    connector.modes_ptr = (uint64_t)(uintptr_t)modes;
    connector.count_modes = 1;
    connector.encoders_ptr = (uint64_t)(uintptr_t)connector_encoder_ids;
    connector.count_encoders = 1;
    stage = "connector-data";
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
        modes[0].hdisplay == 0 || modes[0].vdisplay == 0 ||
        !(modes[0].type & DRM_MODE_TYPE_PREFERRED) ||
        connector_encoder_ids[0] != encoder_ids[0])
        { result = kms_failure(stage, 114); goto out; }

    struct drm_mode_get_encoder encoder = {.encoder_id = encoder_ids[0]};
    stage = "encoder";
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0 ||
        encoder.encoder_type != DRM_MODE_ENCODER_VIRTUAL ||
        encoder.possible_crtcs != 1 || encoder.possible_clones)
        { result = kms_failure(stage, 115); goto out; }
    struct drm_mode_crtc crtc = {.crtc_id = crtc_ids[0]};
    stage = "crtc-disabled";
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || crtc.mode_valid || crtc.fb_id)
        { result = kms_failure(stage, 116); goto out; }
    struct drm_mode_obj_get_properties properties = {
        .obj_id = connector_ids[0], .obj_type = DRM_MODE_OBJECT_CONNECTOR,
        .props_ptr = 1, .prop_values_ptr = 1,
    };
    stage = "connector-empty-properties";
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) < 0 ||
        properties.count_props != 0)
        { result = kms_failure(stage, 117); goto out; }
    properties.obj_id = crtc_ids[0];
    properties.obj_type = DRM_MODE_OBJECT_CRTC;
    stage = "crtc-empty-properties";
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) < 0 ||
        properties.count_props != 0)
        { result = kms_failure(stage, 118); goto out; }
    properties.obj_id = UINT32_MAX;
    errno = 0;
    stage = "invalid-object-properties";
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) != -1 ||
        errno != ENOENT)
        { result = kms_failure(stage, 119); goto out; }

    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    stage = "dumb-capability";
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 1)
        { result = kms_failure(stage, 120); goto out; }
    cap.capability = DRM_CAP_VBLANK_HIGH_CRTC;
    errno = 0;
    stage = "vblank-capability-unsupported";
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != -1 || errno != EOPNOTSUPP)
        { result = kms_failure(stage, 121); goto out; }
    struct drm_mode_atomic atomic = {0};
    errno = 0;
    stage = "atomic-unsupported";
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) != -1 || errno != EOPNOTSUPP)
        { result = kms_failure(stage, 122); goto out; }
    struct drm_mode_crtc_page_flip flip = {.crtc_id = crtc_ids[0]};
    errno = 0;
    stage = "pageflip-unsupported";
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != -1 || errno != EOPNOTSUPP)
        { result = kms_failure(stage, 123); goto out; }

    create.width = modes[0].hdisplay;
    create.height = modes[0].vdisplay;
    create.bpp = 32;
    stage = "create-dumb";
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        !create.handle || create.pitch < create.width * 4u ||
        create.size < (uint64_t)create.pitch * create.height)
        { result = kms_failure(stage, 124); goto out; }
    map.handle = create.handle;
    stage = "map-dumb";
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0 || !map.offset)
        { result = kms_failure(stage, 125); goto out; }
    mapping = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)map.offset);
    if (mapping == MAP_FAILED) { result = kms_failure("mmap-dumb", 126); goto out; }
    for (uint32_t y = 0; y < create.height; ++y) {
        volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)mapping +
                                   (uint64_t)y * create.pitch);
        for (uint32_t x = 0; x < create.width; ++x)
            row[x] = 0xff000000u | ((x * 13u + y * 7u) & 0x00ffffffu);
    }
    fb.width = create.width;
    fb.height = create.height;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = create.handle;
    fb.pitches[0] = create.pitch;
    stage = "addfb2";
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || !fb.fb_id)
        { result = kms_failure(stage, 127); goto out; }
    fb_id = fb.fb_id;
    struct drm_mode_fb_cmd2 bad_fb = fb;
    bad_fb.fb_id = 0;
    bad_fb.offsets[0] = (uint32_t)create.size;
    errno = 0;
    stage = "addfb2-bounds";
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &bad_fb) != -1 || errno != EINVAL)
        { result = kms_failure(stage, 128); goto out; }

    foreign = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (foreign < 0) { result = kms_failure("foreign-open", 129); goto out; }
    struct drm_mode_fb_cmd2 foreign_fb = {
        .width = fb.width, .height = fb.height, .pixel_format = fb.pixel_format,
        .handles = {create.handle}, .pitches = {create.pitch},
    };
    errno = 0;
    stage = "foreign-handle";
    if (ioctl(foreign, DRM_IOCTL_MODE_ADDFB2, &foreign_fb) != -1 ||
        errno != EINVAL || foreign_fb.fb_id)
        { result = kms_failure(stage, 130); goto out; }
    struct drm_mode_fb_dirty_cmd nonmaster_dirty = {.fb_id = fb_id};
    errno = 0;
    stage = "dirty-requires-master";
    if (ioctl(foreign, DRM_IOCTL_MODE_DIRTYFB, &nonmaster_dirty) != -1 ||
        errno != EACCES)
        { result = kms_failure(stage, 131); goto out; }
    close(foreign);
    foreign = -1;

    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uint64_t)(uintptr_t)connector_ids,
        .count_connectors = 1,
        .crtc_id = crtc_ids[0], .fb_id = fb_id,
        .mode_valid = 1, .mode = modes[0],
    };
    struct drm_mode_crtc invalid_set = set;
    ++invalid_set.mode.clock;
    errno = 0;
    stage = "setcrtc-rejects-mode-change";
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &invalid_set) != -1 || errno != EINVAL)
        { result = kms_failure(stage, 132); goto out; }
    stage = "setcrtc-initial-present";
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
        { result = kms_failure(stage, 133); goto out; }
    stage = "sysfs-enabled";
    char status[64] = {0}, mode_text[64] = {0}, expected_mode[64];
    if (read_attribute("connector-status", "/sys/class/drm/card0-Virtual-1/status",
                       status, sizeof(status)) < 0 || strcmp(status, "connected\n") ||
        read_attribute("connector-enabled", "/sys/class/drm/card0-Virtual-1/enabled",
                       status, sizeof(status)) < 0 || strcmp(status, "enabled\n"))
        { result = kms_failure(stage, 134); goto out; }
    snprintf(expected_mode, sizeof(expected_mode), "%ux%u\n",
             create.width, create.height);
    if (read_attribute("connector-mode", "/sys/class/drm/card0-Virtual-1/modes",
                       mode_text, sizeof(mode_text)) < 0 ||
        strcmp(mode_text, expected_mode))
        { result = kms_failure("sysfs-mode", 135); goto out; }

    struct drm_clip_rect clip = {7, 5, 9, 6};
    ((volatile uint32_t *)((uint8_t *)mapping +
        (uint64_t)5 * create.pitch))[7] = 0xff12ab34u;
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb_id, .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&clip,
    };
    stage = "dirtyfb-present";
    if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) < 0)
        { result = kms_failure(stage, 136); goto out; }
    clip.x2 = (uint16_t)(create.width + 1u);
    errno = 0;
    stage = "dirtyfb-bounds";
    if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) != -1 || errno != EINVAL)
        { result = kms_failure(stage, 137); goto out; }

    stage = "drop-master-disables-scanout";
    if (ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL) < 0)
        { result = kms_failure(stage, 138); goto out; }
    if (read_attribute("connector-disabled", "/sys/class/drm/card0-Virtual-1/enabled",
                       status, sizeof(status)) < 0 || strcmp(status, "disabled\n"))
        { result = kms_failure(stage, 139); goto out; }
    crtc.crtc_id = crtc_ids[0];
    stage = "drop-master-crtc-disabled";
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || crtc.mode_valid || crtc.fb_id)
        { result = kms_failure(stage, 140); goto out; }
    stage = "set-master-restores-ownership";
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0 ||
        ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
        { result = kms_failure(stage, 141); goto out; }

    stage = "destroy-handle-with-active-fb";
    if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB,
              &(struct drm_mode_destroy_dumb){.handle = create.handle}) < 0)
        { result = kms_failure(stage, 142); goto out; }
    handle_destroyed = true;
    if (((volatile uint32_t *)((uint8_t *)mapping +
         (uint64_t)5 * create.pitch))[7] != 0xff12ab34u)
        { result = kms_failure("destroyed-handle-map-retained", 143); goto out; }
    stage = "rmfb-active";
    if (ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) < 0)
        { result = kms_failure(stage, 144); goto out; }
    fb_id = 0;
    stage = "rmfb-disables-crtc";
    crtc = (struct drm_mode_crtc){.crtc_id = crtc_ids[0]};
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || crtc.mode_valid || crtc.fb_id)
        { result = kms_failure(stage, 145); goto out; }
    puts("DRM_KMS PASS virtual=1 mode=current scanout=dirtyfb vblank=unsupported pageflip=unsupported atomic=unsupported");

out:
    if (foreign >= 0) close(foreign);
    if (fb_id) (void)ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
    if (!handle_destroyed && create.handle)
        (void)ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                    &(struct drm_mode_destroy_dumb){.handle = create.handle});
    if (mapping != MAP_FAILED && munmap(mapping, (size_t)create.size) < 0 && !result)
        result = kms_failure("munmap", 146);
    if (fd >= 0 && close(fd) < 0 && !result)
        result = kms_failure("close", 147);
    return result;
}

static int resource_failure(const char *stage, int status)
{
    printf("DRM_RESOURCES_FAIL stage=%s status=%d errno=%d\n",
           stage, status, errno);
    return status;
}

static int resource_gpu_info(int fd, struct reliefos_gpu_info *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->version = RELIEFOS_GPU_ABI_VERSION;
    return ioctl(fd, RELIEFOS_IOCTL_GPU_INFO, info);
}

static int open_unprivileged_drm(void)
{
    uid_t old_euid = geteuid();
    gid_t old_egid = getegid();
    if (old_euid != 0)
        return open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

    /* drm_file_open authenticates admin opens. Open this OFD with a real
     * non-admin effective UID, then restore the caller's saved credentials. */
    uid_t unprivileged_uid = getuid() == 0 ? (uid_t)65534 : getuid();
    if (setegid((gid_t)44) < 0) return -1;
    if (seteuid(unprivileged_uid) < 0) {
        int saved_errno = errno;
        (void)setegid(old_egid);
        errno = saved_errno;
        return -1;
    }
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    int open_errno = errno;
    if (seteuid(old_euid) < 0) {
        int restore_errno = errno;
        if (fd >= 0) close(fd);
        errno = restore_errno;
        return -1;
    }
    if (setegid(old_egid) < 0) {
        int restore_errno = errno;
        if (fd >= 0) close(fd);
        errno = restore_errno;
        return -1;
    }
    errno = open_errno;
    return fd;
}

static int expect_prime(void)
{
    int fd = -1, foreign = -1, prime_fd = -1, duplicate_fd = -1;
    int sockets[2] = {-1, -1}, received_fd = -1, gpu_fd = -1;
    int lifetime_sockets[2] = {-1, -1};
    pid_t lifetime_child = -1;
    int result = 0;
    int32_t surface_id = 0;
    uint32_t foreign_references = 0;
    bool owner_reference = false;
    struct drm_vmw_size size = {.width = 64, .height = 64, .depth = 1};
    union drm_vmw_surface_create_arg create = {0};
    struct reliefos_gpu_info before = {0}, during = {0}, after = {0};
    bool have_gpu_info = false;

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return resource_failure("prime-open", 200);
    foreign = open_unprivileged_drm();
    if (foreign < 0) {
        result = resource_failure("prime-foreign-open", 201);
        goto out;
    }
    gpu_fd = open("/dev/gpu", O_RDWR | O_CLOEXEC);
    if (gpu_fd >= 0 && resource_gpu_info(gpu_fd, &before) == 0)
        have_gpu_info = true;

    struct drm_get_cap cap = {.capability = DRM_CAP_PRIME};
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 ||
        cap.value != (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
        result = resource_failure("prime-capability", 202);
        goto out;
    }

    create.req.flags = PROBE_SVGA3D_SURFACE_HINT_TEXTURE;
    create.req.format = PROBE_SVGA3D_A8R8G8B8;
    create.req.mip_levels[0] = 1;
    create.req.size_addr = (uint64_t)(uintptr_t)&size;
    create.req.shareable = 1;
    if (have_gpu_info && resource_gpu_info(gpu_fd, &before) < 0)
        have_gpu_info = false;
    if (ioctl(fd, DRM_IOCTL_VMW_CREATE_SURFACE, &create) < 0 ||
        create.rep.sid < (int32_t)DRM_RESOURCE_HANDLE_MIN) {
        result = resource_failure("prime-create-surface", 203);
        goto out;
    }
    surface_id = create.rep.sid;
    owner_reference = true;
    if (have_gpu_info &&
        (resource_gpu_info(gpu_fd, &during) < 0 ||
         during.surface_bytes != before.surface_bytes + 16384u)) {
        result = resource_failure("prime-surface-accounting", 204);
        goto out;
    }

    struct drm_prime_handle export = {
        .handle = (uint32_t)surface_id,
        .flags = DRM_CLOEXEC | DRM_RDWR,
        .fd = -1,
    };
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export) < 0 || export.fd < 0) {
        result = resource_failure("prime-export", 205);
        goto out;
    }
    prime_fd = export.fd;
    struct stat st;
    int prime_fd_flags = fcntl(prime_fd, F_GETFD);
    if (fstat(prime_fd, &st) < 0 || (st.st_mode & S_IFMT) ||
        (st.st_mode & 0777) != 0600 || st.st_size != 16384 || !st.st_ino ||
        st.st_uid != geteuid() || st.st_gid != getegid() ||
        prime_fd_flags < 0 || !(prime_fd_flags & FD_CLOEXEC)) {
        result = resource_failure("prime-anonymous-fstat-flags", 206);
        goto out;
    }
    duplicate_fd = dup(prime_fd);
    struct stat duplicate_stat;
    int duplicate_fd_flags = duplicate_fd >= 0
                                 ? fcntl(duplicate_fd, F_GETFD) : -1;
    if (duplicate_fd < 0 || duplicate_fd_flags < 0 ||
        (duplicate_fd_flags & FD_CLOEXEC) ||
        fstat(duplicate_fd, &duplicate_stat) < 0 ||
        duplicate_stat.st_ino != st.st_ino || duplicate_stat.st_size != st.st_size) {
        result = resource_failure("prime-dup-shared-ofd", 207);
        goto out;
    }
    errno = 0;
    if (lseek(prime_fd, 0, SEEK_SET) != 0 ||
        lseek(prime_fd, 0, SEEK_END) != st.st_size) {
        result = resource_failure("prime-seek-zero-size", 208);
        goto out;
    }
    errno = 0;
    if (lseek(prime_fd, 1, SEEK_SET) != -1 || errno != EINVAL) {
        result = resource_failure("prime-seek-nonzero", 209);
        goto out;
    }
    struct pollfd ready = {.fd = prime_fd, .events = POLLIN | POLLOUT};
    if (poll(&ready, 1, 0) != 1 ||
        (ready.revents & (POLLIN | POLLOUT)) != (POLLIN | POLLOUT)) {
        result = resource_failure("prime-empty-poll", 210);
        goto out;
    }
    char byte = 0;
    errno = 0;
    if (read(prime_fd, &byte, 1) != -1 || errno != EINVAL) {
        result = resource_failure("prime-unsupported-read", 211);
        goto out;
    }
    errno = 0;
    if (write(prime_fd, &byte, 1) != -1 || errno != EINVAL) {
        result = resource_failure("prime-unsupported-write", 212);
        goto out;
    }
    errno = 0;
    void *mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED,
                         prime_fd, 0);
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, (size_t)st.st_size);
        result = resource_failure("prime-unsupported-mmap", 213);
        goto out;
    }
    if (errno != EINVAL) {
        result = resource_failure("prime-unsupported-mmap-errno", 214);
        goto out;
    }

    struct { uint64_t flags; } sync = {.flags = DRM_DMA_BUF_SYNC_RW};
    if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        result = resource_failure("prime-sync-start-rw", 215);
        goto out;
    }
    sync.flags = DRM_DMA_BUF_SYNC_RW | DRM_DMA_BUF_SYNC_END;
    if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        result = resource_failure("prime-sync-end-rw", 216);
        goto out;
    }
    sync.flags = 0;
    errno = 0;
    if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC, &sync) != -1 || errno != EINVAL) {
        result = resource_failure("prime-sync-invalid-flags", 217);
        goto out;
    }
    errno = 0;
    if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC + 1u, &sync) != -1 ||
        errno != ENOTTY) {
        result = resource_failure("prime-unknown-ioctl", 218);
        goto out;
    }

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) < 0) {
        result = resource_failure("prime-scm-socketpair", 219);
        goto out;
    }
    char marker = 'P';
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct iovec iov = {.iov_base = &marker, .iov_len = sizeof(marker)};
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header) {
        result = resource_failure("prime-scm-control", 220);
        goto out;
    }
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &prime_fd, sizeof(prime_fd));
    if (sendmsg(sockets[0], &message, 0) != (ssize_t)sizeof(marker)) {
        result = resource_failure("prime-scm-send", 221);
        goto out;
    }
    memset(control.bytes, 0, sizeof(control.bytes));
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(sockets[1], &message, MSG_CMSG_CLOEXEC) !=
            (ssize_t)sizeof(marker) ||
        (message.msg_flags & MSG_CTRUNC) || marker != 'P' ||
        !(header = CMSG_FIRSTHDR(&message)) ||
        header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int))) {
        result = resource_failure("prime-scm-receive", 222);
        goto out;
    }
    memcpy(&received_fd, CMSG_DATA(header), sizeof(received_fd));
    int received_fd_flags = received_fd >= 0
                                ? fcntl(received_fd, F_GETFD) : -1;
    if (received_fd < 0 || received_fd_flags < 0 ||
        !(received_fd_flags & FD_CLOEXEC)) {
        result = resource_failure("prime-scm-received-cloexec", 223);
        goto out;
    }

    struct drm_prime_handle imported = {.fd = received_fd};
    if (ioctl(foreign, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imported) < 0 ||
        imported.handle != (uint32_t)surface_id) {
        result = resource_failure("prime-typed-fd-import", 224);
        goto out;
    }
    ++foreign_references;
    union drm_vmw_surface_reference_arg reference = {0};
    reference.req = (struct drm_vmw_surface_arg){
        .sid = received_fd, .handle_type = DRM_VMW_HANDLE_PRIME,
    };
    if (ioctl(foreign, DRM_IOCTL_VMW_REF_SURFACE, &reference) < 0 ||
        reference.rep.format != PROBE_SVGA3D_A8R8G8B8 ||
        reference.rep.shareable != 1 || reference.rep.mip_levels[0] != 1) {
        result = resource_failure("prime-vmwgfx-handle-prime-import", 225);
        goto out;
    }
    ++foreign_references;

    struct drm_vmw_surface_arg owner_unref = {
        .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_SURFACE, &owner_unref) < 0) {
        result = resource_failure("prime-owner-unref", 226);
        goto out;
    }
    owner_reference = false;
    if (close(fd) < 0) {
        fd = -1;
        result = resource_failure("prime-close-exporter", 227);
        goto out;
    }
    fd = -1;
    close(sockets[0]);
    sockets[0] = -1;
    close(sockets[1]);
    sockets[1] = -1;

    struct drm_vmw_surface_arg imported_unref = {
        .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    while (foreign_references) {
        if (ioctl(foreign, DRM_IOCTL_VMW_UNREF_SURFACE, &imported_unref) < 0) {
            result = resource_failure("prime-import-unref", 228);
            goto out;
        }
        --foreign_references;
    }
    close(foreign);
    foreign = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, lifetime_sockets) < 0) {
        result = resource_failure("prime-fork-socketpair", 230);
        goto out;
    }
    /* All card OFDs and typed references are gone before fork. The child
     * closes every inherited PRIME descriptor except prime_fd, so that fd is
     * the sole remaining owner after the parent closes its copies. */
    lifetime_child = fork();
    if (lifetime_child < 0) {
        result = resource_failure("prime-fork", 231);
        goto out;
    }
    if (lifetime_child == 0) {
        close(lifetime_sockets[0]);
        if (fd >= 0) close(fd);
        if (foreign >= 0) close(foreign);
        if (gpu_fd >= 0) close(gpu_fd);
        if (duplicate_fd >= 0) close(duplicate_fd);
        if (received_fd >= 0) close(received_fd);
        char ready_byte = 'R';
        if (write(lifetime_sockets[1], &ready_byte, 1) != 1) _exit(1);
        char continue_byte = 0;
        if (read(lifetime_sockets[1], &continue_byte, 1) != 1 ||
            continue_byte != 'C')
            _exit(2);
        struct stat child_stat;
        if (fstat(prime_fd, &child_stat) < 0 ||
            child_stat.st_ino != st.st_ino || child_stat.st_size != st.st_size)
            _exit(3);
        struct { uint64_t flags; } child_sync = {
            .flags = DRM_DMA_BUF_SYNC_RW,
        };
        if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC, &child_sync) < 0) _exit(4);
        child_sync.flags = DRM_DMA_BUF_SYNC_RW | DRM_DMA_BUF_SYNC_END;
        if (ioctl(prime_fd, DRM_DMA_BUF_IOCTL_SYNC, &child_sync) < 0) _exit(5);
        if (close(prime_fd) < 0) _exit(6);
        char closed_byte = 'D';
        if (write(lifetime_sockets[1], &closed_byte, 1) != 1) _exit(7);
        close(lifetime_sockets[1]);
        _exit(0);
    }
    close(lifetime_sockets[1]);
    lifetime_sockets[1] = -1;
    char ready_byte = 0;
    if (read(lifetime_sockets[0], &ready_byte, 1) != 1 || ready_byte != 'R') {
        result = resource_failure("prime-fork-child-ready", 232);
        goto out;
    }
    if (close(prime_fd) < 0) {
        prime_fd = -1;
        result = resource_failure("prime-fork-parent-close", 233);
        goto out;
    }
    prime_fd = -1;
    if (close(duplicate_fd) < 0) {
        duplicate_fd = -1;
        result = resource_failure("prime-fork-parent-close-duplicate", 234);
        goto out;
    }
    duplicate_fd = -1;
    if (close(received_fd) < 0) {
        received_fd = -1;
        result = resource_failure("prime-fork-parent-close-received", 235);
        goto out;
    }
    received_fd = -1;
    if (have_gpu_info &&
        (resource_gpu_info(gpu_fd, &after) < 0 ||
         after.surface_bytes != before.surface_bytes + 16384u)) {
        result = resource_failure("prime-fork-child-retains-surface", 236);
        goto out;
    }
    char continue_byte = 'C';
    if (write(lifetime_sockets[0], &continue_byte, 1) != 1) {
        result = resource_failure("prime-fork-child-continue", 237);
        goto out;
    }
    char closed_byte = 0;
    if (read(lifetime_sockets[0], &closed_byte, 1) != 1 || closed_byte != 'D') {
        result = resource_failure("prime-fork-child-final-close", 238);
        goto out;
    }
    int child_status = 0;
    pid_t waited_child;
    do {
        waited_child = waitpid(lifetime_child, &child_status, 0);
    } while (waited_child < 0 && errno == EINTR);
    if (waited_child != lifetime_child) {
        result = resource_failure("prime-fork-child-wait", 239);
        goto out;
    }
    lifetime_child = -1;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        result = resource_failure("prime-fork-child-status", 240);
        goto out;
    }
    close(lifetime_sockets[0]);
    lifetime_sockets[0] = -1;
    if (have_gpu_info) {
        bool retired = false;
        for (uint32_t attempt = 0; attempt < 2000; ++attempt) {
            (void)getpid();
            if (resource_gpu_info(gpu_fd, &after) == 0 &&
                after.surface_bytes == before.surface_bytes) {
                retired = true;
                break;
            }
            sched_yield();
        }
        if (!retired) {
            result = resource_failure("prime-final-retirement", 229);
            goto out;
        }
    }
    puts("DRM_PRIME PASS opaque=0600 same-device=typed scm-rights=1 sync=rw "
         "seek=paged-size poll=ready final-close=retired");
    printf("DRM_PRIME_FORK PASS child-last-close=%s\n",
           have_gpu_info ? "retired" : "closed");

out:
    if (lifetime_child > 0) {
        int child_status = 0;
        (void)kill(lifetime_child, SIGKILL);
        (void)waitpid(lifetime_child, &child_status, 0);
    }
    if (lifetime_sockets[0] >= 0) close(lifetime_sockets[0]);
    if (lifetime_sockets[1] >= 0) close(lifetime_sockets[1]);
    if (sockets[0] >= 0) close(sockets[0]);
    if (sockets[1] >= 0) close(sockets[1]);
    if (received_fd >= 0) close(received_fd);
    if (duplicate_fd >= 0) close(duplicate_fd);
    if (prime_fd >= 0) close(prime_fd);
    if (owner_reference && fd >= 0 && surface_id > 0) {
        struct drm_vmw_surface_arg unref = {
            .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
        };
        (void)ioctl(fd, DRM_IOCTL_VMW_UNREF_SURFACE, &unref);
    }
    if (foreign >= 0) {
        struct drm_vmw_surface_arg unref = {
            .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
        };
        while (foreign_references) {
            if (ioctl(foreign, DRM_IOCTL_VMW_UNREF_SURFACE, &unref) < 0) break;
            --foreign_references;
        }
        close(foreign);
    }
    if (fd >= 0) close(fd);
    if (gpu_fd >= 0) close(gpu_fd);
    return result;
}

static int expect_resources(void)
{
    int fd = -1, foreign = -1, gpu_fd = -1, result = 0;
    struct drm_vmw_context_arg context = {0};
    union drm_vmw_surface_create_arg create = {0};
    union drm_vmw_surface_reference_arg reference = {0};
    struct drm_vmw_size sizes[2] = {
        {.width = 64, .height = 64, .depth = 1},
        {.width = 32, .height = 32, .depth = 1},
    };
    struct reliefos_gpu_info before = {0}, during = {0}, after = {0};
    struct reliefos_gpu_info buffer_before = {0}, buffer_during = {0};
    struct reliefos_gpu_info buffer_after = {0};
    bool have_gpu_info = false;

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return resource_failure("open", 150);
    foreign = open_unprivileged_drm();
    if (foreign < 0) {
        result = resource_failure("foreign-open", 151);
        goto out;
    }
    gpu_fd = open("/dev/gpu", O_RDWR | O_CLOEXEC);
    if (gpu_fd >= 0 && resource_gpu_info(gpu_fd, &before) == 0)
        have_gpu_info = true;

    struct drm_vmw_getparam_arg param = {
        .param = DRM_VMW_PARAM_3D,
        .pad64 = 0xfeed1234u,
    };
    sysfs_step("resources.get-param-3d.begin");
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) < 0 ||
        (param.value != 0 && param.value != 1)) {
        result = resource_failure("get-param-3d-not-ready", 152);
        goto out;
    }
    printf("DRM_RESOURCES PARAM_3D value=%lld\n", (long long)param.value);
    sysfs_step("resources.get-param-3d.returned");
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_FIFO_HW_VERSION};
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) < 0 || !param.value) {
        result = resource_failure("get-param-fifo-version", 153);
        goto out;
    }
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_MAX_SURF_MEMORY,
    };
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) < 0 ||
        param.value <= 0 || param.value > 128 * 1024 * 1024) {
        result = resource_failure("get-param-max-surf-memory", 182);
        goto out;
    }
    printf("DRM_RESOURCES MAX_SURF_MEMORY bytes=%lld\n",
           (long long)param.value);
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_3D_CAPS_SIZE};
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) < 0 ||
        param.value < 12 || param.value > 65536) {
        result = resource_failure("get-param-caps-size", 154);
        goto out;
    }
    const uint32_t caps_size = (uint32_t)param.value;
    uint32_t *caps = calloc(1, caps_size);
    if (!caps) {
        result = resource_failure("caps-allocation", 155);
        goto out;
    }
    struct drm_vmw_get_3d_cap_arg cap_arg = {
        .buffer = (uint64_t)(uintptr_t)caps,
        .max_size = caps_size,
    };
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) < 0) {
        free(caps);
        result = resource_failure("get-3d-cap-full", 156);
        goto out;
    }
    uint32_t record_dwords = caps[0];
    uint64_t record_bytes = (uint64_t)record_dwords * sizeof(uint32_t);
    if (record_dwords < 2 || (record_dwords & 1u) || caps[1] != 0x100u ||
        record_bytes + sizeof(uint32_t) > caps_size ||
        caps[record_dwords] != 0) {
        free(caps);
        result = resource_failure("get-3d-cap-record", 157);
        goto out;
    }
    uint32_t prior_cap = 0;
    for (uint32_t word = 2; word < record_dwords; word += 2) {
        uint32_t index = caps[word];
        if (index >= 512 || (word != 2 && index <= prior_cap)) {
            free(caps);
            result = resource_failure("get-3d-cap-pairs", 158);
            goto out;
        }
        prior_cap = index;
    }
    free(caps);

    uint32_t short_record[2] = {0};
    cap_arg = (struct drm_vmw_get_3d_cap_arg){
        .buffer = (uint64_t)(uintptr_t)short_record,
        .max_size = sizeof(short_record),
    };
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) < 0 ||
        short_record[0] != 2 || short_record[1] != 0x100u) {
        result = resource_failure("get-3d-cap-truncated", 159);
        goto out;
    }
    cap_arg.max_size = 0;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) != -1 || errno != EINVAL) {
        result = resource_failure("get-3d-cap-zero-size", 160);
        goto out;
    }
    cap_arg.max_size = sizeof(uint32_t);
    cap_arg.buffer = 1;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) != -1 || errno != EINVAL) {
        result = resource_failure("get-3d-cap-short-before-pointer", 161);
        goto out;
    }
    cap_arg.max_size = sizeof(short_record);
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) != -1 || errno != EFAULT) {
        result = resource_failure("get-3d-cap-bad-pointer", 161);
        goto out;
    }
    cap_arg.buffer = (uint64_t)(uintptr_t)short_record;
    cap_arg.max_size = sizeof(short_record);
    cap_arg.pad64 = 1;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_3D_CAP, &cap_arg) != -1 || errno != EINVAL) {
        result = resource_failure("get-3d-cap-pad", 162);
        goto out;
    }
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_HW_CAPS};
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) != -1 || errno != EINVAL) {
        result = resource_failure("get-param-hw-caps-legacy-profile", 163);
        goto out;
    }
    param = (struct drm_vmw_getparam_arg){.param = UINT32_MAX};
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_GET_PARAM, &param) != -1 || errno != EINVAL) {
        result = resource_failure("get-param-unknown", 164);
        goto out;
    }

    struct drm_vmw_size byte_buffer_size = {
        .width = 32u * 1024u, .height = 1u, .depth = 1u,
    };
    create = (union drm_vmw_surface_create_arg){0};
    create.req.flags = PROBE_SVGA3D_SURFACE_HINT_VERTEXBUFFER;
    create.req.format = PROBE_SVGA3D_BUFFER;
    create.req.mip_levels[0] = 1;
    create.req.size_addr = (uint64_t)(uintptr_t)&byte_buffer_size;
    if (have_gpu_info && resource_gpu_info(gpu_fd, &buffer_before) < 0)
        have_gpu_info = false;
    if (ioctl(fd, DRM_IOCTL_VMW_CREATE_SURFACE, &create) < 0 ||
        create.rep.sid < (int32_t)DRM_RESOURCE_HANDLE_MIN) {
        result = resource_failure("create-surface-32k-buffer", 183);
        goto out;
    }
    const int32_t byte_buffer_sid = create.rep.sid;
    if (have_gpu_info &&
        (resource_gpu_info(gpu_fd, &buffer_during) < 0 ||
         buffer_during.surface_bytes != buffer_before.surface_bytes + 32768u)) {
        result = resource_failure("surface-counter-32k-buffer", 184);
        goto out;
    }
    struct drm_vmw_surface_arg byte_buffer_unref = {
        .sid = byte_buffer_sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_SURFACE, &byte_buffer_unref) < 0) {
        result = resource_failure("unref-32k-buffer", 185);
        goto out;
    }
    if (have_gpu_info) {
        bool buffer_retired = false;
        for (uint32_t attempt = 0; attempt < 2000; ++attempt) {
            (void)getpid();
            if (resource_gpu_info(gpu_fd, &buffer_after) == 0 &&
                buffer_after.surface_bytes == buffer_before.surface_bytes) {
                buffer_retired = true;
                break;
            }
            sched_yield();
        }
        if (!buffer_retired) {
            result = resource_failure("retire-32k-buffer", 186);
            goto out;
        }
        printf("DRM_RESOURCES_BUFFER_GPU_INFO observed bytes before=%llu "
               "during=%llu after=%llu\n",
               (unsigned long long)buffer_before.surface_bytes,
               (unsigned long long)buffer_during.surface_bytes,
               (unsigned long long)buffer_after.surface_bytes);
    }
    puts("DRM_RESOURCES_BUFFER PASS bytes=32768");

    if (ioctl(fd, DRM_IOCTL_VMW_CREATE_CONTEXT, &context) < 0 ||
        context.cid < (int32_t)DRM_RESOURCE_HANDLE_MIN || context.pad64) {
        result = resource_failure("create-context", 165);
        goto out;
    }
    struct drm_vmw_context_arg wrong_context = {.cid = context.cid};
    errno = 0;
    if (ioctl(foreign, DRM_IOCTL_VMW_UNREF_CONTEXT, &wrong_context) != -1 ||
        errno != EINVAL) {
        result = resource_failure("context-per-file", 166);
        goto out;
    }

    create.req.flags = PROBE_SVGA3D_SURFACE_HINT_TEXTURE;
    create.req.format = PROBE_SVGA3D_A8R8G8B8;
    create.req.mip_levels[0] = 2;
    create.req.size_addr = (uint64_t)(uintptr_t)sizes;
    if (have_gpu_info && resource_gpu_info(gpu_fd, &before) < 0)
        have_gpu_info = false;
    if (ioctl(fd, DRM_IOCTL_VMW_CREATE_SURFACE, &create) < 0 ||
        create.rep.sid < (int32_t)DRM_RESOURCE_HANDLE_MIN) {
        result = resource_failure("create-surface-2d-mips", 167);
        goto out;
    }
    const int32_t surface_id = create.rep.sid;
    if (surface_id == context.cid) {
        result = resource_failure("resource-name-type-isolation", 168);
        goto out;
    }
    if (have_gpu_info &&
        (resource_gpu_info(gpu_fd, &during) < 0 ||
         during.surface_bytes != before.surface_bytes + 20480u)) {
        result = resource_failure("surface-counter-after-create", 169);
        goto out;
    }

    memset(&reference, 0, sizeof(reference));
    reference.req = (struct drm_vmw_surface_arg){
        .sid = surface_id,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    reference.rep.size_addr = (uint64_t)(uintptr_t)&sizes[0];
    errno = 0;
    if (ioctl(foreign, DRM_IOCTL_VMW_REF_SURFACE, &reference) != -1 ||
        errno != EACCES) {
        result = resource_failure("ref-surface-requires-auth", 170);
        goto out;
    }
    struct drm_auth auth = {0};
    if (ioctl(foreign, DRM_IOCTL_GET_MAGIC, &auth) < 0 || !auth.magic ||
        ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) < 0) {
        result = resource_failure("ref-surface-authenticate", 171);
        goto out;
    }
    memset(&reference, 0, sizeof(reference));
    reference.req = (struct drm_vmw_surface_arg){
        .sid = surface_id,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    reference.rep.size_addr = (uint64_t)(uintptr_t)&sizes[0];
    sizes[0] = (struct drm_vmw_size){0};
    if (ioctl(foreign, DRM_IOCTL_VMW_REF_SURFACE, &reference) < 0 ||
        reference.rep.flags != PROBE_SVGA3D_SURFACE_HINT_TEXTURE ||
        reference.rep.format != PROBE_SVGA3D_A8R8G8B8 ||
        reference.rep.mip_levels[0] != 2 || reference.rep.mip_levels[1] ||
        sizes[0].width != 64 || sizes[0].height != 64 || sizes[0].depth != 1) {
        result = resource_failure("ref-surface-metadata", 172);
        goto out;
    }
    struct drm_vmw_surface_arg wrong_surface = {
        .sid = context.cid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_SURFACE, &wrong_surface) != -1 ||
        errno != EINVAL) {
        result = resource_failure("surface-rejects-context-id", 173);
        goto out;
    }
    struct drm_vmw_context_arg wrong_type = {.cid = surface_id};
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_CONTEXT, &wrong_type) != -1 ||
        errno != EINVAL) {
        result = resource_failure("context-rejects-surface-id", 174);
        goto out;
    }
    struct drm_vmw_surface_arg secondary_unref = {
        .sid = surface_id,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    if (ioctl(foreign, DRM_IOCTL_VMW_UNREF_SURFACE, &secondary_unref) < 0) {
        result = resource_failure("unref-surface-secondary", 175);
        goto out;
    }
    struct drm_vmw_surface_arg prime_ref = {
        .sid = -1,
        .handle_type = DRM_VMW_HANDLE_PRIME,
    };
    memset(&reference, 0, sizeof(reference));
    reference.req = prime_ref;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_REF_SURFACE, &reference) != -1 ||
        errno != EBADF) {
        result = resource_failure("ref-surface-prime-invalid-fd", 176);
        goto out;
    }
    prime_ref.sid = INT32_MAX;
    reference.req = prime_ref;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_REF_SURFACE, &reference) != -1 ||
        errno != EBADF) {
        result = resource_failure("ref-surface-prime-unallocated-fd", 241);
        goto out;
    }
    prime_ref.sid = fd;
    reference.req = prime_ref;
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_VMW_REF_SURFACE, &reference) != -1 ||
        errno != EINVAL) {
        result = resource_failure("ref-surface-prime-valid-wrong-object", 242);
        goto out;
    }
    struct drm_vmw_surface_arg unref_surface = {
        .sid = surface_id,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_SURFACE, &unref_surface) < 0) {
        result = resource_failure("unref-surface-final", 177);
        goto out;
    }
    struct drm_vmw_context_arg unref_context = {.cid = context.cid};
    if (ioctl(fd, DRM_IOCTL_VMW_UNREF_CONTEXT, &unref_context) < 0) {
        result = resource_failure("unref-context", 178);
        goto out;
    }
    context.cid = 0;
    if (close(foreign) < 0) {
        foreign = -1;
        result = resource_failure("close-secondary", 179);
        goto out;
    }
    foreign = -1;
    if (close(fd) < 0) {
        fd = -1;
        result = resource_failure("close-final-drm-file", 180);
        goto out;
    }
    fd = -1;
    if (have_gpu_info) {
        bool retired = false;
        for (uint32_t attempt = 0; attempt < 2000; ++attempt) {
            (void)getpid(); /* unrelated syscalls drive bounded retirement */
            if (resource_gpu_info(gpu_fd, &after) == 0 &&
                after.surface_bytes == before.surface_bytes) {
                retired = true;
                break;
            }
            sched_yield();
        }
        if (!retired) {
            result = resource_failure("final-close-surface-retirement", 181);
            goto out;
        }
    }
    if (have_gpu_info) {
        printf("DRM_RESOURCES_GPU_INFO observed surface_bytes before=%llu during=%llu after=%llu\n",
               (unsigned long long)before.surface_bytes,
               (unsigned long long)during.surface_bytes,
               (unsigned long long)after.surface_bytes);
        puts("DRM_RESOURCES PASS contexts=1 surface=2d+mips sharing=authenticated \
             prime=invalid-fd-rejected final-close=observed-retired");
    } else {
        puts("DRM_RESOURCES_GPU_INFO skip=unavailable-or-denied");
        puts("DRM_RESOURCES PASS contexts=1 surface=2d+mips sharing=authenticated \
             prime=invalid-fd-rejected final-close=not-observed");
    }

out:
    if (foreign >= 0) close(foreign);
    if (fd >= 0) close(fd);
    if (gpu_fd >= 0) close(gpu_fd);
    return result;
}

int main(int argc, char **argv)
{
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0)
        return 70;
    if (argc == 2 && !strcmp(argv[1], "expect-absent"))
        return expect_absent();
    if (argc == 2 && !strcmp(argv[1], "expect-present"))
        return expect_present();
    if (argc == 2 && !strcmp(argv[1], "expect-bo"))
        return expect_bo();
    if (argc == 2 && !strcmp(argv[1], "expect-kms"))
        return expect_kms();
    if (argc == 2 && !strcmp(argv[1], "expect-prime"))
        return expect_prime();
    if (argc == 2 && !strcmp(argv[1], "expect-resources"))
        return expect_resources();
    if (argc == 2 && !strncmp(argv[1], "positional-", 11))
        return run_positional_probe(argv[1] + 11);
    fprintf(stderr, "usage: %s [expect-absent|expect-present|expect-bo|expect-kms|expect-prime|expect-resources|positional-pread64|positional-preadv|positional-preadv2|positional-preadv2-current|positional-preadv2-hipri|positional-preadv2-current-hipri|positional-preadv-zero-negative|positional-preadv2-zero-negative|positional-lseek|positional-interruptible]\n", argv[0]);
    return 64;
}
