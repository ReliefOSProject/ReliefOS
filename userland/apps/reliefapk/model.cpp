#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ra {
namespace {

constexpr size_t MaxJsonBytes = 64u * 1024u * 1024u;
constexpr size_t MaxStringBytes = 1024u * 1024u;
// Bound GUI callback scanning and the separate field-parse pass for one object.
constexpr size_t MaxStepBytes = 64u * 1024u;
constexpr size_t MaxPackageBytes = 1024u * 1024u;
constexpr size_t MaxPackages = 100000u;
constexpr unsigned MaxJsonDepth = 64u;
const char QueryFields[] = "name,version,description,installed-size,file-size";
const char InstalledQueryFields[] = "name,version,description,installed-size,file-size,status";

struct ParserData {
    const char *json = nullptr;
    size_t length = 0;
    size_t at = 0;
    bool first = true;
    bool array_started = false;
    bool expecting_package = true;
    bool can_close_array = true;
    bool array_closed = false;
    bool scanning_package = false;
    bool in_string = false;
    bool escaped = false;
    size_t package_start = 0;
    unsigned object_depth = 0;
    bool done = false;
    bool failed = false;
    Array<Package> packages;
    Text error;
};

struct ByteBuffer {
    char *bytes = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    ~ByteBuffer() { free(bytes); }
    bool append(const char *source, size_t length) {
        if (length > MaxStringBytes - size) return false;
        const size_t needed = size + length;
        if (needed > capacity) {
            size_t next = capacity ? capacity : 32;
            while (next < needed) {
                if (next > MaxStringBytes / 2) { next = MaxStringBytes; break; }
                next *= 2;
            }
            char *replacement = static_cast<char *>(malloc(next));
            if (!replacement) return false;
            if (size) memcpy(replacement, bytes, size);
            free(bytes);
            bytes = replacement;
            capacity = next;
        }
        if (length) memcpy(bytes + size, source, length);
        size = needed;
        return true;
    }
    bool append(char value) { return append(&value, 1); }
};

bool set_text(Text &out, const char *value) {
    return out.set(value, strlen(value));
}

bool parser_fail(ParserData &data, const char *message) {
    if (!data.failed) data.error.set(message, strlen(message));
    data.failed = true;
    data.done = false;
    data.packages.clear();
    return false;
}

void skip_space(ParserData &data) {
    while (data.at < data.length) {
        const char c = data.json[data.at];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++data.at;
    }
}

bool append_scalar(ByteBuffer &out, uint32_t scalar) {
    char bytes[4];
    size_t count;
    if (scalar <= 0x7f) { bytes[0] = static_cast<char>(scalar); count = 1; }
    else if (scalar <= 0x7ff) {
        bytes[0] = static_cast<char>(0xc0 | (scalar >> 6));
        bytes[1] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 2;
    } else if (scalar <= 0xffff) {
        bytes[0] = static_cast<char>(0xe0 | (scalar >> 12));
        bytes[1] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3f));
        bytes[2] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 3;
    } else if (scalar <= 0x10ffff) {
        bytes[0] = static_cast<char>(0xf0 | (scalar >> 18));
        bytes[1] = static_cast<char>(0x80 | ((scalar >> 12) & 0x3f));
        bytes[2] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3f));
        bytes[3] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 4;
    } else return false;
    return out.append(bytes, count);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_hex16(ParserData &data, uint16_t &value) {
    if (data.length - data.at < 4) return false;
    uint16_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int digit = hex_value(data.json[data.at++]);
        if (digit < 0) return false;
        result = static_cast<uint16_t>((result << 4) | static_cast<unsigned>(digit));
    }
    value = result;
    return true;
}

bool read_string(ParserData &data, Text &result) {
    if (data.at >= data.length || data.json[data.at++] != '"') return false;
    ByteBuffer decoded;
    while (data.at < data.length) {
        const unsigned char c = static_cast<unsigned char>(data.json[data.at++]);
        if (c == '"') return result.set(decoded.bytes, decoded.size);
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '\\') {
            if (data.at >= data.length) return false;
            const char escape = data.json[data.at++];
            if (escape == '"' || escape == '\\' || escape == '/') {
                if (!decoded.append(escape)) return false;
            } else if (escape == 'b' || escape == 'f' || escape == 'n' ||
                       escape == 'r' || escape == 't') {
                char value = escape == 'b' ? '\b' : escape == 'f' ? '\f' :
                    escape == 'n' ? '\n' : escape == 'r' ? '\r' : '\t';
                if (!decoded.append(value)) return false;
            } else if (escape == 'u') {
                uint16_t first = 0;
                if (!read_hex16(data, first)) return false;
                uint32_t scalar = first;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (data.length - data.at < 6 || data.json[data.at] != '\\' ||
                        data.json[data.at + 1] != 'u') return false;
                    data.at += 2;
                    uint16_t second = 0;
                    if (!read_hex16(data, second) || second < 0xdc00 || second > 0xdfff)
                        return false;
                    scalar = 0x10000u + ((static_cast<uint32_t>(first) - 0xd800u) << 10) +
                        (static_cast<uint32_t>(second) - 0xdc00u);
                } else if (first >= 0xdc00 && first <= 0xdfff) return false;
                if (!scalar || !append_scalar(decoded, scalar)) return false;
            } else return false;
            continue;
        }
        if (c < 0x80) {
            if (!decoded.append(static_cast<char>(c))) return false;
            continue;
        }

        const size_t start = data.at - 1;
        unsigned count = 0;
        uint32_t scalar = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 2; scalar = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; scalar = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; scalar = c & 0x07; }
        else return false;
        if (count - 1 > data.length - data.at) return false;
        for (unsigned i = 1; i < count; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(data.json[data.at++]);
            if ((continuation & 0xc0) != 0x80 ||
                (i == 1 && ((c == 0xe0 && continuation < 0xa0) ||
                            (c == 0xed && continuation > 0x9f) ||
                            (c == 0xf0 && continuation < 0x90) ||
                            (c == 0xf4 && continuation > 0x8f)))) return false;
            scalar = (scalar << 6) | (continuation & 0x3f);
        }
        if (scalar == 0 || scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
            !decoded.append(data.json + start, count)) return false;
    }
    return false;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool read_number(ParserData &data, bool unsigned_integer, uint64_t *integer) {
    size_t at = data.at;
    if (at >= data.length) return false;
    if (data.json[at] == '-') {
        if (unsigned_integer) return false;
        ++at;
        if (at >= data.length) return false;
    }
    if (data.json[at] == '0') {
        ++at;
        if (at < data.length && is_digit(data.json[at])) return false;
    } else {
        if (data.json[at] < '1' || data.json[at] > '9') return false;
        while (at < data.length && is_digit(data.json[at])) ++at;
    }
    uint64_t value = 0;
    if (unsigned_integer) {
        for (size_t i = data.at; i < at; ++i) {
            const unsigned digit = static_cast<unsigned>(data.json[i] - '0');
            if (value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
    }
    if (!unsigned_integer && at < data.length && data.json[at] == '.') {
        ++at;
        if (at >= data.length || !is_digit(data.json[at])) return false;
        while (at < data.length && is_digit(data.json[at])) ++at;
    }
    if (!unsigned_integer && at < data.length && (data.json[at] == 'e' || data.json[at] == 'E')) {
        ++at;
        if (at < data.length && (data.json[at] == '+' || data.json[at] == '-')) ++at;
        if (at >= data.length || !is_digit(data.json[at])) return false;
        while (at < data.length && is_digit(data.json[at])) ++at;
    }
    data.at = at;
    if (integer) *integer = value;
    return true;
}

bool skip_value(ParserData &data, unsigned depth);

bool skip_object(ParserData &data, unsigned depth) {
    if (depth > MaxJsonDepth || data.at >= data.length || data.json[data.at++] != '{') return false;
    skip_space(data);
    if (data.at < data.length && data.json[data.at] == '}') { ++data.at; return true; }
    for (;;) {
        Text key;
        if (!read_string(data, key)) return false;
        skip_space(data);
        if (data.at >= data.length || data.json[data.at++] != ':') return false;
        skip_space(data);
        if (!skip_value(data, depth + 1)) return false;
        skip_space(data);
        if (data.at >= data.length) return false;
        const char separator = data.json[data.at++];
        if (separator == '}') return true;
        if (separator != ',') return false;
        skip_space(data);
    }
}

bool skip_array(ParserData &data, unsigned depth) {
    if (depth > MaxJsonDepth || data.at >= data.length || data.json[data.at++] != '[') return false;
    skip_space(data);
    if (data.at < data.length && data.json[data.at] == ']') { ++data.at; return true; }
    for (;;) {
        if (!skip_value(data, depth + 1)) return false;
        skip_space(data);
        if (data.at >= data.length) return false;
        const char separator = data.json[data.at++];
        if (separator == ']') return true;
        if (separator != ',') return false;
        skip_space(data);
    }
}

bool skip_value(ParserData &data, unsigned depth) {
    if (depth > MaxJsonDepth || data.at >= data.length) return false;
    const char c = data.json[data.at];
    if (c == '"') { Text ignored; return read_string(data, ignored); }
    if (c == '{') return skip_object(data, depth);
    if (c == '[') return skip_array(data, depth);
    if (c == 't' && data.length - data.at >= 4 && !memcmp(data.json + data.at, "true", 4)) {
        data.at += 4; return true;
    }
    if (c == 'f' && data.length - data.at >= 5 && !memcmp(data.json + data.at, "false", 5)) {
        data.at += 5; return true;
    }
    if (c == 'n' && data.length - data.at >= 4 && !memcmp(data.json + data.at, "null", 4)) {
        data.at += 4; return true;
    }
    return read_number(data, false, nullptr);
}

bool parse_string_array(ParserData &data, Text &result) {
    if (data.at >= data.length || data.json[data.at++] != '[') return false;
    ByteBuffer joined;
    size_t count = 0;
    skip_space(data);
    if (data.at < data.length && data.json[data.at] == ']') {
        ++data.at;
        return result.set(nullptr, 0);
    }
    for (;;) {
        Text value;
        if (!read_string(data, value)) return false;
        if (count && !joined.append('\n')) return false;
        if (!joined.append(value.c_str(), value.size())) return false;
        ++count;
        skip_space(data);
        if (data.at >= data.length) return false;
        const char separator = data.json[data.at++];
        if (separator == ']') return result.set(joined.bytes, joined.size);
        if (separator != ',') return false;
        skip_space(data);
    }
}

bool parse_contents_object(ParserData &data, Text &result) {
    if (data.at >= data.length || data.json[data.at++] != '{') return false;
    ByteBuffer paths;
    size_t count = 0;
    skip_space(data);
    if (data.at < data.length && data.json[data.at] == '}') {
        ++data.at;
        return result.set(nullptr, 0);
    }
    for (;;) {
        Text path;
        if (!read_string(data, path)) return false;
        skip_space(data);
        if (data.at >= data.length || data.json[data.at++] != ':') return false;
        skip_space(data);
        if (!skip_value(data, 1)) return false;
        if (count && !paths.append('\n')) return false;
        if (!paths.append(path.c_str(), path.size())) return false;
        ++count;
        skip_space(data);
        if (data.at >= data.length) return false;
        const char separator = data.json[data.at++];
        if (separator == '}') return result.set(paths.bytes, paths.size);
        if (separator != ',') return false;
        skip_space(data);
    }
}

enum Field : unsigned {
    Name, Version, Description, Arch, License, Url, Origin, Depends,
    ReverseDepends, Provides, Repositories, Contents, InstalledSize, FileSize,
    Status, Installed, Unknown
};

Field field_for(const Text &key) {
    if (key == "name") return Name;
    if (key == "version") return Version;
    if (key == "description") return Description;
    if (key == "arch") return Arch;
    if (key == "license") return License;
    if (key == "url") return Url;
    if (key == "origin") return Origin;
    if (key == "depends") return Depends;
    if (key == "reverse-depends") return ReverseDepends;
    if (key == "provides") return Provides;
    if (key == "repositories") return Repositories;
    if (key == "contents") return Contents;
    if (key == "installed-size") return InstalledSize;
    if (key == "file-size") return FileSize;
    if (key == "status") return Status;
    if (key == "installed") return Installed;
    return Unknown;
}

bool status_has_installed(const Text &status) {
    const char *value = status.c_str();
    const size_t length = status.size();
    for (size_t at = 0; at < length;) {
        while (at < length && (value[at] == ' ' || value[at] == ',' || value[at] == '\n')) ++at;
        const size_t start = at;
        while (at < length && value[at] != ' ' && value[at] != ',' && value[at] != '\n') ++at;
        if (at - start == 9 && !memcmp(value + start, "installed", 9)) return true;
    }
    return false;
}

bool parse_package(ParserData &data, Package &package) {
    if (data.at >= data.length || data.json[data.at++] != '{') return false;
    uint32_t seen = 0;
    skip_space(data);
    if (data.at < data.length && data.json[data.at] == '}') {
        ++data.at;
        return false;
    }
    for (;;) {
        Text key;
        if (!read_string(data, key)) return false;
        skip_space(data);
        if (data.at >= data.length || data.json[data.at++] != ':') return false;
        skip_space(data);
        const Field field = field_for(key);
        if (field != Unknown) {
            const uint32_t bit = 1u << static_cast<unsigned>(field);
            if (seen & bit) return false;
            seen |= bit;
        }
        Text value;
        uint64_t number = 0;
        switch (field) {
        case Name:
        case Version:
        case Description:
        case Arch:
        case License:
        case Url:
        case Origin:
            if (!read_string(data, value)) return false;
            if (field == Name) package.name = static_cast<Text &&>(value);
            else if (field == Version) package.version = static_cast<Text &&>(value);
            else if (field == Description) package.description = static_cast<Text &&>(value);
            else if (field == Arch) package.arch = static_cast<Text &&>(value);
            else if (field == License) package.license = static_cast<Text &&>(value);
            else if (field == Url) package.url = static_cast<Text &&>(value);
            else package.origin = static_cast<Text &&>(value);
            break;
        case Status:
            if (data.at < data.length && data.json[data.at] == '[') {
                if (!parse_string_array(data, value)) return false;
            } else if (!read_string(data, value)) return false;
            package.installed = status_has_installed(value);
            break;
        case Depends:
            if (!parse_string_array(data, value)) return false;
            package.depends = static_cast<Text &&>(value);
            break;
        case ReverseDepends:
            if (!parse_string_array(data, value)) return false;
            package.reverse_depends = static_cast<Text &&>(value);
            break;
        case Provides:
            if (!parse_string_array(data, value)) return false;
            package.provides = static_cast<Text &&>(value);
            break;
        case Repositories:
            if (!parse_string_array(data, value)) return false;
            package.repositories = static_cast<Text &&>(value);
            break;
        case Contents:
            if (data.at < data.length && data.json[data.at] == '[') {
                if (!parse_string_array(data, value)) return false;
            } else if (data.at < data.length && data.json[data.at] == '{') {
                if (!parse_contents_object(data, value)) return false;
            } else return false;
            package.contents = static_cast<Text &&>(value);
            break;
        case InstalledSize:
            if (!read_number(data, true, &number)) return false;
            package.installed_size = number;
            break;
        case FileSize:
            if (!read_number(data, true, &number)) return false;
            package.file_size = number;
            break;
        case Installed:
            if (data.length - data.at >= 4 && !memcmp(data.json + data.at, "true", 4)) {
                data.at += 4; package.installed = true;
            } else if (data.length - data.at >= 5 && !memcmp(data.json + data.at, "false", 5)) {
                data.at += 5; package.installed = false;
            } else return false;
            break;
        case Unknown:
            if (!skip_value(data, 1)) return false;
            break;
        }
        skip_space(data);
        if (data.at >= data.length) return false;
        const char separator = data.json[data.at++];
        if (separator == '}') break;
        if (separator != ',') return false;
        skip_space(data);
    }
    return !package.name.empty() && !package.version.empty();
}

int compare_package_name(const Package &left, const Package &right) {
    return strcmp(left.name.c_str(), right.name.c_str());
}

void swap_packages(Package &left, Package &right) {
    if (&left == &right) return;
    Package temporary(static_cast<Package &&>(left));
    left = static_cast<Package &&>(right);
    right = static_cast<Package &&>(temporary);
}

void sift_packages(Array<Package> &packages, size_t root, size_t count) {
    for (;;) {
        const size_t child = root * 2 + 1;
        if (child >= count) return;
        size_t largest = child;
        if (child + 1 < count && compare_package_name(packages[child], packages[child + 1]) < 0)
            largest = child + 1;
        if (compare_package_name(packages[root], packages[largest]) >= 0) return;
        swap_packages(packages[root], packages[largest]);
        root = largest;
    }
}

void sort_packages_by_name(Array<Package> &packages) {
    const size_t count = packages.size();
    for (size_t start = count / 2; start > 0; --start)
        sift_packages(packages, start - 1, count);
    for (size_t end = count; end > 1; --end) {
        swap_packages(packages[0], packages[end - 1]);
        sift_packages(packages, 0, end - 1);
    }
}

bool close_document(ParserData &data) {
    sort_packages_by_name(data.packages);
    data.done = true;
    return true;
}

bool parse_scanned_package(ParserData &data, size_t end) {
    ParserData object;
    object.json = data.json;
    object.length = end;
    object.at = data.package_start;
    Package package;
    if (!parse_package(object, package) || object.at != end)
        return parser_fail(data, "invalid package object");
    if (data.packages.size() >= MaxPackages)
        return parser_fail(data, "package catalog exceeds limit");
    if (!data.packages.push(static_cast<Package &&>(package)))
        return parser_fail(data, "out of memory while building package catalog");
    data.scanning_package = false;
    data.expecting_package = false;
    data.can_close_array = true;
    data.first = false;
    return true;
}

bool append_arg(Array<Text> &args, const char *value) {
    Text arg;
    if (!arg.set(value)) return false;
    return args.push(static_cast<Text &&>(arg));
}

bool append_number(Array<Text> &args, unsigned value) {
    char buffer[32];
    const int count = snprintf(buffer, sizeof(buffer), "%u", value);
    return count > 0 && static_cast<size_t>(count) < sizeof(buffer) && append_arg(args, buffer);
}

bool append_global_options(Array<Text> &args, const Settings &settings) {
    if (!append_arg(args, "/sbin/apk")) return false;
    if (settings.offline && !append_arg(args, "--no-network")) return false;
    if (!append_arg(args, "--timeout") || !append_number(args, settings.timeout) ||
        !append_arg(args, "--cache-max-age") || !append_number(args, settings.cache_max_age) ||
        !append_arg(args, "--no-interactive") || !append_arg(args, "--no-progress")) return false;
    if (!settings.cache_dir.empty() &&
        (!append_arg(args, "--cache-dir") || !args.push(settings.cache_dir))) return false;
    return true;
}

void clear_command(Command &command) {
    command.args.clear();
    command.privileged = false;
    command.mutating = false;
    command.cancelable = true;
}

bool command_error(Command &command, Text &error, const char *message) {
    clear_command(command);
    error.set(message, strlen(message));
    return false;
}

bool is_local_apk(const char *value) {
    if (!value || value[0] != '/') return false;
    const size_t length = strlen(value);
    return length >= 5 && !strcmp(value + length - 4, ".apk");
}

bool path_valid(const char *value, bool require_absolute, bool require_apk) {
    if (!value || !*value) return false;
    const size_t length = strlen(value);
    if (length > 4096 || (require_absolute && value[0] != '/')) return false;
    if (require_apk && (length < 5 || strcmp(value + length - 4, ".apk"))) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool exact_c_string(const Text &value) {
    return strlen(value.c_str()) == value.size();
}

bool settings_valid(const Settings &settings) {
    return settings.timeout >= 1 && settings.timeout <= 3600 &&
        settings.cache_max_age <= 43200 &&
        (settings.cache_dir.empty() || (exact_c_string(settings.cache_dir) &&
            path_valid(settings.cache_dir.c_str(), true, false)));
}

bool ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool dependency_name_char(char c) {
    return ascii_alnum(c) || c == '_' || c == '.' || c == '+' || c == '-' ||
        c == ':' || c == '/' || c == '[' || c == ']' || c == ',';
}

bool dependency_tag_char(char c) {
    return ascii_alnum(c) || c == '_' || c == '.' || c == '+' || c == '-' ||
        c == ':' || c == '/' || c == '[' || c == ']' || c == ',' || c == '=';
}

bool dependency_version_char(char c) {
    return ascii_alnum(c) || c == '_' || c == '.' || c == '+' || c == '-' || c == '~';
}

bool identity_hash_char(char c) {
    return ascii_alnum(c) || c == '/' || c == '+' || c == '=';
}

bool valid_dependency(const char *value, size_t length) {
    size_t at = value[0] == '!' ? 1 : 0;
    if (at >= length || !ascii_alnum(value[at])) return false;
    size_t operator_at = length;
    for (size_t i = at; i < length; ++i) {
        if (value[i] == '<' || value[i] == '>' || value[i] == '=' || value[i] == '~') {
            operator_at = i;
            break;
        }
        if (value[i] == '@') continue;
        if (!dependency_name_char(value[i])) return false;
    }
    size_t tag_at = length;
    for (size_t i = at; i < operator_at; ++i) {
        if (value[i] == '@') {
            if (tag_at != length) return false;
            tag_at = i;
        }
    }
    if (tag_at == at || (tag_at != length && tag_at + 1 == operator_at)) return false;
    if (tag_at != length) {
        for (size_t i = tag_at + 1; i < operator_at; ++i)
            if (!dependency_tag_char(value[i])) return false;
    }
    const size_t name_end = tag_at == length ? operator_at : tag_at;
    if (name_end <= at || !ascii_alnum(value[at])) return false;
    if (operator_at == length) return true;

    size_t version_at = operator_at + 1;
    bool identity = false;
    const char op = value[operator_at];
    if (op == '<' || op == '>') {
        if (version_at < length && op == '>' && value[version_at] == '<') {
            identity = true;
            ++version_at;
        } else if (version_at < length && (value[version_at] == '=' || value[version_at] == '~')) {
            ++version_at;
        }
    }
    if (version_at >= length) return false;
    for (size_t i = version_at; i < length; ++i) {
        if (!(identity ? identity_hash_char(value[i]) : dependency_version_char(value[i]))) return false;
    }
    return true;
}

bool all_constraints(const Array<Text> &targets, size_t start, bool allow_local) {
    for (size_t i = start; i < targets.size(); ++i) {
        const char *value = targets[i].c_str();
        if (!exact_c_string(targets[i])) return false;
        if (allow_local && is_local_apk(value)) {
            if (!valid_constraint(value, true)) return false;
        } else if (!valid_constraint(value)) return false;
    }
    return true;
}

bool append_targets(Array<Text> &args, const Array<Text> &targets, size_t start) {
    if (start == targets.size()) return true;
    if (!append_arg(args, "--")) return false;
    for (size_t i = start; i < targets.size(); ++i) {
        if (!args.push(targets[i])) return false;
    }
    return true;
}

bool append_detail_query(Array<Text> &args, const char *fields, bool owner,
                         const Array<Text> &targets) {
    if (!append_arg(args, "query") || !append_arg(args, "--format") ||
        !append_arg(args, "json") || !append_arg(args, "--fields") ||
        !append_arg(args, fields)) return false;
    if (owner && (!append_arg(args, "--match") || !append_arg(args, "owner"))) return false;
    return append_targets(args, targets, 0);
}

bool complete_command(Command &command, Array<Text> &args, bool privileged, bool mutating,
                      bool cancelable) {
    command.args.swap(args);
    command.privileged = privileged;
    command.mutating = mutating;
    command.cancelable = cancelable;
    return true;
}

bool simulation_action(Action action) {
    return action == Action::Install || action == Action::Remove || action == Action::Upgrade ||
        action == Action::Reinstall || action == Action::Fix || action == Action::UpgradeAll;
}

bool package_action(Action action) {
    return action == Action::Install || action == Action::Remove || action == Action::Upgrade ||
        action == Action::Reinstall || action == Action::Policy || action == Action::Contents ||
        action == Action::Depends || action == Action::ReverseDepends || action == Action::Manifest ||
        action == Action::Dot;
}

}

struct PackageParser::State { ParserData data; };

PackageParser::~PackageParser() {
    if (state_) { state_->~State(); free(state_); }
}

bool PackageParser::begin(const char *json, size_t length) {
    fallback_error_.clear();
    if (state_) { state_->~State(); free(state_); state_ = nullptr; }
    void *memory = malloc(sizeof(State));
    if (!memory) { set_text(fallback_error_, "out of memory"); return false; }
    state_ = new (memory, Placement{}) State();
    ParserData &data = state_->data;
    if ((!json && length) || length > MaxJsonBytes) return parser_fail(data, "JSON input exceeds limits");
    data.json = json ? json : "";
    data.length = length;
    return true;
}

bool PackageParser::step(unsigned budget) {
    if (!state_) return false;
    ParserData &data = state_->data;
    if (data.failed) return false;
    if (data.done || !budget) return true;
    unsigned parsed = 0;
    const size_t start_at = data.at;
    while (data.at - start_at < MaxStepBytes) {
        if (data.scanning_package) {
            if (data.at >= data.length) return parser_fail(data, "truncated JSON package object");
            const char c = data.json[data.at++];
            if (data.in_string) {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (byte < 0x20) return parser_fail(data, "control byte in JSON package object");
                if (data.escaped) data.escaped = false;
                else if (c == '\\') data.escaped = true;
                else if (c == '"') data.in_string = false;
            } else if (c == '"') {
                data.in_string = true;
            } else if (c == '{') {
                if (++data.object_depth > MaxJsonDepth)
                    return parser_fail(data, "JSON package object exceeds depth limit");
            } else if (c == '}') {
                if (!data.object_depth) return parser_fail(data, "invalid JSON package object structure");
                if (!--data.object_depth) {
                    if (!parse_scanned_package(data, data.at)) return false;
                    ++parsed;
                    continue;
                }
            }
            if (data.at - data.package_start >= MaxPackageBytes)
                return parser_fail(data, "package object exceeds limit");
            continue;
        }

        if (data.at >= data.length) {
            if (data.array_closed) return close_document(data);
            return parser_fail(data, data.array_started ? "truncated JSON package array" :
                               "expected JSON package array");
        }
        const char c = data.json[data.at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++data.at;
            continue;
        }
        if (!data.array_started) {
            if (c != '[') return parser_fail(data, "expected JSON package array");
            ++data.at;
            data.array_started = true;
            continue;
        }
        if (data.array_closed) return parser_fail(data, "trailing data after JSON array");

        if (data.expecting_package) {
            if (parsed >= budget && !data.first) break;
            if (c == ']') {
                if (!data.can_close_array) return parser_fail(data, "trailing comma in package array");
                ++data.at;
                data.array_closed = true;
                continue;
            }
            if (c != '{') return parser_fail(data, "expected package object");
            data.scanning_package = true;
            data.package_start = data.at;
            data.object_depth = 0;
            data.in_string = false;
            data.escaped = false;
            continue;
        }

        if (c == ',') {
            ++data.at;
            data.expecting_package = true;
            data.can_close_array = false;
        } else if (c == ']') {
            ++data.at;
            data.array_closed = true;
        } else {
            return parser_fail(data, "expected comma or end of package array");
        }
    }

    if (data.array_closed && data.at == data.length) return close_document(data);
    return true;
}

bool PackageParser::done() const { return state_ && state_->data.done; }

const Text &PackageParser::error() const {
    if (state_ && !state_->data.error.empty()) return state_->data.error;
    return fallback_error_;
}

Array<Package> PackageParser::take() {
    Array<Package> result;
    if (state_ && state_->data.done && !state_->data.failed) result.swap(state_->data.packages);
    return result;
}

bool valid_constraint(const char *value, bool local) {
    if (!value || !*value) return false;
    const size_t length = strlen(value);
    if (local) return path_valid(value, true, true);
    if (length > 512) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c <= 0x20 || c >= 0x7f || c == ';' || c == '|' || c == '&' || c == '$' ||
            c == '`' || c == '\\' || c == '\'' || c == '"' || c == '(' || c == ')') return false;
    }
    return valid_dependency(value, length);
}

Text format_size(uint64_t bytes) {
    char buffer[64];
    if (bytes < 1024) {
        const int n = snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
        Text result;
        if (n > 0 && static_cast<size_t>(n) < sizeof(buffer)) result.set(buffer, static_cast<size_t>(n));
        return result;
    }
    static const char *units[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    uint64_t unit = 1024;
    unsigned index = 0;
    while (index + 1 < sizeof(units) / sizeof(units[0]) && bytes / unit >= 1024) {
        unit *= 1024;
        ++index;
    }
    uint64_t whole = bytes / unit;
    uint64_t decimal = ((bytes % unit) * 10 + unit / 2) / unit;
    if (decimal == 10) { ++whole; decimal = 0; }
    const int n = snprintf(buffer, sizeof(buffer), "%llu.%llu %s",
        static_cast<unsigned long long>(whole), static_cast<unsigned long long>(decimal), units[index]);
    Text result;
    if (n > 0 && static_cast<size_t>(n) < sizeof(buffer)) result.set(buffer, static_cast<size_t>(n));
    return result;
}

bool make_command(Action action, const Array<Text> &targets, const Settings &settings,
                  bool simulate, Command &command, Text &error) {
    clear_command(command);
    error.clear();
    if (simulate && !simulation_action(action))
        return command_error(command, error, "simulation is not supported for this action");
    if (action == Action::Keys)
        return command_error(command, error, "key management is handled by the GUI");
    if (!settings_valid(settings))
        return command_error(command, error, "invalid apk settings");

    if (package_action(action) && !all_constraints(targets, 0, action == Action::Install))
        return command_error(command, error, "invalid package constraint");
    if (action == Action::UpgradeAll && targets.size())
        return command_error(command, error, "upgrade all does not take package targets");
    if (action == Action::Fix && !all_constraints(targets, 0, false))
        return command_error(command, error, "invalid package constraint");
    if (action == Action::CacheDownload && !all_constraints(targets, 0, false))
        return command_error(command, error, "invalid package constraint");
    if (action == Action::Fetch) {
        if (targets.size() < 2 || !exact_c_string(targets[0]) ||
            !path_valid(targets[0].c_str(), true, false) ||
            !all_constraints(targets, 1, false))
            return command_error(command, error, "fetch requires an output directory and package constraints");
    }
    if (action == Action::Verify) {
        if (!targets.size()) return command_error(command, error, "verify requires local package files");
        for (size_t i = 0; i < targets.size(); ++i)
            if (!exact_c_string(targets[i]) || !path_valid(targets[i].c_str(), true, true))
                return command_error(command, error, "verify requires absolute .apk paths");
    }
    if (action == Action::Owner) {
        if (targets.size() != 1 || !exact_c_string(targets[0]) ||
            !path_valid(targets[0].c_str(), true, false))
            return command_error(command, error, "owner requires one absolute file path");
    }
    if (action == Action::Version) {
        if (targets.size() != 2 || !exact_c_string(targets[0]) || !exact_c_string(targets[1]) ||
            !valid_constraint(targets[0].c_str()) ||
            !valid_constraint(targets[1].c_str()))
            return command_error(command, error, "version comparison requires two version strings");
    }
    if ((action == Action::Install || action == Action::Remove || action == Action::Upgrade ||
         action == Action::Reinstall || action == Action::Policy || action == Action::Contents ||
         action == Action::Depends || action == Action::ReverseDepends || action == Action::Manifest) &&
        !targets.size()) return command_error(command, error, "action requires package targets");
    if (action == Action::Update || action == Action::CacheClean || action == Action::Audit ||
        action == Action::Stats || action == Action::World) {
        if (targets.size()) return command_error(command, error, "action does not take targets");
    }

    Array<Text> args;
    if (!append_global_options(args, settings)) return command_error(command, error, "out of memory");
    bool privileged = false, mutating = false;
    const char *name = nullptr;
    switch (action) {
    case Action::Install: name = "add"; privileged = mutating = true; break;
    case Action::Remove: name = "del"; privileged = mutating = true; break;
    case Action::Upgrade: name = "fix"; privileged = mutating = true; break;
    case Action::Reinstall: name = "fix"; privileged = mutating = true; break;
    case Action::Fix: name = "fix"; privileged = mutating = true; break;
    case Action::UpgradeAll: name = "upgrade"; privileged = mutating = true; break;
    case Action::Update: name = "update"; privileged = mutating = true; break;
    case Action::CacheClean: name = "cache"; privileged = mutating = true; break;
    case Action::CacheDownload: name = "cache"; privileged = mutating = true; break;
    case Action::Fetch: name = "fetch"; mutating = true; break;
    case Action::Verify: name = "verify"; break;
    case Action::Audit: name = "audit"; break;
    case Action::Stats: name = "stats"; break;
    case Action::Policy: name = "policy"; break;
    case Action::Contents: name = "query"; break;
    case Action::Depends: name = "query"; break;
    case Action::ReverseDepends: name = "query"; break;
    case Action::Owner: name = "query"; break;
    case Action::World: name = "query"; break;
    case Action::Version: name = "version"; break;
    case Action::Dot: name = "dot"; break;
    case Action::Manifest: name = "manifest"; break;
    case Action::Keys: return command_error(command, error, "key management is handled by the GUI");
    }
    if (!append_arg(args, name)) return command_error(command, error, "out of memory");

    bool okay = true;
    switch (action) {
    case Action::Install:
    case Action::Remove:
    case Action::Upgrade:
    case Action::Reinstall:
    case Action::Fix:
    case Action::UpgradeAll:
        if (action == Action::Upgrade) okay = append_arg(args, "--upgrade");
        else if (action == Action::Reinstall) okay = append_arg(args, "--reinstall");
        if (okay && simulate) okay = append_arg(args, "--simulate");
        if (okay && action == Action::Install) okay = all_constraints(targets, 0, true) && append_targets(args, targets, 0);
        else if (okay && action != Action::UpgradeAll) okay = append_targets(args, targets, 0);
        break;
    case Action::Update:
        break;
    case Action::CacheClean:
        okay = append_arg(args, "clean");
        break;
    case Action::CacheDownload:
        okay = append_arg(args, "download") && append_targets(args, targets, 0);
        break;
    case Action::Fetch:
        if (settings.fetch_dependencies) okay = append_arg(args, "--recursive");
        if (okay) okay = append_arg(args, "--output") && args.push(targets[0]) &&
            append_targets(args, targets, 1);
        break;
    case Action::Verify:
        okay = append_targets(args, targets, 0);
        break;
    case Action::Audit:
    case Action::Stats:
        break;
    case Action::Policy:
        okay = append_targets(args, targets, 0);
        break;
    case Action::Contents:
        okay = append_arg(args, "--format") && append_arg(args, "json") &&
            append_arg(args, "--fields") && append_arg(args, "name,version,contents") &&
            append_arg(args, "--all-matches") && append_targets(args, targets, 0);
        break;
    case Action::Depends:
        okay = append_arg(args, "--format") && append_arg(args, "json") &&
            append_arg(args, "--fields") && append_arg(args, "name,version,depends,provides") &&
            append_arg(args, "--all-matches") && append_targets(args, targets, 0);
        break;
    case Action::ReverseDepends:
        okay = append_arg(args, "--format") && append_arg(args, "json") &&
            append_arg(args, "--fields") && append_arg(args, "name,version,reverse-depends:name") &&
            append_arg(args, "--all-matches") && append_targets(args, targets, 0);
        break;
    case Action::Owner:
        okay = append_detail_query(args, "name,version,description", true, targets);
        break;
    case Action::World:
        okay = append_arg(args, "--format") && append_arg(args, "json") &&
            append_arg(args, "--fields") && append_arg(args, QueryFields) &&
            append_arg(args, "--world");
        break;
    case Action::Version:
        okay = append_arg(args, "--test") && append_targets(args, targets, 0);
        break;
    case Action::Dot:
        okay = append_targets(args, targets, 0);
        break;
    case Action::Manifest:
        okay = append_targets(args, targets, 0);
        break;
    case Action::Keys:
        okay = false;
        break;
    }
    if (!okay) return command_error(command, error, "could not build command arguments");
    if (simulate) return complete_command(command, args, false, false, true);
    // Downloading an archive or replacing cached indexes can be canceled
    // without interrupting an installed-package transaction.
    bool cancelable = !mutating || action == Action::Fetch || action == Action::CacheDownload ||
        action == Action::Update;
    return complete_command(command, args, privileged, mutating, cancelable);
}

bool query_command(int filter, const Settings &settings, Command &command, Text &error) {
    clear_command(command);
    error.clear();
    if (filter < 0 || filter > 4) return command_error(command, error, "unknown package query filter");
    if (!settings_valid(settings)) return command_error(command, error, "invalid apk settings");
    Array<Text> args;
    if (!append_global_options(args, settings) || !append_arg(args, "query") ||
        !append_arg(args, "--format") || !append_arg(args, "json") ||
        !append_arg(args, "--fields") ||
        !append_arg(args, filter == 1 ? InstalledQueryFields : QueryFields))
        return command_error(command, error, "out of memory");
    const char *selection = filter == 1 ? "--installed" : filter == 2 ? "--upgradable" :
        filter == 3 ? "--world" : filter == 4 ? "--orphaned" : nullptr;
    if ((selection && !append_arg(args, selection)) ||
        (filter == 1 && (!append_arg(args, "--from") || !append_arg(args, "installed"))))
        return command_error(command, error, "out of memory");
    if (filter != 3 && (!append_arg(args, "--") || !append_arg(args, "*")))
        return command_error(command, error, "out of memory");
    return complete_command(command, args, false, false, true);
}

bool metadata_command(const char *name, const Settings &settings, Command &command, Text &error) {
    clear_command(command);
    error.clear();
    if (!valid_constraint(name) || strpbrk(name, "<>=~@!"))
        return command_error(command, error, "metadata requires a package name");
    Settings local = settings;
    local.offline = true;
    if (!settings_valid(local)) return command_error(command, error, "invalid apk settings");
    Array<Text> args;
    // apk 3.0.8's exact-name matcher does not match name=version correctly.
    // Query all versions by plain name; the view chooses its selected version.
    if (!append_global_options(args, local) || !append_arg(args, "query") ||
        !append_arg(args, "--format") || !append_arg(args, "json") ||
        !append_arg(args, "--all-matches") || !append_arg(args, "--fields") ||
        !append_arg(args, "name,version,description,arch,license,url,origin,installed-size,file-size,"
                          "depends,reverse-depends,provides,repositories:name") ||
        !append_arg(args, "--") || !append_arg(args, name))
        return command_error(command, error, "out of memory");
    return complete_command(command, args, false, false, true);
}

}
