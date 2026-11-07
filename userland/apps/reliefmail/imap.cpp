#include "imap.h"
#include "mime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace rmail {
namespace {

constexpr size_t MaxImapLineBytes = 64u * 1024u;
constexpr size_t ImapReadChunk = 8192;
constexpr size_t MaxImapLiteralOverhead = MaxImapLineBytes + 256;
constexpr size_t MaxImapHeaderBytes = 256u * 1024u;
constexpr size_t MaxImapSearchTermBytes = 1024;

void secure_zero(void *buffer, size_t size);

class WireBuffer {
    unsigned char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
    bool sensitive_ = false;
public:
    explicit WireBuffer(size_t limit = MaxImapLineBytes) : limit_(limit) {}
    WireBuffer(const WireBuffer &) = delete;
    WireBuffer &operator=(const WireBuffer &) = delete;
    WireBuffer(WireBuffer &&other) noexcept
        : bytes_(other.bytes_), size_(other.size_), capacity_(other.capacity_), limit_(other.limit_),
          sensitive_(other.sensitive_) {
        other.bytes_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.sensitive_ = false;
    }
    WireBuffer &operator=(WireBuffer &&other) noexcept {
        if (this != &other) {
            if (sensitive_ && bytes_) secure_zero(bytes_, capacity_);
            free(bytes_);
            bytes_ = other.bytes_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            limit_ = other.limit_;
            sensitive_ = other.sensitive_;
            other.bytes_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
            other.sensitive_ = false;
        }
        return *this;
    }
    ~WireBuffer() { if (sensitive_ && bytes_) secure_zero(bytes_, capacity_); free(bytes_); }
    const unsigned char *data() const { return bytes_; }
    size_t size() const { return size_; }
    void mark_sensitive() { sensitive_ = true; }
    bool append(const void *source, size_t length) {
        if (length > limit_ - size_ || (length && !source) || length > SIZE_MAX - size_) return false;
        if (!length) return true;
        const size_t wanted = size_ + length;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < wanted) {
                if (next > SIZE_MAX / 2) { next = wanted; break; }
                next *= 2;
            }
            if (next > limit_) next = limit_;
            if (next < wanted) return false;
            void *replacement = realloc(bytes_, next);
            if (!replacement) return false;
            bytes_ = static_cast<unsigned char *>(replacement);
            capacity_ = next;
        }
        memcpy(bytes_ + size_, source, length);
        size_ = wanted;
        return true;
    }
    bool append_byte(unsigned char byte) { return append(&byte, 1); }
    void wipe() { if (bytes_) secure_zero(bytes_, capacity_); size_ = 0; }
    bool text(Text &result) const {
        return result.set(reinterpret_cast<const char *>(bytes_), size_);
    }
};

struct ImapState {
    unsigned char input[ImapReadChunk];
    size_t input_at = 0;
    size_t input_size = 0;
    uint32_t tag_counter = 0;
    bool capability_starttls = false;
    bool capability_plain = false;
    bool capability_login = false;
    bool capability_id = false;
    bool capability_move = false;
    bool capability_uidplus = false;
    bool authenticated = false;
    Text selected_name;
    uint64_t selected_uidvalidity = 0;
};

enum class ImapResult : uint8_t { Ok, Rejected, Continuation, Failed };
using ImapRecordHandler = bool (*)(void *context, const unsigned char *record,
    size_t record_size, Text &error);

uint64_t monotonic_ms() {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0) return UINT64_MAX;
    const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
    const uint64_t fraction = static_cast<uint64_t>(value.tv_nsec / 1000000);
    if (seconds > (UINT64_MAX - fraction) / 1000) return UINT64_MAX;
    return seconds * 1000 + fraction;
}

OperationContext with_timeout(const OperationContext &source, uint32_t timeout_ms) {
    OperationContext result = source;
    if (timeout_ms) {
        const uint64_t now = monotonic_ms();
        if (now != UINT64_MAX && now <= UINT64_MAX - timeout_ms) {
            const uint64_t local = now + timeout_ms;
            if (result.deadline_monotonic_ms == UINT64_MAX || local < result.deadline_monotonic_ms)
                result.deadline_monotonic_ms = local;
        }
    }
    return result;
}

bool write_all(Stream &stream, const unsigned char *bytes, size_t length,
               const OperationContext &context, Text &error) {
    size_t offset = 0;
    while (offset < length) {
        size_t written = 0;
        if (!stream.write(bytes + offset, length - offset, &written, context, error) ||
            !written || written > length - offset) {
            if (error.empty()) error.set("IMAP command write failed");
            return false;
        }
        offset += written;
    }
    return true;
}

class ImapReader {
    Stream &stream_;
    ImapState &state_;
    const OperationContext &context_;
    Text &error_;

    bool byte(unsigned char &result) {
        if (state_.input_at == state_.input_size) {
            size_t received = 0;
            if (!stream_.read(state_.input, sizeof(state_.input), &received, context_, error_)) return false;
            if (!received || received > sizeof(state_.input)) {
                error_.set("IMAP connection closed while reading a response");
                return false;
            }
            state_.input_at = 0;
            state_.input_size = received;
        }
        result = state_.input[state_.input_at++];
        return true;
    }

    bool line(WireBuffer &result) {
        result = WireBuffer(MaxImapLineBytes);
        bool carriage_return = false;
        for (;;) {
            unsigned char value = 0;
            if (!byte(value)) return false;
            if (carriage_return) {
                if (value != '\n') {
                    error_.set("malformed IMAP line ending");
                    return false;
                }
                return true;
            }
            if (value == '\r') { carriage_return = true; continue; }
            if (value == '\n' || value == 0 || (value < 0x20 && value != '\t')) {
                error_.set("malformed IMAP response line");
                return false;
            }
            if (!result.append_byte(value)) {
                error_.set("IMAP response line exceeds the 64 KiB limit");
                return false;
            }
        }
    }

    bool exact(unsigned char *destination, size_t length, WireBuffer &record) {
        size_t remaining = length;
        unsigned char scratch[4096];
        while (remaining) {
            if (state_.input_at < state_.input_size) {
                const size_t buffered = state_.input_size - state_.input_at;
                const size_t count = buffered < remaining ? buffered : remaining;
                const unsigned char *source = state_.input + state_.input_at;
                if (destination) memcpy(destination + (length - remaining), source, count);
                if (!record.append(source, count)) {
                    error_.set("IMAP response exceeds the bounded literal limit");
                    return false;
                }
                state_.input_at += count;
                remaining -= count;
                continue;
            }
            const size_t request = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
            size_t received = 0;
            if (!stream_.read(scratch, request, &received, context_, error_)) return false;
            if (!received || received > request) {
                error_.set("IMAP literal ended before its declared length");
                return false;
            }
            if (destination) memcpy(destination + (length - remaining), scratch, received);
            if (!record.append(scratch, received)) {
                error_.set("IMAP response exceeds the bounded literal limit");
                return false;
            }
            remaining -= received;
        }
        return true;
    }

    bool literal_marker(const WireBuffer &line_value, size_t maximum, size_t &count, bool &found) {
        found = false;
        count = 0;
        const unsigned char *bytes = line_value.data();
        const size_t size = line_value.size();
        if (!size || bytes[size - 1] != '}') return true;
        size_t open = size - 1;
        while (open && bytes[open] != '{') --open;
        if (bytes[open] != '{') return true;
        size_t at = open + 1;
        if (at < size - 1 && bytes[at] == '+') ++at;
        if (at == size - 1) {
            error_.set("malformed IMAP literal length");
            return false;
        }
        size_t parsed = 0;
        for (; at < size - 1; ++at) {
            if (bytes[at] < '0' || bytes[at] > '9' ||
                parsed > (SIZE_MAX - static_cast<size_t>(bytes[at] - '0')) / 10) {
                error_.set("malformed IMAP literal length");
                return false;
            }
            parsed = parsed * 10 + static_cast<size_t>(bytes[at] - '0');
        }
        if (parsed > maximum || parsed > MaxRawMessageBytes) {
            error_.set("IMAP literal exceeds the aggregate response budget or configured message bound");
            return false;
        }
        count = parsed;
        found = true;
        return true;
    }

public:
    ImapReader(Stream &stream, ImapState &state, const OperationContext &context, Text &error)
        : stream_(stream), state_(state), context_(context), error_(error) {}

    bool response(WireBuffer &record, size_t max_literal, size_t *remaining_budget = nullptr) {
        record = WireBuffer(MaxRawMessageBytes + MaxImapLiteralOverhead);
        for (;;) {
            WireBuffer line_value(MaxImapLineBytes);
            if (!line(line_value) || !record.append(line_value.data(), line_value.size()) ||
                !record.append("\r\n", 2)) {
                if (error_.empty()) error_.set("IMAP response exceeds its bounded size");
                return false;
            }
            size_t literal_length = 0;
            bool has_literal = false;
            size_t literal_limit = max_literal;
            if (remaining_budget && *remaining_budget < literal_limit)
                literal_limit = *remaining_budget;
            if (!literal_marker(line_value, literal_limit, literal_length, has_literal)) return false;
            if (!has_literal) return true;
            if (!exact(nullptr, literal_length, record)) return false;
            if (remaining_budget) *remaining_budget -= literal_length;
            /* A literal may be followed by a response suffix (or an empty line). */
        }
    }
};

bool equal_ascii(const unsigned char *bytes, size_t size, const char *word) {
    if (size != strlen(word)) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char left = bytes[i];
        unsigned char right = static_cast<unsigned char>(word[i]);
        if (left >= 'a' && left <= 'z') left = static_cast<unsigned char>(left - ('a' - 'A'));
        if (right >= 'a' && right <= 'z') right = static_cast<unsigned char>(right - ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

bool starts_ascii(const unsigned char *bytes, size_t size, const char *prefix) {
    const size_t wanted = strlen(prefix);
    if (size < wanted) return false;
    return equal_ascii(bytes, wanted, prefix);
}

bool is_space(unsigned char value) { return value == ' ' || value == '\t'; }
void skip_spaces(const unsigned char *line, size_t size, size_t &at) {
    while (at < size && is_space(line[at])) ++at;
}

bool parse_uint(const unsigned char *bytes, size_t size, uint64_t maximum, uint64_t &value) {
    if (!size) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] < '0' || bytes[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(bytes[i] - '0');
        if (digit > maximum || result > (maximum - digit) / 10) return false;
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

struct RecordView {
    const unsigned char *line = nullptr;
    size_t line_size = 0;
    const unsigned char *literal = nullptr;
    size_t literal_size = 0;
    size_t literal_marker = SIZE_MAX;
    const unsigned char *suffix = nullptr;
    size_t suffix_size = 0;
};

bool view_record(const unsigned char *record, size_t size, RecordView &view) {
    size_t line_end = 0;
    while (line_end + 1 < size && !(record[line_end] == '\r' && record[line_end + 1] == '\n')) ++line_end;
    if (line_end + 1 >= size) return false;
    view.line = record;
    view.line_size = line_end;
    if (line_end && record[line_end - 1] == '}') {
        size_t open = line_end - 1;
        while (open && record[open] != '{') --open;
        if (record[open] == '{') {
            size_t start = open + 1;
            if (start < line_end && record[start] == '+') ++start;
            uint64_t length = 0;
            if (parse_uint(record + start, line_end - start - 1, MaxRawMessageBytes, length)) {
                const size_t literal_at = line_end + 2;
                if (length <= size - literal_at) {
                    view.literal = record + literal_at;
                    view.literal_size = static_cast<size_t>(length);
                    view.literal_marker = open;
                    const size_t suffix_at = literal_at + static_cast<size_t>(length);
                    view.suffix = record + suffix_at;
                    view.suffix_size = size - suffix_at;
                }
            }
        }
    }
    return true;
}

bool parse_astring(const unsigned char *line, size_t line_size, size_t &at,
                   const RecordView &view, Text &result) {
    skip_spaces(line, line_size, at);
    if (at >= line_size) return false;
    WireBuffer value(MaxEditableTextBytes);
    if (line[at] == '"') {
        ++at;
        bool closed = false;
        while (at < line_size) {
            unsigned char c = line[at++];
            if (c == '"') { closed = true; break; }
            if (c == '\\') {
                if (at >= line_size || (line[at] != '"' && line[at] != '\\')) return false;
                c = line[at++];
            }
            if (!c || c < 0x20 || c > 0x7e || !value.append_byte(c)) return false;
        }
        return closed && value.text(result);
    }
    if (line[at] == '{') {
        const size_t marker = at;
        size_t end = marker + 1;
        while (end < line_size && line[end] != '}') ++end;
        if (end == line_size || marker != view.literal_marker || !view.literal) return false;
        uint64_t length = 0;
        if (!parse_uint(line + marker + 1, end - marker - 1, MaxEditableTextBytes, length) ||
            length != view.literal_size) return false;
        at = end + 1;
        return result.set(reinterpret_cast<const char *>(view.literal), view.literal_size);
    }
    const size_t start = at;
    while (at < line_size && !is_space(line[at])) ++at;
    if (at == start || at - start > MaxEditableTextBytes) return false;
    return result.set(reinterpret_cast<const char *>(line + start), at - start);
}

bool skip_parenthesized(const unsigned char *line, size_t size, size_t &at) {
    skip_spaces(line, size, at);
    if (at >= size || line[at] != '(') return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (; at < size; ++at) {
        const unsigned char c = line[at];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) { ++at; return true; }
    }
    return false;
}

bool ascii_case_equal(const Text &left, const char *right) {
    return equal_ascii(reinterpret_cast<const unsigned char *>(left.c_str()), left.size(), right);
}

bool canonical_inbox(const Text &name) { return ascii_case_equal(name, "INBOX"); }

bool valid_wire_name(const Text &name) {
    if (name.empty() || name.size() > MaxEditableTextBytes) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name.c_str()[i]);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

bool append_quoted(WireBuffer &output, const Text &value) {
    if (value.size() > MaxEditableTextBytes) return false;
    if (!output.append("\"", 1)) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
        if (!c || c < 0x20 || c > 0x7e) return false;
        if (c == '"' || c == '\\') if (!output.append("\\", 1)) return false;
        if (!output.append_byte(c)) return false;
    }
    return output.append("\"", 1);
}

bool append_search_quoted(WireBuffer &output, const Text &value) {
    if (value.empty() || value.size() > MaxImapSearchTermBytes ||
        !valid_utf8(value.c_str(), value.size()) || !output.append("\"", 1)) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if ((c == '"' || c == '\\') && !output.append("\\", 1)) return false;
        if (!output.append_byte(c)) return false;
    }
    return output.append("\"", 1);
}

bool contains_non_ascii(const Text &value) {
    for (size_t i = 0; i < value.size(); ++i)
        if (static_cast<unsigned char>(value.c_str()[i]) >= 0x80) return true;
    return false;
}

bool valid_message_id(const Text &value) {
    if (value.size() < 5 || value.size() > 998 || value.c_str()[0] != '<' ||
        value.c_str()[value.size() - 1] != '>') return false;
    size_t at_signs = 0;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
        if (c <= 0x20 || c >= 0x7f || c == '<' || c == '>') return false;
        if (c == '@') ++at_signs;
    }
    return at_signs == 1;
}

bool next_tag(ImapState &state, char tag[16]) {
    if (++state.tag_counter == 0) ++state.tag_counter;
    const int length = snprintf(tag, 16, "A%08u", state.tag_counter);
    return length == 9;
}

bool send_command(Stream &stream, ImapState &state, const unsigned char *command,
                  size_t command_size, char tag[16], const OperationContext &context, Text &error,
                  bool sensitive = false) {
    if (!command || !command_size || command_size > MaxImapLineBytes - 16 ||
        memchr(command, '\r', command_size) || memchr(command, '\n', command_size) ||
        !next_tag(state, tag)) {
        error.set("invalid or oversized IMAP command");
        return false;
    }
    WireBuffer bytes(MaxImapLineBytes);
    if (sensitive) bytes.mark_sensitive();
    if (!bytes.append(tag, strlen(tag)) || !bytes.append(" ", 1) ||
        !bytes.append(command, command_size) || !bytes.append("\r\n", 2)) {
        error.set("could not prepare IMAP command");
        return false;
    }
    return write_all(stream, bytes.data(), bytes.size(), context, error);
}

ImapResult read_command_result(Stream &stream, ImapState &state, const char *tag,
    const OperationContext &context, Text &error, ImapRecordHandler handler = nullptr,
    void *handler_context = nullptr, size_t max_literal = MaxRawMessageBytes,
    size_t *remaining_budget = nullptr) {
    ImapReader reader(stream, state, context, error);
    for (;;) {
        WireBuffer record(MaxRawMessageBytes + MaxImapLiteralOverhead);
        if (!reader.response(record, max_literal, remaining_budget)) return ImapResult::Failed;
        RecordView view;
        if (!view_record(record.data(), record.size(), view)) {
            error.set("malformed IMAP response");
            return ImapResult::Failed;
        }
        if (view.line_size > strlen(tag) && !memcmp(view.line, tag, strlen(tag)) &&
            view.line[strlen(tag)] == ' ') {
            size_t at = strlen(tag) + 1;
            const size_t status_start = at;
            while (at < view.line_size && !is_space(view.line[at])) ++at;
            const size_t status_size = at - status_start;
            if (equal_ascii(view.line + status_start, status_size, "OK")) return ImapResult::Ok;
            if (equal_ascii(view.line + status_start, status_size, "NO") ||
                equal_ascii(view.line + status_start, status_size, "BAD")) return ImapResult::Rejected;
            error.set("malformed tagged IMAP status");
            return ImapResult::Failed;
        }
        if (view.line_size && view.line[0] == '+') return ImapResult::Continuation;
        if (!view.line_size || view.line[0] != '*') {
            error.set("unexpected IMAP response tag");
            return ImapResult::Failed;
        }
        if (handler && !handler(handler_context, record.data(), record.size(), error)) {
            if (error.empty()) error.set("malformed IMAP response data");
            return ImapResult::Failed;
        }
    }
}

bool run_command(Stream &stream, ImapState &state, const unsigned char *command,
                 size_t command_size, const OperationContext &context, Text &error,
                 ImapRecordHandler handler = nullptr, void *handler_context = nullptr,
                 size_t max_literal = MaxRawMessageBytes, size_t *remaining_budget = nullptr) {
    char tag[16];
    if (!send_command(stream, state, command, command_size, tag, context, error)) return false;
    const ImapResult result = read_command_result(stream, state, tag, context, error,
        handler, handler_context, max_literal, remaining_budget);
    if (result == ImapResult::Ok) return true;
    if (result == ImapResult::Rejected) error.set("IMAP server rejected the command");
    else if (result == ImapResult::Continuation) error.set("unexpected IMAP continuation");
    return false;
}

bool run_command(Stream &stream, ImapState &state, const char *command,
                 const OperationContext &context, Text &error,
                 ImapRecordHandler handler = nullptr, void *handler_context = nullptr,
                 size_t max_literal = MaxRawMessageBytes, size_t *remaining_budget = nullptr) {
    return command && run_command(stream, state,
        reinterpret_cast<const unsigned char *>(command), strlen(command), context,
        error, handler, handler_context, max_literal, remaining_budget);
}

bool capability_handler(void *opaque, const unsigned char *record, size_t size, Text &) {
    ImapState *state = static_cast<ImapState *>(opaque);
    RecordView view;
    if (!view_record(record, size, view) || !starts_ascii(view.line, view.line_size, "* CAPABILITY")) return true;
    size_t at = strlen("* CAPABILITY");
    while (at < view.line_size) {
        skip_spaces(view.line, view.line_size, at);
        const size_t start = at;
        while (at < view.line_size && !is_space(view.line[at])) ++at;
        const size_t length = at - start;
        if (equal_ascii(view.line + start, length, "STARTTLS")) state->capability_starttls = true;
        else if (equal_ascii(view.line + start, length, "AUTH=PLAIN")) state->capability_plain = true;
        else if (equal_ascii(view.line + start, length, "AUTH=LOGIN")) state->capability_login = true;
        else if (equal_ascii(view.line + start, length, "ID")) state->capability_id = true;
        else if (equal_ascii(view.line + start, length, "MOVE")) state->capability_move = true;
        else if (equal_ascii(view.line + start, length, "UIDPLUS")) state->capability_uidplus = true;
    }
    return true;
}

bool query_capabilities(Stream &stream, ImapState &state,
                        const OperationContext &context, Text &error) {
    state.capability_starttls = false;
    state.capability_plain = false;
    state.capability_login = false;
    state.capability_id = false;
    state.capability_move = false;
    state.capability_uidplus = false;
    return run_command(stream, state, "CAPABILITY", context, error, capability_handler, &state);
}

bool id_handler(void *, const unsigned char *, size_t, Text &) { return true; }

bool read_greeting(Stream &stream, ImapState &state, const OperationContext &context,
                   Text &error) {
    ImapReader reader(stream, state, context, error);
    WireBuffer record(MaxImapLineBytes + 2);
    if (!reader.response(record, 0)) return false;
    RecordView view;
    if (!view_record(record.data(), record.size(), view) || !starts_ascii(view.line, view.line_size, "* OK")) {
        error.set("IMAP server greeting was not accepted");
        return false;
    }
    return true;
}

void secure_zero(void *buffer, size_t size) {
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(buffer);
    while (size--) *bytes++ = 0;
}

bool base64_encode(const unsigned char *input, size_t input_size, char *output,
                   size_t output_capacity, size_t &output_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (input_size > (SIZE_MAX - 2) / 4 * 3) return false;
    const size_t needed = ((input_size + 2) / 3) * 4;
    if (needed > output_capacity) return false;
    size_t in = 0, out = 0;
    while (in < input_size) {
        const size_t remain = input_size - in;
        const uint32_t a = input[in++];
        const uint32_t b = remain > 1 ? input[in++] : 0;
        const uint32_t c = remain > 2 ? input[in++] : 0;
        output[out++] = alphabet[a >> 2];
        output[out++] = alphabet[((a & 3) << 4) | (b >> 4)];
        output[out++] = remain > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output[out++] = remain > 2 ? alphabet[c & 63] : '=';
    }
    output_size = out;
    return true;
}

bool make_sasl_plain(const Account &account, SecretView password,
                     unsigned char **encoded, size_t &encoded_size) {
    const Text &user = account.username;
    if (!password.data || !password.size || password.size > 4096 || user.empty() || user.size() > 1024 ||
        user.size() > SIZE_MAX - password.size - 2) return false;
    const size_t raw_size = user.size() + password.size + 2;
    unsigned char *raw = static_cast<unsigned char *>(malloc(raw_size));
    const size_t cap = ((raw_size + 2) / 3) * 4;
    char *response = static_cast<char *>(malloc(cap + 1));
    if (!raw || !response) { if (raw) free(raw); if (response) free(response); return false; }
    raw[0] = 0;
    memcpy(raw + 1, user.c_str(), user.size());
    raw[1 + user.size()] = 0;
    memcpy(raw + user.size() + 2, password.data, password.size);
    bool ok = base64_encode(raw, raw_size, response, cap, encoded_size);
    secure_zero(raw, raw_size);
    free(raw);
    if (!ok) { secure_zero(response, cap + 1); free(response); return false; }
    response[encoded_size] = 0;
    *encoded = reinterpret_cast<unsigned char *>(response);
    return true;
}

bool make_sasl_login_value(const unsigned char *input, size_t input_size,
                           char **encoded, size_t &encoded_size) {
    const size_t cap = ((input_size + 2) / 3) * 4;
    char *result = static_cast<char *>(malloc(cap + 1));
    if (!result) return false;
    if (!base64_encode(input, input_size, result, cap, encoded_size)) { free(result); return false; }
    result[encoded_size] = 0;
    *encoded = result;
    return true;
}

bool send_sasl_response(Stream &stream, const unsigned char *value, size_t length,
                        const OperationContext &context, Text &error) {
    return write_all(stream, value, length, context, error) &&
        write_all(stream, reinterpret_cast<const unsigned char *>("\r\n"), 2, context, error);
}

bool authenticate_sasl(Stream &stream, ImapState &state, const Account &account,
                       SecretView password, bool plain, const OperationContext &context,
                       Text &error) {
    char tag[16];
    const char *mechanism = plain ? "AUTHENTICATE PLAIN" : "AUTHENTICATE LOGIN";
    if (!send_command(stream, state, reinterpret_cast<const unsigned char *>(mechanism),
        strlen(mechanism), tag, context, error)) return false;
    ImapResult result = read_command_result(stream, state, tag, context, error);
    if (result != ImapResult::Continuation) {
        if (result == ImapResult::Rejected) error.set("IMAP authentication rejected");
        else if (result == ImapResult::Ok) error.set("IMAP authentication exchange was incomplete");
        return false;
    }
    if (plain) {
        unsigned char *response = nullptr;
        size_t response_size = 0;
        if (!make_sasl_plain(account, password, &response, response_size)) {
            error.set("could not prepare IMAP authentication response");
            return false;
        }
        const bool sent = send_sasl_response(stream, response, response_size, context, error);
        secure_zero(response, response_size);
        free(response);
        if (!sent) return false;
        result = read_command_result(stream, state, tag, context, error);
    } else {
        const Text &username = account.username;
        if (username.empty() || !password.data || !password.size || password.size > 4096) {
            error.set("IMAP username or password is invalid");
            return false;
        }
        char *user64 = nullptr, *pass64 = nullptr;
        size_t user64_size = 0, pass64_size = 0;
        if (!make_sasl_login_value(reinterpret_cast<const unsigned char *>(username.c_str()),
            username.size(), &user64, user64_size) ||
            !make_sasl_login_value(password.data, password.size, &pass64, pass64_size)) {
            if (user64) { secure_zero(user64, user64_size); free(user64); }
            if (pass64) { secure_zero(pass64, pass64_size); free(pass64); }
            error.set("could not prepare IMAP authentication response");
            return false;
        }
        bool sent = send_sasl_response(stream, reinterpret_cast<unsigned char *>(user64), user64_size, context, error);
        if (sent) result = read_command_result(stream, state, tag, context, error);
        if (sent && result == ImapResult::Continuation)
            sent = send_sasl_response(stream, reinterpret_cast<unsigned char *>(pass64), pass64_size, context, error);
        secure_zero(user64, user64_size); free(user64);
        secure_zero(pass64, pass64_size); free(pass64);
        if (!sent) return false;
        if (result == ImapResult::Continuation) result = read_command_result(stream, state, tag, context, error);
    }
    if (result == ImapResult::Ok) return true;
    if (result == ImapResult::Rejected) error.set("IMAP authentication rejected");
    else if (error.empty()) error.set("IMAP authentication exchange failed");
    return false;
}

bool authenticate_login(Stream &stream, ImapState &state, const Account &account,
                        SecretView password, const OperationContext &context, Text &error) {
    if (!stream.secure() || !password.data || !password.size || password.size > 4096 ||
        account.username.empty() || account.username.size() > 1024) {
        error.set("IMAP authentication requires verified TLS and valid credentials");
        return false;
    }
    const bool sasl_plain = state.capability_plain;
    const bool sasl_login = state.capability_login;
    if (sasl_plain) return authenticate_sasl(stream, state, account, password, true, context, error);
    if (sasl_login) return authenticate_sasl(stream, state, account, password, false, context, error);

    WireBuffer command(MaxImapLineBytes);
    command.mark_sensitive();
    char *password_copy = static_cast<char *>(malloc(password.size));
    if (!password_copy) { error.set("out of memory while preparing IMAP login"); return false; }
    memcpy(password_copy, password.data, password.size);
    bool prepared = command.append("LOGIN ", 6) && append_quoted(command, account.username) &&
        command.append(" ", 1);
    Text secret;
    if (prepared) prepared = secret.set(password_copy, password.size);
    secure_zero(password_copy, password.size);
    free(password_copy);
    if (prepared) prepared = append_quoted(command, secret);
    if (!prepared) {
        /* The temporary Text contains the password and is cleared by reset below. */
        for (size_t i = 0; i < secret.size(); ++i) const_cast<char *>(secret.c_str())[i] = 0;
        secret.clear();
        command.wipe();
        error.set("could not prepare IMAP login command");
        return false;
    }
    char tag[16];
    const bool sent = send_command(stream, state, command.data(), command.size(), tag, context, error, true);
    command.wipe();
    for (size_t i = 0; i < secret.size(); ++i) const_cast<char *>(secret.c_str())[i] = 0;
    secret.clear();
    if (!sent) return false;
    const ImapResult result = read_command_result(stream, state, tag, context, error);
    if (result == ImapResult::Ok) return true;
    if (result == ImapResult::Rejected) error.set("IMAP authentication rejected");
    else if (error.empty()) error.set("IMAP authentication exchange failed");
    return false;
}

bool decode_utf8_scalar(const unsigned char *bytes, size_t length, size_t &at, uint32_t &value) {
    if (at >= length) return false;
    const unsigned char first = bytes[at++];
    if (first < 0x80) { value = first; return true; }
    size_t count = first >= 0xc2 && first <= 0xdf ? 1 :
        first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (!count || count > length - at) return false;
    uint32_t result = first & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    for (size_t i = 0; i < count; ++i) {
        if ((bytes[at] & 0xc0) != 0x80) return false;
        result = (result << 6) | (bytes[at++] & 0x3f);
    }
    if ((count == 1 && result < 0x80) || (count == 2 && result < 0x800) ||
        (count == 3 && (result < 0x10000 || result > 0x10ffff)) ||
        (result >= 0xd800 && result <= 0xdfff)) return false;
    value = result;
    return true;
}

bool append_base64_u16(WireBuffer &output, const unsigned char *bytes, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
    size_t at = 0;
    while (at < size) {
        const size_t remain = size - at;
        const uint32_t a = bytes[at++];
        const uint32_t b = remain > 1 ? bytes[at++] : 0;
        const uint32_t c = remain > 2 ? bytes[at++] : 0;
        if (!output.append_byte(static_cast<unsigned char>(alphabet[a >> 2])) ||
            !output.append_byte(static_cast<unsigned char>(alphabet[((a & 3) << 4) | (b >> 4)])) ||
            (remain > 1 && !output.append_byte(static_cast<unsigned char>(alphabet[((b & 15) << 2) | (c >> 6)]))) ||
            (remain > 2 && !output.append_byte(static_cast<unsigned char>(alphabet[c & 63])))) return false;
    }
    return true;
}

struct FolderCollector {
    uint64_t account_id;
    FolderList *folders;
};

bool contains_ascii_nocase(const unsigned char *bytes, size_t size, const char *word) {
    const size_t length = strlen(word);
    if (length > size) return false;
    for (size_t start = 0; start + length <= size; ++start) {
        size_t i = 0;
        for (; i < length; ++i) {
            unsigned char left = bytes[start + i];
            unsigned char right = static_cast<unsigned char>(word[i]);
            if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + ('a' - 'A'));
            if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + ('a' - 'A'));
            if (left != right) break;
        }
        if (i == length) return true;
    }
    return false;
}

bool list_handler(void *opaque, const unsigned char *record, size_t record_size, Text &error) {
    RecordView view;
    if (!view_record(record, record_size, view)) {
        error.set("malformed IMAP LIST response");
        return false;
    }
    if (!starts_ascii(view.line, view.line_size, "* LIST")) return true;
    size_t at = 1;
    skip_spaces(view.line, view.line_size, at);
    while (at < view.line_size && !is_space(view.line[at])) ++at;
    const size_t flags_open = at;
    if (!skip_parenthesized(view.line, view.line_size, at)) {
        error.set("malformed IMAP LIST flags");
        return false;
    }
    const size_t flags_end = at;
    Text delimiter, wire_name;
    if (!parse_astring(view.line, view.line_size, at, view, delimiter) ||
        !parse_astring(view.line, view.line_size, at, view, wire_name) || !valid_wire_name(wire_name)) {
        error.set("invalid IMAP mailbox name");
        return false;
    }
    FolderCollector *collector = static_cast<FolderCollector *>(opaque);
    if (collector->folders->size() >= MaxFoldersPerAccount) {
        error.set("IMAP folder listing exceeds the account limit");
        return false;
    }
    for (size_t i = 0; i < collector->folders->size(); ++i)
        if ((*collector->folders)[i].key.server_name == wire_name) return true;
    Folder folder;
    folder.key.account_id = collector->account_id;
    if (canonical_inbox(wire_name)) {
        if (!folder.key.server_name.set("INBOX") || !folder.display_name.set("Inbox")) {
            error.set("out of memory while preparing IMAP folder");
            return false;
        }
    folder.role = FolderRole::Inbox;
    } else {
        folder.key.server_name = wire_name;
        Text decode_error;
        if (!decode_modified_utf7(wire_name.c_str(), wire_name.size(), folder.display_name, decode_error)) {
            error.set("invalid modified UTF-7 mailbox name");
            return false;
        }
    }
    folder.selectable = !contains_ascii_nocase(view.line + flags_open,
        flags_end - flags_open, "\\noselect");
    /* LIST does not carry UIDVALIDITY; zero means unknown, never reset. */
    folder.uidvalidity = 0;
    if (!collector->folders->push(static_cast<Folder &&>(folder))) {
        error.set("IMAP folder listing exceeds the account limit");
        return false;
    }
    return true;
}

bool status_mailbox_matches(const Text &actual, const Text &expected) {
    if (canonical_inbox(actual) && canonical_inbox(expected)) return true;
    return actual == expected;
}

bool status_value_end(const unsigned char *bytes, size_t size, size_t &at) {
    if (at >= size) return false;
    if (bytes[at] == '"') {
        ++at;
        bool escaped = false;
        while (at < size) {
            const unsigned char c = bytes[at++];
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '"') return true;
            if (!c || c < 0x20 || c > 0x7e) return false;
        }
        return false;
    }
    if (bytes[at] == '(') return skip_parenthesized(bytes, size, at);
    const size_t start = at;
    while (at < size && !is_space(bytes[at]) && bytes[at] != ')') {
        if (!bytes[at] || bytes[at] < 0x21 || bytes[at] > 0x7e) return false;
        ++at;
    }
    return at > start;
}

bool parse_status_attributes(const unsigned char *bytes, size_t size,
    uint32_t &messages, uint32_t &unseen) {
    messages = 0;
    unseen = 0;
    size_t at = 0;
    skip_spaces(bytes, size, at);
    if (at >= size || bytes[at++] != '(') return false;
    bool closed = false;
    while (at < size) {
        skip_spaces(bytes, size, at);
        if (at < size && bytes[at] == ')') { ++at; closed = true; break; }
        const size_t key_start = at;
        while (at < size && !is_space(bytes[at]) && bytes[at] != ')') {
            if (!bytes[at] || bytes[at] < 0x21 || bytes[at] > 0x7e) return false;
            ++at;
        }
        if (at == key_start) return false;
        const size_t key_size = at - key_start;
        skip_spaces(bytes, size, at);
        const size_t value_start = at;
        if (!status_value_end(bytes, size, at)) return false;
        uint64_t parsed = 0;
        if (equal_ascii(bytes + key_start, key_size, "MESSAGES")) {
            if (!parse_uint(bytes + value_start, at - value_start, UINT32_MAX, parsed)) return false;
            messages = static_cast<uint32_t>(parsed);
        } else if (equal_ascii(bytes + key_start, key_size, "UNSEEN")) {
            if (!parse_uint(bytes + value_start, at - value_start, UINT32_MAX, parsed)) return false;
            unseen = static_cast<uint32_t>(parsed);
        }
    }
    skip_spaces(bytes, size, at);
    return closed && at == size;
}

struct StatusCollector { FolderList *folders; size_t index; };

bool status_handler(void *opaque, const unsigned char *record, size_t record_size, Text &error) {
    RecordView view;
    if (!view_record(record, record_size, view)) {
        error.set("malformed IMAP STATUS response");
        return false;
    }
    if (!starts_ascii(view.line, view.line_size, "* STATUS") ||
        view.line_size <= strlen("* STATUS") || !is_space(view.line[strlen("* STATUS")])) return true;
    StatusCollector *collector = static_cast<StatusCollector *>(opaque);
    if (collector->index >= collector->folders->size()) return true;
    size_t at = strlen("* STATUS");
    Text mailbox;
    if (!parse_astring(view.line, view.line_size, at, view, mailbox)) {
        error.set("malformed IMAP STATUS mailbox");
        return false;
    }
    Folder &expected = (*collector->folders)[collector->index];
    if (!status_mailbox_matches(mailbox, expected.key.server_name)) return true;

    const unsigned char *attributes = view.line;
    size_t attributes_size = view.line_size;
    size_t attributes_at = at;
    if (view.literal) {
        attributes = view.suffix;
        attributes_size = view.suffix_size;
        if (attributes_size >= 2 && attributes[attributes_size - 2] == '\r' &&
            attributes[attributes_size - 1] == '\n') attributes_size -= 2;
        attributes_at = 0;
    }
    if (attributes_at > attributes_size) {
        error.set("malformed IMAP STATUS attributes");
        return false;
    }
    uint32_t message_count = 0, unread_count = 0;
    if (!parse_status_attributes(attributes + attributes_at,
        attributes_size - attributes_at, message_count, unread_count)) {
        error.set("malformed IMAP STATUS attributes");
        return false;
    }
    expected.message_count = message_count;
    expected.unread_count = unread_count;
    return true;
}

struct SelectCollector { uint64_t uidvalidity = 0; uint32_t exists = 0; };

bool select_handler(void *opaque, const unsigned char *record, size_t record_size, Text &error) {
    RecordView view;
    if (!view_record(record, record_size, view)) {
        error.set("malformed IMAP SELECT response");
        return false;
    }
    SelectCollector *collector = static_cast<SelectCollector *>(opaque);
    if (starts_ascii(view.line, view.line_size, "* OK [UIDVALIDITY ")) {
        const size_t start = strlen("* OK [UIDVALIDITY ");
        size_t end = start;
        while (end < view.line_size && view.line[end] >= '0' && view.line[end] <= '9') ++end;
        if (end == start || end + 1 >= view.line_size || view.line[end] != ']') {
            error.set("invalid UIDVALIDITY in SELECT response");
            return false;
        }
        if (!parse_uint(view.line + start, end - start, UINT64_MAX, collector->uidvalidity) ||
            !collector->uidvalidity) {
            error.set("invalid UIDVALIDITY in SELECT response");
            return false;
        }
    } else if (view.line_size > 2 && view.line[0] == '*' && is_space(view.line[1])) {
        size_t at = 2;
        while (at < view.line_size && view.line[at] >= '0' && view.line[at] <= '9') ++at;
        if (at > 2 && at + 7 <= view.line_size && starts_ascii(view.line + at, view.line_size - at, " EXISTS")) {
            uint64_t value = 0;
            if (parse_uint(view.line + 2, at - 2, UINT32_MAX, value)) collector->exists = static_cast<uint32_t>(value);
        }
    }
    return true;
}

bool select_mailbox(Stream &stream, ImapState &state, const FolderKey &key,
                    Folder &selected, const OperationContext &context, Text &error) {
    Text wire = key.server_name;
    if (!valid_wire_name(wire)) { error.set("invalid IMAP folder identity"); return false; }
    if (canonical_inbox(wire)) wire.set("INBOX");
    WireBuffer command(MaxImapLineBytes);
    if (!command.append("SELECT ", 7) || !append_quoted(command, wire)) {
        error.set("invalid or oversized IMAP folder identity");
        return false;
    }
    SelectCollector collector;
    if (!run_command(stream, state, command.data(), command.size(), context, error,
        select_handler, &collector)) return false;
    if (!collector.uidvalidity) {
        error.set("SELECT did not establish UIDVALIDITY");
        return false;
    }
    state.selected_name = wire;
    state.selected_uidvalidity = collector.uidvalidity;
    selected.key.account_id = key.account_id;
    selected.key.server_name = wire;
    selected.uidvalidity = collector.uidvalidity;
    selected.message_count = collector.exists;
    if (canonical_inbox(wire)) {
        selected.display_name.set("Inbox");
        selected.role = FolderRole::Inbox;
    } else {
        Text decode_error;
        if (!decode_modified_utf7(wire.c_str(), wire.size(), selected.display_name, decode_error)) {
            error.set("invalid modified UTF-7 mailbox name");
            return false;
        }
    }
    return true;
}

bool ensure_selected(Stream &stream, ImapState &state, uint64_t account_id,
                     const FolderKey &folder, const OperationContext &context, Text &error) {
    if (folder.account_id != account_id || !valid_wire_name(folder.server_name)) {
        error.set("IMAP folder identity does not match the account");
        return false;
    }
    Text name = folder.server_name;
    if (canonical_inbox(name)) name.set("INBOX");
    if (state.selected_uidvalidity && state.selected_name == name) return true;
    Folder selected;
    return select_mailbox(stream, state, folder, selected, context, error);
}

struct SearchCollector {
    uint64_t account_id;
    const FolderKey *folder;
    uint64_t uidvalidity;
    Array<MessageKey> *keys;
};

bool search_handler(void *opaque, const unsigned char *record, size_t record_size, Text &error) {
    RecordView view;
    if (!view_record(record, record_size, view)) {
        error.set("malformed IMAP SEARCH response");
        return false;
    }
    if (!starts_ascii(view.line, view.line_size, "* SEARCH")) return true;
    size_t at = strlen("* SEARCH");
    while (at < view.line_size) {
        skip_spaces(view.line, view.line_size, at);
        if (at >= view.line_size) break;
        const size_t start = at;
        while (at < view.line_size && view.line[at] >= '0' && view.line[at] <= '9') ++at;
        uint64_t uid = 0;
        if (at == start || !parse_uint(view.line + start, at - start, UINT32_MAX, uid) || !uid) {
            error.set("invalid UID in IMAP SEARCH response");
            return false;
        }
        SearchCollector *collector = static_cast<SearchCollector *>(opaque);
        for (size_t i = 0; i < collector->keys->size(); ++i)
            if ((*collector->keys)[i].uid == uid) goto next_uid;
        if (collector->keys->size() >= MaxIndexedMessagesPerFolder) {
            error.set("IMAP SEARCH result exceeds the indexed-message limit");
            return false;
        }
        {
            MessageKey key;
            key.folder.account_id = collector->account_id;
            key.folder.server_name = collector->folder->server_name;
            key.uidvalidity = collector->uidvalidity;
            key.uid = static_cast<uint32_t>(uid);
            if (!collector->keys->push(static_cast<MessageKey &&>(key))) {
                error.set("out of memory while collecting IMAP search results");
                return false;
            }
        }
        next_uid:
        if (at < view.line_size && !is_space(view.line[at])) {
            error.set("malformed IMAP SEARCH response");
            return false;
        }
    }
    return true;
}

bool search_criterion(Stream &stream, ImapState &state, const Account &account,
    const FolderKey &folder, const unsigned char *criterion, size_t criterion_size,
    bool utf8, Array<MessageKey> &keys, const OperationContext &operation, Text &error) {
    if (!ensure_selected(stream, state, account.id, folder, operation, error)) return false;
    WireBuffer command(MaxImapLineBytes);
    if (!command.append("UID SEARCH ", 11) ||
        (utf8 && !command.append("CHARSET UTF-8 ", 14)) ||
        !command.append(criterion, criterion_size)) {
        error.set("could not prepare bounded IMAP SEARCH command");
        return false;
    }
    Array<MessageKey> next;
    SearchCollector collector = {account.id, &folder, state.selected_uidvalidity, &next};
    if (!run_command(stream, state, command.data(), command.size(), operation,
        error, search_handler, &collector)) {
        if (utf8 && ascii_case_equal(error, "IMAP server rejected the command"))
            error.set("server rejected IMAP SEARCH CHARSET UTF-8");
        return false;
    }
    keys = static_cast<Array<MessageKey> &&>(next);
    return true;
}

bool response_uid(const RecordView &view, uint32_t &uid) {
    for (size_t i = 0; i + 3 < view.line_size; ++i) {
        if ((i && !is_space(view.line[i - 1]) && view.line[i - 1] != '(') ||
            !equal_ascii(view.line + i, 3, "UID") || !is_space(view.line[i + 3])) continue;
        size_t at = i + 3;
        skip_spaces(view.line, view.line_size, at);
        const size_t start = at;
        while (at < view.line_size && view.line[at] >= '0' && view.line[at] <= '9') ++at;
        uint64_t value = 0;
        if (parse_uint(view.line + start, at - start, UINT32_MAX, value) && value) {
            uid = static_cast<uint32_t>(value);
            return true;
        }
    }
    return false;
}

bool field_position(const unsigned char *line, size_t line_size, const char *field, size_t &after) {
    const size_t wanted = strlen(field);
    for (size_t i = 0; i + wanted < line_size; ++i) {
        if (i && !is_space(line[i - 1]) && line[i - 1] != '(') continue;
        if (!equal_ascii(line + i, wanted, field) || !is_space(line[i + wanted])) continue;
        after = i + wanted;
        skip_spaces(line, line_size, after);
        return true;
    }
    return false;
}

bool response_flags(const RecordView &view, uint32_t &flags) {
    size_t at = 0;
    if (!field_position(view.line, view.line_size, "FLAGS", at) ||
        at >= view.line_size || view.line[at++] != '(') return false;
    uint32_t parsed = 0;
    bool closed = false;
    while (at < view.line_size) {
        skip_spaces(view.line, view.line_size, at);
        if (at < view.line_size && view.line[at] == ')') { ++at; closed = true; break; }
        const size_t start = at;
        while (at < view.line_size && !is_space(view.line[at]) && view.line[at] != ')') ++at;
        if (at == start) return false;
        const size_t length = at - start;
        if (equal_ascii(view.line + start, length, "\\Seen")) parsed |= ImapFlagSeen;
        else if (equal_ascii(view.line + start, length, "\\Answered")) parsed |= ImapFlagAnswered;
        else if (equal_ascii(view.line + start, length, "\\Flagged")) parsed |= ImapFlagFlagged;
        else if (equal_ascii(view.line + start, length, "\\Deleted")) parsed |= ImapFlagDeleted;
        else if (equal_ascii(view.line + start, length, "\\Draft")) parsed |= ImapFlagDraft;
    }
    if (!closed) return false;
    flags = parsed;
    return true;
}

int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t year_of_era = static_cast<uint32_t>(year - era * 400);
    const uint32_t adjusted_month = month > 2 ? month - 3 : month + 9;
    const uint32_t day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

bool parse_two_digits(const unsigned char *value, unsigned &parsed) {
    if (value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9') return false;
    parsed = static_cast<unsigned>(value[0] - '0') * 10 + static_cast<unsigned>(value[1] - '0');
    return true;
}

bool response_internal_date(const RecordView &view, int64_t &unix_seconds) {
    size_t at = 0;
    if (!field_position(view.line, view.line_size, "INTERNALDATE", at) ||
        at >= view.line_size || view.line[at++] != '"' || view.line_size - at < 27) return false;
    const unsigned char *date = view.line + at;
    if (date[2] != '-' || date[6] != '-' || date[11] != ' ' || date[14] != ':' ||
        date[17] != ':' || date[20] != ' ' || (date[21] != '+' && date[21] != '-') ||
        date[26] != '"') return false;
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    unsigned day = 0, hour = 0, minute = 0, second = 0, zone_hour = 0, zone_minute = 0;
    if (!parse_two_digits(date, day) || !parse_two_digits(date + 12, hour) ||
        !parse_two_digits(date + 15, minute) || !parse_two_digits(date + 18, second) ||
        !parse_two_digits(date + 22, zone_hour) || !parse_two_digits(date + 24, zone_minute)) return false;
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (!memcmp(date + 3, months[i], 3)) { month = i + 1; break; }
    uint64_t year_value = 0;
    if (!month || !parse_uint(date + 7, 4, 9999, year_value) || year_value < 1 ||
        hour > 23 || minute > 59 || second > 60 || zone_hour > 23 || zone_minute > 59) return false;
    const int32_t year = static_cast<int32_t>(year_value);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const unsigned month_lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned max_day = month_lengths[month - 1] + (month == 2 && leap ? 1u : 0u);
    if (!day || day > max_day) return false;
    const int64_t days = days_from_civil(year, month, day);
    const int64_t local_seconds = days * 86400 + static_cast<int64_t>(hour * 3600 + minute * 60 + second);
    const int64_t zone_seconds = static_cast<int64_t>(zone_hour * 3600 + zone_minute * 60) *
        (date[21] == '+' ? 1 : -1);
    unix_seconds = local_seconds - zone_seconds;
    return true;
}

bool response_size(const RecordView &view, uint64_t &message_size) {
    size_t at = 0;
    if (!field_position(view.line, view.line_size, "RFC822.SIZE", at)) return false;
    const size_t start = at;
    while (at < view.line_size && view.line[at] >= '0' && view.line[at] <= '9') ++at;
    return parse_uint(view.line + start, at - start, UINT64_MAX, message_size);
}

struct FetchCollector {
    const MessageKey *requested;
    size_t requested_count;
    Array<ImapMessage> *messages;
    bool headers_only;
};

bool fetch_handler(void *opaque, const unsigned char *record, size_t record_size, Text &error) {
    RecordView view;
    if (!view_record(record, record_size, view)) {
        error.set("malformed IMAP FETCH response");
        return false;
    }
    if (!starts_ascii(view.line, view.line_size, "* ")) return true;
    size_t response_at = 2;
    while (response_at < view.line_size && view.line[response_at] >= '0' && view.line[response_at] <= '9') ++response_at;
    if (response_at == 2 || response_at >= view.line_size || !is_space(view.line[response_at])) return true;
    skip_spaces(view.line, view.line_size, response_at);
    if (response_at + 5 > view.line_size || !equal_ascii(view.line + response_at, 5, "FETCH")) return true;
    uint32_t uid = 0;
    if (!response_uid(view, uid)) {
        error.set("FETCH response did not include a UID");
        return false;
    }
    FetchCollector *collector = static_cast<FetchCollector *>(opaque);
    size_t wanted = SIZE_MAX;
    for (size_t i = 0; i < collector->requested_count; ++i)
        if (collector->requested[i].uid == uid) { wanted = i; break; }
    if (wanted == SIZE_MAX) return true;
    if (!view.literal || view.literal_size > (collector->headers_only ? MaxImapHeaderBytes : MaxRawMessageBytes)) {
        error.set(collector->headers_only ? "FETCH response is missing a bounded header literal" :
            "FETCH response is missing a bounded message literal");
        return false;
    }
    for (size_t i = 0; i < collector->messages->size(); ++i)
        if ((*collector->messages)[i].key.uid == uid) return true;
    ImapMessage message;
    message.key = collector->requested[wanted];
    if (!response_flags(view, message.flags) || !response_internal_date(view, message.internal_date) ||
        !response_size(view, message.size) || message.size < view.literal_size ||
        !message.raw.set(view.literal, view.literal_size)) {
        error.set("malformed FETCH metadata or out of memory while collecting message data");
        return false;
    }
    message.headers_only = collector->headers_only;
    if (!collector->messages->push(static_cast<ImapMessage &&>(message))) {
        error.set("out of memory while collecting IMAP message data");
        return false;
    }
    return true;
}

bool fetch_messages(Stream &stream, ImapState &state, const Account &account,
    const MessageKey *keys, size_t key_count, bool headers_only,
    Array<ImapMessage> &messages, const OperationContext &operation, Text &error) {
    Array<ImapMessage> next;
    if (!key_count) { messages = static_cast<Array<ImapMessage> &&>(next); return true; }
    const FolderKey &folder = keys[0].folder;
    for (size_t i = 0; i < key_count; ++i) {
        if (keys[i].folder != folder || folder.account_id != account.id || !keys[i].uid ||
            !keys[i].uidvalidity || keys[i].uidvalidity != keys[0].uidvalidity) {
            error.set("IMAP FETCH keys do not identify one current folder version");
            return false;
        }
        for (size_t j = 0; j < i; ++j) if (keys[j].uid == keys[i].uid) {
            error.set("IMAP FETCH request contains a duplicate UID");
            return false;
        }
    }
    if (!ensure_selected(stream, state, account.id, folder, operation, error) ||
        state.selected_uidvalidity != keys[0].uidvalidity) {
        if (error.empty()) error.set("IMAP FETCH UIDVALIDITY is stale");
        return false;
    }
    WireBuffer command(MaxImapLineBytes);
    if (!command.append("UID FETCH ", 10)) { error.set("could not prepare IMAP FETCH command"); return false; }
    for (size_t i = 0; i < key_count; ++i) {
        char uid[16];
        const int length = snprintf(uid, sizeof(uid), "%u", keys[i].uid);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(uid) ||
            (i && !command.append(",", 1)) || !command.append(uid, static_cast<size_t>(length))) {
            error.set("could not prepare IMAP FETCH UID set");
            return false;
        }
    }
    const char *items = headers_only ?
        " (UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER])" :
        " (UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[])";
    if (!command.append(items, strlen(items))) {
        error.set("could not prepare IMAP FETCH data items");
        return false;
    }
    FetchCollector collector = {keys, key_count, &next, headers_only};
    size_t response_budget = MaxImapFetchResponseBytes;
    const size_t per_literal = headers_only ? MaxImapHeaderBytes : MaxRawMessageBytes;
    if (!run_command(stream, state, command.data(), command.size(), operation, error,
        fetch_handler, &collector, per_literal, &response_budget)) return false;
    if (next.size() != key_count) {
        error.set("one or more requested IMAP UIDs disappeared during FETCH");
        return false;
    }
    messages = static_cast<Array<ImapMessage> &&>(next);
    return true;
}

bool append_flag_list(WireBuffer &command, uint32_t flags) {
    struct Flag { uint32_t bit; const char *name; };
    static const Flag known[] = {{ImapFlagAnswered, "\\Answered"}, {ImapFlagFlagged, "\\Flagged"},
        {ImapFlagDeleted, "\\Deleted"}, {ImapFlagSeen, "\\Seen"}, {ImapFlagDraft, "\\Draft"}};
    if (flags & ~(ImapFlagSeen | ImapFlagAnswered | ImapFlagFlagged | ImapFlagDeleted | ImapFlagDraft)) return false;
    if (!command.append("(", 1)) return false;
    bool first = true;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!(flags & known[i].bit)) continue;
        if (!first && !command.append(" ", 1)) return false;
        if (!command.append(known[i].name, strlen(known[i].name))) return false;
        first = false;
    }
    return command.append(")", 1);
}

bool make_single_uid_command(const char *verb, uint32_t uid, const FolderKey *destination,
    WireBuffer &command, Text &error) {
    char number[16];
    const int length = snprintf(number, sizeof(number), "%u", uid);
    if (!uid || length <= 0 || static_cast<size_t>(length) >= sizeof(number) ||
        !command.append("UID ", 4) || !command.append(verb, strlen(verb)) || !command.append(" ", 1) ||
        !command.append(number, static_cast<size_t>(length))) {
        error.set("invalid IMAP UID command");
        return false;
    }
    if (destination) {
        Text name = destination->server_name;
        if (!valid_wire_name(name)) { error.set("invalid IMAP destination folder"); return false; }
        if (canonical_inbox(name)) name.set("INBOX");
        if (!command.append(" ", 1) || !append_quoted(command, name)) {
            error.set("invalid IMAP destination folder");
            return false;
        }
    }
    return true;
}

bool copy_or_move_command(Stream &stream, ImapState &state, const MessageKey &key,
    const FolderKey &destination, bool copy_only, const OperationContext &context, Text &error) {
    WireBuffer command(MaxImapLineBytes);
    if (!make_single_uid_command(copy_only ? "COPY" : "MOVE", key.uid,
        &destination, command, error)) return false;
    return run_command(stream, state, command.data(), command.size(), context, error);
}

} // namespace

bool encode_modified_utf7(const char *utf8, size_t length, Text &wire_name, Text &error) {
    error.clear();
    if ((!utf8 && length) || length > MaxEditableTextBytes || !valid_utf8(utf8, length)) {
        error.set("invalid or oversized UTF-8 folder name");
        return false;
    }
    WireBuffer output(MaxEditableTextBytes);
    WireBuffer shifted(MaxEditableTextBytes);
    size_t at = 0;
    while (at < length) {
        uint32_t codepoint = 0;
        if (!decode_utf8_scalar(reinterpret_cast<const unsigned char *>(utf8), length, at, codepoint)) {
            error.set("invalid UTF-8 folder name");
            return false;
        }
        if (codepoint >= 0x20 && codepoint <= 0x7e) {
            if (!shifted.size()) {
                if (codepoint == '&') {
                    if (!output.append("&-", 2)) { error.set("folder name exceeds the wire limit"); return false; }
                } else if (!output.append_byte(static_cast<unsigned char>(codepoint))) {
                    error.set("folder name exceeds the wire limit"); return false;
                }
            } else {
                if (!output.append_byte('&')) { error.set("folder name exceeds the wire limit"); return false; }
                if (!append_base64_u16(output, shifted.data(), shifted.size())) {
                    error.set("folder name exceeds the wire limit"); return false;
                }
                if (!output.append_byte('-')) { error.set("folder name exceeds the wire limit"); return false; }
                shifted = WireBuffer(MaxEditableTextBytes);
                if (codepoint == '&') {
                    if (!output.append("&-", 2)) { error.set("folder name exceeds the wire limit"); return false; }
                } else if (!output.append_byte(static_cast<unsigned char>(codepoint))) {
                    error.set("folder name exceeds the wire limit"); return false;
                }
            }
        } else {
            uint32_t units[2];
            size_t unit_count = 1;
            if (codepoint <= 0xffff) units[0] = codepoint;
            else {
                codepoint -= 0x10000;
                units[0] = 0xd800 + (codepoint >> 10);
                units[1] = 0xdc00 + (codepoint & 0x3ff);
                unit_count = 2;
            }
            for (size_t i = 0; i < unit_count; ++i) {
                const unsigned char pair[2] = {static_cast<unsigned char>(units[i] >> 8),
                    static_cast<unsigned char>(units[i])};
                if (!shifted.append(pair, sizeof(pair))) { error.set("folder name exceeds the wire limit"); return false; }
            }
        }
    }
    if (shifted.size()) {
        if (!output.append_byte('&')) { error.set("folder name exceeds the wire limit"); return false; }
        if (!append_base64_u16(output, shifted.data(), shifted.size())) {
            error.set("folder name exceeds the wire limit"); return false;
        }
        if (!output.append_byte('-')) { error.set("folder name exceeds the wire limit"); return false; }
    }
    if (!output.text(wire_name)) { error.set("out of memory while encoding folder name"); return false; }
    return true;
}

ImapSession::~ImapSession() { close(); }

bool ImapSession::connect(const Account &account, const OperationContext &context, Text &error) {
    close();
    error.clear();
    if (account.incoming_protocol != IncomingProtocol::Imap || account.incoming_host.empty() ||
        !account.incoming_port || account.incoming_port > 65535) {
        error.set("invalid IMAP account configuration");
        return false;
    }
    void *memory = malloc(sizeof(ImapState));
    if (!memory) { error.set("out of memory while creating IMAP session"); return false; }
    ImapState *state = new (memory, Placement{}) ImapState();
    protocol_state_ = state;
    account_ = account;
    OperationContext connect_context = with_timeout(context, account.connect_timeout_ms);
    if (!stream_.connect(account.incoming_host.c_str(), account.incoming_port,
        account.incoming_security, account.custom_ca_file.empty() ? nullptr : account.custom_ca_file.c_str(),
        connect_context, error)) {
        close();
        return false;
    }
    if (!read_greeting(stream_, *state, connect_context, error) ||
        !query_capabilities(stream_, *state, connect_context, error)) {
        close();
        return false;
    }
    if (account.incoming_security == TransportSecurity::StartTls) {
        if (!state->capability_starttls) {
            error.set("server does not offer the required IMAP STARTTLS extension");
            close();
            return false;
        }
        if (!run_command(stream_, *state, "STARTTLS", connect_context, error) ||
            !stream_.start_tls(connect_context, error)) {
            close();
            return false;
        }
        state->input_at = state->input_size = 0;
        if (!query_capabilities(stream_, *state, connect_context, error)) {
            close();
            return false;
        }
    }
    if (state->capability_id &&
        !run_command(stream_, *state, "ID (\"name\" \"ReliefMail\" \"version\" \"4\")",
            connect_context, error, id_handler, nullptr)) {
        close();
        return false;
    }
    connected_ = true;
    error.clear();
    return true;
}

bool ImapSession::login(SecretView password, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !state || !stream_.secure()) {
        error.set("IMAP authentication requires a connected verified TLS session");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!authenticate_login(stream_, *state, account_, password, operation, error)) return false;
    /* Capabilities can change after authentication; never use the stale
     * pre-auth snapshot to choose MOVE or UIDPLUS behavior. */
    if (!query_capabilities(stream_, *state, operation, error)) {
        if (error.empty()) error.set("IMAP authenticated capability refresh failed");
        close();
        return false;
    }
    state->authenticated = true;
    authenticated_ = true;
    return true;
}

void ImapSession::close() {
    stream_.close();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    protocol_state_ = nullptr;
    if (state) {
        state->~ImapState();
        free(state);
    }
    connected_ = false;
    authenticated_ = false;
    account_ = Account();
}

bool ImapSession::list(FolderList &folders, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state) {
        error.set("IMAP listing requires an authenticated session");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    FolderList next;
    FolderCollector collector = {account_.id, &next};
    if (!run_command(stream_, *state, "LIST \"\" \"*\"", operation, error,
        list_handler, &collector, MaxEditableTextBytes)) return false;
    for (size_t i = 0; i < next.size(); ++i) {
        if (!next[i].selectable) continue;
        Text command_error;
        WireBuffer command(MaxImapLineBytes);
        if (!command.append("STATUS ", 7) || !append_quoted(command, next[i].key.server_name) ||
            !command.append(" (MESSAGES UNSEEN)", 18)) {
            error.set("could not prepare IMAP STATUS command");
            return false;
        }
        StatusCollector status = {&next, i};
        if (!run_command(stream_, *state, command.data(), command.size(), operation,
            command_error, status_handler, &status, MaxEditableTextBytes)) {
            error = command_error.empty() ? Text() : command_error;
            if (error.empty()) error.set("IMAP folder STATUS failed");
            return false;
        }
    }
    folders = static_cast<FolderList &&>(next);
    return true;
}

bool ImapSession::select(const FolderKey &folder, Folder &selected,
                         const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id) {
        error.set("IMAP SELECT requires a matching authenticated account");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    Folder next;
    if (!select_mailbox(stream_, *state, folder, next, operation, error)) return false;
    selected = static_cast<Folder &&>(next);
    return true;
}

bool ImapSession::search(const FolderKey &folder, ImapSearchMode mode, Array<MessageKey> &keys,
                         const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id) {
        error.set("IMAP SEARCH requires a matching authenticated account");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!ensure_selected(stream_, *state, account_.id, folder, operation, error)) return false;
    const char *criterion = mode == ImapSearchMode::All ? "ALL" :
        mode == ImapSearchMode::Unseen ? "UNSEEN" : mode == ImapSearchMode::Flagged ? "FLAGGED" : nullptr;
    if (!criterion) { error.set("unsupported IMAP search mode"); return false; }
    WireBuffer command(MaxImapLineBytes);
    if (!command.append("UID SEARCH ", 11) || !command.append(criterion, strlen(criterion))) {
        error.set("could not prepare IMAP SEARCH command");
        return false;
    }
    Array<MessageKey> next;
    SearchCollector collector = {account_.id, &folder, state->selected_uidvalidity, &next};
    if (!run_command(stream_, *state, command.data(), command.size(), operation,
        error, search_handler, &collector)) return false;
    keys = static_cast<Array<MessageKey> &&>(next);
    return true;
}

bool ImapSession::search_message_id(const FolderKey &folder, const Text &message_id,
    Array<MessageKey> &keys, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id ||
        !valid_message_id(message_id)) {
        error.set("invalid IMAP Message-ID search identity or value");
        return false;
    }
    WireBuffer criterion(MaxImapLineBytes);
    const char *header_criterion = "HEADER Message-ID ";
    if (!criterion.append(header_criterion, strlen(header_criterion)) ||
        !append_search_quoted(criterion, message_id)) {
        error.set("could not prepare bounded IMAP Message-ID search");
        return false;
    }
    const OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    return search_criterion(stream_, *state, account_, folder, criterion.data(),
        criterion.size(), false, keys, operation, error);
}

bool ImapSession::search_text(const FolderKey &folder, ImapTextSearchField field, const Text &text,
    Array<MessageKey> &keys, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id) {
        error.set("IMAP text search requires a matching authenticated account");
        return false;
    }
    const char *field_name = field == ImapTextSearchField::From ? "FROM " :
        field == ImapTextSearchField::Subject ? "SUBJECT " :
        field == ImapTextSearchField::Body ? "BODY " : nullptr;
    if (!field_name) { error.set("unsupported IMAP text search field"); return false; }
    WireBuffer criterion(MaxImapLineBytes);
    if (!criterion.append(field_name, strlen(field_name)) || !append_search_quoted(criterion, text)) {
        error.set("invalid, unbounded, or non-UTF-8 IMAP search text");
        return false;
    }
    const OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    return search_criterion(stream_, *state, account_, folder, criterion.data(),
        criterion.size(), contains_non_ascii(text), keys, operation, error);
}

bool ImapSession::fetch(const MessageKey *keys, size_t key_count, Array<ImapMessage> &messages,
                        const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || (!keys && key_count) || key_count > MaxImapUidBatch) {
        error.set("invalid or oversized IMAP FETCH request");
        return false;
    }
    const OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    return fetch_messages(stream_, *state, account_, keys, key_count, false,
        messages, operation, error);
}

bool ImapSession::fetch_headers(const MessageKey *keys, size_t key_count,
    Array<ImapMessage> &messages, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || (!keys && key_count) || key_count > MaxImapUidBatch) {
        error.set("invalid or oversized IMAP header FETCH request");
        return false;
    }
    const OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    return fetch_messages(stream_, *state, account_, keys, key_count, true,
        messages, operation, error);
}

bool ImapSession::store(const MessageKey &key, uint32_t add_flags, uint32_t remove_flags,
                        const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || key.folder.account_id != account_.id ||
        !key.uid || !key.uidvalidity || (add_flags & remove_flags)) {
        error.set("invalid IMAP STORE identity or flags");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!ensure_selected(stream_, *state, account_.id, key.folder, operation, error) ||
        state->selected_uidvalidity != key.uidvalidity) {
        if (error.empty()) error.set("IMAP STORE UIDVALIDITY is stale");
        return false;
    }
    const uint32_t values[] = {add_flags, remove_flags};
    const char *verbs[] = {" +FLAGS.SILENT ", " -FLAGS.SILENT "};
    for (size_t i = 0; i < 2; ++i) {
        if (!values[i]) continue;
        WireBuffer command(MaxImapLineBytes);
        char uid[16];
        const int length = snprintf(uid, sizeof(uid), "%u", key.uid);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(uid) ||
            !command.append("UID STORE ", 10) || !command.append(uid, static_cast<size_t>(length)) ||
            !command.append(verbs[i], strlen(verbs[i])) || !append_flag_list(command, values[i])) {
            error.set("invalid IMAP STORE flags");
            return false;
        }
        if (!run_command(stream_, *state, command.data(), command.size(), operation, error)) return false;
    }
    return true;
}

bool ImapSession::delete_message(const MessageKey &key, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || key.folder.account_id != account_.id ||
        !key.uid || !key.uidvalidity) {
        error.set("invalid IMAP targeted delete identity");
        return false;
    }
    const OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!ensure_selected(stream_, *state, account_.id, key.folder, operation, error) ||
        state->selected_uidvalidity != key.uidvalidity) {
        if (error.empty()) error.set("IMAP delete UIDVALIDITY is stale");
        return false;
    }
    if (!state->capability_uidplus) {
        error.set("targeted IMAP deletion requires UIDPLUS; unrestricted EXPUNGE is unsupported");
        return false;
    }
    if (!store(key, ImapFlagDeleted, 0, operation, error)) return false;
    WireBuffer command(MaxImapLineBytes);
    char uid[16];
    const int length = snprintf(uid, sizeof(uid), "%u", key.uid);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(uid) ||
        !command.append("UID EXPUNGE ", 12) || !command.append(uid, static_cast<size_t>(length))) {
        error.set("could not prepare exact UID EXPUNGE command");
        return false;
    }
    return run_command(stream_, *state, command.data(), command.size(), operation, error);
}

bool ImapSession::copy(const MessageKey &key, const FolderKey &destination,
                       const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || key.folder.account_id != account_.id ||
        destination.account_id != account_.id || !key.uid || !key.uidvalidity) {
        error.set("invalid IMAP COPY identity");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!ensure_selected(stream_, *state, account_.id, key.folder, operation, error) ||
        state->selected_uidvalidity != key.uidvalidity) {
        if (error.empty()) error.set("IMAP COPY UIDVALIDITY is stale");
        return false;
    }
    return copy_or_move_command(stream_, *state, key, destination, true, operation, error);
}

bool ImapSession::move(const MessageKey &key, const FolderKey &destination,
                       const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || key.folder.account_id != account_.id ||
        destination.account_id != account_.id || !key.uid || !key.uidvalidity) {
        error.set("invalid IMAP MOVE identity");
        return false;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    if (!ensure_selected(stream_, *state, account_.id, key.folder, operation, error) ||
        state->selected_uidvalidity != key.uidvalidity) {
        if (error.empty()) error.set("IMAP MOVE UIDVALIDITY is stale");
        return false;
    }
    if (state->capability_move)
        return copy_or_move_command(stream_, *state, key, destination, false, operation, error);
    if (!state->capability_uidplus) {
        error.set("safe IMAP MOVE fallback requires UIDPLUS");
        return false;
    }
    if (!copy_or_move_command(stream_, *state, key, destination, true, operation, error) ||
        !store(key, ImapFlagDeleted, 0, operation, error)) return false;
    WireBuffer command(MaxImapLineBytes);
    char uid[16];
    const int length = snprintf(uid, sizeof(uid), "%u", key.uid);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(uid) ||
        !command.append("UID EXPUNGE ", 12) || !command.append(uid, static_cast<size_t>(length))) {
        error.set("could not prepare targeted UID EXPUNGE");
        return false;
    }
    return run_command(stream_, *state, command.data(), command.size(), operation, error);
}

bool write_source(Stream &stream, const MessageSource &message,
                  const OperationContext &context, Text &error, bool &attempted_write) {
    if (!message.read || !message.size || message.size > MaxRawMessageBytes) {
        error.set("invalid or oversized immutable message source");
        return false;
    }
    unsigned char chunk[3072];
    size_t offset = 0;
    while (offset < message.size) {
        size_t read_size = 0;
        const size_t capacity = message.size - offset < sizeof(chunk) ? message.size - offset : sizeof(chunk);
        if (!message.read(message.context, offset, chunk, capacity, &read_size) ||
            !read_size || read_size > capacity) {
            error.set("immutable message source failed during IMAP APPEND");
            return false;
        }
        attempted_write = true;
        if (!write_all(stream, chunk, read_size, context, error)) return false;
        offset += read_size;
    }
    return true;
}

ImapAppendOutcome ImapSession::append(const FolderKey &folder, const MessageSource &message,
    const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id ||
        !valid_wire_name(folder.server_name) || !message.read || !message.size ||
        message.size > MaxRawMessageBytes) {
        error.set("invalid IMAP APPEND request");
        return ImapAppendOutcome::Rejected;
    }
    OperationContext operation = with_timeout(context, account_.io_timeout_ms);
    Text mailbox = folder.server_name;
    if (canonical_inbox(mailbox)) mailbox.set("INBOX");
    WireBuffer command(MaxImapLineBytes);
    char size_text[32];
    const int size_length = snprintf(size_text, sizeof(size_text), "%zu", message.size);
    if (size_length <= 0 || static_cast<size_t>(size_length) >= sizeof(size_text) ||
        !command.append("APPEND ", 7) || !append_quoted(command, mailbox) ||
        !command.append(" {", 2) || !command.append(size_text, static_cast<size_t>(size_length)) ||
        !command.append("}", 1)) {
        error.set("could not prepare IMAP APPEND command");
        return ImapAppendOutcome::Rejected;
    }
    char tag[16];
    if (!send_command(stream_, *state, command.data(), command.size(), tag, operation, error))
        return ImapAppendOutcome::Rejected;
    ImapResult result = read_command_result(stream_, *state, tag, operation, error);
    if (result == ImapResult::Rejected) { error.set("IMAP APPEND was rejected before literal transfer"); return ImapAppendOutcome::Rejected; }
    if (result != ImapResult::Continuation) {
        if (error.empty()) error.set("IMAP APPEND continuation was not received");
        return ImapAppendOutcome::Rejected;
    }
    bool attempted_write = false;
    if (!write_source(stream_, message, operation, error, attempted_write) ||
        !write_all(stream_, reinterpret_cast<const unsigned char *>("\r\n"), 2, operation, error)) {
        return attempted_write ? ImapAppendOutcome::Uncertain : ImapAppendOutcome::Rejected;
    }
    result = read_command_result(stream_, *state, tag, operation, error);
    if (result == ImapResult::Ok) return ImapAppendOutcome::Accepted;
    if (result == ImapResult::Rejected) {
        error.set("IMAP APPEND was rejected after literal transfer");
        return ImapAppendOutcome::Rejected;
    }
    if (error.empty()) error.set("IMAP APPEND completion is uncertain");
    return ImapAppendOutcome::Uncertain;
}

bool ImapSession::folder_operation(ImapFolderOperation operation_kind, const FolderKey &folder,
    const Text &new_display_name, const OperationContext &context, Text &error) {
    error.clear();
    ImapState *state = static_cast<ImapState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || folder.account_id != account_.id) {
        error.set("folder operation requires a matching authenticated account");
        return false;
    }
    Text old_name = folder.server_name;
    if (operation_kind != ImapFolderOperation::Create && !valid_wire_name(old_name)) {
        error.set("invalid IMAP folder identity");
        return false;
    }
    if (canonical_inbox(old_name)) old_name.set("INBOX");
    const char *verb = nullptr;
    switch (operation_kind) {
    case ImapFolderOperation::Subscribe: verb = "SUBSCRIBE "; break;
    case ImapFolderOperation::Unsubscribe: verb = "UNSUBSCRIBE "; break;
    case ImapFolderOperation::Create: verb = "CREATE "; break;
    case ImapFolderOperation::Rename: verb = "RENAME "; break;
    case ImapFolderOperation::Delete: verb = "DELETE "; break;
    default: error.set("unsupported IMAP folder operation"); return false;
    }
    Text new_wire;
    if (operation_kind == ImapFolderOperation::Create || operation_kind == ImapFolderOperation::Rename) {
        if (!encode_modified_utf7(new_display_name.c_str(), new_display_name.size(), new_wire, error)) return false;
        if (canonical_inbox(new_wire)) new_wire.set("INBOX");
    }
    WireBuffer command(MaxImapLineBytes);
    bool prepared = false;
    if (operation_kind == ImapFolderOperation::Create) {
        prepared = command.append("CREATE ", 7) && append_quoted(command, new_wire);
    } else {
        prepared = command.append(verb, strlen(verb)) && append_quoted(command, old_name);
        if (prepared && operation_kind == ImapFolderOperation::Rename)
            prepared = command.append(" ", 1) && append_quoted(command, new_wire);
    }
    if (!prepared) {
        error.set("invalid or oversized IMAP folder operation");
        return false;
    }
    OperationContext io = with_timeout(context, account_.io_timeout_ms);
    if (!run_command(stream_, *state, command.data(), command.size(), io, error)) return false;
    if (operation_kind == ImapFolderOperation::Rename || operation_kind == ImapFolderOperation::Delete) {
        state->selected_name.clear();
        state->selected_uidvalidity = 0;
    }
    return true;
}

bool ImapSession::capabilities(ImapCapabilities &snapshot) const {
    const ImapState *state = static_cast<const ImapState *>(protocol_state_);
    if (!connected_ || !state) return false;
    snapshot.starttls = state->capability_starttls;
    snapshot.auth_plain = state->capability_plain;
    snapshot.auth_login = state->capability_login;
    snapshot.id = state->capability_id;
    snapshot.move = state->capability_move;
    snapshot.uidplus = state->capability_uidplus;
    return true;
}

bool ImapSession::diagnostics(TransportDiagnostics &snapshot) const {
    return connected_ && stream_.diagnostics(snapshot);
}

} // namespace rmail
