#include "model.h"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_le32s(const uint8_t *p)
{
    return (int32_t)read_le32(p);
}

void msw_reset(struct msw_game *game, uint32_t seed)
{
    uint32_t y;
    uint32_t x;
    if (!game) return;
    for (y = 0; y < MSW_ROWS; ++y) {
        for (x = 0; x < MSW_COLS; ++x) {
            game->cells[y][x] = 0;
            game->adjacent[y][x] = 0;
        }
    }
    game->mines_placed = 0;
    game->game_over = 0;
    game->won = 0;
    game->revealed_count = 0;
    game->flagged_count = 0;
    game->rng_state = seed ? seed : 1u;
}

int msw_in_board(int x, int y)
{
    return x >= 0 && y >= 0 && x < MSW_COLS && y < MSW_ROWS;
}

uint32_t msw_rng_next(struct msw_game *game)
{
    if (!game) return 0;
    game->rng_state = game->rng_state * 1664525u + 1013904223u;
    return game->rng_state;
}

static void add_mine_adjacency(struct msw_game *game, int mine_x, int mine_y)
{
    int dy;
    int dx;
    for (dy = -1; dy <= 1; ++dy) {
        for (dx = -1; dx <= 1; ++dx) {
            int x = mine_x + dx;
            int y = mine_y + dy;
            if ((dx || dy) && msw_in_board(x, y)) {
                ++game->adjacent[y][x];
            }
        }
    }
}

void msw_place_mines(struct msw_game *game, int safe_x, int safe_y)
{
    uint8_t candidates[MSW_CELLS];
    uint32_t candidate_count = 0;
    uint32_t placed;
    int y;
    int x;
    if (!game) return;
    for (y = 0; y < MSW_ROWS; ++y) {
        for (x = 0; x < MSW_COLS; ++x) {
            if (x != safe_x || y != safe_y) {
                candidates[candidate_count++] = (uint8_t)(y * MSW_COLS + x);
            }
        }
    }
    for (placed = 0; placed < MSW_MINES; ++placed) {
        uint32_t selected = placed + msw_rng_next(game) % (candidate_count - placed);
        uint8_t index = candidates[selected];
        int mine_x = (int)(index % MSW_COLS);
        int mine_y = (int)(index / MSW_COLS);
        candidates[selected] = candidates[placed];
        candidates[placed] = index;
        game->cells[mine_y][mine_x] |= MSW_CELL_MINE;
        add_mine_adjacency(game, mine_x, mine_y);
    }
    game->mines_placed = 1;
}

static void reveal_all_mines(struct msw_game *game)
{
    uint32_t y;
    uint32_t x;
    for (y = 0; y < MSW_ROWS; ++y) {
        for (x = 0; x < MSW_COLS; ++x) {
            if (game->cells[y][x] & MSW_CELL_MINE) {
                game->cells[y][x] |= MSW_CELL_REVEALED;
            }
        }
    }
}

void msw_reveal(struct msw_game *game, int x, int y)
{
    uint8_t queue[MSW_CELLS];
    uint32_t head = 0;
    uint32_t tail = 0;
    if (!game || !msw_in_board(x, y) ||
        (game->cells[y][x] & (MSW_CELL_REVEALED | MSW_CELL_FLAGGED)) ||
        game->game_over) {
        return;
    }
    game->cells[y][x] |= MSW_CELL_REVEALED;
    ++game->revealed_count;
    if (game->cells[y][x] & MSW_CELL_MINE) {
        game->game_over = 1;
        game->won = 0;
        reveal_all_mines(game);
        return;
    }
    queue[tail++] = (uint8_t)(y * MSW_COLS + x);
    while (head < tail) {
        uint8_t index = queue[head++];
        int current_x = (int)(index % MSW_COLS);
        int current_y = (int)(index / MSW_COLS);
        if (game->adjacent[current_y][current_x] != 0) {
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int next_x = current_x + dx;
                int next_y = current_y + dy;
                if ((dx || dy) && msw_in_board(next_x, next_y) &&
                    !(game->cells[next_y][next_x] &
                      (MSW_CELL_MINE | MSW_CELL_REVEALED | MSW_CELL_FLAGGED))) {
                    game->cells[next_y][next_x] |= MSW_CELL_REVEALED;
                    ++game->revealed_count;
                    queue[tail++] = (uint8_t)(next_y * MSW_COLS + next_x);
                }
            }
        }
    }
    if (game->revealed_count >= MSW_CELLS - MSW_MINES) {
        uint32_t yy;
        uint32_t xx;
        game->game_over = 1;
        game->won = 1;
        for (yy = 0; yy < MSW_ROWS; ++yy) {
            for (xx = 0; xx < MSW_COLS; ++xx) {
                if (game->cells[yy][xx] & MSW_CELL_MINE) {
                    game->cells[yy][xx] |= MSW_CELL_FLAGGED;
                }
            }
        }
        game->flagged_count = MSW_MINES;
    }
}

void msw_toggle_flag(struct msw_game *game, int x, int y)
{
    if (!game || !msw_in_board(x, y) || game->game_over ||
        (game->cells[y][x] & MSW_CELL_REVEALED)) {
        return;
    }
    if (game->cells[y][x] & MSW_CELL_FLAGGED) {
        game->cells[y][x] &= (uint8_t)~MSW_CELL_FLAGGED;
        if (game->flagged_count) {
            --game->flagged_count;
        }
    } else if (game->flagged_count < MSW_MINES) {
        game->cells[y][x] |= MSW_CELL_FLAGGED;
        ++game->flagged_count;
    }
}

void msw_mines_left_text(const struct msw_game *game, char *buf, uint32_t cap)
{
    int left = MSW_MINES - (int)(game ? game->flagged_count : 0);
    int negative = left < 0;
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    if (negative) {
        left = -left;
    }
    left %= 100;
    if (negative && pos + 1 < cap) {
        buf[pos++] = '-';
    }
    if (pos + 1 < cap) {
        buf[pos++] = (char)('0' + (left / 10) % 10);
    }
    if (pos + 1 < cap) {
        buf[pos++] = (char)('0' + left % 10);
    }
    buf[pos] = 0;
}

int msw_sprite_parse(const uint8_t *bmp, uint32_t len, struct msw_sprite *sprite)
{
    uint32_t pixel_offset;
    uint32_t row_stride;
    uint32_t height;
    int32_t width;
    int32_t height_signed;
    int top_down;
    uint32_t x;
    uint32_t y;

    if (!bmp || !sprite || len < 54U ||
        len > MSW_SPRITE_BMP_MAX_BYTES) {
        return 0;
    }
    if (bmp[0] != 'B' || bmp[1] != 'M' || read_le32(bmp + 14) < 40 ||
        read_le16(bmp + 26) != 1 || read_le32(bmp + 30) != 0 ||
        read_le16(bmp + 28) != 32) {
        return 0;
    }
    pixel_offset = read_le32(bmp + 10);
    width = read_le32s(bmp + 18);
    height_signed = read_le32s(bmp + 22);
    if (width <= 0 || height_signed == 0 || (uint32_t)width > MSW_SPRITE_SIZE) {
        return 0;
    }
    top_down = height_signed < 0;
    height = top_down ? (uint32_t)(-height_signed) : (uint32_t)height_signed;
    if (height > MSW_SPRITE_SIZE || pixel_offset >= len) {
        return 0;
    }
    row_stride = (uint32_t)width * 4U;
    if (height > (len - pixel_offset) / row_stride) {
        return 0;
    }
    for (y = 0; y < MSW_SPRITE_SIZE; ++y) {
        for (x = 0; x < MSW_SPRITE_SIZE; ++x) {
            sprite->pixels[y * MSW_SPRITE_SIZE + x] = 0;
        }
    }
    for (y = 0; y < height; ++y) {
        uint32_t src_y = top_down ? y : height - 1U - y;
        const uint8_t *row = bmp + pixel_offset + src_y * row_stride;
        for (x = 0; x < (uint32_t)width; ++x) {
            const uint8_t *pixel = row + x * 4U;
            sprite->pixels[y * MSW_SPRITE_SIZE + x] =
                ((uint32_t)pixel[3] << 24) | ((uint32_t)pixel[2] << 16) |
                ((uint32_t)pixel[1] << 8) | pixel[0];
        }
    }
    sprite->width = (uint32_t)width;
    sprite->height = height;
    return 1;
}
