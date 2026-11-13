#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <string.h>
#include <libintl.h>

#include "installer_tty.h"
#include "installer_update.h"
#include "installer_capacity.h"
#include "model.h"

#define T(s) gettext(s)

static int tty_read_line(const char *prompt, char *buffer, uint32_t capacity)
{
    uint32_t length = 0;
    int overflow = 0;
    char input;
    if (!buffer || capacity < 2U) {
        return 0;
    }
    buffer[0] = 0;
    if (prompt) {
        write(1, prompt, strlen(prompt));
    }
    for (;;) {
        ssize_t result = read(0, &input, 1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        if (input == '\r') {
            continue;
        }
        if (input == '\n') {
            buffer[length] = 0;
            if (overflow) { errno = EOVERFLOW; return 0; }
            return 1;
        }
        if ((uint8_t)input >= 32U && length + 1U < capacity) {
            buffer[length++] = input;
            buffer[length] = 0;
        } else if ((uint8_t)input >= 32U) overflow = 1;
    }
    return 0;
}

static int tty_secret(const char *prompt, char *buffer, uint32_t capacity)
{
    struct termios saved, secret;
    if (tcgetattr(0, &saved) < 0) return 0;
    secret = saved;
    secret.c_lflag &= ~(ECHO | ECHONL);
    if (tcsetattr(0, TCSANOW, &secret) < 0) return 0;
    int result = tty_read_line(prompt, buffer, capacity);
    if (tcsetattr(0, TCSANOW, &saved) < 0) result = 0;
    puts("");
    return result;
}

static int tty_setup(struct installer_setup *setup)
{
    for (;;) {
        if (!tty_read_line(T("Username: "), setup->username, sizeof(setup->username)) ||
            !tty_secret(T("Password: "), setup->password, sizeof(setup->password)) ||
            !tty_secret(T("Confirm password: "), setup->password_confirm, sizeof(setup->password_confirm)) ||
            !tty_secret(T("root password: "), setup->root_password, sizeof(setup->root_password)) ||
            !tty_secret(T("Confirm root password: "), setup->root_password_confirm, sizeof(setup->root_password_confirm))) return 0;
        if (installer_setup_valid(setup)) return 1;
        puts(T("Check username and matching passwords (1-32 characters, no spaces)."));
    }
}

static int tty_line_is(const char *line, const char *expected)
{
    uint32_t i = 0;
    if (!line || !expected) {
        return 0;
    }
    while (line[i] && expected[i]) {
        char a = line[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return 0;
        }
        ++i;
    }
    return line[i] == 0 && expected[i] == 0;
}

static int tty_install_xorg(struct installer_setup *setup)
{
    char input[16];
    if (!setup) return 0;
    for (;;) {
        errno = 0;
        if (!tty_read_line(T("Install Xorg desktop environment? [Y/n]: "),
                           input, sizeof(input))) {
            if (errno == EOVERFLOW) {
                puts(T("Please answer yes or no."));
                continue;
            }
            return 0;
        }
        if (!input[0] || tty_line_is(input, "y") || tty_line_is(input, "yes")) {
            setup->skip_xorg = 0;
            return 1;
        }
        if (tty_line_is(input, "n") || tty_line_is(input, "no")) {
            setup->skip_xorg = 1;
            return 1;
        }
        puts(T("Please answer yes or no."));
    }
}

/** @brief Read a default-off swap choice and an explicitly validated MiB size.
 * @param setup Mutable shared wizard settings. @return One on success or zero
 * for EOF, input error, or a null setup; invalid entries are prompted again.
 */
int installer_tty_select_swap(struct installer_setup *setup)
{
    if (!setup) return 0;
    char input[32],prompt[128];
    for (;;) {
        errno=0;
        if (!tty_read_line(T("Create a swapfile? [y/N]: "),input,sizeof(input))) {
            if (errno==EOVERFLOW) continue;
            return 0;
        }
        if (!input[0] || tty_line_is(input,"n") || tty_line_is(input,"no")) {
            setup->swap_enabled=false;return 1;
        }
        if (tty_line_is(input,"y") || tty_line_is(input,"yes")) break;
        puts(T("Please answer yes or no."));
    }
    uint64_t suggested=setup->swap_size_mib ? setup->swap_size_mib : installer_swap_recommend_system();
    snprintf(prompt,sizeof(prompt),T("Swap size in MiB [%llu]: "),(unsigned long long)suggested);
    for (;;) {
        errno=0;
        if (!tty_read_line(prompt,input,sizeof(input))) {
            if (errno==EOVERFLOW) {puts(T("Enter a valid swap size in MiB."));continue;}
            return 0;
        }
        uint64_t value=suggested;
        if (!input[0] || !installer_swap_parse_mib(input,&value)) {
            setup->swap_size_mib=value;setup->swap_enabled=true;return 1;
        }
        puts(T("Enter a valid swap size in MiB."));
    }
}

/** @brief Preserve the detected/default mode on Enter; accept explicit yes/no. */
static int tty_select_loader(const char *name, uint8_t *skip)
{
    char input[16], prompt[160];
    snprintf(prompt, sizeof(prompt), "%s %s: ", name, *skip ? "[y/N]" : "[Y/n]");
    for (;;) {
        errno = 0;
        if (!tty_read_line(prompt, input, sizeof(input))) {
            if (errno == EOVERFLOW) continue;
            return 0;
        }
        if (!input[0]) return 1;
        if (tty_line_is(input, "yes") || tty_line_is(input, "y")) { *skip = 0; return 1; }
        if (tty_line_is(input, "no") || tty_line_is(input, "n")) { *skip = 1; return 1; }
        puts(T("Please answer yes or no."));
    }
}

/** @brief Present independent firmware choices before partition planning. */
static int tty_bootloaders(struct installer_setup *setup)
{
    if (!tty_select_loader(T("Install or update BIOS bootloader?"), &setup->skip_bios) ||
        !tty_select_loader(T("Install or update UEFI bootloader?"), &setup->skip_uefi)) return 0;
    if (setup->skip_bios && setup->skip_uefi)
        puts(T("No bootloader selected. A new installation will need a separately configured bootloader."));
    return 1;
}

static void tty_print_disks(const struct installer_tty_context *context)
{
    char line[160];
    puts(T("\nAvailable disks:"));
    for (uint32_t i = 0; i < *context->disk_count; ++i) {
        context->format_disk_line(line, sizeof(line), &context->disks[i]);
        printf("  [%u] %s\n", i, line);
    }
}

static int tty_choose_disk(const struct installer_tty_context *context)
{
    char input[24];
    char line[160];
    char *end;
    unsigned long value;
    for (;;) {
        tty_print_disks(context);
        if (!tty_read_line(T("Select disk number (r to refresh, q to quit): "),
                           input, sizeof(input))) {
            return 0;
        }
        if (tty_line_is(input, "q")) {
            return 0;
        }
        if (tty_line_is(input, "r")) {
            context->refresh_disks();
            continue;
        }
        value = strtoul(input, &end, 10);
        if (end != input && *end == 0 && value < *context->disk_count) {
            *context->selected_disk = (int32_t)value;
            context->format_disk_line(line, sizeof(line),
                                      &context->disks[*context->selected_disk]);
            printf(T("Selected: %s\n"), line);
            return 1;
        }
        puts(T("Invalid disk selection."));
    }
}

static int tty_fs(const char *name)
{
    if (tty_line_is(name, "fat32") || tty_line_is(name, "fat"))
        return RELIEFOS_BLOCK_FILESYSTEM_FAT32;
    if (tty_line_is(name, "ext2")) return RELIEFOS_BLOCK_FILESYSTEM_EXT2;
    if (tty_line_is(name, "ext4")) return RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    if (tty_line_is(name, "exfat")) return RELIEFOS_BLOCK_FILESYSTEM_EXFAT;
    return RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
}

static void tty_print_partitions(const struct installer_tty_context *context)
{
    char line[256];
    puts(T("\nPartitions:"));
    for (uint32_t i = 0; i < *context->partition_count; ++i) {
        context->format_partition_line(line, sizeof(line), &context->partitions[i]);
        printf("  [%u] %s\n", i, line);
    }
}

static int tty_partition_menu(const struct installer_tty_context *context)
{
    char input[160], fs_name[16], partition_name[80], label[80];
    unsigned long value;
    char *end;
    for (;;) {
        tty_print_partitions(context);
        puts(T("Commands: auto, init, create SIZE FS NAME, resize SIZE, rename NAME,"));
        puts(T("format FS LABEL, format-esp yes|no, bios, esp, root, data, delete, refresh, done. FS raw creates an unformatted partition."));
        printf("ESP: %s\n", context->setup->format_esp ? T("format (erase EFI files)") : T("preserve existing FAT32 files"));
        if (!tty_read_line(T("partition> "), input, sizeof(input))) return 0;
        if (tty_line_is(input, "format-esp yes")) {
            context->setup->format_esp = 1;
            continue;
        }
        if (tty_line_is(input, "format-esp no")) {
            context->setup->format_esp = 0;
            continue;
        }
        if (tty_line_is(input, "done")) {
            if (context->partition_plan_valid()) return 1;
            puts(context->setup->skip_bios
                ? T("Select ESP and root partitions, or use auto.")
                : T("Select an unformatted BIOS Boot partition, ESP and root partitions, or use auto."));
            continue;
        }
        if (tty_line_is(input, "auto")) {
            if (context->partition_auto_layout() < 0) puts(T("Automatic layout failed."));
        } else if (tty_line_is(input, "init")) {
            if (context->partition_initialize() < 0) puts(T("GPT initialization failed."));
        } else if (tty_line_is(input, "refresh")) {
            context->refresh_partitions();
        } else if (!strncmp(input, "create ", 7) &&
                   sscanf(input + 7, "%lu %15s %79[^\n]", &value, fs_name, partition_name) == 3) {
            int filesystem = tty_fs(fs_name);
            if (value > 0xffffffffUL || (filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN && strcmp(fs_name, "raw")) ||
                context->partition_create((uint32_t)filesystem, (uint32_t)value,
                                          partition_name) < 0)
                puts(T("Create failed."));
        } else if (!strncmp(input, "resize ", 7) &&
                   sscanf(input + 7, "%lu", &value) == 1) {
            if (value > 0xffffffffUL || context->partition_resize((uint32_t)value) < 0)
                puts(T("Resize failed."));
        } else if (!strncmp(input, "rename ", 7)) {
            if (context->partition_rename(input + 7) < 0) puts(T("Rename failed."));
        } else if (!strncmp(input, "format ", 7) &&
                   sscanf(input + 7, "%15s %79s", fs_name, label) == 2) {
            int filesystem = tty_fs(fs_name);
            if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN ||
                context->partition_format((uint32_t)filesystem, label) < 0)
                puts(T("Format failed."));
        } else if (tty_line_is(input, "delete")) {
            if (context->partition_delete() < 0) puts(T("Delete failed."));
        } else if (tty_line_is(input, "esp") || tty_line_is(input, "root") ||
                   tty_line_is(input, "data") || tty_line_is(input, "bios")) {
            uint32_t type = tty_line_is(input, "esp") ? RELIEFOS_BLOCK_GPT_ESP :
                            tty_line_is(input, "root") ? RELIEFOS_BLOCK_GPT_LINUX :
                            tty_line_is(input, "bios") ? RELIEFOS_BLOCK_GPT_BIOS_BOOT :
                            RELIEFOS_BLOCK_GPT_BASIC_DATA;
            if (*context->selected_partition < 0 ||
                (uint32_t)*context->selected_partition >= *context->partition_count ||
                context->partition_set_type(type) < 0) {
                puts(T("Select a partition number first."));
            }
        } else {
            value = strtoul(input, &end, 10);
            if (end != input && *end == 0 && value < *context->partition_count)
                *context->selected_partition = (int32_t)value;
            else puts(T("Unknown partition command."));
        }
        context->refresh_partitions();
    }
}

static void tty_update_progress(void *context, unsigned progress, const char *message)
{
    (void)context;
    printf("[%3u%%] %s\n", progress, T(message));
}

int installer_tty_main(const struct installer_tty_context *context)
{
    char input[32];
    if (!context || !context->setup || !context->disks || !context->disk_count ||
        !context->partitions || !context->partition_count || !context->selected_partition ||
        !context->root_partition || !context->esp_partition || !context->partition_auto ||
        !context->selected_disk || !context->install_mode ||
        !context->install_success || !context->page ||
        !context->refresh_disks || !context->format_disk_line || !context->refresh_partitions ||
        !context->format_partition_line || !context->partition_plan_valid ||
        !context->partition_auto_layout || !context->partition_initialize ||
        !context->partition_create || !context->partition_resize ||
        !context->partition_rename || !context->partition_set_type ||
        !context->partition_delete || !context->partition_format ||
        !context->perform_install) {
        return 1;
    }

    puts(T("ReliefOS installer (TTY)"));
    puts(T("This installer uses the same disk formatter and payload as the graphical installer."));
    puts(T("Fresh install defaults to automatic partitioning; manual GPT editing is available."));
    for (;;) {
        if (!tty_read_line(T("Mode [install/update]: "), input, sizeof(input))) {
            return 1;
        }
        if (tty_line_is(input, "install") || tty_line_is(input, "i")) {
            *context->install_mode = INSTALLER_TTY_MODE_INSTALL;
            break;
        }
        if (tty_line_is(input, "update") || tty_line_is(input, "u")) {
            *context->install_mode = INSTALLER_TTY_MODE_UPDATE;
            break;
        }
        puts(T("Enter install or update."));
    }

    context->refresh_disks();
    if (!*context->disk_count || !tty_choose_disk(context)) {
        puts(T("No disk selected. Installation cancelled."));
        return 1;
    }

    if (*context->install_mode == INSTALLER_TTY_MODE_UPDATE) {
        struct installer_update_info inspected, result;
        const char *disk = context->disks[*context->selected_disk].path;
        char root_uuid[37] = {0}, esp_uuid[37] = {0};
        context->refresh_partitions();
        tty_print_partitions(context);
        char *end;
        for (int role = 0; role < 2; ++role) {
            if (!tty_read_line(role ? T("ESP partition number (blank: use fstab): ")
                                    : T("ReliefOS root partition number (blank: auto): "), input, sizeof(input))) return 1;
            if (!input[0]) continue;
            errno = 0;
            unsigned long index = strtoul(input, &end, 10);
            if (errno || *end || index >= *context->partition_count) return 1;
            const struct reliefos_block_partition *part = &context->partitions[index];
            if (part->gpt_type != (role ? RELIEFOS_BLOCK_GPT_ESP : RELIEFOS_BLOCK_GPT_LINUX)) return 1;
            if (reliefos_block_partition_uuid(disk, part->index, role ? esp_uuid : root_uuid)) return 1;
        }
        int error = installer_update_check_target(disk, root_uuid, esp_uuid, &inspected, tty_update_progress, NULL);
        if (error) {
            fprintf(stderr, "%s: %s\n", T(inspected.failure), strerror(-error));
            return 1;
        }
        printf("\n%s: %s -> %s\n%s: %s\n%s: %s\n%s: %s\n",
               T("System version"), inspected.installed_version, inspected.media_version,
               T("Target disk"), inspected.disk, T("System partition"), inspected.root_device,
               T("Boot partition"), inspected.esp_device);
        printf("%s: %llu MiB / %llu MiB\n", T("Free / required space"),
               (unsigned long long)(inspected.free_bytes / (1024u * 1024u)),
               (unsigned long long)((inspected.required_bytes + 1024u * 1024u - 1) / (1024u * 1024u)));
        puts(T("Accounts, personal files, installed software and local configuration are preserved."));
        context->setup->skip_bios = !(inspected.boot_modes & 1u);
        context->setup->skip_uefi = !(inspected.boot_modes & 2u);
        if (!tty_bootloaders(context->setup)) return 1;
        if (!tty_read_line(T("Proceed with system update? [y/N]: "), input, sizeof(input)) ||
            (!tty_line_is(input, "y") && !tty_line_is(input, "Y") && !tty_line_is(input, "yes"))) {
            puts(T("Update not confirmed. Installation cancelled."));
            return 1;
        }
        error = installer_update_apply(&inspected,
            (context->setup->skip_bios ? 0u : 1u) | (context->setup->skip_uefi ? 0u : 2u),
            &result, tty_update_progress, NULL);
        if (!error && !context->setup->skip_uefi) printf("UEFI: %s\n", result.boot_file);
        *context->install_success = error == 0;
        *context->page = INSTALLER_PAGE_FINISH;
        if (error) fprintf(stderr, "%s: %s\n", T(result.failure), strerror(-error));
    } else {
        if (!tty_bootloaders(context->setup)) return 1;
        context->refresh_partitions();
        if (!tty_partition_menu(context)) {
            puts(T("Partition setup cancelled."));
            return 1;
        }
        if (!tty_setup(context->setup)) {
            puts(T("Account setup cancelled or input exceeds the transport buffer."));
            return 1;
        }
        if (!tty_install_xorg(context->setup)) {
            puts(T("Desktop selection cancelled."));
            return 1;
        }
        if (!installer_tty_select_swap(context->setup)) return 1;
        puts(*context->partition_auto ? T("Automatic installation erases the selected disk.") :
             context->setup->format_esp ? T("The selected root and ESP will be formatted.") :
             T("Only the selected root is formatted; existing ESP files are preserved."));
        if (!tty_read_line(T("Type INSTALL to confirm the partition layout and installation: "),
                           input, sizeof(input)) || !tty_line_is(input, "INSTALL")) {
            puts(T("Installation not confirmed. Installation cancelled."));
            return 1;
        }
        context->perform_install();
        if (*context->install_success && !context->setup->skip_uefi && context->boot_entry_path) {
            char entry[128];
            if (!context->boot_entry_path(entry, sizeof(entry))) printf("UEFI: %s\n", entry);
        }
    }

    if (!*context->install_success) {
        return 1;
    }
    if (tty_read_line(T("Reboot now? [Y/n]: "), input, sizeof(input)) &&
        input[0] != 'n' && input[0] != 'N') {
        if (reliefos_system_reboot() < 0) perror(T("Restart failed"));
    }

    puts(T("Installation finished."));
    for (;;) {
        if (!tty_read_line(T("Command [reboot/shutdown/exit]: "), input, sizeof(input))) {
            return 0;
        }
        if (tty_line_is(input, "reboot") || tty_line_is(input, "r")) {
            if (reliefos_system_reboot() < 0) perror(T("Restart failed"));
        } else if (tty_line_is(input, "shutdown") || tty_line_is(input, "poweroff")) {
            if (reliefos_system_shutdown() < 0) perror(T("Shutdown failed"));
        } else if (tty_line_is(input, "exit") || tty_line_is(input, "q")) {
            return 0;
        } else {
            puts(T("Unknown command. Use reboot, shutdown, or exit."));
        }
    }
}
