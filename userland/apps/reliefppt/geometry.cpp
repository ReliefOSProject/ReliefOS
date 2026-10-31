#include "geometry.h"
#include <math.h>

namespace rp {
namespace {

bool finite_rect(const Rect &r) {
    return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h);
}

bool inside_rect(const Rect &r, float x, float y) {
    return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
}

float point_segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    const float length2 = vx * vx + vy * vy;
    if (length2 <= 0.000001f) return hypotf(px - ax, py - ay);
    float t = ((px - ax) * vx + (py - ay) * vy) / length2;
    if (t < 0) t = 0;
    else if (t > 1) t = 1;
    return hypotf(px - (ax + t * vx), py - (ay + t * vy));
}

bool resize_input_valid(Rect r, int handle, float page_w, float page_h, float dx, float dy) {
    return handle >= 0 && handle <= 7 && finite_rect(r) && r.w >= 4 && r.h >= 4 &&
        isfinite(page_w) && isfinite(page_h) && page_w >= 4 && page_h >= 4 &&
        isfinite(dx) && isfinite(dy) && r.x >= -0.02f && r.y >= -0.02f &&
        r.x + r.w <= page_w + 0.02f && r.y + r.h <= page_h + 0.02f &&
        r.w <= page_w + 0.02f && r.h <= page_h + 0.02f;
}

float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

bool decode_scalar(const char *bytes, size_t length, size_t &at, uint32_t &cp) {
    if (at >= length) return false;
    const unsigned char first = static_cast<unsigned char>(bytes[at]);
    if (first < 0x80) { cp = first; ++at; return true; }
    unsigned count = 0;
    uint32_t value = 0;
    if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; }
    else return false;
    if (count > length - at) return false;
    for (unsigned i = 1; i < count; ++i) {
        const unsigned char next = static_cast<unsigned char>(bytes[at + i]);
        if ((next & 0xc0) != 0x80) return false;
        if (i == 1 && ((first == 0xe0 && next < 0xa0) || (first == 0xed && next > 0x9f) ||
                       (first == 0xf0 && next < 0x90) || (first == 0xf4 && next > 0x8f))) return false;
        value = (value << 6) | (next & 0x3f);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    cp = value;
    at += count;
    return true;
}

bool combining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05bd) || cp == 0x05bf ||
        (cp >= 0x05c1 && cp <= 0x05c2) || (cp >= 0x05c4 && cp <= 0x05c5) ||
        (cp >= 0x0610 && cp <= 0x061a) || (cp >= 0x064b && cp <= 0x065f) || cp == 0x0670 ||
        (cp >= 0x06d6 && cp <= 0x06ed) || cp == 0x0711 ||
        (cp >= 0x0730 && cp <= 0x074a) || (cp >= 0x0816 && cp <= 0x082d) ||
        (cp >= 0x08d3 && cp <= 0x0902) || (cp >= 0x093a && cp <= 0x093c) ||
        (cp >= 0x093e && cp <= 0x094f) || (cp >= 0x0951 && cp <= 0x0957) ||
        (cp >= 0x0962 && cp <= 0x0963) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
        (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
        (cp >= 0xfe20 && cp <= 0xfe2f);
}

bool variation_selector(uint32_t cp) {
    return (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

bool grapheme_extend(uint32_t cp) {
    return combining(cp) || variation_selector(cp) || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
        (cp >= 0xe0020 && cp <= 0xe007f);
}

bool regional_indicator(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

bool cjk(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0x20000 && cp <= 0x2fa1f) ||
        (cp >= 0x30000 && cp <= 0x323af) || (cp >= 0x3040 && cp <= 0x30ff) ||
        (cp >= 0xac00 && cp <= 0xd7af);
}

bool break_space(uint32_t cp) {
    return cp == 0x20 || cp == 0x09 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a && cp != 0x2007) || cp == 0x205f || cp == 0x3000;
}

bool break_punctuation(uint32_t cp) {
    return cp == '-' || cp == '/' || cp == 0x2010 || cp == 0x2013 || cp == 0x3001 ||
        cp == 0x3002 || cp == 0xff0c || cp == 0xff0e || cp == 0xff1b || cp == 0xff1a;
}

bool cluster_end(const char *bytes, size_t length, size_t start, size_t &end, uint32_t &first_cp) {
    size_t at = start;
    if (!decode_scalar(bytes, length, at, first_cp)) return false;
    if (regional_indicator(first_cp) && at < length) {
        size_t look = at;
        uint32_t cp = 0;
        if (decode_scalar(bytes, length, look, cp) && regional_indicator(cp)) at = look;
    }
    for (;;) {
        if (at >= length) break;
        size_t look = at;
        uint32_t cp = 0;
        if (!decode_scalar(bytes, length, look, cp)) return false;
        if (grapheme_extend(cp)) { at = look; continue; }
        if (cp == 0x200d) {
            at = look;
            if (at < length) {
                look = at;
                if (!decode_scalar(bytes, length, look, cp)) return false;
                at = look;
                while (at < length) {
                    look = at;
                    if (!decode_scalar(bytes, length, look, cp)) return false;
                    if (!grapheme_extend(cp)) break;
                    at = look;
                }
            }
            continue;
        }
        break;
    }
    end = at;
    return true;
}

bool append_line(Array<TextLine> &lines, const Object &object, const char *normalized,
                 size_t start, size_t end, bool paragraph_start, Measure measure,
                 void *context, float max_width, float line_height, float &total_height,
                 Text &error, bool clip_overflow) {
    if (lines.size() >= MaxTextBytes) {
        error.set("文字行数超限", strlen("文字行数超限"));
        return false;
    }
    const size_t length = end - start;
    const float width = measure(context, normalized + start, length, object.style);
    if (!isfinite(width) || width < 0 || (!clip_overflow && width > max_width + 0.01f)) {
        error.set("文字行无法放入文本框", strlen("文字行无法放入文本框"));
        return false;
    }
    total_height += line_height;
    if (!isfinite(total_height) || (!clip_overflow && total_height > object.rect.h + 0.01f)) {
        error.set("文字内容超出文本框高度", strlen("文字内容超出文本框高度"));
        return false;
    }
    TextLine line;
    line.start = start;
    line.length = length;
    line.width = width;
    line.paragraph_start = paragraph_start;
    if (!lines.push(line)) {
        error.set("内存不足", strlen("内存不足"));
        return false;
    }
    return true;
}

bool wrap_paragraph(const Object &object, const char *normalized, size_t start, size_t end,
                    Measure measure, void *context, float max_width, float line_height,
                    Array<TextLine> &lines, float &total_height, Text &error,
                    bool clip_overflow) {
    if (start == end)
        return append_line(lines, object, normalized, start, end, true, measure,
                           context, max_width, line_height, total_height, error, clip_overflow);
    size_t line_start = start;
    bool first_line = true;
    while (line_start < end) {
        size_t cursor = line_start;
        size_t last_break = line_start;
        float width = 0;
        bool overflow = false;
        bool oversized_cluster = false;
        while (cursor < end) {
            size_t next = cursor;
            uint32_t cp = 0;
            if (!cluster_end(normalized, end, cursor, next, cp) || next <= cursor) {
                error.set("文字编码无效", strlen("文字编码无效"));
                return false;
            }
            const float cluster_width = measure(context, normalized + cursor, next - cursor, object.style);
            if (!isfinite(cluster_width) || cluster_width < 0) {
                error.set("文字测量结果无效", strlen("文字测量结果无效"));
                return false;
            }
            if (cluster_width > max_width + 0.01f && cursor == line_start) {
                if (!clip_overflow) {
                    error.set("单个文字簇超出文本框宽度", strlen("单个文字簇超出文本框宽度"));
                    return false;
                }
                cursor = next;
                oversized_cluster = true;
                break;
            }
            if (width + cluster_width > max_width + 0.01f) { overflow = true; break; }
            width += cluster_width;
            cursor = next;
            if (break_space(cp) || cjk(cp) || break_punctuation(cp)) last_break = cursor;
        }
        if (oversized_cluster) {
            if (!append_line(lines, object, normalized, line_start, cursor, first_line,
                             measure, context, max_width, line_height, total_height, error,
                             clip_overflow)) return false;
            first_line = false;
            line_start = cursor;
            continue;
        }
        if (!overflow) {
            if (!append_line(lines, object, normalized, line_start, end, first_line, measure,
                             context, max_width, line_height, total_height, error,
                             clip_overflow)) return false;
            break;
        }
        size_t line_end = last_break > line_start ? last_break : cursor;
        if (line_end <= line_start || line_end > end) {
            error.set("无法排版文字", strlen("无法排版文字"));
            return false;
        }
        if (!append_line(lines, object, normalized, line_start, line_end, first_line, measure,
                         context, max_width, line_height, total_height, error,
                         clip_overflow)) return false;
        first_line = false;
        line_start = line_end;
    }
    return true;
}

} // namespace

Bounds selection_bounds(const Deck &deck, size_t slide, const Array<uint32_t> &ids) {
    Bounds result;
    if (slide >= deck.slides.size() || ids.empty()) return result;
    bool first = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        const Object *object = deck.object(slide, ids[i]);
        if (!object) return Bounds{};
        if (first) {
            result.rect = object->rect;
            first = false;
        } else {
            const float left = object->rect.x < result.rect.x ? object->rect.x : result.rect.x;
            const float top = object->rect.y < result.rect.y ? object->rect.y : result.rect.y;
            const float right = object->rect.x + object->rect.w > result.rect.x + result.rect.w
                ? object->rect.x + object->rect.w : result.rect.x + result.rect.w;
            const float bottom = object->rect.y + object->rect.h > result.rect.y + result.rect.h
                ? object->rect.y + object->rect.h : result.rect.y + result.rect.h;
            result.rect = {left, top, right - left, bottom - top};
        }
    }
    result.valid = !first;
    return result;
}

uint32_t hit_test(const Slide &slide, float x, float y, float tolerance) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(tolerance) || tolerance < 0) return 0;
    for (size_t i = slide.objects.size(); i > 0; --i) {
        const Object &object = slide.objects[i - 1];
        const Rect &r = object.rect;
        if (!finite_rect(r) || r.w < 0 || r.h < 0) continue;
        bool hit = false;
        if (object.kind == Kind::Line || object.kind == Kind::Arrow) {
            const float effective = tolerance + (isfinite(object.style.line_width) && object.style.line_width > 0
                ? object.style.line_width * 0.5f : 0);
            const float center_y = r.y + r.h * 0.5f;
            const float distance = point_segment_distance(x, y, r.x, center_y, r.x + r.w, center_y);
            hit = distance <= effective;
        } else if (object.kind == Kind::Ellipse && r.w > 0 && r.h > 0) {
            const float nx = (x - (r.x + r.w * 0.5f)) / (r.w * 0.5f);
            const float ny = (y - (r.y + r.h * 0.5f)) / (r.h * 0.5f);
            hit = nx * nx + ny * ny <= 1.0f;
        } else if (static_cast<unsigned>(object.kind) <= static_cast<unsigned>(Kind::Image)) {
            hit = inside_rect(r, x, y);
        }
        if (hit) return object.id;
    }
    return 0;
}

bool resize_rect(Rect original, int handle, float dx, float dy, bool keep_aspect,
                 float page_w, float page_h, Rect &out) {
    if (!resize_input_valid(original, handle, page_w, page_h, dx, dy)) return false;
    Rect base = original;
    base.x = clampf(base.x, 0, page_w - base.w);
    base.y = clampf(base.y, 0, page_h - base.h);
    Rect result = base;
    if (!keep_aspect) {
        const float right = base.x + base.w;
        const float bottom = base.y + base.h;
        switch (handle) {
            case 0: result.x = clampf(base.x + dx, 0, right - 4); result.y = clampf(base.y + dy, 0, bottom - 4); break;
            case 1: result.y = clampf(base.y + dy, 0, bottom - 4); break;
            case 2: result.y = clampf(base.y + dy, 0, bottom - 4); result.w = clampf(base.w + dx, 4, page_w - base.x); break;
            case 3: result.w = clampf(base.w + dx, 4, page_w - base.x); break;
            case 4: result.w = clampf(base.w + dx, 4, page_w - base.x); result.h = clampf(base.h + dy, 4, page_h - base.y); break;
            case 5: result.h = clampf(base.h + dy, 4, page_h - base.y); break;
            case 6: result.x = clampf(base.x + dx, 0, right - 4); result.h = clampf(base.h + dy, 4, page_h - base.y); break;
            case 7: result.x = clampf(base.x + dx, 0, right - 4); break;
        }
        if (handle == 0 || handle == 1 || handle == 2 || handle == 6 || handle == 7)
            result.h = (handle == 0 || handle == 1 || handle == 2) ? bottom - result.y : result.h;
        if (handle == 0 || handle == 6 || handle == 7) result.w = right - result.x;
        if (result.x + result.w > page_w) result.w = page_w - result.x;
        if (result.y + result.h > page_h) result.h = page_h - result.y;
        if (result.w < 4 || result.h < 4) return false;
        out = result;
        return true;
    }

    const float aspect = base.w / base.h;
    const float minimum_scale = fmaxf(4.0f / base.w, 4.0f / base.h);
    const bool east = handle == 2 || handle == 3 || handle == 4;
    const bool west = handle == 0 || handle == 6 || handle == 7;
    const bool north = handle == 0 || handle == 1 || handle == 2;
    const bool south = handle == 4 || handle == 5 || handle == 6;
    const bool horizontal_edge = handle == 1 || handle == 5;
    const bool vertical_edge = handle == 3 || handle == 7;
    float anchor_x = 0, anchor_y = 0, max_scale = 1, desired_scale = 1;
    if (horizontal_edge) {
        anchor_y = north ? base.y + base.h : base.y;
        const float available_y = north ? anchor_y : page_h - anchor_y;
        const float center_x = base.x + base.w * 0.5f;
        const float available_x = 2.0f * fminf(center_x, page_w - center_x);
        max_scale = fminf(available_y / base.h, available_x / base.w);
        const float signed_delta = north ? -dy : dy;
        desired_scale = 1.0f + signed_delta / base.h;
        const float scaled_h = base.h * clampf(desired_scale, minimum_scale, max_scale);
        result.h = scaled_h;
        result.w = scaled_h * aspect;
        result.x = center_x - result.w * 0.5f;
        result.y = north ? anchor_y - result.h : anchor_y;
    } else if (vertical_edge) {
        anchor_x = west ? base.x + base.w : base.x;
        const float available_x = west ? anchor_x : page_w - anchor_x;
        const float center_y = base.y + base.h * 0.5f;
        const float available_y = 2.0f * fminf(center_y, page_h - center_y);
        max_scale = fminf(available_x / base.w, available_y / base.h);
        const float signed_delta = west ? -dx : dx;
        desired_scale = 1.0f + signed_delta / base.w;
        const float scaled_w = base.w * clampf(desired_scale, minimum_scale, max_scale);
        result.w = scaled_w;
        result.h = scaled_w / aspect;
        result.x = west ? anchor_x - result.w : anchor_x;
        result.y = center_y - result.h * 0.5f;
    } else {
        anchor_x = east ? base.x : base.x + base.w;
        anchor_y = south ? base.y : base.y + base.h;
        const float available_x = east ? page_w - anchor_x : anchor_x;
        const float available_y = south ? page_h - anchor_y : anchor_y;
        max_scale = fminf(available_x / base.w, available_y / base.h);
        const float scale_x = 1.0f + (east ? dx : -dx) / base.w;
        const float scale_y = 1.0f + (south ? dy : -dy) / base.h;
        desired_scale = fabsf(scale_x - 1.0f) >= fabsf(scale_y - 1.0f) ? scale_x : scale_y;
        const float scale = clampf(desired_scale, minimum_scale, max_scale);
        result.w = base.w * scale;
        result.h = base.h * scale;
        result.x = east ? anchor_x : anchor_x - result.w;
        result.y = south ? anchor_y : anchor_y - result.h;
    }
    if (!finite_rect(result) || result.w < 4 || result.h < 4 || result.x < -0.001f ||
        result.y < -0.001f || result.x + result.w > page_w + 0.001f ||
        result.y + result.h > page_h + 0.001f || !isfinite(aspect)) return false;
    out = result;
    return true;
}

bool wrap_text(const Object &object, Measure measure, void *context,
               Array<TextLine> &output, Text &error) {
    return wrap_text(object, measure, context, output, error, false);
}

bool wrap_text(const Object &object, Measure measure, void *context,
               Array<TextLine> &output, Text &error, bool clip_overflow) {
    if (!measure) { error.set("文字测量函数无效", strlen("文字测量函数无效")); return false; }
    if (object.kind != Kind::Text || object.text.size() > MaxTextBytes ||
        !valid_utf8(object.text.c_str(), object.text.size()) || !finite_rect(object.rect) ||
        object.rect.w <= 0 || object.rect.h <= 0 || !isfinite(object.style.font_size) ||
        object.style.font_size < 6 || object.style.font_size > 144 ||
        object.style.text > 0xffffff || object.style.fill > 0xffffff ||
        object.style.stroke > 0xffffff || !isfinite(object.style.line_width) ||
        object.style.line_width < 0 || object.style.line_width > 20 || object.style.family > 2 ||
        static_cast<unsigned>(object.style.alignment) > static_cast<unsigned>(Alignment::Right)) {
        error.set("文本对象无效", strlen("文本对象无效"));
        return false;
    }
    const float reserved = object.style.bullets ? object.style.font_size * 0.8f : 0.0f;
    const float max_width = object.rect.w - 16.0f - reserved;
    const float line_height = object.style.font_size * 1.25f;
    if (!isfinite(max_width) || !isfinite(line_height) || line_height <= 0) {
        error.set("文本框没有可用排版区域", strlen("文本框没有可用排版区域"));
        return false;
    }
    if (max_width <= 0) {
        if (!clip_overflow) {
            error.set("文本框没有可用排版区域", strlen("文本框没有可用排版区域"));
            return false;
        }
        Array<TextLine> empty;
        output.swap(empty);
        error.clear();
        return true;
    }
    Array<char> normalized;
    if (object.text.size() == SIZE_MAX || !normalized.resize(object.text.size() + 1)) {
        error.set("内存不足", strlen("内存不足"));
        return false;
    }
    if (object.text.size()) memcpy(normalized.data(), object.text.c_str(), object.text.size());
    normalized[object.text.size()] = 0;
    for (size_t i = 0; i < object.text.size(); ++i)
        if (normalized[i] == '\t') normalized[i] = ' ';

    Array<TextLine> lines;
    float total_height = 0;
    const size_t length = object.text.size();
    size_t paragraph_start = 0;
    for (;;) {
        size_t paragraph_end = paragraph_start;
        while (paragraph_end < length && object.text.c_str()[paragraph_end] != '\n') ++paragraph_end;
        if (!wrap_paragraph(object, normalized.data(), paragraph_start, paragraph_end,
                            measure, context, max_width, line_height, lines, total_height,
                            error, clip_overflow)) return false;
        if (paragraph_end == length) break;
        paragraph_start = paragraph_end + 1;
        if (paragraph_start > length) break;
    }
    output.swap(lines);
    error.clear();
    return true;
}

} // namespace rp
