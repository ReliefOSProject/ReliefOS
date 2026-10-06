#include "engine.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void skip_spaces(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') ++*p;
}

static int parse_expr(const char **p, long long *out);

static int parse_factor(const char **p, long long *out)
{
    int negative = 0;
    skip_spaces(p);
    while (**p == '+' || **p == '-') {
        if (*(*p)++ == '-') negative = !negative;
        skip_spaces(p);
    }
    if (**p == '(') {
        ++*p;
        if (!parse_expr(p, out)) return 0;
        skip_spaces(p);
        if (*(*p)++ != ')') return 0;
        if (negative) {
            if (*out == LLONG_MIN) return 0;
            *out = -*out;
        }
        return 1;
    }
    unsigned long long magnitude = 0;
    unsigned long long limit = (unsigned long long)LLONG_MAX + negative;
    int digits = 0;
    while (**p >= '0' && **p <= '9') {
        unsigned digit = (unsigned)(**p - '0');
        if (magnitude > (limit - digit) / 10ULL) return 0;
        magnitude = magnitude * 10ULL + digit;
        ++*p;
        digits = 1;
    }
    if (!digits) return 0;
    *out = negative ? -(long long)(magnitude - (magnitude != 0)) - (magnitude != 0)
                    : (long long)magnitude;
    return 1;
}

static int parse_term(const char **p, long long *out)
{
    if (!parse_factor(p, out)) return 0;
    for (;;) {
        skip_spaces(p);
        char op = **p;
        if (op != '*' && op != '/') return 1;
        ++*p;
        long long rhs;
        if (!parse_factor(p, &rhs)) return 0;
        if (op == '*') {
            if (__builtin_mul_overflow(*out, rhs, out)) return 0;
        } else {
            if (!rhs || (*out == LLONG_MIN && rhs == -1)) return 0;
            *out /= rhs;
        }
    }
}

static int parse_expr(const char **p, long long *out)
{
    if (!parse_term(p, out)) return 0;
    for (;;) {
        skip_spaces(p);
        char op = **p;
        if (op != '+' && op != '-') return 1;
        ++*p;
        long long rhs;
        if (!parse_term(p, &rhs)) return 0;
        if (op == '+') {
            if (__builtin_add_overflow(*out, rhs, out)) return 0;
        } else if (__builtin_sub_overflow(*out, rhs, out)) {
            return 0;
        }
    }
}

int calc_evaluate(const char *expression, long long *value)
{
    const char *p = expression;
    if (!expression || !value || strlen(expression) >= CALC_EXPR_MAX) return 0;
    if (!parse_expr(&p, value)) return 0;
    skip_spaces(&p);
    return *p == 0;
}

void calc_input(struct calc_state *state, const char *token)
{
    if (!state || !token) return;
    size_t length = strlen(state->expression);
    if (!strcmp(token, "C")) {
        memset(state, 0, sizeof(*state));
    } else if (!strcmp(token, "BS")) {
        if (length) state->expression[length - 1] = 0;
        state->result[0] = 0;
        state->error = 0;
    } else if (!strcmp(token, "=")) {
        long long value;
        state->error = length && !calc_evaluate(state->expression, &value);
        if (!length || state->error) state->result[0] = 0;
        else snprintf(state->result, sizeof(state->result), "%lld", value);
    } else {
        size_t extra = strlen(token);
        if (extra < sizeof(state->expression) - length) {
            memcpy(state->expression + length, token, extra + 1);
        }
        state->error = 0;
        state->result[0] = 0;
    }
}
