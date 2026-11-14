#include "data.h"
#include <limits.h>
#include <strings.h>

namespace clip {
size_t Entry::size() const
{
    size_t result = 0;
    for (const Payload &p : formats) result += p.bytes.size();
    return result;
}

const Payload *Entry::find(const char *target) const
{
    for (const Payload &p : formats) if (p.target == target) return &p;
    return nullptr;
}

bool Entry::clone(const Entry &source)
{
    Entry copy;
    copy.id = source.id;
    copy.created = source.created;
    for (const Payload &p : source.formats) if (!copy.formats.push(p)) return false;
    *this = static_cast<Entry &&>(copy);
    return true;
}

bool Entry::add(const char *target, const char *type, const void *bytes, size_t count)
{
    if (!target || !*target || strlen(target) > 255 || !type || !*type ||
        strlen(type) > 255 || formats.size() >= MaxFormats || find(target) ||
        count > MaxEntryBytes - size()) return false;
    Payload p;
    return p.target.set(target) && p.type.set(type) &&
        p.bytes.set(static_cast<const char *>(bytes), count) && formats.push(static_cast<Payload &&>(p));
}

bool same(const Entry &a, const Entry &b)
{
    if (a.formats.size() != b.formats.size()) return false;
    for (const Payload &p : a.formats) {
        const Payload *q = b.find(p.target.c_str());
        if (!q || p.type != q->type || p.bytes != q->bytes) return false;
    }
    return true;
}

namespace {
bool append(Array<char> &bytes, const char *text, size_t size)
{
    size_t used = bytes.size();
    if (size > MaxEntryBytes - used || !bytes.resize(used + size)) return false;
    memcpy(bytes.data() + used, text, size);
    return true;
}

bool uri(Array<char> &bytes, const Text &path)
{
    if (path.empty() || path.c_str()[0] != '/' || memchr(path.c_str(), 0, path.size()) ||
        !append(bytes, "file://", 7)) return false;
    const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < path.size(); ++i) {
        unsigned char c = path.c_str()[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || strchr("/-._~", c)) {
            char value = c;
            if (!append(bytes, &value, 1)) return false;
        } else {
            char escaped[] = {'%', hex[c >> 4], hex[c & 15]};
            if (!append(bytes, escaped, 3)) return false;
        }
    }
    return true;
}

int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

bool local_uri(const char *value, size_t length, Text &path)
{
    if (length < 6 || strncasecmp(value, "file:", 5)) return false;
    size_t offset = 5;
    if (length >= 7 && value[5] == '/' && value[6] == '/') {
        offset = 7;
        size_t host = offset;
        while (offset < length && value[offset] != '/') ++offset;
        if (offset != host && (offset - host != 9 || strncasecmp(value + host, "localhost", 9)))
            return false;
    }
    if (offset == length || value[offset] != '/') return false;
    Array<char> decoded;
    for (; offset < length; ++offset) {
        unsigned char c = value[offset];
        if (c == '?' || c == '#' || c <= 32 || c == 127) return false;
        if (c == '%') {
            if (length - offset < 3) return false;
            int high = hex_digit(value[offset + 1]), low = hex_digit(value[offset + 2]);
            if (high < 0 || low < 0) return false;
            c = static_cast<unsigned char>(high * 16 + low);
            offset += 2;
        }
        if (!c || decoded.size() >= PATH_MAX - 1 || !decoded.push(char(c))) return false;
    }
    return path.set(decoded.data(), decoded.size());
}

bool file_payload(const Array<Text> &paths, bool cut, Entry &out)
{
    Array<char> uris, gnome, plain;
    if (!append(gnome, cut ? "cut\n" : "copy\n", cut ? 4 : 5)) return false;
    for (const Text &path : paths) {
        if (!uri(uris, path) || !append(uris, "\r\n", 2) ||
            !uri(gnome, path) || !append(gnome, "\n", 1) ||
            !append(plain, path.c_str(), path.size()) || !append(plain, "\n", 1)) return false;
    }
    Entry next;
    if (!next.add("text/uri-list", "text/uri-list", uris.data(), uris.size()) ||
        !next.add("x-special/gnome-copied-files", "x-special/gnome-copied-files", gnome.data(), gnome.size()) ||
        !next.add("application/x-kde-cutselection", "application/x-kde-cutselection", cut ? "1" : "0", 1) ||
        !next.add("UTF8_STRING", "UTF8_STRING", plain.data(), plain.size()))
        return false;
    out = static_cast<Entry &&>(next);
    return true;
}

bool parse_files(const Text &bytes, bool gnome, Array<Text> &out, bool &cut)
{
    if (bytes.size() > MaxEntryBytes || memchr(bytes.c_str(), 0, bytes.size())) return false;
    Array<Text> paths;
    bool move = false;
    size_t offset = 0;
    if (gnome) {
        if (bytes.size() >= 4 && !memcmp(bytes.c_str(), "cut\n", 4)) { move = true; offset = 4; }
        else if (bytes.size() >= 5 && !memcmp(bytes.c_str(), "copy\n", 5)) offset = 5;
        else return false;
    }
    while (offset < bytes.size()) {
        size_t end = offset;
        while (end < bytes.size() && bytes.c_str()[end] != '\n') ++end;
        size_t length = end - offset;
        if (length && bytes.c_str()[offset + length - 1] == '\r') --length;
        if (length && bytes.c_str()[offset] != '#') {
            Text path;
            if (!local_uri(bytes.c_str() + offset, length, path) || paths.size() >= 4096 ||
                !paths.push(static_cast<Text &&>(path))) return false;
        }
        offset = end + (end < bytes.size());
    }
    out.swap(paths);
    cut = move;
    return true;
}

Text preview(const Entry &entry)
{
    const char *targets[] = {"UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING",
        "text/uri-list", "text/html"};
    const Payload *source = nullptr;
    for (const char *target : targets) if ((source = entry.find(target))) break;
    Text result;
    if (source) {
        size_t length = source->bytes.size();
        if (length > 4096) length = 4096;
        while (length && length < source->bytes.size() &&
               (static_cast<unsigned char>(source->bytes.c_str()[length]) & 0xc0) == 0x80) --length;
        Array<char> text;
        if (!text.resize(length)) return result;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = source->bytes.c_str()[i];
            text[i] = c < 32 && c != '\n' && c != '\t' ? ' ' : char(c);
        }
        result.set(text.data(), text.size());
    } else if (!entry.formats.empty()) result = entry.formats[0].target;
    return result;
}

bool matches(const Entry &entry, const char *query)
{
    if (!query || !*query) return true;
    for (const Payload &p : entry.formats) {
        if (strcasestr(p.target.c_str(), query)) return true;
        bool text = p.target == "UTF8_STRING" || p.target == "STRING" || p.target == "TEXT" ||
            !strncmp(p.target.c_str(), "text/", 5);
        if (text && strcasestr(p.bytes.c_str(), query)) return true;
    }
    return false;
}
} // namespace clip
