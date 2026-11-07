#include "catalog.h"
#include "snippet.h"
#include "worker/font.h"
#include "worker/scene.h"

#include <assert.h>
#include <cairo.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr double kPointSize = 32.0;

const rm::Draw *glyph(const rm::Box &box, uint32_t codepoint, size_t occurrence = 0) {
    for (size_t i = 0; i < box.draws.size(); ++i) {
        const rm::Draw &draw = box.draws[i];
        if (draw.kind == rm::Draw::Kind::Glyph && draw.codepoint == codepoint) {
            if (occurrence == 0) return &draw;
            --occurrence;
        }
    }
    return nullptr;
}

size_t count_kind(const rm::Box &box, rm::Draw::Kind kind) {
    size_t count = 0;
    for (size_t i = 0; i < box.draws.size(); ++i)
        if (box.draws[i].kind == kind) ++count;
    return count;
}

double glyph_run_center(const rm::Box &box, uint32_t codepoint,
                        size_t first_occurrence, size_t occurrence_count) {
    double left = 0;
    double right = 0;
    size_t seen = 0;
    size_t selected = 0;
    for (size_t i = 0; i < box.draws.size(); ++i) {
        const rm::Draw &draw = box.draws[i];
        if (draw.kind != rm::Draw::Kind::Glyph || draw.codepoint != codepoint) continue;
        if (seen++ < first_occurrence) continue;
        if (!selected) left = draw.x;
        if (draw.x + draw.width > right || !selected) right = draw.x + draw.width;
        if (++selected == occurrence_count) break;
    }
    assert(selected == occurrence_count);
    return (left + right) * 0.5;
}

bool parse(rm::FontBook &fonts, const char *source, rm::Box &box, rm::Error &error) {
    return rm::parse_latex(source, strlen(source), kPointSize, true, fonts, box, error);
}

void expect_parse(rm::FontBook &fonts, const char *source) {
    rm::Box box;
    rm::Error error;
    if (!parse(fonts, source, box, error)) {
        fprintf(stderr, "formula rejected at [%zu,%zu), code=%u: %s\n",
                error.begin, error.end, static_cast<unsigned>(error.code),
                error.message.c_str());
        fprintf(stderr, "source: %s\n", source);
        assert(false);
    }
    assert(isfinite(box.width) && box.width >= 0);
    assert(isfinite(box.ascent) && box.ascent >= 0);
    assert(isfinite(box.descent) && box.descent >= 0);
    assert(box.draws.size() != 0);
}

void expect_unsupported(rm::FontBook &fonts, const char *source, size_t begin,
                        size_t end) {
    rm::Box box;
    rm::Error error;
    assert(!parse(fonts, source, box, error));
    assert(error.code == rm::Code::Unsupported);
    assert(error.begin == begin && error.end == end);
    assert(error.end > error.begin && error.end <= strlen(source));
}

void test_compound_formula_families(rm::FontBook &fonts) {
    const char *formulas[] = {
        "\\sum_{k=1}^{n}\\dfrac{k}{n}",
        "\\lim_{x\\to0}\\frac{\\sin x}{x}",
        "\\cos x+\\tan x+\\log x+\\ln x+\\exp x+\\min(a,b)+\\max(a,b)",
        "\\int_0^1 x\\,dx+\\iint_D f\\,dx\\,dy+\\iiint_V g\\,dV+\\oint_C h\\,ds",
        "\\sqrt[3]{x_1^2}+\\tfrac{a}{b}",
        "\\left(\\dfrac{a_1}{b^2}\\right)+\\left|x\\right|",
        "\\begin{matrix}a & b \\\\ c & d\\end{matrix}",
        "\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}",
        "\\begin{bmatrix}a & b \\\\ c & d\\end{bmatrix}",
        "\\begin{vmatrix}a & b \\\\ c & d\\end{vmatrix}",
        "\\begin{Vmatrix}a & b \\\\ c & d\\end{Vmatrix}",
        "\\begin{cases}x & x>0 \\\\ -x & x\\le0\\end{cases}",
        "\\begin{aligned}a &= b+c \\\\ d &= e+f\\end{aligned}",
        "\\begin{gathered}a=b \\\\ c=d\\end{gathered}",
        "\\text{中文公式}+\\mathrm{d}x+\\mathbf{v}+\\mathit{x}"
        "+\\mathbb{R}+\\mathcal{F}",
        "\\vec{x}+\\hat{y}+\\bar{z}+\\overline{AB}+\\underline{CD}",
    };
    for (size_t i = 0; i < sizeof(formulas) / sizeof(formulas[0]); ++i)
        expect_parse(fonts, formulas[i]);
}

void test_fraction_has_rule_and_separates_numerator_denominator(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\dfrac{a}{b}", box, error));
    assert(count_kind(box, rm::Draw::Kind::Rule) == 1);
    const rm::Draw *numerator = glyph(box, 'a');
    const rm::Draw *denominator = glyph(box, 'b');
    assert(numerator && denominator);
    const rm::Draw *rule = nullptr;
    for (size_t i = 0; i < box.draws.size(); ++i)
        if (box.draws[i].kind == rm::Draw::Kind::Rule) rule = &box.draws[i];
    assert(rule);
    assert(numerator->y < rule->y);
    assert(denominator->y > rule->y + rule->height);
    assert(rule->width > 0 && rule->height > 0);
}

void test_subscript_and_superscript_move_opposite_the_baseline(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "x_2^3", box, error));
    const rm::Draw *base = glyph(box, 'x');
    const rm::Draw *sub = glyph(box, '2');
    const rm::Draw *sup = glyph(box, '3');
    assert(base && sub && sup);
    assert(sup->y < base->y);
    assert(sub->y > base->y);
}

void test_large_operator_limits_follow_display_style(rm::FontBook &fonts) {
    struct OperatorCase { const char *source; uint32_t codepoint; };
    const OperatorCase cases[] = {
        {"\\sum_{k}^{n}", 0x2211}, {"\\prod_{k}^{n}", 0x220f},
        {"\\int_{k}^{n}", 0x222b}, {"\\iint_{k}^{n}", 0x222c},
        {"\\iiint_{k}^{n}", 0x222d}, {"\\oint_{k}^{n}", 0x222e},
        {"\\lim_{k}^{n}", 'l'},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rm::Box box;
        rm::Error error;
        assert(parse(fonts, cases[i].source, box, error));
        const rm::Draw *operation = glyph(box, cases[i].codepoint);
        const rm::Draw *lower = glyph(box, 'k');
        const rm::Draw *upper = glyph(box, 'n');
        assert(operation && lower && upper);
        assert(upper->y < operation->y && lower->y > operation->y);
    }

    const char *source = "\\sum_{k}^{n}";
    rm::Box text_style;
    rm::Error error;
    assert(rm::parse_latex(source, strlen(source), kPointSize, false,
                           fonts, text_style, error));
    const rm::Draw *operation = glyph(text_style, 0x2211);
    const rm::Draw *lower = glyph(text_style, 'k');
    const rm::Draw *upper = glyph(text_style, 'n');
    assert(operation && lower && upper);
    assert(lower->x > operation->x && upper->x > operation->x);
}

void test_root_bar_covers_the_radicand(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\sqrt{x^2}", box, error));
    const rm::Draw *root = glyph(box, 0x221a);
    const rm::Draw *radicand = glyph(box, 'x');
    assert(root && radicand);
    bool covering_bar = false;
    for (size_t i = 0; i < box.draws.size(); ++i) {
        const rm::Draw &draw = box.draws[i];
        if (draw.kind == rm::Draw::Kind::Rule && draw.x <= radicand->x &&
            draw.x + draw.width > radicand->x &&
            draw.y + draw.height < radicand->y) {
            covering_bar = true;
        }
    }
    assert(covering_bar);
}

void test_matrix_columns_and_rows_align(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\begin{matrix}a & bb \\\\ cc & d\\end{matrix}", box, error));
    const rm::Draw *a = glyph(box, 'a');
    const rm::Draw *b = glyph(box, 'b');
    const rm::Draw *c = glyph(box, 'c');
    const rm::Draw *d = glyph(box, 'd');
    assert(a && b && c && d);
    // Matrix cells are centered within their columns, so compare the centers
    // of differently sized entries rather than requiring equal left edges.
    assert(fabs(glyph_run_center(box, 'a', 0, 1) -
                glyph_run_center(box, 'c', 0, 2)) < 0.01);
    assert(fabs(glyph_run_center(box, 'b', 0, 2) -
                glyph_run_center(box, 'd', 0, 1)) < 0.01);
    assert(c->y > a->y);
}

size_t write_matrix(char *source, size_t rows, size_t columns) {
    const char *opening = "\\begin{matrix}";
    const char *closing = "\\end{matrix}";
    size_t length = strlen(opening);
    memcpy(source, opening, length);
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            source[length++] = 'x';
            if (column + 1 < columns) source[length++] = '&';
        }
        if (row + 1 < rows) {
            source[length++] = '\\';
            source[length++] = '\\';
        }
    }
    const size_t closing_length = strlen(closing);
    memcpy(source + length, closing, closing_length);
    length += closing_length;
    source[length] = '\0';
    return length;
}

void test_matrix_dimension_bounds(rm::FontBook &fonts) {
    char source[2048];
    rm::Box box;
    rm::Error error;
    size_t length = write_matrix(source, 16, 16);
    assert(parse(fonts, source, box, error));
    assert(box.draws.size() == 256);

    length = write_matrix(source, 17, 1);
    assert(!rm::parse_latex(source, length, kPointSize, true, fonts, box, error));
    assert(error.code == rm::Code::Limit);
    assert(error.begin < error.end && error.end <= length);

    length = write_matrix(source, 1, 17);
    assert(!rm::parse_latex(source, length, kPointSize, true, fonts, box, error));
    assert(error.code == rm::Code::Limit);
    assert(error.begin < error.end && error.end <= length);
}

void test_aligned_environment_aligns_relation_columns(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\begin{aligned}a &= b \\\\ c &= d\\end{aligned}", box, error));
    const rm::Draw *first = glyph(box, '=');
    const rm::Draw *second = glyph(box, '=', 1);
    assert(first && second);
    assert(fabs(first->x - second->x) < 0.01);
    assert(second->y > first->y);
}

void test_left_right_delimiters_stretch_to_the_content(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\left(\\dfrac{a}{b}\\right)", box, error));
    const rm::Draw *left = glyph(box, '(');
    const rm::Draw *right = glyph(box, ')');
    const rm::Draw *numerator = glyph(box, 'a');
    const rm::Draw *denominator = glyph(box, 'b');
    assert(left && right && numerator && denominator);
    assert(left->yscale > 1.0 && right->yscale > 1.0);
    assert(left->x < numerator->x);
    assert(right->x > denominator->x);

    rm::Box angle;
    assert(parse(fonts, "\\left<x\\right>", angle, error));
    assert(glyph(angle, '<') && glyph(angle, '>'));

    rm::Box plain;
    rm::Box invisible;
    assert(parse(fonts, "x", plain, error));
    assert(parse(fonts, "\\left.x\\right.", invisible, error));
    assert(fabs(plain.width - invisible.width) < 0.01);
}

void test_text_keeps_chinese_codepoints_and_text_face(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(parse(fonts, "\\text{中文}", box, error));
    const rm::Draw *zhong = glyph(box, 0x4e2d);
    const rm::Draw *wen = glyph(box, 0x6587);
    assert(zhong && wen);
    assert(zhong->face == rm::Face::Text && wen->face == rm::Face::Text);
    assert(wen->x > zhong->x);
    assert(zhong->width > 0 && wen->width > 0);

    cairo_surface_t *surface = cairo_recording_surface_create(CAIRO_CONTENT_ALPHA, nullptr);
    assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    cairo_t *context = cairo_create(surface);
    assert(cairo_status(context) == CAIRO_STATUS_SUCCESS);
    assert(fonts.outline(zhong->codepoint, zhong->size, zhong->face, context, error));
    cairo_path_t *outline = cairo_copy_path(context);
    assert(outline && outline->status == CAIRO_STATUS_SUCCESS && outline->num_data > 0);
    cairo_path_destroy(outline);
    cairo_destroy(context);
    cairo_surface_destroy(surface);
}

void test_catalog_generated_sources_are_parser_supported(rm::FontBook &fonts) {
    size_t count = 0;
    const rm::ToolEntry *entries = rm::catalog_entries(&count);
    assert(entries && count > 0);
    rm::ToolArgs args{};
    rm::Error error;
    for (size_t i = 0; i < count; ++i) {
        rm::Snippet snippet;
        assert(rm::build_snippet(entries[i].tool, args, snippet, error));
        rm::Box box;
        if (!rm::parse_latex(snippet.source.c_str(), snippet.source.size(),
                             kPointSize, true, fonts, box, error)) {
            fprintf(stderr, "catalog source rejected: %s (%s), [%zu,%zu): %s\n",
                    entries[i].key, entries[i].insert, error.begin, error.end,
                    error.message.c_str());
            assert(false);
        }
    }
}

void test_formula_fixture_parses_every_supported_sample(rm::FontBook &fonts) {
    FILE *file = fopen("tests/fixtures/reliefmath/formulas.tsv", "rb");
    assert(file);
    char line[4096];
    size_t rows = 0;
    while (fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        assert(tab);
        *tab++ = '\0';
        size_t length = strlen(tab);
        while (length && (tab[length - 1] == '\n' || tab[length - 1] == '\r'))
            tab[--length] = '\0';
        rm::Box box;
        rm::Error error;
        if (!rm::parse_latex(tab, length, kPointSize, true, fonts, box, error)) {
            fprintf(stderr, "fixture rejected: %s at [%zu,%zu): %s\n",
                    line, error.begin, error.end, error.message.c_str());
            assert(false);
        }
        ++rows;
    }
    assert(fclose(file) == 0);
    assert(rows >= 20);
}

void test_unsafe_or_unknown_macros_are_rejected_at_the_command(rm::FontBook &fonts) {
    expect_unsupported(fonts, "\\newcommand{\\foo}{x}", 0, 11);
    expect_unsupported(fonts, "\\def\\foo{x}", 0, 4);
    expect_unsupported(fonts, "\\usepackage{amsmath}", 0, 11);
    expect_unsupported(fonts, "\\input{private.tex}", 0, 6);
    expect_unsupported(fonts, "\\unknown{x}", 0, 8);
    expect_unsupported(fonts, "\\begin{tikzpicture}x\\end{tikzpicture}", 7, 18);
}

void test_malformed_group_reports_a_source_interval(rm::FontBook &fonts) {
    const char source[] = "\\frac{a}{b";
    rm::Box box;
    rm::Error error;
    assert(!parse(fonts, source, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 8 && error.end == 9);
}

void test_unclosed_layout_constructs_report_openers(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    const char *delimiter = "\\left(x";
    assert(!parse(fonts, delimiter, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 0 && error.end == 5);

    const char *missing_left = "\\left";
    assert(!parse(fonts, missing_left, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 0 && error.end == 5);

    const char *missing_right = "\\left(x\\right";
    assert(!parse(fonts, missing_right, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 7 && error.end == 13);

    const char *root_index = "\\sqrt[3{x}";
    assert(!parse(fonts, root_index, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 5 && error.end == 6);

    const char *environment = "\\begin{matrix}x";
    assert(!parse(fonts, environment, box, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 7 && error.end == 13);
}

void test_input_size_and_nesting_limits(rm::FontBook &fonts) {
    rm::Box box;
    rm::Error error;
    assert(rm::parse_latex("x", 1, 512, false, fonts, box, error));
    assert(rm::parse_latex("x", 1, 512.01, false, fonts, box, error));

    char *source = static_cast<char *>(malloc(rm::kMaxSourceBytes + 1));
    assert(source);
    memset(source, 'x', rm::kMaxSourceBytes);
    source[rm::kMaxSourceBytes] = '\0';

    assert(rm::parse_latex(source, rm::kMaxSourceBytes, kPointSize, false,
                           fonts, box, error));
    assert(box.draws.size() == rm::kMaxSourceBytes);
    assert(!rm::parse_latex(source, rm::kMaxSourceBytes + 1, kPointSize, false,
                            fonts, box, error));
    assert(error.code == rm::Code::Limit);
    assert(error.begin == rm::kMaxSourceBytes);
    assert(error.end == rm::kMaxSourceBytes + 1);
    free(source);

    for (size_t depth = 128; depth <= 129; ++depth) {
        const size_t length = depth * 2 + 1;
        source = static_cast<char *>(malloc(length + 1));
        assert(source);
        memset(source, '{', depth);
        source[depth] = 'x';
        memset(source + depth + 1, '}', depth);
        source[length] = '\0';
        const bool parsed = rm::parse_latex(source, length, kPointSize, false,
                                            fonts, box, error);
        if (depth == 128) {
            assert(parsed);
        } else {
            assert(!parsed && error.code == rm::Code::Limit);
            assert(error.begin == 128 && error.end == 129);
        }
        free(source);
    }

    const char *root = "\\sqrt";
    for (size_t depth = 128; depth <= 129; ++depth) {
        const size_t root_length = strlen(root);
        const size_t length = depth * root_length + 1;
        source = static_cast<char *>(malloc(length + 1));
        assert(source);
        for (size_t i = 0; i < depth; ++i)
            memcpy(source + i * root_length, root, root_length);
        // A digit terminates the control word without consuming the next
        // letter into a longer command name (for example, `\\sqrtx`).
        source[length - 1] = '2';
        source[length] = '\0';
        const bool parsed = rm::parse_latex(source, length, kPointSize, false,
                                            fonts, box, error);
        if (depth == 128 && !parsed)
            fprintf(stderr, "128 nested roots rejected: code=%u range=[%zu,%zu): %s\n",
                    static_cast<unsigned>(error.code), error.begin, error.end,
                    error.message.c_str());
        if (depth == 128) assert(parsed);
        else assert(!parsed && error.code == rm::Code::Limit);
        free(source);
    }

    const char *left = "\\left(";
    const char *right = "\\right)";
    for (size_t depth = 128; depth <= 129; ++depth) {
        const size_t left_length = strlen(left);
        const size_t right_length = strlen(right);
        const size_t length = depth * (left_length + right_length) + 1;
        source = static_cast<char *>(malloc(length + 1));
        assert(source);
        for (size_t i = 0; i < depth; ++i)
            memcpy(source + i * left_length, left, left_length);
        source[depth * left_length] = 'x';
        for (size_t i = 0; i < depth; ++i)
            memcpy(source + depth * left_length + 1 + i * right_length,
                   right, right_length);
        source[length] = '\0';
        const bool parsed = rm::parse_latex(source, length, kPointSize, false,
                                            fonts, box, error);
        if (depth == 128) assert(parsed);
        else assert(!parsed && error.code == rm::Code::Limit);
        free(source);
    }
}

void test_effective_size_is_not_a_point_size_setting(rm::FontBook &fonts) {
    const double sizes[] = {512.0 * 96 / 72, 512.0 * 2400 / 72 * 64};
    for (double size : sizes) {
        rm::Box box;
        rm::Error error;
        assert(rm::parse_latex("x", 1, size, false, fonts, box, error));
        assert(isfinite(box.width) && box.width > 0);
        assert(box.draws.size() == 1);
        assert(box.draws[0].size * box.draws[0].xscale > 512);
    }
    const double invalid[] = {0, -1, INFINITY, NAN};
    for (double size : invalid) {
        rm::Box box;
        rm::Error error;
        assert(!rm::parse_latex("x", 1, size, false, fonts, box, error));
        assert(error.code == rm::Code::Limit);
    }
}

void test_synthetic_glyph_failures_keep_the_command_span() {
    struct Failure { const char *source; size_t begin; size_t end; };
    const Failure cases[] = {
        {"  \\sqrt{}", 2, 7}, {"  \\hat{}", 2, 6}, {"  \\vec{}", 2, 6},
        {"  \\left(\\right)", 2, 7}, {"  \\left.\\right)", 8, 14},
        {"  \\begin{pmatrix}\\end{pmatrix}", 2, 17},
    };
    rm::FontBook unopened;
    for (const Failure &failure : cases) {
        rm::Box box;
        rm::Error error;
        assert(!parse(unopened, failure.source, box, error));
        assert(error.code == rm::Code::Font);
        if (error.begin != failure.begin || error.end != failure.end)
            fprintf(stderr, "synthetic glyph lost span: %s [%zu,%zu), expected [%zu,%zu)\n",
                    failure.source, error.begin, error.end, failure.begin, failure.end);
        assert(error.begin == failure.begin && error.end == failure.end);
    }
}

void test_invalid_utf8_reports_the_bad_byte(rm::FontBook &fonts) {
    const char source[] = {'x', static_cast<char>(0xc0), static_cast<char>(0xaf)};
    rm::Box box;
    rm::Error error;
    assert(!rm::parse_latex(source, sizeof(source), kPointSize, false,
                            fonts, box, error));
    assert(error.code == rm::Code::InvalidUtf8);
    assert(error.begin == 1 && error.end == 2);
}

} // namespace

int main(int argc, char **argv) {
    assert(argc == 3);
    rm::FontBook fonts;
    rm::Error error;
    assert(fonts.open(argv[1], argv[2], error));

    test_compound_formula_families(fonts);
    test_fraction_has_rule_and_separates_numerator_denominator(fonts);
    test_subscript_and_superscript_move_opposite_the_baseline(fonts);
    test_large_operator_limits_follow_display_style(fonts);
    test_root_bar_covers_the_radicand(fonts);
    test_matrix_columns_and_rows_align(fonts);
    test_matrix_dimension_bounds(fonts);
    test_aligned_environment_aligns_relation_columns(fonts);
    test_left_right_delimiters_stretch_to_the_content(fonts);
    test_text_keeps_chinese_codepoints_and_text_face(fonts);
    test_catalog_generated_sources_are_parser_supported(fonts);
    test_formula_fixture_parses_every_supported_sample(fonts);
    test_unsafe_or_unknown_macros_are_rejected_at_the_command(fonts);
    test_malformed_group_reports_a_source_interval(fonts);
    test_unclosed_layout_constructs_report_openers(fonts);
    test_synthetic_glyph_failures_keep_the_command_span();
    test_effective_size_is_not_a_point_size_setting(fonts);
    test_input_size_and_nesting_limits(fonts);
    test_invalid_utf8_reports_the_bad_byte(fonts);
    puts("ReliefMath parser and box layout tests passed");
    return 0;
}
