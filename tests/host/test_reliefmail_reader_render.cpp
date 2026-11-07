#include "reader_render.h"
#include "reader_presentation.h"

#include <assert.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rmail;

static int notice_translation_calls;

static const char *probe_notice_translation(const char *message) {
    ++notice_translation_calls;
    if (strcmp(message,
            "Only headers are cached; the complete original and body are unavailable offline.") == 0)
        return "仅缓存了邮件头；离线时无法获取完整原始邮件和正文。";
    if (strcmp(message, "Some message content could not be fully interpreted.") == 0)
        return "部分邮件内容无法完全解析。";
    return "unexpected translation";
}

static const char *missing_notice_translation(const char *) {
    ++notice_translation_calls;
    return nullptr;
}

static const char *empty_notice_translation(const char *) {
    ++notice_translation_calls;
    return "";
}

static void test_reader_localizes_known_notice_and_preserves_unknown_warning() {
    const char known[] =
        "Only headers are cached; the complete original and body are unavailable offline.";
    notice_translation_calls = 0;
    assert(strcmp(reader_notice_for_display(known, probe_notice_translation),
        "仅缓存了邮件头；离线时无法获取完整原始邮件和正文。") == 0);
    assert(notice_translation_calls == 1);

    const char unknown[] = "A future worker warning remains in its original wording.";
    notice_translation_calls = 0;
    const char *display = reader_notice_for_display(unknown, probe_notice_translation);
    assert(display == unknown);
    assert(strcmp(display, unknown) == 0);
    assert(notice_translation_calls == 0);
}

static void test_reader_localizes_fixed_mime_notice() {
    const char notice[] = "Some message content could not be fully interpreted.";
    notice_translation_calls = 0;
    assert(strcmp(reader_notice_for_display(notice, probe_notice_translation),
        "部分邮件内容无法完全解析。") == 0);
    assert(notice_translation_calls == 1);
}

static void test_reader_preserves_notice_when_translation_is_unavailable() {
    const char *notices[] = {
        "Only headers are cached; the complete original and body are unavailable offline.",
        "Some message content could not be fully interpreted."
    };
    for (const char *notice : notices) {
        notice_translation_calls = 0;
        assert(reader_notice_for_display(notice, nullptr) == notice);
        assert(notice_translation_calls == 0);
        assert(reader_notice_for_display(notice, missing_notice_translation) == notice);
        assert(notice_translation_calls == 1);
        assert(reader_notice_for_display(notice, empty_notice_translation) == notice);
        assert(notice_translation_calls == 2);
    }
}

static void test_reader_preserves_null_empty_and_unrecognized_notices_verbatim() {
    notice_translation_calls = 0;
    assert(reader_notice_for_display(nullptr, probe_notice_translation) == nullptr);
    const char *notices[] = {
        "",
        "Some message content could not be fully interpreted.\n",
        "Some message content could not be fully interpreted. Extra detail: \xff\r\n",
        "未知提示：保留原文\t\r\n"
    };
    for (const char *notice : notices)
        assert(reader_notice_for_display(notice, probe_notice_translation) == notice);
    assert(notice_translation_calls == 0);
}

static Bytes make_png(uint32_t width, uint32_t height) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    const size_t pixel_count = static_cast<size_t>(width) * height;
    unsigned char *pixels = static_cast<unsigned char *>(malloc(pixel_count * 4));
    assert(pixels);
    for (size_t i = 0; i < pixel_count; ++i) {
        pixels[i * 4] = 0x22;
        pixels[i * 4 + 1] = 0x88;
        pixels[i * 4 + 2] = 0xcc;
        pixels[i * 4 + 3] = 0x80;
    }
    png_alloc_size_t size = 0;
    assert(png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr));
    unsigned char *encoded = static_cast<unsigned char *>(malloc(size));
    assert(encoded);
    assert(png_image_write_to_memory(&image, encoded, &size, 0, pixels, 0, nullptr));
    Bytes result;
    assert(result.set(encoded, size));
    free(encoded);
    free(pixels);
    png_image_free(&image);
    return result;
}

static void test_rich_html_rewrites_links_and_renders_only_current_small_cid_png() {
    const char html[] =
        "<p><b>中文段落</b> <a href=\"https://mail.example.test/check?a=1&amp;b=2\">安全链接</a></p>"
        "<table><tr><td>单元格</td><td>第二列</td></tr></table>"
        "<img src=\"cid:logo%40mail.example.test\" alt=\"logo\">"
        "<img src=\"http://10.0.0.1/pixel.png\" alt=\"remote\">"
        "<a href=\"file:///etc/passwd\">本地路径</a>";
    AttachmentList attachments;
    Attachment image;
    assert(image.content_type.set("image/png"));
    assert(image.content_id.set("<logo@mail.example.test>"));
    image.inline_part = true;
    image.data = make_png(1, 1);
    assert(attachments.push(static_cast<Attachment &&>(image)));

    ReaderRenderedDocument document;
    Text error;
    assert(render_safe_reader_html(html, sizeof(html) - 1, attachments, document, error));
    assert(error.empty());
    assert(document.html.size() < MaxReaderHtmlBytes);
    assert(strstr(document.html.c_str(), "<b>中文段落</b>") != nullptr);
    assert(strstr(document.html.c_str(), "<table>") != nullptr);
    assert(strstr(document.html.c_str(), "安全链接") != nullptr);
    assert(strstr(document.html.c_str(), "href=\"rmail-link://0\"") != nullptr);
    assert(strstr(document.html.c_str(), "rmail-cid://0") != nullptr);
    assert(strstr(document.html.c_str(), "https://mail.example.test") == nullptr);
    assert(strstr(document.html.c_str(), "file:///etc/passwd") == nullptr);
    assert(strstr(document.html.c_str(), "10.0.0.1") == nullptr);
    assert(document.links.size() == 1);
    assert(document.links[0] == "https://mail.example.test/check?a=1&b=2");
    assert(document.images.size() == 1);
    assert(document.images[0].width == 1 && document.images[0].height == 1);
    assert(!document.images[0].xpm.empty());
    if (!validate_reader_xpm(document.images[0].xpm.c_str(), document.images[0].xpm.size(),
            document.images[0].width, document.images[0].height, error)) {
        fprintf(stderr, "XPM validation failed: %s\n%s", error.c_str(), document.images[0].xpm.c_str());
        assert(false);
    }
    assert(!document.warning.empty());
}

static void test_reader_html_drops_unmatched_noninline_oversized_and_bad_png_resources() {
    const char html[] =
        "<p>Fallback text</p><img src=\"cid:missing@example.test\" alt=\"missing\">"
        "<img src=\"cid:oversized@example.test\"><img src=\"cid:bad@example.test\">";
    AttachmentList attachments;
    Attachment oversized;
    assert(oversized.content_type.set("image/png"));
    assert(oversized.content_id.set("<oversized@example.test>"));
    oversized.inline_part = true;
    oversized.data = make_png(MaxReaderInlineImageDimension + 1, 1);
    assert(attachments.push(static_cast<Attachment &&>(oversized)));
    Attachment noninline;
    assert(noninline.content_type.set("image/png"));
    assert(noninline.content_id.set("<bad@example.test>"));
    noninline.inline_part = false;
    const unsigned char invalid_png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    assert(noninline.data.set(invalid_png, sizeof(invalid_png)));
    assert(attachments.push(static_cast<Attachment &&>(noninline)));

    ReaderRenderedDocument document;
    Text error;
    assert(render_safe_reader_html(html, sizeof(html) - 1, attachments, document, error));
    assert(error.empty());
    assert(strstr(document.html.c_str(), "Fallback text") != nullptr);
    assert(strstr(document.html.c_str(), "<img") == nullptr);
    assert(document.images.empty());
    assert(!document.warning.empty());
}

static void test_xpm_validator_rejects_mismatched_dimensions_and_unterminated_input() {
    const char xpm[] = "/* XPM */\nstatic const char *rmail_image[] = {\n\"1 1 1 1\",\n\"A c #FFFFFF\",\n\"A\",\n};\n";
    Text error;
    assert(validate_reader_xpm(xpm, sizeof(xpm) - 1, 1, 1, error));
    assert(!validate_reader_xpm(xpm, sizeof(xpm) - 1, 2, 1, error));
    assert(!validate_reader_xpm(xpm, sizeof(xpm) - 2, 1, 1, error));
    assert(!validate_reader_xpm("\"999 999 1 1\"", 13, 1, 1, error));
}

int main() {
    test_reader_localizes_known_notice_and_preserves_unknown_warning();
    test_reader_localizes_fixed_mime_notice();
    test_reader_preserves_notice_when_translation_is_unavailable();
    test_reader_preserves_null_empty_and_unrecognized_notices_verbatim();
    test_rich_html_rewrites_links_and_renders_only_current_small_cid_png();
    test_reader_html_drops_unmatched_noninline_oversized_and_bad_png_resources();
    test_xpm_validator_rejects_mismatched_dimensions_and_unterminated_input();
    return 0;
}
