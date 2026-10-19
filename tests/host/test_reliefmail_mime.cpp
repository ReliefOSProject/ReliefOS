#include "mime.h"
#include "html.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rmail;

static bool text_is(const Text &text, const char *expected) {
    const size_t length = strlen(expected);
    return text.size() == length && (!length || memcmp(text.c_str(), expected, length) == 0);
}

static bool bytes_are(const Bytes &bytes, const unsigned char *expected, size_t length) {
    return bytes.size() == length && (!length || memcmp(bytes.data(), expected, length) == 0);
}

static bool contains(const Text &text, const char *needle) {
    const size_t length = strlen(needle);
    if (length > text.size()) return false;
    for (size_t i = 0; i + length <= text.size(); ++i)
        if (!memcmp(text.c_str() + i, needle, length)) return true;
    return false;
}

static bool find_bytes(const Bytes &haystack, const char *needle, size_t length);

class TestBuffer {
    char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
public:
    TestBuffer() = default;
    TestBuffer(const TestBuffer &) = delete;
    TestBuffer &operator=(const TestBuffer &) = delete;
    TestBuffer(TestBuffer &&other) noexcept { swap(other); }
    TestBuffer &operator=(TestBuffer &&other) noexcept {
        if (this != &other) {
            free(bytes_);
            bytes_ = nullptr;
            size_ = 0;
            capacity_ = 0;
            swap(other);
        }
        return *this;
    }
    ~TestBuffer() { free(bytes_); }
    size_t size() const { return size_; }
    const char *data() const { return bytes_; }
    char *data() { return bytes_; }
    bool append(const char *bytes, size_t length) {
        if (length > SIZE_MAX - size_) return false;
        const size_t wanted = size_ + length;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < wanted) {
                if (next > SIZE_MAX / 2) { next = wanted; break; }
                next *= 2;
            }
            char *replacement = static_cast<char *>(realloc(bytes_, next));
            if (!replacement) return false;
            bytes_ = replacement;
            capacity_ = next;
        }
        if (length) memcpy(bytes_ + size_, bytes, length);
        size_ = wanted;
        return true;
    }
    bool repeat(char byte, size_t count) {
        if (count > SIZE_MAX - size_) return false;
        const size_t wanted = size_ + count;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < wanted) {
                if (next > SIZE_MAX / 2) { next = wanted; break; }
                next *= 2;
            }
            char *replacement = static_cast<char *>(realloc(bytes_, next));
            if (!replacement) return false;
            bytes_ = replacement;
            capacity_ = next;
        }
        if (count) memset(bytes_ + size_, byte, count);
        size_ = wanted;
        return true;
    }
    void swap(TestBuffer &other) {
        char *bytes = bytes_; bytes_ = other.bytes_; other.bytes_ = bytes;
        size_t size = size_; size_ = other.size_; other.size_ = size;
        size_t capacity = capacity_; capacity_ = other.capacity_; other.capacity_ = capacity;
    }
};

static void append_or_abort(Bytes &bytes, const void *part, size_t length) {
    const size_t old_size = bytes.size();
    unsigned char *joined = static_cast<unsigned char *>(malloc(old_size + length));
    assert(joined);
    if (old_size) memcpy(joined, bytes.data(), old_size);
    if (length) memcpy(joined + old_size, part, length);
    assert(bytes.set(joined, old_size + length));
    free(joined);
}

struct Capture {
    Bytes bytes;
    size_t calls = 0;
    size_t largest_write = 0;
    bool fail = false;
};

static bool capture_write(void *context, const unsigned char *data, size_t size) {
    Capture *capture = static_cast<Capture *>(context);
    ++capture->calls;
    if (size > capture->largest_write) capture->largest_write = size;
    if (capture->fail) return false;
    append_or_abort(capture->bytes, data, size);
    return true;
}

static const char mixed_message[] =
    "From: =?UTF-8?B?5byg5LiJ?= <zhang@example.cn>\r\n"
    "Reply-To: replies@example.cn\r\n"
    "To: reader@example.net\r\n"
    "Subject: =?UTF-8?B?5L2g?=\r\n =?UTF-8?B?5aW9?=\r\n"
    "Message-ID: <outer@example.cn>\r\n"
    "Content-Type: multipart/mixed; boundary=outer\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/alternative; boundary=alt\r\n\r\n"
    "--alt\r\n"
    "Content-Type: text/plain; charset=GB18030\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n\r\n"
    "=C4=E3=BA=C3\r\n"
    "--alt\r\n"
    "Content-Type: multipart/related; boundary=rel\r\n\r\n"
    "--rel\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n\r\n"
    "<p>Hello <img src=3D\"cid:logo@example.cn\"></p>\r\n"
    "--rel\r\n"
    "Content-Type: image/png\r\n"
    "Content-ID: <logo@example.cn>\r\n"
    "Content-Disposition: inline; filename=logo.png\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "AAEC/f4=\r\n"
    "--rel--\r\n"
    "--alt--\r\n"
    "--outer\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Disposition: attachment;\r\n"
    " filename*0*=UTF-8''%E6%8A%A5%E5%91%8A%20;\r\n"
    " filename*1*=%E6%96%87%E4%BB%B6.bin\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "AAEC/f4=\r\n"
    "--outer\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Disposition: attachment; filename=qp.bin\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n\r\n"
    "A=00B=FF=3D\r\n"
    "--outer\r\n"
    "Content-Type: message/rfc822\r\n\r\n"
    "Subject: nested\r\n"
    "Content-Type: multipart/mixed; boundary=inner\r\n\r\n"
    "--inner\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Disposition: attachment; filename=inside.bin\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "AQID\r\n"
    "--inner--\r\n"
    "--outer--\r\n";

static void test_parse_encoded_folded_headers_and_nested_mime_parts() {
    ParsedMessage parsed;
    Text error;
    assert(parse_message(mixed_message, sizeof(mixed_message) - 1, parsed, error));
    assert(error.empty());
    assert(text_is(parsed.from, "张三 <zhang@example.cn>"));
    assert(text_is(parsed.reply_to, "replies@example.cn"));
    assert(text_is(parsed.subject, "你好"));
    assert(text_is(parsed.message_id, "<outer@example.cn>"));
    assert(text_is(parsed.plain_text, "你好"));
    assert(contains(parsed.html_text, "cid:logo@example.cn"));
    assert(parsed.attachments.size() == 4);
    assert(text_is(parsed.attachments[0].filename, "logo.png"));
    assert(parsed.attachments[0].inline_part);
    assert(text_is(parsed.attachments[0].content_id, "logo@example.cn"));
    const unsigned char base64_data[] = {0, 1, 2, 0xfd, 0xfe};
    assert(bytes_are(parsed.attachments[0].data, base64_data, sizeof(base64_data)));
    assert(text_is(parsed.attachments[1].filename, "报告 文件.bin"));
    assert(bytes_are(parsed.attachments[1].data, base64_data, sizeof(base64_data)));
    const unsigned char qp_data[] = {'A', 0, 'B', 0xff, '='};
    assert(text_is(parsed.attachments[2].filename, "qp.bin"));
    assert(bytes_are(parsed.attachments[2].data, qp_data, sizeof(qp_data)));
    const unsigned char nested_data[] = {1, 2, 3};
    assert(text_is(parsed.attachments[3].filename, "inside.bin"));
    assert(bytes_are(parsed.attachments[3].data, nested_data, sizeof(nested_data)));
    assert(parsed.original_message.size() == sizeof(mixed_message) - 1);
    assert(!memcmp(parsed.original_message.data(), mixed_message, parsed.original_message.size()));
}

static TestBuffer folded_unknown_header_message(size_t header_size) {
    static const char prefix[] = "Received: from relay.example\r\n\t";
    static const char suffix[] = "\r\n\r\nx";
    static const size_t header_ending_size = 4;
    TestBuffer message;
    assert(header_size >= sizeof(prefix) - 1 + header_ending_size);
    assert(message.append(prefix, sizeof(prefix) - 1));
    assert(message.repeat('a', header_size - (sizeof(prefix) - 1) - header_ending_size));
    assert(message.append(suffix, sizeof(suffix) - 1));
    return message;
}

static void test_folded_unknown_headers_orphan_controls_and_bounds() {
    static const char received_and_dkim[] =
        "Received: from reliefmail.local (unknown [172.17.0.1])\r\n"
        "\tby mail.reliefmail.test (Postfix) with ESMTPSA id 33297C0C46F8\r\n"
        "\tfor <bob@reliefmail.test>; Sun, 18 Oct 2026 14:17:44 +0000 (UTC)\r\n"
        "DKIM-Signature: v=1; a=rsa-sha256; d=reliefmail.test;\r\n"
        "\ts=selector; h=from:to:subject:date; bh=abc123;\r\n"
        "\tb=def456\r\n"
        "From: alice@reliefmail.test\r\n"
        "Subject: folded transport headers survive\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n\r\n"
        "body\r\n";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(received_and_dkim, sizeof(received_and_dkim) - 1, parsed, error));
    assert(error.empty());
    assert(text_is(parsed.from, "alice@reliefmail.test"));
    assert(text_is(parsed.subject, "folded transport headers survive"));
    assert(text_is(parsed.plain_text, "body\r\n"));

    static const char orphan_fold[] =
        "\tby mail.reliefmail.test with ESMTPSA\r\n"
        "Subject: orphan continuation\r\n\r\nbody";
    assert(!parse_message(orphan_fold, sizeof(orphan_fold) - 1, parsed, error));
    assert(!error.empty());

    static const char control_in_fold[] =
        "Received: from relay.example\r\n"
        "\tby mail.example with ESMTP; invalid\x01fold\r\n"
        "Subject: control byte rejected\r\n\r\nbody";
    assert(!parse_message(control_in_fold, sizeof(control_in_fold) - 1, parsed, error));
    assert(!error.empty());

    TestBuffer exact = folded_unknown_header_message(MaxHeaderBytes);
    assert(parse_message(exact.data(), exact.size(), parsed, error));
    TestBuffer oversized = folded_unknown_header_message(MaxHeaderBytes + 1);
    assert(!parse_message(oversized.data(), oversized.size(), parsed, error));
    assert(!error.empty());
}

static void test_decode_modified_utf7_folder_names() {
    Text output;
    Text error;
    assert(decode_modified_utf7("INBOX/&ZeVnLIqe-", 16, output, error));
    assert(text_is(output, "INBOX/日本語"));
    assert(decode_modified_utf7("A&-B", 4, output, error));
    assert(text_is(output, "A&B"));
    assert(!decode_modified_utf7("&invalid-", 9, output, error));
    assert(text_is(output, "A&B"));
}

static void test_unknown_charset_and_missing_boundary_warn_without_losing_raw() {
    const char message[] =
        "Content-Type: multipart/mixed; boundary=\"missing\"\r\n\r\n"
        "orphan body without a delimiter\r\n";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(message, sizeof(message) - 1, parsed, error));
    assert(!parsed.warning.empty());
    assert(parsed.original_message.size() == sizeof(message) - 1);

    const char unknown[] =
        "Content-Type: text/plain; charset=x-not-a-real-charset\r\n\r\n"
        "hello \xff world";
    assert(parse_message(unknown, sizeof(unknown) - 1, parsed, error));
    assert(!parsed.warning.empty());
    assert(valid_utf8(parsed.plain_text.c_str(), parsed.plain_text.size()));
    assert(contains(parsed.plain_text, "hello "));
}

static void test_big5_charset_conversion() {
    const char message[] =
        "Content-Type: text/plain; charset=Big5\r\n\r\n"
        "\xa4\xa4\xa4\xe5";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(message, sizeof(message) - 1, parsed, error));
    assert(text_is(parsed.plain_text, "中文"));
    assert(parsed.warning.empty());
}

static void test_lossy_supported_charset_conversions_warn_and_preserve_original() {
    const char malformed_header[] =
        "Subject: =?UTF-8?Q?bad_=FF?=\r\n"
        "Content-Type: text/plain; charset=US-ASCII\r\n\r\n"
        "plain text";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(malformed_header, sizeof(malformed_header) - 1, parsed, error));
    assert(text_is(parsed.subject, "bad \xef\xbf\xbd"));
    assert(!parsed.warning.empty());
    assert(parsed.original_message.size() == sizeof(malformed_header) - 1);
    assert(!memcmp(parsed.original_message.data(), malformed_header, sizeof(malformed_header) - 1));

    const char malformed_body[] =
        "Content-Type: text/plain; charset=UTF-8\r\n\r\n"
        "body \xff damaged";
    assert(parse_message(malformed_body, sizeof(malformed_body) - 1, parsed, error));
    assert(text_is(parsed.plain_text, "body \xef\xbf\xbd damaged"));
    assert(!parsed.warning.empty());
    assert(parsed.original_message.size() == sizeof(malformed_body) - 1);
    assert(!memcmp(parsed.original_message.data(), malformed_body, sizeof(malformed_body) - 1));
}

static void append_wrapped_message(TestBuffer &output, const TestBuffer &inner, size_t index) {
    char boundary[24];
    const int length = snprintf(boundary, sizeof(boundary), "b%zu", index);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(boundary));
    assert(output.append("Content-Type: multipart/mixed; boundary=", strlen("Content-Type: multipart/mixed; boundary=")));
    assert(output.append(boundary, static_cast<size_t>(length)));
    assert(output.append("\r\n\r\n--", strlen("\r\n\r\n--")));
    assert(output.append(boundary, static_cast<size_t>(length)));
    assert(output.append("\r\n", 2));
    assert(output.append(inner.data(), inner.size()));
    assert(output.append("\r\n--", 4));
    assert(output.append(boundary, static_cast<size_t>(length)));
    assert(output.append("--\r\n", 4));
}

static TestBuffer nested_message(size_t depth) {
    TestBuffer current;
    assert(current.append("Content-Type: text/plain\r\n\r\nx", strlen("Content-Type: text/plain\r\n\r\nx")));
    for (size_t level = 1; level < depth; ++level) {
        TestBuffer parent;
        append_wrapped_message(parent, current, level);
        current.swap(parent);
    }
    return current;
}

static void test_exact_mime_depth_and_part_limits() {
    Text error;
    ParsedMessage parsed;
    TestBuffer depth_limit = nested_message(MaxMimeDepth);
    assert(parse_message(depth_limit.data(), depth_limit.size(), parsed, error));
    TestBuffer too_deep = nested_message(MaxMimeDepth + 1);
    assert(!parse_message(too_deep.data(), too_deep.size(), parsed, error));
    assert(!error.empty());

    const char prefix[] = "Content-Type: multipart/mixed; boundary=p\r\n\r\n--p\r\n";
    const char child[] = "Content-Type: text/plain\r\n\r\nx\r\n--p\r\n";
    const char last_child[] = "Content-Type: text/plain\r\n\r\nx\r\n--p--\r\n";
    for (size_t child_count = MaxMimeParts - 1; child_count <= MaxMimeParts; ++child_count) {
        TestBuffer message;
        assert(message.append(prefix, sizeof(prefix) - 1));
        for (size_t i = 0; i + 1 < child_count; ++i)
            assert(message.append(child, sizeof(child) - 1));
        assert(message.append(last_child, sizeof(last_child) - 1));
        const bool parsed_ok = parse_message(message.data(), message.size(), parsed, error);
        assert(parsed_ok == (child_count == MaxMimeParts - 1));
    }
}

static TestBuffer message_with_header_size(size_t header_size) {
    static const char prefix[] = "Content-Type: text/plain\r\nX-Filler: ";
    TestBuffer message;
    assert(header_size >= sizeof(prefix) - 1 + 4);
    assert(message.append(prefix, sizeof(prefix) - 1));
    assert(message.repeat('a', header_size - (sizeof(prefix) - 1) - 4));
    assert(message.append("\r\n\r\nx", 5));
    return message;
}

static void test_exact_aggregate_header_limit() {
    ParsedMessage parsed;
    Text error;
    TestBuffer exact = message_with_header_size(MaxHeaderBytes);
    assert(parse_message(exact.data(), exact.size(), parsed, error));
    TestBuffer oversized = message_with_header_size(MaxHeaderBytes + 1);
    assert(!parse_message(oversized.data(), oversized.size(), parsed, error));
    assert(!error.empty());
}

static void test_exact_raw_message_limit() {
    static const char header[] = "Content-Type: multipart/mixed; boundary=b\r\n\r\n";
    static const char suffix[] = "\r\n--b\r\nContent-Type: text/plain\r\n\r\nx\r\n--b--\r\n";
    const size_t input_size = MaxRawMessageBytes + 1;
    char *message = static_cast<char *>(malloc(input_size));
    assert(message);
    memcpy(message, header, sizeof(header) - 1);
    const size_t filler = MaxRawMessageBytes - (sizeof(header) - 1) - (sizeof(suffix) - 1);
    memset(message + sizeof(header) - 1, 'p', filler);
    memcpy(message + sizeof(header) - 1 + filler, suffix, sizeof(suffix) - 1);
    message[MaxRawMessageBytes] = 'e';

    ParsedMessage parsed;
    Text error;
    assert(parse_message(message, MaxRawMessageBytes, parsed, error));
    assert(parsed.original_message.size() == MaxRawMessageBytes);
    assert(!parse_message(message, input_size, parsed, error));
    assert(!error.empty());
    free(message);
}

static void test_large_readable_plain_body_is_not_limited_by_editable_text_cap() {
    static const char header[] =
        "From: reader@example.test\r\n"
        "Subject: large reader body\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n";
    const size_t body_size = MaxEditableTextBytes + 257;
    TestBuffer message;
    assert(message.append(header, sizeof(header) - 1));
    assert(message.repeat('x', body_size));

    ParsedMessage parsed;
    Text error;
    assert(parse_message(message.data(), message.size(), parsed, error));
    assert(parsed.plain_text.size() == body_size);
    assert(!memcmp(parsed.plain_text.c_str(), message.data() + sizeof(header) - 1, body_size));
}

static void test_html_sanitizer_keeps_safe_links_and_cid_only_images() {
    const char input[] =
        "<p onclick=\"evil()\">Hello<script>alert(1)</script>"
        "<a href=\"jav&#x61;script&colon;alert(2)\" onmouseover=\"x\">bad</a>"
        "<a href=\"https://safe.example/path?a=1&amp;b=2\">Safe link</a>"
        "<img src=\"https://remote.example/a.png\"><img src=\"file:///etc/passwd\">"
        "<img src=\"cid:logo%40id\" alt=\"logo\">"
        "</p><style>body{background:url(https://remote.example)}</style>"
        "<form action=\"https://remote.example\"><input value=\"secret\">hidden</form>"
        "<iframe src=\"https://remote.example\">frame text</iframe>";
    Text safe, warning, error;
    assert(sanitize_html(input, sizeof(input) - 1, safe, warning, error));
    assert(error.empty());
    assert(contains(safe, "Safe link"));
    assert(contains(safe, "https://safe.example/path?a=1&amp;b=2"));
    assert(contains(safe, "cid:logo@id"));
    assert(contains(safe, "alt=\"logo\""));
    assert(!contains(safe, "onclick"));
    assert(!contains(safe, "onmouseover"));
    assert(!contains(safe, "javascript"));
    assert(!contains(safe, "remote.example"));
    assert(!contains(safe, "file:///"));
    assert(!contains(safe, "<script"));
    assert(!contains(safe, "<style"));
    assert(!contains(safe, "<form"));
    assert(!contains(safe, "frame text"));
    assert(!warning.empty());
}

static void test_html_sanitizer_preserves_canonical_compose_formatting() {
    const char input[] =
        "<p style=\"text-align:center;margin-left:18pt;text-indent:-4pt\">"
        "<span style=\"font-family:SimSun;font-size:12pt;color:#1a2b3c\">中文段落</span>"
        "</p><ul style=\"margin-left:36pt\"><li>列表项</li></ul>"
        "<font face=\"SimSun\" size=\"4\" color=\"#1a2b3c\">旧式字体</font>";
    Text safe, warning, error;
    assert(sanitize_html(input, sizeof(input) - 1, safe, warning, error));
    assert(error.empty());
    assert(contains(safe, "text-align:center"));
    assert(contains(safe, "margin-left:18pt"));
    assert(contains(safe, "text-indent:-4pt"));
    assert(contains(safe, "font-family:SimSun"));
    assert(contains(safe, "font-size:12pt"));
    assert(contains(safe, "color:#1a2b3c"));
    assert(contains(safe, "margin-left:36pt"));
    assert(contains(safe, "<font face=\"SimSun\" size=\"4\" color=\"#1a2b3c\">"));
    assert(contains(safe, "中文段落"));
    assert(contains(safe, "列表项"));
}

static void test_reader_html_removes_every_image_and_keeps_only_safe_web_links() {
    const char input[] =
        "<p>Message <a href=\"https://safe.example/path\">open safely</a>"
        "<a href=\"file:///etc/passwd\">local path</a>"
        "<img src=\"cid:part@example.test\" alt=\"inline image\">"
        "<img src=\"https://remote.example/pixel.png\"></p>";
    Text safe, warning, error;
    assert(sanitize_html_for_reader(input, sizeof(input) - 1, safe, warning, error));
    assert(error.empty());
    assert(contains(safe, "open safely"));
    assert(contains(safe, "https://safe.example/path"));
    assert(!contains(safe, "<img"));
    assert(!contains(safe, "cid:"));
    assert(!contains(safe, "file:///"));
    assert(!contains(safe, "remote.example"));
    assert(!warning.empty());
}

static void test_parsed_html_is_sanitized_while_original_bytes_are_retained() {
    const char message[] =
        "Content-Type: text/html; charset=UTF-8\r\n\r\n"
        "<p>Visible<script>bad()</script><img src=\"file:///etc/passwd\"></p>";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(message, sizeof(message) - 1, parsed, error));
    assert(contains(parsed.html_text, "Visible"));
    assert(!contains(parsed.html_text, "<script"));
    assert(!contains(parsed.html_text, "bad()"));
    assert(!contains(parsed.html_text, "file:///"));
    assert(!parsed.warning.empty());
    assert(parsed.original_message.size() == sizeof(message) - 1);
    assert(find_bytes(parsed.original_message, "bad()", strlen("bad()")));
}

static void test_standard_html_document_wrappers_do_not_trigger_content_warning() {
    const char message[] =
        "Content-Type: text/html; charset=UTF-8\r\n\r\n"
        "<html><body><h2>Weekly report</h2><p>Readable message body.</p></body></html>";
    ParsedMessage parsed;
    Text error;
    assert(parse_message(message, sizeof(message) - 1, parsed, error));
    assert(error.empty());
    assert(contains(parsed.html_text, "<h2>Weekly report</h2>"));
    assert(contains(parsed.html_text, "<p>Readable message body.</p>"));
    assert(parsed.warning.empty());
    assert(parsed.original_message.size() == sizeof(message) - 1);
}

static void test_html_control_bytes_cannot_hide_uri_schemes_or_escape_text_storage() {
    const char input[] = "<a href=\"java&#10;script&colon;alert(1)\">bad</a><p>A\0B</p>";
    Text safe, warning, error;
    assert(sanitize_html(input, sizeof(input) - 1, safe, warning, error));
    assert(!contains(safe, "href="));
    assert(!contains(safe, "javascript"));
    assert(contains(safe, "A\xef\xbf\xbd" "B"));
    assert(!warning.empty());
    for (size_t i = 0; i < safe.size(); ++i) assert(safe.c_str()[i] != 0);
}

struct SourceReader {
    const Bytes *bytes = nullptr;
    size_t calls = 0;
    size_t largest_request = 0;
    size_t next_offset = 0;
};

static bool source_read(void *context, size_t offset, unsigned char *buffer,
                        size_t capacity, size_t *read_size) {
    SourceReader *reader = static_cast<SourceReader *>(context);
    const Bytes *source = reader->bytes;
    if (offset != reader->next_offset) return false;
    ++reader->calls;
    if (capacity > reader->largest_request) reader->largest_request = capacity;
    if (offset > source->size()) return false;
    size_t count = source->size() - offset;
    if (count > capacity) count = capacity;
    if (count) memcpy(buffer, source->data() + offset, count);
    *read_size = count;
    reader->next_offset += count;
    return true;
}

static Account make_account() {
    Account account;
    account.id = 9;
    assert(account.display_name.set("发件人"));
    assert(account.email_address.set("sender@example.com"));
    assert(account.username.set("sender"));
    assert(account.incoming_host.set("imap.example.com"));
    assert(account.outgoing_host.set("smtp.example.com"));
    return account;
}

static void add_address(Array<Address> &list, const char *name, const char *mailbox) {
    Array<Address> parsed;
    Text error;
    assert(parse_addresses(mailbox, parsed, error));
    if (name) assert(parsed[0].display_name.set(name));
    assert(list.push(static_cast<Address &&>(parsed[0])));
}

static Draft make_draft() {
    Draft draft;
    draft.account_id = 9;
    add_address(draft.to, "收件人", "to@example.net");
    add_address(draft.cc, nullptr, "copy@example.net");
    add_address(draft.bcc, nullptr, "hidden@example.net");
    assert(draft.subject.set("Interop 中文"));
    assert(draft.body.set("Plain text body"));
    assert(draft.html_body.set("<p>Rich body</p>"));
    draft.rich_format = true;
    assert(draft.message_id.set("<new@example.com>"));
    assert(draft.in_reply_to.set("<parent@example.com>"));
    assert(draft.references.set("<root@example.com> <parent@example.com>"));
    const unsigned char attachment_data[] = {0, 1, 2, 0xfd, 0xfe, '\r', '\n'};
    Attachment attachment;
    assert(attachment.filename.set("数据.bin"));
    assert(attachment.content_type.set("application/octet-stream"));
    assert(attachment.data.set(attachment_data, sizeof(attachment_data)));
    assert(draft.attachments.push(static_cast<Attachment &&>(attachment)));
    return draft;
}

static void test_outgoing_stream_and_bytes_wrapper_preserve_envelope_and_attachments() {
    Account account = make_account();
    Draft draft = make_draft();
    Text error;
    Envelope envelope;
    Bytes serialized;
    assert(encode_message(draft, account, serialized, envelope, error));
    assert(error.empty());
    assert(envelope.recipients.size() == 3);
    assert(text_is(envelope.sender.local_part, "sender"));

    assert(serialized.size() > 0);
    assert(!find_bytes(serialized, "Bcc:", strlen("Bcc:")));
    assert(find_bytes(serialized, "In-Reply-To: <parent@example.com>",
        strlen("In-Reply-To: <parent@example.com>")));
    assert(find_bytes(serialized, "References: <root@example.com> <parent@example.com>",
        strlen("References: <root@example.com> <parent@example.com>")));
    assert(find_bytes(serialized, "multipart/alternative", strlen("multipart/alternative")));

    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope streamed_envelope;
    assert(encode_message(draft, account, sink, streamed_envelope, error));
    assert(sink.bytes_written == capture.bytes.size());
    assert(capture.calls >= 1 && capture.largest_write <= 4096);
    ParsedMessage parsed;
    assert(parse_message(reinterpret_cast<const char *>(capture.bytes.data()),
        capture.bytes.size(), parsed, error));
    assert(text_is(parsed.subject, "Interop 中文"));
    assert(text_is(parsed.in_reply_to, "<parent@example.com>"));
    assert(text_is(parsed.references, "<root@example.com> <parent@example.com>"));
    assert(parsed.attachments.size() == 1);
    const unsigned char expected[] = {0, 1, 2, 0xfd, 0xfe, '\r', '\n'};
    assert(bytes_are(parsed.attachments[0].data, expected, sizeof(expected)));
}

static void test_streaming_sink_emits_large_body_in_bounded_chunks() {
    Account account = make_account();
    Draft draft;
    draft.account_id = account.id;
    add_address(draft.to, nullptr, "chunks@example.net");
    char body[9000];
    memset(body, 'x', sizeof(body));
    assert(draft.body.set(body, sizeof(body)));
    assert(draft.html_body.set("<p>alternative</p>"));
    draft.rich_format = true;

    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope envelope;
    Text error;
    assert(encode_message(draft, account, sink, envelope, error));
    assert(capture.calls >= 3);
    assert(capture.largest_write <= 4096);
    assert(sink.bytes_written == capture.bytes.size());
    ParsedMessage parsed;
    assert(parse_message(reinterpret_cast<const char *>(capture.bytes.data()),
        capture.bytes.size(), parsed, error));
    assert(parsed.plain_text.size() == sizeof(body));
    assert(!memcmp(parsed.plain_text.c_str(), body, sizeof(body)));
}

static void test_outgoing_rejects_header_injection_before_writing() {
    Account account = make_account();
    Draft draft = make_draft();
    assert(draft.in_reply_to.set("<ok@example.com>\r\nBcc: victim@example.net"));
    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope envelope;
    Text error;
    assert(!encode_message(draft, account, sink, envelope, error));
    assert(!error.empty());
    assert(capture.calls == 0 && sink.bytes_written == 0);
}

static void test_raw_recipient_source_rejects_invalid_and_empty_edits_without_stale_fallback() {
    Account account = make_account();
    Draft draft;
    draft.account_id = account.id;
    assert(draft.body.set("recipient source test"));
    add_address(draft.to, nullptr, "stale@example.net");
    Text error;
    const char invalid_to[] = "unfinished address";
    assert(set_raw_recipient_inputs(draft, invalid_to, sizeof(invalid_to) - 1,
        "", 0, "", 0, error));
    assert(draft.raw_recipient_input);
    assert(text_is(draft.to_input, "unfinished address"));
    assert(draft.to.size() == 1 && text_is(draft.to[0].local_part, "stale"));

    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope envelope;
    assert(!encode_message(draft, account, sink, envelope, error));
    assert(capture.calls == 0 && envelope.recipients.empty());

    assert(set_raw_recipient_inputs(draft, "", 0, "fresh@example.net",
        strlen("fresh@example.net"), "", 0, error));
    Capture fresh_capture;
    MimeSink fresh_sink;
    fresh_sink.write = capture_write;
    fresh_sink.context = &fresh_capture;
    Envelope fresh_envelope;
    assert(encode_message(draft, account, fresh_sink, fresh_envelope, error));
    assert(fresh_envelope.recipients.size() == 1);
    assert(text_is(fresh_envelope.recipients[0].local_part, "fresh"));
    assert(fresh_envelope.recipients[0].local_part != draft.to[0].local_part);
    assert(set_raw_recipient_inputs(draft, "", 0, "", 0, "", 0, error));
    Capture empty_capture;
    MimeSink empty_sink;
    empty_sink.write = capture_write;
    empty_sink.context = &empty_capture;
    Envelope empty_envelope;
    assert(!encode_message(draft, account, empty_sink, empty_envelope, error));
    assert(empty_capture.calls == 0 && empty_envelope.recipients.empty());
}

static void test_outgoing_stream_reads_file_attachment_by_bounded_offsets() {
    Account account = make_account();
    Draft draft;
    draft.account_id = account.id;
    assert(draft.body.set("stream"));
    add_address(draft.to, nullptr, "stream@example.net");
    unsigned char source_data[8197];
    for (size_t i = 0; i < sizeof(source_data); ++i)
        source_data[i] = static_cast<unsigned char>((i * 37) & 0xff);
    Bytes source_bytes;
    assert(source_bytes.set(source_data, sizeof(source_data)));
    SourceReader reader;
    reader.bytes = &source_bytes;
    Attachment attachment;
    assert(attachment.filename.set("stream.bin"));
    assert(attachment.content_type.set("application/octet-stream"));
    attachment.source.context = &reader;
    attachment.source.read = source_read;
    attachment.source.size = sizeof(source_data);
    assert(draft.attachments.push(static_cast<Attachment &&>(attachment)));

    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope envelope;
    Text error;
    assert(encode_message(draft, account, sink, envelope, error));
    assert(reader.calls >= 3);
    assert(reader.largest_request <= 3072);
    assert(reader.next_offset == sizeof(source_data));
    ParsedMessage parsed;
    assert(parse_message(reinterpret_cast<const char *>(capture.bytes.data()),
        capture.bytes.size(), parsed, error));
    assert(parsed.attachments.size() == 1);
    assert(bytes_are(parsed.attachments[0].data, source_data, sizeof(source_data)));
}

static bool find_bytes(const Bytes &haystack, const char *needle, size_t length) {
    if (length > haystack.size()) return false;
    for (size_t i = 0; i + length <= haystack.size(); ++i)
        if (!memcmp(haystack.data() + i, needle, length)) return true;
    return false;
}

static bool ascii_equal_folded(const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static size_t find_top_header_field(const Bytes &message, const char *name,
                                    const char **value, size_t *value_size) {
    size_t count = 0;
    size_t at = 0;
    while (at + 1 < message.size()) {
        size_t end = at;
        while (end + 1 < message.size() &&
            !(message.data()[end] == '\r' && message.data()[end + 1] == '\n')) ++end;
        if (end == at) break;
        const char *line = reinterpret_cast<const char *>(message.data() + at);
        const size_t line_size = end - at;
        const size_t name_size = strlen(name);
        if (line_size > name_size && line[name_size] == ':' &&
            ascii_equal_folded(line, name, name_size)) {
            ++count;
            if (value) *value = line + name_size + 1;
            if (value_size) *value_size = line_size - name_size - 1;
        }
        at = end + 2;
    }
    return count;
}

static bool two_digits(const char *value) {
    return value[0] >= '0' && value[0] <= '9' && value[1] >= '0' && value[1] <= '9';
}

static bool valid_rfc5322_date_value(const char *value, size_t size) {
    static const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!value || (size != 30 && size != 31) || value[3] != ',' || value[4] != ' ') return false;
    const bool one_digit_day = size == 30;
    if (one_digit_day) {
        if (value[5] < '0' || value[5] > '9' || value[6] != ' ') return false;
    } else if (!two_digits(value + 5) || value[7] != ' ') return false;
    const size_t shift = one_digit_day ? 1 : 0;
    const size_t month_at = 8 - shift;
    const size_t year_at = 12 - shift;
    const size_t time_at = 17 - shift;
    const size_t zone_at = 26 - shift;
    if (value[month_at + 3] != ' ' || value[year_at + 4] != ' ' ||
        value[time_at + 2] != ':' || value[time_at + 5] != ':' ||
        value[zone_at - 1] != ' ' || (value[zone_at] != '+' && value[zone_at] != '-')) return false;
    bool weekday = false, month = false;
    for (size_t i = 0; i < sizeof(weekdays) / sizeof(weekdays[0]); ++i)
        weekday |= !memcmp(value, weekdays[i], 3);
    for (size_t i = 0; i < sizeof(months) / sizeof(months[0]); ++i)
        month |= !memcmp(value + month_at, months[i], 3);
    if (!weekday || !month || !two_digits(value + time_at) ||
        !two_digits(value + time_at + 3) || !two_digits(value + time_at + 6) ||
        !two_digits(value + zone_at + 1) || !two_digits(value + zone_at + 3)) return false;
    for (size_t i = year_at; i < year_at + 4; ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    const unsigned day = static_cast<unsigned>(one_digit_day ? value[5] - '0' :
        (value[5] - '0') * 10 + value[6] - '0');
    const unsigned hour = static_cast<unsigned>((value[time_at] - '0') * 10 + value[time_at + 1] - '0');
    const unsigned minute = static_cast<unsigned>((value[time_at + 3] - '0') * 10 + value[time_at + 4] - '0');
    const unsigned second = static_cast<unsigned>((value[time_at + 6] - '0') * 10 + value[time_at + 7] - '0');
    const unsigned zone_hour = static_cast<unsigned>((value[zone_at + 1] - '0') * 10 + value[zone_at + 2] - '0');
    const unsigned zone_minute = static_cast<unsigned>((value[zone_at + 3] - '0') * 10 + value[zone_at + 4] - '0');
    return day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60 &&
        zone_hour <= 23 && zone_minute <= 59;
}

static size_t top_header_block_size(const Bytes &message) {
    for (size_t i = 0; i + 3 < message.size(); ++i)
        if (message.data()[i] == '\r' && message.data()[i + 1] == '\n' &&
            message.data()[i + 2] == '\r' && message.data()[i + 3] == '\n') return i + 4;
    return 0;
}

static void assert_one_valid_date(const Bytes &message) {
    const char *date_value = nullptr;
    size_t date_value_size = 0;
    assert(find_top_header_field(message, "Date", &date_value, &date_value_size) == 1);
    assert((date_value_size == 31 || date_value_size == 32) && date_value[0] == ' ');
    assert(valid_rfc5322_date_value(date_value + 1, date_value_size - 1));
}

static void test_outgoing_emits_exactly_one_valid_date() {
    Account account = make_account();
    Draft draft = make_draft();
    Bytes message;
    Envelope envelope;
    Text error;
    assert(encode_message(draft, account, message, envelope, error));
    assert_one_valid_date(message);

    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope streamed_envelope;
    assert(encode_message(draft, account, sink, streamed_envelope, error));
    assert(sink.bytes_written == capture.bytes.size());
    assert_one_valid_date(capture.bytes);
}

static void test_date_is_included_in_header_limit_preflight() {
    Account account = make_account();
    Draft draft;
    draft.account_id = account.id;
    assert(draft.body.set("header limit test"));
    assert(draft.message_id.set("<fixed@example.com>"));
    add_address(draft.to, nullptr, "to@example.net");
    Bytes baseline;
    Envelope baseline_envelope;
    Text error;
    assert(encode_message(draft, account, baseline, baseline_envelope, error));
    const size_t baseline_header_size = top_header_block_size(baseline);
    const char *baseline_date = nullptr;
    size_t baseline_date_value_size = 0;
    const size_t baseline_date_count = find_top_header_field(baseline, "Date",
        &baseline_date, &baseline_date_value_size);
    assert(baseline_date_count <= 1);
    const size_t date_line_size = baseline_date_count ? 7 + baseline_date_value_size : 0;
    assert(baseline_header_size > date_line_size && baseline_header_size < MaxHeaderBytes - 37);
    const size_t baseline_without_date = baseline_header_size - date_line_size;
    const size_t min_padding = MaxHeaderBytes - 37 - baseline_without_date;
    const size_t max_padding = MaxHeaderBytes - baseline_without_date;

    TestBuffer references;
    bool padding_found = false;
    size_t long_token_count = 0, final_token_size = 0;
    for (size_t count = 1; count < 512 && !padding_found; ++count) {
        const size_t fixed = count * 903 + 14;
        const size_t lower = min_padding > fixed ? min_padding - fixed : 5;
        const size_t upper = max_padding >= fixed ? max_padding - fixed : 0;
        const size_t candidate_lower = lower > 5 ? lower : 5;
        const size_t candidate_upper = upper < 900 ? upper : 900;
        if (candidate_lower <= candidate_upper) {
            long_token_count = count;
            final_token_size = candidate_lower;
            padding_found = true;
        }
    }
    assert(padding_found);
    const char *full_token = nullptr;
    char full_token_bytes[900];
    full_token_bytes[0] = '<';
    memset(full_token_bytes + 1, 'a', sizeof(full_token_bytes) - 2);
    full_token_bytes[450] = '@';
    full_token_bytes[sizeof(full_token_bytes) - 1] = '>';
    full_token = full_token_bytes;
    for (size_t i = 0; i < long_token_count; ++i) {
        if (i) assert(references.append(" ", 1));
        assert(references.append(full_token, sizeof(full_token_bytes)));
    }
    assert(references.append(" ", 1));
    char final_token[900];
    final_token[0] = '<';
    memset(final_token + 1, 'a', final_token_size - 2);
    final_token[final_token_size / 2] = '@';
    final_token[final_token_size - 1] = '>';
    assert(references.append(final_token, final_token_size));
    assert(draft.references.set(references.data(), references.size()));
    Capture capture;
    MimeSink sink;
    sink.write = capture_write;
    sink.context = &capture;
    Envelope envelope;
    assert(!encode_message(draft, account, sink, envelope, error));
    assert(capture.calls == 0 && sink.bytes_written == 0);
}

static void test_opaque_editor_state_is_ignored_when_sending() {
    Account account = make_account();
    Draft draft = make_draft();
    Text error;
    Envelope envelope;
    Bytes without_editor_state;
    assert(encode_message(draft, account, without_editor_state, envelope, error));

    const unsigned char editor_state[] = {
        'P','R','I','V','A','T','E',0,'E','D','I','T','O','R','-','S','T','A','T','E'
    };
    assert(draft.editor_state.set(editor_state, sizeof(editor_state)));
    Bytes with_editor_state;
    Envelope ignored_envelope;
    assert(encode_message(draft, account, with_editor_state, ignored_envelope, error));
    assert(with_editor_state.size() == without_editor_state.size());
    assert(!find_bytes(with_editor_state, reinterpret_cast<const char *>(editor_state), sizeof(editor_state)));
    assert(!find_bytes(with_editor_state, "PRIVATE", strlen("PRIVATE")));
    assert(!find_bytes(with_editor_state, "EDITOR-STATE", strlen("EDITOR-STATE")));
}

static bool emit_interop_message(const char *path) {
    Account account = make_account();
    Draft draft = make_draft();
    static const unsigned char original_eml[] =
        "From: forwarded@example.test\r\nSubject: Attached message\r\n\r\nOriginal bytes\r\n";
    Attachment eml;
    assert(eml.filename.set("forwarded.eml"));
    assert(eml.content_type.set("message/rfc822"));
    assert(eml.data.set(original_eml, sizeof(original_eml) - 1));
    assert(draft.attachments.push(static_cast<Attachment &&>(eml)));
    Bytes message;
    Envelope envelope;
    Text error;
    if (!encode_message(draft, account, message, envelope, error)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    const bool written = fwrite(message.data(), 1, message.size(), file) == message.size();
    const bool closed = fclose(file) == 0;
    return written && closed;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--emit-eml"))
        return emit_interop_message(argv[2]) ? 0 : 2;
    if (argc == 2 && !strcmp(argv[1], "--date-only")) {
        test_outgoing_emits_exactly_one_valid_date();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--date-header-limit")) {
        test_date_is_included_in_header_limit_preflight();
        return 0;
    }
    test_parse_encoded_folded_headers_and_nested_mime_parts();
    test_folded_unknown_headers_orphan_controls_and_bounds();
    test_decode_modified_utf7_folder_names();
    test_unknown_charset_and_missing_boundary_warn_without_losing_raw();
    test_big5_charset_conversion();
    test_lossy_supported_charset_conversions_warn_and_preserve_original();
    test_exact_mime_depth_and_part_limits();
    test_exact_aggregate_header_limit();
    test_exact_raw_message_limit();
    test_large_readable_plain_body_is_not_limited_by_editable_text_cap();
    test_html_sanitizer_keeps_safe_links_and_cid_only_images();
    test_html_sanitizer_preserves_canonical_compose_formatting();
    test_reader_html_removes_every_image_and_keeps_only_safe_web_links();
    test_parsed_html_is_sanitized_while_original_bytes_are_retained();
    test_standard_html_document_wrappers_do_not_trigger_content_warning();
    test_html_control_bytes_cannot_hide_uri_schemes_or_escape_text_storage();
    test_outgoing_stream_and_bytes_wrapper_preserve_envelope_and_attachments();
    test_streaming_sink_emits_large_body_in_bounded_chunks();
    test_outgoing_rejects_header_injection_before_writing();
    test_raw_recipient_source_rejects_invalid_and_empty_edits_without_stale_fallback();
    test_outgoing_stream_reads_file_attachment_by_bounded_offsets();
    test_opaque_editor_state_is_ignored_when_sending();
    test_outgoing_emits_exactly_one_valid_date();
    test_date_is_included_in_header_limit_preflight();
    return 0;
}
