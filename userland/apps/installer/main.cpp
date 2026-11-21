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
#include "installer_tui.h"
#include "installer_tui_partitions.h"
#include "installer_update.h"
#include "installer_job.h"
#include "installer_label_wrap.h"
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
static int CONTENT_W = WINDOW_W - CONTENT_X - 24;
static int CONTENT_H = WINDOW_H - 80;
/* Initialize FLTK colors only after choosing the graphical path.  The TTY
 * frontend must not run toolkit initialization before main selects it. */
static Fl_Color COLOR_BG;
static Fl_Color COLOR_TEXT;
static Fl_Color COLOR_SIDEBAR;
static Fl_Color COLOR_ACTIVE;

static Fl_Window *main_window;
static Fl_Group *content_group;
static Fl_Box *brand_setup;
static Fl_Button *sidebar_rows[INSTALLER_PAGE_COUNT];
static Fl_Button *back_button;
static Fl_Return_Button *primary_button;
static Fl_Button *cancel_button;
static Fl_Hold_Browser *disk_browser;
static Fl_Hold_Browser *partition_browser;
static tui_partition_table partition_table{};
static int partition_view_error;
static Fl_Button *partition_edit_buttons[4];
static Fl_Choice *partition_role_choice;
static Fl_Button *partition_role_button;
static Fl_Choice *update_root_choice, *update_esp_choice;
static Fl_Check_Button *format_esp_checkbox;
static Fl_Input *confirm_input;
static Fl_Choice *desktop_choice;
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
static int license_accepted;
static int primary_pending;
static int back_pending;
static int render_pending;
static int account_input_overflow[5];
static int confirm_input_overflow;
static int review_plan_error;
static ra::Process update_process;
static struct installer_update_info update_info;
static Fl_Text_Buffer *update_log_buffer;
static Fl_Text_Display *update_log_display;
static size_t update_stdout_offset, update_stderr_offset;
static int update_job, update_checked, update_protocol_error;
static char update_message[256];
static struct installer_job install_job;
static char installed_boot_file[128];

/* Embedded verbatim from the repository LICENSE by apache_license.S. */
extern "C" const char installer_license_text[];

/* FLTK 1.3 wraps only at ASCII spaces. Keep its native label drawing while
 * providing display-only line breaks for unspaced CJK and long path names. */
class InstallerWrappedLabel : public Fl_Box {
    static double measure(const char *text, size_t length, void *)
    {
        return fl_width(text, (int)length);
    }

    void draw() override
    {
        draw_box();
        int left = x() + Fl::box_dx(box());
        int width = w() - Fl::box_dw(box());
        /* Match Fl_Widget::draw_label's native horizontal inset. */
        if (width > 11 && (align() & (FL_ALIGN_LEFT | FL_ALIGN_RIGHT))) {
            left += 3;
            width -= 6;
        }
        if (width <= 0) return;
        fl_font(labelfont(), labelsize());
        char *wrapped = installer_label_wrap(label(), width, measure, nullptr);
        Fl_Align alignment = (align() & ~FL_ALIGN_WRAP) | FL_ALIGN_CLIP;
        Fl_Label rendered = {wrapped ? wrapped : label(), image(), deimage(), labelfont(),
            labelsize(), active_r() ? labelcolor() : fl_inactive(labelcolor()),
            alignment, (unsigned char)labeltype()};
        if (!active_r() && rendered.deimage) rendered.image = rendered.deimage;
        rendered.draw(left, y() + Fl::box_dy(box()), width,
            h() - Fl::box_dh(box()), alignment);
        free(wrapped);
    }

public:
    InstallerWrappedLabel(int x, int y, int width, int height, const char *text = nullptr)
        : Fl_Box(x, y, width, height, text) {}
};

static Fl_Box *make_label(const char *text, int x, int y, int w, int h,
                          int size, int wrap)
{
    Fl_Box *box = wrap ? new InstallerWrappedLabel(x, y, w, h) : new Fl_Box(x, y, w, h);
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
    Fl_Box *box = make_label(text, CONTENT_X, y, CONTENT_W, 32, 23, 0);
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
    return installer_model_page_title(target, (enum installer_mode)install_mode);
}

static const char *sidebar_label(enum installer_page target)
{
    switch (target) {
    case INSTALLER_PAGE_LANGUAGE: return T("Language");
    case INSTALLER_PAGE_LICENSE: return T("License");
    case INSTALLER_PAGE_MODE: return T("Mode");
    case INSTALLER_PAGE_DISK: return T("Disk");
    case INSTALLER_PAGE_BOOTLOADERS: return T("Bootloaders");
    case INSTALLER_PAGE_PARTITIONS: return T("Partitions");
    case INSTALLER_PAGE_SYSTEM: return T("System");
    case INSTALLER_PAGE_ACCOUNTS: return T("Accounts");
    case INSTALLER_PAGE_CONFIRM: return T("Confirm");
    case INSTALLER_PAGE_PROGRESS:
        return install_mode == INSTALLER_MODE_UPDATE ? T("Update") : T("Install");
    case INSTALLER_PAGE_FINISH: return T("Finish");
    default: return page_label(target);
    }
}

static const char *primary_label(void)
{
    switch (installer_model_action((enum installer_page)page,
                                   (enum installer_mode)install_mode,
                                   install_success)) {
    case INSTALLER_ACTION_INSTALL: return T("Install");
    case INSTALLER_ACTION_UPDATE: return T("Update");
    case INSTALLER_ACTION_RETRY:
        return install_mode == INSTALLER_MODE_UPDATE ? T("Check Again") : T("Review settings");
    case INSTALLER_ACTION_RESTART: return T("Restart");
    case INSTALLER_ACTION_CLOSE: return T("Close");
    case INSTALLER_ACTION_NEXT:
        return page == INSTALLER_PAGE_DISK && install_mode == INSTALLER_MODE_UPDATE
            ? (update_checked ? T("Review update") : T("Check Update")) : T("Next");
    }
    return T("Next");
}

static int primary_disabled(void)
{
    if (ui_busy || install_running || page == INSTALLER_PAGE_PROGRESS) return 1;
    if (page == INSTALLER_PAGE_LICENSE) return !license_accepted;
    if (page == INSTALLER_PAGE_SYSTEM)
        return setup.swap_enabled && swap_size_invalid;
    if (page == INSTALLER_PAGE_ACCOUNTS) {
        for (int i = 0; i < 5; ++i)
            if (account_input_overflow[i]) return 1;
        return !installer_setup_valid(&setup);
    }
    if (page == INSTALLER_PAGE_DISK)
        return selected_disk < 0 || (uint32_t)selected_disk >= disk_count ||
            (install_mode == INSTALLER_MODE_FRESH && installer_partition_disk_error());
    if (page == INSTALLER_PAGE_PARTITIONS)
        return partition_view_error || !installer_partition_plan_valid();
    if (page == INSTALLER_PAGE_CONFIRM && install_mode == INSTALLER_MODE_UPDATE)
        return !update_checked;
    if (page == INSTALLER_PAGE_CONFIRM)
        return review_plan_error || !installer_partition_plan_valid() || !installer_setup_valid(&setup) ||
               confirm_input_overflow || !confirmation_ok() ||
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
        Fl_Button *row = sidebar_rows[i];
        if (i < count) {
            int active = steps[i] == (enum installer_page)page;
            row->copy_label(sidebar_label(steps[i]));
            row->labelsize(15);
            row->user_data((void *)(uintptr_t)steps[i]);
            row->color(active ? COLOR_ACTIVE : COLOR_SIDEBAR);
            row->labelcolor(active ? COLOR_TEXT : FL_WHITE);
            // Output widgets retain their normal colors while skipping input.
            if (!ui_busy && installer_model_can_visit((enum installer_page)page,
                steps[i], (enum installer_mode)install_mode, install_success)) row->clear_output();
            else row->set_output();
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

static int run_install_worker(void *, struct installer_job_packet *packet)
{
    int result = perform_install();
    packet->progress = progress_value;
    packet->success = install_success;
    packet->selected = selected_partition;
    packet->root = installer_root_partition;
    packet->esp = installer_esp_partition;
    packet->automatic = installer_partition_auto;
    packet->replaced = installer_partition_table_replaced;
    packet->format_esp = setup.format_esp;
    copy_text(packet->message, sizeof(packet->message), status_text);
    copy_text(packet->detail, sizeof(packet->detail), detail_text);
    if (!result) installer_boot_entry_path(packet->info.boot_file, sizeof(packet->info.boot_file));
    return result;
}

static void install_pump(void *)
{
    int result = installer_job_step(&install_job);
    const struct installer_job_packet *packet = result == 0 ? &install_job.final : &install_job.latest;
    progress_value = packet->progress;
    copy_text(status_text, sizeof(status_text), packet->message);
    copy_text(detail_text, sizeof(detail_text), packet->detail);
    snprintf(progress_text, sizeof(progress_text), "%s - %.120s", status_text, detail_text);
    refresh_status_widgets();
    if (result > 0) { Fl::repeat_timeout(0.08, install_pump); return; }
    ui_busy = install_running = 0;
    install_success = result == 0 && packet->result == 0 && packet->success;
    if (result < 0) set_error_status(T("Installation worker failed"), result);
    if (result == 0) {
        selected_partition = packet->selected;
        installer_root_partition = packet->root;
        installer_esp_partition = packet->esp;
        installer_partition_auto = packet->automatic;
        installer_partition_table_replaced = packet->replaced;
        setup.format_esp = packet->format_esp;
        copy_text(installed_boot_file, sizeof(installed_boot_file), packet->info.boot_file);
    }
    uint8_t previous_layout = installer_partition_auto;
    installer_partition_reset();
    installer_partition_auto = previous_layout;
    if (install_success) {
        explicit_bzero(setup.password, sizeof(setup.password));
        explicit_bzero(setup.password_confirm, sizeof(setup.password_confirm));
        explicit_bzero(setup.root_password, sizeof(setup.root_password));
        explicit_bzero(setup.root_password_confirm, sizeof(setup.root_password_confirm));
    }
    page = INSTALLER_PAGE_FINISH;
    show_page();
}

static void start_install_job(void)
{
    int error = installer_job_start(&install_job, run_install_worker, NULL, "/var/log/installer.log");
    if (error) {
        install_success = 0;
        page = INSTALLER_PAGE_FINISH;
        set_error_status(T("Cannot start the installation worker"), error);
        show_page();
        return;
    }
    ui_busy = install_running = 1;
    install_success = 0;
    progress_value = 0;
    installed_boot_file[0] = 0;
    page = INSTALLER_PAGE_PROGRESS;
    copy_text(progress_text, sizeof(progress_text), T("Preparing operation"));
    show_page();
    Fl::add_timeout(0.0, install_pump);
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
            fprintf(stderr, "[installer.elf] inspection complete\n");
            setup.skip_bios = !(update_info.boot_modes & 1u);
            setup.skip_uefi = !(update_info.boot_modes & 2u);
            page = INSTALLER_PAGE_DISK;
            set_status(T("Ready to update"), T("Review the detected system and boot options, then continue to the summary."));
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
        if (error) {
            set_error_status(T("Cannot identify the selected partitions"), error);
            schedule_render();
            return;
        }
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
    explicit_bzero(setup.password, sizeof(setup.password));
    explicit_bzero(setup.password_confirm, sizeof(setup.password_confirm));
    explicit_bzero(setup.root_password, sizeof(setup.root_password));
    explicit_bzero(setup.root_password_confirm, sizeof(setup.root_password_confirm));
    exit(0);
}

static void cancel_setup(Fl_Widget *, void *)
{
    if (ui_busy || !installer_model_can_cancel((enum installer_page)page, install_success)) return;
    if (page != INSTALLER_PAGE_FINISH &&
        fl_choice("%s", T("Keep configuring"), T("Exit installer"), NULL,
            T("Exit the installer? Unapplied disk changes and setup choices will be discarded.")) != 1) return;
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
    reset_confirm();
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
        start_install_job();
        return;
    case INSTALLER_ACTION_UPDATE:
        start_update_job(1);
        return;
    case INSTALLER_ACTION_RETRY:
        if (install_mode == INSTALLER_MODE_UPDATE) start_update_job(0);
        else {
            reset_confirm();
            page = INSTALLER_PAGE_DISK;
            show_page();
        }
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

    if (page == INSTALLER_PAGE_DISK && install_mode == INSTALLER_MODE_UPDATE && !update_checked) {
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
               CONTENT_X, 62, CONTENT_W, 44, 15, 1);
    for (unsigned i = 0; i < sizeof(language_options) / sizeof(language_options[0]); ++i) {
        Fl_Round_Button *option = new Fl_Round_Button(CONTENT_X + 20, 126 + (int)i * 48,
                                                     360, 34, language_options[i].name);
        option->type(FL_RADIO_BUTTON);
        option->value(language_selection() == i);
        option->callback(language_selected, (void *)(uintptr_t)i);
        reliefos_fltk::style(option);
    }
}

static void license_changed(Fl_Widget *widget, void *)
{
    license_accepted = static_cast<Fl_Check_Button *>(widget)->value();
    update_footer();
}

static void build_license_page(void)
{
    make_heading(T("License agreement"), 20);
    make_label("Apache License 2.0", CONTENT_X, 60, CONTENT_W, 28, 15, 0);
    static Fl_Text_Buffer license_buffer;
    if (!license_buffer.length()) license_buffer.text(installer_license_text);
    Fl_Text_Display *text = new Fl_Text_Display(CONTENT_X, 104, CONTENT_W, CONTENT_H - 168);
    text->buffer(license_buffer);
    text->textfont(FL_COURIER);
    text->textsize(13);
    text->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    reliefos_fltk::style(text);
    auto *accept = new Fl_Check_Button(CONTENT_X, CONTENT_H - 48, CONTENT_W, 34,
        T("I accept the license terms"));
    accept->value(license_accepted);
    accept->callback(license_changed);
    reliefos_fltk::style(accept);
}

/** @brief Start a fresh mode with both firmware defaults after leaving update mode. */
static void mode_selected(Fl_Widget *widget, void *data)
{
    if (!static_cast<Fl_Round_Button *>(widget)->value()) return;
    if ((uint8_t)(uintptr_t)data == INSTALLER_MODE_FRESH && install_mode != INSTALLER_MODE_FRESH)
        setup.skip_bios = setup.skip_uefi = 0;
    if (install_mode != (uint8_t)(uintptr_t)data) {
        installer_partition_select_mode((uint8_t)(uintptr_t)data);
        partition_table = tui_partition_table{};
        partition_view_error = 0;
        update_checked = 0;
        reset_confirm();
    }
    schedule_render();
}

static void build_mode_page(void)
{
    make_heading(T("Installation type"), 20);
    make_label(T("Choose a new installation or update an existing ReliefOS system."),
               CONTENT_X, 62, CONTENT_W, 44, 15, 1);
    Fl_Round_Button *fresh = new Fl_Round_Button(CONTENT_X + 20, 124, CONTENT_W - 20, 36,
                                                  T("Fresh Install"));
    Fl_Round_Button *update = new Fl_Round_Button(CONTENT_X + 20, 242, CONTENT_W - 20, 36,
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
               CONTENT_X + 20, 164, CONTENT_W - 20, 56, 15, 1);
    make_label(T("Update preserves users, personal files, installed software and local configuration."),
               CONTENT_X + 20, 282, CONTENT_W - 20, 68, 15, 1);
}

static void update_partition_selected(Fl_Widget *widget, void *)
{
    auto *choice = static_cast<Fl_Choice *>(widget);
    int index = choice->mvalue() ? int((intptr_t)choice->mvalue()->user_data()) - 1 : -1;
    if (widget == update_root_choice) installer_root_partition = index;
    else installer_esp_partition = index;
    update_checked = 0;
    schedule_render();
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
    if (!disk_browser || disk_browser->value() <= 0) return;
    int target = disk_browser->value() - 1;
    if (target == selected_disk) return;
    partition_table = tui_partition_table{};
    partition_view_error = 0;
    int result = installer_partition_select_disk(target);
    if (result < 0 && install_mode == INSTALLER_MODE_FRESH)
        set_error_status(T("Cannot read the selected disk"), result);
    update_checked = 0;
    reset_confirm();
    refresh_update_partitions();
    schedule_render();
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
    update_checked = 0;
    reset_confirm();
    ui_busy = 1;
    refresh_disks();
    refresh_disk_list();
    ui_busy = 0;
    schedule_render();
}

static void bootloader_install_changed(Fl_Widget *, void *);

static void build_disk_page(void)
{
    make_heading(install_mode == INSTALLER_MODE_UPDATE
        ? T("Select Disk to Update") : T("Select Installation Disk"), 20);
    make_label(install_mode == INSTALLER_MODE_UPDATE
        ? T("Select a disk and check its existing installation before continuing.")
        : T("Select the target. The disk layout is configured in the following steps."),
        CONTENT_X, 62, CONTENT_W, 48, 14, 1);
    make_button(T("Refresh"), CONTENT_X + CONTENT_W - 130, 112, 130, 30, refresh_pressed, nullptr);
    disk_browser = new Fl_Hold_Browser(CONTENT_X, 154, CONTENT_W,
        CONTENT_H - (install_mode == INSTALLER_MODE_UPDATE ? 370 : 248));
    disk_browser->callback(select_disk);
    disk_browser->textsize(14);
    disk_browser->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(disk_browser);
    if (install_mode == INSTALLER_MODE_UPDATE) {
        const int column = (CONTENT_W - 16) / 2;
        make_label(T("ReliefOS system partition"), CONTENT_X, CONTENT_H - 208, column, 24, 14, 0);
        update_root_choice = new Fl_Choice(CONTENT_X, CONTENT_H - 180, column, 32);
        make_label(T("EFI system partition"), CONTENT_X + column + 16, CONTENT_H - 208, column, 24, 14, 0);
        update_esp_choice = new Fl_Choice(CONTENT_X + column + 16, CONTENT_H - 180, column, 32);
        update_root_choice->callback(update_partition_selected);
        update_esp_choice->callback(update_partition_selected);
        reliefos_fltk::style(update_root_choice); reliefos_fltk::style(update_esp_choice);
        if (update_checked) {
            char detected[512];
            snprintf(detected, sizeof(detected), "%s: %s → %s\n%s: %s", T("Version"),
                update_info.installed_version, update_info.media_version, T("System partition"), update_info.root_device);
            make_label(detected, CONTENT_X, CONTENT_H - 144, CONTENT_W, 48, 13, 1);
            make_label(T("Update bootloader software"), CONTENT_X, CONTENT_H - 92, CONTENT_W - 200, 30, 14, 1);
            auto *bios = new Fl_Check_Button(CONTENT_X + CONTENT_W - 188, CONTENT_H - 92, 86, 30, "BIOS");
            bios->value(!setup.skip_bios);
            bios->callback(bootloader_install_changed, (void *)(uintptr_t)1);
            auto *uefi = new Fl_Check_Button(CONTENT_X + CONTENT_W - 94, CONTENT_H - 92, 86, 30, "UEFI");
            uefi->value(!setup.skip_uefi);
            uefi->callback(bootloader_install_changed, (void *)(uintptr_t)2);
            reliefos_fltk::style(bios); reliefos_fltk::style(uefi);
        }
    }
    status_box = make_label(status_text, CONTENT_X, CONTENT_H - 60, CONTENT_W, 24, 14, 1);
    detail_box = make_label(detail_text, CONTENT_X, CONTENT_H - 34, CONTENT_W, 30, 13, 1);
    refresh_disk_list();
}

static void refresh_partition_actions(void)
{
    bool has_selection = !partition_view_error && !installer_partition_auto && selected_partition >= 0 && (uint32_t)selected_partition < partition_count;
    for (int i = 0; i < 4; ++i) {
        if (partition_edit_buttons[i]) {
            if (has_selection) partition_edit_buttons[i]->activate();
            else partition_edit_buttons[i]->deactivate();
        }
    }
    if (partition_role_choice) {
        if (has_selection) {
            uint32_t type = partitions[selected_partition].gpt_type;
            partition_role_choice->value(type == RELIEFOS_BLOCK_GPT_ESP ? 0 :
                type == RELIEFOS_BLOCK_GPT_LINUX ? 1 :
                type == RELIEFOS_BLOCK_GPT_BIOS_BOOT ? 3 : 2);
            partition_role_choice->activate();
        } else {
            partition_role_choice->value(-1);
            partition_role_choice->deactivate();
        }
    }
    if (partition_role_button) {
        if (has_selection) partition_role_button->activate();
        else partition_role_button->deactivate();
    }
    if (format_esp_checkbox) {
        if (installer_esp_partition >= 0) format_esp_checkbox->activate();
        else format_esp_checkbox->deactivate();
    }
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
                ? T("Automatic plan: ESP 128 MiB FAT32; root %llu MiB ext4.\nThis replaces all existing partitions only after confirmation.")
                : T("Automatic plan: BIOS Boot 1 MiB; ESP 128 MiB FAT32; root %llu MiB ext4.\nThis replaces all existing partitions only after confirmation."),
                (unsigned long long)(plan.root.sector_count / 2048u));
            partition_hint->copy_label(line);
        } else {
            partition_hint->copy_label(installer_partition_auto
                ? T("The automatic layout does not fit this disk. Select another disk or configure partitions manually.")
                : setup.skip_bios
                    ? T("Manual layout: assign a FAT32 ESP and an ext4 or ext3 root. BIOS Boot is not required for the selected bootloaders.")
                    : T("Manual layout: assign BIOS Boot, ESP and root roles. BIOS Boot stays unformatted."));
        }
    }
    partition_browser->clear();
    installer_tty_context context{};
    context.setup = &setup;
    context.disks = disks;
    context.disk_count = &disk_count;
    context.partitions = partitions;
    context.partition_count = &partition_count;
    context.selected_disk = &selected_disk;
    context.root_partition = &installer_root_partition;
    context.esp_partition = &installer_esp_partition;
    auto &table = partition_table;
    int selected = selected_partition;
    int error = tui_partition_table_update(&table, &context, installer_partition_auto, &selected);
    table.selection = nullptr;
    partition_view_error = error;
    if (!installer_partition_auto) selected_partition = selected;
    if (error) {
        selected_partition = -1;
        set_error_status(T("Cannot read the partition layout"), error);
    } else for (int i = 0; i < table.count; ++i) {
        const auto &row = table.rows[i];
        if (row.source >= 0) format_partition_line(line, sizeof(line), &partitions[row.source]);
        else if (installer_partition_auto) {
            char capacity[32];
            tui_partition_size(capacity, sizeof(capacity), row.sector_count, table.sector_size);
            snprintf(line, sizeof(line), "%s  %s  %s", row.name, capacity,
                row.type == RELIEFOS_BLOCK_GPT_BIOS_BOOT ? T("Unformatted") : reliefos_block_filesystem_name(row.filesystem));
        } else {
            char capacity[32];
            tui_partition_size(capacity, sizeof(capacity), row.sector_count, table.sector_size);
            snprintf(line, sizeof(line), "%s  %s", T("Unallocated"), capacity);
        }
        partition_browser->add(line, (void *)(intptr_t)(row.source + 1));
        if (!installer_partition_auto && i == table.cursor) partition_browser->value(i + 1);
    }
    refresh_partition_actions();
    refresh_status_widgets();
    update_footer();
}

static void select_partition(Fl_Widget *, void *)
{
    if (!installer_partition_auto && partition_browser && partition_browser->value() > 0) {
        partition_table.cursor = partition_browser->value() - 1;
        selected_partition = (int)(intptr_t)partition_browser->data(partition_browser->value()) - 1;
    }
    refresh_partition_actions();
    update_footer();
}

static void partition_result(int ret, const char *success)
{
    char detail[128];
    if (ret < 0) {
        snprintf(detail, sizeof(detail), "%s (%d)", strerror(-ret), -ret);
        set_status(T("Cannot update the partition plan"), detail);
    } else {
        set_status(T("Partition plan updated"), success);
    }
    refresh_partitions();
    refresh_partition_list();
}

/* Partition edits are drafts. This dialog collects typed fields instead of a
 * command string; the shared planner validates them before changing the draft. */
static void partition_prompt_pressed(Fl_Widget *, void *data)
{
    auto action = (enum partition_prompt_action)(uintptr_t)data;
    if (ui_busy) return;
    if (action == PARTITION_PROMPT_INITIALIZE) {
        if (fl_choice("%s", T("Cancel"), T("Plan new table"), NULL,
            T("Replace this disk's partition table in the installation plan? All existing partitions will be removed when you install.")) == 1)
            partition_result(installer_partition_initialize(), T("New partition table added to the plan."));
        return;
    }
    if (action == PARTITION_PROMPT_DELETE) {
        if (selected_partition < 0 || (uint32_t)selected_partition >= partition_count) return;
        if (fl_choice("%s", T("Cancel"), T("Plan deletion"), NULL,
            T("Remove the selected partition when the installation is confirmed?")) == 1)
            partition_result(installer_partition_delete(), T("Partition deletion added to the plan."));
        return;
    }
    const char *title = action == PARTITION_PROMPT_CREATE ? T("Create partition") :
        action == PARTITION_PROMPT_RESIZE ? T("Resize partition") :
        action == PARTITION_PROMPT_RENAME ? T("Rename partition") : T("Format partition");
    if (action != PARTITION_PROMPT_CREATE &&
        (selected_partition < 0 || (uint32_t)selected_partition >= partition_count)) return;
    Fl_Window dialog(510, 330, title);
    dialog.set_modal();
    auto *hint = new InstallerWrappedLabel(20, 16, 470, 48,
        action == PARTITION_PROMPT_RESIZE ? T("Resizing changes the partition boundary. Existing filesystems cannot be shrunk here.") :
        T("This change is added to the plan. Nothing is written until installation is confirmed."));
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    bool size_needed = action == PARTITION_PROMPT_CREATE || action == PARTITION_PROMPT_RESIZE;
    bool fs_needed = action == PARTITION_PROMPT_CREATE || action == PARTITION_PROMPT_FORMAT;
    Fl_Input *size = nullptr, *name = nullptr;
    Fl_Choice *filesystem = nullptr;
    int y = 84;
    if (size_needed) {
        auto *label = new Fl_Box(20, y, 150, 32, T("Size (MiB)"));
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        size = new Fl_Input(184, y, 300, 32);
        size->value("1024");
        size->maximum_size(10);
        if (action == PARTITION_PROMPT_RESIZE) {
            char initial[32];
            uint64_t bytes = partitions[selected_partition].sector_count * (uint64_t)disks[selected_disk].sector_size;
            snprintf(initial, sizeof(initial), "%llu", (unsigned long long)(bytes / (1024u * 1024u)));
            size->value(initial);
        }
        y += 52;
    }
    if (fs_needed) {
        auto *label = new Fl_Box(20, y, 150, 32, T("Filesystem"));
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        filesystem = new Fl_Choice(184, y, 300, 32);
        filesystem->add("ext4|ext3|ext2|FAT32|exFAT");
        if (action == PARTITION_PROMPT_CREATE) filesystem->add(T("Unformatted"));
        filesystem->value(0);
        y += 52;
    }
    if (action != PARTITION_PROMPT_RESIZE) {
        auto *label = new Fl_Box(20, y, 150, 32, action == PARTITION_PROMPT_FORMAT ? T("Volume label") : T("Partition name"));
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        name = new Fl_Input(184, y, 300, 32);
        name->maximum_size(RELIEFOS_BLOCK_NAME_LEN - 1);
        name->value(action == PARTITION_PROMPT_RENAME ? partitions[selected_partition].name : "ReliefOS");
    }
    int accepted = 0;
    auto *cancel = new Fl_Button(214, 278, 126, 34, T("Cancel"));
    cancel->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    auto *apply = new Fl_Return_Button(354, 278, 130, 34, T("Add to plan"));
    apply->callback([](Fl_Widget *w, void *value) { *static_cast<int *>(value) = 1; w->window()->hide(); }, &accepted);
    dialog.callback([](Fl_Widget *w, void *) { w->hide(); });
    dialog.end();
    reliefos_fltk::style(&dialog);
    dialog.show();
    while (dialog.shown()) Fl::wait();
    if (!accepted) return;
    uint32_t number = 0;
    if (size) {
        char *end = nullptr;
        errno = 0;
        unsigned long parsed = strtoul(size->value(), &end, 10);
        if (errno || end == size->value() || *end || !parsed || parsed > UINT32_MAX || size->value()[0] == '-') {
            partition_result(-EINVAL, "");
            return;
        }
        number = (uint32_t)parsed;
    }
    static const uint32_t filesystems[] = {
        RELIEFOS_BLOCK_FILESYSTEM_EXT4, RELIEFOS_BLOCK_FILESYSTEM_EXT3,
        RELIEFOS_BLOCK_FILESYSTEM_EXT2, RELIEFOS_BLOCK_FILESYSTEM_FAT32,
        RELIEFOS_BLOCK_FILESYSTEM_EXFAT, RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN
    };
    uint32_t fs = filesystem ? filesystems[filesystem->value()] : 0;
    int result = action == PARTITION_PROMPT_CREATE ? installer_partition_create(fs, number, name->value()) :
        action == PARTITION_PROMPT_RESIZE ? installer_partition_resize(number) :
        action == PARTITION_PROMPT_RENAME ? installer_partition_rename(name->value()) :
        installer_partition_format(fs, name->value());
    partition_result(result, T("Change added to the installation plan."));
}

static void partition_method_changed(Fl_Widget *widget, void *data)
{
    if (!static_cast<Fl_Round_Button *>(widget)->value()) return;
    installer_partition_auto = (uintptr_t)data != 0;
    reset_confirm();
    schedule_render();
}

static void partition_discard_pressed(Fl_Widget *, void *)
{
    if (fl_choice("%s", T("Keep changes"), T("Discard changes"), NULL,
        T("Discard pending partition edits and reload the disk?")) != 1) return;
    uint8_t previous_layout = installer_partition_auto;
    installer_partition_reset();
    installer_partition_auto = previous_layout;
    schedule_render();
}

static void partition_role_pressed(Fl_Widget *, void *)
{
    static const uint32_t types[] = {RELIEFOS_BLOCK_GPT_ESP, RELIEFOS_BLOCK_GPT_LINUX,
        RELIEFOS_BLOCK_GPT_BASIC_DATA, RELIEFOS_BLOCK_GPT_BIOS_BOOT};
    int ret;
    if (ui_busy) return;
    if (!partition_role_choice || partition_role_choice->value() < 0 ||
        partition_role_choice->value() >= 4 || selected_partition < 0 ||
        (uint32_t)selected_partition >= partition_count) {
        partition_result(-EINVAL, "");
        return;
    }
    ret = installer_partition_set_type(types[partition_role_choice->value()]);
    partition_result(ret, T("Partition role updated."));
}

static void format_esp_changed(Fl_Widget *widget, void *)
{
    setup.format_esp = static_cast<Fl_Check_Button *>(widget)->value() != 0;
    update_footer();
}

static void build_partition_page(void)
{
    make_heading(T("Disk layout"), 8);
    auto *automatic = new Fl_Round_Button(CONTENT_X, 50, CONTENT_W, 28, T("Erase disk and install automatically"));
    auto *manual = new Fl_Round_Button(CONTENT_X, 82, CONTENT_W, 28, T("Manual partitioning / keep other systems"));
    automatic->type(FL_RADIO_BUTTON); manual->type(FL_RADIO_BUTTON);
    automatic->value(installer_partition_auto); manual->value(!installer_partition_auto);
    automatic->callback(partition_method_changed, (void *)(uintptr_t)1);
    manual->callback(partition_method_changed, nullptr);
    reliefos_fltk::style(automatic); reliefos_fltk::style(manual);
    partition_hint = make_label("", CONTENT_X, 116, CONTENT_W, 44, 13, 1);
    partition_browser = new Fl_Hold_Browser(CONTENT_X, 164, CONTENT_W,
        CONTENT_H - (installer_partition_auto ? 264 : 376));
    partition_browser->callback(select_partition);
    partition_browser->textsize(13);
    partition_browser->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(partition_browser);
    if (!installer_partition_auto) {
        const char *labels[] = {T("Create"), T("Resize"), T("Rename"), T("Format"), T("Delete"), T("New GPT")};
        const partition_prompt_action actions[] = {PARTITION_PROMPT_CREATE, PARTITION_PROMPT_RESIZE,
            PARTITION_PROMPT_RENAME, PARTITION_PROMPT_FORMAT, PARTITION_PROMPT_DELETE, PARTITION_PROMPT_INITIALIZE};
        const int button_width = (CONTENT_W - 40) / 6;
        for (int i = 0; i < 6; ++i) {
            auto *button = make_button(labels[i], CONTENT_X + i * (button_width + 8), CONTENT_H - 204, button_width, 28,
                partition_prompt_pressed, (void *)(uintptr_t)actions[i]);
            button->labelsize(button_width < 96 ? 12 : 13);
            if (i >= 1 && i <= 4) partition_edit_buttons[i - 1] = button;
        }
        const char *roles[] = {T("Use as ESP"), T("Use as root"), T("Data partition"), T("BIOS Boot")};
        const int role_label_width = CONTENT_W / 4;
        make_label(T("Partition role"), CONTENT_X, CONTENT_H - 168, role_label_width, 30,
            role_label_width < 150 ? 12 : 14, 1);
        partition_role_choice = new Fl_Choice(CONTENT_X + role_label_width + 12, CONTENT_H - 168,
            CONTENT_W - role_label_width - 112, 30);
        for (const char *role : roles) partition_role_choice->add(role);
        reliefos_fltk::style(partition_role_choice);
        partition_role_button = make_button(T("Use"), CONTENT_X + CONTENT_W - 88, CONTENT_H - 168, 88, 30,
            partition_role_pressed, nullptr);
        format_esp_checkbox = new Fl_Check_Button(CONTENT_X, CONTENT_H - 132, CONTENT_W, 36,
            T("Format selected ESP (erase all EFI boot files on it)"));
        format_esp_checkbox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        format_esp_checkbox->value(setup.format_esp);
        format_esp_checkbox->callback(format_esp_changed);
        reliefos_fltk::style(format_esp_checkbox);
        make_button(T("Discard edits"), CONTENT_X, CONTENT_H - 90, CONTENT_W / 3, 28,
            partition_discard_pressed, nullptr);
    }
    status_box = make_label(status_text, CONTENT_X, CONTENT_H - 56, CONTENT_W, 20, 13, 1);
    detail_box = make_label(detail_text, CONTENT_X, CONTENT_H - 34, CONTENT_W, 30, 13, 1);
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
        if (widget == account_inputs[i]) {
            account_input_overflow[i] =
                !reliefos_fltk::copy_input(destination, capacity, value);
            if (account_input_overflow[i]) explicit_bzero(destination, capacity);
        }
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
    make_heading(T("Accounts"), 20);
    const int column_width = (CONTENT_W - 24) / 2;
    make_label(T("User"), CONTENT_X, 164, column_width, 26, 15, 0)->labelfont(FL_HELVETICA_BOLD);
    make_label(T("Administrator: root"), CONTENT_X + column_width + 24, 164,
        column_width, 26, 15, 0)->labelfont(FL_HELVETICA_BOLD);
    for (int i = 0; i < 5; ++i) {
        const int x = CONTENT_X + (i >= 3 ? column_width + 24 : 0);
        const int y = i == 0 ? 72 : (i == 1 || i == 3) ? 192 : 272;
        const int width = i == 0 ? CONTENT_W : column_width;
        make_label(T(labels[i]), x, y, width, 24, 14, 0);
        account_inputs[i] = new Fl_Input(x, y + 28, width, 34);
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
        T("Check username and matching passwords (1-32 characters, no spaces)."),
        CONTENT_X, 358, CONTENT_W, 48, 13, 1);
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

static void desktop_install_changed(Fl_Widget *widget, void *)
{
    setup.desktop = static_cast<Fl_Choice *>(widget)->value();
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

static const char *desktop_label(void)
{
    return setup.desktop == INSTALLER_DESKTOP_XFCE ? T("XFCE (experimental)")
        : setup.desktop == INSTALLER_DESKTOP_CONSOLE ? T("Console only") : T("IceWM (default)");
}

static void build_system_page(void)
{
    make_heading(T("System options"), 20);
    make_label(T("Choose the desktop and additional disk space for virtual memory."),
        CONTENT_X, 62, CONTENT_W, 42, 15, 1);
    make_label(T("Desktop environment"), CONTENT_X, 120, CONTENT_W, 24, 14, 0);
    desktop_choice = new Fl_Choice(CONTENT_X, 152, CONTENT_W, 34);
    desktop_choice->add(T("IceWM (default)"));
    desktop_choice->add(T("XFCE (experimental)"));
    desktop_choice->add(T("Console only"));
    desktop_choice->value(setup.desktop);
    desktop_choice->callback(desktop_install_changed);
    reliefos_fltk::style(desktop_choice);
    make_label(T("Console only installs without a graphical login. XFCE is an experimental desktop option."),
        CONTENT_X, 198, CONTENT_W, 48, 14, 1);
    if (!swap_size_initialized) {
        if (!setup.swap_size_mib) setup.swap_size_mib = installer_swap_recommend_system();
        snprintf(swap_size_text, sizeof(swap_size_text), "%llu", (unsigned long long)setup.swap_size_mib);
        swap_size_initialized = 1;
    }
    auto *swap = new Fl_Check_Button(CONTENT_X, 268, CONTENT_W, 32, T("Create a swapfile"));
    swap->value(setup.swap_enabled);
    swap->callback(swap_install_changed);
    reliefos_fltk::style(swap);
    make_label(T("Swap size (MiB)"), CONTENT_X, 318, CONTENT_W, 24, 14, 0);
    swap_size_input = new Fl_Input(CONTENT_X, 350, CONTENT_W / 2, 34);
    swap_size_input->value(swap_size_text);
    swap_size_input->maximum_size(20);
    swap_size_input->when(FL_WHEN_CHANGED);
    swap_size_input->callback(swap_size_changed);
    if (!setup.swap_enabled) swap_size_input->deactivate();
    reliefos_fltk::style(swap_size_input);
    make_label(T("Swap uses space on the system partition and currently requires an AHCI disk. Enter a valid whole number of MiB."),
        CONTENT_X, 398, CONTENT_W, 54, 14, 1);
}

static void summary_row(const char *label, const char *value, int y, int height = 40)
{
    const int label_width = CONTENT_W / 3;
    make_label(label, CONTENT_X, y, label_width, height, 14, 1)->labelfont(FL_HELVETICA_BOLD);
    make_label(value, CONTENT_X + label_width + 12, y, CONTENT_W - label_width - 12,
        height, 14, 1);
}

static void build_confirm_page(void)
{
    make_heading(install_mode == INSTALLER_MODE_UPDATE ? T("Review system update") : T("Review installation"), 8);
    make_label(T("Check the target and planned changes. Use Back or the sidebar to change your choices."),
        CONTENT_X, 46, CONTENT_W, 44, 14, 1);
    char text[512];
    if (install_mode == INSTALLER_MODE_UPDATE) {
        summary_row(T("Target disk"), update_info.disk, 96, 30);
        summary_row(T("System partition"), update_info.root_device, 130, 30);
        summary_row(T("Boot partition"), update_info.esp_device, 164, 30);
        snprintf(text, sizeof(text), "%s  →  %s", update_info.installed_version, update_info.media_version);
        summary_row(T("Version"), text, 198, 52);
        snprintf(text, sizeof(text), "BIOS: %s    UEFI: %s", setup.skip_bios ? T("Skip") : T("Install"),
            setup.skip_uefi ? T("Skip") : T("Install"));
        summary_row(T("Bootloaders"), text, 254, 30);
        snprintf(text, sizeof(text), "%llu MiB / %llu MiB",
            (unsigned long long)(update_info.free_bytes / (1024u * 1024u)),
            (unsigned long long)((update_info.required_bytes + 1024u * 1024u - 1) / (1024u * 1024u)));
        summary_row(T("Free / required space"), text, 288, 30);
        make_label(T("Accounts, personal files, installed software and local configuration are preserved."),
            CONTENT_X, 328, CONTENT_W, 48, 14, 1);
        make_label(update_info.pending[0]
            ? T("A previous update is incomplete. This attempt will verify the system and recover the pending update stages.")
            : T("Signed packages are checked before changes. Boot files are published after the package transaction succeeds."),
            CONTENT_X, 384, CONTENT_W, 68, 14, 1);
        return;
    }
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count) {
        snprintf(text, sizeof(text), "%s\n", disks[selected_disk].path);
        size_t used = strlen(text);
        format_disk_line(text + used, sizeof(text) - used, &disks[selected_disk]);
    } else text[0] = 0;
    summary_row(T("Target disk"), text, 96, 48);
    snprintf(text, sizeof(text), "%s    %s: %s", desktop_label(), T("User"), setup.username);
    summary_row(T("System"), text, 148, 40);
    snprintf(text, sizeof(text), "BIOS: %s    UEFI: %s",
        setup.skip_bios ? T("Skip") : T("Install"), setup.skip_uefi ? T("Skip") : T("Install"));
    summary_row(T("Bootloaders"), text, 192, 28);
    if (setup.swap_enabled)
        snprintf(text, sizeof(text), "%llu MiB", (unsigned long long)setup.swap_size_mib);
    else copy_text(text, sizeof(text), T("Disabled"));
    summary_row(T("Swap"), text, 224, 28);
    make_label(T("Disk changes to apply"), CONTENT_X, 258, CONTENT_W, 22, 14, 0)->labelfont(FL_HELVETICA_BOLD);
    static Fl_Text_Buffer plan_buffer;
    static char changes[128u * 1024u];
    review_plan_error = installer_partition_changes(changes, sizeof(changes)) < 0;
    if (review_plan_error)
        copy_text(changes, sizeof(changes), T("The disk plan could not be read. Return to Disk layout and review the target."));
    plan_buffer.text(changes);
    auto *changes_view = new Fl_Text_Display(CONTENT_X, 284, CONTENT_W, CONTENT_H - 384);
    changes_view->buffer(plan_buffer);
    changes_view->textfont(FL_HELVETICA);
    changes_view->textsize(14);
    changes_view->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    reliefos_fltk::style(changes_view);
    make_label(T("These disk changes cannot be undone. Type INSTALL to confirm."),
        CONTENT_X, CONTENT_H - 92, CONTENT_W, 36, 14, 1);
    confirm_input = new Fl_Input(CONTENT_X, CONTENT_H - 48, CONTENT_W / 2, 32);
    confirm_input->value(confirm_text);
    confirm_input->maximum_size((int)sizeof(confirm_text) - 1);
    confirm_input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    confirm_input->callback(confirm_changed);
    reliefos_fltk::style(confirm_input);
}

static void build_update_log(int y)
{
    make_label(T("Update details"), CONTENT_X, y, CONTENT_W, 22, 14, 0);
    if (!update_log_buffer) update_log_buffer = new Fl_Text_Buffer();
    update_log_display = new Fl_Text_Display(CONTENT_X, y + 28, CONTENT_W, CONTENT_H - y - 60);
    update_log_display->buffer(update_log_buffer);
    update_log_display->textfont(FL_HELVETICA);
    update_log_display->textsize(13);
    update_log_display->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    reliefos_fltk::style(update_log_display);
    update_log_display->insert_position(update_log_buffer->length());
    update_log_display->show_insert_position();
    make_label("/var/log/installer-update.log", CONTENT_X, CONTENT_H - 26, CONTENT_W, 20, 12, 0);
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
        build_update_log(294);
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
                : (snprintf(entry, sizeof(entry), "%s", installed_boot_file), !*installed_boot_file);
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
        build_update_log(activity ? 300 : reboot_error ? 290 : 236);
    }
}

static void clear_page_widgets(void)
{
    disk_browser = NULL;
    update_root_choice = update_esp_choice = NULL;
    format_esp_checkbox = NULL;
    partition_browser = NULL;
    for (int i = 0; i < 4; ++i) partition_edit_buttons[i] = nullptr;
    partition_role_choice = nullptr;
    partition_role_button = nullptr;
    confirm_input = NULL;
    desktop_choice = NULL;
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
    // clear() restores resizable(this); each page explicitly assigns the
    // available space to its lists and text views, keeping every control visible.
    content_group->resizable(nullptr);
    content_group->resize(SIDEBAR_W, 0, main_window->w() - SIDEBAR_W, CONTENT_H);
    clear_page_widgets();
    content_group->begin();
    switch ((enum installer_page)page) {
    case INSTALLER_PAGE_LANGUAGE: build_language_page(); break;
    case INSTALLER_PAGE_LICENSE: build_license_page(); break;
    case INSTALLER_PAGE_WELCOME: build_mode_page(); break;
    case INSTALLER_PAGE_MODE: build_mode_page(); break;
    case INSTALLER_PAGE_DISK: build_disk_page(); break;
    case INSTALLER_PAGE_BOOTLOADERS: build_bootloader_page(); break;
    case INSTALLER_PAGE_PARTITIONS: build_partition_page(); break;
    case INSTALLER_PAGE_SYSTEM: build_system_page(); break;
    case INSTALLER_PAGE_ACCOUNTS: build_accounts_page(); break;
    case INSTALLER_PAGE_CONFIRM: build_confirm_page(); break;
    case INSTALLER_PAGE_PROGRESS: build_progress_page(); break;
    case INSTALLER_PAGE_FINISH: build_finish_page(); break;
    default: break;
    }
    content_group->end();
    fprintf(stderr, "[installer.elf] page=%u mode=%u\n", (unsigned)page, (unsigned)install_mode);
    update_sidebar();
    update_footer();
    refresh_status_widgets();
    if (main_window) main_window->redraw();
}

static int app_event_handler(int event)
{
    if (Fl::modal()) return 0;
    if (event == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        if (installer_model_can_go_back((enum installer_page)page, install_success)) go_back(nullptr, nullptr);
        else cancel_setup(nullptr, nullptr);
        return 1;
    }
    return 0;
}

extern "C" void refresh_ui(void)
{
    if (installer_job_is_worker()) { installer_tui_publish_progress(); return; }
    if (installer_tty_mode) return;
    if (!main_window) return;
    refresh_status_widgets();
    update_footer();
    Fl::check();
    Fl::flush();
}

static void sidebar_pressed(Fl_Widget *, void *data)
{
    auto target = (enum installer_page)(uintptr_t)data;
    if (ui_busy || !installer_model_can_visit((enum installer_page)page, target,
        (enum installer_mode)install_mode, install_success)) return;
    reset_confirm();
    page = target;
    if (page == INSTALLER_PAGE_PARTITIONS) refresh_partitions();
    schedule_render();
}

/* Reflow every page into the visible body. Only lists and document views scroll;
 * navigation and form controls stay visible at the minimum window size. */
class InstallerWindow : public Fl_Window {
public:
    Fl_Group *sidebar = nullptr;
    Fl_Group *footer = nullptr;
    InstallerWindow(int w, int h) : Fl_Window(w, h, T("Install ReliefOS")) {}
    void resize(int x, int y, int w, int h) override {
        int previous_width = this->w(), previous_height = this->h();
        Fl_Window::resize(x, y, w, h);
        if (!footer || !sidebar) return;
        sidebar->resize(0, 0, SIDEBAR_W, h);
        content_group->resize(SIDEBAR_W, 0, w - SIDEBAR_W, h - 80);
        footer->resize(SIDEBAR_W, h - 80, w - SIDEBAR_W, 80);
        back_button->resize(CONTENT_X, h - 56, 136, 38);
        primary_button->resize(CONTENT_X + 154, h - 56, 148, 38);
        cancel_button->resize(w - 160, h - 56, 136, 38);
        CONTENT_W = w - CONTENT_X - 24;
        CONTENT_H = h - 80;
        if (w != previous_width || h != previous_height) schedule_render();
    }
};

static int run_graphical(void)
{
    COLOR_BG = fl_rgb_color(236, 238, 244);
    COLOR_TEXT = fl_rgb_color(34, 36, 46);
    COLOR_SIDEBAR = fl_rgb_color(59, 98, 166);
    COLOR_ACTIVE = FL_WHITE;
    reliefos_fltk::init("installer");
    int width = Fl::w() < WINDOW_W + 24 ? Fl::w() - 24 : WINDOW_W;
    int height = Fl::h() < WINDOW_H + 48 ? Fl::h() - 48 : WINDOW_H;
    if (width < 768) width = 768;
    if (height < 540) height = 540;
    CONTENT_W = width - CONTENT_X - 24;
    CONTENT_H = height - 80;
    auto *window = new InstallerWindow(width, height);
    main_window = window;
    main_window->color(COLOR_BG);
    main_window->callback(window_close);
    window->sidebar = new Fl_Group(0, 0, SIDEBAR_W, height);
    auto *sidebar = window->sidebar;
    sidebar->box(FL_FLAT_BOX);
    sidebar->color(COLOR_SIDEBAR);
    sidebar->resizable(nullptr);
    auto *brand = new Fl_Box(16, 18, SIDEBAR_W - 30, 30, "ReliefOS");
    brand->labelfont(FL_HELVETICA_BOLD);
    brand->labelsize(22);
    brand->labelcolor(FL_WHITE);
    reliefos_fltk::style(brand);
    brand_setup = new Fl_Box(16, 48, SIDEBAR_W - 30, 24, T("Setup"));
    brand_setup->labelsize(14);
    brand_setup->labelcolor(FL_WHITE);
    reliefos_fltk::style(brand_setup);
    for (int i = 0; i < INSTALLER_PAGE_COUNT; ++i) {
        sidebar_rows[i] = new Fl_Button(12, 100 + i * 34, SIDEBAR_W - 24, 30, "");
        sidebar_rows[i]->box(FL_FLAT_BOX);
        sidebar_rows[i]->down_box(FL_FLAT_BOX);
        sidebar_rows[i]->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        sidebar_rows[i]->labelsize(15);
        sidebar_rows[i]->labelcolor(FL_WHITE);
        sidebar_rows[i]->callback(sidebar_pressed);
        reliefos_fltk::style(sidebar_rows[i]);
    }
    sidebar->end();
    content_group = new Fl_Group(SIDEBAR_W, 0, width - SIDEBAR_W, height - 80);
    content_group->box(FL_FLAT_BOX);
    content_group->color(COLOR_BG);
    content_group->resizable(nullptr);
    content_group->end();
    window->footer = new Fl_Group(SIDEBAR_W, height - 80, width - SIDEBAR_W, 80);
    window->footer->resizable(nullptr);
    window->footer->box(FL_FLAT_BOX);
    window->footer->color(COLOR_BG);
    back_button = make_button(T("Previous Step"), CONTENT_X, height - 56, 136, 38, go_back, nullptr);
    primary_button = new Fl_Return_Button(CONTENT_X + 154, height - 56, 148, 38, T("Next"));
    primary_button->callback(go_next);
    primary_button->labelsize(14);
    reliefos_fltk::style(primary_button);
    cancel_button = make_button(T("Cancel"), width - 160, height - 56, 136, 38, cancel_setup, nullptr);
    window->footer->end();
    window->resizable(content_group);
    window->size_range(768, 540);
    window->end();
    Fl::add_handler(app_event_handler);
    show_page();
    window->show();
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
    int text_mode = argc == 2 && strcmp(argv[1], "--text") == 0;

    setlocale(LC_ALL, "");
    bindtextdomain("installer", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("installer", "UTF-8");
    textdomain("installer");
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 1 && !graphical && !text_mode) {
        fputs("usage: installer [--graphical|--text]\n", stderr);
        return 2;
    }
    if (text_mode || (!graphical && isatty(STDIN_FILENO))) {
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
