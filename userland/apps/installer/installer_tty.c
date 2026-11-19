/* ncursesw frontend over the shared FLTK wizard model and disk operations. */
#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/system.h>
#include "installer_tui.h"
#include "installer_partitions.h"
#include "installer_capacity.h"
#include "install_ops.h"
#include "../locale_settings.h"

#define T(s) gettext(s)
extern const char installer_license_text[];
enum page_action {
    REFRESH = 10, AUTO_NOW, INIT_GPT, CREATE, RESIZE, RENAME, FORMAT, DELETE_PART,
    SET_ROLE, PICK_ROOT, PICK_ESP, REBOOT, SHUTDOWN
};

static const char *page_title(unsigned current)
{
    static const char *const titles[] = {
        "Language", "License", "Welcome", "Installation mode", "Target disk",
        "Bootloaders", "Configure partitions", "Account and system options",
        "Review installation", "Installation progress", "Installation finished"
    };
    return current < INSTALLER_PAGE_COUNT ? T(titles[current]) : "ReliefOS";
}

static void error_message(int error, const char *detail)
{
    char message[1024];
    snprintf(message, sizeof(message), "%s\n%s (%d)", detail && *detail ? detail : T("Operation failed"),
        strerror(error < 0 ? -error : error), error);
    tui_message(T("Operation failed"), message);
}

static int ask_size(const char *title, uint32_t *size)
{
    char text[32];
    snprintf(text, sizeof(text), "%u", *size ? *size : 1024);
    while (tui_edit(title, T("Size in MiB"), text, sizeof(text))) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(text, &end, 10);
        if (*text >= '0' && *text <= '9' && !errno && !*end && value && value <= UINT32_MAX) {
            *size = (uint32_t)value;
            return 1;
        }
        tui_message(T("Invalid size"), T("Enter a positive whole number of MiB."));
    }
    return 0;
}

static void partition_action(const struct installer_tty_context *context, int action)
{
    struct tui_job job = {0};
    if (action == REFRESH) { context->refresh_partitions(); return; }
    int needs_partition = action != AUTO_NOW && action != INIT_GPT && action != CREATE;
    if (needs_partition && (*context->selected_partition < 0 ||
        (uint32_t)*context->selected_partition >= *context->partition_count)) {
        tui_message(T("Configure partitions"), T("Select a partition first."));
        return;
    }
    char identity[512];
    if (needs_partition) context->format_partition_line(identity, sizeof(identity), &context->partitions[*context->selected_partition]);
    else context->format_disk_line(identity, sizeof(identity), &context->disks[*context->selected_disk]);
    const char *confirmation = NULL;
    if (action == AUTO_NOW) { job.kind = TUI_JOB_AUTO; confirmation = "AUTO"; }
    else if (action == INIT_GPT) { job.kind = TUI_JOB_INIT; confirmation = "INITIALIZE"; }
    else if (action == DELETE_PART) { job.kind = TUI_JOB_DELETE; confirmation = "DELETE"; }
    else if (action == CREATE || action == FORMAT) {
        static const char *const filesystems[] = {"ext4", "ext3", "ext2", "FAT32", "exFAT", "raw"};
        static const unsigned values[] = {RELIEFOS_BLOCK_FILESYSTEM_EXT4, RELIEFOS_BLOCK_FILESYSTEM_EXT3, RELIEFOS_BLOCK_FILESYSTEM_EXT2,
            RELIEFOS_BLOCK_FILESYSTEM_FAT32, RELIEFOS_BLOCK_FILESYSTEM_EXFAT, RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN};
        if (action == CREATE && !ask_size(T("Create partition"), &job.size)) return;
        int chosen = tui_pick(T("Filesystem"), filesystems, action == CREATE ? 6 : 5, 0);
        if (chosen < 0) return;
        job.filesystem = values[chosen];
        snprintf(job.name, sizeof(job.name), "%s", action == CREATE ? "ReliefOS Data" : "RELIEFOS_DATA");
        if (!tui_edit(action == CREATE ? T("Create partition") : T("Format partition"),
            action == CREATE ? T("Partition name") : T("Filesystem label"), job.name, sizeof(job.name))) return;
        job.kind = action == CREATE ? TUI_JOB_CREATE : TUI_JOB_FORMAT;
        if (action == FORMAT) confirmation = "FORMAT";
    } else if (action == RESIZE) {
        job.kind = TUI_JOB_RESIZE;
        if (!ask_size(T("Resize partition"), &job.size)) return;
        confirmation = "RESIZE";
    } else if (action == RENAME) {
        job.kind = TUI_JOB_RENAME;
        snprintf(job.name, sizeof(job.name), "%s", context->partitions[*context->selected_partition].name);
        if (!tui_edit(T("Rename partition"), T("Partition name"), job.name, sizeof(job.name))) return;
    } else if (action == SET_ROLE) {
        const char *roles[] = {T("System root"), "ESP", "BIOS Boot", T("Data partition")};
        const unsigned values[] = {RELIEFOS_BLOCK_GPT_LINUX, RELIEFOS_BLOCK_GPT_ESP,
            RELIEFOS_BLOCK_GPT_BIOS_BOOT, RELIEFOS_BLOCK_GPT_BASIC_DATA};
        int chosen = tui_pick(T("Partition role"), roles, 4, 0);
        if (chosen < 0) return;
        job.kind = TUI_JOB_ROLE;
        job.role = values[chosen];
    } else return;
    if (confirmation) {
        char message[1024];
        snprintf(message, sizeof(message), "%s\n\n%s", identity,
            action == RESIZE ? T("Resize only changes the trailing edge. Check the filesystem size before applying.")
                             : T("This operation changes the selected disk or partition immediately and can destroy its data."));
        if (!tui_confirm(T("Confirm partition operation"), message, confirmation)) return;
    }
    int result = tui_run_job(context, &job);
    context->refresh_partitions();
    if (result) error_message(result, T("Partition operation failed"));
}

static void select_update_partition(const struct installer_tty_context *context, int esp, char uuid[37])
{
    const char *options[RELIEFOS_BLOCK_MAX_PARTITIONS + 1];
    char labels[RELIEFOS_BLOCK_MAX_PARTITIONS][256];
    uint32_t mapping[RELIEFOS_BLOCK_MAX_PARTITIONS];
    int count = 1;
    options[0] = esp ? T("Use the selected installation's fstab") : T("Automatically identify a single ReliefOS installation");
    for (uint32_t i = 0; i < *context->partition_count; ++i) {
        if (context->partitions[i].gpt_type != (esp ? RELIEFOS_BLOCK_GPT_ESP : RELIEFOS_BLOCK_GPT_LINUX)) continue;
        context->format_partition_line(labels[count - 1], sizeof(labels[0]), &context->partitions[i]);
        mapping[count - 1] = context->partitions[i].index;
        options[count] = labels[count - 1];
        ++count;
    }
    int chosen = tui_pick(esp ? T("Boot partition") : T("System partition"), options, count, 0);
    if (chosen < 0) return;
    uuid[0] = 0;
    if (chosen) {
        int result = reliefos_block_partition_uuid(context->disks[*context->selected_disk].path, mapping[chosen - 1], uuid);
        if (result) error_message(result, T("Cannot read partition identity"));
    }
}

/** @brief Assemble the visual partition workspace over the shared disk operations.
 * @param view Caller-owned ncurses page. @param context Borrowed installer state.
 * @param table Persistent read-only snapshot and keyboard viewport.
 * @param scheme Automatic/manual choice. @param chosen Actual partition-array selection.
 * @param format_esp Explicit destructive ESP-format preference.
 */
static void partition_page(struct tui_view *view, const struct installer_tty_context *context,
                           struct tui_partition_table *table, int *scheme, int *chosen, int *format_esp)
{
    tui_compact_radio(view, T("Automatic layout"), scheme, 0);
    tui_compact_radio(view, T("Manual partitioning"), scheme, 1);
    int result = tui_partition_table_update(table, context, !*scheme, chosen);
    tui_partitions(view, table);
    if (result) tui_text(view, !*scheme
        ? T("The automatic layout does not fit this disk. Select another disk or configure partitions manually.")
        : T("Cannot display this partition layout."));
    const char *labels[] = {T("Refresh"), T("Apply automatic layout now"), T("Initialize GPT"),
        T("Create"), T("Resize"), T("Rename"), T("Format"), T("Delete"), T("Partition role")};
    for (int i = 0; i < (*scheme ? 9 : 3); ++i) tui_compact_button(view, labels[i], REFRESH + i);
    if (*scheme) tui_check(view, T("Format selected ESP (erase all EFI boot files on it)"), format_esp);
}

static void review_page(struct tui_view *view, const struct installer_tty_context *context,
                        const struct installer_update_info *update, int *bios, int *uefi,
                        const char *swap_size)
{
    char text[768];
    if (*context->install_mode == INSTALLER_MODE_UPDATE) {
        snprintf(text, sizeof(text), "%s\n%s -> %s\n\n%s\n%s\n%s\n\n%s: %llu MiB / %llu MiB",
            T("System version"), update->installed_version, update->media_version,
            update->disk, update->root_device, update->esp_device, T("Free / required space"),
            (unsigned long long)(update->free_bytes / (1024 * 1024)),
            (unsigned long long)((update->required_bytes + 1024 * 1024 - 1) / (1024 * 1024)));
        tui_text(view, text);
        tui_text(view, T("Accounts, personal files, installed software and local configuration are preserved."));
        if (*update->pending) tui_text(view, T("A previous update is incomplete. This attempt will verify the system and recover the pending update stages."));
        tui_check(view, T("Install or update BIOS bootloader"), bios);
        tui_check(view, T("Install or update UEFI bootloader"), uefi);
    } else {
        context->format_disk_line(text, sizeof(text), &context->disks[*context->selected_disk]);
        tui_text(view, text);
        if (*context->partition_auto) {
            struct installer_partition_plan plan;
            if (!installer_partition_plan_default(&context->disks[*context->selected_disk],
                (*bios ? 1u : 0) | (*uefi ? 2u : 0), &plan)) {
                snprintf(text, sizeof(text), T("Erase disk: BIOS Boot %llu MiB, ESP %llu MiB, ext4 root %llu MiB"),
                    (unsigned long long)(plan.bios.sector_count / 2048),
                    (unsigned long long)(plan.esp.sector_count / 2048),
                    (unsigned long long)(plan.root.sector_count / 2048));
                tui_text(view, text);
            }
        } else {
            for (uint32_t i = 0; i < *context->partition_count; ++i) {
                const struct reliefos_block_partition *part = &context->partitions[i];
                if ((int32_t)part->index == *context->root_partition || (int32_t)part->index == *context->esp_partition) {
                    context->format_partition_line(text, sizeof(text), part);
                    tui_text(view, text);
                }
            }
            tui_text(view, context->setup->format_esp ? T("The selected root and ESP will be formatted.")
                : T("Only the selected root is formatted; existing ESP files are preserved."));
        }
        snprintf(text, sizeof(text), "%s: %s\nBIOS: %s   UEFI: %s\n%s: %s\n%s: %s",
            T("Username"), context->setup->username, *bios ? T("Yes") : T("No"), *uefi ? T("Yes") : T("No"),
            T("Desktop"), context->setup->desktop == INSTALLER_DESKTOP_CONSOLE ? T("Console only") :
                context->setup->desktop == INSTALLER_DESKTOP_XFCE ? T("XFCE (experimental)") : T("IceWM (default)"),
            T("Swap size in MiB"), context->setup->swap_enabled ? swap_size : T("Disabled"));
        tui_text(view, text);
        tui_input(view, T("Type INSTALL to confirm installation"), confirm_text, sizeof(confirm_text), 0);
    }
    if (!*bios && !*uefi) tui_text(view, T("No bootloader selected. A new installation will need a separately configured bootloader."));
}

int installer_tty_main(const struct installer_tty_context *context)
{
    if (!context || !context->setup || !context->selected_disk || !context->disk_count ||
        !context->partitions || !context->partition_count || !context->perform_install) return 1;
    if (tui_open() < 0) {
        fprintf(stderr, "installer: %s: %s\n", T("Cannot initialize the terminal or installer log"), strerror(errno));
        return 1;
    }
    struct tui_view view = {.previous_focus = -1};
    struct installer_update_info inspected = {0};
    unsigned current = INSTALLER_PAGE_LANGUAGE;
    int language = language_selection(), mode = *context->install_mode;
    int previous_mode = mode;
    int bios = !context->setup->skip_bios, uefi = !context->setup->skip_uefi;
    int desktop = context->setup->desktop, swap = context->setup->swap_enabled;
    int scheme = !*context->partition_auto, chosen = *context->selected_partition;
    struct tui_partition_table partition_table = {0};
    int format_esp = context->setup->format_esp, previous_disk = -1, update_checked = 0;
    int disk_choice = *context->selected_disk;
    char root_uuid[37] = "", esp_uuid[37] = "", swap_text[32];
    snprintf(swap_text, sizeof(swap_text), "%llu", (unsigned long long)(context->setup->swap_size_mib
        ? context->setup->swap_size_mib : installer_swap_recommend_system()));
    int exit_code = 1, finished = 0;
    context->refresh_disks();
    disk_choice = *context->selected_disk;
    while (!finished) {
        if (installer_tui_signal) break;
        *context->page = current;
        tui_begin(&view, page_title(current));
        char text[768];
        switch (current) {
        case INSTALLER_PAGE_LANGUAGE:
            for (unsigned i = 0; i < sizeof(language_options) / sizeof(language_options[0]); ++i)
                tui_radio(&view, language_options[i].name, &language, i);
            break;
        case INSTALLER_PAGE_LICENSE:
            tui_document(&view, installer_license_text);
            break;
        case INSTALLER_PAGE_WELCOME:
            tui_text(&view, T("Install a new system or update an existing ReliefOS disk."));
            tui_text(&view, T("Review the disk model, size and partitions before applying changes."));
            tui_text(&view, T("Use Tab and the arrow keys to select controls. F3 continues and F2 returns to the previous step."));
            break;
        case INSTALLER_PAGE_MODE:
            tui_radio(&view, T("Install a new system"), &mode, INSTALLER_MODE_FRESH);
            tui_radio(&view, T("Update an existing system"), &mode, INSTALLER_MODE_UPDATE);
            break;
        case INSTALLER_PAGE_DISK:
            if (!*context->disk_count) tui_text(&view, T("No disks were found"));
            for (uint32_t i = 0; i < *context->disk_count; ++i) {
                context->format_disk_line(text, sizeof(text), &context->disks[i]);
                tui_radio(&view, text, &disk_choice, i);
            }
            tui_button(&view, T("Refresh"), REFRESH);
            if (mode == INSTALLER_MODE_UPDATE) {
                snprintf(text, sizeof(text), "%s: %s\nESP: %s", T("System partition"),
                    root_uuid[0] ? root_uuid : T("Automatic"), esp_uuid[0] ? esp_uuid : T("Use fstab"));
                tui_text(&view, text);
                tui_button(&view, T("Select system partition"), PICK_ROOT);
                tui_button(&view, T("Select boot partition"), PICK_ESP);
            }
            break;
        case INSTALLER_PAGE_BOOTLOADERS:
            tui_check(&view, T("Install or update BIOS bootloader"), &bios);
            tui_check(&view, T("Install or update UEFI bootloader"), &uefi);
            if (!bios && !uefi) tui_text(&view, T("No bootloader selected. A new installation will need a separately configured bootloader."));
            break;
        case INSTALLER_PAGE_PARTITIONS:
            partition_page(&view, context, &partition_table, &scheme, &chosen, &format_esp);
            break;
        case INSTALLER_PAGE_ACCOUNTS:
            tui_input(&view, T("Username"), context->setup->username, sizeof(context->setup->username), 0);
            tui_input(&view, T("Password"), context->setup->password, sizeof(context->setup->password), 1);
            tui_input(&view, T("Confirm password"), context->setup->password_confirm, sizeof(context->setup->password_confirm), 1);
            tui_input(&view, T("root password"), context->setup->root_password, sizeof(context->setup->root_password), 1);
            tui_input(&view, T("Confirm root password"), context->setup->root_password_confirm, sizeof(context->setup->root_password_confirm), 1);
            tui_text(&view, T("Desktop environment"));
            tui_compact_radio(&view, T("IceWM (default)"), &desktop, INSTALLER_DESKTOP_ICEWM);
            tui_compact_radio(&view, T("XFCE (experimental)"), &desktop, INSTALLER_DESKTOP_XFCE);
            tui_compact_radio(&view, T("Console only"), &desktop, INSTALLER_DESKTOP_CONSOLE);
            tui_check(&view, T("Create a swapfile"), &swap);
            if (swap) tui_input(&view, T("Swap size in MiB"), swap_text, sizeof(swap_text), 0);
            break;
        case INSTALLER_PAGE_CONFIRM:
            review_page(&view, context, &inspected, &bios, &uefi, swap_text);
            break;
        case INSTALLER_PAGE_FINISH:
            tui_text(&view, *context->install_success ? T("Installation completed successfully.") : T("Operation failed"));
            tui_text(&view, status_text);
            tui_text(&view, detail_text);
            if (*inspected.failure) tui_text(&view, T(inspected.failure));
            tui_text(&view, mode == INSTALLER_MODE_UPDATE ? "/var/log/installer-update.log" : "/var/log/installer.log");
            tui_button(&view, T("View log"), TUI_LOG);
            if (*context->install_success) {
                tui_text(&view, T("Remove the installation media before rebooting."));
                char entry[128] = "";
                int has_entry = *inspected.boot_file != 0;
                if (has_entry) snprintf(entry, sizeof(entry), "%s", inspected.boot_file);
                else if (context->boot_entry_path) has_entry = !context->boot_entry_path(entry, sizeof(entry));
                if (!context->setup->skip_uefi && has_entry) tui_text(&view, entry);
                tui_button(&view, T("Restart"), REBOOT);
                tui_button(&view, T("Shut down"), SHUTDOWN);
            }
            break;
        default:
            finished = 1;
            continue;
        }
        const char *primary = current == INSTALLER_PAGE_CONFIRM ? (mode == INSTALLER_MODE_UPDATE ? T("Update") : T("Install"))
            : current == INSTALLER_PAGE_FINISH ? (*context->install_success ? T("Exit") : T("Retry")) : T("Next");
        int action = tui_step(&view, primary, installer_model_can_go_back(current, *context->install_success),
            installer_model_can_cancel(current, *context->install_success), 200);
        context->setup->skip_bios = !bios;
        context->setup->skip_uefi = !uefi;
        context->setup->desktop = desktop;
        context->setup->swap_enabled = swap;
        context->setup->format_esp = format_esp;
        *context->partition_auto = !scheme;
        *context->selected_partition = chosen;
        *context->install_mode = mode;
        if (current == INSTALLER_PAGE_LANGUAGE && language != (int)language_selection()) {
            if (!setlocale(LC_ALL, language_options[language].locale)) tui_message(T("Language"), T("Cannot select the requested locale."));
        }
        if (current == INSTALLER_PAGE_DISK && disk_choice != previous_disk && disk_choice >= 0 && (uint32_t)disk_choice < *context->disk_count) {
            *context->selected_disk = disk_choice;
            previous_disk = disk_choice;
            context->refresh_partitions();
            chosen = *context->selected_partition;
            format_esp = context->setup->format_esp;
            root_uuid[0] = esp_uuid[0] = 0;
            update_checked = 0;
            inspected = (struct installer_update_info){0};
        }
        if (action == TUI_NONE) continue;
        if (action == TUI_LOG) {
            char log[16384];
            tui_read_log(mode == INSTALLER_MODE_UPDATE ? "/var/log/installer-update.log" : "/var/log/installer.log", log, sizeof(log));
            tui_message(T("Installer log"), log);
            continue;
        }
        if (action == TUI_CANCEL) {
            if (installer_tui_signal || tui_confirm(T("Cancel installation"), T("Exit the installer?"), NULL)) finished = 1;
            continue;
        }
        if (action == REBOOT || action == SHUTDOWN) {
            if (!tui_confirm(action == REBOOT ? T("Restart") : T("Shut down"), T("Remove the installation media before continuing."), NULL)) continue;
            int result = action == REBOOT ? reliefos_system_reboot() : reliefos_system_shutdown();
            if (result < 0) error_message(errno ? -errno : result, T("Power operation failed"));
            continue;
        }
        if (current == INSTALLER_PAGE_DISK && action == REFRESH) {
            context->refresh_disks();
            disk_choice = *context->selected_disk;
            previous_disk = -1;
            root_uuid[0] = esp_uuid[0] = 0;
            update_checked = 0;
            continue;
        }
        if (current == INSTALLER_PAGE_DISK && (action == PICK_ROOT || action == PICK_ESP)) {
            if (*context->selected_disk < 0 || (uint32_t)*context->selected_disk >= *context->disk_count) tui_message(T("Target disk"), T("Select a disk first."));
            else select_update_partition(context, action == PICK_ESP, action == PICK_ESP ? esp_uuid : root_uuid);
            update_checked = 0;
            continue;
        }
        if (current == INSTALLER_PAGE_PARTITIONS && action >= REFRESH && action <= SET_ROLE) {
            partition_action(context, action);
            scheme = !*context->partition_auto;
            chosen = *context->selected_partition;
            format_esp = context->setup->format_esp;
            continue;
        }
        if (action == TUI_BACK) {
            current = installer_model_prev(current, mode);
            reset_confirm();
            view.focus = view.scroll = 0;
            view.previous_focus = -1;
            view.status[0] = 0;
            continue;
        }
        if (action != TUI_NEXT) continue;
        view.status[0] = 0;
        if (current == INSTALLER_PAGE_MODE) {
            update_checked = 0;
            inspected = (struct installer_update_info){0};
            if (previous_mode != mode) {
                bios = uefi = 1;
                context->setup->skip_bios = context->setup->skip_uefi = 0;
                previous_mode = mode;
            }
        }
        if (current == INSTALLER_PAGE_DISK) {
            if (disk_choice < 0 || (uint32_t)disk_choice >= *context->disk_count) {
                snprintf(view.status, sizeof(view.status), "%s", T("Select a disk first."));
                continue;
            }
            if (mode == INSTALLER_MODE_UPDATE) {
                struct tui_job job = {.kind = TUI_JOB_CHECK};
                snprintf(job.root_uuid, sizeof(job.root_uuid), "%s", root_uuid);
                snprintf(job.esp_uuid, sizeof(job.esp_uuid), "%s", esp_uuid);
                int result = tui_run_job(context, &job);
                inspected = job.info;
                if (result) { error_message(result, T(inspected.failure)); continue; }
                update_checked = 1;
                bios = !!(inspected.boot_modes & 1);
                uefi = !!(inspected.boot_modes & 2);
            }
        }
        if (current == INSTALLER_PAGE_PARTITIONS && !context->partition_plan_valid()) {
            snprintf(view.status, sizeof(view.status), "%s", T("Review the required BIOS Boot, ESP and root partitions."));
            continue;
        }
        if (current == INSTALLER_PAGE_ACCOUNTS) {
            if (!installer_setup_valid(context->setup)) {
                snprintf(view.status, sizeof(view.status), "%s", T("Check username and matching passwords (1-32 characters, no spaces)."));
                continue;
            }
            if (swap && installer_swap_parse_mib(swap_text, &context->setup->swap_size_mib)) {
                snprintf(view.status, sizeof(view.status), "%s", T("Enter a valid swap size in MiB."));
                continue;
            }
        }
        if (current == INSTALLER_PAGE_CONFIRM) {
            if (mode == INSTALLER_MODE_FRESH && !confirmation_ok()) {
                snprintf(view.status, sizeof(view.status), "%s", T("The confirmation word does not match."));
                continue;
            }
            if (mode == INSTALLER_MODE_UPDATE && !update_checked) { current = INSTALLER_PAGE_DISK; continue; }
            struct tui_job job = {.kind = mode == INSTALLER_MODE_UPDATE ? TUI_JOB_UPDATE : TUI_JOB_INSTALL};
            job.info = inspected;
            int result = tui_run_job(context, &job);
            inspected = job.info;
            *context->install_success = result == 0;
            if (result && !*status_text) snprintf(status_text, sizeof(status_text), "%s", strerror(-result));
            current = INSTALLER_PAGE_FINISH;
        } else if (current == INSTALLER_PAGE_FINISH) {
            if (*context->install_success) { exit_code = 0; finished = 1; }
            else {
                current = INSTALLER_PAGE_DISK;
                update_checked = 0;
                context->refresh_disks();
                disk_choice = *context->selected_disk;
                previous_disk = -1;
            }
        } else current = installer_model_next(current, mode);
        reset_confirm();
        view.focus = view.scroll = 0;
        view.previous_focus = -1;
    }
    explicit_bzero(context->setup->password, sizeof(context->setup->password));
    explicit_bzero(context->setup->password_confirm, sizeof(context->setup->password_confirm));
    explicit_bzero(context->setup->root_password, sizeof(context->setup->root_password));
    explicit_bzero(context->setup->root_password_confirm, sizeof(context->setup->root_password_confirm));
    tui_close();
    return installer_tui_signal ? 128 + installer_tui_signal : exit_code;
}
