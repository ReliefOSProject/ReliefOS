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
    bindtextdomain("snake", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("snake", "UTF-8");
    textdomain("snake");
}

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define SNAKE_MAX_WIDTH 32U
#define SNAKE_MAX_HEIGHT 18U
#define SNAKE_MAX_LENGTH (SNAKE_MAX_WIDTH * SNAKE_MAX_HEIGHT)
#define SNAKE_MIN_WIDTH 12U
#define SNAKE_MIN_HEIGHT 8U
#define SNAKE_DEFAULT_WIDTH 28U
#define SNAKE_DEFAULT_HEIGHT 16U
#define SNAKE_SCORE_CAP 32U
#define SNAKE_TICK_START_MS 140UL
#define SNAKE_TICK_MIN_MS 65UL
#define SNAKE_TICK_STEP_MS 5UL
#define SNAKE_POLL_MS 5UL
#define SNAKE_ESCAPE_ATTEMPTS 30U

enum snake_direction {
    SNAKE_UP,
    SNAKE_RIGHT,
    SNAKE_DOWN,
    SNAKE_LEFT,
};

struct snake_point {
    uint8_t x;
    uint8_t y;
};

struct snake_game {
    struct snake_point body[SNAKE_MAX_LENGTH];
    uint32_t length;
    struct snake_point food;
    unsigned width;
    unsigned height;
    unsigned direction;
    unsigned next_direction;
    unsigned score;
    unsigned high_score;
    unsigned paused;
    unsigned game_over;
    uint32_t rng;
};

static struct termios saved_termios;
static unsigned terminal_active;

static void write_all(const char *text)
{
    size_t offset = 0;
    size_t length;
    long written;
    if (!text) {
        return;
    }
    length = strlen(text);
    while (offset < length) {
        written = write(STDOUT_FILENO, text + offset, length - offset);
        if (written <= 0) {
            return;
        }
        offset += (size_t)written;
    }
}

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
    write_all("\033[?1049h\033[?25l\033[2J\033[H");
    return 0;
}

static uint32_t rng_next(struct snake_game *game)
{
    game->rng = game->rng * 1664525U + 1013904223U;
    return game->rng;
}

static int point_equal(struct snake_point left, struct snake_point right)
{
    return left.x == right.x && left.y == right.y;
}

static int contains_point(const struct snake_game *game, struct snake_point point,
                          uint32_t count)
{
    uint32_t index;
    for (index = 0; index < count; ++index) {
        if (point_equal(game->body[index], point)) {
            return 1;
        }
    }
    return 0;
}

static void place_food(struct snake_game *game)
{
    unsigned attempts;
    struct snake_point point;
    for (attempts = 0; attempts < SNAKE_MAX_LENGTH * 2U; ++attempts) {
        point.x = (uint8_t)(rng_next(game) % game->width);
        point.y = (uint8_t)(rng_next(game) % game->height);
        if (!contains_point(game, point, game->length)) {
            game->food = point;
            return;
        }
    }
    for (point.y = 0; point.y < game->height; ++point.y) {
        for (point.x = 0; point.x < game->width; ++point.x) {
            if (!contains_point(game, point, game->length)) {
                game->food = point;
                return;
            }
        }
    }
    game->game_over = 1;
}

static void reset_game(struct snake_game *game)
{
    struct snake_point head;
    game->length = 4;
    game->direction = SNAKE_RIGHT;
    game->next_direction = SNAKE_RIGHT;
    game->score = 0;
    game->paused = 0;
    game->game_over = 0;
    head.x = (uint8_t)(game->width / 2U);
    head.y = (uint8_t)(game->height / 2U);
    game->body[0] = head;
    game->body[1] = (struct snake_point){(uint8_t)(head.x - 1U), head.y};
    game->body[2] = (struct snake_point){(uint8_t)(head.x - 2U), head.y};
    game->body[3] = (struct snake_point){(uint8_t)(head.x - 3U), head.y};
    place_food(game);
}

static void detect_board_size(unsigned *width, unsigned *height)
{
    struct winsize size;
    unsigned columns = 80;
    unsigned rows = 24;
    if (tcgetwinsize(STDOUT_FILENO, &size) == 0) {
        if (size.ws_col >= 2) {
            columns = size.ws_col;
        }
        if (size.ws_row >= 4) {
            rows = size.ws_row;
        }
    }
    *width = (columns - 2U) / 2U;
    if (*width < SNAKE_MIN_WIDTH) {
        *width = SNAKE_MIN_WIDTH;
    }
    if (*width > SNAKE_MAX_WIDTH) {
        *width = SNAKE_MAX_WIDTH;
    }
    *height = rows > 4U ? rows - 4U : SNAKE_DEFAULT_HEIGHT;
    if (*height < SNAKE_MIN_HEIGHT) {
        *height = SNAKE_MIN_HEIGHT;
    }
    if (*height > SNAKE_MAX_HEIGHT) {
        *height = SNAKE_MAX_HEIGHT;
    }
}

static int score_path(char *path, size_t capacity)
{
    const char *home = getenv("HOME");
    int result;
    if (!path || capacity == 0) {
        return -1;
    }
    if (home && home[0]) {
        result = snprintf(path, capacity, "%s/.snake.score", home);
        if (result > 0 && (size_t)result < capacity) {
            return 0;
        }
    }
    result = snprintf(path, capacity, "/tmp/.snake.score");
    return result > 0 && (size_t)result < capacity ? 0 : -1;
}

static unsigned load_high_score(void)
{
    char path[256];
    char text[SNAKE_SCORE_CAP];
    unsigned value = 0;
    size_t length = 0;
    long got;
    int too_long = 0;
    int fd;
    if (score_path(path, sizeof(path)) != 0) {
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    while (length + 1U < sizeof(text)) {
        got = read(fd, text + length, sizeof(text) - length - 1U);
        if (got <= 0) {
            break;
        }
        length += (size_t)got;
    }
    if (length + 1U == sizeof(text)) {
        got = read(fd, text, 1);
        too_long = got > 0;
    }
    close(fd);
    text[length] = 0;
    if (!length || too_long) {
        return 0;
    }
    if (text[length - 1U] == '\n') {
        --length;
        text[length] = 0;
    }
    if (!length || (length > 0 && text[length - 1U] == '\r')) {
        return 0;
    }
    for (size_t index = 0; index < length; ++index) {
        unsigned digit;
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        digit = (unsigned)(text[index] - '0');
        if (value > 100000000U / 10U ||
            (value == 100000000U / 10U && digit > 100000000U % 10U)) {
            return 0;
        }
        value = value * 10U + digit;
    }
    return value;
}

static void save_high_score(unsigned score)
{
    char path[256];
    char text[SNAKE_SCORE_CAP];
    int length;
    int fd;
    long written;
    if (score_path(path, sizeof(path)) != 0) {
        return;
    }
    length = snprintf(text, sizeof(text), "%u\n", score);
    if (length <= 0 || (size_t)length >= sizeof(text)) {
        return;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    written = write(fd, text, (size_t)length);
    close(fd);
    (void)written;
}

static void render(const struct snake_game *game)
{
    char output[4096];
    char line[512];
    size_t output_length = 0;
    unsigned x;
    unsigned y;
    int length;
    memcpy(output, "\033[H\033[2J", 7);
    output_length = 7;
    length = snprintf(line, sizeof(line),
                      T("Snake  Score: %u  High score: %u  %s\n"),
                      game->score, game->high_score,
                      game->game_over ? T("GAME OVER - R restart, Q quit") :
                      game->paused ? T("PAUSED - P or Space to resume") :
                                     T("Arrows/WASD move, P pause, Q quit"));
    if (length > 0) {
        struct winsize size = {0};
        unsigned columns = 80;
        if (tcgetwinsize(STDOUT_FILENO, &size) == 0 && size.ws_col > 1)
            columns = size.ws_col;
        size_t available = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1U;
        size_t offset = 0, cells = 0;
        mbstate_t state = {0};
        while (offset < available && line[offset] != '\n') {
            wchar_t wc;
            size_t bytes = mbrtowc(&wc, line + offset, available - offset, &state);
            if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0) break;
            int width = wcwidth(wc);
            if (width < 0) width = 1;
            if (cells + (size_t)width >= columns || output_length + bytes + 1U >= sizeof(output)) break;
            memcpy(output + output_length, line + offset, bytes);
            output_length += bytes;
            offset += bytes;
            cells += (size_t)width;
        }
        output[output_length++] = '\n';
    }
    for (x = 0; x < game->width + 2U; ++x) {
        output[output_length++] = '#';
        output[output_length++] = '#';
    }
    output[output_length++] = '\n';
    for (y = 0; y < game->height; ++y) {
        output[output_length++] = '#';
        output[output_length++] = '#';
        for (x = 0; x < game->width; ++x) {
            struct snake_point point = {(uint8_t)x, (uint8_t)y};
            const char *cell = "  ";
            if (point_equal(point, game->food)) {
                cell = "()";
            }
            for (unsigned index = game->length; index > 0; --index) {
                if (point_equal(point, game->body[index - 1U])) {
                    cell = index == 1U ? "@@" : "[]";
                    break;
                }
            }
            output[output_length++] = cell[0];
            output[output_length++] = cell[1];
        }
        output[output_length++] = '#';
        output[output_length++] = '#';
        output[output_length++] = '\n';
    }
    for (x = 0; x < game->width + 2U; ++x) {
        output[output_length++] = '#';
        output[output_length++] = '#';
    }
    output[output_length++] = '\n';
    write_bytes(output, output_length);
}

static int read_byte(unsigned char *value)
{
    struct pollfd input;
    if (!value) {
        return 0;
    }
    /* libc intentionally keeps read(0) blocking for normal terminal apps.
     * Snake needs a polling read so its animation clock can run without input. */
    input.fd = STDIN_FILENO;
    input.events = POLLIN;
    input.revents = 0;
    if (poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN)) {
        return 0;
    }
    return read(STDIN_FILENO, value, 1) == 1;
}

static int read_escape_byte(unsigned char *value)
{
    unsigned attempt;
    for (attempt = 0; attempt < SNAKE_ESCAPE_ATTEMPTS; ++attempt) {
        if (read_byte(value)) {
            return 1;
        }
        (void)poll(0, 0, 1);
    }
    return 0;
}

static int read_key(void)
{
    unsigned char value;
    unsigned char sequence;
    if (!read_byte(&value)) {
        return 0;
    }
    if (value == 27) {
        if (!read_escape_byte(&sequence) || sequence != '[' ||
            !read_escape_byte(&sequence)) {
            return 27;
        }
        switch (sequence) {
        case 'A': return 'w';
        case 'B': return 's';
        case 'C': return 'd';
        case 'D': return 'a';
        default: return 0;
        }
    }
    return value;
}

static void set_direction(struct snake_game *game, unsigned direction)
{
    if ((game->next_direction + 2U) % 4U != direction) {
        game->next_direction = direction;
    }
}

static void process_key(struct snake_game *game, int key, unsigned *running)
{
    if (key == 'q' || key == 'Q' || key == 27 || key == 3) {
        *running = 0;
        return;
    }
    if (game->game_over) {
        if (key == 'r' || key == 'R') {
            reset_game(game);
        }
        return;
    }
    if (key == 'p' || key == 'P' || key == ' ') {
        game->paused = !game->paused;
        return;
    }
    switch (key) {
    case 'w': case 'W': set_direction(game, SNAKE_UP); break;
    case 'd': case 'D': set_direction(game, SNAKE_RIGHT); break;
    case 's': case 'S': set_direction(game, SNAKE_DOWN); break;
    case 'a': case 'A': set_direction(game, SNAKE_LEFT); break;
    default: break;
    }
}

static void move_snake(struct snake_game *game)
{
    struct snake_point next = game->body[0];
    unsigned index;
    unsigned growing;
    game->direction = game->next_direction;
    switch (game->direction) {
    case SNAKE_UP: --next.y; break;
    case SNAKE_RIGHT: ++next.x; break;
    case SNAKE_DOWN: ++next.y; break;
    case SNAKE_LEFT: --next.x; break;
    default: break;
    }
    if (next.x >= game->width || next.y >= game->height) {
        game->game_over = 1;
        return;
    }
    growing = point_equal(next, game->food);
    if (contains_point(game, next, game->length - (growing ? 0U : 1U))) {
        game->game_over = 1;
        return;
    }
    if (growing && game->length < SNAKE_MAX_LENGTH) {
        ++game->length;
        ++game->score;
        if (game->score > game->high_score) {
            game->high_score = game->score;
            save_high_score(game->high_score);
        }
    }
    for (index = game->length; index > 1U; --index) {
        game->body[index - 1U] = game->body[index - 2U];
    }
    game->body[0] = next;
    if (growing) {
        place_food(game);
    }
}

static unsigned long current_tick_ms(const struct snake_game *game)
{
    unsigned long tick = SNAKE_TICK_START_MS;
    if (game->score > 0) {
        tick -= (unsigned long)game->score * SNAKE_TICK_STEP_MS;
    }
    return tick < SNAKE_TICK_MIN_MS ? SNAKE_TICK_MIN_MS : tick;
}

int main(void)
{
    init_localization();
    struct snake_game game;
    unsigned running = 1;
    unsigned width;
    unsigned height;
    detect_board_size(&width, &height);
    memset(&game, 0, sizeof(game));
    game.width = width;
    game.height = height;
    game.rng = (uint32_t)getpid() * 2654435761U ^ (uint32_t)(uintptr_t)&game;
    if (!game.rng) {
        game.rng = 1;
    }
    game.high_score = load_high_score();
    reset_game(&game);
    if (setup_terminal() != 0) {
        puts(T("snake requires an interactive terminal"));
        return 1;
    }
    while (running) {
        unsigned long tick = current_tick_ms(&game);
        unsigned long waited = 0;
        render(&game);
        while (running && waited < tick) {
            int key = read_key();
            if (key) {
                process_key(&game, key, &running);
                render(&game);
            }
            (void)poll(0, 0, SNAKE_POLL_MS);
            waited += SNAKE_POLL_MS;
        }
        if (running && !game.paused && !game.game_over) {
            move_snake(&game);
        }
    }
    if (game.score > game.high_score) {
        save_high_score(game.score);
    }
    restore_terminal();
    write_all("\n");
    return 0;
}
