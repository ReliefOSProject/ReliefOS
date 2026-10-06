#include <reliefos/fs.h>
#include <reliefos/blockdev.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include "../locale_settings.h"
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/system.h>
#include <reliefos/ui.h>
#include <reliefos/inputm.h>
#include <reliefos/psf_font.h>
#include "installer_sha256.h"
#include "installer_tty.h"
#include "installer_directory.h"
#include "installer_setup.h"
#include "../../auth/standard_accounts.h"
#include "installer_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <errno.h>
#include <string.h>

#define INSTALLER_MAX_W 1920
#define INSTALLER_MAX_H 1080
#define INSTALLER_INITIAL_W 1280
#define INSTALLER_INITIAL_H 720
#define SIDEBAR_W 220
#define FOOTER_H 64
#define CONTENT_PAD 34
#define BUTTON_W 84
#define BUTTON_H RELIEFOS_UI_BUTTON_H
#define KEY_ESCAPE 1U
#define KEY_SPACE 57U
#define KEY_UP 72U
#define KEY_DOWN 80U
#define COPY_BUF_SIZE (32U * 1024U)
#define COPY_ESP_WRITE_SLICE 4096U
#define COPY_PRESENT_INTERVAL_MS 50U
#define INSTALLER_EVENT_BATCH_MAX 32U
#define UPDATE_APP_ROW_H 24U
#define UPDATE_APP_MAX RELIEFOS_FS_MAX_ENTRIES
#define POLICY_SCROLLBAR_W 18U
#define POLICY_LINE_TEXT_MAX 256U
#define POLICY_MAX_LINES 192U
#define INSTALLER_CJK_FONT RELIEFOS_PATH_BROWSER_CJK_FONT
#define INSTALL_ROOT_PAYLOAD "/install/root"
#define INSTALL_ESP_PAYLOAD "/install/esp"
#define INSTALL_ROOT_MOUNT "/target"
#define INSTALL_ESP_MOUNT "/target/boot"

/* Target-root path contract.  The installer payload uses the same relative
 * paths as the guest root, so each payload path is INSTALL_ROOT_PAYLOAD plus
 * the matching guest-relative name. */
#define TARGET_RELIEFOS_APPS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_APPS
#define TARGET_RELIEFOS_LIB INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_LIB
#define TARGET_RELIEFOS_DRIVERS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_DRIVERS
#define TARGET_RELIEFOS_DOC INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_DOC
#define TARGET_ETC_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_ETC_RELIEFOS
#define TARGET_VAR_LIB_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS
#define TARGET_VAR_LOG INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_LOG
#define TARGET_VAR_TMP INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_TMP
#define TARGET_FONTS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_FONTS
#define TARGET_RESOURCES INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_RESOURCES
#define TARGET_CERTS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_ETC_SSL_CERTS
#define TARGET_USR_BIN INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_BIN
#define TARGET_USR_SBIN INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_SBIN
#define TARGET_USR_LIB INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_LIB
#define TARGET_USR_SHARE INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_SHARE
#define TARGET_HOME INSTALL_ROOT_MOUNT "/home"
#define TARGET_OPT INSTALL_ROOT_MOUNT "/opt"
#define TARGET_RUN_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RUN_RELIEFOS
#define TARGET_BOOT INSTALL_ROOT_MOUNT "/boot"
#define TARGET_ESP_KERNEL TARGET_BOOT "/reliefos/kernel.sys"
#define TARGET_ESP_KERNEL_LEGACY TARGET_BOOT "/leonos/kernel.sys"
/* The installer must be able to change language before it has a writable
 * target system.  Do not make rendering depend on persisting locale.conf on
 * the installation medium. */
#define T(s) gettext(s)

enum installer_page {
    PAGE_LANGUAGE = 0,
    PAGE_THANKS,
    PAGE_THEME,
    PAGE_WELCOME,
    PAGE_MODE,
    PAGE_DISK,
    PAGE_UPDATE_APPS,
    PAGE_ACCOUNTS,
    PAGE_CONFIRM,
    PAGE_PROGRESS,
    PAGE_FINISH,
    PAGE_COUNT,
};

enum installer_mode {
    INSTALL_MODE_FRESH = 0,
    INSTALL_MODE_UPDATE = 1,
};

enum installer_markdown_line_kind {
    POLICY_LINE_NORMAL = 0,
    POLICY_LINE_H1,
    POLICY_LINE_H2,
    POLICY_LINE_BULLET,
    POLICY_LINE_QUOTE,
    POLICY_LINE_RULE,
};

struct installer_layout {
    uint32_t sidebar_w;
    uint32_t footer_y;
    uint32_t content_x;
    uint32_t content_y;
    uint32_t content_w;
    uint32_t content_h;
    uint32_t table_w;
    uint32_t button_y;
    uint32_t back_x;
    uint32_t next_x;
    uint32_t cancel_x;
    uint32_t disk_refresh_x;
    uint32_t disk_refresh_y;
    uint32_t disk_header_y;
    uint32_t disk_list_y;
    uint32_t disk_list_h;
    uint32_t disk_status_y;
    uint32_t disk_detail_y;
    uint32_t confirm_edit_y;
};

struct installer_markdown_line {
    char text[POLICY_LINE_TEXT_MAX];
    uint8_t kind;
};

struct installer_text_view {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t text_x;
    uint32_t text_w;
    uint32_t checkbox_y;
};

struct update_app_entry {
    char name[RELIEFOS_FS_NAME_LEN];
    char src_package[RELIEFOS_FS_PATH_LEN];
    char dst_package[RELIEFOS_FS_PATH_LEN];
    char src_elf[RELIEFOS_FS_PATH_LEN];
    char dst_elf[RELIEFOS_FS_PATH_LEN];
    char src_icon[RELIEFOS_FS_PATH_LEN];
    char dst_icon[RELIEFOS_FS_PATH_LEN];
    uint8_t selected;
    uint8_t missing;
    uint8_t elf_diff;
    uint8_t icon_diff;
    uint8_t package_diff;
};

static uint32_t pixels[INSTALLER_MAX_W * INSTALLER_MAX_H];
static uint32_t surface_w = INSTALLER_INITIAL_W;
static uint32_t surface_h = INSTALLER_INITIAL_H;
static uint8_t page = PAGE_LANGUAGE;
static uint8_t install_mode = INSTALL_MODE_FRESH;
static uint8_t installer_theme = RELIEFOS_UI_THEME_METRO;
static uint8_t installer_theme_explicit;
static uint32_t acknowledgements_scroll_y;
static struct reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS];
static uint32_t disk_count;
static int32_t selected_disk = -1;
static char confirm_text[16];
static struct reliefos_ui_edit_state confirm_edit;
static struct installer_setup setup;
static struct reliefos_ui_edit_state account_edits[5];
static unsigned account_focus;
static struct update_app_entry update_apps[UPDATE_APP_MAX];
static uint32_t update_app_count;
static struct reliefos_ui_listview_state update_app_list;
static char status_text[128] = "Ready";
static char detail_text[128] = "";
static char progress_text[256] = "Ready";
static uint32_t progress_value;
static uint32_t copy_total;
static uint32_t copy_done;
static uint64_t copy_total_bytes;
static uint64_t copy_done_bytes;
static uint8_t install_success;
static int reboot_error;
static uint8_t install_running;
static uint8_t dirty = 1;
static uint8_t installer_tty_mode;
static uint32_t tty_last_progress = 0xffffffffu;
static char tty_last_status[128];
static uint8_t copy_buf[COPY_BUF_SIZE];
static struct installer_markdown_line policy_lines[POLICY_MAX_LINES];
static uint32_t policy_line_count;

static const char acknowledgements_en[] =
    "# Acknowledgements\n"
    "\n"
    "ReliefOS gratefully acknowledges the creators, contributors, and maintainers of the public resources and open-source projects used by this release.\n"
    "\n"
    "## Runtime, Toolchain, and Applications\n"
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
    "## Fonts and Visual Resources\n"
    "- Noto Sans Mono - SIL Open Font License 1.1\n"
    "- Droid Sans Fallback - Apache License 2.0\n"
    "- Noto Sans CJK - SIL Open Font License 1.1\n"
    "- Microsoft fonts used by the distribution - subject to the applicable Microsoft license terms\n"
    "- NASA Image and Video Library, PIA18033 - used in accordance with NASA Media Usage Guidelines\n"
    "\n"
    "## Recorded Browser Source Dependencies\n"
    "- litehtml - BSD-3-Clause\n"
    "- Gumbo HTML Parser - Apache License 2.0\n"
    "\n"
    "These browser sources are retained for future compatibility work and are not linked into the current browser runtime.\n"
    "Complete license texts and attribution notices are preserved with the corresponding source, SDK, and/or installed program package.\n"
    "\n";

static const char acknowledgements_zh[] =
    "# 感谢\n"
    "\n"
    "ReliefOS 诚挚感谢本发行版所使用的公共资源与开源项目的创作者、贡献者和维护者。\n"
    "\n"
    "## 运行时、开发工具链与应用程序\n"
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
    "## 字体与视觉资源\n"
    "- Noto Sans Mono - SIL Open Font License 1.1\n"
    "- Droid Sans Fallback - Apache License 2.0\n"
    "- Noto Sans CJK - SIL Open Font License 1.1\n"
    "- 发行版使用的 Microsoft 字体 - 受相应 Microsoft 许可条款约束\n"
    "- NASA Image and Video Library，PIA18033 - 遵循 NASA 媒体使用指南\n"
    "\n"
    "## 已记录的浏览器源码依赖\n"
    "- litehtml - BSD-3-Clause\n"
    "- Gumbo HTML Parser - Apache License 2.0\n"
    "\n"
    "这些浏览器源码为后续兼容性工作保留，当前浏览器运行时尚未链接它们。\n"
    "完整许可证文本与归属声明保留在相应源代码、开发套件和/或已安装程序包中。\n"
    "\n";

static int text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int name_is_dot(const char *name)
{
    return text_eq(name, ".") || text_eq(name, "..");
}

static uint32_t text_len(const char *text)
{
    uint32_t len = 0;
    while (text && text[len]) {
        ++len;
    }
    return len;
}

static int text_ends_with(const char *text, const char *suffix)
{
    uint32_t text_n = text_len(text);
    uint32_t suffix_n = text_len(suffix);
    if (!suffix_n || suffix_n > text_n) {
        return 0;
    }
    return text_eq(text + text_n - suffix_n, suffix);
}

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (!buf || !pos || *pos + 1 >= cap) {
        return -1;
    }
    buf[(*pos)++] = ch;
    buf[*pos] = 0;
    return 0;
}

static int append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text)
{
    for (uint32_t i = 0; text && text[i]; ++i) {
        if (append_char(buf, pos, cap, text[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

static int append_u64(char *buf, uint32_t *pos, uint32_t cap, uint64_t value)
{
    char tmp[24];
    uint32_t n = 0;
    if (value == 0) {
        return append_char(buf, pos, cap, '0');
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        if (append_char(buf, pos, cap, tmp[--n]) < 0) {
            return -1;
        }
    }
    return 0;
}

static int append_i32(char *buf, uint32_t *pos, uint32_t cap, int32_t value)
{
    uint32_t mag;
    if (value < 0) {
        if (append_char(buf, pos, cap, '-') < 0) {
            return -1;
        }
        mag = (uint32_t)(-value);
    } else {
        mag = (uint32_t)value;
    }
    return append_u64(buf, pos, cap, mag);
}

static uint32_t policy_utf8_char(const char *text, uint32_t pos, uint32_t len,
                                 uint32_t *out_cells)
{
    uint8_t first;
    uint32_t bytes = 1;
    if (out_cells) {
        *out_cells = 1;
    }
    if (!text || pos >= len || !text[pos]) {
        return 0;
    }
    first = (uint8_t)text[pos];
    if (first >= 0xc2u && first < 0xe0u && pos + 1U < len &&
        (((uint8_t)text[pos + 1U] & 0xc0u) == 0x80u)) {
        bytes = 2;
    } else if (first >= 0xe0u && first < 0xf0u && pos + 2U < len &&
               (((uint8_t)text[pos + 1U] & 0xc0u) == 0x80u) &&
               (((uint8_t)text[pos + 2U] & 0xc0u) == 0x80u)) {
        bytes = 3;
    } else if (first >= 0xf0u && first < 0xf5u && pos + 3U < len &&
               (((uint8_t)text[pos + 1U] & 0xc0u) == 0x80u) &&
               (((uint8_t)text[pos + 2U] & 0xc0u) == 0x80u) &&
               (((uint8_t)text[pos + 3U] & 0xc0u) == 0x80u)) {
        bytes = 4;
    }
    if (out_cells && bytes >= 3U) {
        *out_cells = 2;
    }
    return bytes;
}

static uint32_t policy_text_cells(const char *text)
{
    uint32_t pos = 0;
    uint32_t cells = 0;
    uint32_t len = text_len(text);
    while (pos < len) {
        uint32_t char_cells = 1;
        uint32_t bytes = policy_utf8_char(text, pos, len, &char_cells);
        if (!bytes) {
            break;
        }
        cells += char_cells;
        pos += bytes;
    }
    return cells;
}

static uint32_t policy_utf8_prefix_len(const char *text, uint32_t len,
                                       uint32_t capacity)
{
    uint32_t pos = 0;
    while (pos < len && pos < capacity) {
        uint32_t bytes = policy_utf8_char(text, pos, len, 0);
        if (!bytes || bytes > capacity - pos) {
            break;
        }
        pos += bytes;
    }
    return pos;
}

static void policy_clean_inline(char *dst, uint32_t cap, const char *src)
{
    uint32_t out = 0;
    uint32_t pos = 0;
    uint32_t len = text_len(src);
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    while (pos < len) {
        uint32_t bytes = policy_utf8_char(src, pos, len, 0);
        if (!bytes) {
            break;
        }
        if (bytes == 1U && (src[pos] == '*' || src[pos] == '_' || src[pos] == '`')) {
            ++pos;
            continue;
        }
        if (out + bytes >= cap) {
            break;
        }
        for (uint32_t i = 0; i < bytes; ++i) {
            dst[out++] = src[pos + i];
        }
        dst[out] = 0;
        pos += bytes;
    }
}

static void policy_add_line(uint8_t kind, const char *text)
{
    if (policy_line_count >= POLICY_MAX_LINES) {
        return;
    }
    policy_lines[policy_line_count].kind = kind;
    copy_text(policy_lines[policy_line_count].text,
              sizeof(policy_lines[policy_line_count].text), text ? text : "");
    ++policy_line_count;
}

static int policy_line_is_rule(const char *line)
{
    uint32_t count = 0;
    for (uint32_t i = 0; line && line[i]; ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
            continue;
        }
        if (line[i] != '-') {
            return 0;
        }
        ++count;
    }
    return count >= 3U;
}

static void policy_emit_wrapped(uint8_t kind, const char *prefix,
                                const char *text, uint32_t width)
{
    char line[POLICY_LINE_TEXT_MAX];
    uint32_t out = 0;
    uint32_t pos = 0;
    uint32_t max_cells = reliefos_ui_text_fit_chars(width);
    uint32_t prefix_cells = policy_text_cells(prefix);
    uint32_t cells = prefix_cells;
    uint32_t source_len = text_len(text);
    if (max_cells < 16U) {
        max_cells = 16U;
    }
    line[0] = 0;
    (void)append_text(line, &out, sizeof(line), prefix);
    while (pos < source_len) {
        uint32_t char_cells = 1;
        uint32_t bytes = policy_utf8_char(text, pos, source_len, &char_cells);
        if (!bytes) {
            break;
        }
        if ((text[pos] == ' ' || text[pos] == '\t') && cells == prefix_cells) {
            pos += bytes;
            continue;
        }
        if (text[pos] != ' ' && text[pos] != '\t') {
            uint32_t word_pos = pos;
            uint32_t word_cells = 0;
            while (word_pos < source_len && text[word_pos] != ' ' && text[word_pos] != '\t') {
                uint32_t next_cells = 1;
                uint32_t next_bytes = policy_utf8_char(text, word_pos, source_len,
                                                       &next_cells);
                if (!next_bytes) {
                    break;
                }
                word_cells += next_cells;
                word_pos += next_bytes;
            }
            if (word_cells <= max_cells && cells > prefix_cells &&
                cells + word_cells > max_cells) {
                policy_add_line(kind, line);
                out = 0;
                line[0] = 0;
                if (prefix_cells) {
                    (void)append_text(line, &out, sizeof(line), "  ");
                    cells = 2;
                } else {
                    cells = 0;
                }
                continue;
            }
        }
        if (cells + char_cells > max_cells || out + bytes + 1U >= sizeof(line)) {
            policy_add_line(kind, line);
            out = 0;
            line[0] = 0;
            if (prefix_cells) {
                (void)append_text(line, &out, sizeof(line), "  ");
                cells = 2;
            } else {
                cells = 0;
            }
            if (text[pos] == ' ' || text[pos] == '\t') {
                pos += bytes;
                continue;
            }
        }
        for (uint32_t i = 0; i < bytes && out + 1U < sizeof(line); ++i) {
            line[out++] = text[pos + i];
        }
        line[out] = 0;
        cells += char_cells;
        pos += bytes;
    }
    if (out || prefix_cells) {
        policy_add_line(kind, line);
    }
}

static void markdown_reflow(const char *source, uint32_t width)
{
    uint32_t pos = 0;
    policy_line_count = 0;
    while (source[pos] && policy_line_count < POLICY_MAX_LINES) {
        char raw[512];
        char clean[sizeof(raw)];
        uint32_t line_start = pos;
        uint32_t line_end;
        uint32_t raw_len;
        char *line;
        while (source[pos] && source[pos] != '\n' && source[pos] != '\r') {
            ++pos;
        }
        line_end = pos;
        while (source[pos] == '\n' || source[pos] == '\r') {
            ++pos;
        }
        raw_len = line_end - line_start;
        raw_len = policy_utf8_prefix_len(source + line_start, raw_len,
                                         sizeof(raw) - 1U);
        for (uint32_t i = 0; i < raw_len; ++i) {
            raw[i] = source[line_start + i];
        }
        raw[raw_len] = 0;
        line = raw;
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (!line[0]) {
            policy_add_line(POLICY_LINE_NORMAL, "");
        } else if (line[0] == '#' && line[1] == '#' && line[2] == ' ') {
            policy_clean_inline(clean, sizeof(clean), line + 3);
            policy_emit_wrapped(POLICY_LINE_H2, "", clean, width);
        } else if (line[0] == '#' && line[1] == ' ') {
            policy_clean_inline(clean, sizeof(clean), line + 2);
            policy_emit_wrapped(POLICY_LINE_H1, "", clean, width);
        } else if (policy_line_is_rule(line)) {
            policy_add_line(POLICY_LINE_RULE, "");
        } else if (line[0] == '>' && (line[1] == ' ' || line[1] == '\t')) {
            policy_clean_inline(clean, sizeof(clean), line + 2);
            policy_emit_wrapped(POLICY_LINE_QUOTE, "", clean, width);
        } else if ((line[0] == '-' || line[0] == '*') &&
                   (line[1] == ' ' || line[1] == '\t')) {
            policy_clean_inline(clean, sizeof(clean), line + 2);
            policy_emit_wrapped(POLICY_LINE_BULLET, "- ", clean, width);
        } else {
            uint32_t ordered_end = 0;
            while (line[ordered_end] >= '0' && line[ordered_end] <= '9') {
                ++ordered_end;
            }
            if (ordered_end && line[ordered_end] == '.' && line[ordered_end + 1U] == ' ') {
                char prefix[16];
                uint32_t prefix_len = 0;
                for (uint32_t i = 0; i < ordered_end + 2U && prefix_len + 1U < sizeof(prefix); ++i) {
                    prefix[prefix_len++] = line[i];
                }
                prefix[prefix_len] = 0;
                policy_clean_inline(clean, sizeof(clean), line + ordered_end + 2U);
                policy_emit_wrapped(POLICY_LINE_BULLET, prefix, clean, width);
            } else {
                policy_clean_inline(clean, sizeof(clean), line);
                policy_emit_wrapped(POLICY_LINE_NORMAL, "", clean, width);
            }
        }
    }
}

static void acknowledgements_reflow(uint32_t width)
{
    markdown_reflow(language_selection() == 1
                        ? acknowledgements_zh : acknowledgements_en,
                    width);
}

static uint32_t policy_line_height(uint8_t kind)
{
    if (kind == POLICY_LINE_H1) {
        return 24;
    }
    if (kind == POLICY_LINE_H2) {
        return 21;
    }
    if (kind == POLICY_LINE_RULE) {
        return 12;
    }
    return 18;
}

static uint32_t policy_total_height(void)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < policy_line_count; ++i) {
        total += policy_line_height(policy_lines[i].kind);
    }
    return total;
}

static void copy_replace_extension(char *dst, uint32_t cap,
                                   const char *path, const char *extension)
{
    uint32_t len;
    copy_text(dst, cap, path);
    len = text_len(dst);
    if (!extension || !text_ends_with(dst, ".elf") || len < 4) {
        return;
    }
    dst[len - 4] = 0;
    len -= 4;
    (void)append_text(dst, &len, cap, extension);
}

static void set_status(const char *status, const char *detail)
{
    copy_text(status_text, sizeof(status_text), status);
    copy_text(detail_text, sizeof(detail_text), detail);
    /* The status line is the only account of why an install or update stopped,
     * and it lives in the GUI. Mirror it to the serial console so a failure can
     * be diagnosed from a log instead of a screenshot: without this, a reported
     * "mount failed ret=-22" gives no way to tell which check produced it. */
    printf("[installer.elf] status: %s%s%s\n",
           status ? status : "",
           (detail && detail[0]) ? " - " : "",
           (detail && detail[0]) ? detail : "");
}

static void set_progress_text(const char *status, const char *detail)
{
    uint32_t pos = 0;
    progress_text[0] = 0;
    (void)append_text(progress_text, &pos, sizeof(progress_text), status ? status : "");
    if (detail && detail[0]) {
        (void)append_text(progress_text, &pos, sizeof(progress_text), " - ");
        (void)append_text(progress_text, &pos, sizeof(progress_text), detail);
    }
}

static void set_error_status(const char *prefix, int ret)
{
    uint32_t pos = 0;
    detail_text[0] = 0;
    append_text(detail_text, &pos, sizeof(detail_text), prefix);
    append_text(detail_text, &pos, sizeof(detail_text), " ret=");
    append_i32(detail_text, &pos, sizeof(detail_text), ret);
    copy_text(status_text, sizeof(status_text), T("Installation failed"));
}

static int hit_rect_i(int32_t x, int32_t y, int32_t rx, int32_t ry,
                      int32_t rw, int32_t rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void update_surface_size(uint32_t width, uint32_t height)
{
    if (width == 0 || width > INSTALLER_MAX_W) {
        width = INSTALLER_MAX_W;
    }
    if (height == 0 || height > INSTALLER_MAX_H) {
        height = INSTALLER_MAX_H;
    }
    surface_w = width;
    surface_h = height;
}

static void update_surface_size_from_framebuffer(void)
{
    struct reliefos_fb_info fb;
    if (reliefos_fb_info(&fb) >= 0) {
        update_surface_size(fb.width, fb.height);
    }
}

static struct installer_layout get_layout(void)
{
    struct installer_layout l;
    l.sidebar_w = surface_w > 900 ? SIDEBAR_W : 180;
    if (l.sidebar_w + CONTENT_PAD * 2 + 360 > surface_w) {
        l.sidebar_w = surface_w > 520 ? 160 : 0;
    }
    l.footer_y = surface_h > FOOTER_H ? surface_h - FOOTER_H : 0;
    l.content_x = l.sidebar_w + CONTENT_PAD;
    l.content_y = surface_h > 640 ? 64 : 44;
    l.content_w = surface_w > l.content_x + CONTENT_PAD ? surface_w - l.content_x - CONTENT_PAD : surface_w;
    if (l.content_w < 320 && surface_w > CONTENT_PAD * 2) {
        l.content_x = CONTENT_PAD;
        l.content_w = surface_w - CONTENT_PAD * 2;
    }
    l.content_h = l.footer_y > l.content_y + 20 ? l.footer_y - l.content_y - 20 : 120;
    l.table_w = l.content_w;
    if (l.table_w > 1120) {
        l.table_w = 1120;
    }
    l.button_y = l.footer_y + 20;
    if (l.button_y + BUTTON_H + 10 > surface_h) {
        l.button_y = surface_h > BUTTON_H + 12 ? surface_h - BUTTON_H - 12 : 0;
    }
    l.cancel_x = surface_w > BUTTON_W + 28 ? surface_w - BUTTON_W - 28 : 0;
    l.next_x = l.cancel_x > BUTTON_W + 10 ? l.cancel_x - BUTTON_W - 10 : 0;
    l.back_x = l.next_x > BUTTON_W + 10 ? l.next_x - BUTTON_W - 10 : 0;
    l.disk_refresh_y = l.content_y + 44;
    l.disk_refresh_x = l.content_x + l.table_w > 92 ? l.content_x + l.table_w - 92 : l.content_x;
    l.disk_header_y = l.content_y + 84;
    l.disk_list_y = l.disk_header_y + 24;
    l.disk_list_h = l.content_h > 210 ? l.content_h - 146 : 150;
    if (l.disk_list_h > 360) {
        l.disk_list_h = 360;
    }
    l.disk_status_y = l.disk_list_y + l.disk_list_h + 16;
    l.disk_detail_y = l.disk_status_y + 32;
    l.confirm_edit_y = l.content_y + 200;
    return l;
}

static struct installer_text_view get_acknowledgements_view(void)
{
    struct installer_layout l = get_layout();
    struct installer_text_view view;
    view.x = l.content_x;
    view.y = l.content_y + 56;
    view.w = l.table_w;
    view.h = l.footer_y > view.y + 12 ? l.footer_y - view.y - 12 : 64;
    view.text_x = view.x + 10;
    view.text_w = view.w > POLICY_SCROLLBAR_W + 30
                      ? view.w - POLICY_SCROLLBAR_W - 30
                      : 80;
    view.checkbox_y = 0;
    return view;
}

static int path_join(char *dst, uint32_t cap, const char *base, const char *name)
{
    uint32_t pos = 0;
    if (!dst || cap == 0 || !base || !name) {
        return -1;
    }
    dst[0] = 0;
    if (append_text(dst, &pos, cap, base) < 0) {
        return -1;
    }
    if (pos > 0 && dst[pos - 1] != '/') {
        if (append_char(dst, &pos, cap, '/') < 0) {
            return -1;
        }
    }
    while (*name == '/') ++name;
    return append_text(dst, &pos, cap, name);
}

static const char *mode_action_text(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? T("Update") : T("Install");
}

static const char *mode_progress_title(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? T("Updating ReliefOS")
                                               : T("Installing ReliefOS");
}

static void set_disk_select_status(void)
{
    if (install_mode == INSTALL_MODE_UPDATE) {
        set_status(T("Select the disk to update"),
                   T("Setup will check for an existing ReliefOS system."));
    } else {
        set_status(T("Select the target disk"),
                   T("The selected disk will be erased."));
    }
}

static void reset_update_app_list(void)
{
    update_app_count = 0;
    reliefos_ui_listview_state_set_count(&update_app_list, 0);
    update_app_list.selected = -1;
    update_app_list.scroll = 0;
}

static void format_disk_line(char *buf, uint32_t cap,
                             const struct reliefos_block_disk_info *disk)
{
    uint32_t pos = 0;
    uint64_t mib = 0;
    if (!disk) {
        copy_text(buf, cap, "");
        return;
    }
    if (disk->sector_size) {
        mib = (disk->sector_count * (uint64_t)disk->sector_size) / (1024ULL * 1024ULL);
    }
    buf[0] = 0;
    append_text(buf, &pos, cap, "Disk ");
    append_u64(buf, &pos, cap, disk->id);
    append_text(buf, &pos, cap, "  ");
    append_text(buf, &pos, cap, disk->name[0] ? disk->name : "Disk");
    append_text(buf, &pos, cap, "  ");
    if (mib >= 1024) {
        append_u64(buf, &pos, cap, mib / 1024);
        append_text(buf, &pos, cap, " GiB");
    } else {
        append_u64(buf, &pos, cap, mib);
        append_text(buf, &pos, cap, " MiB");
    }
}

static void reset_confirm(void)
{
    confirm_text[0] = 0;
    reliefos_ui_edit_state_init(&confirm_edit, confirm_text, sizeof(confirm_text));
    confirm_edit.focused = 1;
}

static char installer_root_uuid[37], installer_esp_uuid[37];

/* The feature profile is stated explicitly instead of inheriting mke2fs
 * defaults: it must match the images built by tools/build/images.sh, and it
 * must stay inside the kernel's accepted mask (extents, 64bit, flex_bg,
 * filetype, csum_seed incompat; metadata_csum/extra_isize/dir_nlink/... ro). */
#define INSTALLER_MKFS_EXT4_FEATURES \
    "none,filetype,extents,dir_index,metadata_csum,64bit,flex_bg," \
    "has_journal,large_file,huge_file,extra_isize"

static int installer_format_ext4(const char *path)
{
    char *const argv[] = { (char *)"mkfs.ext4", (char *)"-F",
                           (char *)"-b", (char *)"4096",
                           (char *)"-I", (char *)"256",
                           (char *)"-O", (char *)INSTALLER_MKFS_EXT4_FEATURES,
                           (char *)path, NULL };
    pid_t child;
    int status;
    int ret = posix_spawnp(&child, "mkfs.ext4", NULL, NULL, argv, environ);
    if (ret) return -ret;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -EIO;
    return 0;
}

/* Commit fstab only after the root payload is copied; failure aborts install. */
static int installer_write_fstab(void)
{
    char text[320], temporary[] = INSTALL_ROOT_MOUNT "/etc/.fstab.XXXXXX";
    if (!installer_root_uuid[0] || !installer_esp_uuid[0]) return -EINVAL;
    int length = snprintf(text, sizeof(text),
        "# <source> <mountpoint> <type> <options> <dump> <pass>\n"
        "/dev/disk/by-partuuid/%s / ext4 defaults 0 1\n"
        "/dev/disk/by-partuuid/%s /boot vfat defaults 0 2\n",
        installer_root_uuid, installer_esp_uuid);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    int fd = mkstemp(temporary), ret = 0;
    if (fd < 0) return -errno;
    for (int done = 0; done < length;) {
        ssize_t count = write(fd, text + done, (size_t)(length - done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ret = count < 0 ? -errno : -EIO; break; }
        done += (int)count;
    }
    if (!ret && fchown(fd, 0, 0) < 0) ret = -errno;
    if (!ret && fchmod(fd, 0644) < 0) ret = -errno;
    if (!ret && fsync(fd) < 0) ret = -errno;
    if (close(fd) < 0 && !ret) ret = -errno;
    if (!ret && rename(temporary, INSTALL_ROOT_MOUNT "/etc/fstab") < 0) ret = -errno;
    if (ret < 0) (void)unlink(temporary);
    return ret;
}

static int installer_target_partitions(const char *disk_path, int fresh,
                                       char *esp_path, uint32_t esp_cap,
                                       char *root_path, uint32_t root_cap,
                                       uint32_t *root_filesystem)
{
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0, esp = UINT32_MAX, root = UINT32_MAX;
    int ret;
    if (!disk_path || !esp_path || !root_path || !root_filesystem) return -EINVAL;
    *root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    if (fresh) {
        struct reliefos_block_disk_info info;
        uint32_t root_mib;
        ret = reliefos_block_get_info(disk_path, &info);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_initialize(disk_path, 1);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_create(disk_path, RELIEFOS_BLOCK_FILESYSTEM_FAT32,
                                      128, "RELIEFOS_ESP", &esp);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_set_type(disk_path, esp, RELIEFOS_BLOCK_GPT_ESP);
        if (ret < 0) return ret;
        root_mib = (uint32_t)((info.sector_count * info.sector_size) / (1024ULL * 1024ULL));
        /* Match main's install_write_gpt layout: a fixed 128 MiB ESP, then
         * the ext4 root consumes the remaining usable GPT area. The extra
         * 3 MiB reserve covers the primary/backup GPT and 1 MiB alignment
         * slop after the ESP. */
        if (root_mib > 256u) root_mib -= 131u; else root_mib = 64u;
        ret = reliefos_block_gpt_create(disk_path, RELIEFOS_BLOCK_FILESYSTEM_EXT4,
                                      root_mib, "RELIEFOS_ROOT", &root);
        if (ret < 0) return ret;
        ret = reliefos_block_gpt_set_type(disk_path, root, RELIEFOS_BLOCK_GPT_LINUX);
        if (ret < 0) return ret;
        /* Format through the partition nodes after the GPT reread. */
        ret = reliefos_block_partition_path(disk_path, esp, esp_path, esp_cap);
        if (ret < 0) return ret;
        ret = reliefos_block_partition_path(disk_path, root, root_path, root_cap);
        if (ret < 0) return ret;
        ret = reliefos_block_format(esp_path, RELIEFOS_BLOCK_FILESYSTEM_FAT32, "RELIEFOS");
        if (ret < 0) return ret;
        *root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4;
        ret = installer_format_ext4(root_path);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, root, installer_root_uuid);
        if (!ret) ret = reliefos_block_partition_uuid(disk_path, esp, installer_esp_uuid);
        return ret;
    }
    ret = reliefos_block_list_partitions(disk_path, parts, RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    printf("[installer.elf] block list partitions ret=%d count=%u disk=%s\n",
           ret, count, disk_path ? disk_path : "?");
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count && i < RELIEFOS_BLOCK_MAX_PARTITIONS; ++i) {
        printf("[installer.elf] partition[%u] path=%s fs=%u gpt_type=%u\n",
               i, parts[i].path, parts[i].filesystem, parts[i].gpt_type);
        if (parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 && esp == UINT32_MAX) esp = i;
        if ((parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
             parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ||
             parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) && root == UINT32_MAX) root = i;
    }
    if (esp == UINT32_MAX || root == UINT32_MAX) {
        printf("[installer.elf] block partitions missing esp=%u root=%u\n", esp, root);
        return -ENOENT;
    }
    copy_text(esp_path, esp_cap, parts[esp].path);
    copy_text(root_path, root_cap, parts[root].path);
    *root_filesystem = parts[root].filesystem;
    return 0;
}

static uint32_t installer_root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;

static int installer_mount_targets(const char *disk_path, int fresh)
{
    char esp_path[RELIEFOS_BLOCK_PATH_LEN], root_path[RELIEFOS_BLOCK_PATH_LEN];
    uint32_t root_filesystem = RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN;
    const char *root_fs_name = NULL;
    struct stat mountpoint;
    int ret;
    /* /target belongs to the installer runtime, not the installed rootfs. */
    if (mkdir(INSTALL_ROOT_MOUNT, 0755) < 0 && errno != EEXIST) {
        ret = -errno;
        printf("[installer.elf] mountpoint mkdir failed path=%s ret=%d\n", INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    if (lstat(INSTALL_ROOT_MOUNT, &mountpoint) < 0) {
        ret = -errno;
        printf("[installer.elf] mountpoint lstat failed path=%s ret=%d\n", INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    if (!S_ISDIR(mountpoint.st_mode)) {
        printf("[installer.elf] mountpoint is not a directory path=%s\n", INSTALL_ROOT_MOUNT);
        return -ENOTDIR;
    }
    /* Update inspection leaves these mounted. Release children first, before
     * fresh installation rewrites the GPT or formats either filesystem. */
    const char *targets[] = {INSTALL_ESP_MOUNT, INSTALL_ROOT_MOUNT};
    for (uint32_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (umount2(targets[i], 0) < 0 && errno != ENOENT && errno != EINVAL) {
            ret = -errno;
            printf("[installer.elf] unmount target failed path=%s ret=%d\n", targets[i], ret);
            return ret;
        }
    }
    ret = installer_target_partitions(disk_path, fresh, esp_path, sizeof(esp_path),
                                          root_path, sizeof(root_path),
                                          &root_filesystem);
    if (ret < 0) {
        printf("[installer.elf] mount target partitions failed ret=%d disk=%s fresh=%d\n",
               ret, disk_path ? disk_path : "?", fresh);
        return ret;
    }
    installer_root_filesystem = root_filesystem;
    printf("[installer.elf] mount targets disk=%s root=%s esp=%s fresh=%d root_fs=%u\n",
           disk_path ? disk_path : "?", root_path, esp_path, fresh,
           root_filesystem);
    if (root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) {
        root_fs_name = "exfat";
    } else if (root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ||
               root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4) {
        root_fs_name = root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ? "ext4" : "ext2";
    } else {
        /* An unknown root filesystem must not reach mount() with a null type. */
        printf("[installer.elf] unsupported root filesystem fs=%u\n", root_filesystem);
        return -ENODEV;
    }
    if (mount(root_path, INSTALL_ROOT_MOUNT, root_fs_name, 0, NULL) < 0) {
        printf("[installer.elf] mount root failed path=%s errno=%d\n", root_path, errno);
        return -errno;
    }
    if (mkdir(INSTALL_ESP_MOUNT, 0755) < 0 && errno != EEXIST) {
        ret = -errno;
        printf("[installer.elf] mount esp mkdir failed path=%s errno=%d\n",
               INSTALL_ESP_MOUNT, errno);
        (void)umount2(INSTALL_ROOT_MOUNT, 0);
        return ret;
    }
    if (mount(esp_path, INSTALL_ESP_MOUNT, "fat32", 0, NULL) < 0) {
        ret = -errno;
        printf("[installer.elf] mount esp failed path=%s errno=%d\n", esp_path, errno);
        (void)umount2(INSTALL_ROOT_MOUNT, 0);
        return ret;
    }
    return 0;
}

static int confirmation_ok(void)
{
    return install_mode == INSTALL_MODE_UPDATE ? text_eq(confirm_text, "UPDATE")
                                               : text_eq(confirm_text, "INSTALL");
}

static void refresh_disks(void)
{
    uint32_t count = 0;
    int ret = reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count);
    if (ret < 0) {
        disk_count = 0;
        selected_disk = -1;
        set_error_status("Could not list disks", ret);
        dirty = 1;
        return;
    }
    disk_count = count > RELIEFOS_BLOCK_MAX_DISKS ? RELIEFOS_BLOCK_MAX_DISKS : count;
    if (disk_count == 0) {
        selected_disk = -1;
        set_status(T("No disks were found"), T("Attach a disk and click Refresh."));
    } else {
        if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
            selected_disk = 0;
        }
        set_disk_select_status();
    }
    dirty = 1;
}

static void draw_sidebar(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    if (!l.sidebar_w) {
        return;
    }
    reliefos_ui_rect(ui, 0, 0, l.sidebar_w, surface_h, RELIEFOS_UI_ACTIVE_TITLE);
    reliefos_ui_text(ui, 18, 24, "ReliefOS", RELIEFOS_UI_WHITE, RELIEFOS_UI_ACTIVE_TITLE);
    reliefos_ui_text(ui, 18, 48, T("Setup"), RELIEFOS_UI_WHITE, RELIEFOS_UI_ACTIVE_TITLE);
    uint32_t row = 0;
    for (uint32_t i = 0; i < PAGE_COUNT; ++i) {
        if (i == PAGE_UPDATE_APPS && install_mode != INSTALL_MODE_UPDATE) continue;
        if (i == PAGE_ACCOUNTS && install_mode == INSTALL_MODE_UPDATE) continue;
        uint32_t spacing = surface_h >= 600 ? 34 : 26;
        uint32_t y = 104 + row++ * spacing;
        uint32_t fg = i == page ? RELIEFOS_UI_BLACK : RELIEFOS_UI_WHITE;
        uint32_t bg = i == page ? RELIEFOS_UI_LIGHT : RELIEFOS_UI_ACTIVE_TITLE;
        if (i == page) {
            reliefos_ui_rect(ui, 12, y - 6, l.sidebar_w > 34 ? l.sidebar_w - 34 : l.sidebar_w, 24, bg);
        }
        const char *label = "";
        if (i == PAGE_LANGUAGE) {
            label = T("Language");
        } else if (i == PAGE_THANKS) {
            label = T("Thanks");
        } else if (i == PAGE_THEME) {
            label = T("Style");
        } else if (i == PAGE_WELCOME) {
            label = T("Welcome");
        } else if (i == PAGE_MODE) {
            label = T("Mode");
        } else if (i == PAGE_DISK) {
            label = T("Disk");
        } else if (i == PAGE_UPDATE_APPS) {
            label = T("Apps");
        } else if (i == PAGE_ACCOUNTS) {
            label = T("Accounts");
        } else if (i == PAGE_CONFIRM) {
            label = T("Confirm");
        } else if (i == PAGE_PROGRESS) {
            label = mode_action_text();
        } else if (i == PAGE_FINISH) {
            label = T("Finish");
        }
        reliefos_ui_text(ui, 20, y, label, fg, bg);
    }
}

static void draw_title(struct reliefos_ui_surface *ui, const char *title,
                       const char *subtitle)
{
    struct installer_layout l = get_layout();
    reliefos_ui_text(ui, l.content_x, l.content_y, title, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    if (subtitle) {
        reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 26, l.content_w,
                               subtitle, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    }
}

static uint32_t primary_disabled(void)
{
    if (page == PAGE_ACCOUNTS) return !installer_setup_valid(&setup);
    if (install_running) {
        return 1;
    }
    if (page == PAGE_DISK) {
        return selected_disk < 0 || (uint32_t)selected_disk >= disk_count;
    }
    if (page == PAGE_CONFIRM) {
        return !confirmation_ok();
    }
    return 0;
}

static const char *primary_label(void)
{
    if (page == PAGE_CONFIRM) {
        return install_mode == INSTALL_MODE_UPDATE ? T("Update")
                                                   : T("Install");
    }
    if (page == PAGE_FINISH && install_success) {
        return T("Restart");
    }
    if (page == PAGE_FINISH) {
        return T("Close");
    }
    return T("Next");
}

static void draw_footer(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    uint32_t back_disabled = page == PAGE_LANGUAGE || page == PAGE_PROGRESS ||
                             (page == PAGE_FINISH && install_success);
    uint32_t cancel_disabled = page == PAGE_PROGRESS ||
                               (page == PAGE_FINISH && install_success);
    reliefos_ui_rect(ui, l.sidebar_w, l.footer_y, surface_w > l.sidebar_w ? surface_w - l.sidebar_w : surface_w, 1, RELIEFOS_UI_DARK);
    reliefos_ui_rect(ui, l.sidebar_w, l.footer_y + 1, surface_w > l.sidebar_w ? surface_w - l.sidebar_w : surface_w, surface_h > l.footer_y + 1 ? surface_h - l.footer_y - 1 : 0, RELIEFOS_UI_GRAY);
    reliefos_ui_button(ui, l.back_x, l.button_y, BUTTON_W, BUTTON_H, T("Previous Step"),
                     back_disabled ? RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_button(ui, l.next_x, l.button_y, BUTTON_W, BUTTON_H, primary_label(),
                     primary_disabled() ? RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_button(ui, l.cancel_x, l.button_y, BUTTON_W, BUTTON_H, T("Cancel"),
                     cancel_disabled ? RELIEFOS_UI_BUTTON_DISABLED : 0);
}

static void draw_language_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    draw_title(ui, T("Select Language"), T("Choose the language for Setup and the installed system."));
    reliefos_ui_button(ui, l.content_x, l.content_y + 88, 140, BUTTON_H, "English",
                     language_selection() == 0 ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_button(ui, l.content_x + 156, l.content_y + 88, 140, BUTTON_H, "中文",
                     language_selection() == 1 ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_text(ui, l.content_x, l.content_y + 140,
                   T("The installed system will use the same language."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_acknowledgements_page(struct reliefos_ui_surface *ui)
{
    struct installer_text_view view = get_acknowledgements_view();
    uint32_t total_h;
    uint32_t max_scroll;
    uint32_t offset = 0;
    draw_title(ui, T("Thank You"),
               T("Acknowledgements for public resources and open-source projects."));
    acknowledgements_reflow(view.text_w);
    total_h = policy_total_height();
    max_scroll = total_h > view.h ? total_h - view.h : 0;
    if (acknowledgements_scroll_y > max_scroll) {
        acknowledgements_scroll_y = max_scroll;
    }
    reliefos_ui_scroll_view_frame(ui, view.x, view.y, view.w, view.h);
    for (uint32_t i = 0; i < policy_line_count; ++i) {
        uint32_t line_h = policy_line_height(policy_lines[i].kind);
        int32_t line_y = (int32_t)view.y + (int32_t)offset -
                         (int32_t)acknowledgements_scroll_y;
        if (line_y >= (int32_t)view.y &&
            line_y + (int32_t)line_h <= (int32_t)(view.y + view.h)) {
            if (policy_lines[i].kind == POLICY_LINE_H1) {
                reliefos_ui_text_resized_clipped(ui, view.text_x, (uint32_t)line_y,
                                                view.text_w, policy_lines[i].text,
                                                RELIEFOS_UI_ACTIVE_TITLE, RELIEFOS_UI_WHITE, 9, 18);
            } else if (policy_lines[i].kind == POLICY_LINE_H2) {
                reliefos_ui_text_resized_clipped(ui, view.text_x, (uint32_t)line_y,
                                                view.text_w, policy_lines[i].text,
                                                RELIEFOS_UI_ACTIVE_TITLE, RELIEFOS_UI_WHITE, 9, 17);
            } else if (policy_lines[i].kind == POLICY_LINE_RULE) {
                reliefos_ui_rect(ui, view.text_x, (uint32_t)line_y + 5,
                               view.text_w, 1, RELIEFOS_UI_DARK);
            } else if (policy_lines[i].kind == POLICY_LINE_QUOTE) {
                reliefos_ui_rect(ui, view.text_x, (uint32_t)line_y + 1, 3,
                               line_h > 2 ? line_h - 2 : line_h, RELIEFOS_UI_ACTIVE_TITLE);
                reliefos_ui_text_clipped(ui, view.text_x + 9, (uint32_t)line_y,
                                       view.text_w > 9 ? view.text_w - 9 : view.text_w,
                                       policy_lines[i].text, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
            } else {
                reliefos_ui_text_clipped(ui, view.text_x, (uint32_t)line_y,
                                       view.text_w, policy_lines[i].text,
                                       policy_lines[i].kind == POLICY_LINE_BULLET
                                           ? RELIEFOS_UI_BLACK : RELIEFOS_UI_DARK,
                                       RELIEFOS_UI_WHITE);
            }
        }
        offset += line_h;
    }
    reliefos_ui_vscrollbar(ui, view.x + view.w - POLICY_SCROLLBAR_W, view.y,
                         POLICY_SCROLLBAR_W, view.h, acknowledgements_scroll_y,
                         total_h > view.h ? total_h : view.h, view.h,
                         total_h <= view.h ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);
}

static void draw_theme_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    draw_title(ui, T("Choose UI Style"),
               T("Preview a style now and apply it to the installed system."));
    reliefos_ui_button(ui, l.content_x, l.content_y + 88, 140, BUTTON_H, "Metro",
                     installer_theme == RELIEFOS_UI_THEME_METRO
                         ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_button(ui, l.content_x + 156, l.content_y + 88, 140, BUTTON_H, "Win95",
                     installer_theme == RELIEFOS_UI_THEME_WIN95
                         ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_text(ui, l.content_x, l.content_y + 140,
                   installer_theme == RELIEFOS_UI_THEME_METRO
                       ? T("Metro uses the modern flat system appearance.")
                       : T("Win95 keeps the classic beveled system appearance."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_welcome(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    draw_title(ui, T("ReliefOS Setup"), T("Install a new system or update an existing ReliefOS disk."));
    reliefos_ui_text(ui, l.content_x, l.content_y + 84, T("Setup can copy the full normal system payload"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, l.content_x, l.content_y + 108, T("or replace the boot/reliefos and system files on an existing installation."), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, l.content_x, l.content_y + 164, T("SATA/AHCI and IDE/PATA target disks are supported."), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_mode_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    uint32_t card_w = l.content_w > 620 ? 280 : l.content_w;
    draw_title(ui, T("Choose Setup Mode"), T("Fresh install erases the disk. Update keeps existing users and extra programs."));
    reliefos_ui_button(ui, l.content_x, l.content_y + 84, card_w, BUTTON_H,
                     T("Fresh Install"),
                     install_mode == INSTALL_MODE_FRESH ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 122, card_w,
                           T("Format the selected disk and copy a clean ReliefOS system."),
                           RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_button(ui, l.content_x, l.content_y + 180, card_w, BUTTON_H,
                     T("Update Existing System"),
                     install_mode == INSTALL_MODE_UPDATE ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 218, l.content_w,
                           T("Replace boot, system, EFI and bundled docs. Then choose changed or missing system apps."),
                           RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_disk_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    char line[128];
    draw_title(ui,
               install_mode == INSTALL_MODE_UPDATE ? T("Select Disk to Update")
                                                   : T("Select Installation Disk"),
               install_mode == INSTALL_MODE_UPDATE ? T("Choose the disk that already contains ReliefOS.")
                                                   : T("Choose the disk that will receive ReliefOS."));
    reliefos_ui_button(ui, l.disk_refresh_x, l.disk_refresh_y, 92, BUTTON_H, T("Refresh"), 0);
    reliefos_ui_list_header(ui, l.content_x, l.disk_header_y, l.table_w, T("Available disks"));
    reliefos_ui_inset(ui, l.content_x, l.disk_list_y, l.table_w, l.disk_list_h, RELIEFOS_UI_WHITE);
    if (disk_count == 0) {
        reliefos_ui_text(ui, l.content_x + 12, l.disk_list_y + 20, T("No disks were found."), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    }
    for (uint32_t i = 0; i < disk_count && i < RELIEFOS_BLOCK_MAX_DISKS; ++i) {
        uint32_t row_y = l.disk_list_y + 2 + i * 24;
        if (row_y + 22 > l.disk_list_y + l.disk_list_h) {
            break;
        }
        format_disk_line(line, sizeof(line), &disks[i]);
        reliefos_ui_list_row(ui, l.content_x + 2, row_y,
                           l.table_w > 4 ? l.table_w - 4 : l.table_w, line,
                           selected_disk == (int32_t)i ? RELIEFOS_UI_MENU_SELECTED : 0);
    }
    reliefos_ui_inset(ui, l.content_x, l.disk_status_y, l.table_w, 26, RELIEFOS_UI_LIGHT);
    reliefos_ui_text_clipped(ui, l.content_x + 8, l.disk_status_y + 5,
                           l.table_w > 16 ? l.table_w - 16 : l.table_w,
                           status_text, RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
    reliefos_ui_text_clipped(ui, l.content_x, l.disk_detail_y, l.table_w, detail_text, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void format_update_reason(char *buf, uint32_t cap,
                                 const struct update_app_entry *entry)
{
    uint32_t pos = 0;
    buf[0] = 0;
    if (!entry) {
        return;
    }
    append_text(buf, &pos, cap, entry->name);
    append_text(buf, &pos, cap, "  -  ");
    if (entry->missing) {
        append_text(buf, &pos, cap, T("missing on target"));
    } else if (entry->elf_diff && entry->icon_diff) {
        append_text(buf, &pos, cap, T("program and icon differ"));
    } else if (entry->elf_diff) {
        append_text(buf, &pos, cap, T("program differs"));
    } else if (entry->icon_diff) {
        append_text(buf, &pos, cap, T("icon differs"));
    } else if (entry->package_diff) {
        append_text(buf, &pos, cap, T("package files differ"));
    } else {
        append_text(buf, &pos, cap, T("will be replaced"));
    }
}

static void sync_update_list_layout(uint32_t list_h)
{
    uint32_t visible_rows = list_h / UPDATE_APP_ROW_H;
    if (visible_rows == 0) {
        visible_rows = 1;
    }
    update_app_list.visible_rows = visible_rows;
    update_app_list.row_height = UPDATE_APP_ROW_H;
    reliefos_ui_listview_state_set_count(&update_app_list, update_app_count);
}

static void draw_update_apps_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    uint32_t header_y = l.content_y + 112;
    uint32_t list_y = header_y + 24;
    uint32_t list_h = l.content_h > 220 ? l.content_h - 178 : 120;
    uint32_t list_w = l.table_w > 22 ? l.table_w - 22 : l.table_w;
    char line[160];
    sync_update_list_layout(list_h);
    draw_title(ui, T("Program and Driver Updates"),
               T("Changed or missing programs are selected; changed drivers are refreshed automatically."));
    reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 72, l.content_w,
                           T("boot, libraries, kerneldebug, EFI, docs, and drivers will be refreshed. Extra target applications and drivers are kept."),
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_list_header(ui, l.content_x, header_y, list_w,
                          T("Programs to replace"));
    reliefos_ui_inset(ui, l.content_x, list_y, list_w, list_h, RELIEFOS_UI_WHITE);
    if (update_app_count == 0) {
        reliefos_ui_text(ui, l.content_x + 12, list_y + 20,
                       T("No program package differences were found."),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    }
    for (uint32_t row = 0; row < update_app_list.visible_rows; ++row) {
        uint32_t i = update_app_list.scroll + row;
        uint32_t row_y = list_y + 2 + row * UPDATE_APP_ROW_H;
        uint32_t selected = update_app_list.selected == (int32_t)i;
        uint32_t bg = selected ? RELIEFOS_UI_ACTIVE_TITLE : RELIEFOS_UI_WHITE;
        uint32_t fg = selected ? RELIEFOS_UI_WHITE : RELIEFOS_UI_BLACK;
        if (i >= update_app_count || row_y + UPDATE_APP_ROW_H > list_y + list_h) {
            break;
        }
        reliefos_ui_rect(ui, l.content_x + 2, row_y,
                       list_w > 4 ? list_w - 4 : list_w, UPDATE_APP_ROW_H, bg);
        reliefos_ui_checkbox(ui, l.content_x + 8, row_y + 3, "",
                           update_apps[i].selected, 0);
        format_update_reason(line, sizeof(line), &update_apps[i]);
        reliefos_ui_text_clipped(ui, l.content_x + 34, row_y + 5,
                               list_w > 42 ? list_w - 42 : list_w,
                               line, fg, bg);
    }
    reliefos_ui_vscrollbar(ui, l.content_x + list_w, list_y, 18, list_h,
                         update_app_list.scroll,
                         update_app_count > update_app_list.visible_rows
                             ? update_app_count : update_app_list.visible_rows,
                         update_app_list.visible_rows,
                         update_app_count <= update_app_list.visible_rows
                             ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);
    reliefos_ui_text_clipped(ui, l.content_x, list_y + list_h + 14, l.content_w,
                           status_text, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_confirm_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    char line[128];
    draw_title(ui,
               install_mode == INSTALL_MODE_UPDATE ? T("Confirm Update")
                                                   : T("Confirm Installation"),
               install_mode == INSTALL_MODE_UPDATE ? T("Installed ReliefOS packages will be upgraded.")
                                                   : T("This operation is destructive."));
    if (selected_disk >= 0 && (uint32_t)selected_disk < disk_count) {
        format_disk_line(line, sizeof(line), &disks[selected_disk]);
        reliefos_ui_text(ui, l.content_x, l.content_y + 78, T("Target:"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        reliefos_ui_text_clipped(ui, l.content_x + 70, l.content_y + 78,
                               l.content_w > 70 ? l.content_w - 70 : l.content_w,
                               line, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    }
    reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 130, l.content_w,
                           install_mode == INSTALL_MODE_UPDATE
                       ? T("Alpine packages and local configuration are retained. Boot files are updated after the package transaction succeeds.")
                               : T("The selected disk will be erased and formatted with a FAT32 ESP and ext4 system root."),
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, l.content_x, l.content_y + 174,
                   install_mode == INSTALL_MODE_UPDATE
                       ? T("Type UPDATE to enable the Update button.")
                       : T("Type INSTALL to enable the Install button."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_edit_state_draw(ui, l.content_x, l.confirm_edit_y, 220, &confirm_edit, 0);
}

static void draw_progress_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    draw_title(ui, mode_progress_title(), T("Do not turn off this machine."));
    reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 94, l.content_w,
                           progress_text, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_progress(ui, l.content_x, l.content_y + 130, l.content_w, 24, progress_value, 100);
}

static void draw_finish_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    if (install_success) {
        draw_title(ui,
                   install_mode == INSTALL_MODE_UPDATE ? T("Update Complete")
                                                       : T("Installation Complete"),
                   install_mode == INSTALL_MODE_UPDATE ? T("ReliefOS was updated on the selected disk.")
                                                       : T("ReliefOS was installed to the selected disk."));
        reliefos_ui_text(ui, l.content_x, l.content_y + 96, T("Remove the installation media, then restart."), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        if (reboot_error) {
            reliefos_ui_text(ui, l.content_x, l.content_y + 130,
                           T("Restart failed"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
            reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 164, l.content_w,
                                   strerror(reboot_error), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
        }
    } else {
        draw_title(ui,
                   install_mode == INSTALL_MODE_UPDATE ? T("Update Failed")
                                                       : T("Installation Failed"),
                   T("No writes will continue after this error."));
        reliefos_ui_text(ui, l.content_x, l.content_y + 96, status_text, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 130, l.content_w, detail_text, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    }
}

static uint32_t account_width(void)
{
    uint32_t width = get_layout().content_w;
    return width < 480 ? width : 480;
}

static void draw_accounts_page(struct reliefos_ui_surface *ui)
{
    struct installer_layout l = get_layout();
    reliefos_inputm_set_current_context(RELIEFOS_INPUTM_CONTEXT_FOCUSED |
        (account_focus ? RELIEFOS_INPUTM_CONTEXT_SECURE : 0), l.content_x,
        l.content_y + 84 + account_focus * 54, account_width(), RELIEFOS_FONT_H + 8);
    const char *labels[] = {T("Standard user name"), T("Standard user password"),
                            T("Confirm password"),
                            T("root password"), T("Confirm root password")};
    draw_title(ui, T("Accounts"), T("Administrator: root"));
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t y = l.content_y + 64 + i * 54;
        reliefos_ui_text(ui, l.content_x, y, labels[i], RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        char masked[RELIEFOS_AUTH_PASSWORD_LEN];
        const char *text = account_edits[i].buffer;
        if (i) {
            size_t length = strlen(text);
            memset(masked, '*', length);
            masked[length] = 0;
            text = masked;
        }
        reliefos_ui_edit(ui, l.content_x, y + 20, account_width(), text,
                       account_edits[i].cursor, account_edits[i].scroll,
                       i == account_focus ? RELIEFOS_UI_EDIT_FOCUSED : 0);
    }
    if (setup.username[0] && !installer_setup_valid(&setup))
        reliefos_ui_text_clipped(ui, l.content_x, l.content_y + 346, l.content_w,
                               T("Passwords: 1-32 characters, no spaces; confirmations must match"),
                               RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void draw_installer(struct reliefos_ui_surface *ui)
{
    reliefos_ui_rect(ui, 0, 0, surface_w, surface_h, RELIEFOS_UI_WHITE);
    draw_sidebar(ui);
    switch (page) {
    case PAGE_LANGUAGE:
        draw_language_page(ui);
        break;
    case PAGE_THANKS:
        draw_acknowledgements_page(ui);
        break;
    case PAGE_THEME:
        draw_theme_page(ui);
        break;
    case PAGE_WELCOME:
        draw_welcome(ui);
        break;
    case PAGE_MODE:
        draw_mode_page(ui);
        break;
    case PAGE_DISK:
        draw_disk_page(ui);
        break;
    case PAGE_UPDATE_APPS:
        draw_update_apps_page(ui);
        break;
    case PAGE_CONFIRM:
        draw_confirm_page(ui);
        break;
    case PAGE_ACCOUNTS:
        draw_accounts_page(ui);
        break;
    case PAGE_PROGRESS:
        draw_progress_page(ui);
        break;
    case PAGE_FINISH:
    default:
        draw_finish_page(ui);
        break;
    }
    draw_footer(ui);
}

static void present_installer(int window_id, struct reliefos_ui_surface *ui)
{
    if (installer_tty_mode || !ui || window_id <= 0) {
        return;
    }
    draw_installer(ui);
    reliefos_gui_present_window((uint32_t)window_id, surface_w, surface_h,
                              INSTALLER_MAX_W, pixels);
    dirty = 0;
}

static void show_progress(int window_id, struct reliefos_ui_surface *ui,
                          uint32_t value, const char *status,
                          const char *detail)
{
    if (value > 100) {
        value = 100;
    }
    progress_value = value;
    set_status(status, detail);
    set_progress_text(status, detail);
    if (installer_tty_mode) {
        /* File copying calls this for every file. Keep the CLI readable by
         * emitting only stage changes and new percentage values. */
        if (value != tty_last_progress || !text_eq(progress_text, tty_last_status)) {
            printf("[%3u%%] %s\n", value, progress_text);
            tty_last_progress = value;
            copy_text(tty_last_status, sizeof(tty_last_status), progress_text);
        }
        return;
    }
    present_installer(window_id, ui);
}

static uint32_t copy_progress_percent(void)
{
    if (copy_total_bytes == 0) {
        return copy_total ? 35 + (copy_done * 60U) / copy_total : 35;
    }
    if (copy_done_bytes > copy_total_bytes) {
        copy_done_bytes = copy_total_bytes;
    }
    return 35 + (uint32_t)((copy_done_bytes * 60ULL) / copy_total_bytes);
}

static void show_copy_progress(int window_id, struct reliefos_ui_surface *ui,
                               const char *detail)
{
    static unsigned long last_present_ms;
    if (!installer_tty_mode) {
        unsigned long now = reliefos_uptime_ms();
        if (now - last_present_ms < COPY_PRESENT_INTERVAL_MS) return;
        last_present_ms = now;
    }
    show_progress(window_id, ui, copy_progress_percent(),
                  T("Copying system files"), detail);
}

/* mkdir(3) follows the POSIX convention and returns -1 on failure, while
 * installer traversal needs the native negative errno values (in particular
 * -17 for an already-existing directory).  Keep this narrow compatibility
 * helper local to the installer instead of changing libc semantics for every
 * application. */
static int installer_mkdir(const char *path)
{
    long ret;
    struct stat status;
    if (!path) {
        return -22;
    }
    /* Existing mount roots need no creation, and links must not redirect
     * installation writes outside the selected target tree. */
    if (lstat(path, &status) == 0)
        return S_ISDIR(status.st_mode) ? -EEXIST : -ENOTDIR;
    if (errno != ENOENT) return -errno;
    ret = syscall2(SYS_mkdir, (long)path, 0755);
    if (ret == -EEXIST) {
        if (lstat(path, &status) < 0) return -errno;
        if (!S_ISDIR(status.st_mode)) return -ENOTDIR;
    }
    return ret < 0 ? (int)ret : 0;
}

static int count_files_recursive(const char *src, uint32_t *out_count)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(child, sizeof(child), src, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Payload path is too long"));
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            struct reliefos_stat st;
            if (reliefos_stat_legacy(child, &st) == 0 && st.type == RELIEFOS_FS_TYPE_FILE) {
                copy_total_bytes += st.size;
            }
            ++*out_count;
            continue;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            /* Symlink recreation is a metadata-sized work item. */
            ++*out_count;
            continue;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = count_files_recursive(child, out_count);
            if (ret < 0) {
                goto out;
            }
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

static int path_has_type(const char *path, uint32_t type)
{
    struct reliefos_stat st;
    int ret = reliefos_stat_legacy(path, &st);
    if (ret < 0) {
        return ret;
    }
    return st.type == type ? 0 : -20;
}

static int source_file_exists(const char *path)
{
    return path_has_type(path, RELIEFOS_FS_TYPE_FILE) == 0;
}

/* Defined below: final-symlink-aware helpers used by traversal. */
static int path_type_nofollow(const char *path);
static int copy_symlink_path(const char *src, const char *dst);

static int add_file_copy_work(const char *path)
{
    struct reliefos_stat st;
    int type = path_type_nofollow(path);
    if (type == RELIEFOS_FS_TYPE_SYMLINK) {
        ++copy_total;
        return 0;
    }
    if (type < 0) {
        return type;
    }
    int ret = reliefos_stat_legacy(path, &st);
    if (ret < 0) {
        return ret;
    }
    if (st.type != RELIEFOS_FS_TYPE_FILE) {
        return -20;
    }
    ++copy_total;
    copy_total_bytes += st.size;
    return 0;
}

static int remove_path_recursive(const char *path)
{
    struct stat lst;
    struct reliefos_dir_entry *entries;
    /* lstat is essential here: unlinking "/target/bin" must remove the
     * usr-merge symlink itself, never recurse into /usr/bin. */
    if (lstat(path, &lst) < 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (S_ISLNK(lst.st_mode) || !S_ISDIR(lst.st_mode)) {
        return unlink(path);
    }
    entries = NULL;
    int ret;
    for (;;) {
        uint32_t count = 0;
        uint32_t removed = 0;
        free(entries);
        ret = installer_list_dir(path, &entries, &count);
        if (ret < 0) {
            goto out;
        }
        if (count == 0) {
            break;
        }
        for (uint32_t i = 0; i < count; ++i) {
            char child[RELIEFOS_FS_PATH_LEN];
            if (name_is_dot(entries[i].name)) {
                continue;
            }
            if (path_join(child, sizeof(child), path, entries[i].name) < 0) {
                ret = -1;
                goto out;
            }
            ret = remove_path_recursive(child);
            if (ret < 0) {
                goto out;
            }
            removed = 1;
        }
        if (!removed) {
            break;
        }
    }
    ret = rmdir(path);
out:
    free(entries);
    return ret;
}

static int copy_dir_recursive(const char *src, const char *dst,
                              int window_id, struct reliefos_ui_surface *ui);

static int copy_file_path(const char *src, const char *dst,
                          int window_id, struct reliefos_ui_surface *ui)
{
    int in_fd = open(src, RELIEFOS_O_RDONLY, 0);
    int out_fd = -1;
    int error = 0;
    char temporary[RELIEFOS_FS_PATH_LEN];
    struct stat source;
    long got = 0;
    uint32_t write_slice = sizeof(copy_buf);
    /* FAT32 extends the ESP chain synchronously under the kernel lock.
     * Bound each transaction so input and painting can run between writes. */
    if (strncmp(dst, INSTALL_ESP_MOUNT "/", sizeof(INSTALL_ESP_MOUNT)) == 0)
        write_slice = COPY_ESP_WRITE_SLICE;
    if (in_fd < 0) {
        return -errno;
    }
    if (fstat(in_fd, &source) < 0) { error = errno; goto done; }
    if (!S_ISREG(source.st_mode)) { error = EINVAL; goto done; }
    const char *slash = strrchr(dst, '/');
    if (!slash) { error = EINVAL; goto done; }
    int n = snprintf(temporary, sizeof(temporary), "%.*s.reliefos-copy-XXXXXX",
                     (int)(slash - dst + 1), dst);
    if (n < 0 || (size_t)n >= sizeof(temporary)) { error = ENAMETOOLONG; goto done; }
    /* A failed copy must leave the previous file (and any hard-link aliases)
     * intact. mkstemp also refuses to follow an attacker-supplied temp link. */
    out_fd = mkstemp(temporary);
    if (out_fd < 0) {
        error = errno;
        goto done;
    }
    if (!installer_tty_mode) {
        printf("[installer.elf] copying %s -> %s\n", src, dst);
    }
    show_copy_progress(window_id, ui, dst);
    for (;;) {
        got = installer_read_chunk(in_fd, copy_buf, sizeof(copy_buf));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        long written = 0;
        while (written < got) {
            uint32_t chunk = (uint32_t)(got - written);
            if (chunk > write_slice) chunk = write_slice;
            long ret = write(out_fd, copy_buf + written, chunk);
            if (ret < 0 && errno == EINTR) continue;
            if (ret <= 0) {
                error = ret < 0 ? errno : EIO;
                goto done;
            }
            written += ret;
            copy_done_bytes += (uint64_t)ret;
            show_copy_progress(window_id, ui, dst);
            if (written < got) (void)sched_yield();
        }
        /* read(2) may return a single 4 KiB slice. Sleeping after every short
         * read turns each slice into a full PIT tick. Yield without a timed
         * delay; progress painting remains throttled independently. */
        (void)sched_yield();
    }
    if (got < 0) { error = errno; goto done; }
    if (fchown(out_fd, source.st_uid, source.st_gid) < 0 ||
        fchmod(out_fd, source.st_mode & 07777) < 0 || fsync(out_fd) < 0) {
        error = errno;
        goto done;
    }
    if (close(out_fd) < 0) error = errno;
    out_fd = -1;
    if (!error && rename(temporary, dst) < 0) error = errno;
    if (error) unlink(temporary);
done:
    if (out_fd >= 0) {
        close(out_fd);
        unlink(temporary);
    }
    close(in_fd);
    if (error) printf("[installer.elf] copy %s -> %s failed errno=%d\n", src, dst, error);
    return -error;
}

/* Return the type of path without following a final symlink. */
static int path_type_nofollow(const char *path)
{
    struct stat status;
    if (!path || lstat(path, &status) < 0) {
        return errno ? -errno : -2;
    }
    if (S_ISLNK(status.st_mode)) return RELIEFOS_FS_TYPE_SYMLINK;
    if (S_ISDIR(status.st_mode)) return RELIEFOS_FS_TYPE_DIR;
    if (S_ISREG(status.st_mode)) return RELIEFOS_FS_TYPE_FILE;
    return RELIEFOS_FS_TYPE_DEVICE;
}

/* Recreate a symlink with the same literal target.  The Alpine root
 * layout depends on symlinks staying symlinks across installation; copying
 * the target bytes as a directory or regular file is not equivalent. */
static int copy_symlink_path(const char *src, const char *dst)
{
    char target[RELIEFOS_FS_PATH_LEN];
    char temporary[RELIEFOS_FS_PATH_LEN];
    struct stat status;
    ssize_t length;
    if (!src || !dst) return -22;
    length = readlink(src, target, sizeof(target) - 1U);
    if (length < 0) return -errno;
    if (length >= (ssize_t)sizeof(target) - 1U) return -36;
    target[length] = 0;
    if (lstat(src, &status) < 0) return -errno;
    const char *slash = strrchr(dst, '/');
    if (!slash) return -EINVAL;
    int n = snprintf(temporary, sizeof(temporary), "%.*s.reliefos-link-XXXXXX",
                     (int)(slash - dst + 1), dst);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return -ENAMETOOLONG;
    int fd = mkstemp(temporary);
    if (fd < 0) return -errno;
    int error = close(fd) < 0 ? errno : 0;
    if (unlink(temporary) < 0 && !error) error = errno;
    if (error) return -error;
    if (symlink(target, temporary) < 0) return -errno;
    if (lchown(temporary, status.st_uid, status.st_gid) < 0 ||
        rename(temporary, dst) < 0) {
        int error = errno;
        unlink(temporary);
        return -error;
    }
    return 0;
}

static int copy_dir_recursive(const char *src, const char *dst,
                              int window_id, struct reliefos_ui_surface *ui)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        printf("[installer.elf] list source dir %s ret=%d\n", src, ret);
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Copy path is too long"));
            printf("[installer.elf] copy path too long src=%s dst=%s\n", src, dst);
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = installer_mkdir(dst_child);
            if (ret == -17) {
                struct reliefos_stat dst_st;
                ret = reliefos_stat_legacy(dst_child, &dst_st);
                if (ret == 0 && dst_st.type == RELIEFOS_FS_TYPE_DIR) {
                    ret = 0;
                } else if (ret == 0) {
                    ret = -20;
                } else {
                    printf("[installer.elf] stat existing dir %s ret=%d\n", dst_child, ret);
                }
            }
            if (ret < 0) {
                printf("[installer.elf] mkdir %s ret=%d\n", dst_child, ret);
                goto out;
            }
            ret = copy_dir_recursive(src_child, dst_child, window_id, ui);
            if (ret < 0) {
                printf("[installer.elf] recurse copy %s -> %s ret=%d\n",
                       src_child, dst_child, ret);
                goto out;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            ret = copy_file_path(src_child, dst_child, window_id, ui);
            if (ret < 0) {
                printf("[installer.elf] copy %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(window_id, ui, dst_child);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            ret = copy_symlink_path(src_child, dst_child);
            if (ret < 0) {
                printf("[installer.elf] symlink %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(window_id, ui, dst_child);
        }
    }
    struct stat directory_mode;
    if (stat(src, &directory_mode) < 0 ||
        chown(dst, directory_mode.st_uid, directory_mode.st_gid) < 0 ||
        chmod(dst, directory_mode.st_mode & 07777) < 0) {
        ret = -errno;
    } else {
        ret = 0;
    }
out:
    free(entries);
    return ret;
}

static int merge_dir_recursive(const char *src, const char *dst,
                               int window_id, struct reliefos_ui_surface *ui)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_mkdir(dst);
    if (ret < 0 && ret != -17) {
        printf("[installer.elf] mkdir merge dir %s ret=%d\n", dst, ret);
        goto out;
    }
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        printf("[installer.elf] list merge source dir %s ret=%d\n", src, ret);
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Copy path is too long"));
            printf("[installer.elf] merge path too long src=%s dst=%s\n", src, dst);
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = merge_dir_recursive(src_child, dst_child, window_id, ui);
            if (ret < 0) {
                printf("[installer.elf] recurse merge %s -> %s ret=%d\n",
                       src_child, dst_child, ret);
                goto out;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            ret = copy_file_path(src_child, dst_child, window_id, ui);
            if (ret < 0) {
                printf("[installer.elf] copy %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(window_id, ui, dst_child);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            ret = copy_symlink_path(src_child, dst_child);
            if (ret < 0) {
                printf("[installer.elf] symlink %s -> %s ret=%d\n", src_child, dst_child, ret);
                goto out;
            }
            ++copy_done;
            show_copy_progress(window_id, ui, dst_child);
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

/* Return whether any source file differs from the corresponding target file.
 * Extra files already present on the target are intentionally ignored: update
 * mode is additive for user data and only refreshes files shipped by ReliefOS. */
static int package_has_changes(const char *src, const char *dst)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            struct reliefos_stat dst_stat;
            ret = reliefos_stat_legacy(dst_child, &dst_stat);
            if (ret < 0 || dst_stat.type != RELIEFOS_FS_TYPE_DIR) {
                ret = 1;
                goto out;
            }
            ret = package_has_changes(src_child, dst_child);
            if (ret != 0) {
                goto out;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE ||
                   entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            uint8_t missing;
            uint8_t different;
            ret = installer_files_equal(src_child, dst_child, &missing, &different);
            if (ret < 0) {
                goto out;
            }
            if (missing || different) {
                ret = 1;
                goto out;
            }
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

static int count_changed_files_recursive(const char *src, const char *dst)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Copy path is too long"));
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = count_changed_files_recursive(src_child, dst_child);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE ||
                   entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            uint8_t missing;
            uint8_t different;
            ret = installer_files_equal(src_child, dst_child, &missing, &different);
            if (ret >= 0 && (missing || different)) {
                ret = add_file_copy_work(src_child);
            }
        } else {
            continue;
        }
        if (ret < 0) {
            goto out;
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

static int copy_changed_dir_recursive(const char *src, const char *dst,
                                      int window_id, struct reliefos_ui_surface *ui)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    ret = installer_mkdir(dst);
    if (ret < 0 && ret != -17) {
        printf("[installer.elf] mkdir changed dir %s ret=%d\n", dst, ret);
        goto out;
    }
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) {
        printf("[installer.elf] list changed source dir %s ret=%d\n", src, ret);
        goto out;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            set_status(T("Installation failed"), T("Copy path is too long"));
            printf("[installer.elf] changed path too long src=%s dst=%s\n", src, dst);
            ret = -1;
            goto out;
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            int dst_type = path_type_nofollow(dst_child);
            if (dst_type >= 0 && dst_type != RELIEFOS_FS_TYPE_DIR) {
                ret = remove_path_recursive(dst_child);
                if (ret < 0) {
                    goto out;
                }
            } else if (dst_type < 0 && dst_type != -2) {
                ret = dst_type;
                goto out;
            }
            ret = copy_changed_dir_recursive(src_child, dst_child, window_id, ui);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE ||
                   entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            uint8_t missing;
            uint8_t different;
            int dst_type = path_type_nofollow(dst_child);
            if (dst_type >= 0 &&
                dst_type != (int)entries[i].type) {
                ret = remove_path_recursive(dst_child);
                if (ret < 0) {
                    goto out;
                }
            } else if (dst_type < 0 && dst_type != -2) {
                ret = dst_type;
                goto out;
            }
            ret = installer_files_equal(src_child, dst_child, &missing, &different);
            if (ret >= 0 && (missing || different)) {
                if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
                    ret = copy_symlink_path(src_child, dst_child);
                } else {
                    ret = copy_file_path(src_child, dst_child, window_id, ui);
                }
                if (ret >= 0) {
                    ++copy_done;
                    show_copy_progress(window_id, ui, dst_child);
                }
            }
        } else {
            continue;
        }
        if (ret < 0) {
            printf("[installer.elf] recurse changed copy %s -> %s ret=%d\n",
                   src_child, dst_child, ret);
            goto out;
        }
    }
    ret = 0;
out:
    free(entries);
    return ret;
}

static int copy_payload_ordered(int window_id, struct reliefos_ui_surface *ui)
{
    int ret = copy_dir_recursive(INSTALL_ROOT_PAYLOAD, INSTALL_ROOT_MOUNT, window_id, ui);
    if (!ret) ret = installer_write_fstab();
    if (!ret && installer_setup_write(&setup, INSTALL_ROOT_MOUNT) < 0) {
        ret = -errno;
        set_status(T("Account setup failed"), strerror(errno));
    }
    if (ret < 0) {
        printf("[installer.elf] root payload copy failed src=%s dst=%s ret=%d\n",
               INSTALL_ROOT_PAYLOAD, INSTALL_ROOT_MOUNT, ret);
        return ret;
    }
    ret = copy_dir_recursive(INSTALL_ESP_PAYLOAD, INSTALL_ESP_MOUNT, window_id, ui);
    if (ret < 0) {
        printf("[installer.elf] ESP payload copy failed src=%s dst=%s ret=%d\n",
               INSTALL_ESP_PAYLOAD, INSTALL_ESP_MOUNT, ret);
        return ret;
    }

    /* The root payload normally creates this empty directory.  Verify it
     * explicitly and make the operation idempotent for filesystems that keep
     * an end-marker or directory cache across the preceding copy. */
    {
        const char *state_path = TARGET_VAR_LIB_RELIEFOS;
        struct reliefos_stat state_st;
        ret = reliefos_stat_legacy(state_path, &state_st);
        if (ret == 0) {
            if (state_st.type == RELIEFOS_FS_TYPE_DIR) {
                return 0;
            }
            printf("[installer.elf] target state has wrong type=%u path=%s\n",
                   state_st.type, state_path);
            ret = remove_path_recursive(state_path);
            if (ret < 0) {
                printf("[installer.elf] remove wrong target state %s ret=%d\n",
                       state_path, ret);
                return ret;
            }
        } else if (ret != -2) {
            printf("[installer.elf] stat target state %s ret=%d\n", state_path, ret);
            return ret;
        }
        ret = installer_mkdir(state_path);
        if (ret == -17) {
            ret = reliefos_stat_legacy(state_path, &state_st);
            if (ret == 0 && state_st.type == RELIEFOS_FS_TYPE_DIR) {
                return 0;
            }
            printf("[installer.elf] verify existing target state %s ret=%d type=%u\n",
                   state_path, ret, ret == 0 ? state_st.type : 0u);
            return ret < 0 ? ret : -20;
        }
        if (ret < 0) {
            printf("[installer.elf] mkdir target state %s ret=%d\n", state_path, ret);
        }
        return ret;
    }
}

static int check_update_target_required(void)
{
    /* Both brand namespaces use the same non-usr-merge filesystem contract.
     * Accept a complete old namespace for migration, but never a usr-merged
     * root or symlinked hierarchy. This preflight performs no writes. */
    static const char *const required_dirs[] = {
        "/etc", "/var", "/var/lib", "/bin", "/sbin", "/lib",
        "/usr", "/usr/bin", "/usr/sbin", "/usr/lib",
    };
    for (uint32_t i = 0; i < sizeof(required_dirs) / sizeof(required_dirs[0]); ++i) {
        char path[RELIEFOS_FS_PATH_LEN];
        if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, required_dirs[i]) < 0)
            return -ENAMETOOLONG;
        if (path_type_nofollow(path) != RELIEFOS_FS_TYPE_DIR) {
            set_status(T("Unsupported root layout; use a fresh ext4 installation"), path);
            return -EINVAL;
        }
    }
    {
        static const char *const namespaces[][6] = {
            {"/etc/reliefos", "/var/lib/reliefos", "/usr/lib/reliefos",
             "/usr/lib/reliefos/apps", "/usr/lib/reliefos/drivers",
             "/usr/lib/reliefos/apps/desktop/desktop.elf"},
            {"/etc/leonos", "/var/lib/leonos", "/usr/lib/leonos",
             "/usr/lib/leonos/apps", "/usr/lib/leonos/drivers",
             "/usr/lib/leonos/apps/desktop/desktop.elf"},
        };
        int found = 0;
        for (uint32_t n = 0; n < sizeof(namespaces) / sizeof(namespaces[0]); ++n) {
            int complete = 1;
            for (uint32_t i = 0; i < 6; ++i) {
                char path[RELIEFOS_FS_PATH_LEN];
                int required = i == 5 ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DIR;
                if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, namespaces[n][i]) < 0)
                    return -ENAMETOOLONG;
                if (path_type_nofollow(path) != required) { complete = 0; break; }
            }
            if (complete) { found = 1; break; }
        }
        if (!found) {
            set_status(T("Existing ReliefOS was not detected"), INSTALL_ROOT_MOUNT);
            return -EINVAL;
        }
    }
    {
        static const char *const esp_dirs[] = {"boot", "boot/EFI"};
        for (uint32_t i = 0; i < sizeof(esp_dirs) / sizeof(esp_dirs[0]); ++i) {
            char path[RELIEFOS_FS_PATH_LEN];
            if (path_join(path, sizeof(path), INSTALL_ROOT_MOUNT, esp_dirs[i]) < 0 ||
                path_has_type(path, RELIEFOS_FS_TYPE_DIR) < 0) {
                set_status(T("Existing ReliefOS boot partition was not detected"), path);
                return -2;
            }
        }
    }
    {
        char loader[RELIEFOS_FS_PATH_LEN];
        if (path_join(loader, sizeof(loader), TARGET_BOOT, "loader.elf") < 0 ||
            path_has_type(loader, RELIEFOS_FS_TYPE_FILE) < 0) {
            set_status(T("Existing ReliefOS boot partition was not detected"), loader);
            return -2;
        }
    }
    if (path_has_type(TARGET_ESP_KERNEL, RELIEFOS_FS_TYPE_FILE) < 0 &&
        path_has_type(TARGET_ESP_KERNEL_LEGACY, RELIEFOS_FS_TYPE_FILE) < 0) {
        set_status(T("Existing ReliefOS boot partition was not detected"), TARGET_ESP_KERNEL);
        return -2;
    }
    return 0;
}

static int check_update_payload_required(void)
{
    static const char *const required_dirs[] = {
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_ETC_RELIEFOS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_USR_LIB,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_LIB,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_DRIVERS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_DOC,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_FONTS,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_RESOURCES,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LICENSES,
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_ETC_SSL_CERTS,
        INSTALL_ESP_PAYLOAD "/grub",
        INSTALL_ESP_PAYLOAD "/reliefos",
        INSTALL_ESP_PAYLOAD "/leonos",
        INSTALL_ESP_PAYLOAD "/EFI",
    };
    static const char *const required_files[] = {
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_LIB "/kerneldebug.sys",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/ld-musl-x86_64.so.1",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/libc.so",
        INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_LIB "/libmimalloc.so.3",
        INSTALL_ROOT_PAYLOAD RELIEFOS_PATH_LIBRELIEFOS,
        INSTALL_ROOT_PAYLOAD "/usr/lib/leonos/libleonos.so.2",
        INSTALL_ESP_PAYLOAD "/reliefos/kernel.sys",
        INSTALL_ESP_PAYLOAD "/reliefos/loader.elf",
        INSTALL_ESP_PAYLOAD "/leonos/kernel.sys",
        INSTALL_ESP_PAYLOAD "/loader.elf",
        INSTALL_ESP_PAYLOAD "/grub/grub.cfg",
        INSTALL_ESP_PAYLOAD "/EFI/BOOT/BOOTX64.EFI",
    };
    for (uint32_t i = 0; i < sizeof(required_dirs) / sizeof(required_dirs[0]); ++i) {
        int ret = path_has_type(required_dirs[i], RELIEFOS_FS_TYPE_DIR);
        if (ret < 0) {
            set_status(T("Installation media is incomplete"),
                       required_dirs[i]);
            return ret;
        }
    }
    for (uint32_t i = 0; i < sizeof(required_files) / sizeof(required_files[0]); ++i) {
        int ret = path_has_type(required_files[i], RELIEFOS_FS_TYPE_FILE);
        if (ret < 0) {
            set_status(T("Installation media is incomplete"),
                       required_files[i]);
            return ret;
        }
    }
    return 0;
}

static int add_update_app_entry(const char *name,
                                const char *src_elf,
                                const char *dst_elf,
                                const char *src_icon,
                                const char *dst_icon,
                                uint8_t missing,
                                uint8_t elf_diff,
                                uint8_t icon_diff)
{
    struct update_app_entry *entry;
    if (update_app_count >= UPDATE_APP_MAX) {
        return -28;
    }
    entry = &update_apps[update_app_count++];
    copy_text(entry->name, sizeof(entry->name), name);
    copy_text(entry->src_elf, sizeof(entry->src_elf), src_elf);
    copy_text(entry->dst_elf, sizeof(entry->dst_elf), dst_elf);
    copy_text(entry->src_icon, sizeof(entry->src_icon), src_icon);
    copy_text(entry->dst_icon, sizeof(entry->dst_icon), dst_icon);
    entry->selected = 1;
    entry->missing = missing;
    entry->elf_diff = elf_diff;
    entry->icon_diff = icon_diff;
    return 0;
}

/* Application packages from the old /programs tree are optional update
 * entries.  System packages are replaced as part of the core payload, so the
 * manifest's system=1 marker keeps them out of the optional list. */
static int app_package_is_system(const char *package_dir)
{
    char manifest[RELIEFOS_FS_PATH_LEN];
    char text[512];
    int fd;
    long got;
    if (path_join(manifest, sizeof(manifest), package_dir, "manifest.ini") < 0) {
        return 0;
    }
    fd = open(manifest, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    got = read(fd, text, sizeof(text) - 1U);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    text[got] = 0;
    return strstr(text, "system=1") != 0 || strstr(text, "system=true") != 0;
}

static int scan_update_apps(void)
{
    static const char *const source_root = INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS;
    static const char *const target_root = TARGET_RELIEFOS_APPS;
    struct reliefos_dir_entry entries[RELIEFOS_FS_MAX_ENTRIES];
    uint32_t count = 0;
    int ret;
    reset_update_app_list();
    ret = reliefos_list_dir(source_root, entries,
                          RELIEFOS_FS_MAX_ENTRIES, &count);
    printf("[installer.elf] scan programs list ret=%d count=%u path=%s\n",
           ret, count, source_root);
    if (ret < 0) {
        return ret;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char src_elf[RELIEFOS_FS_PATH_LEN];
        char dst_elf[RELIEFOS_FS_PATH_LEN];
        char src_icon[RELIEFOS_FS_PATH_LEN];
        char dst_icon[RELIEFOS_FS_PATH_LEN];
        char src_package[RELIEFOS_FS_PATH_LEN];
        char dst_package[RELIEFOS_FS_PATH_LEN];
        char elf_name[RELIEFOS_FS_PATH_LEN];
        uint32_t name_len = 0;
        uint8_t missing = 0;
        uint8_t elf_diff = 1;
        uint8_t icon_missing = 0;
        uint8_t icon_diff = 0;
        uint8_t package_diff = 0;
        if (entries[i].type != RELIEFOS_FS_TYPE_DIR) {
            continue;
        }
        if (path_join(src_package, sizeof(src_package), source_root, entries[i].name) < 0 ||
            path_join(dst_package, sizeof(dst_package), target_root, entries[i].name) < 0) {
            return -1;
        }
        if (app_package_is_system(src_package)) {
            continue;
        }
        copy_text(elf_name, sizeof(elf_name), entries[i].name);
        while (elf_name[name_len]) {
            ++name_len;
        }
        append_text(elf_name, &name_len, sizeof(elf_name), ".elf");
        if (path_join(src_elf, sizeof(src_elf), src_package, elf_name) < 0 ||
            path_join(dst_elf, sizeof(dst_elf), dst_package, elf_name) < 0 ||
            !source_file_exists(src_elf)) {
            continue;
        }
        copy_replace_extension(src_icon, sizeof(src_icon), src_elf, ".bmp");
        copy_replace_extension(dst_icon, sizeof(dst_icon), dst_elf, ".bmp");
        ret = installer_files_equal(src_elf, dst_elf, &missing, &elf_diff);
        if (ret < 0) {
            printf("[installer.elf] scan compare elf failed app=%s src=%s dst=%s ret=%d\n",
                   entries[i].name, src_elf, dst_elf, ret);
            return ret;
        }
        if (source_file_exists(src_icon)) {
            ret = installer_files_equal(src_icon, dst_icon, &icon_missing, &icon_diff);
            if (ret < 0) {
                return ret;
            }
            if (icon_missing) {
                icon_diff = 1;
            }
        } else {
            src_icon[0] = 0;
            dst_icon[0] = 0;
        }
        if (missing || elf_diff || icon_diff) {
            package_diff = 1;
        } else {
            ret = package_has_changes(src_package, dst_package);
            if (ret < 0) {
                printf("[installer.elf] scan package changes failed app=%s src=%s dst=%s ret=%d\n",
                       entries[i].name, src_package, dst_package, ret);
                return ret;
            }
            package_diff = ret ? 1 : 0;
        }
        if (missing || elf_diff || icon_diff || package_diff) {
            ret = add_update_app_entry(entries[i].name, src_elf, dst_elf,
                                       src_icon, dst_icon, missing,
                                       elf_diff, icon_diff);
            if (ret < 0) {
                return ret;
            }
            copy_text(update_apps[update_app_count - 1].src_package,
                      sizeof(update_apps[update_app_count - 1].src_package),
                      src_package);
            copy_text(update_apps[update_app_count - 1].dst_package,
                      sizeof(update_apps[update_app_count - 1].dst_package),
                      dst_package);
            update_apps[update_app_count - 1].package_diff = package_diff;
        }
    }
    reliefos_ui_listview_state_set_count(&update_app_list, update_app_count);
    update_app_list.selected = update_app_count ? 0 : -1;
    if (update_app_count) {
        char line[128];
        uint32_t pos = 0;
        line[0] = 0;
        append_u64(line, &pos, sizeof(line), update_app_count);
        append_text(line, &pos, sizeof(line),
                    T(" program package update(s) found."));
        set_status(line, T("All are selected by default."));
    } else {
        set_status(T("No program package differences were found."),
                   T("Core files, bundled docs, and drivers can still be updated."));
    }
    return 0;
}

static int count_selected_update_work(void)
{
    int ret;
    for (uint32_t i = 0; i < update_app_count; ++i) {
        if (!update_apps[i].selected) {
            continue;
        }
        ret = count_changed_files_recursive(update_apps[i].src_package,
                                            update_apps[i].dst_package);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int copy_selected_update_apps(int window_id, struct reliefos_ui_surface *ui)
{
    int ret;
    ret = installer_mkdir(TARGET_RELIEFOS_APPS);
    if (ret < 0 && ret != -17) {
        return ret;
    }
    for (uint32_t i = 0; i < update_app_count; ++i) {
        if (!update_apps[i].selected) {
            continue;
        }
        char package_dir[RELIEFOS_FS_PATH_LEN];
        if (path_join(package_dir, sizeof(package_dir), TARGET_RELIEFOS_APPS,
                      update_apps[i].name) < 0) {
            return -1;
        }
        {
            struct reliefos_stat package_stat;
            ret = reliefos_stat_legacy(package_dir, &package_stat);
            if (ret == 0) {
                if (package_stat.type != RELIEFOS_FS_TYPE_DIR) {
                    ret = remove_path_recursive(package_dir);
                    if (ret < 0) {
                        return ret;
                    }
                }
            } else if (ret != -2) {
                return ret;
            }
            ret = installer_mkdir(package_dir);
            if (ret < 0 && ret != -17) {
                return ret;
            }
        }
        ret = copy_changed_dir_recursive(update_apps[i].src_package,
                                         update_apps[i].dst_package,
                                         window_id, ui);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int write_target_locale(void)
{
    return write_locale_setting(TARGET_ETC_RELIEFOS "/locale.conf", language_selection()) == 0
        ? 0 : -errno;
}

static int display_config_line_is_theme(const char *line, uint32_t len)
{
    return len >= 6 && line[0] == 't' && line[1] == 'h' && line[2] == 'e' &&
           line[3] == 'm' && line[4] == 'e' && line[5] == '=';
}

static int write_target_theme(void)
{
    char input[384];
    char output[512];
    const char *theme = installer_theme == RELIEFOS_UI_THEME_WIN95 ? "win95" : "metro";
    struct reliefos_stat stat_info;
    uint32_t input_len = 0;
    uint32_t output_len = 0;
    uint32_t offset = 0;
    int ret = reliefos_stat_legacy(TARGET_ETC_RELIEFOS "/display.conf", &stat_info);
    if (ret == 0) {
        int fd;
        long got;
        if (stat_info.type != RELIEFOS_FS_TYPE_FILE || stat_info.size >= sizeof(input)) {
            return -27;
        }
        fd = open(TARGET_ETC_RELIEFOS "/display.conf", RELIEFOS_O_RDONLY, 0);
        if (fd < 0) {
            return fd;
        }
        got = read(fd, input, (uint32_t)stat_info.size);
        close(fd);
        if (got < 0) {
            return (int)got;
        }
        input_len = (uint32_t)got;
    } else if (ret != -2) {
        return ret;
    }
    while (offset < input_len) {
        uint32_t line_start = offset;
        uint32_t line_end;
        while (offset < input_len && input[offset] != '\n') {
            ++offset;
        }
        line_end = offset;
        if (line_end > line_start && input[line_end - 1] == '\r') {
            --line_end;
        }
        if (offset < input_len) {
            ++offset;
        }
        if (display_config_line_is_theme(input + line_start, line_end - line_start)) {
            continue;
        }
        for (uint32_t index = line_start; index < line_end; ++index) {
            if (append_char(output, &output_len, sizeof(output), input[index]) < 0) {
                return -27;
            }
        }
        if (append_char(output, &output_len, sizeof(output), '\n') < 0) {
            return -27;
        }
    }
    if (append_text(output, &output_len, sizeof(output), "theme=") < 0 ||
        append_text(output, &output_len, sizeof(output), theme) < 0 ||
        append_char(output, &output_len, sizeof(output), '\n') < 0) {
        return -27;
    }
    {
        int fd = open(TARGET_ETC_RELIEFOS "/display.conf",
                      RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
        long wrote;
        if (fd < 0) {
            return fd;
        }
        wrote = write(fd, output, output_len);
        close(fd);
        if (wrote < 0) {
            return (int)wrote;
        }
        return wrote == (long)output_len ? 0 : -5;
    }
}

static int write_target_preferences(void)
{
    int ret;
    if (install_mode == INSTALL_MODE_FRESH) {
        ret = write_target_locale();
        if (ret < 0) {
            return ret;
        }
    }
    if (install_mode == INSTALL_MODE_FRESH || installer_theme_explicit) {
        return write_target_theme();
    }
    return 0;
}

static void finish_install(int window_id, struct reliefos_ui_surface *ui, int ret,
                           const char *prefix)
{
    install_running = 0;
    page = PAGE_FINISH;
    if (ret < 0) {
        printf("[installer.elf] %s ret=%d\n", prefix ? prefix : "install failed", ret);
        install_success = 0;
        set_error_status(prefix, ret);
        progress_value = 0;
    } else {
        fprintf(stderr, "[installer.elf] %s completed successfully\n",
               install_mode == INSTALL_MODE_UPDATE ? "update" : "installation");
        install_success = 1;
        progress_value = 100;
        set_status(install_mode == INSTALL_MODE_UPDATE
                       ? T("Update completed successfully")
                       : T("Installation completed successfully"),
                   T("Press Restart to boot from the installed disk."));
    }
    if (installer_tty_mode) {
        if (ret < 0) {
            printf("\n%s (error %d)\n", prefix ? prefix : "Install failed", ret);
        } else {
            puts("\nInstallation completed successfully.");
            puts("Remove the installation media before rebooting.");
        }
    } else {
        present_installer(window_id, ui);
    }
}

static void perform_install(int window_id, struct reliefos_ui_surface *ui);
static void perform_update(int window_id, struct reliefos_ui_surface *ui);
static void prepare_update_target(int window_id, struct reliefos_ui_surface *ui);

static void tty_print_update_packages(void)
{
    char line[160];
    if (!update_app_count) {
        puts("No program package differences were found.");
        return;
    }
    puts("Program packages selected for update:");
    for (uint32_t i = 0; i < update_app_count; ++i) {
        format_update_reason(line, sizeof(line), &update_apps[i]);
        printf("  [%u] %s %s\n", i, update_apps[i].name, line);
    }
}

static void tty_prepare_update(void)
{
    prepare_update_target(0, 0);
}

static void tty_perform_install(void)
{
    perform_install(0, 0);
}

static void tty_perform_update(void)
{
    perform_update(0, 0);
}


/* Helpers for updates within the current rootfs layout. */

static int installer_mkdir_p(const char *path)
{
    char buffer[RELIEFOS_FS_PATH_LEN];
    uint32_t i;
    if (!path || !path[0] || strlen(path) >= sizeof(buffer)) {
        return path && path[0] ? -36 : -22;
    }
    copy_text(buffer, sizeof(buffer), path);
    for (i = 1; buffer[i]; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = 0;
            int ret = installer_mkdir(buffer);
            if (ret < 0 && ret != -17) {
                return ret;
            }
            buffer[i] = '/';
        }
    }
    return installer_mkdir(buffer);
}

static int apply_directory_metadata(const char *src, const char *dst)
{
    struct stat status;
    if (!src || !dst || stat(src, &status) < 0) {
        return src && dst ? -errno : -22;
    }
    if (chown(dst, status.st_uid, status.st_gid) < 0) {
        return -errno;
    }
    return chmod(dst, status.st_mode & 07777) < 0 ? -errno : 0;
}

static int payload_has_app_package(const char *name)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (path_join(path, sizeof(path), INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                  name) < 0) {
        return 0;
    }
    return path_has_type(path, RELIEFOS_FS_TYPE_DIR) == 0;
}

/* Require real hierarchy components. The standard links are copied from
 * the same layout table as fresh images; no old-root transformation occurs. */
static int apply_runtime_root_paths(void)
{
    static const struct { const char *path; const char *target; } links[] = {
#define ROOT_LINK(path, target) {INSTALL_ROOT_MOUNT path, target},
        RELIEFOS_ROOTFS_SYMLINKS(ROOT_LINK)
#undef ROOT_LINK
    };
    for (uint32_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
        int type = path_type_nofollow(links[i].path);
        if (type == -ENOENT) {
            if (symlink(links[i].target, links[i].path) < 0) return -errno;
        } else if (type == RELIEFOS_FS_TYPE_SYMLINK) {
            char target[RELIEFOS_FS_PATH_LEN];
            ssize_t length = readlink(links[i].path, target, sizeof(target) - 1);
            if (length < 0) return -errno;
            target[length] = 0;
            if (strcmp(target, links[i].target)) return -EEXIST;
        } else {
            return type < 0 ? type : -EEXIST;
        }
    }
    return 0;
}
static int ensure_runtime_layout_dirs(void)
{
    static const struct {
        const char *path;
        uint32_t mode;
    } dirs[] = {
#define ROOT_DIR(path, mode) {INSTALL_ROOT_MOUNT path, mode},
        RELIEFOS_ROOTFS_DIRECTORIES(ROOT_DIR)
#undef ROOT_DIR
    };
    for (uint32_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        int ret = installer_mkdir_p(dirs[i].path);
        if (ret < 0 && ret != -17) return ret;
        if (chown(dirs[i].path, 0, 0) < 0 ||
            chmod(dirs[i].path, dirs[i].mode) < 0) return -errno;
    }
    return 0;
}

/* Copy new payload paths over an existing tree without deleting unrelated
 * target entries.  System files are refreshed; user-created commands and
 * packages survive. */
static int overlay_dir_recursive_filtered(const char *src, const char *dst,
                                          int window_id, struct reliefos_ui_surface *ui,
                                          const char *skip_source)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    int dst_type = path_type_nofollow(dst);
    if (dst_type == -2) {
        ret = installer_mkdir_p(dst);
        if (ret < 0 && ret != -17) return ret;
        ret = apply_directory_metadata(src, dst);
        if (ret < 0) return ret;
    } else if (dst_type != RELIEFOS_FS_TYPE_DIR) {
        printf("[installer.elf] overlay target is not a directory: %s type=%d\n",
               dst, dst_type);
        return -17;
    }
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) continue;
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            free(entries);
            return -1;
        }
        if (skip_source && !strcmp(src_child, skip_source)) continue;
        if (strcmp(src_child, INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS) == 0) {
            /* Packages have their own system/optional selection below. */
            continue;
        }
        if (strcmp(src, INSTALL_ROOT_PAYLOAD "/usr/bin") == 0 &&
            payload_has_app_package(entries[i].name)) {
            char package[RELIEFOS_FS_PATH_LEN];
            if (path_join(package, sizeof(package),
                          INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                          entries[i].name) < 0) { free(entries); return -ENAMETOOLONG; }
            int publish = app_package_is_system(package);
            if (!publish) {
                char executable[RELIEFOS_FS_PATH_LEN];
                int size = snprintf(executable, sizeof(executable), "%s/%s/%s.elf",
                                    TARGET_RELIEFOS_APPS, entries[i].name, entries[i].name);
                if (size < 0 || (size_t)size >= sizeof(executable)) {
                    free(entries); return -ENAMETOOLONG;
                }
                publish = path_has_type(executable, RELIEFOS_FS_TYPE_FILE) == 0;
            }
            for (uint32_t j = 0; j < update_app_count; ++j) {
                if (!strcmp(update_apps[j].name, entries[i].name)) {
                    publish = update_apps[j].selected;
                    break;
                }
            }
            if (!publish) continue; /* preserve the unchecked app's existing entry */
        }
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            ret = overlay_dir_recursive_filtered(src_child, dst_child, window_id, ui,
                                                 skip_source);
        } else if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
            int child_type = path_type_nofollow(dst_child);
            if (child_type >= 0 && child_type != RELIEFOS_FS_TYPE_FILE &&
                child_type != RELIEFOS_FS_TYPE_SYMLINK) {
                ret = -17;
            } else {
                ret = copy_file_path(src_child, dst_child, window_id, ui);
                if (ret >= 0) ++copy_done;
            }
        } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
            int child_type = path_type_nofollow(dst_child);
            if (child_type == -2 || child_type == RELIEFOS_FS_TYPE_FILE ||
                child_type == RELIEFOS_FS_TYPE_SYMLINK) {
                ret = copy_symlink_path(src_child, dst_child);
            } else {
                ret = -17;
            }
            if (ret >= 0 && child_type == -2) ++copy_done;
        } else {
            ret = 0;
        }
        if (ret < 0) {
            printf("[installer.elf] layout overlay conflict src=%s dst=%s ret=%d\n",
                   src_child, dst_child, ret);
            free(entries);
            return ret;
        }
    }
    free(entries);
    return 0;
}

static int overlay_dir_recursive(const char *src, const char *dst,
                                 int window_id, struct reliefos_ui_surface *ui)
{
    return overlay_dir_recursive_filtered(src, dst, window_id, ui, NULL);
}

/* Copy defaults only when the destination does not already exist.  Used for
 * /etc and persistent state so an update never overwrites user settings. */
static int merge_missing_dir_recursive(const char *src, const char *dst,
                                       int window_id, struct reliefos_ui_surface *ui)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret;
    int dst_type = path_type_nofollow(dst);
    if (dst_type == -2) {
        ret = installer_mkdir_p(dst);
        if (ret < 0 && ret != -17) return ret;
        ret = apply_directory_metadata(src, dst);
        if (ret < 0) return ret;
    } else if (dst_type != RELIEFOS_FS_TYPE_DIR) {
        return -17;
    }
    ret = installer_list_dir(src, &entries, &count);
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count; ++i) {
        char src_child[RELIEFOS_FS_PATH_LEN];
        char dst_child[RELIEFOS_FS_PATH_LEN];
        if (name_is_dot(entries[i].name)) continue;
        if (path_join(src_child, sizeof(src_child), src, entries[i].name) < 0 ||
            path_join(dst_child, sizeof(dst_child), dst, entries[i].name) < 0) {
            free(entries);
            return -1;
        }
        int dst_type = path_type_nofollow(dst_child);
        if (entries[i].type == RELIEFOS_FS_TYPE_DIR) {
            if (dst_type == -2) {
                ret = installer_mkdir_p(dst_child);
                if (ret < 0 && ret != -17) {
                    free(entries);
                    return ret;
                }
                ret = apply_directory_metadata(src_child, dst_child);
            } else if (dst_type == RELIEFOS_FS_TYPE_DIR) {
                ret = 0;
            } else {
                ret = -17;
            }
            if (ret >= 0) {
                ret = merge_missing_dir_recursive(src_child, dst_child, window_id, ui);
            }
        } else if (dst_type == -2) {
            if (entries[i].type == RELIEFOS_FS_TYPE_FILE) {
                ret = copy_file_path(src_child, dst_child, window_id, ui);
            } else if (entries[i].type == RELIEFOS_FS_TYPE_SYMLINK) {
                ret = copy_symlink_path(src_child, dst_child);
            } else {
                ret = -20;
            }
            if (ret >= 0) ++copy_done;
        } else {
            /* Existing user file or user-modified symlink: keep it. */
            ret = 0;
        }
        if (ret < 0) {
            printf("[installer.elf] default merge conflict src=%s dst=%s ret=%d\n",
                   src_child, dst_child, ret);
            free(entries);
            return ret;
        }
    }
    free(entries);
    return 0;
}

static int sync_application_packages(int window_id, struct reliefos_ui_surface *ui)
{
    struct reliefos_dir_entry *entries = NULL;
    uint32_t count = 0;
    int ret = installer_mkdir_p(TARGET_RELIEFOS_APPS);
    if (ret < 0 && ret != -17) return ret;
    /* Optional packages are handled by copy_selected_update_apps. */
    ret = installer_list_dir(INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                             &entries, &count);
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count; ++i) {
        char source[RELIEFOS_FS_PATH_LEN];
        char destination[RELIEFOS_FS_PATH_LEN];
        if (entries[i].type != RELIEFOS_FS_TYPE_DIR || name_is_dot(entries[i].name)) {
            continue;
        }
        if (path_join(source, sizeof(source),
                      INSTALL_ROOT_PAYLOAD RELIEFOS_LAYOUT_RELIEFOS_APPS,
                      entries[i].name) < 0 ||
            path_join(destination, sizeof(destination), TARGET_RELIEFOS_APPS,
                      entries[i].name) < 0) {
            free(entries);
            return -1;
        }
        if (!app_package_is_system(source)) continue;
        ret = overlay_dir_recursive(source, destination, window_id, ui);
        if (ret < 0) {
            free(entries);
            return ret;
        }
    }
    free(entries);
    entries = NULL;
    count = 0;
    /* Remove stale build-managed system packages; keep user packages. */
    ret = installer_list_dir(TARGET_RELIEFOS_APPS, &entries, &count);
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < count; ++i) {
        char package[RELIEFOS_FS_PATH_LEN];
        if (entries[i].type != RELIEFOS_FS_TYPE_DIR || name_is_dot(entries[i].name)) {
            continue;
        }
        if (payload_has_app_package(entries[i].name)) {
            continue;
        }
        if (path_join(package, sizeof(package), TARGET_RELIEFOS_APPS,
                      entries[i].name) < 0) {
            free(entries);
            return -1;
        }
        if (!app_package_is_system(package)) {
            continue;
        }
        ret = remove_path_recursive(package);
        if (ret < 0) {
            free(entries);
            return ret;
        }
        char command[RELIEFOS_FS_PATH_LEN], expected[RELIEFOS_FS_PATH_LEN];
        char current[RELIEFOS_FS_PATH_LEN];
        int size = snprintf(expected, sizeof(expected), "../lib/leonos/apps/%s/%s.elf",
                            entries[i].name, entries[i].name);
        if (path_join(command, sizeof(command), TARGET_USR_BIN, entries[i].name) < 0 ||
            size < 0 || (size_t)size >= sizeof(expected)) {
            free(entries); return -ENAMETOOLONG;
        }
        if (path_type_nofollow(command) == RELIEFOS_FS_TYPE_SYMLINK) {
            ssize_t length = readlink(command, current, sizeof(current) - 1);
            if (length < 0) { free(entries); return -errno; }
            current[length] = 0;
            if (!strcmp(current, expected) && unlink(command) < 0) {
                free(entries); return -errno;
            }
        }
    }
    free(entries);
    return 0;
}

static int sync_system_payload(int window_id, struct reliefos_ui_surface *ui)
{
    pid_t child;
    char *const argv[] = {"sh", "/usr/lib/reliefos/reliefos-apk-update", INSTALL_ROOT_MOUNT,
                         INSTALL_ROOT_PAYLOAD "/usr/share/reliefos/apk/repository", NULL};
    char *const envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "HOME=/root", NULL};
    int ret = posix_spawn(&child, "/bin/sh", NULL, NULL, argv, envp);
    if (ret) return -ret;
    int status;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) return -errno;
        present_installer(window_id, ui);
        usleep(50000);
    }
    printf("[installer.elf] APK update wait_status=%d\n", status);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

static void prepare_update_target(int window_id, struct reliefos_ui_surface *ui)
{
    int ret;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    page = PAGE_PROGRESS;
    progress_value = 0;
    reset_update_app_list();
    show_progress(window_id, ui, 5,
                  T("Mounting target filesystems"),
                  T("Root: /target; ESP: /target/boot"));
    ret = installer_mount_targets(disks[selected_disk].path, 0);
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Mount failed"));
        return;
    }
    if (reliefos_account_legacy_check(INSTALL_ROOT_MOUNT) < 0) {
        finish_install(window_id, ui, -errno,
                       T("Legacy accounts require recovery before updating"));
        return;
    }
    show_progress(window_id, ui, 18,
                  T("Checking existing ReliefOS"),
                  T("Target: /target"));
    ret = check_update_payload_required();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Payload check failed"));
        return;
    }
    ret = check_update_target_required();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Existing system check failed"));
        return;
    }
    if (path_type_nofollow(INSTALL_ROOT_MOUNT "/lib/apk/db/installed") != RELIEFOS_FS_TYPE_FILE) {
        finish_install(window_id, ui, -ENOENT,
                       T("No APK database: reinstall from this media"));
        return;
    }
    set_status(T("Update installed ReliefOS packages"),
               T("Alpine packages and local configuration are preserved."));
    page = PAGE_CONFIRM;
    reset_confirm();
    dirty = 1;
    present_installer(window_id, ui);
}

static void perform_install(int window_id, struct reliefos_ui_surface *ui)
{
    int ret;
    if (!installer_setup_valid(&setup)) {
        finish_install(window_id, ui, -EINVAL, T("Invalid installation settings"));
        return;
    }
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    install_running = 1;
    install_success = 0;
    page = PAGE_PROGRESS;
    copy_total = 0;
    copy_done = 0;
    copy_total_bytes = 0;
    copy_done_bytes = 0;

    show_progress(window_id, ui, 2, "Preparing target disk", "");
    show_progress(window_id, ui, 22, "Mounting target filesystems", "Root: /target (ext4 for new installs; ext4 or exFAT for updates), ESP: /target/boot (FAT32)");
    ret = installer_mount_targets(disks[selected_disk].path, 1);
    if (ret < 0) {
        finish_install(window_id, ui, ret, "Mount failed");
        return;
    }

    show_progress(window_id, ui, 30, "Scanning installation payload", "Root and boot partitions");
    ret = count_files_recursive(INSTALL_ROOT_PAYLOAD, &copy_total);
    if (ret >= 0) {
        ret = count_files_recursive(INSTALL_ESP_PAYLOAD, &copy_total);
    }
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Payload scan failed"));
        return;
    }

    show_progress(window_id, ui, 35, T("Copying system files"),
                  T("Root: /target; boot partition: /target/boot"));
    ret = copy_payload_ordered(window_id, ui);
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Copy failed"));
        return;
    }

    explicit_bzero(setup.password, sizeof(setup.password));
    explicit_bzero(setup.password_confirm, sizeof(setup.password_confirm));
    explicit_bzero(setup.root_password, sizeof(setup.root_password));
    explicit_bzero(setup.root_password_confirm, sizeof(setup.root_password_confirm));
    ret = write_target_preferences();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Could not save installed preferences"));
        return;
    }

    show_progress(window_id, ui, 100, T("Installation completed successfully"), T("Target disk is ready."));
    finish_install(window_id, ui, 0, "");
}

static void perform_update(int window_id, struct reliefos_ui_surface *ui)
{
    static const char *const boot_dirs[] = {"reliefos", "grub", "EFI"};
    int ret;
    if (selected_disk < 0 || (uint32_t)selected_disk >= disk_count) {
        return;
    }
    install_running = 1;
    install_success = 0;
    page = PAGE_PROGRESS;
    copy_total = 0;
    copy_done = 0;
    copy_total_bytes = 0;
    copy_done_bytes = 0;

    show_progress(window_id, ui, 2, T("Mounting target filesystems"),
                  T("Root: /target; ESP: /target/boot"));
    ret = installer_mount_targets(disks[selected_disk].path, 0);
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Mount failed"));
        return;
    }

    show_progress(window_id, ui, 10, T("Checking existing ReliefOS"),
                  T("Target: /target"));
    ret = check_update_payload_required();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Payload check failed"));
        return;
    }
    ret = check_update_target_required();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Existing system check failed"));
        return;
    }
    if (installer_root_filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT) {
        /* The current root contract needs real symlinks; exFAT cannot store
         * them, so an update would silently produce an unusable namespace.
         * Refuse instead of materializing directory copies. */
        finish_install(window_id, ui, -38,
                       T("exFAT root cannot carry the current symlink layout; use a fresh ext4 install"));
        return;
    }

    ret = ensure_runtime_layout_dirs();
    if (ret >= 0) ret = apply_runtime_root_paths();
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Invalid root layout"));
        return;
    }

    show_progress(window_id, ui, 22, T("Scanning update payload"),
                  T("Root and boot payloads"));
    for (uint32_t i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); ++i) {
        char src[RELIEFOS_FS_PATH_LEN];
        if (path_join(src, sizeof(src), INSTALL_ESP_PAYLOAD, boot_dirs[i]) < 0) {
            finish_install(window_id, ui, -1, T("Payload path is too long"));
            return;
        }
        ret = count_files_recursive(src, &copy_total);
        if (ret < 0) {
            finish_install(window_id, ui, ret, T("Payload scan failed"));
            return;
        }
    }
    show_progress(window_id, ui, 35, T("Upgrading signed ReliefOS packages"),
                  T("Checking dependencies and preserving local configuration"));
    ret = sync_system_payload(window_id, ui);
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Core update failed"));
        return;
    }

    /* Publish boot files only after the root payload is complete. Each file
     * is replaced after its copy succeeds; never delete the EFI/GRUB tree. */
    for (uint32_t i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); ++i) {
        char src[RELIEFOS_FS_PATH_LEN], dst[RELIEFOS_FS_PATH_LEN];
        if (path_join(src, sizeof(src), INSTALL_ESP_PAYLOAD, boot_dirs[i]) < 0 ||
            path_join(dst, sizeof(dst), INSTALL_ESP_MOUNT, boot_dirs[i]) < 0) {
            finish_install(window_id, ui, -ENAMETOOLONG, T("Boot update failed"));
            return;
        }
        ret = !strcmp(boot_dirs[i], "grub")
            ? overlay_dir_recursive_filtered(src, dst, window_id, ui,
                                             INSTALL_ESP_PAYLOAD "/grub/grub.cfg")
            : overlay_dir_recursive(src, dst, window_id, ui);
        if (ret < 0) {
            finish_install(window_id, ui, ret, T("Boot update failed"));
            return;
        }
    }
    /* Publish the new boot menu only after its kernel, loader and EFI image
     * are present. The config's legacy entry keeps the untouched old payload
     * under /leonos available if a subsequent boot fails. */
    ret = copy_file_path(INSTALL_ESP_PAYLOAD "/grub/grub.cfg",
                         INSTALL_ESP_MOUNT "/grub/grub.cfg", window_id, ui);
    if (ret < 0) {
        finish_install(window_id, ui, ret, T("Boot update failed"));
        return;
    }

    show_progress(window_id, ui, 100, T("Update completed successfully"),
                  T("Target disk is ready."));
    finish_install(window_id, ui, 0, "");
}

static void go_back(void)
{
    if (page == PAGE_THANKS) {
        page = PAGE_LANGUAGE;
    } else if (page == PAGE_THEME) {
        page = PAGE_THANKS;
    } else if (page == PAGE_WELCOME) {
        page = PAGE_THEME;
    } else if (page == PAGE_MODE) {
        page = PAGE_WELCOME;
    } else if (page == PAGE_DISK) {
        page = PAGE_MODE;
    } else if (page == PAGE_UPDATE_APPS) {
        page = PAGE_DISK;
    } else if (page == PAGE_CONFIRM) {
        page = install_mode == INSTALL_MODE_UPDATE ? PAGE_DISK : PAGE_ACCOUNTS;
    } else if (page == PAGE_ACCOUNTS) {
        page = PAGE_DISK;
    } else if (page == PAGE_FINISH && !install_success) {
        page = PAGE_DISK;
    }
    dirty = 1;
}

static int go_primary(int window_id, struct reliefos_ui_surface *ui)
{
    if (primary_disabled()) {
        return 0;
    }
    if (page == PAGE_LANGUAGE) {
        page = PAGE_THANKS;
        dirty = 1;
        return 0;
    }
    if (page == PAGE_THANKS) {
        page = PAGE_THEME;
        dirty = 1;
        return 0;
    }
    if (page == PAGE_THEME) {
        page = PAGE_WELCOME;
        dirty = 1;
        return 0;
    }
    if (page == PAGE_WELCOME) {
        page = PAGE_MODE;
        dirty = 1;
        return 0;
    }
    if (page == PAGE_MODE) {
        page = PAGE_DISK;
        refresh_disks();
        dirty = 1;
        return 0;
    }
    if (page == PAGE_DISK) {
        if (install_mode == INSTALL_MODE_UPDATE) {
            prepare_update_target(window_id, ui);
            return 0;
        }
        page = PAGE_ACCOUNTS;
        dirty = 1;
        return 0;
    }
    if (page == PAGE_UPDATE_APPS) {
        page = PAGE_CONFIRM;
        reset_confirm();
        dirty = 1;
        return 0;
    }
    if (page == PAGE_ACCOUNTS) {
        page = PAGE_CONFIRM;
        reset_confirm();
        dirty = 1;
        return 0;
    }
    if (page == PAGE_CONFIRM) {
        if (install_mode == INSTALL_MODE_UPDATE) {
            perform_update(window_id, ui);
        } else {
            perform_install(window_id, ui);
        }
        return 0;
    }
    if (page == PAGE_FINISH && install_success) {
        fprintf(stderr, "[installer.elf] restart requested from completion page\n");
        if (reliefos_system_reboot() < 0) {
            reboot_error = errno;
            fprintf(stderr, "[installer.elf] restart failed: %s\n", strerror(reboot_error));
            dirty = 1;
        }
        return 0;
    }
    if (page == PAGE_FINISH) {
        return 1;
    }
    return 0;
}

static void handle_disk_click(int32_t x, int32_t y)
{
    struct installer_layout l = get_layout();
    if (hit_rect_i(x, y, (int32_t)l.disk_refresh_x, (int32_t)l.disk_refresh_y, 92, BUTTON_H)) {
        refresh_disks();
        return;
    }
    if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)l.disk_list_y,
                   (int32_t)l.table_w, (int32_t)l.disk_list_h)) {
        int32_t row = (y - (int32_t)l.disk_list_y - 2) / 24;
        if (row >= 0 && (uint32_t)row < disk_count) {
            selected_disk = row;
            set_disk_select_status();
            dirty = 1;
        }
    }
}

static void handle_mode_click(int32_t x, int32_t y)
{
    struct installer_layout l = get_layout();
    uint32_t card_w = l.content_w > 620 ? 280 : l.content_w;
    if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)l.content_y + 84,
                   (int32_t)card_w, BUTTON_H)) {
        install_mode = INSTALL_MODE_FRESH;
        reset_update_app_list();
        dirty = 1;
    } else if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)l.content_y + 180,
                          (int32_t)card_w, BUTTON_H)) {
        install_mode = INSTALL_MODE_UPDATE;
        reset_update_app_list();
        dirty = 1;
    }
}

static void installer_apply_language_font(void)
{
    if (language_selection() == 1) {
        /* SimSun supplies the CJK glyphs used throughout the Chinese UI. */
        (void)reliefos_ui_set_font_fallback_path(0);
        (void)reliefos_ui_set_font_path(INSTALLER_CJK_FONT);
    } else {
        /* The language page contains the native-language "中文" selector.
         * Use the complete face here so it renders deterministically before
         * any fallback glyph cache has been warmed. */
        (void)reliefos_ui_set_font_fallback_path(0);
        (void)reliefos_ui_set_font_path(INSTALLER_CJK_FONT);
    }
}

static void handle_language_click(int32_t x, int32_t y)
{
    struct installer_layout l = get_layout();
    unsigned language = language_selection();
    if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)l.content_y + 88, 140, BUTTON_H)) {
        language = 0;
    } else if (hit_rect_i(x, y, (int32_t)l.content_x + 156, (int32_t)l.content_y + 88, 140, BUTTON_H)) {
        language = 1;
    }
    if (language != language_selection()) {
        if (!setlocale(LC_ALL, language_options[language].locale)) return;
        installer_apply_language_font();
        acknowledgements_scroll_y = 0;
        dirty = 1;
    }
}

static void handle_acknowledgements_click(int32_t x, int32_t y)
{
    struct installer_text_view view = get_acknowledgements_view();
    uint32_t total_h;
    acknowledgements_reflow(view.text_w);
    total_h = policy_total_height();
    if (reliefos_ui_vscrollbar_handle_mouse(&acknowledgements_scroll_y,
                                           total_h > view.h ? total_h : view.h,
                                           view.h,
                                           view.x + view.w - POLICY_SCROLLBAR_W,
                                           view.y, POLICY_SCROLLBAR_W, view.h,
                                           x, y)) {
        dirty = 1;
    }
}

static void handle_acknowledgements_wheel(int32_t delta)
{
    struct installer_text_view view = get_acknowledgements_view();
    uint32_t total_h;
    uint32_t steps = delta < 0 ? (uint32_t)(-delta) : (uint32_t)delta;
    int32_t pixels;
    if (!steps) {
        steps = 1;
    }
    acknowledgements_reflow(view.text_w);
    total_h = policy_total_height();
    pixels = delta > 0 ? (int32_t)(steps * 36U) : -(int32_t)(steps * 36U);
    if (reliefos_ui_vscrollbar_handle_wheel(&acknowledgements_scroll_y,
                                          total_h > view.h ? total_h : view.h,
                                          view.h, pixels)) {
        dirty = 1;
    }
}

static void handle_theme_click(int32_t x, int32_t y)
{
    struct installer_layout l = get_layout();
    uint32_t theme = installer_theme;
    uint8_t selected = 0;
    if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)l.content_y + 88,
                   140, BUTTON_H)) {
        theme = RELIEFOS_UI_THEME_METRO;
        selected = 1;
    } else if (hit_rect_i(x, y, (int32_t)l.content_x + 156,
                          (int32_t)l.content_y + 88, 140, BUTTON_H)) {
        theme = RELIEFOS_UI_THEME_WIN95;
        selected = 1;
    }
    if (selected) {
        installer_theme = (uint8_t)theme;
        installer_theme_explicit = 1;
        (void)reliefos_ui_theme_set(theme);
        dirty = 1;
    }
}

static void handle_update_apps_click(int32_t x, int32_t y)
{
    struct installer_layout l = get_layout();
    uint32_t header_y = l.content_y + 112;
    uint32_t list_y = header_y + 24;
    uint32_t list_h = l.content_h > 220 ? l.content_h - 178 : 120;
    uint32_t list_w = l.table_w > 22 ? l.table_w - 22 : l.table_w;
    sync_update_list_layout(list_h);
    if (reliefos_ui_vscrollbar_handle_mouse(&update_app_list.scroll,
                                           update_app_count > update_app_list.visible_rows
                                               ? update_app_count : update_app_list.visible_rows,
                                           update_app_list.visible_rows,
                                           l.content_x + list_w, list_y, 18, list_h, x, y)) {
        dirty = 1;
        return;
    }
    if (hit_rect_i(x, y, (int32_t)l.content_x, (int32_t)list_y,
                   (int32_t)list_w, (int32_t)list_h)) {
        int32_t row = (y - (int32_t)list_y - 2) / (int32_t)UPDATE_APP_ROW_H;
        uint32_t index;
        if (row < 0) {
            return;
        }
        index = update_app_list.scroll + (uint32_t)row;
        if (index >= update_app_count) {
            return;
        }
        update_app_list.selected = (int32_t)index;
        update_apps[index].selected = update_apps[index].selected ? 0 : 1;
        dirty = 1;
    }
}

static void handle_update_apps_wheel(int32_t delta)
{
    if (reliefos_ui_listview_state_handle_wheel(&update_app_list, delta)) {
        dirty = 1;
    }
}

static int handle_mouse(int window_id, struct reliefos_ui_surface *ui,
                        const struct reliefos_gui_app_event *event)
{
    struct installer_layout l = get_layout();
    if (!(event->buttons & 1u)) {
        return 0;
    }
    if (page == PAGE_ACCOUNTS) {
        for (unsigned i = 0; i < 5; ++i) {
            if (hit_rect_i(event->x, event->y, l.content_x, l.content_y + 84 + i * 54,
                           account_width(), RELIEFOS_FONT_H + 8)) {
                for (unsigned j = 0; j < 5; ++j) account_edits[j].focused = 0;
                account_focus = i;
                reliefos_ui_edit_state_handle_mouse(&account_edits[i], event->x, event->y,
                    l.content_x, l.content_y + 84 + i * 54, account_width(), event->buttons);
                dirty = 1;
                break;
            }
        }
    }
    if (page == PAGE_CONFIRM &&
        reliefos_ui_edit_state_handle_mouse(&confirm_edit, event->x, event->y,
                                          l.content_x, l.confirm_edit_y, 220, event->buttons)) {
        dirty = 1;
    }
    if (page == PAGE_DISK) {
        handle_disk_click(event->x, event->y);
    }
    if (page == PAGE_MODE) {
        handle_mode_click(event->x, event->y);
    }
    if (page == PAGE_UPDATE_APPS) {
        handle_update_apps_click(event->x, event->y);
    }
    if (page == PAGE_LANGUAGE) {
        handle_language_click(event->x, event->y);
    }
    if (page == PAGE_THANKS) {
        handle_acknowledgements_click(event->x, event->y);
    }
    if (page == PAGE_THEME) {
        handle_theme_click(event->x, event->y);
    }
    if (!install_running &&
        hit_rect_i(event->x, event->y, (int32_t)l.back_x, (int32_t)l.button_y, BUTTON_W, BUTTON_H)) {
        if (!(page == PAGE_LANGUAGE || page == PAGE_PROGRESS ||
              (page == PAGE_FINISH && install_success))) {
            go_back();
        }
    } else if (!install_running &&
               hit_rect_i(event->x, event->y, (int32_t)l.next_x, (int32_t)l.button_y, BUTTON_W, BUTTON_H)) {
        if (go_primary(window_id, ui)) {
            return 1;
        }
    } else if (!install_running &&
               hit_rect_i(event->x, event->y, (int32_t)l.cancel_x, (int32_t)l.button_y, BUTTON_W, BUTTON_H)) {
        if (!(page == PAGE_FINISH && install_success)) {
            return 1;
        }
    }
    return 0;
}

static int handle_key(int window_id, struct reliefos_ui_surface *ui,
                      const struct reliefos_gui_app_event *event)
{
    if (page == PAGE_ACCOUNTS) {
        if (event->pressed && (event->keycode == 15 || event->keycode == RELIEFOS_KEY_ENTER)) {
            if (event->keycode == RELIEFOS_KEY_ENTER && account_focus == 4)
                return go_primary(window_id, ui);
            account_edits[account_focus].focused = 0;
            account_focus = (account_focus + 1) % 5;
            account_edits[account_focus].focused = 1;
            dirty = 1;
            return 0;
        }
        if (reliefos_ui_edit_state_handle_key(&account_edits[account_focus], event->keycode, event->pressed))
            dirty = 1;
        if (event->keycode != KEY_ESCAPE) return 0;
    }
    if (event->type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event->pressed) {
        if (event->keycode == KEY_ESCAPE && page != PAGE_PROGRESS) {
            return 1;
        }
        if (page == PAGE_UPDATE_APPS) {
            uint32_t activated = 0;
            if (event->keycode == KEY_SPACE) {
                if (update_app_list.selected >= 0 &&
                    (uint32_t)update_app_list.selected < update_app_count) {
                    uint32_t i = (uint32_t)update_app_list.selected;
                    update_apps[i].selected = update_apps[i].selected ? 0 : 1;
                    dirty = 1;
                }
                return 0;
            }
            if (event->keycode != RELIEFOS_KEY_ENTER &&
                reliefos_ui_listview_state_handle_key(&update_app_list,
                                                    event->keycode, &activated)) {
                dirty = 1;
                return 0;
            }
        }
        if (page == PAGE_DISK) {
            if (event->keycode == KEY_UP && selected_disk > 0) {
                --selected_disk;
                dirty = 1;
                return 0;
            }
            if (event->keycode == KEY_DOWN &&
                selected_disk >= 0 && (uint32_t)(selected_disk + 1) < disk_count) {
                ++selected_disk;
                dirty = 1;
                return 0;
            }
        }
        if (event->keycode == RELIEFOS_KEY_ENTER && page != PAGE_PROGRESS) {
            return go_primary(window_id, ui);
        }
    }
    if (page == PAGE_CONFIRM &&
        (event->type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
         event->type == RELIEFOS_GUI_APP_EVENT_KEY_UP)) {
        if (reliefos_ui_edit_state_handle_key(&confirm_edit, event->keycode,
                                            event->pressed)) {
            dirty = 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    struct installer_tty_context tty_context;
    int window_id;

    setvbuf(stdout, NULL, _IOLBF, 0);
    reliefos_ui_edit_state_init(&account_edits[0], setup.username, sizeof(setup.username));
    reliefos_ui_edit_state_init(&account_edits[1], setup.password, sizeof(setup.password));
    reliefos_ui_edit_state_init(&account_edits[2], setup.password_confirm, sizeof(setup.password_confirm));
    reliefos_ui_edit_state_init(&account_edits[3], setup.root_password, sizeof(setup.root_password));
    reliefos_ui_edit_state_init(&account_edits[4], setup.root_password_confirm, sizeof(setup.root_password_confirm));
    account_edits[0].focused = 1;

    int graphical = argc == 2 && strcmp(argv[1], "--graphical") == 0;
    if (argc != 1 && !graphical) {
        fputs("usage: installer [--graphical]\n", stderr);
        return 2;
    }
    if (!graphical && isatty(STDIN_FILENO)) {
        installer_tty_mode = 1;

        tty_context.disks = disks;
        tty_context.setup = &setup;
        tty_context.disk_count = &disk_count;
        tty_context.selected_disk = &selected_disk;
        tty_context.install_mode = &install_mode;
        tty_context.install_success = &install_success;
        tty_context.page = &page;
        tty_context.update_apps_page = PAGE_UPDATE_APPS;
        tty_context.refresh_disks = refresh_disks;
        tty_context.format_disk_line = format_disk_line;
        tty_context.print_update_packages = tty_print_update_packages;
        tty_context.prepare_update = tty_prepare_update;
        tty_context.perform_install = tty_perform_install;
        tty_context.perform_update = tty_perform_update;
        return installer_tty_main(&tty_context);
    }
    puts("[installer.elf] starting installer wizard");
    update_surface_size_from_framebuffer();
    window_id = reliefos_gui_create_app_window_ex("ReliefOS Setup", "Install ReliefOS",
                                                surface_w, surface_h,
                                                RELIEFOS_GUI_WINDOW_FULLSCREEN);
    if (window_id <= 0) {
        printf("[installer.elf] create window failed=%d\n", window_id);
        return 1;
    }

    reliefos_ui_bind(&ui, pixels, surface_w, surface_h, INSTALLER_MAX_W);

    installer_theme = (uint8_t)reliefos_ui_theme();
    installer_apply_language_font();
    reliefos_ui_listview_state_init(&update_app_list, 1, UPDATE_APP_ROW_H);
    refresh_disks();
    page = PAGE_LANGUAGE;
    present_installer(window_id, &ui);

    for (;;) {
        uint32_t event_count = 0;
        event.window_id = (uint32_t)window_id;
        /* Wait only when idle. A fresh idle wait after every event prevents
         * painting for as long as mouse motion keeps arriving. */
        while (event_count < INSTALLER_EVENT_BATCH_MAX &&
               (event_count == 0
                    ? reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS)
                    : reliefos_gui_poll_app_event(&event)) > 0) {
            ++event_count;
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                reliefos_gui_destroy_app_window((uint32_t)window_id);
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                update_surface_size(event.width, event.height);
                reliefos_ui_bind(&ui, pixels, surface_w, surface_h, INSTALLER_MAX_W);
                dirty = 1;
                break;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
                handle_mouse(window_id, &ui, &event)) {
                reliefos_gui_destroy_app_window((uint32_t)window_id);
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (page == PAGE_UPDATE_APPS) {
                    handle_update_apps_wheel(event.dy);
                } else if (page == PAGE_THANKS) {
                    handle_acknowledgements_wheel(event.dy);
                }
            }
            if ((event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
                 event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) &&
                handle_key(window_id, &ui, &event)) {
                reliefos_gui_destroy_app_window((uint32_t)window_id);
                return 0;
            }
            if (dirty) {
                break;
            }
        }
        if (dirty) {
            present_installer(window_id, &ui);
        }
    }
}
