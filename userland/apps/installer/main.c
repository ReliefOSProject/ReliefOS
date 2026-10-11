/* ReliefOS installer wizard: Motif X11 frontend over the install operations.
 *
 * The page machine and destructive operations live in model.c and
 * install_ops.c; this file only renders pages and forwards user intent. */
#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SelectioB.h>
#include <Xm/ScrolledW.h>
#include <Xm/Text.h>
#include <Xm/ToggleB.h>
#include <reliefos/layout.h>
#include <reliefos/system.h>

#include "../locale_settings.h"
#include "install_ops.h"
#include "installer_tty.h"
#include "model.h"

#define T(s) gettext(s)

#define SIDEBAR_W 180
#define FOOTER_H 72
#define ACCOUNT_FIELD_W 380

static XtAppContext app;
static Widget shell;
static Widget content;
static Widget sidebar;
static Widget sidebar_rows[INSTALLER_PAGE_COUNT];
static Widget back_button;
static Widget primary_button;
static Widget cancel_button;
static Widget disk_list;
static Widget partition_list;
static Widget confirm_field;
static Widget account_fields[5];
static Widget account_hint;
static Widget progress_text_label;
static Widget progress_bar_label;
static unsigned long sidebar_bg;
static unsigned long sidebar_bg_active;
static unsigned long sidebar_fg;
static unsigned long sidebar_fg_active;

enum partition_prompt_action {
    PARTITION_PROMPT_AUTO = 1,
    PARTITION_PROMPT_INITIALIZE,
    PARTITION_PROMPT_CREATE,
    PARTITION_PROMPT_RESIZE,
    PARTITION_PROMPT_RENAME,
    PARTITION_PROMPT_FORMAT,
    PARTITION_PROMPT_DELETE,
};

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

static void key(Widget widget, XtPointer unused, XEvent *event, Boolean *dispatch);
static void set_label(Widget widget, const char *text)
{
    XmString value = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, value, NULL);
    XmStringFree(value);
}

static unsigned long alloc_color(Display *display, uint32_t rgb)
{
    XColor color;
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    color.red = (unsigned short)(((rgb >> 16) & 0xffu) * 0x0101u);
    color.green = (unsigned short)(((rgb >> 8) & 0xffu) * 0x0101u);
    color.blue = (unsigned short)((rgb & 0xffu) * 0x0101u);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, colormap, &color)) {
        return BlackPixel(display, DefaultScreen(display));
    }
    return color.pixel;
}

static void close_now(void)
{
    exit(0);
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
    switch (installer_model_action(page, install_mode, install_success)) {
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
    if (page == INSTALLER_PAGE_ACCOUNTS) return !installer_setup_valid(&setup);
    if (install_running) return 1;
    if (page == INSTALLER_PAGE_DISK) {
        return selected_disk < 0 || (uint32_t)selected_disk >= disk_count;
    }
    if (page == INSTALLER_PAGE_PARTITIONS) {
        return !installer_partition_plan_valid();
    }
    if (page == INSTALLER_PAGE_CONFIRM) return !confirmation_ok();
    return 0;
}

static void update_sidebar(void)
{
    enum installer_page steps[INSTALLER_PAGE_COUNT];
    int count = installer_model_steps(install_mode, steps, INSTALLER_PAGE_COUNT);
    for (int i = 0; i < INSTALLER_PAGE_COUNT; ++i) {
        Widget row = sidebar_rows[i];
        if (i < count) {
            int active = steps[i] == (enum installer_page)page;
            XtVaSetValues(row, XmNmappedWhenManaged, True,
                          XmNbackground, active ? sidebar_bg_active : sidebar_bg,
                          XmNforeground, active ? sidebar_fg_active : sidebar_fg,
                          NULL);
            set_label(row, page_label(steps[i]));
        } else {
            XtVaSetValues(row, XmNmappedWhenManaged, False, NULL);
        }
    }
}

static void update_footer(void)
{
    set_label(primary_button, primary_label());
    XtVaSetValues(back_button,
                  XmNsensitive, installer_model_can_go_back(page, install_success),
                  NULL);
    XtVaSetValues(primary_button, XmNsensitive, !primary_disabled(), NULL);
    XtVaSetValues(cancel_button,
                  XmNsensitive, installer_model_can_cancel(page, install_success),
                  NULL);
}

static void show_page(void);

static void go_primary(void)
{
    if (primary_disabled()) {
        return;
    }
    switch (installer_model_action(page, install_mode, install_success)) {
    case INSTALLER_ACTION_INSTALL:
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        perform_install();
        show_page();
        return;
    case INSTALLER_ACTION_UPDATE:
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        perform_update();
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
        /* Update mode performs its read-only target checks now, then lands on
         * the confirm page (or the finish page when the target is rejected). */
        page = INSTALLER_PAGE_PROGRESS;
        show_page();
        prepare_update_target();
        show_page();
        return;
    }
    page = (uint8_t)installer_model_next(page, install_mode);
    if (page == INSTALLER_PAGE_DISK) {
        refresh_disks();
    }
    if (page == INSTALLER_PAGE_PARTITIONS) {
        refresh_partitions();
    }
    if (page == INSTALLER_PAGE_CONFIRM) {
        reset_confirm();
    }
    show_page();
}

static void go_back(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    if (!installer_model_can_go_back(page, install_success)) {
        return;
    }
    page = (uint8_t)installer_model_prev(page, install_mode);
    if (page == INSTALLER_PAGE_DISK) {
        refresh_disks();
    }
    if (page == INSTALLER_PAGE_PARTITIONS) {
        refresh_partitions();
    }
    show_page();
}

static void go_next(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    go_primary();
}

static void cancel_setup(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    if (installer_model_can_cancel(page, install_success)) {
        close_now();
    }
}

/* --- page content construction ------------------------------------------ */

static Widget place_label(Widget parent, const char *text, int top, int left)
{
    Widget label = XtVaCreateManagedWidget("label", xmLabelWidgetClass, parent,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, top,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, left,
        XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    set_label(label, text);
    return label;
}

static void language_selected(Widget widget, XtPointer unused, XtPointer call)
{
    unsigned target;
    (void)unused;
    (void)call;
    if (!XmToggleButtonGetState(widget)) {
        return;
    }
    target = !strcmp(XtName(widget), "zh") ? 1U : 0U;
    if (target != language_selection()) {
        if (!setlocale(LC_ALL, language_options[target].locale)) {
            return;
        }
    }
    show_page();
}

static void build_language_page(void)
{
    place_label(content, T("Select Language"), 16, 24);
    place_label(content, T("Choose the language for Setup and the installed system."),
                48, 24);
    Widget radio = XtVaCreateManagedWidget("language", xmRowColumnWidgetClass, content,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 96,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
        XmNradioBehavior, True, XmNpacking, XmPACK_COLUMN, XmNnumColumns, 1, NULL);
    static const char *const names[2] = {"en", "zh"};
    unsigned current = language_selection();
    for (int i = 0; i < 2; ++i) {
        Widget toggle = XtVaCreateManagedWidget(names[i], xmToggleButtonWidgetClass,
                                                radio, NULL);
        set_label(toggle, language_options[i].name);
        XtVaSetValues(toggle, XmNset, i == (int)current ? True : False, NULL);
        XtAddCallback(toggle, XmNvalueChangedCallback, language_selected, NULL);
    }
    place_label(content, T("The installed system will use the same language."), 160, 24);
}

static void build_thanks_page(void)
{
    Arg args[6];
    Cardinal n = 0;
    Widget text;
    place_label(content, T("Thank You"), 16, 24);
    place_label(content, T("Acknowledgements for public resources and open-source projects."),
                48, 24);
    XtSetArg(args[n], XmNeditMode, XmMULTI_LINE_EDIT); ++n;
    XtSetArg(args[n], XmNeditable, False); ++n;
    XtSetArg(args[n], XmNscrollBarDisplayPolicy, XmAS_NEEDED); ++n;
    XtSetArg(args[n], XmNscrollVertical, True); ++n;
    XtSetArg(args[n], XmNwordWrap, True); ++n;
    XtSetArg(args[n], XmNvalue,
             language_selection() == 1 ? acknowledgements_zh : acknowledgements_en); ++n;
    text = XmCreateScrolledText(content, "acknowledgements", args, n);
    XtVaSetValues(XtParent(text),
                  XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 88,
                  XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
                  XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24,
                  XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 24, NULL);
    XtManageChild(text);
}

static void build_welcome_page(void)
{
    place_label(content, T("ReliefOS Setup"), 16, 24);
    place_label(content, T("Install a new system or update an existing ReliefOS disk."),
                48, 24);
    place_label(content, T("Setup can copy the full normal system payload"), 96, 24);
    place_label(content, T("or replace the boot/reliefos and system files on an existing installation."),
                122, 24);
    place_label(content, T("SATA/AHCI and IDE/PATA target disks are supported."), 168, 24);
}

static void mode_selected(Widget widget, XtPointer unused, XtPointer call)
{
    (void)unused;
    (void)call;
    if (!XmToggleButtonGetState(widget)) {
        return;
    }
    install_mode = !strcmp(XtName(widget), "update")
                       ? INSTALLER_MODE_UPDATE : INSTALLER_MODE_FRESH;
    show_page();
}

static void build_mode_page(void)
{
    place_label(content, T("Choose Setup Mode"), 16, 24);
    place_label(content, T("Fresh install erases the disk. Update keeps existing users and extra programs."),
                48, 24);
    Widget radio = XtVaCreateManagedWidget("mode", xmRowColumnWidgetClass, content,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 96,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
        XmNradioBehavior, True, XmNpacking, XmPACK_COLUMN, XmNnumColumns, 1, NULL);
    Widget fresh = XtVaCreateManagedWidget("fresh", xmToggleButtonWidgetClass, radio, NULL);
    Widget update = XtVaCreateManagedWidget("update", xmToggleButtonWidgetClass, radio, NULL);
    set_label(fresh, T("Fresh Install"));
    set_label(update, T("Update Existing System"));
    XtVaSetValues(fresh, XmNset, install_mode == INSTALLER_MODE_FRESH ? True : False, NULL);
    XtVaSetValues(update, XmNset, install_mode == INSTALLER_MODE_UPDATE ? True : False, NULL);
    XtAddCallback(fresh, XmNvalueChangedCallback, mode_selected, NULL);
    XtAddCallback(update, XmNvalueChangedCallback, mode_selected, NULL);
    place_label(content, T("Prepare a clean ReliefOS system with automatic or manual disk partitioning."), 176, 24);
    place_label(content, T("Choose the target disk, configure its partitions, then replace boot, system and EFI files."),
                228, 24);
}

static void select_disk(Widget widget, XtPointer unused, XtPointer call)
{
    XmListCallbackStruct *selection = call;
    (void)widget;
    (void)unused;
    if (!selection || selection->item_position < 1) {
        return;
    }
    selected_disk = selection->item_position - 1;
    update_footer();
}

static void refresh_disk_list(void)
{
    char line[160];
    XmListDeleteAllItems(disk_list);
    for (uint32_t i = 0; i < disk_count && i < RELIEFOS_BLOCK_MAX_DISKS; ++i) {
        XmString item;
        format_disk_line(line, sizeof(line), &disks[i]);
        item = XmStringCreateLocalized(line);
        XmListAddItemUnselected(disk_list, item, 0);
        XmStringFree(item);
    }
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count) {
        XmListSelectPos(disk_list, selected_disk + 1, False);
    }
    update_footer();
}

static void refresh_pressed(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    refresh_disks();
    refresh_disk_list();
}

static void build_disk_page(void)
{
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Select Disk to Update") : T("Select Installation Disk"),
                16, 24);
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Choose the disk that already contains ReliefOS.")
                             : T("Choose the disk that will receive ReliefOS."),
                48, 24);
    Widget refresh = XtVaCreateManagedWidget("refresh", xmPushButtonWidgetClass, content,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 88,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24, NULL);
    set_label(refresh, T("Refresh"));
    XtAddCallback(refresh, XmNactivateCallback, refresh_pressed, NULL);
    place_label(content, T("Available disks"), 96, 24);
    disk_list = XmCreateScrolledList(content, "disks", NULL, 0);
    XtVaSetValues(disk_list, XmNselectionPolicy, XmBROWSE_SELECT,
                  XmNvisibleItemCount, 8, NULL);
    XtVaSetValues(XtParent(disk_list),
                  XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 124,
                  XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
                  XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24,
                  XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 96, NULL);
    XtAddCallback(disk_list, XmNbrowseSelectionCallback, select_disk, NULL);
    XtManageChild(disk_list);
    {
        Widget status = place_label(content, status_text, 0, 24);
        Widget detail = place_label(content, detail_text, 0, 24);
        XtVaSetValues(status,
                      XmNtopAttachment, XmATTACH_NONE,
                      XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 64, NULL);
        XtVaSetValues(detail,
                      XmNtopAttachment, XmATTACH_NONE,
                      XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 36, NULL);
    }
    refresh_disk_list();
}

static void refresh_partition_list(void)
{
    char line[256];
    XmListDeleteAllItems(partition_list);
    for (uint32_t i = 0; i < partition_count; ++i) {
        XmString item;
        format_partition_line(line, sizeof(line), &partitions[i]);
        item = XmStringCreateLocalized(line);
        XmListAddItemUnselected(partition_list, item, 0);
        XmStringFree(item);
    }
    if (selected_partition >= 0 && (uint32_t)selected_partition < partition_count)
        XmListSelectPos(partition_list, selected_partition + 1, False);
    update_footer();
}

static void select_partition(Widget widget, XtPointer unused, XtPointer call)
{
    XmListCallbackStruct *selection = call;
    (void)widget;
    (void)unused;
    if (!selection || selection->item_position < 1) return;
    selected_partition = selection->item_position - 1;
    update_footer();
}

static void partition_result(int ret, const char *success)
{
    char detail[128];
    if (ret < 0) {
        snprintf(detail, sizeof(detail), "%s ret=%d", T("The disk may have been changed"), ret);
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
        if (length + 1 + strlen(remainder) >= capacity) return -ENAMETOOLONG;
        out[length++] = ' ';
        memcpy(out + length, remainder, strlen(remainder) + 1);
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

static void partition_prompt_cancel(Widget widget, XtPointer unused, XtPointer call)
{
    (void)unused;
    (void)call;
    XtDestroyWidget(widget);
}

static void partition_prompt_ok(Widget widget, XtPointer data, XtPointer call)
{
    XmSelectionBoxCallbackStruct *selection = call;
    char *value = NULL;
    char *cursor;
    char *word;
    uint32_t number, filesystem;
    char name[RELIEFOS_BLOCK_NAME_LEN];
    int ret = -EINVAL;
    enum partition_prompt_action action = (enum partition_prompt_action)(uintptr_t)data;
    if (!selection || !XmStringGetLtoR(selection->value, XmFONTLIST_DEFAULT_TAG, &value)) {
        XtDestroyWidget(widget);
        return;
    }
    cursor = value;
    word = partition_next_word(&cursor);
    if (action == PARTITION_PROMPT_AUTO) {
        if (word && text_eq(word, "AUTO")) ret = installer_partition_auto_layout();
    } else if (action == PARTITION_PROMPT_INITIALIZE) {
        if (word && text_eq(word, "INITIALIZE")) ret = installer_partition_initialize();
    } else if (action == PARTITION_PROMPT_CREATE) {
        char *filesystem_name = partition_next_word(&cursor);
        char *name_word = partition_next_word(&cursor);
        filesystem = partition_filesystem(filesystem_name);
        if (!name_word) name_word = "ReliefOS Data";
        if (partition_collect_name(name, sizeof(name), name_word, &cursor) < 0)
            name[0] = 0;
        if (!partition_number(word, &number) && filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN)
            if (name[0]) ret = installer_partition_create(filesystem, number, name);
    } else if (action == PARTITION_PROMPT_RESIZE) {
        if (!partition_number(word, &number)) ret = installer_partition_resize(number);
    } else if (action == PARTITION_PROMPT_RENAME) {
        if (word && !partition_collect_name(name, sizeof(name), word, &cursor))
            ret = installer_partition_rename(name);
    } else if (action == PARTITION_PROMPT_FORMAT) {
        char *label = partition_next_word(&cursor);
        filesystem = partition_filesystem(word);
        if (!label) label = "RELIEFOS_DATA";
        if (filesystem != RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN)
            ret = installer_partition_format(filesystem, label);
    } else if (action == PARTITION_PROMPT_DELETE) {
        if (word && text_eq(word, "DELETE")) ret = installer_partition_delete();
    }
    if (value) XtFree(value);
    XtDestroyWidget(widget);
    partition_result(ret, T("Review the selected ESP and root before continuing."));
}

static void open_partition_prompt(enum partition_prompt_action action,
                                  const char *title, const char *message,
                                  const char *initial)
{
    XmString dialog_title = XmStringCreateLocalized((char *)title);
    XmString prompt = XmStringCreateLocalized((char *)message);
    XmString value = XmStringCreateLocalized((char *)initial);
    Widget dialog = XmCreatePromptDialog(content, "partitionPrompt", NULL, 0);
    XtVaSetValues(dialog, XmNdialogTitle, dialog_title,
                  XmNselectionLabelString, prompt, XmNtextString, value, NULL);
    XtAddCallback(dialog, XmNokCallback, partition_prompt_ok, (XtPointer)(uintptr_t)action);
    XtAddCallback(dialog, XmNcancelCallback, partition_prompt_cancel, NULL);
    XtUnmanageChild(XmSelectionBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XmStringFree(dialog_title);
    XmStringFree(prompt);
    XmStringFree(value);
    XtManageChild(dialog);
}

static void partition_prompt_pressed(Widget widget, XtPointer data, XtPointer call)
{
    enum partition_prompt_action action = (enum partition_prompt_action)(uintptr_t)data;
    (void)widget;
    (void)call;
    switch (action) {
    case PARTITION_PROMPT_AUTO:
        open_partition_prompt(action, T("Automatic partitioning"),
                              T("Type AUTO to erase the disk and create ESP + root:"), "AUTO");
        break;
    case PARTITION_PROMPT_INITIALIZE:
        open_partition_prompt(action, T("Initialize GPT"),
                              T("Type INITIALIZE to replace the partition table:"), "INITIALIZE");
        break;
    case PARTITION_PROMPT_CREATE:
        open_partition_prompt(action, T("Create partition"),
                              T("Enter: size_MiB filesystem name (ext4/ext2/fat32/exfat):"),
                              "1024 ext4 ReliefOS Data");
        break;
    case PARTITION_PROMPT_RESIZE:
        open_partition_prompt(action, T("Resize partition"),
                              T("Enter the new size in MiB:"), "1024");
        break;
    case PARTITION_PROMPT_RENAME:
        open_partition_prompt(action, T("Rename partition"),
                              T("Enter the GPT partition name:"), "ReliefOS Data");
        break;
    case PARTITION_PROMPT_FORMAT:
        open_partition_prompt(action, T("Format partition"),
                              T("Enter: filesystem label (this destroys its files):"), "ext4 RELIEFOS_DATA");
        break;
    case PARTITION_PROMPT_DELETE:
        open_partition_prompt(action, T("Delete partition"),
                              T("Type DELETE to remove the selected partition:"), "DELETE");
        break;
    }
}

static void partition_refresh_pressed(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    refresh_partitions();
    refresh_partition_list();
}

static void partition_role_pressed(Widget widget, XtPointer data, XtPointer call)
{
    uint32_t type = (uint32_t)(uintptr_t)data;
    int ret;
    (void)widget;
    (void)call;
    if (selected_partition < 0 || (uint32_t)selected_partition >= partition_count) {
        partition_result(-EINVAL, "");
        return;
    }
    ret = installer_partition_set_type(type);
    partition_result(ret, T("Partition role updated."));
}

static void build_partition_page(void)
{
    Widget action_box;
    place_label(content, T("Configure partitions"), 16, 24);
    place_label(content, installer_partition_auto
                             ? T("Automatic layout will create and format a 128 MiB ESP and an ext4 root.")
                             : T("Manual layout: select partitions, then assign ESP and root roles."),
                48, 24);
    partition_list = XmCreateScrolledList(content, "partitions", NULL, 0);
    XtVaSetValues(partition_list, XmNselectionPolicy, XmBROWSE_SELECT,
                  XmNvisibleItemCount, 8, NULL);
    XtVaSetValues(XtParent(partition_list),
                  XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 82,
                  XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
                  XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24,
                  XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 300, NULL);
    XtAddCallback(partition_list, XmNbrowseSelectionCallback, select_partition, NULL);
    XtManageChild(partition_list);
    action_box = XtVaCreateManagedWidget("partitionActions", xmRowColumnWidgetClass, content,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 310,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24,
        XmNpacking, XmPACK_COLUMN, XmNnumColumns, 4, XmNorientation, XmHORIZONTAL, NULL);
    {
        const char *const labels[] = {"Refresh", "Automatic", "Initialize GPT", "Create",
                                      "Resize", "Rename", "Format", "Delete",
                                      "Set ESP", "Set root", "Data type"};
        const enum partition_prompt_action prompt_actions[] = {
            0, PARTITION_PROMPT_AUTO, PARTITION_PROMPT_INITIALIZE, PARTITION_PROMPT_CREATE,
            PARTITION_PROMPT_RESIZE, PARTITION_PROMPT_RENAME, PARTITION_PROMPT_FORMAT,
            PARTITION_PROMPT_DELETE
        };
        for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
            Widget button = XtVaCreateManagedWidget("partitionAction", xmPushButtonWidgetClass,
                                                    action_box, NULL);
            set_label(button, T(labels[i]));
            if (i == 0) XtAddCallback(button, XmNactivateCallback, partition_refresh_pressed, NULL);
            else if (i < 8) XtAddCallback(button, XmNactivateCallback,
                                           partition_prompt_pressed,
                                           (XtPointer)(uintptr_t)prompt_actions[i]);
            else if (i == 8) XtAddCallback(button, XmNactivateCallback,
                                            partition_role_pressed,
                                            (XtPointer)(uintptr_t)RELIEFOS_BLOCK_GPT_ESP);
            else if (i == 9) XtAddCallback(button, XmNactivateCallback,
                                            partition_role_pressed,
                                            (XtPointer)(uintptr_t)RELIEFOS_BLOCK_GPT_LINUX);
            else XtAddCallback(button, XmNactivateCallback, partition_role_pressed,
                               (XtPointer)(uintptr_t)RELIEFOS_BLOCK_GPT_BASIC_DATA);
        }
    }
    place_label(content, T("Create: size in MiB, filesystem, name. Resize only changes the trailing edge."),
                490, 24);
    place_label(content, (installer_partition_auto || installer_partition_table_replaced)
                             ? T("The final confirmation formats a FAT32 ESP and ext4 root on the selected disk.")
                             : T("The final confirmation formats only the selected ESP and root partitions."),
                520, 24);
    refresh_partition_list();
}

static void copy_field(char *dst, uint32_t cap, Widget field)
{
    char *value = XmTextGetString(field);
    copy_text(dst, cap, value ? value : "");
    if (value) {
        XtFree(value);
    }
}

static void secret_changed(Widget widget, XtPointer data, XtPointer call)
{
    XmTextVerifyCallbackStruct *change = call;
    char *secret = data;
    (void)widget;
    int count;
    if (!change || !change->text) return;
    count = change->text->length < 0 ? -1 :
        installer_model_secret_characters(change->text->ptr, change->text->length);
    if (!change->doit || change->startPos < 0 || change->endPos < 0 || count < 0 ||
        !installer_model_edit_secret(secret, RELIEFOS_AUTH_PASSWORD_LEN,
                                     change->startPos, change->endPos,
                                     change->text->ptr, change->text->length)) {
        change->doit = False;
        return;
    }
    for (int i = 0; i < count; ++i) change->text->ptr[i] = '*';
    change->text->length = count;
}

static void account_changed(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    copy_field(setup.username, sizeof(setup.username), account_fields[0]);
    if (account_hint) {
        XtVaSetValues(account_hint, XmNmappedWhenManaged,
                      (setup.username[0] && !installer_setup_valid(&setup)) ? True : False,
                      NULL);
    }
    update_footer();
}

static void account_activated(Widget widget, XtPointer unused, XtPointer call)
{
    (void)unused;
    (void)call;
    account_changed(widget, NULL, NULL);
    if (widget == account_fields[4]) {
        go_primary();
    } else {
        for (int i = 0; i < 4; ++i) {
            if (widget == account_fields[i]) {
                XmProcessTraversal(account_fields[i + 1], XmTRAVERSE_CURRENT);
                return;
            }
        }
    }
}

static void build_accounts_page(void)
{
    const char *labels[5];
    char *values[5];
    labels[0] = T("Standard user name");
    labels[1] = T("Standard user password");
    labels[2] = T("Confirm password");
    labels[3] = T("root password");
    labels[4] = T("Confirm root password");
    values[0] = setup.username;
    values[1] = setup.password;
    values[2] = setup.password_confirm;
    values[3] = setup.root_password;
    values[4] = setup.root_password_confirm;
    place_label(content, T("Accounts"), 16, 24);
    place_label(content, T("Administrator: root"), 48, 24);
    for (int i = 0; i < 5; ++i) {
        char name[16];
        char masked[RELIEFOS_AUTH_PASSWORD_LEN];
        const char *shown = values[i];
        if (i) {
            int length = installer_model_secret_characters(values[i], strlen(values[i]));
            if (length < 0) length = 0;
            memset(masked, '*', length);
            masked[length] = 0;
            shown = masked;
        }
        place_label(content, labels[i], 88 + i * 58, 24);
        snprintf(name, sizeof(name), "account%d", i);
        account_fields[i] = XtVaCreateManagedWidget(name, xmTextWidgetClass, content,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 112 + i * 58,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
            XmNwidth, ACCOUNT_FIELD_W,
            XmNmaxLength, i ? RELIEFOS_AUTH_PASSWORD_MAX_CHARS : RELIEFOS_AUTH_USERNAME_LEN - 1,
            XmNvalue, shown, NULL);
        if (i) {
            XtAddCallback(account_fields[i], XmNmodifyVerifyCallback, secret_changed, values[i]);
        }
        XtAddCallback(account_fields[i], XmNvalueChangedCallback, account_changed, NULL);
        XtAddCallback(account_fields[i], XmNactivateCallback, account_activated, NULL);
    }
    account_hint = place_label(content,
        T("Passwords: 1-32 characters, no spaces; confirmations must match"),
        88 + 5 * 58, 24);
    if (!setup.username[0] || installer_setup_valid(&setup)) {
        XtVaSetValues(account_hint, XmNmappedWhenManaged, False, NULL);
    }
}

static void confirm_changed(Widget widget, XtPointer unused, XtPointer call)
{
    (void)unused;
    (void)call;
    copy_field(confirm_text, sizeof(confirm_text), widget);
    update_footer();
}

static void confirm_activated(Widget widget, XtPointer unused, XtPointer call)
{
    (void)widget;
    (void)unused;
    (void)call;
    go_primary();
}

static void build_confirm_page(void)
{
    char line[160];
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Confirm Update") : T("Confirm Installation"),
                16, 24);
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Installed ReliefOS packages will be upgraded.")
                             : T("This operation is destructive."),
                48, 24);
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count) {
        format_disk_line(line, sizeof(line), &disks[selected_disk]);
    } else {
        line[0] = 0;
    }
    {
        char target[192];
        snprintf(target, sizeof(target), "%s %s", T("Target:"), line);
        place_label(content, target, 96, 24);
    }
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Alpine packages and local configuration are retained. Boot files are updated after the package transaction succeeds.")
                             : (installer_partition_auto || installer_partition_table_replaced)
                               ? T("The selected disk will be erased and formatted with a FAT32 ESP and ext4 system root.")
                               : T("Only the selected ESP and root partitions will be formatted; other partitions are preserved."),
                136, 24);
    place_label(content, install_mode == INSTALLER_MODE_UPDATE
                             ? T("Type UPDATE to enable the Update button.")
                             : T("Type INSTALL to enable the Install button."),
                184, 24);
    confirm_field = XtVaCreateManagedWidget("confirm", xmTextWidgetClass, content,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 212,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
        XmNwidth, 280,
        XmNvalue, confirm_text, NULL);
    XtAddCallback(confirm_field, XmNvalueChangedCallback, confirm_changed, NULL);
    XtAddCallback(confirm_field, XmNactivateCallback, confirm_activated, NULL);
}

static void build_progress_page(void)
{
    char bar[64];
    place_label(content, mode_progress_title(), 16, 24);
    place_label(content, T("Do not turn off this machine."), 48, 24);
    progress_text_label = place_label(content, progress_text, 96, 24);
    snprintf(bar, sizeof(bar), "[%.*s%.*s] %u%%",
             (int)(progress_value / 5), "####################",
             (int)(20 - progress_value / 5), "....................",
             (unsigned)progress_value);
    progress_bar_label = place_label(content, bar, 136, 24);
}

static void build_finish_page(void)
{
    char target[192];
    if (install_success) {
        place_label(content, install_mode == INSTALLER_MODE_UPDATE
                                 ? T("Update Complete") : T("Installation Complete"),
                    16, 24);
        place_label(content, install_mode == INSTALLER_MODE_UPDATE
                                 ? T("ReliefOS was updated on the selected disk.")
                                 : T("ReliefOS was installed to the selected disk."),
                    48, 24);
        place_label(content, T("Remove the installation media, then restart."), 96, 24);
        if (reboot_error) {
            place_label(content, T("Restart failed"), 136, 24);
            snprintf(target, sizeof(target), "%s", strerror(reboot_error));
            place_label(content, target, 164, 24);
        }
    } else {
        place_label(content, install_mode == INSTALLER_MODE_UPDATE
                                 ? T("Update Failed") : T("Installation Failed"),
                    16, 24);
        place_label(content, T("No writes will continue after this error."), 48, 24);
        place_label(content, status_text, 96, 24);
        place_label(content, detail_text, 128, 24);
    }
}

static void clear_content(void)
{
    WidgetList children;
    Cardinal count = 0;
    XtVaGetValues(content, XmNchildren, &children, XmNnumChildren, &count, NULL);
    /* Copy first: destroying a child mutates the parent's live child list. */
    if (count) {
        Widget *saved = malloc(count * sizeof(Widget));
        if (saved) {
            memcpy(saved, children, count * sizeof(Widget));
            for (Cardinal i = 0; i < count; ++i) {
                XtUnmanageChild(saved[i]);
                XtDestroyWidget(saved[i]);
            }
            free(saved);
        }
    }
    disk_list = NULL;
    partition_list = NULL;
    confirm_field = NULL;
    account_hint = NULL;
    progress_text_label = NULL;
    progress_bar_label = NULL;
    for (int i = 0; i < 5; ++i) {
        account_fields[i] = NULL;
    }
}

static void show_page(void)
{
    clear_content();
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
    update_sidebar();
    update_footer();
}

/* --- keyboard ------------------------------------------------------------ */

/* Xt event handlers do not bubble to ancestors, so every widget that can hold
 * keyboard focus needs its own registration (paint/fileman do the same). */
static void watch_keys(Widget widget)
{
    XtInsertEventHandler(widget, KeyPressMask, False, key, NULL, XtListHead);
}

static void key(Widget widget, XtPointer unused, XEvent *event, Boolean *dispatch)
{
    KeySym symbol;
    (void)widget;
    (void)unused;
    if (event->type != KeyPress) {
        return;
    }
    symbol = XLookupKeysym(&event->xkey, 0);
    if (symbol == XK_Escape) {
        if (installer_model_can_cancel(page, install_success)) {
            close_now();
        }
        *dispatch = False;
        return;
    }
    if (symbol == XK_Return || symbol == XK_KP_Enter) {
        /* Text fields handle Enter through their activate callback. */
        if (widget != confirm_field && widget != account_fields[0] &&
            widget != account_fields[1] && widget != account_fields[2] &&
            widget != account_fields[3] && widget != account_fields[4]) {
            go_primary();
            *dispatch = False;
        }
    }
}

/* The XmNfontList string resource can only describe core fonts, which have no
 * CJK glyphs; route widgets to a CJK-capable Xft rendition instead. */
static XmFontList app_font_list(Widget widget_shell)
{
    Arg args[3];
    XmRendition rendition;
    XtSetArg(args[0], XmNfontName, "SimSun");
    XtSetArg(args[1], XmNfontType, XmFONT_IS_XFT);
    XtSetArg(args[2], XmNloadModel, XmLOAD_IMMEDIATE);
    rendition = XmRenditionCreate(widget_shell, XmFONTLIST_DEFAULT_TAG, args, 3);
    XmFontList list = XmRenderTableAddRenditions(NULL, &rendition, 1, XmDUPLICATE);
    XmRenditionFree(rendition);
    return list;
}

/* --- install progress repaint ------------------------------------------- */

void refresh_ui(void)
{
    if (installer_tty_mode || !shell) {
        return;
    }
    if (progress_text_label) {
        set_label(progress_text_label, progress_text);
    }
    if (progress_bar_label) {
        char bar[64];
        snprintf(bar, sizeof(bar), "[%.*s%.*s] %u%%",
                 (int)(progress_value / 5), "####################",
                 (int)(20 - progress_value / 5), "....................",
                 (unsigned)progress_value);
        set_label(progress_bar_label, bar);
    }
    update_footer();
    while (XtAppPending(app)) {
        XtAppProcessEvent(app, XtIMAll);
    }
    XFlush(XtDisplay(shell));
}

int main(int argc, char **argv)
{
    struct installer_tty_context tty_context;
    int graphical = argc == 2 && strcmp(argv[1], "--graphical") == 0;

    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
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

    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*background: #eceef4", "*foreground: #22242e",
        "*highlightColor: #3b62a6", NULL
    };
    shell = XtVaAppInitialize(&app, "ReliefOSSetup", NULL, 0, &argc, argv,
                              fallback, XtNtitle, "Install ReliefOS",
                              XtNiconName, "ReliefOS Setup",
                              XtNwidth, 1024, XtNheight, 700, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    {
        Display *display = XtDisplay(shell);
        sidebar_bg = alloc_color(display, 0x003B62A6);
        sidebar_bg_active = alloc_color(display, 0x00ECEEF4);
        sidebar_fg = alloc_color(display, 0x00FFFFFF);
        sidebar_fg_active = alloc_color(display, 0x0022242E);
    }
    Widget form = XtVaCreateManagedWidget("installer", xmFormWidgetClass, shell,
        XmNresizePolicy, XmRESIZE_GROW, XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    sidebar = XtVaCreateManagedWidget("sidebar", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM, XmNwidth, SIDEBAR_W,
        XmNbackground, sidebar_bg, XmNresizePolicy, XmRESIZE_NONE, NULL);
    Widget brand = XtVaCreateManagedWidget("brand", xmLabelWidgetClass, sidebar,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 16,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 16,
        XmNwidth, SIDEBAR_W - 32,
        XmNbackground, sidebar_bg, XmNforeground, sidebar_fg,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNrecomputeSize, False, NULL);
    set_label(brand, "ReliefOS");
    Widget brand2 = XtVaCreateManagedWidget("brandSetup", xmLabelWidgetClass, sidebar,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 42,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 16,
        XmNwidth, SIDEBAR_W - 32,
        XmNbackground, sidebar_bg, XmNforeground, sidebar_fg,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNrecomputeSize, False, NULL);
    set_label(brand2, T("Setup"));
    for (int i = 0; i < INSTALLER_PAGE_COUNT; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "step%d", i);
        sidebar_rows[i] = XtVaCreateManagedWidget(name, xmLabelWidgetClass, sidebar,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 96 + i * 28,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 16,
            XmNwidth, SIDEBAR_W - 32,
            XmNbackground, sidebar_bg, XmNforeground, sidebar_fg,
            XmNalignment, XmALIGNMENT_BEGINNING,
            XmNrecomputeSize, False, NULL);
    }
    Widget footer = XtVaCreateManagedWidget("footer", xmFormWidgetClass, form,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, sidebar,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM, XmNheight, FOOTER_H, NULL);
    content = XtVaCreateManagedWidget("content", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, sidebar,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, footer, NULL);
    back_button = XtVaCreateManagedWidget("back", xmPushButtonWidgetClass, footer,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 18,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 24,
        XmNwidth, 132, NULL);
    set_label(back_button, T("Previous Step"));
    XtAddCallback(back_button, XmNactivateCallback, go_back, NULL);
    primary_button = XtVaCreateManagedWidget("next", xmPushButtonWidgetClass, footer,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 18,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, back_button,
        XmNleftOffset, 12, XmNwidth, 132, NULL);
    XtAddCallback(primary_button, XmNactivateCallback, go_next, NULL);
    cancel_button = XtVaCreateManagedWidget("cancel", xmPushButtonWidgetClass, footer,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 18,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 24,
        XmNwidth, 132, NULL);
    set_label(cancel_button, T("Cancel"));
    XtAddCallback(cancel_button, XmNactivateCallback, cancel_setup, NULL);

    watch_keys(shell);
    watch_keys(back_button);
    watch_keys(primary_button);
    watch_keys(cancel_button);
    watch_keys(form);

    show_page();
    XtRealizeWidget(shell);
    {
        Atom delete_window = XInternAtom(XtDisplay(shell), "WM_DELETE_WINDOW", False);
        XmAddWMProtocolCallback(shell, delete_window, cancel_setup, NULL);
    }
    puts("[installer.elf] Motif installer ready");
    fflush(stdout);
    XtAppMainLoop(app);
    return 0;
}
