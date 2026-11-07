#include "render.h"
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
namespace rp {
namespace {
Fl_Color rgb(uint32_t c) { return fl_rgb_color(c >> 16, (c >> 8) & 255, c & 255); }
// FLTK 1.3's Xft path draws one font at a time. Use the bundled Chinese
// face for CJK runs, with matching fonts during measuring and drawing.
bool chinese(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0xfe10 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xffef) ||
           (cp >= 0x20000 && cp <= 0x323af);
}
uint32_t next_codepoint(const char *text, size_t length, size_t &at) {
    unsigned char c = static_cast<unsigned char>(text[at++]);
    unsigned count = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    uint32_t cp = count ? c & (0x7fU >> count) : c;
    while (count && at < length) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[at++]) & 63);
        --count;
    }
    return cp;
}
float text_runs(const char *text, size_t length, const Style &style, int pixels, bool draw, float X = 0,
                int Y = 0) {
    size_t at = 0;
    float width = 0;
    const Fl_Font latin = style_font(style),
                  cjk = Fl_Font(16 + (style.bold ? 1 : 0) + (style.italic ? 2 : 0));
    while (at < length) {
        size_t start = at;
        Fl_Font font = chinese(next_codepoint(text, length, at)) ? cjk : latin;
        while (at < length) {
            size_t next = at;
            Fl_Font next_font = chinese(next_codepoint(text, length, next)) ? cjk : latin;
            if (next_font != font)
                break;
            at = next;
        }
        fl_font(font, pixels);
        if (draw)
            fl_draw(text + start, int(at - start), int(round(X + width)), Y);
        width += float(fl_width(text + start, int(at - start)));
    }
    return width;
}
bool normalize_tabs(const char *bytes, size_t n, Text &out) {
    if (!memchr(bytes, '\t', n))
        return out.set(bytes, n);
    Array<char> b;
    if (n > MaxTextBytes || !b.reserve(n + 1))
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (!b.push(bytes[i] == '\t' ? ' ' : bytes[i]))
            return false;
    }
    return out.set(b.data(), b.size());
}
bool append(Array<char> &out, const char *s, size_t n) {
    if (!out.reserve(out.size() + n))
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!out.push(s[i]))
            return false;
    return true;
}
bool append(Array<char> &out, const char *s) { return append(out, s, strlen(s)); }
bool base64(Array<char> &out, const Text &data) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out.reserve(out.size() + (data.size() + 2) / 3 * 4))
        return false;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(data.c_str());
    for (size_t i = 0; i < data.size(); i += 3) {
        size_t left = data.size() - i;
        unsigned n = unsigned(b[i]) << 16;
        if (left > 1)
            n |= unsigned(b[i + 1]) << 8;
        if (left > 2)
            n |= b[i + 2];
        char c[4] = {alphabet[n >> 18], alphabet[(n >> 12) & 63], left > 1 ? alphabet[(n >> 6) & 63] : '=',
                     left > 2 ? alphabet[n & 63] : '='};
        if (!append(out, c, 4))
            return false;
    }
    return true;
}
bool escaped(Array<char> &out, const Text &text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text.c_str()[i];
        if (c == '&') {
            if (!append(out, "&amp;"))
                return false;
        } else if (c == '<') {
            if (!append(out, "&lt;"))
                return false;
        } else if (c == '>') {
            if (!append(out, "&gt;"))
                return false;
        } else if (c == '\"') {
            if (!append(out, "&quot;"))
                return false;
        } else if (!out.push(c))
            return false;
    }
    return true;
}
bool draw_object(const Deck &deck, const Object &o, int X, int Y, float s, Text &error) {
    const int x = X + int(round(o.rect.x * s)), y = Y + int(round(o.rect.y * s));
    const int w = int(round(o.rect.w * s)), h = int(round(o.rect.h * s));
    if (w <= 0 || h <= 0)
        return true;
    if (o.kind == Kind::Image) {
        if (o.asset >= deck.assets.size()) {
            error.set("Invalid image reference");
            return false;
        }
        const Asset &a = deck.assets[o.asset];
        Fl_RGB_Image image(reinterpret_cast<const unsigned char *>(a.rgba.c_str()), int(a.width),
                           int(a.height), 4);
        Fl_Image *scaled = image.copy(w, h);
        if (!scaled) {
            error.set("Not enough memory to draw the image");
            return false;
        }
        scaled->draw(x, y);
        delete scaled;
        return true;
    }
    if (o.kind == Kind::Rectangle || o.kind == Kind::Ellipse) {
        if (o.style.filled) {
            fl_color(rgb(o.style.fill));
            if (o.kind == Kind::Rectangle)
                fl_rectf(x, y, w, h);
            else
                fl_pie(x, y, w, h, 0, 360);
        }
        if (o.style.outlined && o.style.line_width > 0) {
            fl_color(rgb(o.style.stroke));
            fl_line_style(FL_SOLID, int(fmax(1, round(o.style.line_width * s))));
            if (o.kind == Kind::Rectangle)
                fl_rect(x, y, w, h);
            else
                fl_arc(x, y, w, h, 0, 360);
            fl_line_style(0);
        }
        return true;
    }
    if (o.kind == Kind::Line || o.kind == Kind::Arrow) {
        fl_color(rgb(o.style.stroke));
        fl_line_style(FL_SOLID, int(fmax(1, round(o.style.line_width * s))));
        fl_line(x, y + h / 2, x + w, y + h / 2);
        if (o.kind == Kind::Arrow) {
            int a = int(fmin(w / 3., fmax(7., h / 2.)));
            fl_line(x + w - a, y + h / 2 - a, x + w, y + h / 2, x + w - a, y + h / 2 + a);
        }
        fl_line_style(0);
        return true;
    }
    if (o.style.filled) {
        fl_color(rgb(o.style.fill));
        fl_rectf(x, y, w, h);
    }
    if (o.style.outlined && o.style.line_width > 0) {
        fl_color(rgb(o.style.stroke));
        fl_line_style(FL_SOLID, int(fmax(1, round(o.style.line_width * s))));
        fl_rect(x, y, w, h);
        fl_line_style(0);
    }
    if (o.rect.w <= 16 || o.rect.h <= 16)
        return true;
    Array<TextLine> lines;
    if (!wrap_text(o, measure_text, nullptr, lines, error, true))
        return false;
    fl_push_clip(x + int(8 * s), y + int(8 * s), int((o.rect.w - 16) * s), int((o.rect.h - 16) * s));
    fl_font(style_font(o.style), int(fmax(1, round(o.style.font_size * s))));
    fl_color(rgb(o.style.text));
    const float bullet = o.style.bullets ? o.style.font_size * .8f : 0;
    for (size_t n = 0; n < lines.size(); ++n) {
        const TextLine &l = lines[n];
        const float top = 8 + n * o.style.font_size * 1.25f;
        if (top >= o.rect.h - 8)
            break;
        float px = 8 + bullet, available = o.rect.w - 16 - bullet;
        if (o.style.alignment == Alignment::Center)
            px += (available - l.width) / 2;
        else if (o.style.alignment == Alignment::Right)
            px += available - l.width;
        const int baseline = y + int(round((top + o.style.font_size) * s));
        if (o.style.bullets && l.paragraph_start) {
            fl_font(style_font(o.style), int(fmax(1, round(o.style.font_size * s))));
            fl_draw("•", x + int(8 * s), baseline);
        }
        Text text;
        if (!normalize_tabs(o.text.c_str() + l.start, l.length, text)) {
            fl_pop_clip();
            error.set("Not enough memory to draw text");
            return false;
        }
        text_runs(text.c_str(), text.size(), o.style, int(fmax(1, round(o.style.font_size * s))), true,
                  x + px * s, baseline);
        if (o.style.underline && l.length)
            fl_line(x + int(round(px * s)), baseline + int(fmax(1, s * 2)),
                    x + int(round((px + l.width) * s)), baseline + int(fmax(1, s * 2)));
    }
    fl_pop_clip();
    return true;
}
} // namespace
Fl_Font style_font(const Style &s) {
    return Fl_Font(16 + int(s.family) * 4 + (s.bold ? 1 : 0) + (s.italic ? 2 : 0));
}
Fl_Font editing_font(const Style &style, const char *text, size_t length) {
    size_t at = 0;
    while (at < length)
        if (chinese(next_codepoint(text, length, at)))
            return Fl_Font(16 + (style.bold ? 1 : 0) + (style.italic ? 2 : 0));
    return style_font(style);
}
float measure_text(void *, const char *text, size_t size, const Style &s) {
    if (!memchr(text, '\t', size))
        return text_runs(text, size, s, int(round(s.font_size)), false);
    Text expanded;
    if (!normalize_tabs(text, size, expanded))
        return -1;
    return text_runs(expanded.c_str(), expanded.size(), s, int(round(s.font_size)), false);
}
bool text_overflows(const Object &o) {
    Array<TextLine> lines;
    Text error;
    return !wrap_text(o, measure_text, nullptr, lines, error) ||
           lines.size() * o.style.font_size * 1.25f > o.rect.h - 16;
}
bool draw_slide(const Deck &deck, size_t index, int x, int y, float scale, const Array<Object> *preview,
                Text *error_out) {
    if (index >= deck.slides.size() || !isfinite(scale) || scale <= 0)
        return false;
    const Slide &slide = deck.slides[index];
    fl_push_clip(x, y, int(round(deck.width * scale)), int(round(deck.height * scale)));
    fl_color(rgb(slide.background));
    fl_rectf(x, y, int(round(deck.width * scale)), int(round(deck.height * scale)));
    bool ok = true;
    Text error;
    for (size_t i = 0; i < slide.objects.size(); ++i) {
        const Object *o = &slide.objects[i];
        if (preview)
            for (size_t p = 0; p < preview->size(); ++p)
                if ((*preview)[p].id == o->id) {
                    o = &(*preview)[p];
                    break;
                }
        if (!draw_object(deck, *o, x, y, scale, error)) {
            ok = false;
            break;
        }
    }
    fl_pop_clip();
    if (!ok && error_out)
        *error_out = error;
    return ok;
}
bool render_png(const Deck &deck, size_t index, Text &output, Text &error) {
    if (index >= deck.slides.size() || !deck.validate(error))
        return false;
    Fl_Surface_Device *previous = Fl_Surface_Device::surface();
    Fl_RGB_Image *image = nullptr;
    {
        Fl_Image_Surface surface(int(round(deck.width)), int(round(deck.height)));
        surface.set_current();
        bool ok = draw_slide(deck, index, 0, 0, 1, nullptr, &error);
        if (ok)
            image = surface.image();
        previous->set_current();
        if (!ok)
            return false;
    }
    if (!image || !image->array) {
        delete image;
        error.set("Could not render the slide image");
        return false;
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = image->w();
    png.height = image->h();
    png.format = image->d() == 4 ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;
    png_alloc_size_t size = 0;
    bool ok = png_image_write_to_memory(&png, nullptr, &size, 0, image->array, image->ld(), nullptr) != 0;
    unsigned char *bytes = ok ? static_cast<unsigned char *>(malloc(size)) : nullptr;
    if (!bytes)
        ok = false;
    if (ok)
        ok = png_image_write_to_memory(&png, bytes, &size, 0, image->array, image->ld(), nullptr) != 0 &&
             output.set(reinterpret_cast<char *>(bytes), size);
    free(bytes);
    png_image_free(&png);
    delete image;
    if (!ok)
        error.set("Could not encode the slide image");
    return ok;
}
bool render_html(const Deck &deck, Text &output, Text &error, ExportProgress progress, void *context) {
    Array<char> out;
    bool ok =
        append(out, "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\"><meta name=\"viewport\" "
                    "content=\"width=device-width,initial-scale=1\"><title>") &&
        escaped(out, deck.title) &&
        append(out,
               " — "
               "ReliefPPT</"
               "title><style>html,body{margin:0;width:100%;height:100%;background:#10151d;color:white;font:"
               "16px sans-serif}main{height:calc(100% - "
               "38px);display:flex;align-items:center;justify-content:center}img{max-width:100%;max-height:"
               "100%;object-fit:contain;display:none}img.active{display:block}footer{height:38px;display:"
               "flex;align-items:center;justify-content:center;gap:18px}button{font:inherit;color:inherit;"
               "background:#293443;border:1px solid #667788;padding:3px 14px;cursor:pointer}</style><main>");
    for (size_t i = 0; ok && i < deck.slides.size(); ++i) {
        if (progress && !progress(context, unsigned(i), unsigned(deck.slides.size()))) {
            error.set("Export cancelled");
            return false;
        }
        Text png;
        ok = render_png(deck, i, png, error);
        char number[120];
        snprintf(number, sizeof(number), "<img%s alt=\"Slide %zu\" src=\"data:image/png;base64,\"",
                 i == 0 ? " class=\"active\"" : "", i + 1);
        // Keep the data URL in one attribute; no untrusted content enters script.
        if (ok) {
            number[strlen(number) - 1] = 0;
            ok = append(out, number) && base64(out, png) && append(out, "\">");
        }
        if (out.size() > 256U * 1024U * 1024U) {
            error.set("HTML export is too large");
            return false;
        }
    }
    ok = ok &&
         append(out, "</main><footer><button id=\"prev\" aria-label=\"Previous slide\">←</button><span "
                     "id=\"count\"></span><button id=\"next\" aria-label=\"Next slide\">→</button><button "
                     "id=\"full\">Full screen</button></footer><script>'use strict';const "
                     "slides=Array.from(document.querySelectorAll('main img'));let n=0;function "
                     "show(i){n=Math.max(0,Math.min(slides.length-1,i));slides.forEach((s,j)=>s.classList."
                     "toggle('active',j===n));document.getElementById('count').textContent=(n+1)+' / "
                     "'+slides.length;}document.getElementById('prev').onclick=()=>show(n-1);document."
                     "getElementById('next').onclick=()=>show(n+1);document.getElementById('full').onclick=()"
                     "=>{if(!document.fullscreenElement)document.documentElement.requestFullscreen?.();else "
                     "document.exitFullscreen?.();};document.addEventListener('keydown',e=>{if(['ArrowRight',"
                     "'PageDown',' "
                     "'].includes(e.key)){e.preventDefault();show(n+1);}if(['ArrowLeft','PageUp'].includes(e."
                     "key)){e.preventDefault();show(n-1);}if(e.key==='Home')show(0);if(e.key==='End')show("
                     "slides.length-1);if(e.key.toLowerCase()==='b')document.querySelector('main').style."
                     "visibility=document.querySelector('main').style.visibility==='hidden'?'visible':'"
                     "hidden';});show(0);</script></html>");
    if (out.size() > 256U * 1024U * 1024U) {
        error.set("HTML export is too large");
        return false;
    }
    if (ok)
        ok = output.set(out.data(), out.size());
    if (!ok && error.empty())
        error.set("Not enough memory to export HTML");
    return ok;
}
bool load_image(const char *path, Asset &out, Text &error) {
    struct stat st{};
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        uint64_t(st.st_size) > MaxFileBytes) {
        error.set("Could not read the image file");
        return false;
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&png, path)) {
        if (!png.width || !png.height || png.width > 4096 || png.height > 4096 ||
            uint64_t(png.width) * png.height * 4 > MaxAssetBytes) {
            png_image_free(&png);
            error.set("Image dimensions exceed the supported limit");
            return false;
        }
        png.format = PNG_FORMAT_RGBA;
        size_t size = size_t(png.width) * png.height * 4;
        char *data = static_cast<char *>(malloc(size));
        bool ok = data && png_image_finish_read(&png, nullptr, data, 0, nullptr);
        Asset next;
        if (ok) {
            next.width = png.width;
            next.height = png.height;
            ok = next.rgba.set(data, size);
        }
        free(data);
        png_image_free(&png);
        if (ok)
            out = static_cast<Asset &&>(next);
        else
            error.set("Could not decode the PNG image");
        return ok;
    }
    png_image_free(&png);
    // Uncompressed 24/32-bit BMP, checked before any dimensional allocation.
    FILE *f = fopen(path, "rb");
    if (!f) {
        error.set("Could not open the image");
        return false;
    }
    size_t size = size_t(st.st_size);
    char *data = static_cast<char *>(malloc(size));
    bool read = data && fread(data, 1, size, f) == size;
    if (fclose(f))
        read = false;
    if (!read) {
        free(data);
        error.set("Could not read the image");
        return false;
    }
    const unsigned char *b = reinterpret_cast<unsigned char *>(data);
    auto le16 = [b](size_t p) { return unsigned(b[p]) | (unsigned(b[p + 1]) << 8); };
    auto le32 = [b](size_t p) {
        return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) |
               (uint32_t(b[p + 3]) << 24);
    };
    bool ok = size >= 54 && b[0] == 'B' && b[1] == 'M' && le32(14) >= 40 && le32(14) <= size - 14 &&
              le16(26) == 1 && (le16(28) == 24 || le16(28) == 32) && le32(30) == 0;
    int32_t width = ok ? int32_t(le32(18)) : 0, height = ok ? int32_t(le32(22)) : 0;
    uint32_t h = height < 0 ? uint32_t(-int64_t(height)) : uint32_t(height);
    size_t depth = ok ? le16(28) / 8 : 0;
    if (!ok || width <= 0 || width > 4096 || !h || h > 4096 || uint64_t(width) * h * 4 > MaxAssetBytes) {
        free(data);
        error.set("Use PNG or an uncompressed 24/32-bit BMP image");
        return false;
    }
    size_t stride = (size_t(width) * depth + 3) & ~size_t(3), offset = le32(10);
    if (offset < 14 + le32(14) || offset > size || stride * h > size - offset) {
        free(data);
        error.set("The BMP image is truncated");
        return false;
    }
    size_t pixels = size_t(width) * h * 4;
    char *rgba = static_cast<char *>(malloc(pixels));
    if (!rgba) {
        free(data);
        error.set("Not enough memory");
        return false;
    }
    for (uint32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < width; ++x) {
            size_t src = offset + (height > 0 ? h - y - 1 : y) * stride + size_t(x) * depth,
                   dst = (size_t(y) * width + x) * 4;
            rgba[dst] = b[src + 2];
            rgba[dst + 1] = b[src + 1];
            rgba[dst + 2] = b[src];
            rgba[dst + 3] = char(255);
        }
    Asset next;
    next.width = uint32_t(width);
    next.height = h;
    ok = next.rgba.set(rgba, pixels);
    free(rgba);
    free(data);
    if (ok)
        out = static_cast<Asset &&>(next);
    else
        error.set("Not enough memory");
    return ok;
}
} // namespace rp
