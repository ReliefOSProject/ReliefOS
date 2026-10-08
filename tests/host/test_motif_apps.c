#include "engine.h"
#include "debug_click.h"
#include "model.h"
#include "leonmmcoset/model.h"
#include "minesweeper/model.h"
#include "xiaobai/model.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
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

    puts("ok - calculator arithmetic, editing, bounds and logo click timing");
    puts("ok - task manager formatting, utilization, history ring and tree order");
    puts("ok - minesweeper placement, reveal, flags, win/lose and sprite parsing");
    puts("ok - easter egg image fitting for leonmmcoset and xiaobai");
    return 0;
}
