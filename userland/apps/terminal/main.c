#include <reliefos/gui.h>
#include <reliefos/environment.h>
#include <reliefos/launch.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <pty.h>
#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#define TERMINAL_DEFAULT_W 760U
#define TERMINAL_DEFAULT_H 480U
#define TERMINAL_MIN_W 160U
#define TERMINAL_MIN_H 96U
#define TERMINAL_MAX_W 1920U
#define TERMINAL_MAX_H 1080U
#define TERMINAL_MAX_SESSIONS 8U
#define TERMINAL_HISTORY_ROWS 160U
#define TERMINAL_TAB_ADD_W 28U
#define TERMINAL_BODY_X 0U
#define TERMINAL_MAX_COLUMNS (TERMINAL_MAX_W / RELIEFOS_FONT_W)
#define TERMINAL_CSI_PARAM_CAP 12U
#define TERMINAL_OSC_CAP 64U
#define TERMINAL_DEFAULT_FOREGROUND 0x00d7e3f4U
#define TERMINAL_DEFAULT_BACKGROUND 0x00000000U
/* Keep a complete typical curses redraw in one terminal frame.  Smaller
 * batches make full-screen programs redraw and present the window repeatedly. */
#define TERMINAL_OUTPUT_BUDGET 8192U
#define TERMINAL_CELL_CONTINUATION 0xffffffffU
#define TERMINAL_KEY_T 20U
#define TERMINAL_KEY_W 17U
#define T(s) gettext(s)

enum terminal_escape_state {
    TERMINAL_TEXT,
    TERMINAL_ESCAPE,
    TERMINAL_CSI,
    TERMINAL_OSC,
    TERMINAL_OSC_ESCAPE,
};

struct terminal_cell {
    uint32_t codepoint;
    uint32_t foreground;
    uint32_t background;
};

struct terminal_line {
    struct terminal_cell cells[TERMINAL_MAX_COLUMNS];
};

struct terminal_screen_snapshot {
    struct terminal_line lines[TERMINAL_HISTORY_ROWS];
    uint32_t snapshot_history_first;
    uint32_t snapshot_history_count;
    uint32_t snapshot_active_line;
    uint32_t snapshot_cursor_column;
    uint32_t snapshot_saved_line;
    uint32_t snapshot_saved_column;
    uint32_t snapshot_text_foreground;
    uint32_t snapshot_text_background;
    int8_t snapshot_foreground_index;
    int8_t snapshot_background_index;
    uint8_t snapshot_text_bright;
    uint8_t snapshot_text_dim;
    uint8_t snapshot_text_inverse;
    uint8_t snapshot_background_bright;
    uint8_t snapshot_cursor_visible;
    uint8_t snapshot_saved_cursor_valid;
};

static uint32_t pixels[TERMINAL_MAX_W * TERMINAL_MAX_H];

struct terminal_session {
    struct terminal_line history[TERMINAL_HISTORY_ROWS];
    struct terminal_screen_snapshot normal_screen;
    uint32_t history_first;
    uint32_t history_count;
    uint32_t active_line;
    uint32_t cursor_column;
    uint32_t saved_line;
    uint32_t saved_column;
    int pty_fd;
    pid_t child_pid;
    uint32_t text_foreground;
    uint32_t text_background;
    int8_t foreground_index;
    int8_t background_index;
    uint8_t used;
    uint8_t text_bright;
    uint8_t text_dim;
    uint8_t text_inverse;
    uint8_t background_bright;
    uint8_t cursor_visible;
    uint8_t saved_cursor_valid;
    enum terminal_escape_state escape_state;
    uint16_t csi_params[TERMINAL_CSI_PARAM_CAP];
    uint8_t csi_param_count;
    uint8_t csi_private;
    char osc[TERMINAL_OSC_CAP];
    uint8_t osc_length;
    uint8_t osc_overflow;
    uint8_t utf8_bytes[4];
    uint8_t utf8_length;
    uint8_t utf8_expected;
    uint8_t alternate_screen_active;
    /* The PTY host supplies canonical-mode echo, so keep enough state to
     * erase only text entered after the current prompt, not the prompt. */
    uint32_t local_echoed_input_count;
};

static struct terminal_session sessions[TERMINAL_MAX_SESSIONS];
static struct terminal_session *active_session;
static struct reliefos_ui_tab_state tabs_state;
static uint32_t terminal_view_width = TERMINAL_DEFAULT_W;
static uint32_t terminal_view_height = TERMINAL_DEFAULT_H;
static const char *const terminal_tab_labels[TERMINAL_MAX_SESSIONS] = {
    "1", "2", "3", "4", "5", "6", "7", "8",
};

#define history (active_session->history)
#define normal_screen (active_session->normal_screen)
#define history_first (active_session->history_first)
#define history_count (active_session->history_count)
#define active_line (active_session->active_line)
#define cursor_column (active_session->cursor_column)
#define saved_line (active_session->saved_line)
#define saved_column (active_session->saved_column)
#define active_pty_fd (active_session->pty_fd)
#define text_foreground (active_session->text_foreground)
#define text_background (active_session->text_background)
#define foreground_index (active_session->foreground_index)
#define background_index (active_session->background_index)
#define text_bright (active_session->text_bright)
#define text_dim (active_session->text_dim)
#define text_inverse (active_session->text_inverse)
#define background_bright (active_session->background_bright)
#define cursor_visible (active_session->cursor_visible)
#define saved_cursor_valid (active_session->saved_cursor_valid)
#define escape_state (active_session->escape_state)
#define csi_params (active_session->csi_params)
#define csi_param_count (active_session->csi_param_count)
#define csi_private (active_session->csi_private)
#define utf8_bytes (active_session->utf8_bytes)
#define utf8_length (active_session->utf8_length)
#define utf8_expected (active_session->utf8_expected)
#define alternate_screen_active (active_session->alternate_screen_active)
#define local_echoed_input_count (active_session->local_echoed_input_count)

static uint32_t terminal_visible_rows(void);
static uint32_t terminal_columns(void);
static void terminal_flush_utf8(void);
static int terminal_write_input(const char *buffer, uint32_t length);
static uint32_t terminal_tab_items(struct reliefos_ui_tab_item *items);
static int terminal_close_session(uint32_t id);

static const uint32_t ansi_normal[8] = {
    0x00708090U, 0x00d06060U, 0x006fc77aU, 0x00d5b85aU,
    0x007f9ee8U, 0x00c38bd7U, 0x006bc8d8U, 0x00d7e3f4U,
};

static const uint32_t ansi_bright[8] = {
    0x0092a1b2U, 0x00ff8484U, 0x008de39aU, 0x00f0d37bU,
    0x009bb7ffU, 0x00e8a5ffU, 0x0088e8faU, 0x00ffffffU,
};

static int terminal_arg_eq(const char *left, const char *right)
{
    if (!left || !right) {
        return 0;
    }
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static uint32_t terminal_dim_color(uint32_t color)
{
    return (color & 0x00fefefeU) >> 1U;
}

static uint32_t terminal_style_foreground(void)
{
    uint32_t color = foreground_index < 0 ? text_foreground :
                     (text_bright ? ansi_bright : ansi_normal)[(uint8_t)foreground_index];
    if (text_dim) {
        color = terminal_dim_color(color);
    }
    if (text_inverse) {
        return background_index < 0 ? text_background :
               (background_bright ? ansi_bright : ansi_normal)[(uint8_t)background_index];
    }
    return color;
}

static uint32_t terminal_style_background(void)
{
    uint32_t color = background_index < 0 ? text_background :
                     (background_bright ? ansi_bright : ansi_normal)[(uint8_t)background_index];
    if (!text_inverse) {
        return color;
    }
    color = foreground_index < 0 ? text_foreground :
            (text_bright ? ansi_bright : ansi_normal)[(uint8_t)foreground_index];
    return text_dim ? terminal_dim_color(color) : color;
}

static void terminal_reset_style(void)
{
    text_foreground = TERMINAL_DEFAULT_FOREGROUND;
    text_background = TERMINAL_DEFAULT_BACKGROUND;
    foreground_index = -1;
    background_index = -1;
    text_bright = 0;
    text_dim = 0;
    text_inverse = 0;
    background_bright = 0;
}

static void terminal_save_normal_screen(void)
{
    uint32_t row;
    for (row = 0; row < TERMINAL_HISTORY_ROWS; ++row) {
        normal_screen.lines[row] = history[row];
    }
    normal_screen.snapshot_history_first = history_first;
    normal_screen.snapshot_history_count = history_count;
    normal_screen.snapshot_active_line = active_line;
    normal_screen.snapshot_cursor_column = cursor_column;
    normal_screen.snapshot_saved_line = saved_line;
    normal_screen.snapshot_saved_column = saved_column;
    normal_screen.snapshot_text_foreground = text_foreground;
    normal_screen.snapshot_text_background = text_background;
    normal_screen.snapshot_foreground_index = foreground_index;
    normal_screen.snapshot_background_index = background_index;
    normal_screen.snapshot_text_bright = text_bright;
    normal_screen.snapshot_text_dim = text_dim;
    normal_screen.snapshot_text_inverse = text_inverse;
    normal_screen.snapshot_background_bright = background_bright;
    normal_screen.snapshot_cursor_visible = cursor_visible;
    normal_screen.snapshot_saved_cursor_valid = saved_cursor_valid;
}

static void terminal_restore_normal_screen(void)
{
    uint32_t row;
    for (row = 0; row < TERMINAL_HISTORY_ROWS; ++row) {
        history[row] = normal_screen.lines[row];
    }
    history_first = normal_screen.snapshot_history_first;
    history_count = normal_screen.snapshot_history_count;
    active_line = normal_screen.snapshot_active_line;
    cursor_column = normal_screen.snapshot_cursor_column;
    saved_line = normal_screen.snapshot_saved_line;
    saved_column = normal_screen.snapshot_saved_column;
    text_foreground = normal_screen.snapshot_text_foreground;
    text_background = normal_screen.snapshot_text_background;
    foreground_index = normal_screen.snapshot_foreground_index;
    background_index = normal_screen.snapshot_background_index;
    text_bright = normal_screen.snapshot_text_bright;
    text_dim = normal_screen.snapshot_text_dim;
    text_inverse = normal_screen.snapshot_text_inverse;
    background_bright = normal_screen.snapshot_background_bright;
    cursor_visible = normal_screen.snapshot_cursor_visible;
    saved_cursor_valid = normal_screen.snapshot_saved_cursor_valid;
}

static void terminal_blank_cell(struct terminal_cell *cell)
{
    cell->codepoint = 0;
    cell->foreground = terminal_style_foreground();
    cell->background = terminal_style_background();
}

static void terminal_reset_line(struct terminal_line *line)
{
    uint32_t column;
    for (column = 0; column < TERMINAL_MAX_COLUMNS; ++column) {
        terminal_blank_cell(&line->cells[column]);
    }
}

static uint32_t terminal_columns(void)
{
    uint32_t columns = terminal_view_width / RELIEFOS_FONT_W;
    if (columns == 0) {
        return 1;
    }
    return columns > TERMINAL_MAX_COLUMNS ? TERMINAL_MAX_COLUMNS : columns;
}

static void terminal_normalize_cursor(void)
{
    uint32_t columns = terminal_columns();
    if (cursor_column > columns) {
        cursor_column = columns;
    }
    if (active_line >= TERMINAL_HISTORY_ROWS) {
        active_line = 0;
    }
    if (history_count == 0) {
        history_first = 0;
        history_count = 1;
        active_line = 0;
    } else if (history_count > TERMINAL_HISTORY_ROWS) {
        history_count = TERMINAL_HISTORY_ROWS;
    }
}

static uint32_t terminal_visible_rows(void)
{
    uint32_t body_y = reliefos_ui_tab_height();
    uint32_t rows;
    if (terminal_view_height <= body_y) {
        return 1;
    }
    rows = (terminal_view_height - body_y) / RELIEFOS_FONT_H;
    return rows ? rows : 1;
}

static void terminal_normalize_line_width(struct terminal_line *line,
                                          uint32_t columns)
{
    uint32_t column;
    if (!line || columns == 0 || columns > TERMINAL_MAX_COLUMNS) {
        return;
    }

    /* Do not leave half of a double-width glyph at the new right edge. */
    if (columns < TERMINAL_MAX_COLUMNS &&
        line->cells[columns].codepoint == TERMINAL_CELL_CONTINUATION) {
        terminal_blank_cell(&line->cells[columns - 1U]);
    }
    for (column = columns; column < TERMINAL_MAX_COLUMNS; ++column) {
        terminal_blank_cell(&line->cells[column]);
    }
}

static void terminal_resize_active_session_grid(void)
{
    uint32_t row;
    uint32_t columns = terminal_columns();

    if (!active_session || !active_session->used) {
        return;
    }
    for (row = 0; row < TERMINAL_HISTORY_ROWS; ++row) {
        terminal_normalize_line_width(&history[row], columns);
    }
    if (alternate_screen_active) {
        for (row = 0; row < TERMINAL_HISTORY_ROWS; ++row) {
            terminal_normalize_line_width(&normal_screen.lines[row], columns);
        }
        if (normal_screen.snapshot_cursor_column > columns) {
            normal_screen.snapshot_cursor_column = columns;
        }
        if (normal_screen.snapshot_saved_column > columns) {
            normal_screen.snapshot_saved_column = columns;
        }
    }
    if (cursor_column > columns) {
        cursor_column = columns;
    }
    if (saved_column > columns) {
        saved_column = columns;
    }
}

static void terminal_sync_session_winsize(const struct terminal_session *session)
{
    struct winsize winsize;
    if (!session || !session->used || session->pty_fd < 0) {
        return;
    }
    winsize.ws_row = (uint16_t)terminal_visible_rows();
    winsize.ws_col = (uint16_t)terminal_columns();
    (void)tcsetwinsize(session->pty_fd, &winsize);
}

static int terminal_resize_view(uint32_t width, uint32_t height)
{
    struct terminal_session *selected = active_session;
    uint32_t previous_width = terminal_view_width;
    uint32_t previous_height = terminal_view_height;

    if (width < TERMINAL_MIN_W) {
        width = TERMINAL_MIN_W;
    } else if (width > TERMINAL_MAX_W) {
        width = TERMINAL_MAX_W;
    }
    if (height < TERMINAL_MIN_H) {
        height = TERMINAL_MIN_H;
    } else if (height > TERMINAL_MAX_H) {
        height = TERMINAL_MAX_H;
    }
    terminal_view_width = width;
    terminal_view_height = height;

    for (uint32_t index = 0; index < TERMINAL_MAX_SESSIONS; ++index) {
        if (!sessions[index].used) {
            continue;
        }
        active_session = &sessions[index];
        terminal_resize_active_session_grid();
        terminal_sync_session_winsize(active_session);
    }
    active_session = selected;
    return previous_width != width || previous_height != height;
}

static uint32_t terminal_logical_active(void)
{
    uint32_t row;
    for (row = 0; row < history_count; ++row) {
        if ((history_first + row) % TERMINAL_HISTORY_ROWS == active_line) {
            return row;
        }
    }
    return history_count ? history_count - 1U : 0;
}

static void terminal_select_logical(uint32_t row)
{
    if (history_count == 0) {
        return;
    }
    if (row >= history_count) {
        row = history_count - 1U;
    }
    active_line = (history_first + row) % TERMINAL_HISTORY_ROWS;
}

static struct terminal_line *terminal_current_line(void)
{
    terminal_normalize_cursor();
    return &history[active_line];
}

static void terminal_append_line(void)
{
    if (history_count < TERMINAL_HISTORY_ROWS) {
        active_line = (history_first + history_count) % TERMINAL_HISTORY_ROWS;
        ++history_count;
    } else {
        if (saved_cursor_valid && saved_line == history_first) {
            saved_cursor_valid = 0;
        }
        history_first = (history_first + 1U) % TERMINAL_HISTORY_ROWS;
        active_line = (history_first + history_count - 1U) % TERMINAL_HISTORY_ROWS;
    }
    cursor_column = 0;
    terminal_reset_line(terminal_current_line());
}

static void terminal_newline(void)
{
    uint32_t row = terminal_logical_active();
    if (row + 1U < history_count) {
        terminal_select_logical(row + 1U);
        cursor_column = 0;
        return;
    }
    terminal_append_line();
}

static void terminal_clear(void)
{
    uint32_t row;
    history_first = 0;
    history_count = 1;
    active_line = 0;
    cursor_column = 0;
    saved_cursor_valid = 0;
    local_echoed_input_count = 0;
    for (row = 0; row < TERMINAL_HISTORY_ROWS; ++row) {
        terminal_reset_line(&history[row]);
    }
}

static void terminal_enter_alternate_screen(void)
{
    if (alternate_screen_active) {
        return;
    }
    terminal_flush_utf8();
    terminal_save_normal_screen();
    alternate_screen_active = 1;
    terminal_reset_style();
    terminal_clear();
}

static void terminal_leave_alternate_screen(void)
{
    if (!alternate_screen_active) {
        return;
    }
    terminal_flush_utf8();
    terminal_restore_normal_screen();
    alternate_screen_active = 0;
}

static uint32_t terminal_codepoint_width(uint32_t codepoint)
{
    if (codepoint >= 0x1100U &&
        (codepoint <= 0x115fU ||
         (codepoint >= 0x2e80U && codepoint <= 0xa4cfU) ||
         (codepoint >= 0xac00U && codepoint <= 0xd7a3U) ||
         (codepoint >= 0xf900U && codepoint <= 0xfaffU) ||
         (codepoint >= 0xfe10U && codepoint <= 0xfe6fU) ||
         (codepoint >= 0xff01U && codepoint <= 0xff60U) ||
         (codepoint >= 0xffe0U && codepoint <= 0xffe6U))) {
        return 2;
    }
    return 1;
}

static void terminal_prepare_cell(struct terminal_line *line, uint32_t column)
{
    if (column >= terminal_columns()) {
        return;
    }
    if (line->cells[column].codepoint == TERMINAL_CELL_CONTINUATION && column > 0) {
        terminal_blank_cell(&line->cells[column - 1U]);
    }
    if (column + 1U < terminal_columns() &&
        line->cells[column + 1U].codepoint == TERMINAL_CELL_CONTINUATION) {
        terminal_blank_cell(&line->cells[column + 1U]);
    }
    terminal_blank_cell(&line->cells[column]);
}

static void terminal_put_codepoint(uint32_t codepoint)
{
    struct terminal_line *line;
    uint32_t width = terminal_codepoint_width(codepoint);
    uint32_t column;
    if (width > terminal_columns()) {
        codepoint = '?';
        width = 1;
    }
    if (cursor_column >= terminal_columns() ||
        cursor_column + width > terminal_columns()) {
        terminal_newline();
    }
    line = terminal_current_line();
    terminal_prepare_cell(line, cursor_column);
    line->cells[cursor_column].codepoint = codepoint;
    line->cells[cursor_column].foreground = terminal_style_foreground();
    line->cells[cursor_column].background = terminal_style_background();
    for (column = 1; column < width; ++column) {
        terminal_prepare_cell(line, cursor_column + column);
        line->cells[cursor_column + column].codepoint = TERMINAL_CELL_CONTINUATION;
        line->cells[cursor_column + column].foreground = line->cells[cursor_column].foreground;
        line->cells[cursor_column + column].background = line->cells[cursor_column].background;
    }
    cursor_column += width;
}

static void terminal_flush_utf8(void)
{
    if (utf8_length) {
        terminal_put_codepoint('?');
    }
    utf8_length = 0;
    utf8_expected = 0;
}

static void terminal_put_utf8_byte(uint8_t byte)
{
    uint32_t codepoint;
    if (byte < 0x80U) {
        terminal_flush_utf8();
        terminal_put_codepoint(byte);
        return;
    }
    if (utf8_length == 0) {
        if (byte >= 0xc2U && byte <= 0xdfU) {
            utf8_expected = 2;
        } else if (byte >= 0xe0U && byte <= 0xefU) {
            utf8_expected = 3;
        } else if (byte >= 0xf0U && byte <= 0xf4U) {
            utf8_expected = 4;
        } else {
            terminal_put_codepoint('?');
            return;
        }
        utf8_bytes[0] = byte;
        utf8_length = 1;
        return;
    }
    if ((byte & 0xc0U) != 0x80U) {
        terminal_flush_utf8();
        terminal_put_utf8_byte(byte);
        return;
    }
    utf8_bytes[utf8_length++] = byte;
    if (utf8_length != utf8_expected) {
        return;
    }
    if (utf8_expected == 2) {
        codepoint = ((uint32_t)(utf8_bytes[0] & 0x1fU) << 6U) |
                    (uint32_t)(utf8_bytes[1] & 0x3fU);
    } else if (utf8_expected == 3) {
        codepoint = ((uint32_t)(utf8_bytes[0] & 0x0fU) << 12U) |
                    ((uint32_t)(utf8_bytes[1] & 0x3fU) << 6U) |
                    (uint32_t)(utf8_bytes[2] & 0x3fU);
    } else {
        codepoint = ((uint32_t)(utf8_bytes[0] & 0x07U) << 18U) |
                    ((uint32_t)(utf8_bytes[1] & 0x3fU) << 12U) |
                    ((uint32_t)(utf8_bytes[2] & 0x3fU) << 6U) |
                    (uint32_t)(utf8_bytes[3] & 0x3fU);
    }
    utf8_length = 0;
    utf8_expected = 0;
    if (codepoint >= 0x110000U || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        codepoint = '?';
    }
    terminal_put_codepoint(codepoint);
}

static void terminal_move_up(uint32_t amount)
{
    uint32_t row = terminal_logical_active();
    if (amount >= TERMINAL_HISTORY_ROWS) {
        amount = TERMINAL_HISTORY_ROWS - 1U;
    }
    terminal_select_logical(row > amount ? row - amount : 0);
}

static void terminal_move_down(uint32_t amount)
{
    uint32_t row = terminal_logical_active();
    if (amount >= TERMINAL_HISTORY_ROWS) {
        amount = TERMINAL_HISTORY_ROWS - 1U;
    }
    uint32_t target = row + amount;
    if (target >= TERMINAL_HISTORY_ROWS) {
        target = TERMINAL_HISTORY_ROWS - 1U;
    }
    while (target >= history_count) {
        terminal_append_line();
    }
    terminal_select_logical(target);
}

static void terminal_move_home(uint32_t row, uint32_t column)
{
    uint32_t visible_rows = terminal_visible_rows();
    uint32_t first_visible = history_count > visible_rows ? history_count - visible_rows : 0;
    uint32_t target;
    if (row == 0) {
        row = 1;
    }
    if (column == 0) {
        column = 1;
    }
    if (row > visible_rows) {
        row = visible_rows;
    }
    target = first_visible + row - 1U;
    while (target >= history_count) {
        terminal_append_line();
    }
    terminal_select_logical(target);
    cursor_column = column - 1U;
    if (cursor_column >= terminal_columns()) {
        cursor_column = terminal_columns() - 1U;
    }
}

static void terminal_erase_line(uint32_t mode)
{
    struct terminal_line *line = terminal_current_line();
    uint32_t first = 0;
    uint32_t last = terminal_columns();
    uint32_t column;
    if (mode == 0) {
        first = cursor_column;
    } else if (mode == 1) {
        last = cursor_column < terminal_columns() ? cursor_column + 1U : terminal_columns();
    }
    if (first > terminal_columns()) {
        first = terminal_columns();
    }
    for (column = first; column < last; ++column) {
        terminal_blank_cell(&line->cells[column]);
    }
}

static void terminal_erase_display(uint32_t mode)
{
    uint32_t row = terminal_logical_active();
    uint32_t index;
    if (mode == 2) {
        terminal_clear();
        return;
    }
    if (mode == 0) {
        terminal_erase_line(0);
        for (index = row + 1U; index < history_count; ++index) {
            terminal_reset_line(&history[(history_first + index) % TERMINAL_HISTORY_ROWS]);
        }
    } else if (mode == 1) {
        terminal_erase_line(1);
        for (index = 0; index < row; ++index) {
            terminal_reset_line(&history[(history_first + index) % TERMINAL_HISTORY_ROWS]);
        }
    }
}

static void terminal_save_cursor(void)
{
    saved_line = active_line;
    saved_column = cursor_column;
    saved_cursor_valid = 1;
}

static void terminal_restore_cursor(void)
{
    uint32_t row;
    if (!saved_cursor_valid) {
        return;
    }
    for (row = 0; row < history_count; ++row) {
        if ((history_first + row) % TERMINAL_HISTORY_ROWS == saved_line) {
            active_line = saved_line;
            cursor_column = saved_column < terminal_columns() ?
                            saved_column : terminal_columns();
            return;
        }
    }
    saved_cursor_valid = 0;
}

static void terminal_apply_sgr(uint16_t code)
{
    if (code == 0) {
        terminal_reset_style();
    } else if (code == 1) {
        text_bright = 1;
        text_dim = 0;
    } else if (code == 2) {
        text_dim = 1;
        text_bright = 0;
    } else if (code == 7) {
        text_inverse = 1;
    } else if (code == 22) {
        text_bright = 0;
        text_dim = 0;
    } else if (code == 27) {
        text_inverse = 0;
    } else if (code == 39) {
        foreground_index = -1;
        text_foreground = TERMINAL_DEFAULT_FOREGROUND;
    } else if (code == 49) {
        background_index = -1;
        text_background = TERMINAL_DEFAULT_BACKGROUND;
    } else if (code >= 30 && code <= 37) {
        foreground_index = (int8_t)(code - 30U);
    } else if (code >= 40 && code <= 47) {
        background_index = (int8_t)(code - 40U);
        background_bright = 0;
    } else if (code >= 90 && code <= 97) {
        foreground_index = (int8_t)(code - 90U);
        text_bright = 1;
    } else if (code >= 100 && code <= 107) {
        background_index = (int8_t)(code - 100U);
        background_bright = 1;
    }
}

static uint32_t terminal_palette_color(uint16_t index)
{
    if (index < 8) return ansi_normal[index];
    if (index < 16) return ansi_bright[index - 8];
    if (index >= 232) return (8U + (index - 232U) * 10U) * 0x010101U;
    static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
    index -= 16;
    return ((uint32_t)levels[index / 36] << 16) |
           ((uint32_t)levels[(index / 6) % 6] << 8) | levels[index % 6];
}

static void terminal_csi_begin(void)
{
    uint32_t index;
    for (index = 0; index < TERMINAL_CSI_PARAM_CAP; ++index) {
        csi_params[index] = 0;
    }
    csi_param_count = 1;
    csi_private = 0;
}

static uint16_t terminal_csi_param(uint32_t index, uint16_t default_value)
{
    if (index >= csi_param_count || csi_params[index] == 0) {
        return default_value;
    }
    return csi_params[index];
}

static void terminal_finish_csi(char final)
{
    uint16_t amount = terminal_csi_param(0, 1);
    uint32_t columns;
    struct terminal_line *line;
    uint32_t column;
    terminal_normalize_cursor();
    columns = terminal_columns();
    if (final == 'm') {
        uint32_t index;
        for (index = 0; index < csi_param_count; ++index) {
            if (csi_params[index] == 38 || csi_params[index] == 48) {
                int background = csi_params[index] == 48;
                uint32_t color;
                if (++index >= csi_param_count) break;
                if (csi_params[index] == 2) {
                    if (index + 3 >= csi_param_count) break;
                    index += 3;
                    if (csi_params[index - 2] > 255 || csi_params[index - 1] > 255 ||
                        csi_params[index] > 255) continue;
                    color = ((uint32_t)csi_params[index - 2] << 16) |
                            ((uint32_t)csi_params[index - 1] << 8) | csi_params[index];
                } else if (csi_params[index] == 5) {
                    if (++index >= csi_param_count) break;
                    if (csi_params[index] > 255) continue;
                    color = terminal_palette_color(csi_params[index]);
                } else continue;
                if (background) {
                    background_index = -1;
                    text_background = color;
                } else {
                    foreground_index = -1;
                    text_foreground = color;
                }
                continue;
            }
            terminal_apply_sgr(csi_params[index]);
        }
    } else if (final == 'A') {
        terminal_move_up(amount);
    } else if (final == 'B') {
        terminal_move_down(amount);
    } else if (final == 'C') {
        cursor_column += amount;
        if (cursor_column > columns) {
            cursor_column = columns;
        }
    } else if (final == 'D') {
        cursor_column = cursor_column > amount ? cursor_column - amount : 0;
    } else if (final == 'E') {
        terminal_move_down(amount);
        cursor_column = 0;
    } else if (final == 'F') {
        terminal_move_up(amount);
        cursor_column = 0;
    } else if (final == 'a') {
        cursor_column += amount;
        if (cursor_column > columns) {
            cursor_column = columns;
        }
    } else if (final == 'G') {
        cursor_column = amount - 1U;
        if (cursor_column >= terminal_columns()) {
            cursor_column = terminal_columns() - 1U;
        }
    } else if (final == 'H' || final == 'f') {
        terminal_move_home(terminal_csi_param(0, 1), terminal_csi_param(1, 1));
    } else if (final == 'J') {
        terminal_erase_display(csi_params[0]);
    } else if (final == 'K') {
        terminal_erase_line(csi_params[0]);
    } else if (final == '@') {
        line = terminal_current_line();
        if (cursor_column >= columns) {
            amount = 0;
        } else if (amount > columns - cursor_column) {
            amount = (uint16_t)(columns - cursor_column);
        }
        for (column = columns; column-- > cursor_column + amount;) {
            line->cells[column] = line->cells[column - amount];
        }
        for (column = cursor_column; column < cursor_column + amount; ++column) {
            terminal_blank_cell(&line->cells[column]);
        }
    } else if (final == 'P') {
        line = terminal_current_line();
        if (cursor_column >= columns) {
            amount = 0;
        } else if (amount > columns - cursor_column) {
            amount = (uint16_t)(columns - cursor_column);
        }
        for (column = cursor_column; column + amount < columns; ++column) {
            line->cells[column] = line->cells[column + amount];
        }
        for (; column < columns; ++column) {
            terminal_blank_cell(&line->cells[column]);
        }
    } else if (final == 'X') {
        line = terminal_current_line();
        if (cursor_column >= columns) {
            amount = 0;
        } else if (amount > columns - cursor_column) {
            amount = (uint16_t)(columns - cursor_column);
        }
        for (column = cursor_column; column < cursor_column + amount; ++column) {
            terminal_blank_cell(&line->cells[column]);
        }
    } else if (final == 'd') {
        terminal_move_home(amount, 1);
    } else if (final == 'c' && !csi_private && csi_params[0] == 0) {
        /* Primary device attributes: VT100 with no optional hardware. */
        (void)terminal_write_input("\033[?1;0c", 7);
    } else if (final == 'n' && csi_params[0] == 6) {
        char response[32];
        uint32_t row = terminal_logical_active();
        uint32_t visible_rows = terminal_visible_rows();
        uint32_t first_visible = history_count > visible_rows ? history_count - visible_rows : 0;
        int length;
        row = row >= first_visible ? row - first_visible + 1U : 1U;
        length = snprintf(response, sizeof(response), "\033[%u;%uR", row,
                          cursor_column + 1U);
        if (length > 0) {
            (void)write(active_pty_fd, response, (size_t)length);
        }
    } else if (final == 's') {
        terminal_save_cursor();
    } else if (final == 'u') {
        terminal_restore_cursor();
    } else if ((final == 'h' || final == 'l') && csi_private && csi_params[0] == 1049) {
        if (final == 'h') {
            terminal_enter_alternate_screen();
        } else {
            terminal_leave_alternate_screen();
        }
    } else if ((final == 'h' || final == 'l') && csi_private && csi_params[0] == 25) {
        cursor_visible = final == 'h';
    }
    escape_state = TERMINAL_TEXT;
}

static void terminal_finish_osc(int bell)
{
    struct terminal_session *session = active_session;
    session->osc[session->osc_length] = 0;
    if (!session->osc_overflow &&
        (terminal_arg_eq(session->osc, "10;?") ||
         terminal_arg_eq(session->osc, "11;?"))) {
        uint32_t color = session->osc[1] == '0' ? TERMINAL_DEFAULT_FOREGROUND :
                                                TERMINAL_DEFAULT_BACKGROUND;
        char response[40];
        int length = snprintf(response, sizeof(response),
                              "\033]%c%c;rgb:%04x/%04x/%04x%s",
                              session->osc[0], session->osc[1],
                              ((color >> 16) & 255U) * 257U,
                              ((color >> 8) & 255U) * 257U,
                              (color & 255U) * 257U, bell ? "\007" : "\033\\");
        if (length > 0 && (size_t)length < sizeof(response))
            (void)terminal_write_input(response, (uint32_t)length);
    }
    escape_state = TERMINAL_TEXT;
}

static void terminal_put_char(char value)
{
    uint8_t byte = (uint8_t)value;
    if (escape_state == TERMINAL_OSC || escape_state == TERMINAL_OSC_ESCAPE) {
        if (byte == 24 || byte == 26) {
            escape_state = TERMINAL_TEXT;
        } else if (byte == 7 || (escape_state == TERMINAL_OSC_ESCAPE && value == '\\')) {
            terminal_finish_osc(byte == 7);
        } else if (byte == 27) {
            escape_state = TERMINAL_OSC_ESCAPE;
        } else if (escape_state == TERMINAL_OSC_ESCAPE) {
            /* An unexpected escape cancels the string and starts a new one. */
            escape_state = TERMINAL_ESCAPE;
            terminal_put_char(value);
        } else if (active_session->osc_length + 1U < TERMINAL_OSC_CAP) {
            active_session->osc[active_session->osc_length++] = value;
        } else {
            active_session->osc_overflow = 1;
        }
        return;
    }
    if (escape_state == TERMINAL_ESCAPE) {
        if (value == '[') {
            escape_state = TERMINAL_CSI;
            terminal_csi_begin();
        } else if (value == ']') {
            escape_state = TERMINAL_OSC;
            active_session->osc_length = active_session->osc_overflow = 0;
        } else {
            if (value == '7') {
                terminal_save_cursor();
            } else if (value == '8') {
                terminal_restore_cursor();
            } else if (value == 'c') {
                terminal_reset_style();
                terminal_clear();
                cursor_visible = 1;
            } else if (value == 'D') {
                terminal_newline();
            }
            escape_state = TERMINAL_TEXT;
        }
        return;
    }
    if (escape_state == TERMINAL_CSI) {
        if (byte >= '0' && byte <= '9') {
            uint16_t *value_slot = &csi_params[csi_param_count - 1U];
            if (*value_slot < 6553U) {
                *value_slot = (uint16_t)(*value_slot * 10U + byte - '0');
            }
            return;
        }
        if (byte == ';') {
            if (csi_param_count < TERMINAL_CSI_PARAM_CAP) {
                ++csi_param_count;
            }
            return;
        }
        if (byte == '?' && csi_param_count == 1U && csi_params[0] == 0) {
            csi_private = 1;
            return;
        }
        terminal_finish_csi(value);
        return;
    }
    if (byte == 27U) {
        terminal_flush_utf8();
        escape_state = TERMINAL_ESCAPE;
    } else if (value == '\n') {
        terminal_flush_utf8();
        terminal_newline();
    } else if (value == '\r') {
        terminal_flush_utf8();
        cursor_column = 0;
    } else if (value == '\b') {
        terminal_flush_utf8();
        if (cursor_column) {
            --cursor_column;
        }
    } else if (value == '\f') {
        terminal_flush_utf8();
        terminal_clear();
    } else if (value == '\t') {
        uint32_t next_stop;
        uint32_t columns = terminal_columns();
        terminal_flush_utf8();
        next_stop = (cursor_column + 4U) & ~3U;
        /* A tab moves the cursor without wrapping or erasing existing text. */
        cursor_column = next_stop < columns ? next_stop : columns - 1U;
    } else if (byte >= 32U) {
        terminal_put_utf8_byte(byte);
    }
}

static void terminal_put_text(const char *text)
{
    while (text && *text) {
        terminal_put_char(*text++);
    }
}

static void terminal_draw_monospace_glyph(struct reliefos_ui_surface *ui,
                                          uint32_t x, uint32_t y,
                                          uint32_t codepoint, uint32_t width,
                                          uint32_t foreground,
                                          uint32_t background)
{
    const uint8_t *glyph;
    if (!ui || !ui->pixels || codepoint > 0xffU || width != 1U ||
        x + RELIEFOS_FONT_W > ui->width || y + RELIEFOS_FONT_H > ui->height) {
        reliefos_ui_codepoint(ui, x, y, codepoint, width, foreground, background);
        return;
    }

    /* The embedded PSF font is an 8x16 bitmap, so every terminal cell has
     * the same advance regardless of the active desktop UI theme. */
    glyph = reliefos_psf_glyph((char)codepoint);
    for (uint32_t row = 0; row < RELIEFOS_FONT_H; ++row) {
        uint8_t bits = glyph[row];
        uint32_t *pixels_row = ui->pixels + (uint64_t)(y + row) * ui->stride + x;
        for (uint32_t column = 0; column < RELIEFOS_FONT_W; ++column) {
            if (bits & (uint8_t)(0x80U >> column)) {
                pixels_row[column] = foreground;
            }
        }
    }
}

static void terminal_draw_line(struct reliefos_ui_surface *ui, uint32_t y,
                               const struct terminal_line *line)
{
    uint32_t column;
    for (column = 0; column < terminal_columns(); ++column) {
        const struct terminal_cell *cell = &line->cells[column];
        uint32_t width = 1;
        uint32_t x = TERMINAL_BODY_X + column * RELIEFOS_FONT_W;
        if (cell->codepoint == TERMINAL_CELL_CONTINUATION) {
            continue;
        }
        if (column + 1U < terminal_columns() &&
            line->cells[column + 1U].codepoint == TERMINAL_CELL_CONTINUATION) {
            width = 2;
        }
        reliefos_ui_rect(ui, x, y, width * RELIEFOS_FONT_W, RELIEFOS_FONT_H,
                       cell->background);
        if (cell->codepoint) {
            terminal_draw_monospace_glyph(ui, x, y, cell->codepoint, width,
                                          cell->foreground, cell->background);
        }
    }
}

static void terminal_draw(struct reliefos_ui_surface *ui)
{
    struct reliefos_ui_tab_item tab_items[TERMINAL_MAX_SESSIONS];
    uint32_t tab_count;
    uint32_t rows = terminal_visible_rows();
    uint32_t first_visible = history_count > rows ? history_count - rows : 0;
    uint32_t active_visible = terminal_logical_active();
    uint32_t row;

    tab_count = terminal_tab_items(tab_items);

    reliefos_ui_rect(ui, 0, 0, terminal_view_width, terminal_view_height, 0x00000000U);
    reliefos_ui_rect(ui, 0, 0, terminal_view_width, reliefos_ui_tab_height(), RELIEFOS_UI_GRAY);
    reliefos_ui_tab_control(ui, 0, 0, terminal_view_width - TERMINAL_TAB_ADD_W,
                          tab_items, tab_count, &tabs_state);
    reliefos_ui_button(ui, terminal_view_width - TERMINAL_TAB_ADD_W, 0,
                     TERMINAL_TAB_ADD_W, reliefos_ui_tab_height(), "+",
                     tab_count < TERMINAL_MAX_SESSIONS ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
    for (row = 0; row < rows && first_visible + row < history_count; ++row) {
        uint32_t line_index = (history_first + first_visible + row) % TERMINAL_HISTORY_ROWS;
        terminal_draw_line(ui, reliefos_ui_tab_height() + row * RELIEFOS_FONT_H,
                           &history[line_index]);
    }
    if (cursor_visible && active_visible >= first_visible &&
        active_visible < first_visible + rows &&
        (reliefos_uptime_ms() / 450UL) % 2UL == 0UL) {
        uint32_t cursor_x = TERMINAL_BODY_X +
                            (cursor_column < terminal_columns() ?
                             cursor_column : terminal_columns() - 1U) * RELIEFOS_FONT_W;
        uint32_t cursor_y = reliefos_ui_tab_height() +
                            (active_visible - first_visible) * RELIEFOS_FONT_H;
        reliefos_ui_rect(ui, cursor_x, cursor_y + RELIEFOS_FONT_H - 2U,
                       RELIEFOS_FONT_W, 2, terminal_style_foreground());
    }
}

static int terminal_pump_output(void)
{
    char buffer[1024];
    uint32_t consumed = 0;
    int changed = 0;
    int received;
    do {
        uint32_t request = TERMINAL_OUTPUT_BUDGET - consumed;
        if (request > sizeof(buffer)) {
            request = sizeof(buffer);
        }
        received = (int)read(active_pty_fd, buffer, request);
        if (received > 0) {
            int index;
            for (index = 0; index < received; ++index) {
                terminal_put_char(buffer[index]);
            }
            consumed += (uint32_t)received;
            changed = 1;
        }
    } while (received > 0 && consumed < TERMINAL_OUTPUT_BUDGET);
    return changed;
}

static int terminal_pump_all_output(void)
{
    struct terminal_session *selected = active_session;
    int selected_changed = 0;

    for (uint32_t index = 0; index < TERMINAL_MAX_SESSIONS; ++index) {
        if (!sessions[index].used) {
            continue;
        }
        active_session = &sessions[index];
        if (terminal_pump_output() && active_session == selected) {
            selected_changed = 1;
        }
    }
    active_session = selected;
    return selected_changed;
}

static int terminal_write_input(const char *buffer, uint32_t length)
{
    return write(active_pty_fd, buffer, length) == (long)length;
}

static int terminal_control_character(char character, uint8_t ctrl, char *out)
{
    if (!out) {
        return 0;
    }
    if (!ctrl) {
        *out = character;
        return 1;
    }
    if (character >= 'a' && character <= 'z') {
        *out = (char)(character - 'a' + 1);
        return 1;
    }
    if (character >= 'A' && character <= 'Z') {
        *out = (char)(character - 'A' + 1);
        return 1;
    }
    switch (character) {
    case '@': *out = 0; return 1;
    case '[': *out = 27; return 1;
    case '\\': *out = 28; return 1;
    case ']': *out = 29; return 1;
    case '^': *out = 30; return 1;
    case '_': *out = 31; return 1;
    default: return 0;
    }
}

static int terminal_send_key(uint8_t keycode, uint8_t pressed,
                             uint8_t *shift_down, uint8_t *ctrl_down,
                             uint8_t *alt_down)
{
    char character;
    char sequence[8];
    uint32_t sequence_length = 0;
    struct termios termios;
    int have_termios;
    int local_echo;
    if (keycode == RELIEFOS_KEY_CAPS_LOCK) {
        return 0;
    }
    if (keycode == RELIEFOS_KEY_LEFT_SHIFT || keycode == RELIEFOS_KEY_RIGHT_SHIFT) {
        *shift_down = pressed ? 1 : 0;
        return 0;
    }
    if (keycode == RELIEFOS_KEY_LEFT_CTRL || keycode == RELIEFOS_KEY_RIGHT_CTRL) {
        *ctrl_down = pressed ? 1 : 0;
        return 0;
    }
    if (keycode == RELIEFOS_KEY_LEFT_ALT || keycode == RELIEFOS_KEY_RIGHT_ALT) {
        *alt_down = pressed ? 1 : 0;
        return 0;
    }
    if (!pressed) {
        return 0;
    }
    have_termios = tcgetattr(active_pty_fd, &termios) == 0;
    if (keycode == RELIEFOS_KEY_ESCAPE) {
        sequence[0] = '\033';
        sequence_length = 1;
    } else if (keycode == RELIEFOS_KEY_UP) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'A'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_DOWN) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'B'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_RIGHT) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'C'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_LEFT) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'D'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_HOME) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'H'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_END) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = 'F'; sequence_length = 3;
    } else if (keycode == RELIEFOS_KEY_INSERT) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = '2'; sequence[3] = '~'; sequence_length = 4;
    } else if (keycode == RELIEFOS_KEY_DELETE) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = '3'; sequence[3] = '~'; sequence_length = 4;
    } else if (keycode == RELIEFOS_KEY_PAGE_UP) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = '5'; sequence[3] = '~'; sequence_length = 4;
    } else if (keycode == RELIEFOS_KEY_PAGE_DOWN) {
        sequence[0] = '\033'; sequence[1] = '['; sequence[2] = '6'; sequence[3] = '~'; sequence_length = 4;
    } else if (keycode == RELIEFOS_KEY_ENTER) {
        /* A terminal Enter key emits CR.  Canonical PTYs translate it to LF
         * for shell input, while raw-mode programs such as nano receive ^M. */
        character = '\r';
    } else if (keycode == RELIEFOS_KEY_BACKSPACE) {
        /* Send the configured erase byte.  The default is DEL (0x7f), which
         * lets the PTY remove it in ICANON mode and is also understood by
         * nano, BusyBox vi and other raw-mode editors. */
        character = have_termios && termios.c_cc[VERASE] ?
                    (char)termios.c_cc[VERASE] : '\177';
    } else if (keycode == RELIEFOS_KEY_TAB) {
        character = '\t';
    } else if (!reliefos_ui_keycode_to_char_shift(keycode, *shift_down, &character)) {
        return 0;
    } else if (!terminal_control_character(character, *ctrl_down, &character)) {
        return 0;
    }
    if (sequence_length) {
        if (*ctrl_down && sequence_length == 3U && sequence[2] >= 'A' && sequence[2] <= 'D') {
            sequence[2] = (char)(sequence[2] - 'A' + 'A');
            sequence[1] = '[';
            /* BusyBox read_key understands the standard Ctrl-arrow form. */
            sequence[2] = '1'; sequence[3] = ';'; sequence[4] = '5';
            sequence[5] = (char)(keycode == RELIEFOS_KEY_UP ? 'A' :
                                  keycode == RELIEFOS_KEY_DOWN ? 'B' :
                                  keycode == RELIEFOS_KEY_RIGHT ? 'C' : 'D');
            sequence_length = 6;
        }
        if (!terminal_write_input(sequence, sequence_length)) {
            return 0;
        }
        return 1;
    }
    if (*alt_down) {
        char alt_sequence[2] = {'\033', character};
        if (!terminal_write_input(alt_sequence, sizeof(alt_sequence))) {
            return 0;
        }
        return 1;
    }
    if (!terminal_write_input(&character, 1)) {
        return 0;
    }
    /* Tab completion and raw-mode programs decide their own cursor movement.
     * Do not fake a four-column local echo; render only what the PTY produces. */
    if (keycode == RELIEFOS_KEY_TAB) {
        return 1;
    }
    local_echo = !have_termios || (termios.c_lflag & ECHO) != 0;
    if (local_echo) {
        if (keycode == RELIEFOS_KEY_BACKSPACE) {
            if (local_echoed_input_count) {
                terminal_put_char('\b');
                terminal_put_char(' ');
                terminal_put_char('\b');
                --local_echoed_input_count;
            }
        } else if (have_termios &&
                   (termios.c_lflag & ICANON) != 0 &&
                   character == (char)termios.c_cc[VKILL]) {
            while (local_echoed_input_count) {
                terminal_put_char('\b');
                terminal_put_char(' ');
                terminal_put_char('\b');
                --local_echoed_input_count;
            }
        } else if (!(have_termios &&
                     (termios.c_lflag & ICANON) != 0 &&
                     character == (char)termios.c_cc[VEOF])) {
            if (character == '\r' && have_termios &&
                (termios.c_iflag & ICRNL)) {
                terminal_put_char('\n');
                local_echoed_input_count = 0;
            } else {
                terminal_put_char(character);
                if ((uint8_t)character >= 32U || character == '\t') {
                    ++local_echoed_input_count;
                }
            }
        }
    }
    return 1;
}

static uint32_t terminal_tab_items(struct reliefos_ui_tab_item *items)
{
    uint32_t count = 0;

    for (uint32_t index = 0; index < TERMINAL_MAX_SESSIONS; ++index) {
        if (!sessions[index].used) {
            continue;
        }
        if (items) {
            items[count].label = terminal_tab_labels[index];
            items[count].id = index + 1U;
            items[count].flags = RELIEFOS_UI_TAB_CLOSABLE;
        }
        ++count;
    }
    return count;
}

static int terminal_select_tab(uint32_t id)
{
    struct terminal_session *session;

    if (id == 0 || id > TERMINAL_MAX_SESSIONS) {
        return 0;
    }
    session = &sessions[id - 1U];
    if (!session->used) {
        return 0;
    }
    active_session = session;
    tabs_state.selected_id = id;
    tabs_state.hovered_id = id;
    return 1;
}

static int terminal_select_adjacent_tab(int direction)
{
    int index;

    if (terminal_tab_items(0) < 2U || !active_session) {
        return 0;
    }
    index = (int)(active_session - sessions);
    for (uint32_t step = 1; step <= TERMINAL_MAX_SESSIONS; ++step) {
        index += direction;
        if (index < 0) {
            index = TERMINAL_MAX_SESSIONS - 1;
        } else if (index >= (int)TERMINAL_MAX_SESSIONS) {
            index = 0;
        }
        if (sessions[index].used) {
            return terminal_select_tab((uint32_t)index + 1U);
        }
    }
    return 0;
}

static int terminal_close_session(uint32_t id)
{
    struct terminal_session *session;
    int index;
    if (id == 0 || id > TERMINAL_MAX_SESSIONS || !sessions[id - 1U].used) {
        return 0;
    }
    session = &sessions[id - 1U];
    if (session->child_pid > 0) {
        (void)kill(session->child_pid, SIGHUP);
    }
    if (session->pty_fd >= 0) {
        (void)close(session->pty_fd);
    }
    index = (int)(session - sessions);
    session->used = 0;
    if (terminal_tab_items(0) == 0U) {
        active_session = 0;
        return -1;
    }
    if (active_session == session) {
        for (uint32_t step = 1; step <= TERMINAL_MAX_SESSIONS; ++step) {
            int candidate = index + (int)step;
            if (candidate >= (int)TERMINAL_MAX_SESSIONS) {
                candidate %= (int)TERMINAL_MAX_SESSIONS;
            }
            if (sessions[candidate].used) {
                (void)terminal_select_tab((uint32_t)candidate + 1U);
                break;
            }
        }
    }
    return 1;
}

static void terminal_close_all_sessions(void)
{
    for (uint32_t index = 0; index < TERMINAL_MAX_SESSIONS; ++index) {
        if (sessions[index].used) {
            if (sessions[index].child_pid > 0) {
                (void)kill(sessions[index].child_pid, SIGHUP);
            }
            if (sessions[index].pty_fd >= 0) {
                (void)close(sessions[index].pty_fd);
            }
            sessions[index].used = 0;
        }
    }
    active_session = 0;
}

static struct terminal_session *terminal_open_session(const char *path,
                                                      char *const command_argv[],
                                                      char *const command_envp[])
{
    struct terminal_session *session = 0;
    int new_pty = -1;
    pid_t child_pid;
    struct winsize initial_winsize;

    if (!path || !path[0]) {
        return 0;
    }
    for (uint32_t index = 0; index < TERMINAL_MAX_SESSIONS; ++index) {
        if (!sessions[index].used) {
            session = &sessions[index];
            break;
        }
    }
    if (!session) {
        return 0;
    }

    initial_winsize = (struct winsize){
        .ws_row = (uint16_t)terminal_visible_rows(),
        .ws_col = (uint16_t)terminal_columns(),
    };
    child_pid = forkpty(&new_pty, 0, 0, &initial_winsize);
    /* forkpty returns the master only to the parent; the child uses stdio. */
    if (child_pid == 0) {
        (void)execve(path, command_argv, command_envp);
        fprintf(stderr, "terminal: exec %s failed errno=%d\n", path, errno);
        _exit(127);
    }
    if (child_pid < 0 || new_pty < 0) {
        if (new_pty >= 0) {
            (void)close(new_pty);
        }
        printf("terminal: PTY creation failed errno=%d\n", errno);
        return 0;
    }
    {
        int flags = fcntl(new_pty, F_GETFL);
        if (flags >= 0) {
            (void)fcntl(new_pty, F_SETFL, flags | O_NONBLOCK);
        }
        printf("terminal: PTY ready master=%d child=%d flags=%d\n",
               new_pty, child_pid, flags);
    }
    *session = (struct terminal_session){0};
    session->used = 1;
    session->pty_fd = new_pty;
    session->child_pid = child_pid;
    active_session = session;
    cursor_visible = 1;
    terminal_reset_style();
    terminal_clear();
    terminal_sync_session_winsize(session);
    (void)terminal_select_tab((uint32_t)(session - sessions) + 1U);
    return session;
}

static int terminal_handle_mouse(int32_t x, int32_t y, uint8_t buttons,
                                 const char *path, char *const command_argv[],
                                 char *const command_envp[])
{
    struct reliefos_ui_tab_item tab_items[TERMINAL_MAX_SESSIONS];
    uint32_t tab_count;
    uint32_t closed_id = 0;

    if ((buttons & 1U) == 0) {
        return 0;
    }
    tab_count = terminal_tab_items(tab_items);
    if (reliefos_ui_tab_control_handle_mouse_ex(&tabs_state, x, y, 0, 0,
                                              terminal_view_width - TERMINAL_TAB_ADD_W,
                                              tab_items, tab_count, &closed_id)) {
        if (closed_id) {
            return terminal_close_session(closed_id);
        }
        return terminal_select_tab(tabs_state.selected_id);
    }
    if (x >= (int32_t)(terminal_view_width - TERMINAL_TAB_ADD_W) &&
        x < (int32_t)terminal_view_width && y >= 0 &&
        y < (int32_t)reliefos_ui_tab_height()) {
        return terminal_open_session(path, command_argv, command_envp) != 0;
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    char *shell_argv[4];
    char shell_path[256];
    char shell_environment[sizeof(shell_path) + sizeof("SHELL=")];
    char shell_term[] = "TERM=xterm";
    char shell_terminfo[] = "TERMINFO_DIRS=/usr/share/terminfo:/etc/terminfo:/lib/terminfo";
    char terminal_program[] = "TERM_PROGRAM=ReliefOS Terminal";
    char *terminal_envp[] = { shell_term, shell_terminfo, terminal_program, 0 };
    char *shell_envp[] = { shell_environment, shell_term, shell_terminfo, terminal_program, 0 };
    char **command_env_owned = 0;
    char *const *command_argv;
    char *const *command_envp;
    char *const *environment_overrides = terminal_envp;
    const char *command_path;
    uint8_t shift_down = 0;
    uint8_t ctrl_down = 0;
    uint8_t alt_down = 0;
    uint8_t cursor_phase = 2;
    int window_id;

    (void)envp;
    if (argc > 2 && terminal_arg_eq(argv[1], "--run") && argv[2] && argv[2][0]) {
        command_path = argv[2];
        command_argv = &argv[2];
        command_envp = 0;
    } else {
        struct passwd *account = getpwuid(getuid());
        int path_length = snprintf(shell_path, sizeof(shell_path), "%s",
                                   account && account->pw_shell && account->pw_shell[0] == '/'
                                       ? account->pw_shell : "/bin/bash");
        if (path_length < 0 || (size_t)path_length >= sizeof(shell_path)) {
            printf("terminal: shell path is too long\n");
            return 1;
        }
        shell_argv[0] = shell_path;
        int length = snprintf(shell_environment, sizeof(shell_environment),
                              "SHELL=%s", shell_argv[0]);
        if (length < 0 || (size_t)length >= sizeof(shell_environment)) {
            printf("terminal: shell path is too long\n");
            return 1;
        }
        shell_argv[1] = "-l";
        shell_argv[2] = 0;
        shell_argv[3] = 0;
        command_path = shell_argv[0];
        command_argv = shell_argv;
        command_envp = 0;
        environment_overrides = shell_envp;
    }
    if (reliefos_environment_build((char *const *)environment_overrides,
                                 &command_env_owned) < 0) {
        printf("terminal: environment setup failed\n");
        return 1;
    }
    command_envp = command_env_owned;
    window_id = reliefos_gui_create_app_window_ex(T("Terminal"),
                                                "", TERMINAL_DEFAULT_W,
                                                TERMINAL_DEFAULT_H, 0);
    if (window_id <= 0) {
        printf("terminal: window creation failed (%d)\n", window_id);
        reliefos_environment_free(command_env_owned);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, terminal_view_width, terminal_view_height,
                   TERMINAL_MAX_W);
    reliefos_ui_tab_state_init(&tabs_state, 0);
    if (!terminal_open_session(command_path, command_argv, command_envp)) {
        printf("terminal: PTY creation failed\n");
        reliefos_environment_free(command_env_owned);
        return 1;
    }
    terminal_draw(&ui);
    reliefos_gui_present_window((uint32_t)window_id, terminal_view_width,
                              terminal_view_height, TERMINAL_MAX_W, pixels);
    for (;;) {
        int redraw = terminal_pump_all_output();
        uint8_t next_cursor_phase = (uint8_t)((reliefos_uptime_ms() / 450UL) % 2UL);
        if (next_cursor_phase != cursor_phase) {
            cursor_phase = next_cursor_phase;
            redraw = 1;
        }
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, 40U) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                terminal_close_all_sessions();
                reliefos_environment_free(command_env_owned);
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN) {
                if (ctrl_down && shift_down && event.keycode == TERMINAL_KEY_T) {
                    redraw |= terminal_open_session(command_path, command_argv,
                                                    command_envp) != 0;
                } else if (ctrl_down && shift_down && event.keycode == TERMINAL_KEY_W) {
                    int close_result = terminal_close_session(tabs_state.selected_id);
                    if (close_result < 0) {
                        reliefos_environment_free(command_env_owned);
                        return 0;
                    }
                    redraw |= close_result;
                } else if (ctrl_down && event.keycode == RELIEFOS_KEY_TAB) {
                    redraw |= terminal_select_adjacent_tab(shift_down ? -1 : 1);
                } else {
                    redraw |= terminal_send_key(event.keycode, 1, &shift_down,
                                                &ctrl_down, &alt_down);
                }
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                (void)terminal_send_key(event.keycode, 0, &shift_down,
                                        &ctrl_down, &alt_down);
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON) {
                int mouse_result = terminal_handle_mouse(event.x, event.y,
                                                         event.buttons,
                                                         command_path,
                                                         command_argv,
                                                         command_envp);
                if (mouse_result < 0) {
                    reliefos_environment_free(command_env_owned);
                    return 0;
                }
                redraw |= mouse_result;
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                /* Theme changes are translated to RESIZE by libc but carry
                 * no geometry. Preserve the current PTY size in that case. */
                if (event.width && event.height) {
                    redraw |= terminal_resize_view(event.width, event.height);
                } else {
                    redraw = 1;
                }
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_THEME_CHANGED ||
                       event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                redraw = 1;
            }
        }
        if (redraw) {
            if (ui.width != terminal_view_width || ui.height != terminal_view_height)
                reliefos_ui_bind(&ui, pixels, terminal_view_width, terminal_view_height,
                               TERMINAL_MAX_W);
            terminal_draw(&ui);
            reliefos_gui_present_window((uint32_t)window_id, terminal_view_width,
                                      terminal_view_height, TERMINAL_MAX_W, pixels);
        }
    }
}
