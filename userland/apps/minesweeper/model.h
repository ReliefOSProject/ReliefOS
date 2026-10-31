#ifndef RELIEFOS_MINESWEEPER_MODEL_H
#define RELIEFOS_MINESWEEPER_MODEL_H

/*
 * Toolkit-independent minesweeper behavior: board state, mine placement,
 * reveal flood, flag rules, win/lose transitions and sprite BMP parsing.
 * The Motif frontend maps its widgets onto these; the host suite runs them.
 */
#include <stdint.h>

#define MSW_COLS 9
#define MSW_ROWS 9
#define MSW_MINES 10
#define MSW_CELLS (MSW_COLS * MSW_ROWS)
#define MSW_SPRITE_SIZE 20
#define MSW_SPRITE_BMP_MAX_BYTES (MSW_SPRITE_SIZE * MSW_SPRITE_SIZE * 4U + 128U)

#define MSW_CELL_MINE 0x01u
#define MSW_CELL_REVEALED 0x02u
#define MSW_CELL_FLAGGED 0x04u

struct msw_game {
    uint8_t cells[MSW_ROWS][MSW_COLS];
    uint8_t adjacent[MSW_ROWS][MSW_COLS];
    uint8_t mines_placed;
    uint8_t game_over;
    uint8_t won;
    uint32_t revealed_count;
    uint32_t flagged_count;
    uint32_t rng_state;
};

struct msw_sprite {
    uint32_t width;
    uint32_t height;
    uint32_t pixels[MSW_SPRITE_SIZE * MSW_SPRITE_SIZE];
};

void msw_reset(struct msw_game *game, uint32_t seed);
int msw_in_board(int x, int y);
uint32_t msw_rng_next(struct msw_game *game);
void msw_place_mines(struct msw_game *game, int safe_x, int safe_y);
void msw_reveal(struct msw_game *game, int x, int y);
void msw_toggle_flag(struct msw_game *game, int x, int y);
void msw_mines_left_text(const struct msw_game *game, char *buf, uint32_t cap);
int msw_sprite_parse(const uint8_t *bmp, uint32_t len, struct msw_sprite *sprite);

#endif
