#ifndef RELIEFOS_CALC_ENGINE_H
#define RELIEFOS_CALC_ENGINE_H

#define CALC_EXPR_MAX 120

struct calc_state {
    char expression[CALC_EXPR_MAX];
    char result[CALC_EXPR_MAX];
    int error;
};

int calc_evaluate(const char *expression, long long *value);
void calc_input(struct calc_state *state, const char *token);

#endif
