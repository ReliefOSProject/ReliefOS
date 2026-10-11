#include "engine.h"
#include "debug_click.h"
#include "model.h"
#include "installer/model.h"
#include "doomlauncher/model.h"
#include "imageview/model.h"
#include "leonmmcoset/model.h"
#include "minesweeper/model.h"
#include "paint/model.h"
#include "xiaobai/model.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static const struct {
        const char *expression;
        long long value;
        int valid;
    } cases[] = {
        {"2+3*4", 14, 1}, {"(2+3)*4", 20, 1},
        {" -7 / 3 ", -2, 1}, {"--4", 4, 1},
        {"9223372036854775807", LLONG_MAX, 1},
        {"-9223372036854775808", LLONG_MIN, 1},
        {"(-9223372036854775807-1)*1", LLONG_MIN, 1},
        {"(-9223372036854775807-1)*0", 0, 1},
        {"9223372036854775807+1", 0, 0},
        {"-9223372036854775808-1", 0, 0},
        {"3037000500*3037000500", 0, 0},
        {"(-9223372036854775807-1)/-1", 0, 0},
        {"-(-9223372036854775807-1)", 0, 0},
        {"1/0", 0, 0}, {"(1+2", 0, 0},
        {"1+", 0, 0}, {"1 2", 0, 0}, {"", 0, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        long long value = 0;
        int valid = calc_evaluate(cases[i].expression, &value);
        if (valid != cases[i].valid || (valid && value != cases[i].value)) {
            fprintf(stderr, "FAIL - %s: valid=%d value=%lld\n", cases[i].expression, valid, value);
            return 1;
        }
    }
    struct calc_state calc = {0};
    calc_input(&calc, "12+3");
    calc_input(&calc, "=");
    assert(strcmp(calc.result, "15") == 0);
    calc_input(&calc, "BS");
    assert(strcmp(calc.expression, "12+") == 0);
    calc_input(&calc, "C");
    assert(!calc.expression[0] && !calc.result[0]);
    calc_input(&calc, "1/0");
    calc_input(&calc, "=");
    assert(calc.error);
    calc_input(&calc, "BS");
    calc_input(&calc, "2");
    calc_input(&calc, "=");
    assert(!calc.error && strcmp(calc.result, "0") == 0);
    calc_input(&calc, "C");
    for (unsigned i = 0; i < 200; ++i) calc_input(&calc, "1");
    assert(strlen(calc.expression) == CALC_EXPR_MAX - 1);

    struct osver_debug_click clicks = {0};
    for (unsigned i = 0; i < 4; ++i) assert(!osver_debug_click(&clicks, 100 + i * 100, 1));
    assert(osver_debug_click(&clicks, 500, 1));
    assert(!osver_debug_click(&clicks, 600, 1));
    assert(!osver_debug_click(&clicks, 700, 0));
    for (unsigned i = 0; i < 4; ++i) assert(!osver_debug_click(&clicks, 800 + i * 100, 1));
    assert(!osver_debug_click(&clicks, 3001, 1));
    for (unsigned i = 0; i < 3; ++i) assert(!osver_debug_click(&clicks, 3100 + i * 100, 1));
    assert(osver_debug_click(&clicks, 3400, 1));
    clicks = (struct osver_debug_click){0};
    for (unsigned i = 0; i < 4; ++i) assert(!osver_debug_click(&clicks, UINT_MAX - 300 + i * 100, 1));
    assert(osver_debug_click(&clicks, 50, 1));

    /* Task manager model: formatting, utilization math, history ring and tree order. */
    {
        char buf[32];
        taskmgr_format_percent(buf, sizeof(buf), 50);
        assert(strcmp(buf, "50%") == 0);
        taskmgr_format_percent(buf, sizeof(buf), 100);
        assert(strcmp(buf, "100%") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 0);
        assert(strcmp(buf, "0 B") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 512);
        assert(strcmp(buf, "512 KB") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 1024);
        assert(strcmp(buf, "1 MB") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 1536);
        assert(strcmp(buf, "1.5 MB") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 9728);
        assert(strcmp(buf, "9.5 MB") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 10752);
        assert(strcmp(buf, "10 MB") == 0);
        taskmgr_format_memory(buf, sizeof(buf), 1997);
        assert(strcmp(buf, "2 MB") == 0);
        taskmgr_format_kib(buf, sizeof(buf), 12345);
        assert(strcmp(buf, "12345 KiB") == 0);
        taskmgr_format_uptime(buf, sizeof(buf), 0);
        assert(strcmp(buf, "0:00:00") == 0);
        taskmgr_format_uptime(buf, sizeof(buf), 61000);
        assert(strcmp(buf, "0:01:01") == 0);
        taskmgr_format_uptime(buf, sizeof(buf), 3661000);
        assert(strcmp(buf, "1:01:01") == 0);
        taskmgr_format_hex_fixed(buf, sizeof(buf), 0x1234, 12);
        assert(strcmp(buf, "0x000000001234") == 0);
        taskmgr_format_dec(buf, sizeof(buf), 4294967296ULL);
        assert(strcmp(buf, "4294967296") == 0);
    }
    assert(strcmp(taskmgr_state_name(0), "ready") == 0);
    assert(strcmp(taskmgr_state_name(2), "sleep") == 0);
    assert(strcmp(taskmgr_state_name(9), "?") == 0);
    assert(strcmp(taskmgr_kind_name(1), "user") == 0);
    assert(strcmp(taskmgr_kind_name(0), "kern") == 0);

    assert(taskmgr_task_cpu_percent(50, 100, 0, 0) == 50);
    assert(taskmgr_task_cpu_percent(200, 100, 0, 0) == 100);
    assert(taskmgr_task_cpu_percent(50, 0, 33, 1) == 33);
    assert(taskmgr_task_cpu_percent(50, 0, 0, 0) == 0);

    assert(taskmgr_busy_percent(100, 100, 150, 150, 0, 0) == 50);
    assert(taskmgr_busy_percent(0, 0, 10, 90, 0, 0) == 10);
    assert(taskmgr_busy_percent(100, 100, 150, 150, 1, 33) == 50);
    assert(taskmgr_busy_percent(150, 150, 150, 150, 1, 33) == 33);
    assert(taskmgr_busy_percent(0, 0, 0, 0, 0, 0) == 0);

    assert(taskmgr_mem_percent(1024, 256) == 75);
    assert(taskmgr_mem_percent(0, 0) == 0);
    assert(taskmgr_mem_percent(100, 200) == 0);

    assert(taskmgr_task_killable(42, 7, 1, 1, 0) == 1);
    assert(taskmgr_task_killable(0, 7, 1, 1, 0) == 0);
    assert(taskmgr_task_killable(42, 7, 0, 1, 0) == 0);
    assert(taskmgr_task_killable(42, 7, 1, 3, 0) == 0);
    assert(taskmgr_task_killable(42, 7, 1, 1, 1) == 0);
    assert(taskmgr_task_killable(7, 7, 1, 1, 0) == 0);

    {
        struct taskmgr_perf_history history;
        uint32_t slot;
        taskmgr_perf_history_init(&history);
        assert(history.head == 0 && history.count == 0);
        slot = taskmgr_perf_history_push(&history, 42, 0, 0);
        assert(slot == 0 && history.head == 1 && history.count == 1);
        assert(history.memory[0] == 42);
        assert(history.gpu[0] == TASKMGR_PERF_MISSING);
        slot = taskmgr_perf_history_push(&history, 250, 55, 1);
        assert(slot == 1 && history.memory[1] == 100 && history.gpu[1] == 55);
        history.core[0][slot] = 77;
        assert(history.core[0][1] == 77);
        for (uint32_t i = history.count; i < TASKMGR_PERF_HISTORY; ++i)
            taskmgr_perf_history_push(&history, i, 0, 0);
        assert(history.count == TASKMGR_PERF_HISTORY);
        assert(history.head == 0);
        slot = taskmgr_perf_history_push(&history, 5, 9, 1);
        assert(slot == 0 && history.count == TASKMGR_PERF_HISTORY);
        taskmgr_perf_history_clear_gpu(&history);
        assert(history.gpu[0] == TASKMGR_PERF_MISSING &&
               history.gpu[TASKMGR_PERF_HISTORY - 1] == TASKMGR_PERF_MISSING);
    }

    {
        const uint32_t pids[] = {0, 1, 2, 4};
        const uint32_t parents[] = {0, 0, 1, 2};
        struct taskmgr_tree_row rows[4];
        assert(taskmgr_tree_order(pids, parents, 4, rows, 4) == 4);
        assert(rows[0].index == 0 && rows[0].depth == 0);
        assert(rows[1].index == 1 && rows[1].depth == 1);
        assert(rows[2].index == 2 && rows[2].depth == 2);
        assert(rows[3].index == 3 && rows[3].depth == 3);
    }
    {
        const uint32_t pids[] = {10, 20, 30, 40};
        const uint32_t parents[] = {0, 10, 10, 30};
        struct taskmgr_tree_row rows[4];
        assert(taskmgr_tree_order(pids, parents, 4, rows, 4) == 4);
        assert(rows[0].index == 0 && rows[0].depth == 0);
        assert(rows[1].index == 1 && rows[1].depth == 1);
        assert(rows[2].index == 2 && rows[2].depth == 1);
        assert(rows[3].index == 3 && rows[3].depth == 2);
    }
    {
        const uint32_t pids[] = {5, 6};
        const uint32_t parents[] = {6, 5};
        struct taskmgr_tree_row rows[2];
        assert(taskmgr_tree_order(pids, parents, 2, rows, 2) == 2);
        assert(rows[0].index == 0 && rows[0].depth == 0);
        assert(rows[1].index == 1 && rows[1].depth == 0);
    }
    {
        const uint32_t pids[] = {0, 1, 2, 4};
        const uint32_t parents[] = {0, 0, 1, 2};
        struct taskmgr_tree_row rows[2];
        assert(taskmgr_tree_order(pids, parents, 4, rows, 2) == 2);
        assert(rows[0].index == 0 && rows[1].index == 1);
    }

    {
        struct msw_game g;
        memset(&g, 0xff, sizeof(g));
        msw_reset(&g, 1234u);
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                assert(g.cells[y][x] == 0 && g.adjacent[y][x] == 0);
            }
        }
        assert(g.mines_placed == 0 && g.game_over == 0 && g.won == 0);
        assert(g.revealed_count == 0 && g.flagged_count == 0 && g.rng_state != 0);
    }
    assert(msw_in_board(0, 0) && msw_in_board(8, 8));
    assert(!msw_in_board(-1, 0) && !msw_in_board(0, -1));
    assert(!msw_in_board(9, 0) && !msw_in_board(0, 9));
    {
        struct msw_game a;
        struct msw_game b;
        uint32_t first;
        msw_reset(&a, 42u);
        msw_reset(&b, 42u);
        first = msw_rng_next(&a);
        assert(first == msw_rng_next(&b));
        assert(msw_rng_next(&a) != first);
    }
    {
        struct msw_game g;
        int mines = 0;
        msw_reset(&g, 7u);
        msw_place_mines(&g, 4, 4);
        assert(g.mines_placed == 1);
        assert(!(g.cells[4][4] & MSW_CELL_MINE));
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                if (g.cells[y][x] & MSW_CELL_MINE) {
                    ++mines;
                }
            }
        }
        assert(mines == MSW_MINES);
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx || dy) && msw_in_board(x + dx, y + dy) &&
                            (g.cells[y + dy][x + dx] & MSW_CELL_MINE)) {
                            ++count;
                        }
                    }
                }
                assert(g.adjacent[y][x] == count);
            }
        }
    }
    {
        int found = 0;
        for (uint32_t seed = 1; seed <= 40 && !found; ++seed) {
            struct msw_game g;
            msw_reset(&g, seed);
            msw_place_mines(&g, 8, 8);
            for (int y = 0; y < MSW_ROWS && !found; ++y) {
                for (int x = 0; x < MSW_COLS && !found; ++x) {
                    if (g.adjacent[y][x] == 0 && !(g.cells[y][x] & MSW_CELL_MINE)) {
                        msw_reveal(&g, x, y);
                        assert(g.revealed_count > 1 && g.game_over == 0);
                        found = 1;
                    }
                }
            }
        }
        assert(found);
    }
    {
        struct msw_game g;
        int mx = -1;
        int my = -1;
        msw_reset(&g, 3u);
        msw_place_mines(&g, 0, 0);
        for (int y = 0; y < MSW_ROWS && mx < 0; ++y) {
            for (int x = 0; x < MSW_COLS && mx < 0; ++x) {
                if (g.cells[y][x] & MSW_CELL_MINE) {
                    mx = x;
                    my = y;
                }
            }
        }
        assert(mx >= 0);
        msw_reveal(&g, mx, my);
        assert(g.game_over == 1 && g.won == 0);
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                if (g.cells[y][x] & MSW_CELL_MINE) {
                    assert(g.cells[y][x] & MSW_CELL_REVEALED);
                }
            }
        }
    }
    {
        struct msw_game g;
        msw_reset(&g, 5u);
        msw_toggle_flag(&g, 2, 2);
        assert(g.cells[2][2] & MSW_CELL_FLAGGED && g.flagged_count == 1);
        msw_toggle_flag(&g, 2, 2);
        assert(!(g.cells[2][2] & MSW_CELL_FLAGGED) && g.flagged_count == 0);
        for (int i = 0; i < MSW_MINES; ++i) {
            msw_toggle_flag(&g, i % MSW_COLS, i / MSW_COLS);
        }
        assert(g.flagged_count == MSW_MINES);
        msw_toggle_flag(&g, 8, 8);
        assert(g.flagged_count == MSW_MINES && !(g.cells[8][8] & MSW_CELL_FLAGGED));
        msw_reset(&g, 5u);
        g.cells[3][3] |= MSW_CELL_REVEALED;
        msw_toggle_flag(&g, 3, 3);
        assert(!(g.cells[3][3] & MSW_CELL_FLAGGED) && g.flagged_count == 0);
        g.game_over = 1;
        msw_toggle_flag(&g, 4, 4);
        assert(!(g.cells[4][4] & MSW_CELL_FLAGGED));
    }
    {
        struct msw_game g;
        msw_reset(&g, 1u);
        for (int i = 0; i < MSW_MINES; ++i) {
            int x = i % MSW_COLS;
            int y = i / MSW_COLS;
            g.cells[y][x] |= MSW_CELL_MINE;
        }
        g.mines_placed = 1;
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx || dy) && msw_in_board(x + dx, y + dy) &&
                            (g.cells[y + dy][x + dx] & MSW_CELL_MINE)) {
                            ++count;
                        }
                    }
                }
                g.adjacent[y][x] = (uint8_t)count;
            }
        }
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                if (!(g.cells[y][x] & MSW_CELL_MINE)) {
                    msw_reveal(&g, x, y);
                }
            }
        }
        assert(g.game_over == 1 && g.won == 1);
        assert(g.flagged_count == MSW_MINES);
        for (int y = 0; y < MSW_ROWS; ++y) {
            for (int x = 0; x < MSW_COLS; ++x) {
                if (g.cells[y][x] & MSW_CELL_MINE) {
                    assert(g.cells[y][x] & MSW_CELL_FLAGGED);
                }
            }
        }
    }
    {
        struct msw_game g;
        char buf[32];
        msw_reset(&g, 1u);
        msw_mines_left_text(&g, buf, sizeof(buf));
        assert(strcmp(buf, "10") == 0);
        g.flagged_count = 3;
        msw_mines_left_text(&g, buf, sizeof(buf));
        assert(strcmp(buf, "07") == 0);
        g.flagged_count = 10;
        msw_mines_left_text(&g, buf, sizeof(buf));
        assert(strcmp(buf, "00") == 0);
        g.flagged_count = 12;
        msw_mines_left_text(&g, buf, sizeof(buf));
        assert(strcmp(buf, "-02") == 0);
    }
    {
        uint8_t bmp[54 + 4 * 4 * 4];
        struct msw_sprite s;
        uint8_t *pixels = bmp + 54;
        memset(bmp, 0, sizeof(bmp));
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 32;
        bmp[30] = 0;
        pixels[0] = 1;
        pixels[1] = 2;
        pixels[2] = 3;
        pixels[3] = 4;
        pixels[4] = 5;
        pixels[5] = 6;
        pixels[6] = 7;
        pixels[7] = 8;
        pixels[8] = 9;
        pixels[9] = 10;
        pixels[10] = 11;
        pixels[11] = 12;
        pixels[12] = 13;
        pixels[13] = 14;
        pixels[14] = 15;
        pixels[15] = 16;
        assert(msw_sprite_parse(bmp, sizeof(bmp), &s) == 1);
        assert(s.width == 2 && s.height == 2);
        assert(s.pixels[0] == (12u << 24 | 11u << 16 | 10u << 8 | 9u));
        assert(s.pixels[1] == (16u << 24 | 15u << 16 | 14u << 8 | 13u));
        assert(s.pixels[20] == (4u << 24 | 3u << 16 | 2u << 8 | 1u));
        assert(s.pixels[21] == (8u << 24 | 7u << 16 | 6u << 8 | 5u));
        assert(s.pixels[2] == 0);
        bmp[22] = 0xfe;
        bmp[23] = 0xff;
        bmp[24] = 0xff;
        bmp[25] = 0xff;
        assert(msw_sprite_parse(bmp, sizeof(bmp), &s) == 1);
        assert(s.pixels[0] == (4u << 24 | 3u << 16 | 2u << 8 | 1u));
        assert(s.pixels[20] == (12u << 24 | 11u << 16 | 10u << 8 | 9u));
        bmp[0] = 'X';
        assert(msw_sprite_parse(bmp, sizeof(bmp), &s) == 0);
        bmp[0] = 'B';
        bmp[28] = 24;
        assert(msw_sprite_parse(bmp, sizeof(bmp), &s) == 0);
        bmp[28] = 32;
        bmp[18] = 21;
        assert(msw_sprite_parse(bmp, sizeof(bmp), &s) == 0);
        bmp[18] = 2;
        assert(msw_sprite_parse(bmp, 60, &s) == 0);
    }
    {
        struct leonmmcoset_fit f;
        leonmmcoset_fit_rect(&f, 760, 760, 8, 100, 50);
        assert(f.w == 744 && f.h == 372 && f.x == 8 && f.y == 194);
        leonmmcoset_fit_rect(&f, 760, 760, 8, 50, 100);
        assert(f.w == 372 && f.h == 744 && f.x == 194 && f.y == 8);
        leonmmcoset_fit_rect(&f, 100, 50, 8, 10, 10);
        assert(f.w == 34 && f.h == 34 && f.x == 33 && f.y == 8);
        leonmmcoset_fit_rect(&f, 760, 760, 8, 0, 50);
        assert(f.w == 0 && f.h == 0 && f.x == 0 && f.y == 0);
        leonmmcoset_fit_rect(&f, 10, 10, 8, 4, 4);
        assert(f.w == 0 && f.h == 0);
    }
    {
        struct xiaobai_fit f;
        xiaobai_fit_rect(&f, 760, 760, 8, 100, 50);
        assert(f.w == 744 && f.h == 372 && f.x == 8 && f.y == 194);
        xiaobai_fit_rect(&f, 760, 760, 8, 50, 100);
        assert(f.w == 372 && f.h == 744 && f.x == 194 && f.y == 8);
        xiaobai_fit_rect(&f, 100, 50, 8, 10, 10);
        assert(f.w == 34 && f.h == 34 && f.x == 33 && f.y == 8);
        xiaobai_fit_rect(&f, 760, 760, 8, 0, 50);
        assert(f.w == 0 && f.h == 0 && f.x == 0 && f.y == 0);
        xiaobai_fit_rect(&f, 10, 10, 8, 4, 4);
        assert(f.w == 0 && f.h == 0);
    }

    /* Paint model: BMP codec round trip and brush stamping rules. */
    {
        uint32_t source[4] = {0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu};
        uint8_t *data = 0;
        uint32_t len = 0;
        uint32_t *pixels = 0;
        uint32_t w = 0, h = 0;
        assert(paint_bmp_encode(source, 2, 2, &data, &len) == 0);
        assert(data && len == 54 + 2 * 8); /* 24bpp rows pad to 8 bytes */
        assert(paint_bmp_decode(data, len, &pixels, &w, &h) == 0);
        assert(w == 2 && h == 2);
        assert(pixels[0] == source[0] && pixels[1] == source[1]);
        assert(pixels[2] == source[2] && pixels[3] == source[3]);
        free(pixels);
        pixels = 0;
        assert(paint_bmp_decode(data, 30, &pixels, &w, &h) == -1);
        data[0] = 'X';
        assert(paint_bmp_decode(data, len, &pixels, &w, &h) == -1);
        free(data);
    }
    {
        uint32_t buffer[16 * 16];
        memset(buffer, 0, sizeof(buffer));
        paint_draw_point(buffer, 16, 16, 8, 8, 0x00112233u, 3, PAINT_TOOL_PENCIL);
        assert(buffer[8 * 16 + 8] == 0x00112233u);
        assert(buffer[7 * 16 + 7] == 0x00112233u); /* square dab */
        assert(buffer[5 * 16 + 5] == 0);
        memset(buffer, 0, sizeof(buffer));
        paint_draw_point(buffer, 16, 16, 8, 8, 0x00112233u, 4, PAINT_TOOL_BRUSH);
        assert(buffer[8 * 16 + 8] == 0x00112233u);
        assert(buffer[6 * 16 + 6] == 0); /* circular dab clips the corner */
        paint_draw_point(buffer, 16, 16, 8, 8, 0x00000000u, 4, PAINT_TOOL_ERASER);
        assert(buffer[8 * 16 + 8] == 0x00ffffffu); /* eraser paints white */
        memset(buffer, 0, sizeof(buffer));
        paint_draw_line(buffer, 16, 16, 1, 1, 14, 14, 0x00aa5500u, 2,
                        PAINT_TOOL_PENCIL);
        assert(buffer[1 * 16 + 1] == 0x00aa5500u);
        assert(buffer[8 * 16 + 8] == 0x00aa5500u); /* interpolation has no gaps */
        assert(buffer[14 * 16 + 14] == 0x00aa5500u);
        paint_draw_point(buffer, 16, 16, 0, 0, 0x00000001u, 14, PAINT_TOOL_BRUSH);
        paint_draw_line(buffer, 16, 16, 15, 15, 0, 15, 0x00000001u, 2,
                        PAINT_TOOL_PENCIL);
    }

    /* Image viewer model: extension filter, zoom math, navigation, detail. */
    {
        uint32_t w = 0, h = 0;
        char text[64];
        assert(imageview_is_supported_path("a/b/c.bmp"));
        assert(imageview_is_supported_path("C.PNG"));
        assert(imageview_is_supported_path("x.dib"));
        assert(!imageview_is_supported_path("x.txt"));
        assert(!imageview_is_supported_path(""));
        assert(!imageview_is_supported_path(0));
        imageview_zoom_dims(100, 50, IMAGEVIEW_ZOOM_FIT, 100, 100, &w, &h);
        assert(w == 100 && h == 50);
        imageview_zoom_dims(100, 50, IMAGEVIEW_ZOOM_FIT, 25, 100, &w, &h);
        assert(w == 25 && h == 12);
        imageview_zoom_dims(100, 50, IMAGEVIEW_ZOOM_1X, 10, 10, &w, &h);
        assert(w == 100 && h == 50);
        imageview_zoom_dims(100, 50, IMAGEVIEW_ZOOM_2X, 10, 10, &w, &h);
        assert(w == 200 && h == 100);
        assert(imageview_next_index(0, 3, -1) == 2);
        assert(imageview_next_index(2, 3, 1) == 0);
        assert(imageview_next_index(1, 3, 1) == 2);
        assert(imageview_next_index(1, 3, -1) == 0);
        assert(imageview_next_index(4, 1, 1) == 0);
        imageview_format_detail(text, sizeof(text), 800, 600, IMAGEVIEW_ZOOM_FIT, 1, 5);
        assert(strcmp(text, "800x600  Fit  2/5") == 0);
        imageview_format_detail(text, sizeof(text), 800, 600, IMAGEVIEW_ZOOM_2X, 0, 0);
        assert(strcmp(text, "800x600  2x") == 0);
        imageview_format_detail(text, sizeof(text), 0, 0, IMAGEVIEW_ZOOM_1X, 0, 1);
        assert(text[0] == 0);
    }
    {
        uint32_t source[4] = {0x00010203u, 0x00040506u, 0x00070809u, 0x000a0b0cu};
        uint8_t *data = 0;
        uint32_t len = 0;
        uint32_t *pixels = 0;
        uint32_t w = 0, h = 0;
        assert(paint_bmp_encode(source, 2, 2, &data, &len) == 0);
        assert(imageview_bmp_decode(data, len, &pixels, &w, &h) == 0);
        assert(w == 2 && h == 2);
        assert(pixels[0] == source[0] && pixels[3] == source[3]);
        free(pixels);
        data[0] = 'X';
        pixels = 0;
        assert(imageview_bmp_decode(data, len, &pixels, &w, &h) == -1);
        free(data);
    }

    /* DOOM launcher model: argv assembly, overflow and exit-code mapping. */
    {
        struct doomlauncher_options options;
        char *argv[DOOMLAUNCHER_MAX_ARGS];
        char *extra[2];
        int argc;
        doomlauncher_defaults(&options, "/doom/freedoom1.wad");
        assert(strcmp(options.iwad, "/doom/freedoom1.wad") == 0);
        assert(options.disable_sound == 1 && options.fullscreen == 0);
        argc = doomlauncher_build_argv("/doom/doom.elf", &options, 0, 0, argv,
                                       DOOMLAUNCHER_MAX_ARGS);
        assert(argc == 5);
        assert(strcmp(argv[0], "/doom/doom.elf") == 0);
        assert(strcmp(argv[1], "-iwad") == 0);
        assert(strcmp(argv[2], "/doom/freedoom1.wad") == 0);
        assert(strcmp(argv[3], "-nosound") == 0);
        assert(strcmp(argv[4], "-windowed") == 0);
        assert(argv[5] == 0);
        options.disable_sound = 0;
        options.fullscreen = 1;
        extra[0] = "-skill";
        extra[1] = "4";
        argc = doomlauncher_build_argv("/doom/doom.elf", &options, extra, 2, argv,
                                       DOOMLAUNCHER_MAX_ARGS);
        assert(argc == 5);
        assert(strcmp(argv[3], "-skill") == 0 && strcmp(argv[4], "4") == 0);
        assert(argv[5] == 0);
        options.iwad[0] = 0;
        assert(doomlauncher_build_argv("/doom/doom.elf", &options, 0, 0, argv,
                                       DOOMLAUNCHER_MAX_ARGS) ==
               DOOMLAUNCHER_ERR_NO_IWAD);
        options.iwad[0] = '/';
        assert(doomlauncher_build_argv("/doom/doom.elf", &options, extra, 2, argv,
                                       5) == DOOMLAUNCHER_ERR_TOO_MANY_ARGS);
        assert(doomlauncher_exit_code(1, 0x200) == 2);
        assert(doomlauncher_exit_code(1, 0xff) == 0);
        assert(doomlauncher_exit_code(0, 0x200) == -1);
    }

    /* Installer wizard model: page flow, mode branching and disk rows. */
    {
        enum installer_page steps[INSTALLER_PAGE_COUNT];
        char buf[64];
        int count;

        assert(installer_model_next(INSTALLER_PAGE_LANGUAGE, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_THANKS);
        assert(installer_model_next(INSTALLER_PAGE_THANKS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_WELCOME);
        assert(installer_model_next(INSTALLER_PAGE_MODE, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_DISK);
        assert(installer_model_next(INSTALLER_PAGE_DISK, INSTALLER_MODE_UPDATE) ==
               INSTALLER_PAGE_CONFIRM);
        assert(installer_model_next(INSTALLER_PAGE_DISK, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_PARTITIONS);
        assert(installer_model_next(INSTALLER_PAGE_PARTITIONS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_ACCOUNTS);
        assert(installer_model_next(INSTALLER_PAGE_ACCOUNTS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_CONFIRM);
        assert(installer_model_next(INSTALLER_PAGE_CONFIRM, INSTALLER_MODE_UPDATE) ==
               INSTALLER_PAGE_PROGRESS);
        assert(installer_model_next(INSTALLER_PAGE_PROGRESS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_PROGRESS);
        assert(installer_model_prev(INSTALLER_PAGE_CONFIRM, INSTALLER_MODE_UPDATE) ==
               INSTALLER_PAGE_DISK);
        assert(installer_model_prev(INSTALLER_PAGE_CONFIRM, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_ACCOUNTS);
        assert(installer_model_prev(INSTALLER_PAGE_ACCOUNTS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_PARTITIONS);
        assert(installer_model_prev(INSTALLER_PAGE_PARTITIONS, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_DISK);
        assert(installer_model_prev(INSTALLER_PAGE_WELCOME, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_THANKS);
        assert(installer_model_prev(INSTALLER_PAGE_FINISH, INSTALLER_MODE_UPDATE) ==
               INSTALLER_PAGE_DISK);
        assert(installer_model_prev(INSTALLER_PAGE_LANGUAGE, INSTALLER_MODE_FRESH) ==
               INSTALLER_PAGE_LANGUAGE);
        assert(!installer_model_can_go_back(INSTALLER_PAGE_LANGUAGE, 0));
        assert(installer_model_can_go_back(INSTALLER_PAGE_DISK, 0));
        assert(!installer_model_can_go_back(INSTALLER_PAGE_PROGRESS, 0));
        assert(installer_model_can_go_back(INSTALLER_PAGE_FINISH, 0));
        assert(!installer_model_can_go_back(INSTALLER_PAGE_FINISH, 1));
        assert(installer_model_can_cancel(INSTALLER_PAGE_CONFIRM, 0));
        assert(!installer_model_can_cancel(INSTALLER_PAGE_PROGRESS, 0));
        assert(!installer_model_can_cancel(INSTALLER_PAGE_FINISH, 1));
        assert(installer_model_action(INSTALLER_PAGE_CONFIRM, INSTALLER_MODE_UPDATE, 0) ==
               INSTALLER_ACTION_UPDATE);
        assert(installer_model_action(INSTALLER_PAGE_CONFIRM, INSTALLER_MODE_FRESH, 0) ==
               INSTALLER_ACTION_INSTALL);
        assert(installer_model_action(INSTALLER_PAGE_MODE, INSTALLER_MODE_FRESH, 0) ==
               INSTALLER_ACTION_NEXT);
        assert(installer_model_action(INSTALLER_PAGE_FINISH, INSTALLER_MODE_FRESH, 1) ==
               INSTALLER_ACTION_RESTART);
        assert(installer_model_action(INSTALLER_PAGE_FINISH, INSTALLER_MODE_FRESH, 0) ==
               INSTALLER_ACTION_CLOSE);
        assert(strcmp(installer_model_confirm_word(INSTALLER_MODE_UPDATE), "UPDATE") == 0);
        assert(strcmp(installer_model_confirm_word(INSTALLER_MODE_FRESH), "INSTALL") == 0);

        count = installer_model_steps(INSTALLER_MODE_FRESH, steps, INSTALLER_PAGE_COUNT);
        assert(count == INSTALLER_PAGE_COUNT);
        assert(steps[0] == INSTALLER_PAGE_LANGUAGE);
        assert(steps[1] == INSTALLER_PAGE_THANKS);
        assert(steps[2] == INSTALLER_PAGE_WELCOME);
        assert(steps[count - 1] == INSTALLER_PAGE_FINISH);
        count = installer_model_steps(INSTALLER_MODE_UPDATE, steps, INSTALLER_PAGE_COUNT);
        assert(count == INSTALLER_PAGE_COUNT - 2);
        for (int i = 0; i < count; ++i) assert(steps[i] != INSTALLER_PAGE_ACCOUNTS);
        count = installer_model_steps(INSTALLER_MODE_FRESH, steps, 2);
        assert(count == INSTALLER_PAGE_COUNT && steps[1] == INSTALLER_PAGE_THANKS);

        installer_model_format_disk_line(buf, sizeof(buf), 0, "vda", 2048 * 1024, 512);
        assert(strcmp(buf, "Disk 0  vda  1 GiB") == 0);
        installer_model_format_disk_line(buf, sizeof(buf), 3, "", 1024, 512);
        assert(strcmp(buf, "Disk 3  Disk  0 MiB") == 0);
        installer_model_format_disk_line(buf, sizeof(buf), 7, "sdb", 4096, 1024);
        assert(strcmp(buf, "Disk 7  sdb  4 MiB") == 0);
    }

    {
        char secret[33] = "";
        assert(installer_model_edit_secret(secret, sizeof(secret), 0, 0, "U!ab", 4));
        assert(strcmp(secret, "U!ab") == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 2, 4, "c", 1));
        assert(strcmp(secret, "U!c") == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 1, 2, NULL, 0));
        assert(strcmp(secret, "Uc") == 0);
        assert(!installer_model_edit_secret(secret, sizeof(secret), 3, 3, "x", 1));
        assert(!installer_model_edit_secret(secret, sizeof(secret), 0, 1, " ", 1));
        assert(!installer_model_edit_secret(secret, sizeof(secret), 0, 1, "\n", 1));
        assert(strcmp(secret, "Uc") == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 0, 2,
                                           "12345678901234567890123456789012", 32));
        assert(strlen(secret) == 32);
        assert(!installer_model_edit_secret(secret, sizeof(secret), 32, 32, "x", 1));
        assert(strlen(secret) == 32);
        assert(installer_model_edit_secret(secret, sizeof(secret), 0, 32, NULL, 0));
        assert(secret[0] == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 0, 0, "中a文", 7));
        assert(strcmp(secret, "中a文") == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 1, 2, "🙂", 4));
        assert(strcmp(secret, "中🙂文") == 0);
        assert(installer_model_edit_secret(secret, sizeof(secret), 1, 2, NULL, 0));
        assert(strcmp(secret, "中文") == 0);
        assert(!installer_model_edit_secret(secret, sizeof(secret), 0, 0, "\xc0\xaf", 2));
        assert(strcmp(secret, "中文") == 0);
    }

    puts("ok - calculator arithmetic, editing, bounds and logo click timing");
    puts("ok - task manager formatting, utilization, history ring and tree order");
    puts("ok - minesweeper placement, reveal, flags, win/lose and sprite parsing");
    puts("ok - easter egg image fitting for leonmmcoset and xiaobai");
    puts("ok - paint brushes, image viewer scaling and DOOM launcher argv");
    puts("ok - installer wizard page flow, mode branching and disk rows");
    return 0;
}
