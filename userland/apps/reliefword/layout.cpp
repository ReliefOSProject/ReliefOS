#include "layout.h"
#include <math.h>
#include <string.h>

namespace rw {

namespace {

constexpr unsigned MaxLayoutPages = 4096;
constexpr float Epsilon = 0.0001f;

bool fail(Text &error, const char *message) {
    error.set(message);
    return false;
}

bool valid_metrics(const Metrics &metrics) {
    return isfinite(metrics.advance) && isfinite(metrics.ascent) &&
           isfinite(metrics.descent) && metrics.advance >= 0 &&
           metrics.ascent >= 0 && metrics.descent >= 0;
}

bool valid_scalar(uint32_t cp) {
    return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

bool is_extend(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05bd) || cp == 0x05bf ||
           (cp >= 0x05c1 && cp <= 0x05c2) ||
           (cp >= 0x0610 && cp <= 0x061a) ||
           (cp >= 0x064b && cp <= 0x065f) || cp == 0x0670 ||
           (cp >= 0x06d6 && cp <= 0x06ed) || cp == 0x0711 ||
           (cp >= 0x0730 && cp <= 0x074a) ||
           (cp >= 0x07a6 && cp <= 0x07b0) ||
           (cp >= 0x0816 && cp <= 0x082d) ||
           (cp >= 0x08d3 && cp <= 0x0903) ||
           (cp >= 0x093a && cp <= 0x094f) ||
           (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x0962 && cp <= 0x0963) ||
           (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) ||
           (cp >= 0xe0100 && cp <= 0xe01ef) ||
           (cp >= 0x1f3fb && cp <= 0x1f3ff);
}

bool is_regional_indicator(uint32_t cp) {
    return cp >= 0x1f1e6 && cp <= 0x1f1ff;
}

size_t cluster_end(const Document &doc, size_t start, size_t limit) {
    if (start >= limit) return start;
    size_t end = start + 1;
    unsigned regional_count = is_regional_indicator(doc.chars[start].codepoint) ? 1 : 0;
    while (end < limit) {
        const uint32_t cp = doc.chars[end].codepoint;
        const uint32_t previous = doc.chars[end - 1].codepoint;
        if (is_extend(cp) || cp == 0x200d || previous == 0x200d) {
            if (is_regional_indicator(cp)) ++regional_count;
            ++end;
            continue;
        }
        if (is_regional_indicator(previous) && is_regional_indicator(cp) &&
            (regional_count & 1u)) {
            ++regional_count;
            ++end;
            continue;
        }
        break;
    }
    return end;
}

bool is_cjk(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) ||
           (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0x3040 && cp <= 0x30ff) ||
           (cp >= 0x3100 && cp <= 0x312f) ||
           (cp >= 0xac00 && cp <= 0xd7af) ||
           (cp >= 0x20000 && cp <= 0x2fa1f) ||
           (cp >= 0x3000 && cp <= 0x303f) ||
           (cp >= 0xff00 && cp <= 0xffef);
}

bool is_glue(uint32_t cp) {
    return cp == 0x00a0 || cp == 0x202f || cp == 0x2060 || cp == 0xfeff;
}

bool is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x3000;
}

bool is_break_space(uint32_t cp) {
    return is_space(cp);
}

bool is_opening(uint32_t cp) {
    switch (cp) {
    case '(': case '[': case '{': case '"': case '\'':
    case 0x2018: case 0x201c: case 0x3008: case 0x300a:
    case 0x300c: case 0x300e: case 0x3010: case 0x3014:
    case 0x3016: case 0x3018: case 0x301a:
    case 0xff08: case 0xff3b: case 0xff5b:
        return true;
    default:
        return false;
    }
}

bool is_closing(uint32_t cp) {
    switch (cp) {
    case ',': case '.': case ';': case ':': case '!': case '?':
    case ')': case ']': case '}': case '"': case '\'':
    case 0x2019: case 0x201d: case 0x3001: case 0x3002:
    case 0x3009: case 0x300b: case 0x300d: case 0x300f:
    case 0x3011: case 0x3015: case 0x3017: case 0x3019:
    case 0x301b: case 0xff01: case 0xff09: case 0xff0c:
    case 0xff0e: case 0xff1a: case 0xff1b: case 0xff1f:
    case 0xff3d: case 0xff5d:
        return true;
    default:
        return false;
    }
}

bool is_hyphen(uint32_t cp) {
    return cp == '-' || cp == 0x2010 || cp == 0x2012 || cp == 0x2013;
}

bool break_allowed(const Document &doc, size_t boundary, size_t start, size_t end) {
    if (boundary <= start || boundary >= end) return false;
    const uint32_t previous = doc.chars[boundary - 1].codepoint;
    const uint32_t next = doc.chars[boundary].codepoint;
    if (is_glue(previous) || is_glue(next) || is_opening(previous) || is_closing(next))
        return false;
    if (is_break_space(previous) || is_hyphen(previous) || previous == 0x200b)
        return true;
    return is_cjk(previous) || is_cjk(next);
}

bool justification_gap(const Document &doc, size_t boundary, size_t start, size_t end) {
    if (boundary <= start || boundary >= end) return false;
    const uint32_t previous = doc.chars[boundary - 1].codepoint;
    const uint32_t next = doc.chars[boundary].codepoint;
    if (is_glue(previous) || is_glue(next) || is_extend(next) ||
        previous == 0x200d || next == 0x200d ||
        is_opening(previous) || is_closing(previous) ||
        is_opening(next) || is_closing(next)) return false;
    if (previous == ' ' || previous == 0x3000) return true;
    return is_cjk(previous) && is_cjk(next);
}

float cluster_width(const Array<Metrics> &metrics, size_t start, size_t end) {
    float width = 0;
    for (size_t i = start; i < end; ++i) width += metrics[i].advance;
    return width;
}

bool choose_line_end(const Document &doc, const Array<Metrics> &metrics,
                     size_t start, size_t limit, float available,
                     size_t &line_end, float &line_width) {
    if (start >= limit) {
        line_end = start;
        line_width = 0;
        return true;
    }
    float width = 0;
    float break_width = 0;
    float last_cluster_width = 0;
    size_t last_break = SIZE_MAX;
    size_t last_cluster_start = start;
    size_t pos = start;
    while (pos < limit) {
        const size_t next = cluster_end(doc, pos, limit);
        const float advance = cluster_width(metrics, pos, next);
        if (!isfinite(advance) || !isfinite(width + advance)) return false;
        if (width + advance > available + Epsilon) {
            if (last_break != SIZE_MAX && last_break > start) {
                line_end = last_break;
                line_width = break_width;
                return true;
            }
            if (pos == start) {
                line_end = next;
                line_width = advance;
                if (next < limit &&
                    (is_opening(doc.chars[start].codepoint) ||
                     is_closing(doc.chars[next].codepoint))) {
                    const size_t after = cluster_end(doc, next, limit);
                    line_width += cluster_width(metrics, next, after);
                    line_end = after;
                }
                return true;
            }

            line_end = pos;
            line_width = width;
            if (line_end > start && is_opening(doc.chars[line_end - 1].codepoint)) {
                if (last_cluster_start > start) {
                    line_end = last_cluster_start;
                    line_width = width - last_cluster_width;
                } else {
                    line_width += advance;
                    line_end = next;
                }
            }
            if (line_end < limit && is_closing(doc.chars[line_end].codepoint)) {
                const size_t close_end = cluster_end(doc, line_end, limit);
                line_width += cluster_width(metrics, line_end, close_end);
                line_end = close_end;
            }
            if (line_end <= start) {
                line_end = next;
                line_width = advance;
            }
            return true;
        }
        width += advance;
        last_cluster_start = pos;
        last_cluster_width = advance;
        pos = next;
        if (break_allowed(doc, pos, start, limit)) {
            last_break = pos;
            break_width = width;
        }
    }
    line_end = limit;
    line_width = width;
    return true;
}

bool paragraph_style_valid(const ParagraphStyle &style) {
    return static_cast<unsigned>(style.alignment) <= static_cast<unsigned>(Alignment::Justify) &&
           static_cast<unsigned>(style.list) <= static_cast<unsigned>(ListKind::Number) &&
           style.line_spacing != 0;
}

bool measure_marker(Measure measure, void *context, const TextStyle &style,
                    ListKind kind, unsigned number, float &width) {
    width = 0;
    if (kind == ListKind::Plain) return true;
    if (kind == ListKind::Bullet) {
        const Metrics marker = measure(context, 0x2022, style);
        if (!valid_metrics(marker)) return false;
        width += marker.advance;
    } else {
        char digits[16];
        unsigned count = 0;
        unsigned value = number;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < sizeof(digits));
        while (count) {
            const Metrics digit = measure(context, static_cast<uint32_t>(digits[--count]), style);
            if (!valid_metrics(digit)) return false;
            width += digit.advance;
        }
        const Metrics period = measure(context, '.', style);
        if (!valid_metrics(period)) return false;
        width += period.advance;
    }
    const Metrics space = measure(context, ' ', style);
    if (!valid_metrics(space)) return false;
    width += space.advance;
    return isfinite(width);
}

bool validate_document(const Document &doc, Text &error) {
    if (doc.chars.size() > MaxChars || doc.paragraphs.size() > MaxParagraphs ||
        doc.styles.size() > MaxStyles)
        return fail(error, "文档超过排版上限");
    if (doc.paragraphs.empty()) return fail(error, "文档缺少段落");
    if (doc.paragraphs[0].start != 0) return fail(error, "首段起点无效");
    size_t previous = 0;
    for (size_t i = 0; i < doc.paragraphs.size(); ++i) {
        const size_t start = doc.paragraphs[i].start;
        if (start > doc.chars.size() || (i && start <= previous))
            return fail(error, "段落起点无效");
        if (!paragraph_style_valid(doc.paragraphs[i].style))
            return fail(error, "段落格式无效");
        if (i) {
            if (doc.chars[start - 1].codepoint != '\n')
                return fail(error, "段落起点未对应换行符");
        }
        previous = start;
    }
    for (size_t p = 0; p < doc.paragraphs.size(); ++p) {
        const size_t start = doc.paragraphs[p].start;
        const size_t end = p + 1 < doc.paragraphs.size()
                               ? doc.paragraphs[p + 1].start - 1
                               : doc.chars.size();
        for (size_t i = start; i < end; ++i) {
            if (doc.chars[i].codepoint == '\n')
                return fail(error, "段落中包含未登记的换行符");
        }
    }
    for (size_t i = 0; i < doc.chars.size(); ++i) {
        const Glyph &glyph = doc.chars[i];
        if (!valid_scalar(glyph.codepoint)) return fail(error, "文档含无效 Unicode 标量");
        if (!doc.styles.empty() && glyph.style >= doc.styles.size())
            return fail(error, "字符样式索引无效");
    }
    return true;
}

}

bool Layout::build(const Document &doc, Measure measure, void *context) {
    if (!measure) return fail(error, "布局缺少字符测量函数");
    if (!validate_document(doc, error)) return false;
    const PageSettings &page = doc.page;
    if (static_cast<unsigned>(page.left) + page.right >= page.width ||
        static_cast<unsigned>(page.top) + page.bottom >= page.height)
        return fail(error, "页面边距超出页面范围");
    const float printable_bottom = static_cast<float>(page.height - page.bottom);
    const float printable_height = printable_bottom - page.top;
    if (printable_height <= 0) return fail(error, "页面可用高度无效");

    Layout next;
    Array<Metrics> measured;
    if (!measured.resize(doc.chars.size())) return fail(error, "布局内存分配失败");
    TextStyle fallback_style;
    for (size_t i = 0; i < doc.chars.size(); ++i) {
        const uint32_t cp = doc.chars[i].codepoint;
        if (cp == '\n' || cp == '\f') {
            measured[i] = Metrics{};
            continue;
        }
        const TextStyle &style = doc.styles.empty() ? fallback_style : doc.styles[doc.chars[i].style];
        measured[i] = measure(context, cp, style);
        if (!valid_metrics(measured[i])) return fail(error, "字符测量结果无效");
    }
    if (!next.boxes.resize(doc.chars.size() + 1))
        return fail(error, "布局内存分配失败");
    if (!next.grapheme_boundaries.resize(doc.chars.size() + 1))
        return fail(error, "布局内存分配失败");
    for (size_t i = 0; i < next.grapheme_boundaries.size(); ++i)
        next.grapheme_boundaries[i] = 0;
    size_t run_start = 0;
    while (run_start < doc.chars.size()) {
        const uint32_t cp = doc.chars[run_start].codepoint;
        next.grapheme_boundaries[run_start] = 1;
        if (cp == '\n' || cp == '\f') {
            ++run_start;
            next.grapheme_boundaries[run_start] = 1;
            continue;
        }
        size_t run_end = run_start;
        while (run_end < doc.chars.size() && doc.chars[run_end].codepoint != '\n' &&
               doc.chars[run_end].codepoint != '\f') ++run_end;
        size_t offset = run_start;
        while (offset < run_end) {
            const size_t next_cluster_offset = cluster_end(doc, offset, run_end);
            if (next_cluster_offset <= offset)
                return fail(error, "无法确定组合字符边界");
            next.grapheme_boundaries[next_cluster_offset] = 1;
            offset = next_cluster_offset;
        }
        run_start = run_end;
    }
    next.grapheme_boundaries[doc.chars.size()] = 1;

    unsigned page_index = 0;
    float cursor_y = static_cast<float>(page.top);
    bool page_has_line = false;
    unsigned next_number = 1;
    bool have_document_end_box = false;

    auto new_page = [&]() -> bool {
        if (next.pages >= MaxLayoutPages) return false;
        ++page_index;
        ++next.pages;
        cursor_y = static_cast<float>(page.top);
        page_has_line = false;
        return true;
    };

    auto style_for_offset = [&](size_t offset) -> const TextStyle & {
        if (offset < doc.chars.size() && !doc.styles.empty())
            return doc.styles[doc.chars[offset].style];
        return doc.styles.empty() ? fallback_style : doc.styles[0];
    };

    auto line_metrics = [&](size_t start, size_t end, const TextStyle &fallback,
                            float &ascent, float &descent) -> bool {
        ascent = 0;
        descent = 0;
        for (size_t i = start; i < end; ++i) {
            if (measured[i].ascent > ascent) ascent = measured[i].ascent;
            if (measured[i].descent > descent) descent = measured[i].descent;
        }
        if (ascent + descent > 0) return true;
        const Metrics blank = measure(context, ' ', fallback);
        if (!valid_metrics(blank) || blank.ascent + blank.descent <= 0) return false;
        ascent = blank.ascent;
        descent = blank.descent;
        return true;
    };

    auto append_line = [&](size_t start, size_t end, size_t run_end,
                           size_t paragraph_index, const ParagraphStyle &paragraph,
                           unsigned list_number, float marker_space,
                           bool paragraph_start, bool paragraph_end,
                           float natural_width) -> bool {
        const float base_x = static_cast<float>(page.left + paragraph.left_indent) + marker_space;
        const float nominal_x = base_x + (paragraph_start ? paragraph.first_indent : 0);
        const float right_edge = static_cast<float>(page.width - page.right) - paragraph.right_indent;
        const float available = right_edge - nominal_x;
        if (!isfinite(available) || available <= 0) {
            fail(error, "段落缩进超出可用页面宽度");
            return false;
        }

        float ascent = 0, descent = 0;
        if (!line_metrics(start, end, style_for_offset(start), ascent, descent)) {
            fail(error, "空行测量结果无效");
            return false;
        }
        const float height = (ascent + descent) * paragraph.line_spacing / 100.0f;
        if (!isfinite(height) || height <= 0 || height > printable_height + Epsilon) {
            fail(error, "行高超过页面可用高度");
            return false;
        }

        size_t gaps = 0;
        if (paragraph.alignment == Alignment::Justify && end < run_end && end > start) {
            size_t cluster = start;
            while (cluster < end) {
                const size_t boundary = cluster_end(doc, cluster, end);
                if (boundary < end && justification_gap(doc, boundary, start, end)) ++gaps;
                cluster = boundary;
            }
        }
        const bool stretch = gaps && natural_width < available;
        const float extra = stretch ? (available - natural_width) / static_cast<float>(gaps) : 0;
        float visual_width = stretch ? available : natural_width;
        float x = nominal_x;
        if (paragraph.alignment == Alignment::Center && natural_width < available)
            x += (available - natural_width) / 2.0f;
        else if (paragraph.alignment == Alignment::Right && natural_width < available)
            x += available - natural_width;

        const float before = paragraph_start ? paragraph.before : 0.0f;
        float line_y = cursor_y + before;
        if (line_y + height > printable_bottom + Epsilon && page_has_line) {
            if (!new_page()) {
                fail(error, "文档超过 4096 页限制");
                return false;
            }
            line_y = cursor_y + before;
        }
        if (line_y + height > printable_bottom + Epsilon) {
            fail(error, "段前距与行高超过页面可用高度");
            return false;
        }

        if (next.lines.size() >= UINT32_MAX) {
            fail(error, "文档行数超过索引范围");
            return false;
        }
        Line line;
        line.start = start;
        line.end = end;
        line.paragraph = paragraph_index;
        line.page = page_index;
        line.list_number = list_number;
        line.x = x;
        line.y = line_y;
        line.width = visual_width;
        line.height = height;
        line.baseline = line_y + ascent;
        line.paragraph_start = paragraph_start;
        line.paragraph_end = paragraph_end;
        if (!next.lines.push(line)) {
            fail(error, "布局内存分配失败");
            return false;
        }
        const unsigned line_index = static_cast<unsigned>(next.lines.size() - 1);

        float glyph_x = x;
        size_t cluster = start;
        while (cluster < end) {
            const size_t cluster_limit = cluster_end(doc, cluster, end);
            for (size_t i = cluster; i < cluster_limit; ++i) {
                GlyphBox &box = next.boxes[i];
                box.x = glyph_x;
                box.y = line_y;
                box.width = measured[i].advance;
                box.line = line_index;
                glyph_x += measured[i].advance;
            }
            if (cluster_limit < end && stretch &&
                justification_gap(doc, cluster_limit, start, end))
                glyph_x += extra;
            cluster = cluster_limit;
        }
        GlyphBox &end_box = next.boxes[end];
        end_box.x = glyph_x;
        end_box.y = line_y;
        end_box.width = 0;
        end_box.line = line_index;

        cursor_y = line_y + height;
        page_has_line = true;
        have_document_end_box = end == doc.chars.size();
        return true;
    };

    auto layout_run = [&](size_t run_start, size_t run_end, size_t paragraph_index,
                          const ParagraphStyle &paragraph, unsigned list_number,
                          float marker_space, bool &first_visual_line,
                          bool last_run) -> bool {
        if (run_start == run_end) {
            if (!append_line(run_start, run_end, run_end, paragraph_index, paragraph,
                             list_number, marker_space, first_visual_line, last_run, 0))
                return false;
            first_visual_line = false;
            return true;
        }
        size_t offset = run_start;
        while (offset < run_end) {
            const float base_x = static_cast<float>(page.left + paragraph.left_indent) + marker_space;
            const float nominal_x = base_x + (first_visual_line ? paragraph.first_indent : 0);
            const float right_edge = static_cast<float>(page.width - page.right) - paragraph.right_indent;
            const float available = right_edge - nominal_x;
            size_t line_end = offset;
            float width = 0;
            if (!choose_line_end(doc, measured, offset, run_end, available, line_end, width)) {
                fail(error, "无法确定换行位置");
                return false;
            }
            const bool is_last_line = line_end == run_end;
            if (!append_line(offset, line_end, run_end, paragraph_index, paragraph,
                             list_number, marker_space, first_visual_line,
                             last_run && is_last_line, width)) return false;
            first_visual_line = false;
            offset = line_end;
        }
        return true;
    };

    for (size_t p = 0; p < doc.paragraphs.size(); ++p) {
        const Paragraph &paragraph_record = doc.paragraphs[p];
        const ParagraphStyle &paragraph = paragraph_record.style;
        const size_t paragraph_start_offset = paragraph_record.start;
        const size_t paragraph_end_offset = p + 1 < doc.paragraphs.size()
                                                ? doc.paragraphs[p + 1].start - 1
                                                : doc.chars.size();
        const unsigned list_number = paragraph.list == ListKind::Number ? next_number++ : 0;
        if (paragraph.list != ListKind::Number) next_number = 1;
        const TextStyle &marker_style = style_for_offset(paragraph_start_offset);
        float marker_space = 0;
        if (!measure_marker(measure, context, marker_style, paragraph.list,
                           list_number ? list_number : 1, marker_space))
            return fail(error, "列表标记测量结果无效");

        bool first_visual_line = true;
        size_t run_start = paragraph_start_offset;
        for (size_t i = paragraph_start_offset; i < paragraph_end_offset; ++i) {
            if (doc.chars[i].codepoint != '\f') continue;
            if (run_start < i || run_start == paragraph_start_offset ||
                (i > paragraph_start_offset && doc.chars[i - 1].codepoint == '\f')) {
                if (!layout_run(run_start, i, p, paragraph, list_number,
                                marker_space, first_visual_line, false)) return false;
            }
            run_start = i + 1;
            if (!new_page()) return fail(error, "文档超过 4096 页限制");
        }
        if (run_start < paragraph_end_offset || run_start == paragraph_start_offset ||
            (paragraph_end_offset > paragraph_start_offset &&
             doc.chars[paragraph_end_offset - 1].codepoint == '\f')) {
            if (!layout_run(run_start, paragraph_end_offset, p, paragraph,
                            list_number, marker_space, first_visual_line, true)) return false;
        }
        cursor_y += paragraph.after;
    }

    if (doc.chars.empty() && next.lines.empty())
        return fail(error, "空文档未生成可编辑行");
    if (!have_document_end_box && !next.lines.empty()) {
        const Line &last = next.lines[next.lines.size() - 1];
        GlyphBox &end_box = next.boxes[doc.chars.size()];
        end_box.x = last.x + last.width;
        end_box.y = last.y;
        end_box.width = 0;
        end_box.line = static_cast<unsigned>(next.lines.size() - 1);
    }

    next.error.clear();
    lines.swap(next.lines);
    boxes.swap(next.boxes);
    grapheme_boundaries.swap(next.grapheme_boundaries);
    pages = next.pages;
    error.clear();
    return true;
}

size_t Layout::hit_test(unsigned page, float x, float y) const {
    if (lines.empty() || boxes.empty()) return 0;
    size_t page_start = 0, page_limit = lines.size();
    while (page_start < page_limit) {
        const size_t middle = page_start + (page_limit - page_start) / 2;
        if (lines[middle].page < page) page_start = middle + 1;
        else page_limit = middle;
    }
    if (page_start >= lines.size() || lines[page_start].page != page)
        return boxes.size() - 1;
    size_t page_end = page_start;
    page_limit = lines.size();
    while (page_end < page_limit) {
        const size_t middle = page_end + (page_limit - page_end) / 2;
        if (lines[middle].page <= page) page_end = middle + 1;
        else page_limit = middle;
    }

    size_t line_low = page_start, line_high = page_end;
    while (line_low < line_high) {
        const size_t middle = line_low + (line_high - line_low) / 2;
        const Line &candidate = lines[middle];
        if (candidate.y + candidate.height < y) line_low = middle + 1;
        else line_high = middle;
    }
    size_t selected = line_low < page_end ? line_low : page_end - 1;
    if (selected > page_start) {
        const Line &current = lines[selected];
        const Line &previous = lines[selected - 1];
        const float current_distance = y < current.y ? current.y - y :
            y > current.y + current.height ? y - current.y - current.height : 0;
        const float previous_distance = y < previous.y ? previous.y - y :
            y > previous.y + previous.height ? y - previous.y - previous.height : 0;
        if (previous_distance <= current_distance) --selected;
    }
    const Line &line = lines[selected];
    size_t offset_low = line.start, offset_high = line.end + 1;
    while (offset_low < offset_high) {
        const size_t middle = offset_low + (offset_high - offset_low) / 2;
        if (caret_x(middle, selected) < x) offset_low = middle + 1;
        else offset_high = middle;
    }
    size_t next_boundary = offset_low > line.end ? line.end : offset_low;
    while (next_boundary < line.end && !grapheme_boundaries[next_boundary])
        ++next_boundary;
    size_t previous_boundary = next_boundary;
    if (previous_boundary > line.start) {
        --previous_boundary;
        while (previous_boundary > line.start && !grapheme_boundaries[previous_boundary])
            --previous_boundary;
    }
    const float next_distance = fabsf(x - caret_x(next_boundary, selected));
    const float previous_distance = fabsf(x - caret_x(previous_boundary, selected));
    return previous_distance <= next_distance ? previous_boundary : next_boundary;
}

size_t Layout::line_for(size_t offset, bool prefer_previous) const {
    if (lines.empty() || boxes.empty()) return 0;
    if (offset >= boxes.size()) offset = boxes.size() - 1;
    size_t line = boxes[offset].line;
    if (line >= lines.size()) line = lines.size() - 1;
    if (prefer_previous && line > 0 && lines[line - 1].end == offset &&
        lines[line - 1].paragraph == lines[line].paragraph &&
        !lines[line - 1].paragraph_end)
        return line - 1;
    return line;
}

float Layout::caret_x(size_t offset, size_t line_index) const {
    if (lines.empty() || boxes.empty()) return 0;
    if (offset >= boxes.size()) offset = boxes.size() - 1;
    if (line_index >= lines.size()) line_index = lines.size() - 1;
    const Line &line = lines[line_index];
    if (offset <= line.start) return line.x;
    if (offset >= line.end) return line.x + line.width;
    if (boxes[offset].line == line_index) return boxes[offset].x;
    return line.x + line.width;
}

}
