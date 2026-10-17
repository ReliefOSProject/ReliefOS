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

extern "C" {
#include <reliefos/blockdev.h>
#include <reliefos/system.h>
}

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include "../fltk_support.h"
#include "../locale_settings.h"
#include "install_ops.h"
#include "installer_tty.h"
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
static Fl_Input *confirm_input;
static Fl_Check_Button *xorg_checkbox;
static Fl_Input *account_inputs[5];
static Fl_Box *account_hint;
static Fl_Box *status_box;
static Fl_Box *detail_box;
static Fl_Box *progress_text_box;
static Fl_Box *progress_percent_box;
static Fl_Progress *progress_bar;
static int ui_busy;
static int primary_pending;
static int back_pending;
static int render_pending;
static int account_input_overflow[5];
static int confirm_input_overflow;

static const char acknowledgements_en[] =
    "Acknowledgements\n"
    "\n"
    "ReliefOS gratefully acknowledges the creators, contributors, and maintainers "
    "of the public resources and open-source projects used by this release.\n"
    "\n"
    "Runtime, Toolchain, and Applications\n"
    "- GNU GRUB 2 - GPL-3.0-or-later\n"
    "- Mbed TLS 3.6.7 - Apache License 2.0\n"
    "- musl 1.2.6 - MIT\n"
    "- mimalloc 3.5.1 - MIT\n"
    "- zlib 1.3.2, the bundled compression library - zlib License\n"
    "- libpng 1.6.58, the bundled PNG decoder library - libpng License\n"
    "- SQLite 3.46.1 - public-domain dedication and blessing\n"
    "- BusyBox 1.36.1 - GPL-2.0-only\n"
    "- less (Alpine APK) - GPL-3.0-or-later OR BSD-2-Clause\n"
    "- Vim (Alpine APK) - Vim License\n"
    "- Fastfetch 2.68.1 - MIT\n"
    "- sl - permissive upstream license\n"
    "- Leonmmcoset/pl_editor (modified PL Editor fork) - MIT\n"
    "- file and libmagic - BSD-2-Clause\n"
    "- StardustUI - MIT\n"
    "- DoomGeneric - GPL-2.0-only\n"
    "- Freedoom - BSD-3-Clause\n"
    "- rime-pinyin-simp dictionary - Apache License 2.0\n"
    "- minimp3 - CC0-1.0\n"
    "\n"
    "Fonts and Visual Resources\n"
    "- Noto Sans Mono - SIL Open Font License 1.1\n"
    "- Droid Sans Fallback - Apache License 2.0\n"
    "- Noto Sans CJK - SIL Open Font License 1.1\n"
    "- Microsoft fonts used by the distribution - subject to the applicable "
    "Microsoft license terms\n"
    "- NASA Image and Video Library, PIA18033 - used in accordance with NASA "
    "Media Usage Guidelines\n"
    "\n"
    "Recorded Browser Source Dependencies\n"
    "- litehtml - BSD-3-Clause\n"
    "- Gumbo HTML Parser - Apache License 2.0\n"
    "\n"
    "These browser sources are retained for future compatibility work and are "
    "not linked into the current browser runtime.\n"
    "Complete license texts and attribution notices are preserved with the "
    "corresponding source, SDK, and/or installed program package.\n";

static const char acknowledgements_zh[] =
    "感谢\n"
    "\n"
    "ReliefOS 诚挚感谢本发行版所使用的公共资源与开源项目的创作者、贡献者和维护者。\n"
    "\n"
    "运行时、开发工具链与应用程序\n"
    "- GNU GRUB 2 - GPL-3.0-or-later\n"
    "- Mbed TLS 3.6.7 - Apache License 2.0\n"
    "- musl 1.2.6 - MIT\n"
    "- mimalloc 3.5.1 - MIT\n"
    "- zlib 1.3.2（内置压缩库）- zlib License\n"
    "- libpng 1.6.58（内置 PNG 解码库）- libpng License\n"
    "- SQLite 3.46.1 - 公共领域声明与许可祝福文本\n"
    "- BusyBox 1.36.1 - GPL-2.0-only\n"
    "- less (Alpine APK) - GPL-3.0-or-later OR BSD-2-Clause\n"
    "- Vim (Alpine APK) - Vim License\n"
    "- Fastfetch 2.68.1 - MIT\n"
    "- sl - 上游宽松许可\n"
    "- Leonmmcoset/pl_editor（PL Editor 修改版）- MIT\n"
    "- file 与 libmagic - BSD-2-Clause\n"
    "- StardustUI - MIT\n"
    "- DoomGeneric - GPL-2.0-only\n"
    "- Freedoom - BSD-3-Clause\n"
    "- rime-pinyin-simp 中文词库 - Apache License 2.0\n"
    "- minimp3 - CC0-1.0\n"
    "\n"
    "字体与视觉资源\n"
    "- Noto Sans Mono - SIL Open Font License 1.1\n"
    "- Droid Sans Fallback - Apache License 2.0\n"
    "- Noto Sans CJK - SIL Open Font License 1.1\n"
    "- 发行版使用的 Microsoft 字体 - 受相应 Microsoft 许可条款约束\n"
    "- NASA Image and Video Library，PIA18033 - 遵循 NASA 媒体使用指南\n"
    "\n"
    "已记录的浏览器源码依赖\n"
    "- litehtml - BSD-3-Clause\n"
    "- Gumbo HTML Parser - Apache License 2.0\n"
    "\n"
    "这些浏览器源码为后续兼容性工作保留，当前浏览器运行时尚未链接它们。\n"
    "完整许可证文本与归属声明保留在相应源代码、开发套件和/或已安装程序包中。\n";


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
    case INSTALLER_PAGE_THANKS: return T("Thanks");
    case INSTALLER_PAGE_WELCOME: return T("Welcome");
    case INSTALLER_PAGE_MODE: return T("Mode");
    case INSTALLER_PAGE_DISK: return T("Disk");
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
    case INSTALLER_ACTION_RESTART: return T("Restart");
    case INSTALLER_ACTION_CLOSE: return T("Close");
    case INSTALLER_ACTION_NEXT: return T("Next");
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
    if (page == INSTALLER_PAGE_CONFIRM)
        return confirm_input_overflow || !confirmation_ok();
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
        ui_busy = 1;
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        Fl::flush();
        perform_update();
        ui_busy = 0;
        show_page();
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
        ui_busy = 1;
        confirm_input_overflow = 0;
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        Fl::flush();
        prepare_update_target();
        ui_busy = 0;
        show_page();
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
    if (target != language_selection() &&
        !setlocale(LC_ALL, language_options[target].locale))
        return;
    schedule_render();
}

static void build_language_page(void)
{
    make_heading(T("Select Language"), 20);
    make_label(T("Choose the language for Setup and the installed system."),
               CONTENT_X, 62, CONTENT_W, 30, 15, 1);
    Fl_Round_Button *english = new Fl_Round_Button(CONTENT_X + 20, 126, 360, 34,
                                                   language_options[0].name);
    Fl_Round_Button *chinese = new Fl_Round_Button(CONTENT_X + 20, 174, 360, 34,
                                                   language_options[1].name);
    english->type(FL_RADIO_BUTTON);
    chinese->type(FL_RADIO_BUTTON);
    english->value(language_selection() == 0);
    chinese->value(language_selection() == 1);
    english->callback(language_selected, (void *)(uintptr_t)0);
    chinese->callback(language_selected, (void *)(uintptr_t)1);
    reliefos_fltk::style(english);
    reliefos_fltk::style(chinese);
    make_label(T("The installed system will use the same language."),
               CONTENT_X, 238, CONTENT_W, 32, 15, 1);
}

static void build_thanks_page(void)
{
    make_heading(T("Thank You"), 20);
    make_label(T("Acknowledgements for public resources and open-source projects."),
               CONTENT_X, 58, CONTENT_W, 32, 15, 1);
    Fl_Scroll *scroll = new Fl_Scroll(CONTENT_X, 98, CONTENT_W, 500);
    scroll->box(FL_DOWN_BOX);
    scroll->begin();
    Fl_Multiline_Output *text =
        new Fl_Multiline_Output(CONTENT_X, 98, CONTENT_W - 18, 900);
    text->value(language_selection() == 1 ? acknowledgements_zh
                                          : acknowledgements_en);
    text->textsize(13);
    text->wrap(1);
    reliefos_fltk::style(text);
    scroll->end();
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

static void mode_selected(Fl_Widget *widget, void *data)
{
    if (!static_cast<Fl_Round_Button *>(widget)->value()) return;
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

static void select_disk(Fl_Widget *, void *)
{
    if (disk_browser && disk_browser->value() > 0)
        selected_disk = disk_browser->value() - 1;
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
    disk_browser = new Fl_Hold_Browser(CONTENT_X, 132, CONTENT_W, 330);
    disk_browser->callback(select_disk);
    disk_browser->textsize(14);
    disk_browser->selection_color(COLOR_SIDEBAR);
    reliefos_fltk::style(disk_browser);
    status_box = make_label(status_text, CONTENT_X, 478, CONTENT_W, 34, 14, 1);
    detail_box = make_label(detail_text, CONTENT_X, 516, CONTENT_W, 48, 14, 1);
    refresh_disk_list();
}

static void refresh_partition_list(void)
{
    if (!partition_browser) return;
    char line[256];
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
        return T("Type AUTO to erase the disk and create an ESP and root partition:");
    case PARTITION_PROMPT_INITIALIZE:
        return T("Type INITIALIZE to replace the partition table:");
    case PARTITION_PROMPT_CREATE:
        return T("Enter size_MiB filesystem name (ext4/ext2/fat32/exfat):");
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
            filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN && name[0])
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
    make_label(installer_partition_auto
                   ? T("Automatic layout creates a 128 MiB FAT32 ESP and an ext4 root.")
                   : T("Manual layout: select partitions, then assign ESP and root roles."),
               CONTENT_X, 52, CONTENT_W, 38, 14, 1);
    partition_browser = new Fl_Hold_Browser(CONTENT_X, 96, CONTENT_W, 280);
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

    status_box = make_label(status_text, CONTENT_X, 480, CONTENT_W, 30, 13, 1);
    detail_box = make_label(detail_text, CONTENT_X, 512, CONTENT_W, 38, 13, 1);
    make_label(T("Create: size in MiB, filesystem, name. Resize only changes the trailing edge."),
               CONTENT_X, 552, CONTENT_W, 26, 12, 1);
    make_label((installer_partition_auto || installer_partition_table_replaced)
                   ? T("Final confirmation formats a FAT32 ESP and ext4 root on the selected disk.")
                   : T("Final confirmation formats only the selected ESP and root; other partitions are preserved."),
               CONTENT_X, 578, CONTENT_W, 32, 12, 1);
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

static void build_confirm_page(void)
{
    char line[160];
    char target[192];
    make_heading(install_mode == INSTALLER_MODE_UPDATE
                     ? T("Confirm Update") : T("Confirm Installation"), 16);
    make_label(install_mode == INSTALLER_MODE_UPDATE
                   ? T("Installed ReliefOS packages will be upgraded.")
                   : T("This operation is destructive."),
               CONTENT_X, 54, CONTENT_W, 32, 15, 1);
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count)
        format_disk_line(line, sizeof(line), &disks[selected_disk]);
    else
        line[0] = 0;
    snprintf(target, sizeof(target), "%s %s", T("Target:"), line);
    make_label(target, CONTENT_X, 98, CONTENT_W, 34, 14, 1);
    make_label(install_mode == INSTALLER_MODE_UPDATE
                   ? T("Alpine packages and local configuration are retained. Boot files are updated after the package transaction succeeds.")
                   : (installer_partition_auto || installer_partition_table_replaced)
                       ? T("The selected disk will be erased and formatted with a FAT32 ESP and ext4 system root.")
                       : T("Only the selected ESP and root partitions will be formatted; other partitions are preserved."),
               CONTENT_X, 142, CONTENT_W, 64, 14, 1);
    make_label(install_mode == INSTALLER_MODE_UPDATE
                   ? T("Type UPDATE to enable the Update button.")
                   : T("Type INSTALL to enable the Install button."),
               CONTENT_X, 224, CONTENT_W, 30, 14, 1);
    confirm_input = new Fl_Input(CONTENT_X, 260, 330, 36);
    confirm_input->value(confirm_text);
    confirm_input->maximum_size((int)sizeof(confirm_text) - 1);
    confirm_input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    confirm_input->callback(confirm_changed);
    reliefos_fltk::style(confirm_input);
    if (install_mode != INSTALLER_MODE_UPDATE) {
        xorg_checkbox = new Fl_Check_Button(CONTENT_X, 318, 560, 32,
                                             T("Install Xorg desktop environment"));
        xorg_checkbox->value(setup.skip_xorg ? 0 : 1);
        xorg_checkbox->callback(xorg_install_changed);
        reliefos_fltk::style(xorg_checkbox);
    }
}

static void build_progress_page(void)
{
    make_heading(mode_progress_title(), 20);
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
        if (reboot_error) {
            make_label(T("Restart failed"), CONTENT_X, 154,
                       CONTENT_W, 28, 14, 1);
            snprintf(message, sizeof(message), "%s", strerror(reboot_error));
            make_label(message, CONTENT_X, 184, CONTENT_W, 36, 14, 1);
        }
    } else {
        make_heading(install_mode == INSTALLER_MODE_UPDATE
                         ? T("Update Failed") : T("Installation Failed"), 20);
        make_label(T("No writes will continue after this error."),
                   CONTENT_X, 62, CONTENT_W, 32, 15, 1);
        make_label(status_text, CONTENT_X, 108, CONTENT_W, 42, 14, 1);
        make_label(detail_text, CONTENT_X, 154, CONTENT_W, 60, 14, 1);
    }
}

static void clear_page_widgets(void)
{
    disk_browser = NULL;
    partition_browser = NULL;
    confirm_input = NULL;
    xorg_checkbox = NULL;
    account_hint = NULL;
    status_box = NULL;
    detail_box = NULL;
    progress_text_box = NULL;
    progress_percent_box = NULL;
    progress_bar = NULL;
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
    case INSTALLER_PAGE_THANKS: build_thanks_page(); break;
    case INSTALLER_PAGE_WELCOME: build_welcome_page(); break;
    case INSTALLER_PAGE_MODE: build_mode_page(); break;
    case INSTALLER_PAGE_DISK: build_disk_page(); break;
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
    struct installer_tty_context tty_context;
    int graphical = argc == 2 && strcmp(argv[1], "--graphical") == 0;

    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("leonos", "UTF-8");
    textdomain("leonos");
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
        tty_context.prepare_update = prepare_update_target;
        tty_context.perform_install = perform_install;
        tty_context.perform_update = perform_update;
        return installer_tty_main(&tty_context);
    }
    puts("[installer.elf] starting installer wizard");
    return run_graphical();
}
