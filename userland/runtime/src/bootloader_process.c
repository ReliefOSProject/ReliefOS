#include "bootloader_internal.h"
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>

/**
 * @brief Execute GNU GRUB's standard PC installer with a private environment.
 * The parent retains all UI state and environment ownership. No shell parses
 * paths, and the callback runs only on this calling thread while waiting.
 */
int boot_install_bios(const struct reliefos_boot_install_options *options)
{
    char directory[BOOT_PATH_MAX], boot_directory[BOOT_PATH_MAX];
    int n = snprintf(directory, sizeof(directory), "--directory=%s/grub/i386-pc", options->source);
    if (n < 0 || (size_t)n >= sizeof(directory)) return -ENAMETOOLONG;
    n = snprintf(boot_directory, sizeof(boot_directory), "--boot-directory=%s", options->boot_dir);
    if (n < 0 || (size_t)n >= sizeof(boot_directory)) return -ENAMETOOLONG;
    char *const argv[] = {BOOT_GRUB_INSTALL, "--target=i386-pc", directory,
        boot_directory, "--recheck", "--force-file-id", "--fonts=", "--themes=", "--locales=",
        "--modules=part_gpt fat normal multiboot2 cmp search search_fs_file configfile serial terminal all_video font gfxterm gfxmenu",
        "--", (char *)options->disk_path, NULL};
    char *const environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C",
                                "HOME=/root", NULL};
    pid_t child;
    int result = posix_spawn(&child, BOOT_GRUB_INSTALL, NULL, NULL, argv, environment);
    if (result) return -result;
    int status;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) return -errno;
        if (options->progress) options->progress(options->progress_context);
        const struct timespec delay = {0, 20000000};
        (void)nanosleep(&delay, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "[boot-install] GRUB BIOS stage failed (wait status=%d); disk writes may be partial\n", status);
        return -EIO;
    }
    return 0;
}
