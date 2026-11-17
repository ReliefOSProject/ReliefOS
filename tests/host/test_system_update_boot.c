/* SPDX-License-Identifier: Apache-2.0 */
/* Include the real private validator to exercise its real-root-only guard. */
#include "../../userland/apps/systemupdate/manifest.c"
#include <assert.h>
#include <sys/stat.h>

static int input_fd, interrupt_read;
static const char release[] = "5.1.2-ko1-aaaaaaaaaaaa";
static const char digest[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

int __wrap_uname(struct utsname *system)
{
    memset(system, 0, sizeof(*system));
    strcpy(system->sysname, "ReliefNT"); strcpy(system->release, release);
    return 0;
}

int __wrap_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/proc/cmdline"));
    assert(flags == (O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    return dup(input_fd);
}

ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (interrupt_read) { interrupt_read = 0; errno = EINTR; return -1; }
    return __real_read(fd, buffer, size > 3 ? 3 : size);
}

static int validate(const void *bytes, size_t size)
{
    int channel[2]; assert(!pipe(channel)); input_fd = channel[0];
    struct stat status; assert(!fstat(input_fd, &status) && !status.st_size);
    assert(write(channel[1], bytes, size) == (ssize_t)size); assert(!close(channel[1]));
    struct update_context context = {0}; strcpy(context.root, "/");
    struct boot_identity identity = {"A", "3", "", ""};
    strcpy(identity.release, release); strcpy(identity.initramfs, digest);
    interrupt_read = 1;
    int result = running_confirmed(&context, &identity);
    assert(!close(input_fd)); return result;
}

int main(void)
{
    char command[512];
    int size = snprintf(command, sizeof(command),
        "quiet reliefos.boot_slot=A reliefos.boot_generation=3 reliefos.boot_initramfs=%s\n", digest);
    assert(size > 0 && (size_t)size < sizeof(command));
    assert(!validate(command, (size_t)size));
    strcat(command, " reliefos.boot_slot=A");
    assert(validate(command, strlen(command)) == -EBUSY);
    assert(validate("quiet", 5) == -EBUSY);
    command[0] = 0;
    assert(validate(command, (size_t)size) == -E2BIG);
    char oversized[4096]; memset(oversized, 'x', sizeof(oversized));
    assert(validate(oversized, sizeof(oversized)) == -E2BIG);
    puts("Real-root fallback guard reads zero-size streams, retries EINTR/short reads and rejects unsafe identities");
    return 0;
}
