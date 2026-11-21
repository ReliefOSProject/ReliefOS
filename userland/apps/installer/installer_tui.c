#define _XOPEN_SOURCE_EXTENDED 1
#include <ncurses.h>
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <locale.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include "installer_tui.h"
#include "model.h"

#define T(s) gettext(s)
volatile sig_atomic_t installer_tui_signal;
static SCREEN *terminal_screen;
static FILE *terminal_input, *terminal_output;
static int saved_output = -1, saved_error = -1;
static struct sigaction old_signals[3];
static const int handled_signals[] = {SIGINT, SIGTERM, SIGHUP};
static int signals_installed;

static void interrupted(int signal_number) { installer_tui_signal = signal_number; }

int tui_open(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { errno = ENOTTY; return -1; }
    int output = dup(STDOUT_FILENO), input = dup(STDIN_FILENO);
    if (output < 0 || input < 0) {
        if (output >= 0) close(output);
        if (input >= 0) close(input);
        return -1;
    }
    fcntl(output, F_SETFD, FD_CLOEXEC);
    fcntl(input, F_SETFD, FD_CLOEXEC);
    terminal_output = fdopen(output, "w");
    terminal_input = fdopen(input, "r");
    if (!terminal_output || !terminal_input) {
        if (terminal_output) fclose(terminal_output); else close(output);
        if (terminal_input) fclose(terminal_input); else close(input);
        terminal_output = terminal_input = NULL;
        return -1;
    }
    terminal_screen = newterm(NULL, terminal_output, terminal_input);
    if (!terminal_screen) { tui_close(); errno = ENOTSUP; return -1; }
    raw(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(150);
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_BLACK, COLOR_WHITE);
        init_pair(3, COLOR_BLACK, COLOR_CYAN);
        init_pair(4, COLOR_BLACK, COLOR_GREEN);
        init_pair(5, COLOR_BLACK, COLOR_YELLOW);
        init_pair(6, COLOR_WHITE, COLOR_BLUE);
        bkgd(COLOR_PAIR(1));
    }
    struct sigaction action = {0};
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    for (unsigned i = 0; i < sizeof(handled_signals) / sizeof(handled_signals[0]); ++i) {
        if (sigaction(handled_signals[i], &action, &old_signals[i]) < 0) {
            tui_close(); return -1;
        }
        ++signals_installed;
    }
    /* ncurses owns duplicate terminal streams. Backend diagnostics go to the
     * log, including output from child utilities, without corrupting the UI. */
    int log = open("/var/log/installer.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat status;
    if (log < 0) { tui_close(); return -1; }
    if (fstat(log, &status) < 0 || !S_ISREG(status.st_mode)) {
        close(log); tui_close(); errno = EINVAL; return -1;
    }
    saved_output = dup(STDOUT_FILENO); saved_error = dup(STDERR_FILENO);
    if (saved_output < 0 || saved_error < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
        close(log); tui_close(); return -1;
    }
    fcntl(saved_output, F_SETFD, FD_CLOEXEC);
    fcntl(saved_error, F_SETFD, FD_CLOEXEC);
    close(log);
    return 0;
}

void tui_close(void)
{
    if (terminal_screen) {
        /* ncurses keeps a search tree for initialized color pairs. Release it
         * before destroying the screen, as well as restoring terminal modes. */
        if (has_colors()) reset_color_pairs();
        endwin(); delscreen(terminal_screen); terminal_screen = NULL;
    }
    while (signals_installed) {
        --signals_installed;
        sigaction(handled_signals[signals_installed], &old_signals[signals_installed], NULL);
    }
    fflush(stdout); fflush(stderr);
    if (saved_output >= 0) { dup2(saved_output, STDOUT_FILENO); close(saved_output); saved_output = -1; }
    if (saved_error >= 0) { dup2(saved_error, STDERR_FILENO); close(saved_error); saved_error = -1; }
    if (terminal_output) { fclose(terminal_output); terminal_output = NULL; }
    if (terminal_input) { fclose(terminal_input); terminal_input = NULL; }
}

void tui_begin(struct tui_view *view, const char *title)
{
    view->count = 0;
    snprintf(view->title, sizeof(view->title), "%s", title);
}

static struct tui_item *add_item(struct tui_view *view, enum tui_kind kind, const char *label)
{
    if (view->count >= (int)(sizeof(view->items) / sizeof(view->items[0]))) return NULL;
    struct tui_item *item = &view->items[view->count++];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    snprintf(item->label, sizeof(item->label), "%s", label ? label : "");
    return item;
}

void tui_text(struct tui_view *view, const char *text) { add_item(view, TUI_TEXT, text); }
void tui_document(struct tui_view *view, const char *text)
{ struct tui_item *item = add_item(view, TUI_TEXT, ""); if (item) item->document = text; }
void tui_button(struct tui_view *view, const char *label, int action)
{ struct tui_item *item = add_item(view, TUI_BUTTON, label); if (item) item->action = action; }
void tui_radio(struct tui_view *view, const char *label, int *selection, int option)
{ struct tui_item *item = add_item(view, TUI_RADIO, label); if (item) { item->selection = selection; item->option = option; } }
void tui_check(struct tui_view *view, const char *label, int *selection)
{ struct tui_item *item = add_item(view, TUI_CHECK, label); if (item) item->selection = selection; }
void tui_input(struct tui_view *view, const char *label, char *value, size_t capacity, int secret)
{
    struct tui_item *item = add_item(view, TUI_INPUT, label);
    if (item) { item->value = value; item->capacity = capacity; item->secret = secret; }
}

/** @brief Bind a live, caller-owned disk snapshot to one keyboard control.
 * @param view Page under construction. @param table Snapshot borrowed until tui_step returns.
 */
void tui_partitions(struct tui_view *view, struct tui_partition_table *table)
{
    struct tui_item *item = add_item(view, TUI_PARTITIONS, "");
    if (item) item->table = table;
}

/** @brief Append an action that shares available horizontal space with its neighbors.
 * @param view Page under construction. @param label Copied UTF-8 label.
 * @param action Result returned on activation.
 */
void tui_compact_button(struct tui_view *view, const char *label, int action)
{
    struct tui_item *item = add_item(view, TUI_BUTTON, label);
    if (item) { item->action = action; item->compact = 1; }
}

/** @brief Append a horizontally arranged radio option without changing selection.
 * @param view Page under construction. @param label Copied UTF-8 label.
 * @param selection Borrowed mutable value. @param option Activation value.
 */
void tui_compact_radio(struct tui_view *view, const char *label, int *selection, int option)
{
    struct tui_item *item = add_item(view, TUI_RADIO, label);
    if (item) { item->selection = selection; item->option = option; item->compact = 1; }
}

static size_t character(const char *text, wchar_t *wide)
{
    mbstate_t state = {0};
    size_t length = mbrtowc(wide, text, MB_CUR_MAX, &state);
    if (length == (size_t)-1 || length == (size_t)-2) { *wide = L'?'; return 1; }
    return length;
}

/* Render measured Unicode cells and sanitize control bytes in device names
 * and command output. A document can span arbitrary viewport-sized pages. */
static int wrapped(WINDOW *window, int row, int width, const char *text)
{
    int column = 0;
    for (const char *cursor = text; *cursor;) {
        wchar_t wide;
        size_t bytes = character(cursor, &wide);
        cursor += bytes;
        if (wide == L'\n') { ++row; column = 0; continue; }
        int cells = wcwidth(wide);
        if (cells < 0) { wide = L' '; cells = 1; }
        if (cells && column + cells > width) { ++row; column = 0; }
        if (window && cells <= width) mvwaddnwstr(window, row, column, &wide, 1);
        column += cells;
    }
    return row + 1;
}

static void clipped(WINDOW *window, int row, int column, int width, const char *text)
{
    int used = 0;
    while (*text) {
        wchar_t wide;
        size_t bytes = character(text, &wide);
        int cells = wcwidth(wide);
        if (cells < 0) { wide = L' '; cells = 1; }
        if (used + cells > width) break;
        mvwaddnwstr(window, row, column + used, &wide, 1);
        used += cells; text += bytes;
    }
}

/** @brief Measure sanitized UTF-8 text in terminal cells.
 * @param text NUL-terminated borrowed text. @return Nonnegative terminal width.
 */
static int text_width(const char *text)
{
    int width = 0;
    while (*text) {
        wchar_t wide;
        text += character(text, &wide);
        int cells = wcwidth(wide);
        width += cells < 0 ? 1 : cells;
    }
    return width;
}

/** @brief Render a proportional, labeled disk map; tiny extents retain a one-cell marker.
 * @param window Caller-owned ncurses canvas. @param row Destination row.
 * @param width Available terminal columns. @param table Borrowed validated snapshot.
 */
static void partition_map(WINDOW *window, int row, int width, const struct tui_partition_table *table)
{
    mvwaddch(window, row, 0, '[');
    mvwhline(window, row, 1, '.', width - 2);
    mvwaddch(window, row, width - 1, ']');
    int used = 0;
    for (int i = 0; table->sector_count && i < table->count; ++i) {
        const struct tui_partition_row *part = &table->rows[i];
        int start = (int)((long double)part->first_lba / table->sector_count * (width - 2) + 0.5L);
        int end = (int)((long double)(part->first_lba + part->sector_count) / table->sector_count * (width - 2) + 0.5L);
        if (start < used) start = used;
        if (end <= start) end = start + 1;
        if (end > width - 2) end = width - 2;
        if (start >= end) continue;
        int pair = part->symbol == 'B' ? 5 : part->symbol == 'E' ? 3 : part->symbol == 'R' ? 4 : part->symbol == 'D' ? 6 : 2;
        wattrset(window, has_colors() ? COLOR_PAIR(pair) : part->symbol == '.' ? A_DIM : A_REVERSE);
        mvwhline(window, row, start + 1, part->symbol == '.' ? '.' : ' ', end - start);
        char capacity[32], label[64];
        tui_partition_size(capacity, sizeof(capacity), part->sector_count, table->sector_size);
        snprintf(label, sizeof(label), "%c %s", part->symbol, capacity);
        if ((int)strlen(label) <= end - start)
            clipped(window, row, start + 1 + (end - start - (int)strlen(label)) / 2, end - start, label);
        else mvwaddch(window, row, start + 1, (chtype)part->symbol);
        used = end;
    }
    wattrset(window, has_colors() ? COLOR_PAIR(2) : A_NORMAL);
}

/** @brief Draw fixed column headers and the selected physical-row viewport.
 * @param window Caller-owned canvas. @param row First destination row.
 * @param width Available terminal columns. @param height Reserved widget rows.
 * @param table Mutable row viewport. @param focused Nonzero highlights its active selection.
 */
static void partition_table(WINDOW *window, int row, int width, int height,
    struct tui_partition_table *table, int focused)
{
    clipped(window, row, 0, width, table->disk);
    partition_map(window, row + 1, width, table);
    clipped(window, row + 2, 0, width, T("B BIOS  E ESP  R /  D Data  . Free"));
    clipped(window, row + 3, 0, width, table->preview
        ? T("Pending layout - written only after INSTALL confirmation.") : T("Installation layout - changes remain pending"));
    int widths[] = {width >= 80 ? 20 : width >= 65 ? 18 : 14,
        width >= 65 ? 11 : 9, 10, width >= 80 ? 14 : width >= 65 ? 12 : 9, 0};
    widths[4] = width - widths[0] - widths[1] - widths[2] - widths[3] - 4;
    const char *headers[] = {T("Partition"), T("Size"), T("Filesystem"), T("Use"), T("Name")};
    if (text_width(headers[2]) > widths[2]) headers[2] = "FS";
    mvwhline(window, row + 4, 0, ACS_HLINE, width);
    mvwhline(window, row + 6, 0, ACS_HLINE, width);
    int column = 0;
    for (int i = 0; i < 5; ++i) {
        wattron(window, A_BOLD);
        clipped(window, row + 5, column, widths[i], headers[i]);
        wattroff(window, A_BOLD);
        if (i < 4) mvwaddch(window, row + 5, column + widths[i], ACS_VLINE);
        column += widths[i] + 1;
    }
    table->visible = height - 10;
    if (table->visible < 1) table->visible = 1;
    if (table->first > table->cursor) table->first = table->cursor;
    if (table->cursor >= table->first + table->visible) table->first = table->cursor - table->visible + 1;
    if (table->first > table->count - table->visible) table->first = table->count - table->visible;
    if (table->first < 0) table->first = 0;
    for (int i = 0; i < table->visible; ++i) {
        int index = table->first + i;
        if (index >= table->count) break;
        const struct tui_partition_row *part = &table->rows[index];
        if (index == table->cursor) wattron(window, focused ? A_REVERSE : A_BOLD);
        int destination = row + 7 + i;
        mvwhline(window, destination, 0, ' ', width);
        char device[96], size[32];
        snprintf(device, sizeof(device), "%c %s", index == table->cursor ? '>' : ' ',
            part->path[0] ? part->path : T("Unallocated"));
        tui_partition_size(size, sizeof(size), part->sector_count, table->sector_size);
        const char *values[] = {device, size,
            part->filesystem ? reliefos_block_filesystem_name(part->filesystem) : "-", part->role, part->name};
        column = 0;
        for (int field = 0; field < 5; ++field) {
            clipped(window, destination, column, widths[field], values[field]);
            if (field < 4) mvwaddch(window, destination, column + widths[field], ACS_VLINE);
            column += widths[field] + 1;
        }
        if (index == table->cursor) wattroff(window, focused ? A_REVERSE : A_BOLD);
    }
    mvwhline(window, row + 7 + table->visible, 0, ACS_HLINE, width);
    char range[64];
    snprintf(range, sizeof(range), "%d-%d / %d", table->count ? table->first + 1 : 0,
        table->first + table->visible < table->count ? table->first + table->visible : table->count, table->count);
    clipped(window, row + 8 + table->visible, width - (int)strlen(range), (int)strlen(range), range);
    if (table->count) {
        const struct tui_partition_row *part = &table->rows[table->cursor];
        char selected[192];
        snprintf(selected, sizeof(selected), "%s  %s", part->path[0] ? part->path : T("Unallocated"), part->name);
        clipped(window, row + 8 + table->visible, 0, width - (int)strlen(range) - 2, selected);
    }
}

static int previous_character(const char *value, int cursor)
{
    if (cursor <= 0) return 0;
    --cursor;
    while (cursor && ((unsigned char)value[cursor] & 0xc0) == 0x80) --cursor;
    return cursor;
}

int tui_edit_value(char *value, size_t capacity, int *cursor, unsigned key, int special, int secret)
{
    size_t length = strlen(value);
    if (*cursor < 0 || (size_t)*cursor > length) *cursor = (int)length;
    if (special && key == KEY_LEFT) { *cursor = previous_character(value, *cursor); return 1; }
    if (special && key == KEY_RIGHT) {
        if ((size_t)*cursor < length) { wchar_t wide; *cursor += (int)character(value + *cursor, &wide); }
        return 1;
    }
    if ((special && key == KEY_HOME) || (!special && key == 1)) { *cursor = 0; return 1; }
    if ((special && key == KEY_END) || (!special && key == 5)) { *cursor = (int)length; return 1; }
    if (!special && key == 21) { memset(value, 0, capacity); *cursor = 0; return 1; }
    int start = *cursor, end = *cursor;
    if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) start = previous_character(value, start);
    else if (special && key == KEY_DC) { if ((size_t)end < length) { wchar_t wide; end += (int)character(value + end, &wide); } }
    else if (!special && !iswcntrl((wint_t)key)) {
        char encoded[MB_LEN_MAX]; mbstate_t state = {0};
        size_t count = wcrtomb(encoded, (wchar_t)key, &state);
        if (count == (size_t)-1 || length + count >= capacity) return 0;
        if (secret && (installer_model_secret_characters(encoded, count) < 0 ||
                       installer_model_secret_characters(value, length) >= 32)) return 0;
        memmove(value + start + count, value + start, length - start + 1);
        memcpy(value + start, encoded, count); *cursor += (int)count;
        return 1;
    } else return 0;
    memmove(value + start, value + end, length - end + 1);
    memset(value + length - (end - start), 0, capacity - (length - (end - start)));
    *cursor = start;
    return 1;
}

int tui_step(struct tui_view *view, const char *primary, int back, int cancel, int timeout_ms)
{
    int height = LINES > 36 ? 36 : LINES;
    int width = COLS > 110 ? 110 : COLS;
    int top = (LINES - height) / 2, left = (COLS - width) / 2;
    erase(); curs_set(0);
    wtimeout(stdscr, timeout_ms);
    if (height < 20 || width < 60) {
        clipped(stdscr, 0, 0, width, T("Resize the terminal to at least 60 columns and 20 rows."));
        refresh();
        wint_t key; int result = get_wch(&key);
        return cancel && (installer_tui_signal || (result == OK && (key == 27 || key == 3))) ? TUI_CANCEL : TUI_NONE;
    }
    WINDOW *frame = newwin(height, width, top, left);
    if (!frame) return TUI_CANCEL;
    keypad(frame, TRUE);
    wtimeout(frame, timeout_ms);
    idlok(frame, FALSE);
    idcok(frame, FALSE);
    wbkgd(frame, has_colors() ? COLOR_PAIR(2) : A_NORMAL);
    box(frame, 0, 0);
    int controls[100], control_count = 0, rows = 0;
    int content_width = width - 6, visible = height - 8, inline_column = 0;
    for (int i = 0; i < view->count; ++i) {
        struct tui_item *item = &view->items[i];
        item->column = 0; item->width = content_width;
        item->height = item->kind == TUI_TEXT ? wrapped(NULL, 0, content_width, item->document ? item->document : item->label) + 1 : item->kind == TUI_INPUT ? 3 : 2;
        if (item->kind == TUI_PARTITIONS) {
            int shown = item->table->count;
            if (shown > 6) shown = 6;
            if (shown > visible - 12) shown = visible - 12;
            if (shown < 1) shown = 1;
            item->height = shown + 10;
        }
        if (item->compact) {
            item->height = 1;
            item->width = text_width(item->label) + (item->kind == TUI_RADIO ? 6 : 4);
            if (item->width > content_width) item->width = content_width;
            if (!inline_column || inline_column + item->width > content_width) {
                item->row = rows++;
                inline_column = 0;
            } else item->row = rows - 1;
            item->column = inline_column;
            inline_column += item->width + 2;
        } else {
            inline_column = 0;
            item->row = rows;
            rows += item->height;
        }
        if (item->kind != TUI_TEXT) controls[control_count++] = i;
    }
    if (primary) controls[control_count++] = -TUI_NEXT;
    if (back) controls[control_count++] = -TUI_BACK;
    if (cancel) controls[control_count++] = -TUI_CANCEL;
    if (view->focus >= control_count || view->focus < 0) view->focus = 0;
    int selected = control_count ? controls[view->focus] : -1;
    if (view->previous_focus != view->focus && selected >= 0) {
        struct tui_item *item = &view->items[selected];
        if (item->row < view->scroll) view->scroll = item->row;
        if (item->row + item->height > view->scroll + visible) view->scroll = item->row + item->height - visible;
        view->cursor = item->value ? (int)strlen(item->value) : 0;
    }
    view->previous_focus = view->focus;
    int max_scroll = rows > visible ? rows - visible : 0;
    if (view->scroll > max_scroll) view->scroll = max_scroll;
    if (view->scroll < 0) view->scroll = 0;
    clipped(frame, 0, 2, width - 4, view->title);
    mvwhline(frame, 1, 1, ACS_HLINE, width - 2);
    WINDOW *canvas = newpad(rows + visible + 1, content_width + 1);
    if (!canvas) { delwin(frame); return TUI_CANCEL; }
    wbkgd(canvas, has_colors() ? COLOR_PAIR(2) : A_NORMAL);
    idlok(canvas, FALSE);
    idcok(canvas, FALSE);
    int cursor_row = -1, cursor_column = 0;
    for (int i = 0; i < view->count; ++i) {
        struct tui_item *item = &view->items[i];
        if (item->kind == TUI_PARTITIONS) {
            partition_table(canvas, item->row, content_width, item->height, item->table, i == selected);
            continue;
        }
        if (i == selected) wattron(canvas, A_REVERSE);
        char prefix[8] = "";
        if (item->kind == TUI_RADIO) snprintf(prefix, sizeof(prefix), "(%c) ", *item->selection == item->option ? '*' : ' ');
        if (item->kind == TUI_CHECK) snprintf(prefix, sizeof(prefix), "[%c] ", *item->selection ? 'x' : ' ');
        if (item->kind == TUI_TEXT) wrapped(canvas, item->row, content_width, item->document ? item->document : item->label);
        else {
            int start = item->column;
            if (item->compact && item->kind == TUI_BUTTON) {
                char label[800]; snprintf(label, sizeof(label), "[ %s ]", item->label);
                clipped(canvas, item->row, start, item->width, label);
            } else {
                mvwaddstr(canvas, item->row, start, prefix);
                clipped(canvas, item->row, start + (int)strlen(prefix), item->width - (int)strlen(prefix), item->label);
            }
        }
        if (i == selected) wattroff(canvas, A_REVERSE);
        if (item->kind == TUI_INPUT) {
            int start = 0, cells = 0;
            for (int position = 0; position < view->cursor && item->value[position];) {
                wchar_t wide; size_t bytes = character(item->value + position, &wide);
                cells += item->secret ? 1 : (wcwidth(wide) > 0 ? wcwidth(wide) : 0); position += bytes;
                while (cells >= content_width - 2 && start < position) {
                    bytes = character(item->value + start, &wide);
                    cells -= item->secret ? 1 : (wcwidth(wide) > 0 ? wcwidth(wide) : 0); start += bytes;
                }
            }
            wattron(canvas, A_REVERSE);
            mvwhline(canvas, item->row + 1, 0, ' ', content_width);
            if (item->secret) {
                int column = 0;
                for (const char *p = item->value + start; *p && column < content_width - 1;) {
                    wchar_t wide; p += character(p, &wide); mvwaddch(canvas, item->row + 1, column++, '*');
                }
            } else clipped(canvas, item->row + 1, 0, content_width - 1, item->value + start);
            wattroff(canvas, A_REVERSE);
            if (i == selected) { cursor_row = item->row + 1 - view->scroll + 2; cursor_column = cells + 3; }
        }
    }
    mvwhline(frame, height - 6, 1, ACS_HLINE, width - 2);
    clipped(frame, height - 5, 2, width - 4, view->status);
    clipped(frame, height - 3, 2, width - 4, selected >= 0 && view->items[selected].kind == TUI_PARTITIONS
        ? T("Up/Down: partition   PgUp/PgDn: page   Home/End: first/last   Tab: actions")
        : T("Tab / arrows: select   Space: toggle   PgUp/PgDn: scroll"));
    const char *buttons[] = {primary, back ? T("Back") : NULL, cancel ? T("Cancel") : NULL};
    const int actions[] = {TUI_NEXT, TUI_BACK, TUI_CANCEL};
    int column = 2;
    for (int i = 0; i < 3; ++i) if (buttons[i]) {
        char label[160]; snprintf(label, sizeof(label), " %s %s ", i == 0 ? "F3" : i == 1 ? "F2" : "Esc", buttons[i]);
        if (selected == -actions[i]) wattron(frame, A_REVERSE);
        clipped(frame, height - 2, column, width - column - 1, label);
        if (selected == -actions[i]) wattroff(frame, A_REVERSE);
        /* Footer labels are translated and measured in terminal cells. */
        for (const char *p = label; *p;) { wchar_t wide; p += character(p, &wide); int cells = wcwidth(wide); if (cells > 0) column += cells; }
        column += 2;
    }
    wnoutrefresh(stdscr);
    wnoutrefresh(frame);
    pnoutrefresh(canvas, view->scroll, 0, top + 2, left + 3, top + height - 7, left + width - 4);
    if (cursor_row >= 2 && cursor_row <= height - 7) { curs_set(1); setsyx(top + cursor_row, left + cursor_column); }
    doupdate();
    wint_t key; int result = wget_wch(frame, &key);
    delwin(canvas);
    delwin(frame);
    if (installer_tui_signal) return cancel ? TUI_CANCEL : TUI_NONE;
    if (result == ERR) return TUI_NONE;
    if (result == KEY_CODE_YES && key == KEY_RESIZE) { clearok(stdscr, TRUE); return TUI_NONE; }
    if (result == KEY_CODE_YES && key == KEY_F(3)) return primary ? TUI_NEXT : TUI_NONE;
    if (result == KEY_CODE_YES && key == KEY_F(2)) return back ? TUI_BACK : TUI_NONE;
    if (result == KEY_CODE_YES && key == KEY_F(4)) return TUI_LOG;
    if (result == OK && (key == 27 || key == 3)) return cancel ? TUI_CANCEL : TUI_NONE;
    if (selected >= 0 && view->items[selected].kind == TUI_PARTITIONS && result == KEY_CODE_YES) {
        struct tui_partition_table *table = view->items[selected].table;
        int movement = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : key == KEY_PPAGE ? -table->visible :
            key == KEY_NPAGE ? table->visible : key == KEY_HOME ? -(int)TUI_PARTITION_ROWS :
            key == KEY_END ? (int)TUI_PARTITION_ROWS : 0;
        if (movement) { tui_partition_table_move(table, movement); return TUI_NONE; }
    }
    if (result == KEY_CODE_YES && (key == KEY_NPAGE || key == KEY_PPAGE)) { view->scroll += key == KEY_NPAGE ? visible : -visible; return TUI_NONE; }
    if (result == KEY_CODE_YES && (key == KEY_HOME || key == KEY_END) &&
        (selected < 0 || view->items[selected].kind != TUI_INPUT)) {
        view->focus = key == KEY_HOME || !control_count ? 0 : control_count - 1;
        return TUI_NONE;
    }
    if ((result == OK && key == '\t') || (result == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_UP || key == KEY_BTAB))) {
        if (control_count) view->focus = (view->focus + ((key == KEY_UP || key == KEY_BTAB) ? control_count - 1 : 1)) % control_count;
        return TUI_NONE;
    }
    int enter = (result == OK && (key == '\n' || key == '\r')) || (result == KEY_CODE_YES && key == KEY_ENTER);
    if (selected < -1) return enter ? -selected : TUI_NONE;
    if (selected < 0) return TUI_NONE;
    struct tui_item *item = &view->items[selected];
    if (item->kind == TUI_INPUT) {
        if (enter) { view->focus = (view->focus + 1) % control_count; return TUI_NONE; }
        if (!tui_edit_value(item->value, item->capacity, &view->cursor, key, result == KEY_CODE_YES, item->secret)) beep();
    } else if (enter || (result == OK && key == ' ')) {
        if (item->kind == TUI_BUTTON) return item->action;
        if (item->kind == TUI_RADIO) *item->selection = item->option;
        if (item->kind == TUI_CHECK) *item->selection = !*item->selection;
    }
    return TUI_NONE;
}

int tui_confirm(const char *title, const char *message, const char *word)
{
    struct tui_view view = {.previous_focus = -1}; char answer[64] = "";
    for (;;) {
        tui_begin(&view, title); tui_text(&view, message);
        if (word) { tui_text(&view, word); tui_input(&view, T("Type the confirmation word"), answer, sizeof(answer), 0); }
        int action = tui_step(&view, T("Continue"), 0, 1, 200);
        if (action == TUI_CANCEL) return 0;
        if (action == TUI_NEXT) {
            if (!word || !strcmp(answer, word)) return 1;
            snprintf(view.status, sizeof(view.status), "%s", T("The confirmation word does not match."));
        }
    }
}

int tui_edit(const char *title, const char *label, char *text, size_t capacity)
{
    struct tui_view view = {.previous_focus = -1};
    for (;;) {
        tui_begin(&view, title); tui_input(&view, label, text, capacity, 0);
        int action = tui_step(&view, T("Apply"), 0, 1, 200);
        if (action == TUI_CANCEL) return 0;
        if (action == TUI_NEXT) return 1;
    }
}

int tui_pick(const char *title, const char *const *options, int count, int initial)
{
    struct tui_view view = {.previous_focus = -1}; int selected = initial;
    for (;;) {
        tui_begin(&view, title);
        for (int i = 0; i < count; ++i) tui_radio(&view, options[i], &selected, i);
        int action = tui_step(&view, T("Apply"), 0, 1, 200);
        if (action == TUI_CANCEL) return -1;
        if (action == TUI_NEXT) return selected;
    }
}

void tui_message(const char *title, const char *message)
{
    struct tui_view view = {.previous_focus = -1};
    for (;;) {
        tui_begin(&view, title); tui_text(&view, message);
        int action = tui_step(&view, T("Close"), 0, 1, 200);
        if (action == TUI_NEXT || action == TUI_CANCEL) return;
    }
}

void tui_read_log(const char *path, char *text, size_t capacity)
{
    if (!capacity) return;
    text[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    if (fd < 0) { snprintf(text, capacity, "%s", strerror(errno)); return; }
    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode)) {
        close(fd); snprintf(text, capacity, "%s", T("Cannot read installer log")); return;
    }
    off_t start = status.st_size > (off_t)(capacity - 1) ? status.st_size - (off_t)(capacity - 1) : 0;
    size_t used = 0;
    while (used < capacity - 1) {
        ssize_t count = pread(fd, text + used, capacity - 1 - used, start + used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += count;
    }
    text[used] = 0;
    close(fd);
    if (start) {
        char *line = strchr(text, '\n');
        if (line) memmove(text, line + 1, strlen(line + 1) + 1);
    }
}
