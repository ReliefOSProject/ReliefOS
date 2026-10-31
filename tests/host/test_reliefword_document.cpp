#include "document.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char test_root[1200];

static void path(char *out, size_t capacity, const char *name) {
    int n = snprintf(out, capacity, "%s/%s", test_root, name);
    assert(n > 0 && (size_t)n < capacity);
}

static void write_bytes(const char *name, const void *bytes, size_t length) {
    char target[1400];
    path(target, sizeof(target), name);
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const unsigned char *at = (const unsigned char *)bytes;
    size_t left = length;
    while (left) {
        ssize_t count = write(fd, at, left);
        assert(count > 0);
        at += (size_t)count;
        left -= (size_t)count;
    }
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

static size_t read_bytes(const char *name, unsigned char *bytes, size_t capacity) {
    char target[1400];
    path(target, sizeof(target), name);
    int fd = open(target, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size >= 0 && (uint64_t)st.st_size <= capacity);
    size_t expected = (size_t)st.st_size, offset = 0;
    while (offset < expected) {
        ssize_t count = read(fd, bytes + offset, expected - offset);
        assert(count > 0);
        offset += (size_t)count;
    }
    assert(close(fd) == 0);
    return expected;
}

static bool same_file(const char *name, const void *expected, size_t length) {
    unsigned char bytes[4096];
    size_t actual = read_bytes(name, bytes, sizeof(bytes));
    return actual == length && !memcmp(bytes, expected, length);
}

static bool has_temp_file(const char *prefix) {
    DIR *directory = opendir(test_root);
    assert(directory);
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (!strncmp(entry->d_name, prefix, strlen(prefix))) found = true;
    assert(closedir(directory) == 0);
    return found;
}

static rw::ParagraphStyle make_paragraph_style() {
    rw::ParagraphStyle style;
    style.alignment = rw::Alignment::Justify;
    style.list = rw::ListKind::Number;
    style.left_indent = 23;
    style.right_indent = 11;
    style.first_indent = -7;
    style.before = 4;
    style.after = 9;
    style.line_spacing = 135;
    return style;
}

static rw::TextStyle make_text_style() {
    rw::TextStyle style;
    style.foreground = 0x2456a8;
    style.background = 0xf2df81;
    style.size = 17;
    style.family = rw::FontFamily::Sans;
    style.bold = 1;
    style.italic = 1;
    style.underline = 1;
    style.strike = 1;
    style.highlight = 1;
    return style;
}

static void make_document(rw::Document &document) {
    assert(document.init());
    assert(document.title.set("ReliefWord 标题", strlen("ReliefWord 标题")));
    assert(document.author.set("作者", strlen("作者")));
    assert(document.page.header.set("页眉", strlen("页眉")));
    assert(document.page.footer.set("Page 页脚", strlen("Page 页脚")));
    document.page.width = 612;
    document.page.height = 792;
    document.page.left = 48;
    document.page.right = 51;
    document.page.top = 63;
    document.page.bottom = 57;
    document.page.page_numbers = false;

    rw::TextStyle styled = make_text_style();
    assert(document.styles.push(styled));
    rw::ParagraphStyle paragraph_style = make_paragraph_style();
    document.paragraphs[0].style = paragraph_style;

    const uint32_t points[] = {'A', 0x4f60, 'e', 0x0301, '\n', 0x7b2c, 0x4e8c, '!', '&', '<', '>', '\n'};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        uint16_t style = i < 4 ? 1 : 0;
        assert(document.chars.push({points[i], style}));
        if (points[i] == '\n') {
            rw::Paragraph next;
            next.start = i + 1;
            assert(document.paragraphs.push(next));
        }
    }
    rw::Text error;
    assert(document.validate(error));
}

static bool same_document(const rw::Document &a, const rw::Document &b) {
    if (a.revision != b.revision || !(a.title == b.title) || !(a.author == b.author) ||
        a.page.width != b.page.width || a.page.height != b.page.height ||
        a.page.left != b.page.left || a.page.right != b.page.right ||
        a.page.top != b.page.top || a.page.bottom != b.page.bottom ||
        a.page.page_numbers != b.page.page_numbers || !(a.page.header == b.page.header) ||
        !(a.page.footer == b.page.footer) || a.styles.size() != b.styles.size() ||
        a.chars.size() != b.chars.size() || a.paragraphs.size() != b.paragraphs.size())
        return false;
    for (size_t i = 0; i < a.styles.size(); ++i)
        if (!(a.styles[i] == b.styles[i])) return false;
    for (size_t i = 0; i < a.chars.size(); ++i)
        if (a.chars[i].codepoint != b.chars[i].codepoint || a.chars[i].style != b.chars[i].style)
            return false;
    for (size_t i = 0; i < a.paragraphs.size(); ++i)
        if (a.paragraphs[i].start != b.paragraphs[i].start ||
            !(a.paragraphs[i].style == b.paragraphs[i].style)) return false;
    return true;
}

static void expect_decode_failure_preserves(const unsigned char *bytes, size_t length,
                                            const rw::Document &sentinel) {
    rw::Document output;
    assert(output.clone_from(sentinel));
    rw::Text error;
    assert(!rw::decode_document((const char *)bytes, length, rw::FileFormat::Native,
                                output, error));
    assert(same_document(output, sentinel));
}

static uint32_t read_u32(const unsigned char *bytes, size_t at) {
    return (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1] << 8) |
        ((uint32_t)bytes[at + 2] << 16) | ((uint32_t)bytes[at + 3] << 24);
}

static void store_u32(unsigned char *bytes, size_t at, uint32_t value) {
    bytes[at] = (unsigned char)value;
    bytes[at + 1] = (unsigned char)(value >> 8);
    bytes[at + 2] = (unsigned char)(value >> 16);
    bytes[at + 3] = (unsigned char)(value >> 24);
}

static void store_u64(unsigned char *bytes, size_t at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = (unsigned char)(value >> (8 * i));
}

struct NativeOffsets { size_t title, style_count, first_style, char_count, first_char, paragraph_count, first_paragraph; };

static size_t skip_string(const unsigned char *bytes, size_t at) {
    return at + 4 + read_u32(bytes, at);
}

static NativeOffsets native_offsets(const unsigned char *bytes) {
    size_t at = 16 + 8;
    NativeOffsets offsets{};
    offsets.title = at + 4;
    at = skip_string(bytes, at);
    at = skip_string(bytes, at);
    at += 12 + 1;
    at = skip_string(bytes, at);
    at = skip_string(bytes, at);
    offsets.style_count = at;
    uint32_t styles = read_u32(bytes, at);
    offsets.first_style = at + 4;
    at += 4 + (size_t)styles * 16;
    offsets.char_count = at;
    uint32_t chars = read_u32(bytes, at);
    offsets.first_char = at + 4;
    at += 4 + (size_t)chars * 6;
    offsets.paragraph_count = at;
    offsets.first_paragraph = at + 4;
    return offsets;
}

static bool cancel_progress(void *, unsigned, unsigned) { return false; }

struct ProgressResult { unsigned calls = 0; };
static bool observe_progress(void *context, unsigned, unsigned) {
    ++static_cast<ProgressResult *>(context)->calls;
    return true;
}

static bool cancel_after_write_quantum(void *context, unsigned completed, unsigned total) {
    (void)total;
    ProgressResult *state = static_cast<ProgressResult *>(context);
    if (completed) { ++state->calls; return false; }
    return true;
}

static bool edit_target_during_save(void *context, unsigned completed, unsigned total) {
    const char *target = (const char *)context;
    if (completed && completed == total) {
        int fd = open(target, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const char external[] = "external edit";
        bool ok = write(fd, external, sizeof(external) - 1) == (ssize_t)(sizeof(external) - 1) &&
            ftruncate(fd, (off_t)(sizeof(external) - 1)) == 0 && fsync(fd) == 0;
        int close_result = close(fd);
        return ok && close_result == 0;
    }
    return true;
}

static void test_native_round_trip_and_strict_validation() {
    rw::Document source;
    make_document(source);
    rw::Text error, encoded;
    assert(rw::encode_document(source, rw::FileFormat::Native, encoded, error));
    assert(encoded.size() > 16 && !memcmp(encoded.c_str(), "RWOR", 4));
    assert((unsigned char)encoded.c_str()[4] == 1 && (unsigned char)encoded.c_str()[5] == 0);
    assert(read_u32((const unsigned char *)encoded.c_str(), 8) == (uint32_t)(encoded.size() - 16));

    rw::Document restored;
    assert(rw::decode_document(encoded.c_str(), encoded.size(), rw::FileFormat::Native,
                               restored, error));
    assert(same_document(source, restored));

    rw::Document sentinel;
    make_document(sentinel);
    assert(sentinel.title.set("keep", 4));
    const size_t size = encoded.size();
    unsigned char *damaged = (unsigned char *)malloc(size + 1);
    assert(damaged);
    memcpy(damaged, encoded.c_str(), size);
    damaged[0] = 'X';
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    damaged[4] = 2;
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    store_u64(damaged, 8, (uint64_t)(size - 16) + 1);
    expect_decode_failure_preserves(damaged, size, sentinel);
    expect_decode_failure_preserves((const unsigned char *)encoded.c_str(), size - 1, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    damaged[size] = 0;
    expect_decode_failure_preserves(damaged, size + 1, sentinel);

    NativeOffsets offsets = native_offsets((const unsigned char *)encoded.c_str());
    memcpy(damaged, encoded.c_str(), size);
    damaged[offsets.title] = 0xff;
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    store_u32(damaged, offsets.first_char, 0xd800);
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    damaged[offsets.first_char + 4] = 0xff;
    damaged[offsets.first_char + 5] = 0xff;
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    store_u32(damaged, offsets.char_count, UINT32_MAX);
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    damaged[offsets.first_style + 11] = 2;
    expect_decode_failure_preserves(damaged, size, sentinel);

    memcpy(damaged, encoded.c_str(), size);
    store_u32(damaged, offsets.first_paragraph, 1);
    expect_decode_failure_preserves(damaged, size, sentinel);

    const size_t oversized_length = 16 * 1024 * 1024 + 1;
    unsigned char *oversized = (unsigned char *)calloc(oversized_length, 1);
    assert(oversized);
    expect_decode_failure_preserves(oversized, oversized_length, sentinel);
    free(oversized);
    free(damaged);
}

static void test_text_bom_newline_normalization_and_export() {
    const unsigned char text[] = {0xef, 0xbb, 0xbf, 0xe4, 0xbd, 0xa0, '\r', '\n',
                                  0xe5, 0xa5, 0xbd, '\r', 'A'};
    rw::Document imported;
    rw::Text error, exported;
    assert(rw::decode_document((const char *)text, sizeof(text), rw::FileFormat::Text,
                               imported, error));
    assert(imported.paragraphs.size() == 3);
    assert(rw::document_text(imported, 0, imported.chars.size(), exported));
    assert(exported == "你\n好\nA");
    assert(rw::encode_document(imported, rw::FileFormat::Text, exported, error));
    assert(exported == "你\n好\nA");
}

static void test_html_formatted_text_self_round_trip_and_safe_subset() {
    rw::Document source;
    make_document(source);
    rw::Text error, html, warning;
    assert(rw::encode_document(source, rw::FileFormat::Html, html, error));
    assert(strstr(html.c_str(), "<html") && strstr(html.c_str(), "font-weight:bold"));
    assert(strstr(html.c_str(), "\xe4\xbd\xa0") != nullptr);
    assert(strstr(html.c_str(), "&amp;") && strstr(html.c_str(), "&lt;") && strstr(html.c_str(), "&gt;"));

    rw::Document restored;
    assert(rw::decode_document(html.c_str(), html.size(), rw::FileFormat::Html,
                               restored, error, &warning));
    assert(same_document(source, restored));

    const char unsafe[] = "<p>before</p><script>alert(1)</script><style>.x{}</style>"
        "<table><tr><td>cell</td></tr></table><img src='file:///etc/passwd'>"
        "<iframe>embedded fallback</iframe><p>after</p>";
    assert(rw::decode_document(unsafe, sizeof(unsafe) - 1, rw::FileFormat::Html,
                               restored, error, &warning));
    assert(!warning.empty());
    assert(rw::document_text(restored, 0, restored.chars.size(), html));
    assert(strstr(html.c_str(), "before") && strstr(html.c_str(), "cell") &&
           strstr(html.c_str(), "after"));
    assert(!strstr(html.c_str(), "alert") && !strstr(html.c_str(), "passwd") &&
           !strstr(html.c_str(), "embedded fallback"));

    const char formatted[] = "<h1>章 &amp; 标题</h1><p style=\"text-align:center;margin-left:18pt;"
        "font-size:20pt;color:#010203\"><b>粗</b><i>斜</i><u>线</u><s>删除</s></p>";
    assert(rw::decode_document(formatted, sizeof(formatted) - 1, rw::FileFormat::Html,
                               restored, error, &warning));
    assert(rw::document_text(restored, 0, restored.chars.size(), html));
    assert(html == "章 & 标题\n粗斜线删除");
    assert(restored.paragraphs.size() == 2);
    assert(restored.paragraphs[1].style.alignment == rw::Alignment::Center);
    assert(restored.paragraphs[1].style.left_indent == 18);
    assert(restored.style_at(0).size == 24 && restored.style_at(0).bold);
    assert(restored.style_at(7).size == 20 && restored.style_at(7).foreground == 0x010203);
    assert(restored.style_at(7).bold && restored.style_at(8).italic &&
           restored.style_at(9).underline && restored.style_at(10).strike &&
           restored.style_at(11).strike);
}

static void test_html_nested_transparent_block_has_no_phantom_paragraph() {
    const char html[] = "<div><p>Text</p></div>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "Text");
    assert(imported.paragraphs.size() == 1);
}

static void test_html_preformatted_whitespace_and_monospace() {
    const char html[] = "<pre>a\n  b</pre>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "a\n  b");
    assert(imported.style_at(0).family == rw::FontFamily::Mono);
    assert(imported.style_at(imported.chars.size() - 1).family == rw::FontFamily::Mono);
}

static void test_html_links_warn_but_preserve_visible_text() {
    const char html[] = "<p><a href=\"https://example.invalid/\">linked text</a></p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "linked text");
    assert(strstr(warning.c_str(), "link") != nullptr);
}

static void test_html_preserves_leading_empty_paragraph() {
    const char html[] = "<p></p><p>Text</p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "\nText");
    assert(imported.paragraphs.size() == 2);
}

static void test_html_preserves_middle_empty_paragraph() {
    const char html[] = "<p>one</p><p></p><p>three</p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "one\n\nthree");
    assert(imported.paragraphs.size() == 3);
}

static void test_html_preserves_consecutive_trailing_empty_paragraphs() {
    const char html[] = "<p>Text</p><p></p><p></p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "Text\n\n");
    assert(imported.paragraphs.size() == 3);
}

static void test_html_br_only_paragraph_does_not_duplicate_boundary() {
    const char html[] = "<p><br></p><p>Text</p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "\nText");
    assert(imported.paragraphs.size() == 2);
}

static void test_html_trailing_br_does_not_duplicate_boundary() {
    const char html[] = "<p>A<br></p><p>B</p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "A\nB");
    assert(imported.paragraphs.size() == 2);
}

static void test_html_multiple_br_and_empty_paragraph_boundaries() {
    const char html[] = "<p>A<br><br></p><p></p><p>B</p>";
    rw::Document imported;
    rw::Text error, warning, text;
    assert(rw::decode_document(html, sizeof(html) - 1, rw::FileFormat::Html,
                               imported, error, &warning));
    assert(rw::document_text(imported, 0, imported.chars.size(), text));
    assert(text == "A\n\n\nB");
    assert(imported.paragraphs.size() == 4);
}

static void test_atomic_save_conflict_cancel_symlink_and_progress() {
    rw::Document document;
    make_document(document);
    rw::Text error;
    char target[1400];
    path(target, sizeof(target), "atomic.rword");
    const char original[] = "preserve original";
    write_bytes("atomic.rword", original, sizeof(original) - 1);

    rw::FileStamp stamp;
    assert(rw::file_stamp(target, stamp, error) && stamp.exists);
    const char external[] = "external data!!";
    write_bytes("atomic.rword", external, sizeof(external) - 1);
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error, &stamp));
    assert(same_file("atomic.rword", external, sizeof(external) - 1));

    const char before[] = "original bytes";
    const char after[] = "external bytes";
    assert(sizeof(before) == sizeof(after));
    write_bytes("same-stamp.rword", before, sizeof(before) - 1);
    path(target, sizeof(target), "same-stamp.rword");
    struct stat original_stat;
    assert(stat(target, &original_stat) == 0);
    assert(rw::file_stamp(target, stamp, error) && stamp.exists);
    write_bytes("same-stamp.rword", after, sizeof(after) - 1);
    struct timespec times[2];
#if defined(__APPLE__)
    times[0] = original_stat.st_atimespec;
    times[1] = original_stat.st_mtimespec;
#else
    times[0] = original_stat.st_atim;
    times[1] = original_stat.st_mtim;
#endif
    assert(utimensat(AT_FDCWD, target, times, 0) == 0);
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error, &stamp));
    assert(same_file("same-stamp.rword", after, sizeof(after) - 1));

    write_bytes("cancel.rword", original, sizeof(original) - 1);
    path(target, sizeof(target), "cancel.rword");
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error,
                              nullptr, cancel_progress));
    assert(same_file("cancel.rword", original, sizeof(original) - 1));
    ProgressResult partial{};
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error,
                              nullptr, cancel_after_write_quantum, &partial));
    assert(same_file("cancel.rword", original, sizeof(original) - 1));
    assert(!has_temp_file("cancel.rword.tmp."));

    path(target, sizeof(target), "raced.rword");
    write_bytes("raced.rword", original, sizeof(original) - 1);
    assert(rw::file_stamp(target, stamp, error));
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error,
                              &stamp, edit_target_during_save, target));
    assert(same_file("raced.rword", "external edit", strlen("external edit")));
    assert(!has_temp_file("raced.rword.tmp."));

    path(target, sizeof(target), "save-link.rword");
    write_bytes("linked-target.rword", original, sizeof(original) - 1);
    assert(symlink("linked-target.rword", target) == 0);
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error));
    assert(same_file("linked-target.rword", original, sizeof(original) - 1));

    ProgressResult observed{};
    path(target, sizeof(target), "progress.rword");
    assert(rw::save_document(target, document, rw::FileFormat::Native, error,
                             nullptr, observe_progress, &observed));
    assert(observed.calls > 0);

    write_bytes("existing.rword", original, sizeof(original) - 1);
    path(target, sizeof(target), "existing.rword");
    assert(rw::file_stamp(target, stamp, error) && stamp.exists);
    assert(rw::save_document(target, document, rw::FileFormat::Native, error, &stamp));
    assert(!has_temp_file("existing.rword.rollback."));
    rw::Document overwritten;
    assert(rw::load_document(target, rw::FileFormat::Native, overwritten, error));
    assert(same_document(document, overwritten));

    const size_t large_size = 128U * 1024U;
    unsigned char *large_original = (unsigned char *)malloc(large_size);
    unsigned char *large_readback = (unsigned char *)malloc(large_size);
    assert(large_original && large_readback);
    memset(large_original, 0x5a, large_size);
    write_bytes("large-existing.rword", large_original, large_size);
    path(target, sizeof(target), "large-existing.rword");
    assert(rw::file_stamp(target, stamp, error) && stamp.exists);
    ProgressResult backup_cancel{};
    assert(!rw::save_document(target, document, rw::FileFormat::Native, error,
                              &stamp, cancel_after_write_quantum, &backup_cancel));
    assert(error == "Operation cancelled" && backup_cancel.calls > 0);
    assert(read_bytes("large-existing.rword", large_readback, large_size) == large_size);
    assert(!memcmp(large_original, large_readback, large_size));
    assert(!has_temp_file("large-existing.rword.tmp."));
    assert(!has_temp_file("large-existing.rword.rollback."));
    free(large_original);
    free(large_readback);
}

static void test_load_file_round_trip_and_recovery_locks() {
    rw::Document source;
    make_document(source);
    rw::Text error;
    char target[1400];
    path(target, sizeof(target), "load.rword");
    assert(rw::save_document(target, source, rw::FileFormat::Native, error));
    rw::Document restored;
    ProgressResult observed{};
    assert(rw::load_document(target, rw::FileFormat::Native, restored, error,
                             nullptr, observe_progress, &observed));
    assert(observed.calls > 0 && same_document(source, restored));

    char link[1400];
    path(link, sizeof(link), "load-link.rword");
    assert(symlink("load.rword", link) == 0);
    rw::Document linked;
    assert(rw::load_document(link, rw::FileFormat::Native, linked, error));
    assert(same_document(source, linked));

    unsigned char native[4096];
    size_t native_size = read_bytes("load.rword", native, sizeof(native));
    write_bytes("bad-load.rword", native, native_size - 1);
    char bad_target[1400];
    path(bad_target, sizeof(bad_target), "bad-load.rword");
    rw::Document preserved;
    make_document(preserved);
    assert(preserved.title.set("preserve on failed load", strlen("preserve on failed load")));
    assert(!rw::load_document(bad_target, rw::FileFormat::Native,
                              preserved, error));
    assert(preserved.title == "preserve on failed load");

    const char *state_root = getenv("TMPDIR");
    assert(state_root && *state_root);
    char state_home[1400];
    int n = snprintf(state_home, sizeof(state_home), "%s/reliefword-state", state_root);
    assert(n > 0 && (size_t)n < sizeof(state_home));
    assert(setenv("XDG_STATE_HOME", state_home, 1) == 0);
    rw::Text recovery;
    assert(rw::recovery_directory(recovery, error));
    rw::Text first, second;
    assert(rw::reserve_recovery_path(first, error));
    assert(rw::reserve_recovery_path(second, error));
    assert(first != second && strstr(first.c_str(), ".rword"));
    int lock1 = -1, lock2 = -1;
    assert(rw::lock_recovery_path(first.c_str(), lock1, error));
    assert(!rw::lock_recovery_path(first.c_str(), lock2, error));
    assert(lock2 == -1);
    rw::unlock_recovery_path(lock1);
    assert(rw::lock_recovery_path(first.c_str(), lock2, error));
    rw::unlock_recovery_path(lock2);
}

int main(int argc, char **argv) {
    const char *tmp = getenv("TMPDIR");
    assert(tmp && *tmp);
    int n = snprintf(test_root, sizeof(test_root), "%s/reliefword-document-XXXXXX", tmp);
    assert(n > 0 && (size_t)n < sizeof(test_root));
    assert(mkdtemp(test_root));

    if (argc == 2) {
        if (!strcmp(argv[1], "html-nested-block"))
            test_html_nested_transparent_block_has_no_phantom_paragraph();
        else if (!strcmp(argv[1], "html-pre"))
            test_html_preformatted_whitespace_and_monospace();
        else if (!strcmp(argv[1], "html-links"))
            test_html_links_warn_but_preserve_visible_text();
        else if (!strcmp(argv[1], "html-empty-first"))
            test_html_preserves_leading_empty_paragraph();
        else if (!strcmp(argv[1], "html-empty-middle"))
            test_html_preserves_middle_empty_paragraph();
        else if (!strcmp(argv[1], "html-empty-trailing"))
            test_html_preserves_consecutive_trailing_empty_paragraphs();
        else if (!strcmp(argv[1], "html-br-only"))
            test_html_br_only_paragraph_does_not_duplicate_boundary();
        else if (!strcmp(argv[1], "html-br-text"))
            test_html_trailing_br_does_not_duplicate_boundary();
        else if (!strcmp(argv[1], "html-br-empty"))
            test_html_multiple_br_and_empty_paragraph_boundaries();
        else assert(!"unknown test case");
        return 0;
    }

    test_native_round_trip_and_strict_validation();
    test_text_bom_newline_normalization_and_export();
    test_html_formatted_text_self_round_trip_and_safe_subset();
    test_html_nested_transparent_block_has_no_phantom_paragraph();
    test_html_preformatted_whitespace_and_monospace();
    test_html_links_warn_but_preserve_visible_text();
    test_html_preserves_leading_empty_paragraph();
    test_html_preserves_middle_empty_paragraph();
    test_html_preserves_consecutive_trailing_empty_paragraphs();
    test_html_br_only_paragraph_does_not_duplicate_boundary();
    test_html_trailing_br_does_not_duplicate_boundary();
    test_html_multiple_br_and_empty_paragraph_boundaries();
    test_atomic_save_conflict_cancel_symlink_and_progress();
    test_load_file_round_trip_and_recovery_locks();
    return 0;
}
