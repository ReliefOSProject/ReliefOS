#include "engine.h"
#include "debug_click.h"
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
    puts("ok - calculator arithmetic, editing, bounds and logo click timing");
    return 0;
}
