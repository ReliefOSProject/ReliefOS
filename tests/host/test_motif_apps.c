#include "engine.h"
#include "debug_click.h"
#include "model.h"
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

    puts("ok - calculator arithmetic, editing, bounds and logo click timing");
    puts("ok - task manager formatting, utilization, history ring and tree order");
    return 0;
}
