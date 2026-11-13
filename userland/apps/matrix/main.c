#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>

#define T(text) gettext(text)

static void init_localization(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("matrix", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("matrix", "UTF-8");
    textdomain("matrix");
}

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define MATRIX_MAX_COLUMNS 120U
#define MATRIX_MAX_ROWS 40U
#define MATRIX_FRAME_MS 50UL
#define MATRIX_MIN_COLUMNS 16U
#define MATRIX_MIN_ROWS 6U
#define MATRIX_RENDER_BUFFER_BYTES 32768U

enum matrix_shade {
    MATRIX_BLACK,
    MATRIX_GREEN,
    MATRIX_BRIGHT_GREEN,
};

struct matrix_cell {
    unsigned char character;
    unsigned char shade;
};

struct matrix_column {
    int head;
    unsigned trail;
    unsigned speed;
    unsigned phase;
};

struct matrix_state {
    struct matrix_cell cells[MATRIX_MAX_ROWS][MATRIX_MAX_COLUMNS];
    struct matrix_column columns[MATRIX_MAX_COLUMNS];
    unsigned columns_count;
    unsigned rows_count;
    uint32_t random;
    unsigned paused;
};

static struct termios saved_termios;
static unsigned terminal_active;
static char render_buffer[MATRIX_RENDER_BUFFER_BYTES];

static const char matrix_characters[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "!@#$%^&*()_+-=[]{}<>?/\\|";

static void write_bytes(const char *text, size_t length)
{
    size_t offset = 0;
    long written;
    while (text && offset < length) {
        written = write(STDOUT_FILENO, text + offset, length - offset);
        if (written <= 0) {
            return;
        }
        offset += (size_t)written;
    }
}

static void write_all(const char *text)
{
    write_bytes(text, text ? strlen(text) : 0);
}

static void restore_terminal(void)
{
    if (!terminal_active) {
        return;
    }
    write_all("\033[0m\033[?25h\033[?1049l");
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    terminal_active = 0;
}

static int setup_terminal(void)
{
    struct termios raw;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        return -1;
    }
    raw = saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INLCR | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return -1;
    }
    terminal_active = 1;
    write_all("\033[?1049h\033[?25l\033[40m\033[2J\033[H");
    return 0;
}

static uint32_t random_next(struct matrix_state *state)
{
    state->random = state->random * 1664525U + 1013904223U;
    return state->random;
}

static unsigned random_range(struct matrix_state *state, unsigned limit)
{
    return limit ? random_next(state) % limit : 0;
}

static unsigned char random_character(struct matrix_state *state)
{
    return (unsigned char)matrix_characters[
        random_range(state, sizeof(matrix_characters) - 1U)];
}

static void terminal_size(unsigned *columns, unsigned *rows)
{
    struct winsize size;
    unsigned detected_columns = 80;
    unsigned detected_rows = 24;
    if (tcgetwinsize(STDOUT_FILENO, &size) == 0) {
        if (size.ws_col) {
            detected_columns = size.ws_col;
        }
        if (size.ws_row) {
            detected_rows = size.ws_row;
        }
    }
    if (detected_columns < MATRIX_MIN_COLUMNS) {
        detected_columns = MATRIX_MIN_COLUMNS;
    }
    if (detected_rows < MATRIX_MIN_ROWS) {
        detected_rows = MATRIX_MIN_ROWS;
    }
    *columns = detected_columns > MATRIX_MAX_COLUMNS ? MATRIX_MAX_COLUMNS :
               detected_columns;
    *rows = detected_rows > MATRIX_MAX_ROWS ? MATRIX_MAX_ROWS : detected_rows;
}

static void reset_column(struct matrix_state *state, unsigned index,
                         int staggered)
{
    struct matrix_column *column = &state->columns[index];
    unsigned max_trail = state->rows_count / 2U;
    if (max_trail < 4U) {
        max_trail = 4U;
    }
    column->trail = 3U + random_range(state, max_trail);
    column->speed = 1U + random_range(state, 3U);
    column->phase = random_range(state, column->speed);
    column->head = staggered ?
        -(int)random_range(state, state->rows_count + column->trail) :
        -(int)column->trail;
}

static void reset_matrix(struct matrix_state *state, unsigned columns,
                         unsigned rows)
{
    memset(state->cells, 0, sizeof(state->cells));
    state->columns_count = columns;
    state->rows_count = rows;
    for (unsigned index = 0; index < state->columns_count; ++index) {
        reset_column(state, index, 1);
    }
}

static void update_matrix(struct matrix_state *state)
{
    for (unsigned y = 0; y < state->rows_count; ++y) {
        for (unsigned x = 0; x < state->columns_count; ++x) {
            struct matrix_cell *cell = &state->cells[y][x];
            if (cell->shade > MATRIX_BLACK) {
                --cell->shade;
            }
        }
    }
    for (unsigned x = 0; x < state->columns_count; ++x) {
        struct matrix_column *column = &state->columns[x];
        if (column->phase++ % column->speed != 0) {
            continue;
        }
        ++column->head;
        for (unsigned segment = 0; segment < column->trail; ++segment) {
            int y = column->head - (int)segment;
            struct matrix_cell *cell;
            if (y < 0 || y >= (int)state->rows_count) {
                continue;
            }
            cell = &state->cells[y][x];
            cell->character = random_character(state);
            cell->shade = segment == 0 ? MATRIX_BRIGHT_GREEN : MATRIX_GREEN;
        }
        if (column->head - (int)column->trail >= (int)state->rows_count) {
            reset_column(state, x, 0);
        }
    }
}

static void append_text(char *output, size_t capacity, size_t *length,
                        const char *text)
{
    while (text && *text && *length < capacity) {
        output[(*length)++] = *text++;
    }
}

/* Keep translated status text within the terminal's cell width and never
 * split a UTF-8 character at the end of the render buffer. */
static void append_status(char *output, size_t capacity, size_t *length,
                          const char *text, unsigned columns)
{
    size_t cells = 0;
    mbstate_t state = {0};
    while (*text && *length < capacity) {
        wchar_t wc;
        size_t bytes = mbrtowc(&wc, text, strlen(text), &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0) break;
        int width = wcwidth(wc);
        if (width < 0) width = 1;
        if (cells + (size_t)width >= columns || bytes > capacity - *length) break;
        memcpy(output + *length, text, bytes);
        *length += bytes;
        text += bytes;
        cells += (size_t)width;
    }
}

static void render_matrix(const struct matrix_state *state)
{
    char *output = render_buffer;
    size_t length = 0;
    unsigned previous_shade = 99U;
    append_text(output, MATRIX_RENDER_BUFFER_BYTES, &length, "\033[H\033[40m");
    for (unsigned y = 0; y < state->rows_count; ++y) {
        for (unsigned x = 0; x < state->columns_count; ++x) {
            const struct matrix_cell *cell = &state->cells[y][x];
            if (cell->shade != previous_shade) {
                if (cell->shade == MATRIX_BRIGHT_GREEN) {
                    append_text(output, MATRIX_RENDER_BUFFER_BYTES, &length, "\033[92m");
                } else if (cell->shade == MATRIX_GREEN) {
                    append_text(output, MATRIX_RENDER_BUFFER_BYTES, &length, "\033[32m");
                } else {
                    append_text(output, MATRIX_RENDER_BUFFER_BYTES, &length, "\033[30m");
                }
                previous_shade = cell->shade;
            }
            if (length < MATRIX_RENDER_BUFFER_BYTES) {
                output[length++] = cell->shade ? (char)cell->character : ' ';
            }
        }
        if (length < MATRIX_RENDER_BUFFER_BYTES) {
            output[length++] = '\n';
        }
    }
    if (state->paused) {
        struct winsize size = {0};
        unsigned columns = state->columns_count;
        if (tcgetwinsize(STDOUT_FILENO, &size) == 0 && size.ws_col && size.ws_col < columns)
            columns = size.ws_col;
        append_text(output, MATRIX_RENDER_BUFFER_BYTES, &length, "\033[32m");
        append_status(output, MATRIX_RENDER_BUFFER_BYTES, &length,
                      T("[Paused - P resumes, Q exits]"), columns);
    }
    write_bytes(output, length);
}

static int read_key(void)
{
    unsigned char character;
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN) ||
        read(STDIN_FILENO, &character, 1) != 1) {
        return 0;
    }
    return character;
}

int main(void)
{
    init_localization();
    struct matrix_state state;
    unsigned running = 1;
    unsigned columns;
    unsigned rows;
    terminal_size(&columns, &rows);
    memset(&state, 0, sizeof(state));
    state.random = (uint32_t)getpid() * 2654435761U ^
                   (uint32_t)(uintptr_t)&state;
    if (!state.random) {
        state.random = 1;
    }
    reset_matrix(&state, columns, rows);
    if (setup_terminal() != 0) {
        puts(T("matrix requires an interactive terminal"));
        return 1;
    }
    while (running) {
        int key = read_key();
        unsigned resized_columns;
        unsigned resized_rows;
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) {
            running = 0;
            continue;
        }
        if (key == 'p' || key == 'P' || key == ' ') {
            state.paused = !state.paused;
        }
        terminal_size(&resized_columns, &resized_rows);
        if (resized_columns != state.columns_count || resized_rows != state.rows_count) {
            reset_matrix(&state, resized_columns, resized_rows);
        }
        if (!state.paused) {
            update_matrix(&state);
        }
        render_matrix(&state);
        (void)poll(0, 0, MATRIX_FRAME_MS);
    }
    restore_terminal();
    write_all("\n");
    return 0;
}
