#include "../../userland/apps/installer/installer_capacity.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Create real sparse payload files with controlled logical sizes. */
static void file(const char *path, off_t bytes)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, bytes) == 0 && close(fd) == 0);
}

/** @brief Exercise real POSIX traversal, firmware filtering and capacity refusal. */
int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(!mkdir("root", 0700) && !mkdir("boot", 0700));
    assert(!mkdir("boot/EFI", 0700) && !mkdir("boot/grub", 0700));
    assert(!mkdir("boot/grub/i386-pc", 0700));
    file("root/file", 4097);
    assert(!symlink("/", "root/link")); /* Never walk the host's root. */
    file("boot/kernel", 1);
    file("boot/EFI/efi", 32 * 1024 * 1024);
    file("boot/grub/i386-pc/bios", 16 * 1024 * 1024);
    struct installer_capacity none, both, bios, uefi;
    assert(!installer_capacity_measure("root", "boot", 0, 0, &none));
    assert(none.root_inodes == 3 && none.root_bytes == 16384);
    assert(!installer_capacity_measure("root", "boot", 1, 1, &both));
    assert(!installer_capacity_measure("root", "boot", 1, 0, &bios));
    assert(!installer_capacity_measure("root", "boot", 0, 1, &uefi));
    assert(both.boot_bytes > bios.boot_bytes && both.boot_bytes > uefi.boot_bytes);
    assert(bios.boot_bytes > none.boot_bytes && uefi.boot_bytes > none.boot_bytes);
    assert(!installer_capacity_check(&both, 2ULL * 1024 * 2048, 128 * 2048));
    struct installer_capacity large = both;
    large.root_bytes = 300ULL * 1024 * 1024;
    assert(installer_capacity_check(&large, 128 * 2048, 128 * 2048) == -ENOSPC);
    large = both; large.root_inodes = 100000;
    assert(installer_capacity_check(&large, 128 * 2048, 128 * 2048) == -ENOSPC);
    large = both; large.boot_bytes = 129ULL * 1024 * 1024;
    assert(installer_capacity_check(&large, 2ULL * 1024 * 2048, 128 * 2048) == -ENOSPC);
    assert(installer_capacity_check(&both, UINT64_MAX, 128 * 2048) == -EOVERFLOW);
    large = both; large.root_bytes = UINT64_MAX;
    assert(installer_capacity_check(&large, 128 * 2048, 128 * 2048) < 0);
    file("root/huge", 512ULL * 1024 * 1024);
    assert(!installer_capacity_measure("root", "boot", 1, 1, &large));
    assert(installer_capacity_check(&large, 256 * 2048, 128 * 2048) == -ENOSPC);
    assert(!symlink("boot", "boot-link"));
    assert(installer_capacity_measure("root", "boot-link", 1, 1, &large) == -ELOOP);
    puts("PASS payload allocation/inode/firmware measurement and checked capacity bounds");
    return 0;
}
