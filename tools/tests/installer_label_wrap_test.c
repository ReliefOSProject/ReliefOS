/* Exercise the Installer's production line wrapper independently of FLTK. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../userland/apps/installer/installer_label_wrap.h"

struct metrics {
    double scale;
    unsigned calls;
    int allow_invalid;
};

/** @brief Decode one complete scalar; invalid or truncated input returns zero. */
static size_t scalar_bytes(const unsigned char *text, size_t remaining)
{
    if (!remaining) return 0;
    unsigned char first = text[0];
    if (first < 0x80) return 1;
    size_t count = first >= 0xc2 && first <= 0xdf ? 2 :
                   first >= 0xe0 && first <= 0xef ? 3 :
                   first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!count || count > remaining) return 0;
    for (size_t i = 1; i < count; ++i)
        if ((text[i] & 0xc0) != 0x80) return 0;
    if ((first == 0xe0 && text[1] < 0xa0) ||
        (first == 0xed && text[1] >= 0xa0) ||
        (first == 0xf0 && text[1] < 0x90) ||
        (first == 0xf4 && text[1] >= 0x90)) return 0;
    return count;
}

/** @brief Measure ASCII at one unit and complete CJK/emoji scalars at two. */
static double measure(const char *text, size_t length, void *context)
{
    struct metrics *metrics = context;
    assert(metrics && metrics->scale > 0);
    ++metrics->calls;
    double width = 0;
    for (size_t offset = 0; offset < length;) {
        size_t count = scalar_bytes((const unsigned char *)text + offset, length - offset);
        if (!count) {
            assert(metrics->allow_invalid && "measurement split a valid UTF-8 scalar");
            count = 1;
        }
        width += (count > 1 ? 2 : 1) * metrics->scale;
        offset += count;
    }
    return width;
}

/** @brief Count explicit output lines without losing a trailing empty line. */
static size_t line_count(const char *text)
{
    size_t count = 1;
    for (; *text; ++text) count += *text == '\n';
    return count;
}

/** @brief Require the byte content to survive, apart from wrap separators. */
static void assert_content(const char *original, const char *wrapped)
{
    for (;;) {
        while (*original == ' ' || *original == '\t' || *original == '\n') ++original;
        while (*wrapped == ' ' || *wrapped == '\t' || *wrapped == '\n') ++wrapped;
        assert((unsigned char)*original == (unsigned char)*wrapped);
        if (!*original) return;
        ++original;
        ++wrapped;
    }
}

/** @brief Check every rendered line; only a single too-wide glyph may overflow. */
static void assert_line_widths(const char *text, double limit, struct metrics *metrics)
{
    for (const char *line = text;;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        double width = measure(line, length, metrics);
        if (width > limit) {
            size_t one = scalar_bytes((const unsigned char *)line, length);
            if (!one && metrics->allow_invalid) one = 1;
            assert(one && length == one && "more than one glyph exceeds the line width");
        }
        if (!end) return;
        line = end + 1;
    }
}

/** @brief Verify visible line choices and ownership against a writable source. */
static void expect(const char *original, double width, double scale, const char *expected)
{
    size_t bytes = strlen(original) + 1;
    char *source = malloc(bytes);
    assert(source);
    memcpy(source, original, bytes);
    struct metrics metrics = {scale, 0, 0};
    char *result = installer_label_wrap(source, width, measure, &metrics);
    assert(result && result != source);
    if (strcmp(result, expected)) {
        fprintf(stderr, "wrap mismatch at width %.2f scale %.2f:\nexpected <%s>\nactual   <%s>\n",
                width, scale, expected, result);
        abort();
    }
    assert(!memcmp(source, original, bytes));
    assert_content(original, result);
    assert_line_widths(result, width, &metrics);
    free(result);
    free(source);
}

/** @brief Preserve words when they fit, then split only an oversized token. */
static void english_and_paths(void)
{
    expect("one two three", 7, 1, "one two\nthree");
    expect("alpha beta gamma", 10, 1, "alpha beta\ngamma");
    expect("ab cd", 4, 1, "ab\ncd");
    expect("ab cd", 5, 1, "ab cd");
    expect("one  two", 20, 1, "one  two");
    expect("one   two", 3, 1, "one\ntwo");
    expect("one\ttwo", 3, 1, "one\ntwo");
    expect("alpha betabetabetabeta", 8, 1, "alpha\nbetabeta\nbetabeta");
    expect("/target/lib/libreliefos.so.2", 8, 1,
           "/target/\nlib/libr\neliefos.\nso.2");
    expect("0123456789abcdef", 5, 1, "01234\n56789\nabcde\nf");
}

/** @brief Chinese, Japanese and four-byte scalars stay whole at line edges. */
static void unicode_and_newlines(void)
{
    expect("中文日本語😀", 4, 1, "中文\n日本\n語😀");
    expect("更新ではユーザーを保持します", 8, 1,
           "更新では\nユーザー\nを保持し\nます");
    expect("a😀b中c", 3, 1, "a😀\nb中\nc");
    expect("😀😀😀", 3, 1, "😀\n😀\n😀");
    expect("\nalpha\n\nbeta\n", 100, 1, "\nalpha\n\nbeta\n");
    expect("abc def\n日本語\n", 4, 1, "abc\ndef\n日本\n語\n");
    expect("\n\n", .5, 1, "\n\n");
    expect("a", .5, 1, "a");
    expect("ab", .5, 1, "a\nb");
    expect("中😀a", .5, 1, "中\n😀\na");
    expect("", 4, 1, "");
}

/** @brief Reflow original text for a changed widget width and font metrics. */
static void reflow_and_invalid_arguments(void)
{
    const char original[] = "中文日本語😀";
    struct metrics regular = {1, 0, 0}, large = {2, 0, 0};
    char *narrow = installer_label_wrap(original, 4, measure, &regular);
    char *wide = installer_label_wrap(original, 8, measure, &regular);
    char *larger_font = installer_label_wrap(original, 8, measure, &large);
    assert(narrow && wide && larger_font);
    assert(!strcmp(narrow, larger_font));
    assert(line_count(wide) < line_count(narrow));
    assert(regular.calls && large.calls);
    assert(!strcmp(original, "中文日本語😀"));
    free(narrow);
    free(wide);
    free(larger_font);

    char *empty = installer_label_wrap(NULL, 4, measure, &regular);
    assert(empty && !*empty);
    free(empty);
    assert(!installer_label_wrap("text", 0, measure, &regular));
    assert(!installer_label_wrap("text", -1, measure, &regular));
    assert(!installer_label_wrap("text", 4, NULL, &regular));
    assert(!installer_label_wrap(NULL, 0, measure, &regular));
    assert(!installer_label_wrap(NULL, 4, NULL, &regular));
}

/** @brief Malformed and truncated UTF-8 must make bounded byte-wise progress. */
static void malformed_bytes(void)
{
    const char *const inputs[] = {
        "\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xe4\xb8", "\xf0\x9f\x98",
        "中\x80" "a", "\xf4\x90\x80\x80", "\xed\xa0\x80",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        size_t length = strlen(inputs[i]);
        /* Exact allocation exposes lookahead beyond the final NUL to ASan. */
        char *original = malloc(length + 1);
        assert(original);
        memcpy(original, inputs[i], length + 1);
        struct metrics metrics = {1, 0, 1};
        char *wrapped = installer_label_wrap(original, 1, measure, &metrics);
        assert(wrapped && !memcmp(original, inputs[i], length + 1));
        assert_content(original, wrapped);
        assert_line_widths(wrapped, 1, &metrics);
        free(wrapped);
        free(original);
    }
}

/** @brief Different widths/fonts preserve complete text and bound every line. */
static void width_and_content_matrix(void)
{
    char long_path[4097];
    for (size_t i = 0; i < sizeof(long_path) - 1; ++i)
        long_path[i] = i % 23 == 0 ? '/' : (char)('a' + i % 26);
    long_path[sizeof(long_path) - 1] = 0;
    const char *const labels[] = {
        "Check the target and planned changes. Use Back to change choices.",
        "更新会保留用户、个人文件、已安装软件和本地配置。",
        "更新ではユーザー、個人ファイル、ソフトウェアを保持します。",
        "root=/dev/disk/by-partuuid/01234567-89ab-cdef-0123-456789abcdef",
        "Mixed 中文 text 😀 and 日本語\nwith an explicit final line\n", long_path,
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        for (unsigned width = 1; width <= 17; width += 2) {
            for (unsigned font = 2; font <= 4; ++font) {
                struct metrics metrics = {font / 2.0, 0, 0};
                char *wrapped = installer_label_wrap(labels[i], width, measure, &metrics);
                assert(wrapped && metrics.calls);
                assert_content(labels[i], wrapped);
                assert_line_widths(wrapped, width, &metrics);
                free(wrapped);
            }
        }
    }
}

/** @brief Run behavioral and sanitizer regressions against the real wrapper. */
int main(void)
{
    english_and_paths();
    unicode_and_newlines();
    reflow_and_invalid_arguments();
    malformed_bytes();
    width_and_content_matrix();
    puts("PASS Installer labels: word wrapping, CJK/emoji boundaries, reflow, explicit newlines and malformed input");
    return 0;
}
