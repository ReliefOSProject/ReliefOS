#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <string.h>

#include "installer_tty.h"
#include "model.h"

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
        if (!tty_read_line("Username: ", setup->username, sizeof(setup->username)) ||
            !tty_secret("Password: ", setup->password, sizeof(setup->password)) ||
            !tty_secret("Confirm password: ", setup->password_confirm, sizeof(setup->password_confirm)) ||
            !tty_secret("root password: ", setup->root_password, sizeof(setup->root_password)) ||
            !tty_secret("Confirm root password: ", setup->root_password_confirm, sizeof(setup->root_password_confirm))) return 0;
        if (installer_setup_valid(setup)) return 1;
        puts("Check username and matching passwords (1-32 characters, no spaces).");
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

static void tty_print_disks(const struct installer_tty_context *context)
{
    char line[160];
    puts("\nAvailable disks:");
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
        if (!tty_read_line("Select disk number (r to refresh, q to quit): ",
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
            printf("Selected: %s\n", line);
            return 1;
        }
        puts("Invalid disk selection.");
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
    puts("\nPartitions:");
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
        puts("Commands: auto, init, create SIZE FS NAME, resize SIZE, rename NAME,");
        puts("format FS LABEL, esp, root, data, delete, refresh, done.");
        if (!tty_read_line("partition> ", input, sizeof(input))) return 0;
        if (tty_line_is(input, "done")) {
            if (context->partition_plan_valid()) return 1;
            puts("Select an ESP and a root partition, or use auto.");
            continue;
        }
        if (tty_line_is(input, "auto")) {
            if (context->partition_auto_layout() < 0) puts("Automatic layout failed.");
        } else if (tty_line_is(input, "init")) {
            if (context->partition_initialize() < 0) puts("GPT initialization failed.");
        } else if (tty_line_is(input, "refresh")) {
            context->refresh_partitions();
        } else if (!strncmp(input, "create ", 7) &&
                   sscanf(input + 7, "%lu %15s %79[^\n]", &value, fs_name, partition_name) == 3) {
            int filesystem = tty_fs(fs_name);
            if (value > 0xffffffffUL || filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN ||
                context->partition_create((uint32_t)filesystem, (uint32_t)value,
                                          partition_name) < 0)
                puts("Create failed.");
        } else if (!strncmp(input, "resize ", 7) &&
                   sscanf(input + 7, "%lu", &value) == 1) {
            if (value > 0xffffffffUL || context->partition_resize((uint32_t)value) < 0)
                puts("Resize failed.");
        } else if (!strncmp(input, "rename ", 7)) {
            if (context->partition_rename(input + 7) < 0) puts("Rename failed.");
        } else if (!strncmp(input, "format ", 7) &&
                   sscanf(input + 7, "%15s %79s", fs_name, label) == 2) {
            int filesystem = tty_fs(fs_name);
            if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN ||
                context->partition_format((uint32_t)filesystem, label) < 0)
                puts("Format failed.");
        } else if (tty_line_is(input, "delete")) {
            if (context->partition_delete() < 0) puts("Delete failed.");
        } else if (tty_line_is(input, "esp") || tty_line_is(input, "root") ||
                   tty_line_is(input, "data")) {
            uint32_t type = tty_line_is(input, "esp") ? RELIEFOS_BLOCK_GPT_ESP :
                            tty_line_is(input, "root") ? RELIEFOS_BLOCK_GPT_LINUX :
                            RELIEFOS_BLOCK_GPT_BASIC_DATA;
            if (*context->selected_partition < 0 ||
                (uint32_t)*context->selected_partition >= *context->partition_count ||
                context->partition_set_type(type) < 0) {
                puts("Select a partition number first.");
            }
        } else {
            value = strtoul(input, &end, 10);
            if (end != input && *end == 0 && value < *context->partition_count)
                *context->selected_partition = (int32_t)value;
            else puts("Unknown partition command.");
        }
        context->refresh_partitions();
    }
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
        !context->prepare_update ||
        !context->perform_install || !context->perform_update) {
        return 1;
    }

    puts("ReliefOS installer (TTY)");
    puts("This installer uses the same disk formatter and payload as the graphical installer.");
    puts("Fresh install defaults to automatic partitioning; manual GPT editing is available.");
    for (;;) {
        if (!tty_read_line("Mode [install/update]: ", input, sizeof(input))) {
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
        puts("Enter install or update.");
    }

    context->refresh_disks();
    if (!*context->disk_count || !tty_choose_disk(context)) {
        puts("No disk selected. Installation cancelled.");
        return 1;
    }

    if (*context->install_mode == INSTALLER_TTY_MODE_UPDATE) {
        context->prepare_update();
        if (*context->page == INSTALLER_PAGE_FINISH && !*context->install_success) {
            return 1;
        }
        if (!tty_read_line("Type UPDATE to confirm an in-place update: ",
                           input, sizeof(input)) || !tty_line_is(input, "UPDATE")) {
            puts("Update not confirmed. Installation cancelled.");
            return 1;
        }
        context->perform_update();
    } else {
        context->refresh_partitions();
        if (!tty_partition_menu(context)) {
            puts("Partition setup cancelled.");
            return 1;
        }
        if (!tty_setup(context->setup)) {
            puts("Account setup cancelled or input exceeds the transport buffer.");
            return 1;
        }
        if (!tty_read_line("Type INSTALL to confirm the partition layout and installation: ",
                           input, sizeof(input)) || !tty_line_is(input, "INSTALL")) {
            puts("Installation not confirmed. Installation cancelled.");
            return 1;
        }
        context->perform_install();
    }

    if (!*context->install_success) {
        return 1;
    }
    if (tty_read_line("Reboot now? [Y/n]: ", input, sizeof(input)) &&
        input[0] != 'n' && input[0] != 'N') {
        if (reliefos_system_reboot() < 0) perror("Restart failed");
    }

    puts("Installation finished.");
    for (;;) {
        if (!tty_read_line("Command [reboot/shutdown/exit]: ", input, sizeof(input))) {
            return 0;
        }
        if (tty_line_is(input, "reboot") || tty_line_is(input, "r")) {
            if (reliefos_system_reboot() < 0) perror("Restart failed");
        } else if (tty_line_is(input, "shutdown") || tty_line_is(input, "poweroff")) {
            if (reliefos_system_shutdown() < 0) perror("Shutdown failed");
        } else if (tty_line_is(input, "exit") || tty_line_is(input, "q")) {
            return 0;
        } else {
            puts("Unknown command. Use reboot, shutdown, or exit.");
        }
    }
}
