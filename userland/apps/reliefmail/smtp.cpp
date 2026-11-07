#include "smtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace rmail {
namespace {

constexpr size_t MaxSmtpReplyLineBytes = 4096;
constexpr size_t MaxSmtpCommandBytes = 4096;
constexpr size_t SmtpReadChunk = 2048;
constexpr size_t MaxSmtpReplyLines = 32;

void secure_zero(void *buffer, size_t size) {
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(buffer);
    while (size--) *bytes++ = 0;
}

struct SmtpState {
    unsigned char input[SmtpReadChunk];
    size_t input_at = 0;
    size_t input_size = 0;
    bool authenticated = false;
    bool starttls = false;
    bool auth_plain = false;
    bool auth_login = false;
    bool size = false;
    uint64_t maximum_size = 0;
    bool smtp_utf8 = false;
    bool eight_bit_mime = false;
};

class WireBuffer {
    unsigned char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
    bool sensitive_ = false;
public:
    explicit WireBuffer(size_t limit) : limit_(limit) {}
    WireBuffer(const WireBuffer &) = delete;
    WireBuffer &operator=(const WireBuffer &) = delete;
    ~WireBuffer() {
        if (sensitive_ && bytes_) secure_zero(bytes_, capacity_);
        free(bytes_);
    }
    void mark_sensitive() { sensitive_ = true; }
    const unsigned char *data() const { return bytes_; }
    size_t size() const { return size_; }
    bool append(const void *source, size_t length) {
        if ((length && !source) || length > limit_ - size_) return false;
        if (!length) return true;
        const size_t wanted = size_ + length;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 128;
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
};

bool equal_ascii(const char *left, size_t length, const char *right) {
    if (length != strlen(right)) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'a' && a <= 'z') a = static_cast<unsigned char>(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z') b = static_cast<unsigned char>(b - ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

bool is_space(unsigned char byte) { return byte == ' ' || byte == '\t'; }

void skip_spaces(const char *line, size_t length, size_t &at) {
    while (at < length && is_space(static_cast<unsigned char>(line[at]))) ++at;
}

bool parse_uint(const char *bytes, size_t length, uint64_t &value) {
    if (!length) return false;
    uint64_t parsed = 0;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] < '0' || bytes[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(bytes[i] - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool check_operation(const OperationContext &operation, Text &error) {
    if (operation.is_cancelled && operation.is_cancelled(operation.cancellation_context)) {
        error.set("network operation cancelled");
        return false;
    }
    if (operation.deadline_monotonic_ms != UINT64_MAX) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
            error.set("network clock is unavailable");
            return false;
        }
        const uint64_t ms = static_cast<uint64_t>(now.tv_sec) * 1000 +
            static_cast<uint64_t>(now.tv_nsec / 1000000);
        if (ms >= operation.deadline_monotonic_ms) {
            error.set("network operation deadline expired");
            return false;
        }
    }
    return true;
}

bool write_all(Stream &stream, const unsigned char *bytes, size_t size,
    const OperationContext &operation, Text &error) {
    size_t offset = 0;
    while (offset < size) {
        size_t written = 0;
        if (!stream.write(bytes + offset, size - offset, &written, operation, error) ||
            !written || written > size - offset) {
            if (error.empty()) error.set("SMTP write failed");
            return false;
        }
        offset += written;
    }
    return true;
}

class SmtpReader {
    Stream &stream_;
    SmtpState &state_;
    const OperationContext &operation_;
    Text &error_;
    bool byte(unsigned char &result) {
        if (state_.input_at == state_.input_size) {
            if (!check_operation(operation_, error_)) return false;
            size_t received = 0;
            if (!stream_.read(state_.input, sizeof(state_.input), &received, operation_, error_)) return false;
            if (!received || received > sizeof(state_.input)) {
                error_.set("SMTP connection closed while reading a reply");
                return false;
            }
            state_.input_at = 0;
            state_.input_size = received;
        }
        result = state_.input[state_.input_at++];
        return true;
    }
public:
    SmtpReader(Stream &stream, SmtpState &state, const OperationContext &operation, Text &error)
        : stream_(stream), state_(state), operation_(operation), error_(error) {}
    bool line(char *output, size_t capacity, size_t &length) {
        length = 0;
        bool cr = false;
        for (;;) {
            unsigned char value = 0;
            if (!byte(value)) return false;
            if (cr) {
                if (value != '\n') { error_.set("malformed SMTP line ending"); return false; }
                output[length] = 0;
                return true;
            }
            if (value == '\r') { cr = true; continue; }
            if (value == '\n' || value == 0 || (value < 0x20 && value != '\t') || value == 0x7f) {
                error_.set("malformed SMTP reply text");
                return false;
            }
            if (length + 1 >= capacity) { error_.set("SMTP reply line exceeds 4 KiB"); return false; }
            output[length++] = static_cast<char>(value);
        }
    }
};

struct SmtpReply {
    int code = 0;
    size_t line_count = 0;
    char lines[MaxSmtpReplyLines][MaxSmtpReplyLineBytes + 1];
    size_t lengths[MaxSmtpReplyLines];
};

bool read_reply(Stream &stream, SmtpState &state, const OperationContext &operation,
    Text &error, SmtpReply &reply) {
    reply = SmtpReply();
    SmtpReader reader(stream, state, operation, error);
    int expected_code = 0;
    for (;;) {
        char line[MaxSmtpReplyLineBytes + 1];
        size_t length = 0;
        if (!reader.line(line, sizeof(line), length)) return false;
        if (length < 4 || line[0] < '0' || line[0] > '9' || line[1] < '0' || line[1] > '9' ||
            line[2] < '0' || line[2] > '9' || (line[3] != ' ' && line[3] != '-')) {
            error.set("malformed SMTP status line");
            return false;
        }
        const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
        if (!expected_code) expected_code = code;
        if (code != expected_code || reply.line_count == MaxSmtpReplyLines) {
            error.set("inconsistent or oversized SMTP multiline reply");
            return false;
        }
        const size_t content_size = length - 4;
        memcpy(reply.lines[reply.line_count], line + 4, content_size);
        reply.lines[reply.line_count][content_size] = 0;
        reply.lengths[reply.line_count] = content_size;
        ++reply.line_count;
        if (line[3] == ' ') {
            reply.code = code;
            return true;
        }
    }
}

bool send_command(Stream &stream, const unsigned char *command, size_t command_size,
    const OperationContext &operation, Text &error, bool sensitive = false) {
    if (!command || !command_size || command_size > MaxSmtpCommandBytes ||
        memchr(command, '\r', command_size) || memchr(command, '\n', command_size) ||
        memchr(command, 0, command_size)) {
        error.set("invalid or oversized SMTP command");
        return false;
    }
    WireBuffer line(MaxSmtpCommandBytes + 2);
    if (sensitive) line.mark_sensitive();
    if (!line.append(command, command_size) || !line.append("\r\n", 2)) {
        error.set("could not prepare SMTP command");
        return false;
    }
    return write_all(stream, line.data(), line.size(), operation, error);
}

bool exchange(Stream &stream, SmtpState &state, const unsigned char *command, size_t command_size,
    const OperationContext &operation, Text &error, int &reply_code, bool sensitive = false) {
    if (!send_command(stream, command, command_size, operation, error, sensitive)) return false;
    SmtpReply reply;
    if (!read_reply(stream, state, operation, error, reply)) return false;
    reply_code = reply.code;
    return true;
}

bool exchange(Stream &stream, SmtpState &state, const char *command,
    const OperationContext &operation, Text &error, int &reply_code) {
    return command && exchange(stream, state, reinterpret_cast<const unsigned char *>(command),
        strlen(command), operation, error, reply_code);
}

void reset_capabilities(SmtpState &state) {
    state.starttls = false;
    state.auth_plain = false;
    state.auth_login = false;
    state.size = false;
    state.maximum_size = 0;
    state.smtp_utf8 = false;
    state.eight_bit_mime = false;
}

void parse_capability_line(SmtpState &state, const char *line, size_t length) {
    size_t at = 0;
    skip_spaces(line, length, at);
    const size_t start = at;
    while (at < length && !is_space(static_cast<unsigned char>(line[at]))) ++at;
    if (at == start) return;
    const size_t keyword_size = at - start;
    const char *keyword = line + start;
    if (equal_ascii(keyword, keyword_size, "STARTTLS")) state.starttls = true;
    else if (equal_ascii(keyword, keyword_size, "SMTPUTF8")) state.smtp_utf8 = true;
    else if (equal_ascii(keyword, keyword_size, "8BITMIME")) state.eight_bit_mime = true;
    else if (equal_ascii(keyword, keyword_size, "SIZE")) {
        state.size = true;
        skip_spaces(line, length, at);
        size_t end = at;
        while (end < length && line[end] >= '0' && line[end] <= '9') ++end;
        if (end > at) parse_uint(line + at, end - at, state.maximum_size);
    } else if (equal_ascii(keyword, keyword_size, "AUTH")) {
        while (at < length) {
            skip_spaces(line, length, at);
            const size_t mechanism = at;
            while (at < length && !is_space(static_cast<unsigned char>(line[at]))) ++at;
            const size_t size = at - mechanism;
            if (equal_ascii(line + mechanism, size, "PLAIN")) state.auth_plain = true;
            else if (equal_ascii(line + mechanism, size, "LOGIN")) state.auth_login = true;
        }
    } else if (keyword_size > 5 && equal_ascii(keyword, 5, "AUTH=")) {
        const char *mechanism = keyword + 5;
        const size_t mechanism_size = keyword_size - 5;
        if (equal_ascii(mechanism, mechanism_size, "PLAIN")) state.auth_plain = true;
        else if (equal_ascii(mechanism, mechanism_size, "LOGIN")) state.auth_login = true;
    }
}

bool ehlo(Stream &stream, SmtpState &state, const OperationContext &operation, Text &error) {
    reset_capabilities(state);
    if (!send_command(stream, reinterpret_cast<const unsigned char *>("EHLO reliefmail.local"),
        strlen("EHLO reliefmail.local"), operation, error)) return false;
    SmtpReply reply;
    if (!read_reply(stream, state, operation, error, reply)) return false;
    if (reply.code != 250) { error.set("SMTP EHLO was rejected"); return false; }
    for (size_t i = 0; i < reply.line_count; ++i)
        parse_capability_line(state, reply.lines[i], reply.lengths[i]);
    return true;
}

bool make_base64(const unsigned char *input, size_t input_size, char **output, size_t &output_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if ((!input && input_size) || input_size > (SIZE_MAX - 2) / 4 * 3) return false;
    const size_t capacity = ((input_size + 2) / 3) * 4;
    char *encoded = static_cast<char *>(malloc(capacity + 1));
    if (!encoded) return false;
    size_t in = 0, out = 0;
    while (in < input_size) {
        const size_t remaining = input_size - in;
        const uint32_t a = input[in++];
        const uint32_t b = remaining > 1 ? input[in++] : 0;
        const uint32_t c = remaining > 2 ? input[in++] : 0;
        encoded[out++] = alphabet[a >> 2];
        encoded[out++] = alphabet[((a & 3) << 4) | (b >> 4)];
        encoded[out++] = remaining > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded[out++] = remaining > 2 ? alphabet[c & 63] : '=';
    }
    encoded[out] = 0;
    *output = encoded;
    output_size = out;
    return true;
}

bool username_for(const Account &account, const Text *&username) {
    username = account.smtp_username.empty() ? &account.username : &account.smtp_username;
    return username && !username->empty() && username->size() <= 1024;
}

bool valid_password(SecretView password) {
    if (!password.data || !password.size || password.size > 4096) return false;
    for (size_t i = 0; i < password.size; ++i)
        if (!password.data[i] || password.data[i] == '\r' || password.data[i] == '\n') return false;
    return true;
}

bool smtp_auth_plain(Stream &stream, SmtpState &state, const Account &account,
    SecretView password, const OperationContext &operation, Text &error) {
    const Text *username = nullptr;
    if (!username_for(account, username) || !valid_password(password) ||
        username->size() > SIZE_MAX - password.size - 2) {
        error.set("SMTP credentials are invalid");
        return false;
    }
    const size_t raw_size = username->size() + password.size + 2;
    unsigned char *raw = static_cast<unsigned char *>(malloc(raw_size));
    if (!raw) { error.set("out of memory while preparing SMTP authentication"); return false; }
    raw[0] = 0;
    memcpy(raw + 1, username->c_str(), username->size());
    raw[username->size() + 1] = 0;
    memcpy(raw + username->size() + 2, password.data, password.size);
    char *encoded = nullptr;
    size_t encoded_size = 0;
    const bool encoded_ok = make_base64(raw, raw_size, &encoded, encoded_size);
    secure_zero(raw, raw_size);
    free(raw);
    if (!encoded_ok) { error.set("could not prepare SMTP authentication"); return false; }
    WireBuffer command(MaxSmtpCommandBytes);
    command.mark_sensitive();
    const bool prepared = command.append("AUTH PLAIN ", 11) && command.append(encoded, encoded_size);
    secure_zero(encoded, encoded_size + 1);
    free(encoded);
    if (!prepared) { error.set("could not prepare SMTP authentication"); return false; }
    int code = 0;
    if (!exchange(stream, state, command.data(), command.size(), operation, error, code, true)) return false;
    if (code != 235) { error.set("SMTP authentication was rejected"); return false; }
    return true;
}

bool smtp_auth_login(Stream &stream, SmtpState &state, const Account &account,
    SecretView password, const OperationContext &operation, Text &error) {
    const Text *username = nullptr;
    if (!username_for(account, username) || !valid_password(password)) {
        error.set("SMTP credentials are invalid");
        return false;
    }
    int code = 0;
    if (!exchange(stream, state, "AUTH LOGIN", operation, error, code)) return false;
    if (code != 334) { error.set("SMTP LOGIN authentication was rejected"); return false; }
    char *user64 = nullptr, *password64 = nullptr;
    size_t user64_size = 0, password64_size = 0;
    if (!make_base64(reinterpret_cast<const unsigned char *>(username->c_str()), username->size(),
            &user64, user64_size) || !make_base64(password.data, password.size, &password64, password64_size)) {
        if (user64) { secure_zero(user64, user64_size + 1); free(user64); }
        if (password64) { secure_zero(password64, password64_size + 1); free(password64); }
        error.set("could not prepare SMTP LOGIN authentication");
        return false;
    }
    WireBuffer user_command(MaxSmtpCommandBytes);
    user_command.mark_sensitive();
    bool sent = user_command.append(user64, user64_size) &&
        send_command(stream, user_command.data(), user_command.size(), operation, error, true);
    user_command.wipe();
    secure_zero(user64, user64_size + 1); free(user64);
    if (!sent) { secure_zero(password64, password64_size + 1); free(password64); return false; }
    SmtpReply reply;
    if (!read_reply(stream, state, operation, error, reply)) {
        secure_zero(password64, password64_size + 1); free(password64); return false;
    }
    if (reply.code != 334) {
        secure_zero(password64, password64_size + 1); free(password64);
        error.set("SMTP LOGIN username was rejected");
        return false;
    }
    WireBuffer password_command(MaxSmtpCommandBytes);
    password_command.mark_sensitive();
    sent = password_command.append(password64, password64_size) &&
        send_command(stream, password_command.data(), password_command.size(), operation, error, true);
    password_command.wipe();
    secure_zero(password64, password64_size + 1); free(password64);
    if (!sent) return false;
    if (!read_reply(stream, state, operation, error, reply)) return false;
    if (reply.code != 235) { error.set("SMTP LOGIN authentication was rejected"); return false; }
    return true;
}

bool mailbox_text(const Address &address, Text &mailbox, Text &error) {
    Address bare = address;
    bare.display_name.clear();
    bare.display_name_quoted = false;
    if (!serialize_address(bare, mailbox, error)) return false;
    if (mailbox.empty() || mailbox.size() > 320) {
        error.set("SMTP mailbox exceeds the protocol limit");
        return false;
    }
    return true;
}

class DataWriter {
    Stream &stream_;
    const OperationContext &operation_;
    Text &error_;
    unsigned char buffer_[4096];
    size_t used_ = 0;
public:
    bool attempted_write = false;
    DataWriter(Stream &stream, const OperationContext &operation, Text &error)
        : stream_(stream), operation_(operation), error_(error) {}
    bool flush() {
        if (!used_) return true;
        attempted_write = true;
        const bool ok = write_all(stream_, buffer_, used_, operation_, error_);
        used_ = 0;
        return ok;
    }
    bool put(unsigned char byte) {
        if (used_ == sizeof(buffer_) && !flush()) return false;
        buffer_[used_++] = byte;
        return true;
    }
};

struct CanonicalStats {
    size_t size = 0;
    bool header_has_8bit = false;
    bool body_has_8bit = false;
    bool headers_done = false;
    bool line_start = true;
    bool pending_lf = false;
    size_t header_line_size = 0;
};

bool canonical_byte(CanonicalStats &stats, unsigned char byte, DataWriter *output, Text &error) {
    if (stats.size == MaxRawMessageBytes) {
        error.set("canonical SMTP message exceeds the configured message limit");
        return false;
    }
    ++stats.size;
    if (byte >= 0x80) {
        if (!stats.headers_done) stats.header_has_8bit = true;
        else stats.body_has_8bit = true;
    }
    if (!stats.headers_done) {
        if (stats.pending_lf && byte == '\n') {
            stats.pending_lf = false;
            if (!stats.header_line_size) stats.headers_done = true;
            else stats.header_line_size = 0;
        } else if (byte == '\r') {
            stats.pending_lf = true;
        } else {
            stats.pending_lf = false;
            if (byte != '\n') ++stats.header_line_size;
        }
    }
    stats.line_start = byte == '\n';
    if (output && !output->put(byte)) return false;
    return true;
}

/* Counts a stable source after canonical CRLF conversion. When a writer is
 * supplied, it emits dot-transparent DATA bytes while retaining only 4 KiB. */
bool normalize_source(const MessageSource &source, DataWriter *output,
    CanonicalStats &stats, Text &error) {
    if (!source.read || !source.size || source.size > MaxRawMessageBytes) {
        error.set("invalid or oversized immutable SMTP message source");
        return false;
    }
    unsigned char input[4096];
    size_t offset = 0;
    bool pending_cr = false;
    bool at_line_start = true;
    while (offset < source.size) {
        size_t count = 0;
        const size_t capacity = source.size - offset < sizeof(input) ? source.size - offset : sizeof(input);
        if (!source.read(source.context, offset, input, capacity, &count) || !count || count > capacity) {
            error.set("immutable SMTP message source failed during preflight or DATA");
            return false;
        }
        offset += count;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char byte = input[i];
            if (byte == 0 || (byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t') || byte == 0x7f) {
                error.set("SMTP message contains a prohibited control byte");
                return false;
            }
            if (pending_cr) {
                if (!canonical_byte(stats, '\r', output, error) ||
                    !canonical_byte(stats, '\n', output, error)) return false;
                at_line_start = true;
                pending_cr = false;
                if (byte == '\n') continue;
            }
            if (byte == '\r') { pending_cr = true; continue; }
            if (byte == '\n') {
                if (!canonical_byte(stats, '\r', output, error) ||
                    !canonical_byte(stats, '\n', output, error)) return false;
                at_line_start = true;
                continue;
            }
            if (output && at_line_start && byte == '.' && !output->put('.')) return false;
            if (!canonical_byte(stats, byte, output, error)) return false;
            at_line_start = false;
        }
    }
    if (pending_cr || !at_line_start) {
        if (!canonical_byte(stats, '\r', output, error) ||
            !canonical_byte(stats, '\n', output, error)) return false;
    }
    return !output || output->flush();
}

bool callback_allows(SendObserver &observer, SendStage stage) {
    return !observer.advance || observer.advance(observer.context, stage);
}

bool has_non_ascii(const Text &text) {
    for (size_t i = 0; i < text.size(); ++i)
        if (static_cast<unsigned char>(text.c_str()[i]) >= 0x80) return true;
    return false;
}

} // namespace

SmtpSession::~SmtpSession() { close(); }

bool SmtpSession::connect(const Account &account, const OperationContext &context, Text &error) {
    close();
    error.clear();
    if (account.outgoing_host.empty() || !account.outgoing_port || account.outgoing_port > 65535) {
        error.set("invalid SMTP account configuration");
        return false;
    }
    void *memory = malloc(sizeof(SmtpState));
    if (!memory) { error.set("out of memory while creating SMTP session"); return false; }
    SmtpState *state = new (memory, Placement{}) SmtpState();
    protocol_state_ = state;
    account_ = account;
    OperationContext operation = context;
    if (account.connect_timeout_ms) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0) {
            const uint64_t current = static_cast<uint64_t>(now.tv_sec) * 1000 +
                static_cast<uint64_t>(now.tv_nsec / 1000000);
            const uint64_t local = current > UINT64_MAX - account.connect_timeout_ms ? UINT64_MAX :
                current + account.connect_timeout_ms;
            if (operation.deadline_monotonic_ms == UINT64_MAX || local < operation.deadline_monotonic_ms)
                operation.deadline_monotonic_ms = local;
        }
    }
    if (!stream_.connect(account.outgoing_host.c_str(), account.outgoing_port,
        account.outgoing_security, account.custom_ca_file.empty() ? nullptr : account.custom_ca_file.c_str(),
        operation, error)) { close(); return false; }
    SmtpReply greeting;
    if (!read_reply(stream_, *state, operation, error, greeting) || greeting.code != 220) {
        if (error.empty()) error.set("SMTP server greeting was not accepted");
        close();
        return false;
    }
    if (!ehlo(stream_, *state, operation, error)) { close(); return false; }
    if (account.outgoing_security == TransportSecurity::StartTls) {
        if (!state->starttls) {
            error.set("server does not offer the required SMTP STARTTLS extension");
            close();
            return false;
        }
        int code = 0;
        if (!exchange(stream_, *state, "STARTTLS", operation, error, code) || code != 220) {
            if (error.empty()) error.set("SMTP STARTTLS was rejected");
            close();
            return false;
        }
        if (!stream_.start_tls(operation, error)) { close(); return false; }
        state->input_at = state->input_size = 0;
        if (!ehlo(stream_, *state, operation, error)) { close(); return false; }
    }
    connected_ = true;
    error.clear();
    return true;
}

bool SmtpSession::authenticate(SecretView password, const OperationContext &context, Text &error) {
    error.clear();
    SmtpState *state = static_cast<SmtpState *>(protocol_state_);
    if (!connected_ || !state || !stream_.secure() || state->authenticated || !valid_password(password)) {
        error.set("SMTP authentication requires verified TLS and valid credentials");
        return false;
    }
    OperationContext operation = context;
    if (account_.io_timeout_ms) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0) {
            const uint64_t current = static_cast<uint64_t>(now.tv_sec) * 1000 +
                static_cast<uint64_t>(now.tv_nsec / 1000000);
            const uint64_t local = current > UINT64_MAX - account_.io_timeout_ms ? UINT64_MAX :
                current + account_.io_timeout_ms;
            if (operation.deadline_monotonic_ms == UINT64_MAX || local < operation.deadline_monotonic_ms)
                operation.deadline_monotonic_ms = local;
        }
    }
    bool authenticated = false;
    if (state->auth_plain) authenticated = smtp_auth_plain(stream_, *state, account_, password, operation, error);
    else if (state->auth_login) authenticated = smtp_auth_login(stream_, *state, account_, password, operation, error);
    else error.set("SMTP server offers no supported AUTH PLAIN or LOGIN mechanism");
    if (authenticated) state->authenticated = authenticated_ = true;
    return authenticated;
}

void SmtpSession::close() {
    stream_.close();
    SmtpState *state = static_cast<SmtpState *>(protocol_state_);
    protocol_state_ = nullptr;
    if (state) { state->~SmtpState(); free(state); }
    connected_ = false;
    authenticated_ = false;
    account_ = Account();
}

SubmitOutcome SmtpSession::submit(const Envelope &envelope, const MessageSource &message,
    SendObserver &observer, const OperationContext &context, Text &error) {
    error.clear();
    SmtpState *state = static_cast<SmtpState *>(protocol_state_);
    if (!connected_ || !authenticated_ || !state || !stream_.secure()) {
        error.set("SMTP submission requires an authenticated verified TLS session");
        return SubmitOutcome::Rejected;
    }
    if (envelope.recipients.empty() || envelope.recipients.size() > MaxContactAddresses) {
        error.set("SMTP envelope has no recipients or exceeds the recipient limit");
        return SubmitOutcome::Rejected;
    }
    Text sender;
    if (!mailbox_text(envelope.sender, sender, error)) return SubmitOutcome::Rejected;
    bool envelope_has_utf8 = has_non_ascii(sender);
    for (size_t i = 0; i < envelope.recipients.size(); ++i) {
        Text recipient;
        if (!mailbox_text(envelope.recipients[i], recipient, error)) return SubmitOutcome::Rejected;
        envelope_has_utf8 = envelope_has_utf8 || has_non_ascii(recipient);
    }
    CanonicalStats measured;
    if (!normalize_source(message, nullptr, measured, error)) return SubmitOutcome::Rejected;
    const bool smtp_utf8_needed = envelope_has_utf8 || measured.header_has_8bit;
    if (smtp_utf8_needed && !state->smtp_utf8) {
        error.set("SMTPUTF8 is required for non-ASCII message headers or envelope addresses");
        return SubmitOutcome::Rejected;
    }
    if (measured.body_has_8bit && !state->eight_bit_mime) {
        error.set("8BITMIME is required for a non-ASCII message body");
        return SubmitOutcome::Rejected;
    }
    if (state->size && state->maximum_size && measured.size > state->maximum_size) {
        error.set("message exceeds the server-advertised SMTP SIZE limit");
        return SubmitOutcome::Rejected;
    }
    if (!callback_allows(observer, SendStage::SendingEnvelope)) {
        error.set("send observer refused the SMTP envelope stage");
        return SubmitOutcome::Rejected;
    }
    OperationContext operation = context;
    if (account_.io_timeout_ms) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0) {
            const uint64_t current = static_cast<uint64_t>(now.tv_sec) * 1000 +
                static_cast<uint64_t>(now.tv_nsec / 1000000);
            const uint64_t local = current > UINT64_MAX - account_.io_timeout_ms ? UINT64_MAX :
                current + account_.io_timeout_ms;
            if (operation.deadline_monotonic_ms == UINT64_MAX || local < operation.deadline_monotonic_ms)
                operation.deadline_monotonic_ms = local;
        }
    }
    WireBuffer mail(MaxSmtpCommandBytes);
    if (!mail.append("MAIL FROM:<", 11) || !mail.append(sender.c_str(), sender.size()) ||
        !mail.append(">", 1)) {
        error.set("could not prepare SMTP MAIL FROM command");
        return SubmitOutcome::Rejected;
    }
    if (state->size) {
        char size[32];
        const int length = snprintf(size, sizeof(size), " SIZE=%zu", measured.size);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(size) || !mail.append(size, static_cast<size_t>(length))) {
            error.set("could not prepare bounded SMTP SIZE parameter");
            return SubmitOutcome::Rejected;
        }
    }
    if (smtp_utf8_needed && !mail.append(" SMTPUTF8", 9)) {
        error.set("could not prepare bounded SMTPUTF8 parameter");
        return SubmitOutcome::Rejected;
    }
    if (measured.body_has_8bit && !mail.append(" BODY=8BITMIME", 14)) {
        error.set("could not prepare bounded 8BITMIME parameter");
        return SubmitOutcome::Rejected;
    }
    int code = 0;
    if (!exchange(stream_, *state, mail.data(), mail.size(), operation, error, code)) {
        close();
        return SubmitOutcome::Rejected;
    }
    if (code != 250) {
        error.set("SMTP server rejected MAIL FROM");
        close();
        return SubmitOutcome::Rejected;
    }
    for (size_t i = 0; i < envelope.recipients.size(); ++i) {
        Text recipient;
        if (!mailbox_text(envelope.recipients[i], recipient, error)) {
            error.set("SMTP recipient validation changed after envelope preflight");
            return SubmitOutcome::Rejected;
        }
        WireBuffer command(MaxSmtpCommandBytes);
        if (!command.append("RCPT TO:<", 9) || !command.append(recipient.c_str(), recipient.size()) ||
            !command.append(">", 1)) {
            error.set("could not prepare SMTP RCPT TO command");
            return SubmitOutcome::Rejected;
        }
        if (!exchange(stream_, *state, command.data(), command.size(), operation, error, code)) {
            close();
            return SubmitOutcome::Rejected;
        }
        if (code != 250 && code != 251) {
            const char *reason = "SMTP server rejected one or more recipients; message not submitted";
            Text reset_error;
            int reset_code = 0;
            const bool reset_ok = exchange(stream_, *state, "RSET", operation,
                reset_error, reset_code);
            if (!reset_ok || reset_code != 250) {
                char detail[512];
                if (!reset_ok) {
                    snprintf(detail, sizeof(detail), "%s; SMTP RSET failed: %s",
                        reason, reset_error.c_str());
                } else {
                    snprintf(detail, sizeof(detail), "%s; SMTP RSET was rejected (code %d)",
                        reason, reset_code);
                }
                error.set(detail);
                close();
            } else {
                error.set(reason);
            }
            return SubmitOutcome::Rejected;
        }
    }
    if (!callback_allows(observer, SendStage::SendingData)) {
        const char *reason = "send observer refused before SMTP DATA";
        Text reset_error;
        int reset_code = 0;
        const bool reset_ok = exchange(stream_, *state, "RSET", operation,
            reset_error, reset_code);
        if (!reset_ok || reset_code != 250) {
            char detail[512];
            if (!reset_ok) {
                snprintf(detail, sizeof(detail), "%s; SMTP RSET failed: %s",
                    reason, reset_error.c_str());
            } else {
                snprintf(detail, sizeof(detail), "%s; SMTP RSET was rejected (code %d)",
                    reason, reset_code);
            }
            error.set(detail);
            close();
        } else {
            error.set(reason);
        }
        return SubmitOutcome::Rejected;
    }
    if (!exchange(stream_, *state, "DATA", operation, error, code)) {
        close();
        return SubmitOutcome::Rejected;
    }
    if (code != 354) {
        error.set("SMTP server refused DATA before message transfer");
        close();
        return SubmitOutcome::Rejected;
    }
    DataWriter output(stream_, operation, error);
    CanonicalStats emitted;
    if (!normalize_source(message, &output, emitted, error)) {
        stream_.close();
        connected_ = authenticated_ = false;
        return output.attempted_write ? SubmitOutcome::Uncertain : SubmitOutcome::Rejected;
    }
    if (!callback_allows(observer, SendStage::AwaitingFinalResponse)) {
        stream_.close();
        connected_ = authenticated_ = false;
        error.set("send observer refused before the SMTP DATA terminator");
        return SubmitOutcome::Rejected;
    }
    static const unsigned char terminator[] = ".\r\n";
    output.attempted_write = true;
    if (!write_all(stream_, terminator, sizeof(terminator) - 1, operation, error)) {
        stream_.close();
        connected_ = authenticated_ = false;
        return SubmitOutcome::Uncertain;
    }
    SmtpReply final_reply;
    if (!read_reply(stream_, *state, operation, error, final_reply)) {
        if (error.empty()) error.set("SMTP final DATA reply was lost");
        close();
        return SubmitOutcome::Uncertain;
    }
    if (final_reply.code == 250) {
        const bool observed = callback_allows(observer, SendStage::Accepted);
        if (!observed) error.set("SMTP accepted the message but the progress observer refused confirmation");
        return SubmitOutcome::Accepted;
    }
    if (final_reply.code >= 400 && final_reply.code <= 599) {
        error.set("SMTP server explicitly rejected the completed DATA transaction");
        if (final_reply.code == 421) close();
        return SubmitOutcome::Rejected;
    }
    error.set("SMTP final DATA response was inconclusive");
    close();
    return SubmitOutcome::Uncertain;
}

bool SmtpSession::capabilities(SmtpCapabilities &snapshot) const {
    const SmtpState *state = static_cast<const SmtpState *>(protocol_state_);
    if (!connected_ || !state) return false;
    snapshot.starttls = state->starttls;
    snapshot.auth_plain = state->auth_plain;
    snapshot.auth_login = state->auth_login;
    snapshot.size = state->size;
    snapshot.maximum_size = state->maximum_size;
    snapshot.smtp_utf8 = state->smtp_utf8;
    snapshot.eight_bit_mime = state->eight_bit_mime;
    return true;
}

bool SmtpSession::diagnostics(TransportDiagnostics &snapshot) const {
    return connected_ && stream_.diagnostics(snapshot);
}

} // namespace rmail
