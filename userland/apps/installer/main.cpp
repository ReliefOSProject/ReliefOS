/* ReliefOS installer wizard: FLTK frontend over the shared C install operations.
 *
 * Disk transactions and the TTY frontend remain in C. This file owns only the
 * graphical presentation and forwards user choices to the existing backend. */
#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

extern "C" {
#include <reliefos/blockdev.h>
#include <reliefos/system.h>
}

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include "../fltk_support.h"
#include "../locale_settings.h"
#include "install_ops.h"
#include "installer_capacity.h"
#include "installer_partitions.h"
#include "installer_tty.h"
#include "installer_update.h"
#include "../reliefapk/process.h"
#include "model.h"

enum partition_prompt_action {
    PARTITION_PROMPT_AUTO = 1,
    PARTITION_PROMPT_INITIALIZE,
    PARTITION_PROMPT_CREATE,
    PARTITION_PROMPT_RESIZE,
    PARTITION_PROMPT_RENAME,
    PARTITION_PROMPT_FORMAT,
    PARTITION_PROMPT_DELETE,
};

static const int WINDOW_W = 1024;
static const int WINDOW_H = 700;
static const int SIDEBAR_W = 188;
static const int CONTENT_X = 212;
static const int CONTENT_W = 790;
/* Initialize FLTK colors only after choosing the graphical path.  The TTY
 * frontend must not run toolkit initialization before main selects it. */
static Fl_Color COLOR_BG;
static Fl_Color COLOR_TEXT;
static Fl_Color COLOR_SIDEBAR;
static Fl_Color COLOR_ACTIVE;

static Fl_Window *main_window;
static Fl_Group *content_group;
static Fl_Box *brand_setup;
static Fl_Box *sidebar_rows[INSTALLER_PAGE_COUNT];
static Fl_Button *back_button;
static Fl_Return_Button *primary_button;
static Fl_Button *cancel_button;
static Fl_Hold_Browser *disk_browser;
static Fl_Hold_Browser *partition_browser;
static Fl_Choice *update_root_choice, *update_esp_choice;
static Fl_Check_Button *format_esp_checkbox;
static Fl_Input *confirm_input;
static Fl_Check_Button *xorg_checkbox;
static Fl_Input *swap_size_input;
static char swap_size_text[32];
static int swap_size_initialized, swap_size_invalid;
static Fl_Box *bootloader_hint;
static Fl_Box *partition_hint;
static Fl_Input *account_inputs[5];
static Fl_Box *account_hint;
static Fl_Box *status_box;
static Fl_Box *detail_box;
static Fl_Box *progress_text_box;
static Fl_Box *update_activity_box;
static char update_activity_text[768];
static Fl_Box *progress_percent_box;
static Fl_Progress *progress_bar;
static int ui_busy;
static int primary_pending;
static int back_pending;
static int render_pending;
static int account_input_overflow[5];
static int confirm_input_overflow;
static ra::Process update_process;
static struct installer_update_info update_info;
static Fl_Text_Buffer *update_log_buffer;
static Fl_Text_Display *update_log_display;
static size_t update_stdout_offset, update_stderr_offset;
static int update_job, update_checked, update_protocol_error;
static char update_message[256];

/* Embedded verbatim from the repository LICENSE by apache_license.S. */
extern "C" const char installer_license_text[];

static Fl_Box *make_label(const char *text, int x, int y, int w, int h,
                          int size, int wrap)
{
    Fl_Box *box = new Fl_Box(x, y, w, h);
    box->copy_label(text ? text : "");
    box->box(FL_NO_BOX);
    box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE |
               (wrap ? FL_ALIGN_WRAP : 0));
    box->labelfont(FL_HELVETICA);
    box->labelsize(size);
    box->labelcolor(COLOR_TEXT);
    reliefos_fltk::style(box);
    return box;
}

static Fl_Box *make_heading(const char *text, int y)
{
    Fl_Box *box = make_label(text, CONTENT_X, y, CONTENT_W, 36, 23, 0);
    box->labelfont(FL_HELVETICA_BOLD);
    return box;
}

static Fl_Button *make_button(const char *text, int x, int y, int w, int h,
                              Fl_Callback *callback, void *data)
{
    Fl_Button *button = new Fl_Button(x, y, w, h, text);
    button->callback(callback, data);
    button->labelsize(14);
    reliefos_fltk::style(button);
    return button;
}

static const char *page_label(enum installer_page target)
{
    switch (target) {
    case INSTALLER_PAGE_LANGUAGE: return T("Language");
    case INSTALLER_PAGE_LICENSE: return T("License");
    case INSTALLER_PAGE_WELCOME: return T("Welcome");
    case INSTALLER_PAGE_MODE: return T("Mode");
    case INSTALLER_PAGE_DISK: return T("Disk");
    case INSTALLER_PAGE_BOOTLOADERS: return T("Bootloaders");
    case INSTALLER_PAGE_PARTITIONS: return T("Partitions");
    case INSTALLER_PAGE_ACCOUNTS: return T("Accounts");
    case INSTALLER_PAGE_CONFIRM: return T("Confirm");
    case INSTALLER_PAGE_PROGRESS: return mode_action_text();
    case INSTALLER_PAGE_FINISH: return T("Finish");
    default: return "";
    }
}

static const char *primary_label(void)
{
    switch (installer_model_action((enum installer_page)page,
                                   (enum installer_mode)install_mode,
                                   install_success)) {
    case INSTALLER_ACTION_INSTALL: return T("Install");
    case INSTALLER_ACTION_UPDATE: return T("Update");
    case INSTALLER_ACTION_RETRY: return T("Check Again");
    case INSTALLER_ACTION_RESTART: return T("Restart");
    case INSTALLER_ACTION_CLOSE: return T("Close");
    case INSTALLER_ACTION_NEXT:
        return page == INSTALLER_PAGE_DISK && install_mode == INSTALLER_MODE_UPDATE
            ? T("Check Update") : T("Next");
    }
    return T("Next");
}

static int primary_disabled(void)
{
    if (ui_busy || install_running || page == INSTALLER_PAGE_PROGRESS) return 1;
    if (page == INSTALLER_PAGE_ACCOUNTS) {
        for (int i = 0; i < 5; ++i)
            if (account_input_overflow[i]) return 1;
        return !installer_setup_valid(&setup);
    }
    if (page == INSTALLER_PAGE_DISK)
        return selected_disk < 0 || (uint32_t)selected_disk >= disk_count;
    if (page == INSTALLER_PAGE_PARTITIONS)
        return !installer_partition_plan_valid();
    if (page == INSTALLER_PAGE_CONFIRM && install_mode == INSTALLER_MODE_UPDATE)
        return !update_checked;
    if (page == INSTALLER_PAGE_CONFIRM)
        return confirm_input_overflow || !confirmation_ok() ||
               (install_mode==INSTALLER_MODE_FRESH && setup.swap_enabled && swap_size_invalid);
    return 0;
}

static void update_sidebar(void)
{
    enum installer_page steps[INSTALLER_PAGE_COUNT];
    int count = installer_model_steps((enum installer_mode)install_mode,
                                      steps, INSTALLER_PAGE_COUNT);
    if (brand_setup) brand_setup->copy_label(T("Setup"));
    for (int i = 0; i < INSTALLER_PAGE_COUNT; ++i) {
        Fl_Box *row = sidebar_rows[i];
        if (i < count) {
            int active = steps[i] == (enum installer_page)page;
            row->copy_label(page_label(steps[i]));
            row->color(active ? COLOR_ACTIVE : COLOR_SIDEBAR);
            row->labelcolor(active ? COLOR_TEXT : FL_WHITE);
            row->show();
        } else {
            row->hide();
        }
    }
}

static void update_footer(void)
{
    if (!primary_button) return;
    primary_button->copy_label(primary_label());
    if (installer_model_can_go_back((enum installer_page)page, install_success) &&
        !ui_busy) back_button->activate();
    else back_button->deactivate();
    if (primary_disabled()) primary_button->deactivate();
    else primary_button->activate();
    if (installer_model_can_cancel((enum installer_page)page, install_success) &&
        !ui_busy) cancel_button->activate();
    else cancel_button->deactivate();
}

static void refresh_status_widgets(void)
{
    if (status_box) status_box->copy_label(status_text);
    if (detail_box) detail_box->copy_label(detail_text);
    if (progress_text_box) progress_text_box->copy_label(progress_text);
    if (update_activity_box) update_activity_box->copy_label(update_activity_text);
    if (progress_bar) {
        char percent[16];
        uint32_t value = progress_value > 100 ? 100 : progress_value;
        progress_bar->value((double)value);
        if (progress_percent_box) {
            snprintf(percent, sizeof(percent), "%u%%", (unsigned)value);
            progress_percent_box->copy_label(percent);
        }
    }
}

static void show_page(void);

static void render_deferred(void *)
{
    render_pending = 0;
    show_page();
}

static void schedule_render(void)
{
    if (render_pending) return;
    render_pending = 1;
    Fl::add_timeout(0.0, render_deferred);
}

static void update_pump(void *)
{
    update_process.step(16384);
    const char *output = update_process.output();
    size_t length = update_process.output_size();
    while (update_stdout_offset < length) {
        const char *line = output + update_stdout_offset;
        const char *end = static_cast<const char *>(memchr(line, '\n', length - update_stdout_offset));
        if (!end) break;
        size_t bytes = static_cast<size_t>(end - line);
        char text[512];
        if (bytes < sizeof(text)) {
            memcpy(text, line, bytes);
            text[bytes] = 0;
            unsigned value = progress_value;
            int result = installer_update_decode(&update_info, &value, update_message, sizeof(update_message), text);
            if (result < 0) update_protocol_error = 1;
            if (result == 2 || result == 3) {
                progress_value = value;
                copy_text(progress_text, sizeof(progress_text), T(update_message));
                copy_text(status_text, sizeof(status_text), T(update_message));
            }
            if (result == 2 || result == 3 || result == 4) {
                if (installer_update_activity_format(&update_info.activity, update_activity_text,
                                                     sizeof(update_activity_text)))
                    update_protocol_error = 1;
            }
        }
        update_stdout_offset += bytes + 1;
    }
    size_t errors = update_process.errors_size();
    if (errors > update_stderr_offset) {
        const char *logs = update_process.errors();
        fwrite(logs + update_stderr_offset, 1, errors - update_stderr_offset, stderr);
        update_stderr_offset = errors;
        if (update_log_buffer) {
            size_t start = errors > 65536 ? errors - 65536 : 0;
            while (start < errors && (static_cast<unsigned char>(logs[start]) & 0xc0) == 0x80) ++start;
            update_log_buffer->text(logs + start);
            if (update_log_display) {
                update_log_display->insert_position(update_log_buffer->length());
                update_log_display->show_insert_position();
            }
        }
    }
    refresh_status_widgets();
    if (update_process.running()) { Fl::repeat_timeout(0.08, update_pump); return; }
    int checking = update_job == 1;
    update_job = 0;
    ui_busy = 0;
    install_running = 0;
    if (update_process.exit_code() == 0 && !update_protocol_error && !update_info.error_number) {
        if (checking) {
            update_checked = 1;
            setup.skip_bios = !(update_info.boot_modes & 1u);
            setup.skip_uefi = !(update_info.boot_modes & 2u);
            page = INSTALLER_PAGE_CONFIRM;
            set_status(T("Ready to update"), T("Review the detected system and start the update."));
        } else {
            page = INSTALLER_PAGE_FINISH;
            install_success = 1;
            progress_value = 100;
            set_status(T("System update completed successfully"), T("Remove the installation media, then restart."));
            fprintf(stderr, "[installer.elf] update completed successfully\n");
        }
    } else {
        page = INSTALLER_PAGE_FINISH;
        install_success = 0;
        int error = update_info.error_number ? update_info.error_number
            : update_process.exec_error() ? update_process.exec_error() : EIO;
        char detail[128];
        snprintf(detail, sizeof(detail), "%s (%d)", strerror(error), error);
        set_status(update_info.failure[0] ? T(update_info.failure) : T("Update worker failed"), detail);
    }
    show_page();
}

static void start_update_job(int applying)
{
    if (update_job || selected_disk < 0 || static_cast<uint32_t>(selected_disk) >= disk_count) return;
    if (applying && !update_checked) return;
    if (!applying) memset(&update_info, 0, sizeof(update_info));
    update_checked = 0;
    update_protocol_error = 0;
    memset(&update_info.activity, 0, sizeof(update_info.activity));
    update_activity_text[0] = 0;
    update_stdout_offset = update_stderr_offset = 0;
    if (!update_log_buffer) update_log_buffer = new Fl_Text_Buffer();
    update_log_buffer->text("");
    char executable[PATH_MAX], modes[2] = {
        static_cast<char>('0' + (setup.skip_bios ? 0 : 1) + (setup.skip_uefi ? 0 : 2)), 0};
    ssize_t bytes = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (bytes < 0 || static_cast<size_t>(bytes) >= sizeof(executable) - 1) {
        page = INSTALLER_PAGE_FINISH;
        install_success = 0;
        set_status(T("Cannot start the update worker"), strerror(bytes < 0 ? errno : ENAMETOOLONG));
        show_page();
        return;
    }
    executable[bytes] = 0;
    char root_uuid[37] = {0}, esp_uuid[37] = {0};
    if (!applying) {
        int error = 0;
        if (installer_root_partition >= 0)
            error = reliefos_block_partition_uuid(disks[selected_disk].path, installer_root_partition, root_uuid);
        if (!error && installer_esp_partition >= 0)
            error = reliefos_block_partition_uuid(disks[selected_disk].path, installer_esp_partition, esp_uuid);
        if (error) { set_error_status(T("Cannot identify the selected partitions"), error); return; }
    }
    const char *check[] = {executable, "--update-worker", "check", disks[selected_disk].path, root_uuid, esp_uuid, NULL};
    const char *apply[] = {executable, "--update-worker", "apply", update_info.disk,
        update_info.root_uuid, update_info.esp_uuid, update_info.index_sha256, update_info.database_sha256,
        update_info.world_sha256, modes, NULL};
    if (!update_process.start(applying ? apply : check, false, ra::AuthMode::Sudo, false)) {
        page = INSTALLER_PAGE_FINISH;
        install_success = 0;
        set_status(T("Cannot start the update worker"), strerror(update_process.exec_error() ? update_process.exec_error() : EIO));
        show_page();
        return;
    }
    update_job = applying ? 2 : 1;
    ui_busy = 1;
    install_running = 1;
    install_success = 0;
    page = INSTALLER_PAGE_PROGRESS;
    progress_value = 0;
    copy_text(progress_text, sizeof(progress_text), applying ? T("Preparing the system update") : T("Checking the selected system"));
    show_page();
    Fl::add_timeout(0.0, update_pump);
}

static void close_now(void)
{
    exit(0);
}

static void cancel_setup(Fl_Widget *, void *)
{
    if (!ui_busy && installer_model_can_cancel((enum installer_page)page,
                                                install_success))
        close_now();
}

static void window_close(Fl_Widget *, void *)
{
    cancel_setup(NULL, NULL);
}

static void go_back_now(void *)
{
    back_pending = 0;
    if (ui_busy || !installer_model_can_go_back((enum installer_page)page,
                                                 install_success))
        return;
    page = (uint8_t)installer_model_prev((enum installer_page)page,
                                         (enum installer_mode)install_mode);
    if (page == INSTALLER_PAGE_DISK) {
        ui_busy = 1;
        refresh_disks();
        ui_busy = 0;
    }
    if (page == INSTALLER_PAGE_PARTITIONS) refresh_partitions();
    show_page();
}

static void go_back(Fl_Widget *, void *)
{
    if (back_pending) return;
    back_pending = 1;
    Fl::add_timeout(0.0, go_back_now);
}

static void go_primary_now(void *)
{
    primary_pending = 0;
    if (primary_disabled()) return;
    switch (installer_model_action((enum installer_page)page,
                                   (enum installer_mode)install_mode,
                                   install_success)) {
    case INSTALLER_ACTION_INSTALL:
        ui_busy = 1;
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        Fl::flush();
        perform_install();
        ui_busy = 0;
        show_page();
        return;
    case INSTALLER_ACTION_UPDATE:
        start_update_job(1);
        return;
    case INSTALLER_ACTION_RETRY:
        start_update_job(0);
        return;
    case INSTALLER_ACTION_RESTART:
        fprintf(stderr, "[installer.elf] restart requested from completion page\n");
        if (reliefos_system_reboot() < 0) {
            reboot_error = errno;
            fprintf(stderr, "[installer.elf] restart failed: %s\n",
                    strerror(reboot_error));
            show_page();
        }
        return;
    case INSTALLER_ACTION_CLOSE:
        close_now();
        return;
    case INSTALLER_ACTION_NEXT:
        break;
    }

    if (page == INSTALLER_PAGE_DISK && install_mode == INSTALLER_MODE_UPDATE) {
        start_update_job(0);
        return;
    }
    page = (uint8_t)installer_model_next((enum installer_page)page,
                                         (enum installer_mode)install_mode);
    if (page == INSTALLER_PAGE_DISK) {
        ui_busy = 1;
        refresh_disks();
        ui_busy = 0;
    }
    if (page == INSTALLER_PAGE_PARTITIONS) refresh_partitions();
    if (page == INSTALLER_PAGE_CONFIRM) {
        reset_confirm();
        confirm_input_overflow = 0;
    }
    show_page();
}

static void go_primary(void)
{
    if (primary_pending) return;
    primary_pending = 1;
    Fl::add_timeout(0.0, go_primary_now);
}

static void go_next(Fl_Widget *, void *)
{
    go_primary();
}

static void language_selected(Fl_Widget *, void *data)
{
    unsigned target = (unsigned)(uintptr_t)data;
    if (target >= sizeof(language_options) / sizeof(language_options[0])) return;
    if (target != language_selection() &&
        !setlocale(LC_ALL, language_options[target].locale))
        return;
    reliefos_fltk::translate_dialogs();
    main_window->copy_label(T("Install ReliefOS"));
    back_button->copy_label(T("Previous Step"));
    cancel_button->copy_label(T("Cancel"));
    schedule_render();
}

static void build_language_page(void)
{
    make_heading(T("Select Language"), 20);
    make_label(T("Choose the language for Setup and the installed system."),
               CONTENT_X, 62, CONTENT_W, 30, 15, 1);
    for (unsigned i = 0; i < sizeof(language_options) / sizeof(language_options[0]); ++i) {
        Fl_Round_Button *option = new Fl_Round_Button(CONTENT_X + 20, 126 + (int)i * 48,
                                                     360, 34, language_options[i].name);
        option->type(FL_RADIO_BUTTON);
        option->value(language_selection() == i);
        option->callback(language_selected, (void *)(uintptr_t)i);
        reliefos_fltk::style(option);
    }
    make_label(T("The installed system will use the same language."),
               CONTENT_X, 286, CONTENT_W, 32, 15, 1);
}

static void build_license_page(void)
{
    make_heading("Apache License 2.0", 20);
    static Fl_Text_Buffer license_buffer;
    if (!license_buffer.length()) license_buffer.text(installer_license_text);
    Fl_Text_Display *text = new Fl_Text_Display(CONTENT_X, 62, CONTENT_W, 536);
    text->buffer(license_buffer);
    text->textfont(FL_COURIER);
    text->textsize(13);
    text->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    reliefos_fltk::style(text);
}

static void build_welcome_page(void)
{
    make_heading(T("ReliefOS Setup"), 20);
    make_label(T("Install a new system or update an existing ReliefOS disk."),
               CONTENT_X, 62, CONTENT_W, 34, 15, 1);
    make_label(T("Setup can copy the full normal system payload"),
               CONTENT_X, 116, CONTENT_W, 30, 15, 1);
    make_label(T("or replace the boot/reliefos and system files on an existing installation."),
               CONTENT_X, 150, CONTENT_W, 44, 15, 1);
    make_label(T("SATA/AHCI and IDE/PATA target disks are supported."),
               CONTENT_X, 218, CONTENT_W, 32, 15, 1);
}

/** @brief Start a fresh mode with both firmware defaults after leaving update mode. */
static void mode_selected(Fl_Widget *widget, void *data)
{
    if (!static_cast<Fl_Round_Button *>(widget)->value()) return;
    if ((uint8_t)(uintptr_t)data == INSTALLER_MODE_FRESH && install_mode != INSTALLER_MODE_FRESH)
        setup.skip_bios = setup.skip_uefi = 0;
    install_mode = (uint8_t)(uintptr_t)data;
    schedule_render();
}

static void build_mode_page(void)
{
    make_heading(T("Choose Setup Mode"), 20);
    make_label(T("Fresh install erases the disk. Update keeps existing users and extra programs."),
               CONTENT_X, 62, CONTENT_W, 44, 15, 1);
    Fl_Round_Button *fresh = new Fl_Round_Button(CONTENT_X + 20, 124, 600, 36,
                                                  T("Fresh Install"));
    Fl_Round_Button *update = new Fl_Round_Button(CONTENT_X + 20, 170, 600, 36,
                                                   T("Update Existing System"));
    fresh->type(FL_RADIO_BUTTON);
    update->type(FL_RADIO_BUTTON);
    fresh->value(install_mode == INSTALLER_MODE_FRESH);
    update->value(install_mode == INSTALLER_MODE_UPDATE);
    fresh->callback(mode_selected, (void *)(uintptr_t)INSTALLER_MODE_FRESH);
    update->callback(mode_selected, (void *)(uintptr_t)INSTALLER_MODE_UPDATE);
    reliefos_fltk::style(fresh);
    reliefos_fltk::style(update);
    make_label(T("Prepare a clean ReliefOS system with automatic or manual disk partitioning."),
               CONTENT_X, 228, CONTENT_W, 40, 15, 1);
    make_label(T("Choose the target disk, configure its partitions, then replace boot, system and EFI files."),
               CONTENT_X, 278, CONTENT_W, 42, 15, 1);
}

static void update_partition_selected(Fl_Widget *widget, void *)
{
    auto *choice = static_cast<Fl_Choice *>(widget);
    int index = choice->mvalue() ? int((intptr_t)choice->mvalue()->user_data()) - 1 : -1;
    if (widget == update_root_choice) installer_root_partition = index;
    else installer_esp_partition = index;
    update_checked = 0;
}

static void refresh_update_partitions(void)
{
    if (!update_root_choice || !update_esp_choice) return;
    refresh_partitions();
    update_root_choice->clear(); update_esp_choice->clear();
    update_root_choice->add(T("Automatically identify a single ReliefOS installation"));
    update_esp_choice->add(T("Use the selected installation's fstab"));
    update_root_choice->value(0); update_esp_choice->value(0);
    for (uint32_t i = 0; i < partition_count; ++i) {
        char line[256]; format_partition_line(line, sizeof(line), &partitions[i]);
        Fl_Choice *choice = partitions[i].gpt_type == RELIEFOS_BLOCK_GPT_ESP ? update_esp_choice :
            partitions[i].gpt_type == RELIEFOS_BLOCK_GPT_LINUX ? update_root_choice : nullptr;
        if (!choice) continue;
        char escaped[sizeof(line) * 2]; size_t used = 0;
        for (const char *p = line; *p; ++p) {
            if (*p == '/' || *p == '\\' || *p == '&') escaped[used++] = '\\';
            escaped[used++] = *p;
        }
        escaped[used] = 0;
        choice->add(escaped, 0, nullptr, (void *)(intptr_t)(partitions[i].index + 1));
        int selected = choice == update_root_choice ? installer_root_partition : installer_esp_partition;
        if (selected == (int)partitions[i].index) choice->value(choice->size() - 2);
    }
}

static void select_disk(Fl_Widget *, void *)
{
    if (disk_browser && disk_browser->value() > 0)
        selected_disk = disk_browser->value() - 1;
    setup.format_esp = 0;
    refresh_update_partitions();
    update_footer();
}

static void refresh_disk_list(void)
{
    if (!disk_browser) return;
    char line[160];
    disk_browser->clear();
    for (uint32_t i = 0; i < disk_count && i < RELIEFOS_BLOCK_MAX_DISKS; ++i) {
        format_disk_line(line, sizeof(line), &disks[i]);
        disk_browser->add(line);
    }
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count)
        disk_browser->value(selected_disk + 1);
    refresh_update_partitions();
    update_footer();
    refresh_status_widgets();
}

static void refresh_pressed(Fl_Widget *, void *)
{
    if (ui_busy) return;
    ui_busy = 1;
    refresh_disks();
    refresh_disk_list();
    ui_busy = 0;
    update_footer();
}

static void build_disk_page(void)
{
    make_heading(install_mode == INSTALLER_MODE_UPDATE
                     ? T("Select Disk to Update") : T("Select Installation Disk"), 20);
    make_label(install_mode == INSTALLER_MODE_UPDATE
                   ? T("Choose the disk that already contains ReliefOS.")
                   : T("Choose the disk that will receive ReliefOS."),
               CONTENT_X, 62, 600, 32, 15, 1);
    make_button(T("Refresh"), CONTENT_X + 650, 22, 130, 34, refresh_pressed, NULL);
    make_label(T("Available disks"), CONTENT_X, 102, CONTENT_W, 26, 14, 0);
    disk_browser = new Fl_Hold_Browser(CONTENT_X, 132, CONTENT_W,
                                     install_mode == INSTALLER_MODE_UPDATE ? 190 : 330);
    disk_browser->callback(select_disk);
    disk_browser->textsize(14);
    disk_browser->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(disk_browser);
    if (install_mode == INSTALLER_MODE_UPDATE) {
        make_label(T("ReliefOS system partition"), CONTENT_X, 330, CONTENT_W, 24, 14, 0);
        update_root_choice = new Fl_Choice(CONTENT_X, 356, CONTENT_W, 32);
        make_label(T("EFI system partition"), CONTENT_X, 396, CONTENT_W, 24, 14, 0);
        update_esp_choice = new Fl_Choice(CONTENT_X, 422, CONTENT_W, 32);
        update_root_choice->callback(update_partition_selected);
        update_esp_choice->callback(update_partition_selected);
        reliefos_fltk::style(update_root_choice); reliefos_fltk::style(update_esp_choice);
    }
    status_box = make_label(status_text, CONTENT_X, 478, CONTENT_W, 34, 14, 1);
    detail_box = make_label(detail_text, CONTENT_X, 516, CONTENT_W, 48, 14, 1);
    refresh_disk_list();
}

static void refresh_partition_list(void)
{
    if (!partition_browser) return;
    if (format_esp_checkbox) format_esp_checkbox->value(setup.format_esp);
    char line[640];
    if (partition_hint) {
        struct installer_partition_plan plan;
        if (installer_partition_auto && selected_disk >= 0 &&
            (uint32_t)selected_disk < disk_count &&
            !installer_partition_plan_default(&disks[selected_disk], installer_boot_modes(), &plan)) {
            snprintf(line, sizeof(line), setup.skip_bios
                ? T("Automatic plan: ESP 128 MiB FAT32; root %llu MiB ext4.\nThe list shows current partitions. The plan is applied only after confirmation.")
                : T("Automatic plan: BIOS Boot 1 MiB; ESP 128 MiB FAT32; root %llu MiB ext4.\nThe list shows current partitions. The plan is applied only after confirmation."),
                (unsigned long long)(plan.root.sector_count / 2048u));
            partition_hint->copy_label(line);
        } else {
            partition_hint->copy_label(installer_partition_auto
                ? T("The automatic layout does not fit this disk. Select another disk or configure partitions manually.")
                : setup.skip_bios
                    ? T("Manual layout: assign a FAT32 ESP and an ext4 root. BIOS Boot is not required for the selected bootloaders.")
                    : T("Manual layout: assign BIOS Boot, ESP and root roles. BIOS Boot stays unformatted."));
        }
    }
    partition_browser->clear();
    for (uint32_t i = 0; i < partition_count; ++i) {
        format_partition_line(line, sizeof(line), &partitions[i]);
        partition_browser->add(line);
    }
    if (selected_partition >= 0 && (uint32_t)selected_partition < partition_count)
        partition_browser->value(selected_partition + 1);
    refresh_status_widgets();
    update_footer();
}

static void select_partition(Fl_Widget *, void *)
{
    if (partition_browser && partition_browser->value() > 0)
        selected_partition = partition_browser->value() - 1;
    update_footer();
}

static void partition_result(int ret, const char *success)
{
    char detail[128];
    if (ret < 0) {
        snprintf(detail, sizeof(detail), "%s ret=%d",
                 T("The disk may have been changed"), ret);
        set_status(T("Partition operation failed"), detail);
    } else {
        set_status(T("Partition layout updated"), success);
    }
    refresh_partitions();
    refresh_partition_list();
}

static uint32_t partition_filesystem(const char *text)
{
    if (text_eq(text, "fat32") || text_eq(text, "fat"))
        return RELIEFOS_BLOCK_FILESYSTEM_FAT32;
    if (text_eq(text, "ext2")) return RELIEFOS_BLOCK_FILESYSTEM_EXT2;
    if (text_eq(text, "ext4")) return RELIEFOS_BLOCK_FILESYSTEM_EXT4;
    if (text_eq(text, "exfat")) return RELIEFOS_BLOCK_FILESYSTEM_EXFAT;
    return RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
}

static char *partition_next_word(char **cursor)
{
    char *word;
    if (!cursor || !*cursor) return NULL;
    while (**cursor == ' ' || **cursor == '\t') ++*cursor;
    if (!**cursor) return NULL;
    word = *cursor;
    while (**cursor && **cursor != ' ' && **cursor != '\t') ++*cursor;
    if (**cursor) *(*cursor)++ = 0;
    return word;
}

static int partition_collect_name(char *out, uint32_t capacity,
                                 const char *first, char **cursor)
{
    char *remainder;
    size_t length;
    if (!out || !capacity || !first || !first[0]) return -EINVAL;
    length = strlen(first);
    remainder = cursor ? *cursor : NULL;
    while (remainder && (*remainder == ' ' || *remainder == '\t')) ++remainder;
    if (length >= capacity) return -ENAMETOOLONG;
    memcpy(out, first, length);
    if (remainder && *remainder) {
        size_t remaining = strlen(remainder);
        if (length + 1 + remaining >= capacity) return -ENAMETOOLONG;
        out[length++] = ' ';
        memcpy(out + length, remainder, remaining + 1);
    } else {
        out[length] = 0;
    }
    if (cursor) *cursor = remainder;
    return 0;
}

static int partition_number(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !text[0] || !value) return -EINVAL;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed > 0xffffffffUL || !parsed)
        return -EINVAL;
    *value = (uint32_t)parsed;
    return 0;
}

static const char *partition_prompt_text(enum partition_prompt_action action)
{
    switch (action) {
    case PARTITION_PROMPT_AUTO:
        return setup.skip_bios
            ? T("Type AUTO to erase the disk and create ESP and root partitions:")
            : T("Type AUTO to erase the disk and create BIOS Boot, ESP and root partitions:");
    case PARTITION_PROMPT_INITIALIZE:
        return T("Type INITIALIZE to replace the partition table:");
    case PARTITION_PROMPT_CREATE:
        return T("Enter size_MiB filesystem name (ext4/ext2/fat32/exfat/raw):");
    case PARTITION_PROMPT_RESIZE:
        return T("Enter the new partition size in MiB:");
    case PARTITION_PROMPT_RENAME:
        return T("Enter the GPT partition name:");
    case PARTITION_PROMPT_FORMAT:
        return T("Enter filesystem and label (for example: ext4 RELIEFOS_DATA):");
    case PARTITION_PROMPT_DELETE:
        return T("Type DELETE to remove the selected partition:");
    }
    return T("Enter a value:");
}

static const char *partition_prompt_default(enum partition_prompt_action action)
{
    switch (action) {
    case PARTITION_PROMPT_AUTO: return "AUTO";
    case PARTITION_PROMPT_INITIALIZE: return "INITIALIZE";
    case PARTITION_PROMPT_CREATE: return "1024 ext4 ReliefOS Data";
    case PARTITION_PROMPT_RESIZE: return "1024";
    case PARTITION_PROMPT_RENAME: return "ReliefOS Data";
    case PARTITION_PROMPT_FORMAT: return "ext4 RELIEFOS_DATA";
    case PARTITION_PROMPT_DELETE: return "DELETE";
    }
    return "";
}

static void partition_prompt_pressed(Fl_Widget *, void *data)
{
    enum partition_prompt_action action =
        (enum partition_prompt_action)(uintptr_t)data;
    if (ui_busy) return;

    if (action == PARTITION_PROMPT_FORMAT) {
        int choice = fl_choice(
            "%s", T("Cancel"), T("Continue"), NULL,
            T("Formatting destroys all files on the selected partition."));
        if (choice != 1) return;
    }

    const char *entered = fl_input("%s", partition_prompt_default(action),
                                   partition_prompt_text(action));
    if (!entered) return;
    char value[256];
    if (!reliefos_fltk::copy_input(value, sizeof(value), entered)) {
        partition_result(-ENAMETOOLONG, "");
        return;
    }

    char *cursor = value;
    char *word = partition_next_word(&cursor);
    uint32_t number = 0, filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    char name[RELIEFOS_BLOCK_NAME_LEN];
    name[0] = 0;
    int ret = -EINVAL;

    if (action == PARTITION_PROMPT_AUTO) {
        if (text_eq(entered, "AUTO")) ret = installer_partition_auto_layout();
    } else if (action == PARTITION_PROMPT_INITIALIZE) {
        if (text_eq(entered, "INITIALIZE")) ret = installer_partition_initialize();
    } else if (action == PARTITION_PROMPT_CREATE) {
        char *filesystem_name = partition_next_word(&cursor);
        const char *name_word = partition_next_word(&cursor);
        filesystem = partition_filesystem(filesystem_name);
        if (!name_word) name_word = "ReliefOS Data";
        if (partition_collect_name(name, sizeof(name), name_word, &cursor) < 0)
            name[0] = 0;
        if (!partition_number(word, &number) &&
            (filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN || text_eq(filesystem_name, "raw")) && name[0])
            ret = installer_partition_create(filesystem, number, name);
    } else if (action == PARTITION_PROMPT_RESIZE) {
        if (!partition_number(word, &number))
            ret = installer_partition_resize(number);
    } else if (action == PARTITION_PROMPT_RENAME) {
        if (word && !partition_collect_name(name, sizeof(name), word, &cursor))
            ret = installer_partition_rename(name);
    } else if (action == PARTITION_PROMPT_FORMAT) {
        const char *label = partition_next_word(&cursor);
        filesystem = partition_filesystem(word);
        if (!label) label = "RELIEFOS_DATA";
        if (filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN)
            ret = installer_partition_format(filesystem, label);
    } else if (action == PARTITION_PROMPT_DELETE) {
        if (text_eq(entered, "DELETE")) ret = installer_partition_delete();
    }
    partition_result(ret, T("Review the selected ESP and root before continuing."));
}

static void partition_refresh_pressed(Fl_Widget *, void *)
{
    refresh_partitions();
    refresh_partition_list();
}

static void partition_role_pressed(Fl_Widget *, void *data)
{
    uint32_t type = (uint32_t)(uintptr_t)data;
    int ret;
    if (ui_busy) return;
    if (selected_partition < 0 ||
        (uint32_t)selected_partition >= partition_count) {
        partition_result(-EINVAL, "");
        return;
    }
    ret = installer_partition_set_type(type);
    partition_result(ret, T("Partition role updated."));
}

static void format_esp_changed(Fl_Widget *widget, void *)
{
    setup.format_esp = static_cast<Fl_Check_Button *>(widget)->value() != 0;
    update_footer();
}

static void build_partition_page(void)
{
    static const char *const labels[] = {
        "Refresh", "Automatic", "Initialize GPT", "Create",
        "Resize", "Rename", "Format", "Delete"
    };
    static const enum partition_prompt_action actions[] = {
        PARTITION_PROMPT_AUTO, PARTITION_PROMPT_INITIALIZE,
        PARTITION_PROMPT_CREATE, PARTITION_PROMPT_RESIZE,
        PARTITION_PROMPT_RENAME, PARTITION_PROMPT_FORMAT,
        PARTITION_PROMPT_DELETE
    };

    make_heading(T("Configure partitions"), 16);
    partition_hint = make_label("", CONTENT_X, 52, CONTENT_W, 68, 14, 1);
    partition_browser = new Fl_Hold_Browser(CONTENT_X, 128, CONTENT_W, 248);
    partition_browser->callback(select_partition);
    partition_browser->textsize(13);
    partition_browser->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(partition_browser);

    int button_w = 94;
    int gap = 5;
    for (int i = 0; i < 8; ++i) {
        int x = CONTENT_X + i * (button_w + gap);
        Fl_Button *button = make_button(T(labels[i]), x, 388, button_w, 36,
                                        i == 0 ? partition_refresh_pressed
                                               : partition_prompt_pressed,
                                        i == 0 ? NULL : (void *)(uintptr_t)actions[i - 1]);
        button->labelsize(12);
    }
    make_button(T("Set ESP"), CONTENT_X, 432, 150, 36,
                partition_role_pressed,
                (void *)(uintptr_t)RELIEFOS_BLOCK_GPT_ESP);
    make_button(T("Set root"), CONTENT_X + 160, 432, 150, 36,
                partition_role_pressed,
                (void *)(uintptr_t)RELIEFOS_BLOCK_GPT_LINUX);
    make_button(T("Data type"), CONTENT_X + 320, 432, 150, 36,
                partition_role_pressed,
                (void *)(uintptr_t)RELIEFOS_BLOCK_GPT_BASIC_DATA);
    make_button(T("Set BIOS Boot"), CONTENT_X + 480, 432, 170, 36,
                partition_role_pressed,
                (void *)(uintptr_t)RELIEFOS_BLOCK_GPT_BIOS_BOOT);

    status_box = make_label(status_text, CONTENT_X, 480, CONTENT_W, 30, 13, 1);
    detail_box = make_label(detail_text, CONTENT_X, 512, CONTENT_W, 38, 13, 1);
    make_label(T("Create: size in MiB, filesystem, name. Resize only changes the trailing edge."),
               CONTENT_X, 552, CONTENT_W, 26, 12, 1);
    make_label(installer_partition_auto
                   ? T("Final confirmation formats a FAT32 ESP and ext4 root on the selected disk.")
                   : T("The selected root is formatted. An existing FAT32 ESP is preserved by default."),
               CONTENT_X, 578, CONTENT_W, 32, 12, 1);
    if (!installer_partition_auto) {
        auto *format = new Fl_Check_Button(CONTENT_X, 613, CONTENT_W, 28,
            T("Format selected ESP (erase all EFI boot files on it)"));
        format->value(setup.format_esp);
        format_esp_checkbox = format;
        format->callback(format_esp_changed);
        reliefos_fltk::style(format);
    }
    refresh_partition_list();
}

static void account_changed(Fl_Widget *widget, void *)
{
    for (int i = 0; i < 5; ++i) {
        if (!account_inputs[i]) continue;
        const char *value = account_inputs[i]->value();
        char *destination = i == 0 ? setup.username :
                            i == 1 ? setup.password :
                            i == 2 ? setup.password_confirm :
                            i == 3 ? setup.root_password :
                                     setup.root_password_confirm;
        uint32_t capacity = i == 0 ? sizeof(setup.username) :
                                     sizeof(setup.password);
        if (widget == account_inputs[i])
            account_input_overflow[i] =
                !reliefos_fltk::copy_input(destination, capacity, value);
    }
    if (account_hint) {
        int overflow = 0;
        for (int i = 0; i < 5; ++i) overflow |= account_input_overflow[i];
        if (overflow || (setup.username[0] && !installer_setup_valid(&setup)))
            account_hint->show();
        else
            account_hint->hide();
    }
    update_footer();

    if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
        for (int i = 0; i < 5; ++i) {
            if (widget != account_inputs[i]) continue;
            if (i == 4) {
                go_primary();
            } else if (account_inputs[i + 1]) {
                Fl::focus(account_inputs[i + 1]);
            }
            return;
        }
    }
}

static void build_accounts_page(void)
{
    static const char *const labels[5] = {
        "Standard user name",
        "Standard user password",
        "Confirm password",
        "root password",
        "Confirm root password"
    };
    char *values[5] = {
        setup.username, setup.password, setup.password_confirm,
        setup.root_password, setup.root_password_confirm
    };
    for (int i = 0; i < 5; ++i) account_input_overflow[i] = 0;
    make_heading(T("Accounts"), 16);
    make_label(T("Administrator: root"), CONTENT_X, 52, CONTENT_W, 28, 14, 1);
    for (int i = 0; i < 5; ++i) {
        int y = 92 + i * 82;
        make_label(T(labels[i]), CONTENT_X, y, CONTENT_W, 25, 14, 0);
        account_inputs[i] = new Fl_Input(CONTENT_X, y + 28, 610, 34);
        account_inputs[i]->value(values[i]);
        account_inputs[i]->maximum_size(i == 0
            ? (int)sizeof(setup.username) - 1
            : (int)sizeof(setup.password) - 1);
        if (i) account_inputs[i]->type(FL_SECRET_INPUT);
        account_inputs[i]->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
        account_inputs[i]->callback(account_changed);
        reliefos_fltk::style(account_inputs[i]);
    }
    account_hint = make_label(
        T("Passwords: 1-32 characters, no spaces; confirmations must match"),
        CONTENT_X, 516, CONTENT_W, 32, 13, 1);
    if (!setup.username[0] || installer_setup_valid(&setup))
        account_hint->hide();
}

static void confirm_changed(Fl_Widget *widget, void *)
{
    confirm_input_overflow =
        !reliefos_fltk::copy_input(confirm_text, sizeof(confirm_text),
                                   ((Fl_Input *)widget)->value());
    update_footer();
    if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)
        go_primary();
}

static void xorg_install_changed(Fl_Widget *widget, void *)
{
    setup.skip_xorg = static_cast<Fl_Check_Button *>(widget)->value() ? 0 : 1;
}

/** @brief Retain a checked editable MiB value across page rebuilds. */
static void swap_size_changed(Fl_Widget *widget,void *)
{
    const char *value=static_cast<Fl_Input *>(widget)->value();
    swap_size_invalid=!reliefos_fltk::copy_input(swap_size_text,sizeof(swap_size_text),value);
    uint64_t size=0;
    if (!swap_size_invalid) swap_size_invalid=installer_swap_parse_mib(swap_size_text,&size)!=0;
    if (!swap_size_invalid) setup.swap_size_mib=size;
    update_footer();
}

/** @brief Change only the fresh-install swap choice. */
static void swap_install_changed(Fl_Widget *widget,void *)
{
    setup.swap_enabled=static_cast<Fl_Check_Button *>(widget)->value()!=0;
    if (swap_size_input) {
        if (setup.swap_enabled) swap_size_input->activate();
        else swap_size_input->deactivate();
    }
    update_footer();
}

/** @brief Apply one firmware choice and explain the effect of skipping both. */
static const char *bootloader_description(void)
{
    if (setup.skip_bios && setup.skip_uefi)
        return T("No bootloader selected. A new installation will need a separately configured bootloader.");
    return setup.skip_bios
        ? T("Automatic partitioning creates a FAT32 ESP and an ext4 root. No BIOS Boot partition is needed.")
        : T("Automatic partitioning creates an unformatted BIOS Boot partition, a FAT32 ESP and an ext4 root. BIOS also uses the FAT32 partition for boot files.");
}

static void bootloader_install_changed(Fl_Widget *widget, void *data)
{
    uint8_t skip = static_cast<Fl_Check_Button *>(widget)->value() ? 0 : 1;
    if ((uintptr_t)data == 1) setup.skip_bios = skip;
    else setup.skip_uefi = skip;
    if (bootloader_hint) {
        bootloader_hint->copy_label(bootloader_description());
        bootloader_hint->redraw();
    }
}

static void build_bootloader_page(void)
{
    make_heading(T("Choose bootloaders"), 16);
    make_label(T("Select the firmware used to start this disk. You can select both BIOS and UEFI."),
               CONTENT_X, 62, CONTENT_W, 54, 15, 1);
    Fl_Check_Button *bios = new Fl_Check_Button(CONTENT_X, 138, CONTENT_W, 32,
                                                T("Install or update BIOS bootloader"));
    bios->value(setup.skip_bios ? 0 : 1);
    bios->callback(bootloader_install_changed, (void *)(uintptr_t)1);
    reliefos_fltk::style(bios);
    Fl_Check_Button *uefi = new Fl_Check_Button(CONTENT_X, 182, CONTENT_W, 32,
                                                T("Install or update UEFI bootloader"));
    uefi->value(setup.skip_uefi ? 0 : 1);
    uefi->callback(bootloader_install_changed, (void *)(uintptr_t)2);
    reliefos_fltk::style(uefi);
    bootloader_hint = make_label(bootloader_description(),
                                 CONTENT_X, 244, CONTENT_W, 100, 14, 1);
}

static void build_confirm_page(void)
{
    if (install_mode == INSTALLER_MODE_UPDATE) {
        char text[256];
        make_heading(T("Review system update"), 16);
        make_label(T("Installed version"), CONTENT_X, 66, 200, 26, 14, 0);
        make_label(update_info.installed_version, CONTENT_X + 215, 66, 220, 26, 14, 0);
        make_label(T("Media version"), CONTENT_X + 445, 66, 170, 26, 14, 0);
        make_label(update_info.media_version, CONTENT_X + 620, 66, 160, 26, 14, 0);
        make_label(T("Target disk"), CONTENT_X, 112, 200, 26, 14, 0);
        make_label(update_info.disk, CONTENT_X + 215, 112, 560, 26, 14, 0);
        make_label(T("System partition"), CONTENT_X, 150, 200, 26, 14, 0);
        make_label(update_info.root_device, CONTENT_X + 215, 150, 560, 26, 14, 0);
        make_label(T("Boot partition"), CONTENT_X, 188, 200, 26, 14, 0);
        make_label(update_info.esp_device, CONTENT_X + 215, 188, 560, 26, 14, 0);
        make_label(T("Desktop environment"), CONTENT_X, 226, 200, 26, 14, 0);
        make_label(!strcmp(update_info.profile, "console") ? T("Console") : "Xorg / IceWM",
                   CONTENT_X + 215, 226, 560, 26, 14, 0);
        make_label(T("Free / required space"), CONTENT_X, 264, 200, 26, 14, 0);
        snprintf(text, sizeof(text), "%llu MiB / %llu MiB",
                 (unsigned long long)(update_info.free_bytes / (1024u * 1024u)),
                 (unsigned long long)((update_info.required_bytes + 1024u * 1024u - 1) / (1024u * 1024u)));
        make_label(text, CONTENT_X + 215, 264, 560, 26, 14, 0);
        make_label(T("Update bootloader software"), CONTENT_X, 310, CONTENT_W, 26, 14, 0);
        Fl_Check_Button *bios = new Fl_Check_Button(CONTENT_X, 346, 175, 30, "BIOS");
        bios->value(!setup.skip_bios);
        bios->callback(bootloader_install_changed, (void *)(uintptr_t)1);
        reliefos_fltk::style(bios);
        Fl_Check_Button *uefi = new Fl_Check_Button(CONTENT_X + 190, 346, 175, 30, "UEFI");
        uefi->value(!setup.skip_uefi);
        uefi->callback(bootloader_install_changed, (void *)(uintptr_t)2);
        reliefos_fltk::style(uefi);
        make_label(T("Accounts, personal files, installed software and local configuration are preserved."),
                   CONTENT_X, 398, CONTENT_W, 46, 14, 1);
        if (update_info.pending[0])
            make_label(T("A previous update is incomplete. This attempt will verify the system and recover the pending update stages."),
                       CONTENT_X, 464, CONTENT_W, 60, 14, 1);
        else
            make_label(T("Signed packages are checked before changes. Boot files are published after the package transaction succeeds."),
                       CONTENT_X, 464, CONTENT_W, 60, 14, 1);
        return;
    }
    char line[160];
    char target[192];
    make_heading(T("Confirm Installation"), 16);
    make_label(T("This operation is destructive."),
               CONTENT_X, 54, CONTENT_W, 32, 15, 1);
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count)
        format_disk_line(line, sizeof(line), &disks[selected_disk]);
    else
        line[0] = 0;
    snprintf(target, sizeof(target), "%s %s", T("Target:"), line);
    make_label(target, CONTENT_X, 98, CONTENT_W, 34, 14, 1);
    make_label(installer_partition_auto
                       ? T("The selected disk will be erased and formatted with a FAT32 ESP and ext4 system root.")
                       : setup.format_esp
                           ? T("The selected root and ESP will be formatted. All EFI boot files on that ESP will be erased.")
                           : T("Only the selected root will be formatted. Existing ESP files and other partitions are preserved."),
               CONTENT_X, 142, CONTENT_W, 64, 14, 1);
    make_label(T("Type INSTALL to enable the Install button."),
               CONTENT_X, 224, CONTENT_W, 30, 14, 1);
    confirm_input = new Fl_Input(CONTENT_X, 260, 330, 36);
    confirm_input->value(confirm_text);
    confirm_input->maximum_size((int)sizeof(confirm_text) - 1);
    confirm_input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    confirm_input->callback(confirm_changed);
    reliefos_fltk::style(confirm_input);
    xorg_checkbox = new Fl_Check_Button(CONTENT_X, 318, 375, 32,
                                         T("Install Xorg desktop environment"));
    xorg_checkbox->value(setup.skip_xorg ? 0 : 1);
    xorg_checkbox->callback(xorg_install_changed);
    reliefos_fltk::style(xorg_checkbox);
    if (!swap_size_initialized) {
        if (!setup.swap_size_mib) setup.swap_size_mib=installer_swap_recommend_system();
        snprintf(swap_size_text,sizeof(swap_size_text),"%llu",(unsigned long long)setup.swap_size_mib);
        swap_size_initialized=1;
    }
    Fl_Check_Button *swap=new Fl_Check_Button(CONTENT_X+390,318,220,32,T("Swapfile (MiB)"));
    swap->value(setup.swap_enabled ? 1 : 0);
    swap->callback(swap_install_changed);
    reliefos_fltk::style(swap);
    swap_size_input=new Fl_Input(CONTENT_X+615,318,130,32);
    swap_size_input->value(swap_size_text);
    swap_size_input->maximum_size(20);
    swap_size_input->when(FL_WHEN_CHANGED);
    swap_size_input->callback(swap_size_changed);
    if (!setup.swap_enabled) swap_size_input->deactivate();
    reliefos_fltk::style(swap_size_input);
    snprintf(line, sizeof(line), "%s BIOS: %s    UEFI: %s", T("Bootloaders:"),
             setup.skip_bios ? T("Skip") : T("Install"),
             setup.skip_uefi ? T("Skip") : T("Install"));
    make_label(line, CONTENT_X, 366, CONTENT_W, 36, 14, 1);
    make_label(bootloader_description(), CONTENT_X, 414, CONTENT_W, 100, 13, 1);
}

static void build_update_log(int y, int height)
{
    make_label(T("Update details"), CONTENT_X, y, CONTENT_W, 26, 14, 0);
    if (!update_log_buffer) update_log_buffer = new Fl_Text_Buffer();
    update_log_display = new Fl_Text_Display(CONTENT_X, y + 32, CONTENT_W, height);
    update_log_display->buffer(update_log_buffer);
    update_log_display->textfont(FL_HELVETICA);
    update_log_display->textsize(13);
    update_log_display->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    reliefos_fltk::style(update_log_display);
    update_log_display->insert_position(update_log_buffer->length());
    update_log_display->show_insert_position();
    make_label("/var/log/installer-update.log", CONTENT_X, y + height + 40, CONTENT_W, 22, 12, 0);
}

static void build_progress_page(void)
{
    make_heading(update_job == 1 ? T("Checking system update") : mode_progress_title(), 20);
    make_label(T("Do not turn off this machine."), CONTENT_X, 62,
               CONTENT_W, 30, 15, 1);
    progress_text_box = make_label(progress_text, CONTENT_X, 112,
                                   CONTENT_W, 40, 14, 1);
    progress_bar = new Fl_Progress(CONTENT_X, 164, CONTENT_W - 112, 28);
    progress_bar->minimum(0.0);
    progress_bar->maximum(100.0);
    progress_bar->value(progress_value > 100 ? 100 : progress_value);
    progress_bar->color(fl_rgb_color(218, 222, 232));
    progress_bar->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(progress_bar);
    char percent[16];
    uint32_t value = progress_value > 100 ? 100 : progress_value;
    snprintf(percent, sizeof(percent), "%u%%", (unsigned)value);
    progress_percent_box = make_label(percent, CONTENT_X + CONTENT_W - 98,
                                      162, 72, 30, 14, 0);
    if (install_mode == INSTALLER_MODE_UPDATE) {
        update_activity_box = make_label(update_activity_text, CONTENT_X, 202, CONTENT_W, 82, 13, 1);
        build_update_log(294, 218);
    }
}

static void build_finish_page(void)
{
    char message[192];
    if (install_success) {
        make_heading(install_mode == INSTALLER_MODE_UPDATE
                         ? T("Update Complete") : T("Installation Complete"), 20);
        make_label(install_mode == INSTALLER_MODE_UPDATE
                       ? T("ReliefOS was updated on the selected disk.")
                       : T("ReliefOS was installed to the selected disk."),
                   CONTENT_X, 62, CONTENT_W, 32, 15, 1);
        make_label(T("Remove the installation media, then restart."),
                   CONTENT_X, 108, CONTENT_W, 32, 15, 1);
        if (!setup.skip_uefi) {
            char entry[128];
            int error = install_mode == INSTALLER_MODE_UPDATE
                ? (snprintf(entry, sizeof(entry), "%s", update_info.boot_file), 0)
                : installer_boot_entry_path(entry, sizeof(entry));
            if (!error) {
                make_label(T("UEFI boot file (add it in firmware if no automatic entry appears):"),
                           CONTENT_X, 148, CONTENT_W, 28, 13, 1);
                make_label(entry, CONTENT_X, 180, CONTENT_W, 28, 13, 1);
            }
        }
        if (reboot_error) {
            make_label(T("Restart failed"), CONTENT_X, 212,
                       CONTENT_W, 28, 14, 1);
            snprintf(message, sizeof(message), "%s", strerror(reboot_error));
            make_label(message, CONTENT_X, 242, CONTENT_W, 36, 14, 1);
        }
    } else {
        make_heading(install_mode == INSTALLER_MODE_UPDATE
                         ? T("Update Failed") : T("Installation Failed"), 20);
        make_label(install_mode == INSTALLER_MODE_UPDATE
                       ? T("Review the details, then check the update again to retry.")
                       : T("No writes will continue after this error."),
                   CONTENT_X, 62, CONTENT_W, 32, 15, 1);
        make_label(status_text, CONTENT_X, 108, CONTENT_W, 42, 14, 1);
        make_label(detail_text, CONTENT_X, 154, CONTENT_W,
                   install_mode == INSTALLER_MODE_UPDATE ? 40 : 60, 14, 1);
        if (install_mode == INSTALLER_MODE_UPDATE && *update_activity_text)
            update_activity_box = make_label(update_activity_text, CONTENT_X, 200, CONTENT_W, 86, 13, 1);
    }
    if (install_mode == INSTALLER_MODE_UPDATE) {
        int activity = !install_success && *update_activity_text;
        build_update_log(activity ? 300 : reboot_error ? 290 : 236, activity ? 200 : reboot_error ? 210 : 264);
    }
}

static void clear_page_widgets(void)
{
    disk_browser = NULL;
    update_root_choice = update_esp_choice = NULL;
    format_esp_checkbox = NULL;
    partition_browser = NULL;
    confirm_input = NULL;
    xorg_checkbox = NULL;
    swap_size_input = NULL;
    bootloader_hint = NULL;
    partition_hint = NULL;
    account_hint = NULL;
    status_box = NULL;
    detail_box = NULL;
    progress_text_box = NULL;
    update_activity_box = NULL;
    progress_percent_box = NULL;
    progress_bar = NULL;
    update_log_display = NULL;
    for (int i = 0; i < 5; ++i) account_inputs[i] = NULL;
}

static void show_page(void)
{
    if (!content_group) return;
    content_group->clear();
    clear_page_widgets();
    content_group->begin();
    switch ((enum installer_page)page) {
    case INSTALLER_PAGE_LANGUAGE: build_language_page(); break;
    case INSTALLER_PAGE_LICENSE: build_license_page(); break;
    case INSTALLER_PAGE_WELCOME: build_welcome_page(); break;
    case INSTALLER_PAGE_MODE: build_mode_page(); break;
    case INSTALLER_PAGE_DISK: build_disk_page(); break;
    case INSTALLER_PAGE_BOOTLOADERS: build_bootloader_page(); break;
    case INSTALLER_PAGE_PARTITIONS: build_partition_page(); break;
    case INSTALLER_PAGE_ACCOUNTS: build_accounts_page(); break;
    case INSTALLER_PAGE_CONFIRM: build_confirm_page(); break;
    case INSTALLER_PAGE_PROGRESS: build_progress_page(); break;
    case INSTALLER_PAGE_FINISH: build_finish_page(); break;
    default: break;
    }
    content_group->end();
    update_sidebar();
    update_footer();
    refresh_status_widgets();
    if (main_window) main_window->redraw();
}

static int app_event_handler(int event)
{
    if (Fl::modal()) return 0;
    if (event == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        cancel_setup(NULL, NULL);
        return 1;
    }
    if (event == FL_KEYDOWN &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
        Fl_Widget *focus = Fl::focus();
        int text_field = focus == confirm_input;
        for (int i = 0; i < 5; ++i)
            if (focus == account_inputs[i]) text_field = 1;
        if (!text_field) {
            go_primary();
            return 1;
        }
    }
    return 0;
}

extern "C" void refresh_ui(void)
{
    if (installer_tty_mode || !main_window) return;
    refresh_status_widgets();
    update_footer();
    Fl::check();
    Fl::flush();
}

static int run_graphical(void)
{
    COLOR_BG = fl_rgb_color(236, 238, 244);
    COLOR_TEXT = fl_rgb_color(34, 36, 46);
    COLOR_SIDEBAR = fl_rgb_color(59, 98, 166);
    COLOR_ACTIVE = fl_rgb_color(255, 255, 255);
    reliefos_fltk::init("installer");
    main_window = new Fl_Window(WINDOW_W, WINDOW_H, T("Install ReliefOS"));
    main_window->color(COLOR_BG);
    main_window->callback(window_close);

    Fl_Group *sidebar = new Fl_Group(0, 0, SIDEBAR_W, WINDOW_H);
    sidebar->box(FL_FLAT_BOX);
    sidebar->color(COLOR_SIDEBAR);
    sidebar->begin();
    Fl_Box *brand = new Fl_Box(16, 18, SIDEBAR_W - 30, 30, "ReliefOS");
    brand->box(FL_NO_BOX);
    brand->labelfont(FL_HELVETICA_BOLD);
    brand->labelsize(23);
    brand->labelcolor(FL_WHITE);
    reliefos_fltk::style(brand);
    brand_setup = new Fl_Box(16, 48, SIDEBAR_W - 30, 24, T("Setup"));
    brand_setup->box(FL_NO_BOX);
    brand_setup->labelsize(14);
    brand_setup->labelcolor(FL_WHITE);
    reliefos_fltk::style(brand_setup);
    for (int i = 0; i < INSTALLER_PAGE_COUNT; ++i) {
        sidebar_rows[i] = new Fl_Box(12, 100 + i * 34, SIDEBAR_W - 24, 30, "");
        sidebar_rows[i]->box(FL_FLAT_BOX);
        sidebar_rows[i]->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        sidebar_rows[i]->labelsize(14);
        sidebar_rows[i]->labelcolor(FL_WHITE);
        reliefos_fltk::style(sidebar_rows[i]);
    }
    sidebar->end();

    content_group = new Fl_Group(SIDEBAR_W, 0,
                                 WINDOW_W - SIDEBAR_W, 620);
    content_group->box(FL_FLAT_BOX);
    content_group->color(COLOR_BG);
    content_group->end();

    Fl_Group *footer = new Fl_Group(SIDEBAR_W, 620,
                                    WINDOW_W - SIDEBAR_W, WINDOW_H - 620);
    footer->box(FL_FLAT_BOX);
    footer->color(COLOR_BG);
    footer->begin();
    back_button = make_button(T("Previous Step"), CONTENT_X, 644, 136, 38,
                              go_back, NULL);
    primary_button = new Fl_Return_Button(CONTENT_X + 154, 644, 148, 38,
                                          T("Next"));
    primary_button->callback(go_next);
    primary_button->labelsize(14);
    reliefos_fltk::style(primary_button);
    cancel_button = make_button(T("Cancel"), WINDOW_W - 160, 644, 136, 38,
                                cancel_setup, NULL);
    footer->end();

    main_window->end();
    Fl::add_handler(app_event_handler);
    show_page();
    main_window->show();
    puts("[installer.elf] FLTK installer ready");
    fflush(stdout);
    return Fl::run();
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--update-worker")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return installer_update_worker(argc, argv);
    }
    struct installer_tty_context tty_context;
    int graphical = argc == 2 && strcmp(argv[1], "--graphical") == 0;

    setlocale(LC_ALL, "");
    bindtextdomain("installer", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("installer", "UTF-8");
    textdomain("installer");
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 1 && !graphical) {
        fputs("usage: installer [--graphical]\n", stderr);
        return 2;
    }
    if (!graphical && isatty(STDIN_FILENO)) {
        installer_tty_mode = 1;
        tty_context.setup = &setup;
        tty_context.disks = disks;
        tty_context.disk_count = &disk_count;
        tty_context.partitions = partitions;
        tty_context.partition_count = &partition_count;
        tty_context.selected_partition = &selected_partition;
        tty_context.root_partition = &installer_root_partition;
        tty_context.esp_partition = &installer_esp_partition;
        tty_context.partition_auto = &installer_partition_auto;
        tty_context.selected_disk = &selected_disk;
        tty_context.install_mode = &install_mode;
        tty_context.install_success = &install_success;
        tty_context.page = &page;
        tty_context.refresh_disks = refresh_disks;
        tty_context.format_disk_line = format_disk_line;
        tty_context.refresh_partitions = refresh_partitions;
        tty_context.format_partition_line = format_partition_line;
        tty_context.partition_plan_valid = installer_partition_plan_valid;
        tty_context.partition_auto_layout = installer_partition_auto_layout;
        tty_context.partition_initialize = installer_partition_initialize;
        tty_context.partition_create = installer_partition_create;
        tty_context.partition_resize = installer_partition_resize;
        tty_context.partition_rename = installer_partition_rename;
        tty_context.partition_set_type = installer_partition_set_type;
        tty_context.partition_delete = installer_partition_delete;
        tty_context.partition_format = installer_partition_format;
        tty_context.perform_install = perform_install;
        tty_context.boot_entry_path = installer_boot_entry_path;
        return installer_tty_main(&tty_context);
    }
    puts("[installer.elf] starting installer wizard");
    return run_graphical();
}
