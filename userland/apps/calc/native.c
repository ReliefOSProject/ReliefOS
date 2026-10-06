#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#define CALC_W 312
#define CALC_H 364
#define DISPLAY_W 280
#define DISPLAY_X 16
#define DISPLAY_Y 16
#define DISPLAY_H 58
#define BUTTON_W 60
#define BUTTON_H 34
#define BUTTON_GAP 8
#define GRID_X 16
#define GRID_Y 92
#define GRID_COLS 4
#define GRID_ROWS 5
#define EXPR_MAX 120
#define T(s) gettext(s)

static uint32_t pixels[CALC_W * CALC_H];
static char expr[EXPR_MAX];
static char result_text[EXPR_MAX];
static uint8_t expr_len;
static uint8_t error_state;

static const char *button_labels[GRID_ROWS][GRID_COLS] = {
    {"(", ")", "C", "BS"},
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"0", "=", "+", ""},
};

static int text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void append_char(char *dst, uint8_t *len, uint8_t cap, char ch)
{
    if (!dst || !len || *len + 1 >= cap) {
        return;
    }
    dst[*len] = ch;
    ++(*len);
    dst[*len] = 0;
}

static void set_result_error(const char *text)
{
    error_state = 1;
    copy_text(result_text, sizeof(result_text), text);
}

static void clear_all(void)
{
    expr[0] = 0;
    result_text[0] = 0;
    expr_len = 0;
    error_state = 0;
}

static void backspace_expr(void)
{
    if (expr_len == 0) {
        return;
    }
    --expr_len;
    expr[expr_len] = 0;
}

static int is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void skip_spaces(const char **p)
{
    while (p && *p && is_space(**p)) {
        ++(*p);
    }
}

static int parse_expr(const char **p, long long *out);

static int checked_add(long long a, long long b, long long *out)
{
    unsigned long long ua = (unsigned long long)a;
    unsigned long long ub = (unsigned long long)b;
    unsigned long long ur = ua + ub;
    long long r = (long long)ur;
    if (((a ^ r) & (b ^ r)) < 0) {
        return 0;
    }
    *out = r;
    return 1;
}

static int checked_sub(long long a, long long b, long long *out)
{
    unsigned long long ua = (unsigned long long)a;
    unsigned long long ub = (unsigned long long)b;
    unsigned long long ur = ua - ub;
    long long r = (long long)ur;
    if (((a ^ b) & (a ^ r)) < 0) {
        return 0;
    }
    *out = r;
    return 1;
}

static int checked_mul(long long a, long long b, long long *out)
{
    int neg = 0;
    unsigned long long ua;
    unsigned long long ub;
    unsigned long long res = 0;

    if (a < 0) {
        neg = !neg;
        ua = (unsigned long long)(-(a + 1)) + 1ULL;
    } else {
        ua = (unsigned long long)a;
    }
    if (b < 0) {
        neg = !neg;
        ub = (unsigned long long)(-(b + 1)) + 1ULL;
    } else {
        ub = (unsigned long long)b;
    }

    while (ub) {
        if (ub & 1ULL) {
            if (~res < ua) {
                return 0;
            }
            res += ua;
        }
        ub >>= 1;
        if (!ub) {
            break;
        }
        if ((ua >> 63) != 0) {
            return 0;
        }
        ua <<= 1;
    }

    if (neg) {
        if (res > 0x8000000000000000ULL) {
            return 0;
        }
        *out = -(long long)res;
    } else {
        if (res > 0x7fffffffffffffffULL) {
            return 0;
        }
        *out = (long long)res;
    }
    return 1;
}

static int parse_number(const char **p, long long *out)
{
    long long value = 0;
    int saw_digit = 0;
    skip_spaces(p);
    while (is_digit(**p)) {
        int digit = **p - '0';
        long long tmp;
        saw_digit = 1;
        if (!checked_mul(value, 10, &tmp) || !checked_add(tmp, digit, &value)) {
            return 0;
        }
        ++(*p);
    }
    if (!saw_digit) {
        return 0;
    }
    *out = value;
    return 1;
}

static int parse_factor(const char **p, long long *out)
{
    long long inner;
    skip_spaces(p);
    if (**p == '(') {
        ++(*p);
        if (!parse_expr(p, &inner)) {
            return 0;
        }
        skip_spaces(p);
        if (**p != ')') {
            return 0;
        }
        ++(*p);
        *out = inner;
        return 1;
    }
    if (**p == '+') {
        ++(*p);
        return parse_factor(p, out);
    }
    if (**p == '-') {
        ++(*p);
        if (!parse_factor(p, &inner)) {
            return 0;
        }
        if (inner == (-9223372036854775807LL - 1LL)) {
            return 0;
        }
        *out = -inner;
        return 1;
    }
    return parse_number(p, out);
}

static int parse_term(const char **p, long long *out)
{
    long long value;
    if (!parse_factor(p, &value)) {
        return 0;
    }
    for (;;) {
        long long rhs;
        skip_spaces(p);
        if (**p != '*' && **p != '/') {
            break;
        }
        char op = *(*p)++;
        if (!parse_factor(p, &rhs)) {
            return 0;
        }
        if (op == '*') {
            if (!checked_mul(value, rhs, &value)) {
                return 0;
            }
        } else {
            if (rhs == 0) {
                return 0;
            }
            if (value == (-9223372036854775807LL - 1LL) && rhs == -1) {
                return 0;
            }
            value /= rhs;
        }
    }
    *out = value;
    return 1;
}

static int parse_expr(const char **p, long long *out)
{
    long long value;
    if (!parse_term(p, &value)) {
        return 0;
    }
    for (;;) {
        long long rhs;
        long long next;
        skip_spaces(p);
        if (**p != '+' && **p != '-') {
            break;
        }
        char op = *(*p)++;
        if (!parse_term(p, &rhs)) {
            return 0;
        }
        if (op == '+') {
            if (!checked_add(value, rhs, &next)) {
                return 0;
            }
        } else {
            if (!checked_sub(value, rhs, &next)) {
                return 0;
            }
        }
        value = next;
    }
    *out = value;
    return 1;
}

static void format_i64(long long value, char *buf, uint32_t cap)
{
    char tmp[32];
    uint32_t pos = 0;
    uint32_t n = 0;
    unsigned long long mag;

    if (!buf || cap == 0) {
        return;
    }
    if (value < 0) {
        buf[pos++] = '-';
        mag = (unsigned long long)(-(value + 1)) + 1ULL;
    } else {
        mag = (unsigned long long)value;
    }
    if (mag == 0) {
        if (pos + 1 < cap) {
            buf[pos++] = '0';
        }
        buf[pos] = 0;
        return;
    }
    while (mag && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (mag % 10ULL));
        mag /= 10ULL;
    }
    while (n && pos + 1 < cap) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = 0;
}

static void evaluate_expr(void)
{
    const char *p = expr;
    long long value;
    if (!expr_len) {
        result_text[0] = 0;
        error_state = 0;
        return;
    }
    if (!parse_expr(&p, &value)) {
        set_result_error(T("Error"));
        return;
    }
    skip_spaces(&p);
    if (*p != 0) {
        set_result_error(T("Error"));
        return;
    }
    format_i64(value, result_text, sizeof(result_text));
    error_state = 0;
}

static void append_token(const char *token)
{
    if (!token || !token[0]) {
        return;
    }
    if (error_state) {
        error_state = 0;
        result_text[0] = 0;
    }
    while (*token) {
        append_char(expr, &expr_len, EXPR_MAX, *token++);
    }
}

static void apply_button(const char *label)
{
    if (!label || !label[0]) {
        return;
    }
    if (text_eq(label, "C")) {
        clear_all();
        return;
    }
    if (text_eq(label, "BS")) {
        backspace_expr();
        return;
    }
    if (text_eq(label, "=")) {
        evaluate_expr();
        return;
    }
    append_token(label);
}

static void draw_display(struct reliefos_ui_surface *ui)
{
    const char *shown_expr = expr_len ? expr : "0";
    const char *shown_result = result_text[0] ? result_text : "";
    uint32_t result_bg = error_state ? 0x00c0c0c0u : RELIEFOS_UI_WHITE;
    uint32_t result_fg = error_state ? 0x00000080u : RELIEFOS_UI_BLACK;

    reliefos_ui_inset(ui, DISPLAY_X, DISPLAY_Y, DISPLAY_W, DISPLAY_H, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, DISPLAY_X + 8, DISPLAY_Y + 10, shown_expr, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_rect(ui, DISPLAY_X + 6, DISPLAY_Y + 32, DISPLAY_W - 12, 1, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, DISPLAY_X + 8, DISPLAY_Y + 38, shown_result, result_fg, result_bg);
}

static void draw_calc(struct reliefos_ui_surface *ui, int pressed_index)
{
    int idx = 0;
    reliefos_ui_rect(ui, 0, 0, CALC_W, CALC_H, RELIEFOS_UI_LIGHT);
    reliefos_ui_panel(ui, 8, 8, CALC_W - 16, CALC_H - 16, RELIEFOS_UI_GRAY);
    draw_display(ui);
    for (int row = 0; row < GRID_ROWS; ++row) {
        for (int col = 0; col < GRID_COLS; ++col, ++idx) {
            const char *label = button_labels[row][col];
            int x = GRID_X + col * (BUTTON_W + BUTTON_GAP);
            int y = GRID_Y + row * (BUTTON_H + BUTTON_GAP);
            if (!label[0]) {
                continue;
            }
            reliefos_ui_button(ui, (uint32_t)x, (uint32_t)y, BUTTON_W, BUTTON_H, label,
                             idx == pressed_index ? RELIEFOS_UI_BUTTON_PRESSED : 0);
        }
    }
    reliefos_ui_text(ui, 16, CALC_H - 22, T("Integer calculator"), RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
}

static int hit_button(int x, int y)
{
    int idx = 0;
    for (int row = 0; row < GRID_ROWS; ++row) {
        for (int col = 0; col < GRID_COLS; ++col, ++idx) {
            const char *label = button_labels[row][col];
            int bx = GRID_X + col * (BUTTON_W + BUTTON_GAP);
            int by = GRID_Y + row * (BUTTON_H + BUTTON_GAP);
            if (!label[0]) {
                continue;
            }
            if (x >= bx && x < bx + BUTTON_W && y >= by && y < by + BUTTON_H) {
                return idx;
            }
        }
    }
    return -1;
}

static const char *button_label_by_index(int index)
{
    int idx = 0;
    for (int row = 0; row < GRID_ROWS; ++row) {
        for (int col = 0; col < GRID_COLS; ++col, ++idx) {
            if (idx == index) {
                return button_labels[row][col];
            }
        }
    }
    return "";
}

static int map_keycode(uint8_t keycode, uint8_t pressed, char *out)
{
    static uint8_t shift_down;
    if (keycode == RELIEFOS_KEY_LEFT_SHIFT || keycode == RELIEFOS_KEY_RIGHT_SHIFT) {
        shift_down = pressed ? 1 : 0;
        return 0;
    }
    if (keycode == RELIEFOS_KEY_CAPS_LOCK) {
        return 0;
    }
    if (!pressed) {
        return 0;
    }
    return reliefos_ui_keycode_to_char_shift(keycode, shift_down, out);
}

static void apply_key(char ch)
{
    if (ch == '\n') {
        evaluate_expr();
        return;
    }
    if (ch == '=') {
        evaluate_expr();
        return;
    }
    if (ch == '\b') {
        backspace_expr();
        return;
    }
    if (ch == '[') {
        append_token("(");
        return;
    }
    if (ch == ']') {
        append_token(")");
        return;
    }
    if (ch == ';') {
        clear_all();
        return;
    }
    if (ch == '\\') {
        apply_button("BS");
        return;
    }
    if (is_digit(ch) || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '(' || ch == ')') {
        char token[2];
        token[0] = ch;
        token[1] = 0;
        append_token(token);
    }
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    int pressed = -1;

    puts("[calc.elf] calculator starting");
    printf("[calc.elf] pid=%d creating Calculator window\n", getpid());
    window_id = reliefos_gui_create_app_window_ex(T("Calculator"), T("Integer calculator"),
                                                CALC_W, CALC_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[calc.elf] create window failed=%d\n", window_id);
        return 1;
    }

    clear_all();
    reliefos_ui_bind(&ui, pixels, CALC_W, CALC_H, CALC_W);
    draw_calc(&ui, pressed);
    reliefos_gui_present_window((uint32_t)window_id, CALC_W, CALC_H, CALC_W, pixels);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                break;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_MOVE) {
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON) {
                if (event.buttons & 1u) {
                    pressed = hit_button(event.x, event.y);
                } else if (pressed >= 0) {
                    int released = hit_button(event.x, event.y);
                    if (released == pressed) {
                        apply_button(button_label_by_index(pressed));
                    }
                    pressed = -1;
                }
                draw_calc(&ui, pressed);
                reliefos_gui_present_window((uint32_t)window_id, CALC_W, CALC_H, CALC_W, pixels);
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN || event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                char ch;
                if (map_keycode(event.keycode, event.pressed, &ch)) {
                    apply_key(ch);
                    draw_calc(&ui, pressed);
                    reliefos_gui_present_window((uint32_t)window_id, CALC_W, CALC_H, CALC_W, pixels);
                }
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE || event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                draw_calc(&ui, pressed);
                reliefos_gui_present_window((uint32_t)window_id, CALC_W, CALC_H, CALC_W, pixels);
            }
        } else {
            sleep_ms(10);
        }
    }
    return 0;
}
