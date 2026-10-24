#include <reliefos/bootloader.h>
#include <stdio.h>
#include <string.h>

/** @brief Describe explicit firmware selection; omitted modes both default to yes. */
static void usage(void)
{
    puts("Usage: reliefos-boot-install --source DIR --boot-directory DIR [--disk DEVICE]");
    puts("       [--bios=yes|no] [--uefi=yes|no] [--check] [--update]");
    puts("       [--root-partuuid UUID] (default: the selected disk's unique GPT Linux root)");
    puts("BIOS requires a whole disk with a GPT BIOS Boot partition. Both modes default to yes.");
    puts("--update retains rollback and skipped existing loaders. Neither mode deletes an existing loader.");
    puts("Selecting no for both still installs shared kernel and GRUB files.");
}

/** @brief Parse literal mode values without treating any user input as shell code. */
static int mode_value(const char *value, uint8_t *out)
{
    if (!strcmp(value, "yes")) *out = 1;
    else if (!strcmp(value, "no")) *out = 0;
    else return -1;
    return 0;
}

/** @brief Run the same checked boot transaction used by the GUI and TTY installer. */
int main(int argc, char **argv)
{
    struct reliefos_boot_install_options options = {
        .source = "/install/esp", .install_bios = 1, .install_uefi = 1,
    };
    int check = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--update")) options.preserve_rollback = 1;
        else if (!strncmp(argv[i], "--bios=", 7)) {
            if (mode_value(argv[i] + 7, &options.install_bios)) { usage(); return 2; }
        } else if (!strncmp(argv[i], "--uefi=", 7)) {
            if (mode_value(argv[i] + 7, &options.install_uefi)) { usage(); return 2; }
        } else if ((!strcmp(argv[i], "--source") || !strcmp(argv[i], "--boot-directory") ||
                    !strcmp(argv[i], "--disk") || !strcmp(argv[i], "--root-partuuid")) && i + 1 < argc) {
            const char *option = argv[i++];
            if (!strcmp(option, "--source")) options.source = argv[i];
            else if (!strcmp(option, "--disk")) options.disk_path = argv[i];
            else if (!strcmp(option, "--root-partuuid")) options.root_partuuid = argv[i];
            else options.boot_dir = argv[i];
        } else { usage(); return 2; }
    }
    if (!options.boot_dir || (options.install_bios && !options.disk_path)) { usage(); return 2; }
    int result = check ? reliefos_boot_validate_payload(&options) : reliefos_boot_install(&options);
    if (check && !result) result = reliefos_boot_validate_target(&options);
    if (result) {
        fprintf(stderr, "reliefos-boot-install: %s (%d); incomplete stages are not a complete rollback\n",
                strerror(-result), result);
        return 1;
    }
    puts(check ? "Boot installation checks passed." : "Boot files and selected loaders installed successfully.");
    return 0;
}
