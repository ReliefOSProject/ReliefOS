#include "boxes.h"

#include <math.h>
#include <string.h>

namespace rm {
namespace {

bool finite_box(const Box &box) {
    return isfinite(box.width) && isfinite(box.ascent) && isfinite(box.descent) &&
           box.width >= 0 && box.ascent >= 0 && box.descent >= 0;
}

bool fail(Error &error, Code code, const char *message) {
    error.set(code, message);
    return false;
}

bool place_rule(Box &out, double x, double top, double width, double thickness,
                Error &error) {
    Box line;
    if (!rule_box(width, thickness, line, error)) return false;
    return box_place(out, line, x, top + thickness, error);
}

} // namespace

bool box_make_glyph(FontBook &fonts, uint32_t codepoint, double size, Face face,
                    Box &out, Error &error) {
    Box candidate;
    if (!glyph_box(fonts, codepoint, size, face, candidate, error)) return false;
    if (candidate.draws.size()) {
        Draw &draw = candidate.draws[0];
        draw.width = candidate.width;
        draw.height = candidate.ascent + candidate.descent;
    }
    if (!finite_box(candidate)) return fail(error, Code::Limit, "Glyph metrics exceed layout limits");
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_make_spacer(double width, Box &out, Error &error) {
    if (!isfinite(width) || width < 0)
        return fail(error, Code::Limit, "Invalid horizontal spacing");
    Box candidate;
    candidate.width = width;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_place(Box &out, const Box &child, double dx, double dy, Error &error) {
    if (!finite_box(child) || !isfinite(dx) || !isfinite(dy))
        return fail(error, Code::Limit, "Invalid box placement");
    const double right = dx + child.width;
    const double ascent = child.ascent - dy;
    const double descent = child.descent + dy;
    if (!isfinite(right) || !isfinite(ascent) || !isfinite(descent))
        return fail(error, Code::Limit, "Formula geometry overflow");
    if (!out.append(child, dx, dy, error)) {
        if (error.code == Code::None) error.set(Code::NoMemory, "Cannot append formula box");
        return false;
    }
    if (right > out.width) out.width = right;
    if (ascent > out.ascent) out.ascent = ascent;
    if (descent > out.descent) out.descent = descent;
    return finite_box(out) || fail(error, Code::Limit, "Formula geometry overflow");
}

bool box_fraction(const Box &numerator, const Box &denominator, double size,
                  Box &out, Error &error) {
    if (!finite_box(numerator) || !finite_box(denominator) ||
        !isfinite(size) || size <= 0)
        return fail(error, Code::Limit, "Invalid fraction geometry");
    const double thickness = size * 0.045;
    const double gap = size * 0.14;
    const double width = (numerator.width > denominator.width
                          ? numerator.width : denominator.width) + size * 0.28;
    if (!isfinite(thickness) || !isfinite(gap) || !isfinite(width))
        return fail(error, Code::Limit, "Fraction geometry overflow");

    Box candidate;
    const double numerator_x = (width - numerator.width) * 0.5;
    const double denominator_x = (width - denominator.width) * 0.5;
    const double numerator_y = -(thickness * 0.5 + gap + numerator.descent);
    const double denominator_y = thickness * 0.5 + gap + denominator.ascent;
    if (!box_place(candidate, numerator, numerator_x, numerator_y, error) ||
        !place_rule(candidate, 0, -thickness * 0.5, width, thickness, error) ||
        !box_place(candidate, denominator, denominator_x, denominator_y, error))
        return false;
    candidate.width = width;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_scale(Box &box, double scale, Error &error) {
    if (!finite_box(box) || !isfinite(scale) || scale <= 0)
        return fail(error, Code::Limit, "Invalid formula scale");
    box.width *= scale;
    box.ascent *= scale;
    box.descent *= scale;
    if (!finite_box(box)) return fail(error, Code::Limit, "Formula geometry overflow");
    for (size_t i = 0; i < box.draws.size(); ++i) {
        Draw &draw = box.draws[i];
        draw.x *= scale;
        draw.y *= scale;
        draw.width *= scale;
        draw.height *= scale;
        if (draw.kind == Draw::Kind::Glyph) {
            draw.xscale *= scale;
            draw.yscale *= scale;
        }
        if (!isfinite(draw.x) || !isfinite(draw.y) || !isfinite(draw.width) ||
            !isfinite(draw.height) || !isfinite(draw.xscale) || !isfinite(draw.yscale))
            return fail(error, Code::Limit, "Formula geometry overflow");
    }
    return true;
}

bool box_scripts(const Box &base, const Box *sub, const Box *sup, double size,
                 bool vertical_limits, Box &out, Error &error) {
    if (!finite_box(base) || (sub && !finite_box(*sub)) || (sup && !finite_box(*sup)) ||
        !isfinite(size) || size <= 0)
        return fail(error, Code::Limit, "Invalid script geometry");
    Box candidate;
    const double gap = size * 0.08;
    if (vertical_limits) {
        double width = base.width;
        if (sub && sub->width > width) width = sub->width;
        if (sup && sup->width > width) width = sup->width;
        const double base_x = (width - base.width) * 0.5;
        const double sub_x = sub ? (width - sub->width) * 0.5 : 0;
        const double sup_x = sup ? (width - sup->width) * 0.5 : 0;
        if (!box_place(candidate, base, base_x, 0, error))
            return false;
        if (sup) {
            const double dy = -base.ascent - gap - sup->descent;
            if (!box_place(candidate, *sup, sup_x, dy, error)) return false;
        }
        if (sub) {
            const double dy = base.descent + gap + sub->ascent;
            if (!box_place(candidate, *sub, sub_x, dy, error)) return false;
        }
        if (sup && sup->width > candidate.width) candidate.width = sup->width;
        if (sub && sub->width > candidate.width) candidate.width = sub->width;
    } else {
        if (!box_place(candidate, base, 0, 0, error)) return false;
        const double x = base.width + size * 0.06;
        if (sup) {
            double dy = -((base.ascent * 0.62) + (sup->descent * 0.35));
            if (dy > -size * 0.52) dy = -size * 0.52;
            if (sub) dy -= gap;
            if (!box_place(candidate, *sup, x, dy, error)) return false;
        }
        if (sub) {
            double dy = (base.descent * 0.62) + (sub->ascent * 0.3);
            if (dy < size * 0.3) dy = size * 0.3;
            if (sup) dy += gap;
            if (!box_place(candidate, *sub, x, dy, error)) return false;
        }
    }
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_radical(FontBook &fonts, const Box &radicand, const Box *index, double size,
                 Box &out, Error &error) {
    if (!finite_box(radicand) || (index && !finite_box(*index)) ||
        !isfinite(size) || size <= 0)
        return fail(error, Code::Limit, "Invalid radical geometry");
    const double thickness = size * 0.045;
    const double gap = size * 0.08;
    Box radical;
    if (!box_make_glyph(fonts, 0x221a, size, Face::Math, radical, error)) return false;
    if (radical.draws.empty() || radical.ascent + radical.descent <= 0)
        return fail(error, Code::Font, "Radical glyph has no measurable extent");

    const double index_x = index ? index->width * 0.7 : 0;
    const double radical_x = index_x;
    const double radicand_x = radical_x + radical.width * 0.82;
    const double rule_top = -radicand.ascent - gap - thickness;
    const double target_height = radicand.ascent + radicand.descent + gap + thickness;
    double scale = target_height / (radical.ascent + radical.descent);
    if (scale < 1.0) scale = 1.0;
    const double root_y = rule_top + radical.ascent * scale;
    if (!isfinite(radicand_x) || !isfinite(rule_top) || !isfinite(scale) ||
        !isfinite(root_y))
        return fail(error, Code::Limit, "Radical geometry overflow");

    Box candidate;
    Draw &radical_draw = radical.draws[0];
    radical_draw.x = radical_x;
    radical_draw.y = root_y;
    radical_draw.yscale = scale;
    radical_draw.height = (radical.ascent + radical.descent) * scale;
    if (!candidate.draws.push(radical_draw))
        return fail(error, Code::NoMemory, "Cannot allocate radical glyph");
    candidate.width = radical_x + radical.width;

    if (index) {
        if (!box_place(candidate, *index, 0,
                       -radicand.ascent * 0.55 - index->descent,
                       error))
            return false;
    }
    if (!box_place(candidate, radicand, radicand_x, 0, error) ||
        !place_rule(candidate, radicand_x, rule_top, radicand.width + gap, thickness,
                    error))
        return false;
    const double full_width = radicand_x + radicand.width + gap;
    if (!isfinite(full_width)) return fail(error, Code::Limit, "Radical width overflow");
    candidate.width = full_width;
    if (candidate.ascent < -rule_top) candidate.ascent = -rule_top;
    if (candidate.descent < radicand.descent) candidate.descent = radicand.descent;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_delimiter(FontBook &fonts, uint32_t codepoint, double size,
                   double ascent, double descent, Box &out, Error &error) {
    if (!isfinite(size) || size <= 0 || !isfinite(ascent) || !isfinite(descent) ||
        ascent < 0 || descent < 0)
        return fail(error, Code::Limit, "Invalid delimiter geometry");
    Box candidate;
    candidate.ascent = ascent;
    candidate.descent = descent;
    if (!codepoint) {
        out = static_cast<Box &&>(candidate);
        return true;
    }
    Box base;
    if (!box_make_glyph(fonts, codepoint, size, Face::Math, base, error)) return false;
    const double base_height = base.ascent + base.descent;
    const double target_height = ascent + descent;
    double scale = 1.0;
    if (base_height > 0 && target_height > base_height)
        scale = target_height / base_height;
    Draw &draw = base.draws[0];
    draw.yscale = scale;
    draw.height = base_height * scale;
    draw.y = -ascent + base.ascent * scale;
    candidate.width = base.width;
    if (!candidate.draws.push(draw))
        return fail(error, Code::NoMemory, "Cannot allocate delimiter glyph");
    if (!finite_box(candidate)) return fail(error, Code::Limit, "Delimiter geometry overflow");
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_accent(FontBook &fonts, const Box &base, uint32_t codepoint, bool rule,
                bool underline, double size, Box &out, Error &error) {
    if (!finite_box(base) || !isfinite(size) || size <= 0)
        return fail(error, Code::Limit, "Invalid accent geometry");
    Box candidate;
    if (!box_place(candidate, base, 0, 0, error)) return false;
    const double gap = size * 0.05;
    if (rule) {
        const double thickness = size * 0.045;
        const double top = underline ? base.descent + gap
                                     : -base.ascent - gap - thickness;
        if (!place_rule(candidate, 0, top, base.width, thickness, error)) return false;
    } else {
        Box mark;
        if (!box_make_glyph(fonts, codepoint, size * 0.72, Face::Math, mark, error))
            return false;
        if (mark.draws.empty()) return fail(error, Code::Font, "Accent glyph has no outline");
        Draw draw = mark.draws[0];
        double xscale = mark.width > 0 && base.width > mark.width
                            ? base.width / mark.width : 1.0;
        draw.xscale = xscale;
        draw.width *= xscale;
        draw.x = (base.width - draw.width) * 0.5;
        draw.y = -base.ascent - gap - mark.descent;
        if (!candidate.draws.push(draw))
            return fail(error, Code::NoMemory, "Cannot allocate accent glyph");
        if (draw.y - mark.ascent < 0) candidate.ascent = -draw.y + mark.ascent;
    }
    out = static_cast<Box &&>(candidate);
    return true;
}

bool box_grid(const Box *cells, size_t rows, size_t columns, GridLayout layout,
              double size, Box &out, Error &error) {
    if (!cells || !rows || !columns || rows > 16 || columns > 16 ||
        rows > SIZE_MAX / columns || !isfinite(size) || size <= 0)
        return fail(error, Code::Limit, "Matrix dimensions must be between 1 and 16");
    double widths[16] = {};
    double ascents[16] = {};
    double descents[16] = {};
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            const Box &cell = cells[row * columns + column];
            if (!finite_box(cell)) return fail(error, Code::Limit, "Invalid matrix cell geometry");
            if (cell.width > widths[column]) widths[column] = cell.width;
            if (cell.ascent > ascents[row]) ascents[row] = cell.ascent;
            if (cell.descent > descents[row]) descents[row] = cell.descent;
        }
        if (ascents[row] < size * 0.58) ascents[row] = size * 0.58;
        if (descents[row] < size * 0.22) descents[row] = size * 0.22;
    }

    const double column_gap = size * 0.72;
    const double row_gap = size * 0.34;
    double total_width = 0;
    for (size_t column = 0; column < columns; ++column) {
        total_width += widths[column];
        if (column) total_width += column_gap;
    }
    double total_height = 0;
    for (size_t row = 0; row < rows; ++row) {
        total_height += ascents[row] + descents[row];
        if (row) total_height += row_gap;
    }
    if (!isfinite(total_width) || !isfinite(total_height))
        return fail(error, Code::Limit, "Matrix geometry overflow");

    Box candidate;
    double baseline = -total_height * 0.5 + ascents[0];
    double row_top = -total_height * 0.5;
    for (size_t row = 0; row < rows; ++row) {
        if (row) baseline = row_top + ascents[row];
        double x = 0;
        if (layout == GridLayout::Gathered)
            x = (total_width - widths[0]) * 0.5;
        for (size_t column = 0; column < columns; ++column) {
            const Box &cell = cells[row * columns + column];
            double cell_x = x;
            if (layout == GridLayout::Centered)
                cell_x += (widths[column] - cell.width) * 0.5;
            else if (layout == GridLayout::Aligned && (column % 2) == 0)
                cell_x += widths[column] - cell.width;
            if (!box_place(candidate, cell, cell_x, baseline, error)) return false;
            if (layout == GridLayout::Gathered) break;
            x += widths[column] + column_gap;
        }
        row_top = baseline + descents[row] + row_gap;
    }
    candidate.width = total_width;
    candidate.ascent = total_height * 0.5;
    candidate.descent = total_height * 0.5;
    if (!finite_box(candidate)) return fail(error, Code::Limit, "Matrix geometry overflow");
    out = static_cast<Box &&>(candidate);
    return true;
}

} // namespace rm
