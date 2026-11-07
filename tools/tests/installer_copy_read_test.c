#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../userland/apps/installer/installer_copy.h"

ssize_t __real_read(int fd, void *buffer, size_t size);
static int injected_eintr, inject_error;
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (!injected_eintr++) { errno = EINTR; return -1; }
    if (inject_error && injected_eintr > 2) { errno = EIO; return -1; }
    if (size > 4096) size = 4096;
    return __real_read(fd, buffer, size);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "w+b");
    assert(file);
    assert(!unlink(argv[1]));
    unsigned char data[50003], got[32768];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 17);
    assert(fwrite(data, 1, sizeof(data), file) == sizeof(data) && !fflush(file));
    int fd = fileno(file);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(installer_read_chunk(fd, got, sizeof(got)) == sizeof(got));
    assert(!memcmp(got, data, sizeof(got)));
    assert(installer_read_chunk(fd, got, sizeof(got)) == sizeof(data) - sizeof(got));
    assert(!memcmp(got, data + sizeof(got), sizeof(data) - sizeof(got)));
    assert(installer_read_chunk(fd, got, sizeof(got)) == 0);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    inject_error = 1;
    injected_eintr = 0;
    assert(installer_read_chunk(fd, got, sizeof(got)) == -1 && errno == EIO);
    assert(!fclose(file));
    puts("PASS installer read aggregation: short reads, EINTR, partial EOF and I/O failure");
}
