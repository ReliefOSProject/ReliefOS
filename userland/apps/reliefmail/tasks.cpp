#include "tasks.h"
#include "mime.h"
#include "html.h"
#include "reader_render.h"
#include "imap.h"
#include "smtp.h"
#include "vault.h"
#include "profile_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef RELIEFMAIL_TESTING
extern char **environ;
#endif

namespace rmail {
namespace {

constexpr size_t MaxSecretPacketBytes = 1024u * 1024u;
constexpr size_t MaxTaskTextBytes = MaxEditableTextBytes;
constexpr size_t MaxAddressCount = 4096;
constexpr size_t MaxTaskPayloadBytes = MaxRawMessageBytes + 2u * 1024u * 1024u;
constexpr uint32_t TaskPayloadVersion = 7;
constexpr uint32_t RememberedCredentialPayloadVersion = 6;
constexpr uint32_t ReaderPayloadVersion = 7;
constexpr size_t MaxChunkBytes = 256u * 1024u;

#ifdef RELIEFMAIL_TESTING
bool test_pause_after_durable_commit(bool requested, const Text &marker_path) {
    if (!requested) return true;
    if (marker_path.empty() || marker_path.size() >= PATH_MAX || marker_path.c_str()[0] != '/' ||
        !valid_utf8(marker_path.c_str(), marker_path.size()) ||
        memchr(marker_path.c_str(), 0, marker_path.size())) return false;
    const int marker_fd = open(marker_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker_fd < 0) return false;
    static const unsigned char marker[] = {'c', 'o', 'm', 'm', 'i', 't', 't', 'e', 'd'};
    size_t marker_offset = 0;
    while (marker_offset < sizeof(marker)) {
        const ssize_t count = write(marker_fd, marker + marker_offset, sizeof(marker) - marker_offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(marker_fd); return false; }
        marker_offset += static_cast<size_t>(count);
    }
    const bool marker_synced = fsync(marker_fd) == 0;
    const bool marker_closed = close(marker_fd) == 0;
    if (!marker_synced || !marker_closed) return false;
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        unsigned char released = 0;
        const ssize_t count = read(4, &released, 1);
        if (count == 1) return true;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
        return false;
    }
    return false;
}
#endif

bool set_error(Text &error, const char *message) {
    error.clear();
    error.set(message);
    return false;
}

bool valid_text(const Text &value, size_t maximum = MaxTaskTextBytes, bool empty = true) {
    if (value.size() > maximum || (!empty && value.empty()) || !valid_utf8(value.c_str(), value.size())) return false;
    return !value.size() || !memchr(value.c_str(), 0, value.size());
}

bool compose_text_contains(const Text &value, const Text &query) {
    if (query.empty() || query.size() > value.size()) return false;
    const size_t limit = value.size() - query.size();
    for (size_t start = 0; start <= limit; ++start) {
        size_t offset = 0;
        for (; offset < query.size(); ++offset) {
            unsigned char left = static_cast<unsigned char>(value.c_str()[start + offset]);
            unsigned char right = static_cast<unsigned char>(query.c_str()[offset]);
            if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + ('a' - 'A'));
            if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + ('a' - 'A'));
            if (left != right) break;
        }
        if (offset == query.size()) return true;
    }
    return false;
}

class Writer {
    unsigned char *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
    bool reserve(size_t extra) {
        if (extra > limit_ - size_) return false;
        const size_t wanted = size_ + extra;
        if (wanted <= capacity_) return true;
        size_t next = capacity_ ? capacity_ : 256;
        while (next < wanted) {
            if (next > limit_ / 2) { next = limit_; break; }
            next *= 2;
        }
        unsigned char *replacement = static_cast<unsigned char *>(realloc(data_, next));
        if (!replacement) return false;
        data_ = replacement;
        capacity_ = next;
        return true;
    }
public:
    explicit Writer(size_t limit = MaxTaskPayloadBytes) : limit_(limit) {}
    ~Writer() { if (data_) { detail::wipe_memory(data_, capacity_); free(data_); } }
    bool raw(const void *data, size_t size) {
        if (!reserve(size)) return false;
        if (size) memcpy(data_ + size_, data, size);
        size_ += size;
        return true;
    }
    bool u8(uint8_t value) { return raw(&value, 1); }
    bool boolean(bool value) { return u8(value ? 1 : 0); }
    bool u16(uint16_t value) {
        unsigned char bytes[2] = {static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        return raw(bytes, sizeof(bytes));
    }
    bool u32(uint32_t value) {
        unsigned char bytes[4] = {static_cast<unsigned char>(value >> 24), static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        return raw(bytes, sizeof(bytes));
    }
    bool u64(uint64_t value) {
        unsigned char bytes[8];
        for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<unsigned char>(value >> (56 - i * 8));
        return raw(bytes, sizeof(bytes));
    }
    bool i64(int64_t value) { return u64(static_cast<uint64_t>(value)); }
    bool text(const Text &value, size_t max = MaxTaskTextBytes) {
        return value.size() <= max && value.size() <= UINT32_MAX && u32(static_cast<uint32_t>(value.size())) &&
            raw(value.c_str(), value.size());
    }
    bool bytes(const Bytes &value, size_t max = MaxTaskPayloadBytes) {
        return value.size() <= max && value.size() <= UINT32_MAX && u32(static_cast<uint32_t>(value.size())) &&
            raw(value.data(), value.size());
    }
    bool finish(Bytes &out) { return out.set(data_, size_); }
    bool finish(SecretBytes &out) { return out.set(data_, size_); }
    const unsigned char *data() const { return data_; }
    size_t size() const { return size_; }
};

class Reader {
    const unsigned char *data_;
    size_t size_;
    size_t at_ = 0;
public:
    Reader(const unsigned char *data, size_t size) : data_(data), size_(size) {}
    bool raw(void *out, size_t size) {
        if (size > size_ - at_) return false;
        if (size) memcpy(out, data_ + at_, size);
        at_ += size;
        return true;
    }
    bool view(const unsigned char *&out, size_t size) {
        if (size > size_ - at_) return false;
        out = data_ + at_; at_ += size; return true;
    }
    bool u8(uint8_t &out) { return raw(&out, 1); }
    bool boolean(bool &out) { uint8_t value = 0; if (!u8(value) || value > 1) return false; out = value != 0; return true; }
    bool u16(uint16_t &out) {
        unsigned char b[2]; if (!raw(b, 2)) return false;
        out = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]); return true;
    }
    bool u32(uint32_t &out) {
        unsigned char b[4]; if (!raw(b, 4)) return false;
        out = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
            (static_cast<uint32_t>(b[2]) << 8) | b[3]; return true;
    }
    bool u64(uint64_t &out) {
        unsigned char b[8]; if (!raw(b, 8)) return false;
        out = 0; for (unsigned char value : b) out = (out << 8) | value; return true;
    }
    bool i64(int64_t &out) { uint64_t value = 0; if (!u64(value)) return false; out = static_cast<int64_t>(value); return true; }
    bool text(Text &out, size_t max = MaxTaskTextBytes) {
        uint32_t size = 0; const unsigned char *value = nullptr;
        return u32(size) && size <= max && view(value, size) && out.set(reinterpret_cast<const char *>(value), size) &&
            valid_text(out, max);
    }
    bool bytes(Bytes &out, size_t max = MaxTaskPayloadBytes) {
        uint32_t size = 0; const unsigned char *value = nullptr;
        return u32(size) && size <= max && view(value, size) && out.set(value, size);
    }
    size_t remaining() const { return size_ - at_; }
};

bool put_magic(Writer &writer, const char magic[4], uint16_t version) {
    return writer.raw(magic, 4) && writer.u16(version);
}
bool get_magic(Reader &reader, const char magic[4], uint16_t version) {
    char got[4]; uint16_t found = 0;
    return reader.raw(got, sizeof(got)) && !memcmp(got, magic, sizeof(got)) &&
        reader.u16(found) && found == version;
}

bool encode_address(const Address &address, Writer &writer) {
    return valid_text(address.display_name) && valid_text(address.local_part, 1024, false) &&
        valid_text(address.domain, 1024, false) && writer.text(address.display_name) &&
        writer.text(address.local_part, 1024) && writer.text(address.domain, 1024) &&
        writer.boolean(address.display_name_quoted) && writer.boolean(address.local_part_quoted);
}
bool decode_address(Reader &reader, Address &address) {
    Address next;
    if (!reader.text(next.display_name) || !reader.text(next.local_part, 1024) ||
        !reader.text(next.domain, 1024) || !reader.boolean(next.display_name_quoted) ||
        !reader.boolean(next.local_part_quoted) || next.local_part.empty() || next.domain.empty()) return false;
    address = static_cast<Address &&>(next);
    return true;
}
bool encode_addresses(const Array<Address> &addresses, Writer &writer) {
    if (addresses.size() > MaxAddressCount || !writer.u32(static_cast<uint32_t>(addresses.size()))) return false;
    for (size_t i = 0; i < addresses.size(); ++i) if (!encode_address(addresses[i], writer)) return false;
    return true;
}
bool decode_addresses(Reader &reader, Array<Address> &addresses) {
    uint32_t count = 0;
    if (!reader.u32(count) || count > MaxAddressCount) return false;
    Array<Address> next;
    for (uint32_t i = 0; i < count; ++i) {
        Address address;
        if (!decode_address(reader, address) || !next.push(static_cast<Address &&>(address))) return false;
    }
    addresses = static_cast<Array<Address> &&>(next);
    return true;
}

bool encode_account(const Account &a, Writer &w) {
    return valid_text(a.display_name) && valid_text(a.email_address) && valid_text(a.username) &&
        valid_text(a.smtp_username) && valid_text(a.incoming_host, 4096) && valid_text(a.outgoing_host, 4096) &&
        w.u64(a.id) && w.u64(a.generation) && w.text(a.display_name) && w.text(a.email_address) &&
        w.text(a.username) && w.text(a.smtp_username) && w.text(a.incoming_host, 4096) &&
        w.text(a.outgoing_host, 4096) && w.u32(a.incoming_port) && w.u32(a.outgoing_port) &&
        w.u8(static_cast<uint8_t>(a.incoming_protocol)) && w.u8(static_cast<uint8_t>(a.incoming_security)) &&
        w.u8(static_cast<uint8_t>(a.outgoing_security)) && w.u8(static_cast<uint8_t>(a.authentication)) &&
        w.boolean(a.enabled) && w.boolean(a.is_default) && w.u32(a.connect_timeout_ms) &&
        w.u32(a.io_timeout_ms) && w.u32(a.poll_interval_minutes) && w.u32(a.cache_days) &&
        w.text(a.custom_ca_file, 4096) && w.text(a.inbox_folder, 4096) && w.text(a.sent_folder, 4096) &&
        w.text(a.drafts_folder, 4096) && w.text(a.trash_folder, 4096) && w.text(a.junk_folder, 4096) &&
        w.text(a.archive_folder, 4096) && w.u64(a.signature_id);
}
bool decode_account(Reader &r, Account &out) {
    Account a; uint8_t protocol = 0, incoming_tls = 0, outgoing_tls = 0, authentication = 0;
    if (!r.u64(a.id) || !r.u64(a.generation) || !r.text(a.display_name) || !r.text(a.email_address) ||
        !r.text(a.username) || !r.text(a.smtp_username) || !r.text(a.incoming_host, 4096) ||
        !r.text(a.outgoing_host, 4096) || !r.u32(a.incoming_port) || !r.u32(a.outgoing_port) ||
        !r.u8(protocol) || protocol > static_cast<uint8_t>(IncomingProtocol::Pop3) ||
        !r.u8(incoming_tls) || incoming_tls > static_cast<uint8_t>(TransportSecurity::StartTls) ||
        !r.u8(outgoing_tls) || outgoing_tls > static_cast<uint8_t>(TransportSecurity::StartTls) ||
        !r.u8(authentication) || authentication > static_cast<uint8_t>(AuthenticationMethod::ApplicationPassword) ||
        !r.boolean(a.enabled) || !r.boolean(a.is_default) || !r.u32(a.connect_timeout_ms) ||
        !r.u32(a.io_timeout_ms) || !r.u32(a.poll_interval_minutes) || !r.u32(a.cache_days) ||
        !r.text(a.custom_ca_file, 4096) || !r.text(a.inbox_folder, 4096) || !r.text(a.sent_folder, 4096) ||
        !r.text(a.drafts_folder, 4096) || !r.text(a.trash_folder, 4096) || !r.text(a.junk_folder, 4096) ||
        !r.text(a.archive_folder, 4096) || !r.u64(a.signature_id)) return false;
    a.incoming_protocol = static_cast<IncomingProtocol>(protocol);
    a.incoming_security = static_cast<TransportSecurity>(incoming_tls);
    a.outgoing_security = static_cast<TransportSecurity>(outgoing_tls);
    a.authentication = static_cast<AuthenticationMethod>(authentication);
    out = static_cast<Account &&>(a);
    return true;
}

bool encode_folder(const Folder &f, Writer &w) {
    return w.u64(f.key.account_id) && w.text(f.key.server_name, 4096) && w.text(f.display_name) &&
        w.u8(static_cast<uint8_t>(f.role)) && w.u64(f.uidvalidity) && w.u64(f.generation) &&
        w.u32(f.message_count) && w.u32(f.unread_count) && w.boolean(f.selectable);
}
bool decode_folder(Reader &r, Folder &f) {
    Folder next; uint8_t role = 0;
    if (!r.u64(next.key.account_id) || !r.text(next.key.server_name, 4096) ||
        !r.text(next.display_name) || !r.u8(role) || role > static_cast<uint8_t>(FolderRole::Archive) ||
        !r.u64(next.uidvalidity) || !r.u64(next.generation) || !r.u32(next.message_count) ||
        !r.u32(next.unread_count) || !r.boolean(next.selectable) || !next.key.account_id ||
        next.key.server_name.empty() || next.unread_count > next.message_count ||
        (!next.selectable && next.uidvalidity == 0 && (next.message_count || next.unread_count))) return false;
    next.role = static_cast<FolderRole>(role);
    f = static_cast<Folder &&>(next);
    return true;
}

bool encode_message(const MessageSummary &m, Writer &w) {
    return w.u64(m.key.folder.account_id) && w.text(m.key.folder.server_name, 4096) &&
        w.u64(m.key.uidvalidity) && w.u32(m.key.uid) && w.text(m.from) && w.text(m.subject) &&
        w.i64(m.internal_date) && w.u64(m.size) && w.u32(m.flags) && w.boolean(m.headers_only);
}
bool decode_message(Reader &r, MessageSummary &m) {
    MessageSummary next;
    if (!r.u64(next.key.folder.account_id) || !r.text(next.key.folder.server_name, 4096) ||
        !r.u64(next.key.uidvalidity) || !r.u32(next.key.uid) || !r.text(next.from) ||
        !r.text(next.subject) || !r.i64(next.internal_date) || !r.u64(next.size) ||
        !r.u32(next.flags) || !r.boolean(next.headers_only) || !next.key.folder.account_id ||
        next.key.folder.server_name.empty() || !next.key.uidvalidity || !next.key.uid ||
        next.size > MaxRawMessageBytes) return false;
    m = static_cast<MessageSummary &&>(next);
    return true;
}

bool encode_key(const MessageKey &key, Writer &w) {
    return key.folder.account_id && !key.folder.server_name.empty() && key.uidvalidity && key.uid &&
        w.u64(key.folder.account_id) && w.text(key.folder.server_name, 4096) &&
        w.u64(key.uidvalidity) && w.u32(key.uid);
}
bool decode_key(Reader &r, MessageKey &key) {
    MessageKey next;
    if (!r.u64(next.folder.account_id) || !r.text(next.folder.server_name, 4096) ||
        !r.u64(next.uidvalidity) || !r.u32(next.uid) || !next.folder.account_id ||
        next.folder.server_name.empty() || !next.uidvalidity || !next.uid) return false;
    key = static_cast<MessageKey &&>(next); return true;
}

bool encode_draft_fields(const Draft &d, Writer &w) {
    return d.editor_state.size() <= MaxEditorStateBytes && w.u64(d.id) && w.u64(d.revision) &&
        w.u64(d.account_id) && encode_addresses(d.to, w) && encode_addresses(d.cc, w) &&
        encode_addresses(d.bcc, w) && w.text(d.to_input) && w.text(d.cc_input) && w.text(d.bcc_input) &&
        w.boolean(d.raw_recipient_input) && w.text(d.subject) && w.text(d.body) && w.text(d.html_body) &&
        w.bytes(d.editor_state, MaxEditorStateBytes) && w.boolean(d.rich_format) && w.text(d.message_id, 998) &&
        w.text(d.in_reply_to, 998) && w.text(d.references, 8192) &&
        w.u8(static_cast<uint8_t>(d.send_stage));
}
bool decode_draft_fields(Reader &r, Draft &d) {
    Draft next; uint8_t stage = 0;
    if (!r.u64(next.id) || !r.u64(next.revision) || !r.u64(next.account_id) ||
        !decode_addresses(r, next.to) || !decode_addresses(r, next.cc) || !decode_addresses(r, next.bcc) ||
        !r.text(next.to_input) || !r.text(next.cc_input) || !r.text(next.bcc_input) ||
        !r.boolean(next.raw_recipient_input) || !r.text(next.subject) || !r.text(next.body) ||
        !r.text(next.html_body) || !r.bytes(next.editor_state, MaxEditorStateBytes) ||
        !r.boolean(next.rich_format) || !r.text(next.message_id, 998) || !r.text(next.in_reply_to, 998) ||
        !r.text(next.references, 8192) || !r.u8(stage) || stage > static_cast<uint8_t>(SendStage::Archived)) return false;
    next.send_stage = static_cast<SendStage>(stage);
    d = static_cast<Draft &&>(next);
    return true;
}

bool encode_attachment_metadata(const WorkerAttachmentMetadata &a, Writer &w) {
    return valid_text(a.filename, 4096) && valid_text(a.content_type, 1024) && valid_text(a.content_id, 1024) &&
        a.size <= DefaultMaxAttachmentBytes && w.text(a.filename, 4096) && w.text(a.content_type, 1024) &&
        w.text(a.content_id, 1024) && w.u64(a.size) && w.boolean(a.inline_part);
}
bool decode_attachment_metadata(Reader &r, WorkerAttachmentMetadata &a) {
    WorkerAttachmentMetadata next;
    if (!r.text(next.filename, 4096) || !r.text(next.content_type, 1024) || !r.text(next.content_id, 1024) ||
        !r.u64(next.size) || next.size > DefaultMaxAttachmentBytes || !r.boolean(next.inline_part)) return false;
    a = static_cast<WorkerAttachmentMetadata &&>(next); return true;
}

bool encode_page_query(const MessageQuerySpec &q, Writer &w) {
    if (static_cast<uint8_t>(q.scope) > static_cast<uint8_t>(MessageQueryScope::AllMail) ||
        static_cast<uint8_t>(q.sort) > static_cast<uint8_t>(MessageQuerySort::Subject) ||
        static_cast<uint8_t>(q.search_field) > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        static_cast<uint8_t>(q.attachment_filter) > static_cast<uint8_t>(AttachmentQueryFilter::Unknown) ||
        (q.has_received_after && q.has_received_before && q.received_after > q.received_before) ||
        q.limit == 0 || q.limit > MaxRowsPerPage || q.offset > static_cast<uint64_t>(INT64_MAX) ||
        !valid_text(q.query, 1024)) return false;
    return w.u8(static_cast<uint8_t>(q.scope)) && w.u8(static_cast<uint8_t>(q.sort)) &&
        w.u8(static_cast<uint8_t>(q.search_field)) && w.u64(q.account_id) &&
        w.u64(q.folder.account_id) && w.text(q.folder.server_name, 4096) && w.u64(q.offset) &&
        w.u32(q.limit) && w.u32(q.required_flags) && w.u32(q.excluded_flags) &&
        w.boolean(q.has_received_after) && w.i64(q.received_after) &&
        w.boolean(q.has_received_before) && w.i64(q.received_before) &&
        w.u8(static_cast<uint8_t>(q.attachment_filter)) && w.text(q.query, 1024);
}
bool decode_page_query(Reader &r, MessageQuerySpec &q) {
    MessageQuerySpec next; uint8_t scope = 0, sort = 0, field = 0, attachment_filter = 0;
    if (!r.u8(scope) || scope > static_cast<uint8_t>(MessageQueryScope::AllMail) ||
        !r.u8(sort) || sort > static_cast<uint8_t>(MessageQuerySort::Subject) ||
        !r.u8(field) || field > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        !r.u64(next.account_id) || !r.u64(next.folder.account_id) ||
        !r.text(next.folder.server_name, 4096) || !r.u64(next.offset) || next.offset > static_cast<uint64_t>(INT64_MAX) ||
        !r.u32(next.limit) || !next.limit || next.limit > MaxRowsPerPage ||
        !r.u32(next.required_flags) || !r.u32(next.excluded_flags) ||
        !r.boolean(next.has_received_after) || !r.i64(next.received_after) ||
        !r.boolean(next.has_received_before) || !r.i64(next.received_before) ||
        !r.u8(attachment_filter) || attachment_filter > static_cast<uint8_t>(AttachmentQueryFilter::Unknown) ||
        (next.has_received_after && next.has_received_before && next.received_after > next.received_before) ||
        !r.text(next.query, 1024)) return false;
    next.scope = static_cast<MessageQueryScope>(scope);
    next.sort = static_cast<MessageQuerySort>(sort);
    next.search_field = static_cast<MessageQueryField>(field);
    next.attachment_filter = static_cast<AttachmentQueryFilter>(attachment_filter);
    q = static_cast<MessageQuerySpec &&>(next); return true;
}

bool valid_page_folder_identity(const MessageQueryPage &page) {
    if (page.query.scope != MessageQueryScope::Folder)
        return !page.folder_generation && !page.folder_uidvalidity;
    if (!page.query.account_id || page.query.folder.account_id != page.query.account_id ||
        page.query.folder.server_name.empty() || !page.folder_generation) return false;
    for (size_t i = 0; i < page.rows.size(); ++i) {
        const MessageQueryRow &row = page.rows[i];
        if (!page.folder_uidvalidity || row.message.key.folder != page.query.folder ||
            row.message.key.uidvalidity != page.folder_uidvalidity ||
            row.folder_generation != page.folder_generation) return false;
    }
    return true;
}

bool encode_refs(const WorkerDraftAttachmentRefs &refs, Writer &w) {
    if (refs.size() > MaxMimeParts || !w.u32(static_cast<uint32_t>(refs.size()))) return false;
    for (size_t i = 0; i < refs.size(); ++i) {
        const WorkerDraftAttachmentRef &ref = refs[i];
        if (static_cast<uint8_t>(ref.kind) > static_cast<uint8_t>(WorkerAttachmentRefKind::ExistingDraftPart) ||
            !valid_text(ref.path, PATH_MAX) || !valid_text(ref.filename, 4096) ||
            !valid_text(ref.content_type, 1024) || !valid_text(ref.content_id, 1024) ||
            !w.u8(static_cast<uint8_t>(ref.kind)) || !w.text(ref.path, PATH_MAX) ||
            !w.u64(ref.source_draft_id) || !w.u64(ref.source_revision) || !w.u32(ref.source_position) || !w.text(ref.filename, 4096) ||
            !w.text(ref.content_type, 1024) || !w.text(ref.content_id, 1024) || !w.boolean(ref.inline_part)) return false;
    }
    return true;
}
bool decode_refs(Reader &r, WorkerDraftAttachmentRefs &refs) {
    uint32_t count = 0;
    if (!r.u32(count) || count > MaxMimeParts) return false;
    WorkerDraftAttachmentRefs next;
    for (uint32_t i = 0; i < count; ++i) {
        WorkerDraftAttachmentRef ref; uint8_t kind = 0;
        if (!r.u8(kind) || kind > static_cast<uint8_t>(WorkerAttachmentRefKind::ExistingDraftPart) ||
            !r.text(ref.path, PATH_MAX) || !r.u64(ref.source_draft_id) || !r.u64(ref.source_revision) || !r.u32(ref.source_position) ||
            !r.text(ref.filename, 4096) || !r.text(ref.content_type, 1024) ||
            !r.text(ref.content_id, 1024) || !r.boolean(ref.inline_part)) return false;
        ref.kind = static_cast<WorkerAttachmentRefKind>(kind);
        if ((ref.kind == WorkerAttachmentRefKind::SelectedFile && ref.path.empty()) ||
            (ref.kind == WorkerAttachmentRefKind::ExistingDraftPart && !ref.source_draft_id) ||
            !next.push(static_cast<WorkerDraftAttachmentRef &&>(ref))) return false;
    }
    refs = static_cast<WorkerDraftAttachmentRefs &&>(next); return true;
}

struct CancelContext {
    const OperationContext *context;
};

bool cancelled(const OperationContext &context) {
    if (context.deadline_monotonic_ms != UINT64_MAX) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            const uint64_t millis = static_cast<uint64_t>(now.tv_sec) * 1000u + now.tv_nsec / 1000000u;
            if (millis >= context.deadline_monotonic_ms) return true;
        }
    }
    return context.is_cancelled && context.is_cancelled(context.cancellation_context);
}

struct SelectedFileSource { int fd; size_t references; };

bool file_read_callback(void *context, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size) {
    const int fd = static_cast<SelectedFileSource *>(context)->fd;
    if (offset > static_cast<size_t>(INT64_MAX)) return false;
    ssize_t count;
    do { count = pread(fd, buffer, capacity, static_cast<off_t>(offset)); } while (count < 0 && errno == EINTR);
    if (count < 0) return false;
    *read_size = static_cast<size_t>(count);
    return true;
}

void retain_selected_file(void *context) {
    SelectedFileSource *source = static_cast<SelectedFileSource *>(context);
    if (source) ++source->references;
}
void release_selected_file(void *context) {
    SelectedFileSource *source = static_cast<SelectedFileSource *>(context);
    if (!source) return;
    if (--source->references == 0) { close(source->fd); free(source); }
}

bool read_selected_file(const WorkerDraftAttachmentRef &ref, Attachment &attachment, Text &error) {
    int fd = open(ref.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return set_error(error, "cannot open selected attachment safely");
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > DefaultMaxAttachmentBytes) {
        close(fd); return set_error(error, "selected attachment is not a bounded regular file");
    }
    attachment.filename = ref.filename;
    if (attachment.filename.empty()) {
        const char *base = strrchr(ref.path.c_str(), '/');
        if (!attachment.filename.set(base ? base + 1 : ref.path.c_str(), strlen(base ? base + 1 : ref.path.c_str()))) {
            close(fd); return set_error(error, "cannot store selected attachment name");
        }
    }
    attachment.content_type = ref.content_type;
    attachment.content_id = ref.content_id;
    attachment.inline_part = ref.inline_part;
    SelectedFileSource *source = static_cast<SelectedFileSource *>(malloc(sizeof(SelectedFileSource)));
    if (!source) { close(fd); return set_error(error, "cannot allocate attachment file handle"); }
    source->fd = fd;
    source->references = 1;
    attachment.source.set_owned(source, file_read_callback, static_cast<size_t>(st.st_size),
        retain_selected_file, release_selected_file);
    return true;
}

} // namespace

bool worker_encode_draft(const Draft &draft, Bytes &payload, Text &error) {
    error.clear();
    if (draft.attachments.size() > MaxMimeParts) return set_error(error, "draft has too many attachments");
    size_t total = 0;
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMD1", TaskPayloadVersion) || !encode_draft_fields(draft, writer) ||
        !writer.u32(static_cast<uint32_t>(draft.attachments.size())))
        return set_error(error, "draft payload is invalid or too large");
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        const Attachment &attachment = draft.attachments[i];
        if (attachment.source.read || attachment.data.size() > DefaultMaxAttachmentBytes - total)
            return set_error(error, "attachment callbacks must remain local and inline bytes exceed the bound");
        total += attachment.data.size();
        WorkerAttachmentMetadata metadata;
        metadata.filename = attachment.filename;
        metadata.content_type = attachment.content_type;
        metadata.content_id = attachment.content_id;
        metadata.size = attachment.data.size();
        metadata.inline_part = attachment.inline_part;
        if (!encode_attachment_metadata(metadata, writer) || !writer.bytes(attachment.data, DefaultMaxAttachmentBytes))
            return set_error(error, "draft attachment payload exceeds its bound");
    }
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding draft");
    return true;
}

bool worker_decode_draft(const unsigned char *payload, size_t size, Draft &draft, Text &error) {
    error.clear();
    Reader reader(payload, size);
    if (!get_magic(reader, "RMD1", TaskPayloadVersion)) return set_error(error, "draft payload version is invalid");
    Draft next;
    uint32_t count = 0;
    if (!decode_draft_fields(reader, next) || !reader.u32(count) || count > MaxMimeParts)
        return set_error(error, "draft payload header is malformed");
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        WorkerAttachmentMetadata metadata;
        Bytes data;
        if (!decode_attachment_metadata(reader, metadata) || metadata.size > DefaultMaxAttachmentBytes - total ||
            !reader.bytes(data, DefaultMaxAttachmentBytes) || data.size() != metadata.size)
            return set_error(error, "draft attachment payload is malformed or too large");
        Attachment attachment;
        attachment.filename = metadata.filename;
        attachment.content_type = metadata.content_type;
        attachment.content_id = metadata.content_id;
        attachment.inline_part = metadata.inline_part;
        attachment.data = static_cast<Bytes &&>(data);
        total += attachment.data.size();
        if (!next.attachments.push(static_cast<Attachment &&>(attachment)))
            return set_error(error, "draft attachment count exceeds its bound");
    }
    if (reader.remaining() || !validate_draft(next, error)) return set_error(error, "draft payload is invalid");
    draft = static_cast<Draft &&>(next);
    return true;
}

bool worker_encode_draft_snapshot(const WorkerDraftSnapshot &snapshot, Bytes &payload, Text &error) {
    error.clear();
    if (snapshot.attachments.size() > MaxMimeParts) return set_error(error, "draft attachment metadata exceeds its bound");
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMDS", TaskPayloadVersion) || !encode_draft_fields(snapshot.draft, writer) ||
        !writer.u64(snapshot.account_generation) || !writer.u32(static_cast<uint32_t>(snapshot.attachments.size())))
        return set_error(error, "draft snapshot exceeds its bound");
    for (size_t i = 0; i < snapshot.attachments.size(); ++i)
        if (!encode_attachment_metadata(snapshot.attachments[i], writer))
            return set_error(error, "draft attachment metadata is invalid");
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding draft snapshot");
    return true;
}

bool worker_decode_draft_snapshot(const unsigned char *payload, size_t size,
    WorkerDraftSnapshot &snapshot, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerDraftSnapshot next; uint32_t count = 0;
    if (!get_magic(reader, "RMDS", TaskPayloadVersion) || !decode_draft_fields(reader, next.draft) ||
        !reader.u64(next.account_generation) || !reader.u32(count) || count > MaxMimeParts)
        return set_error(error, "draft snapshot header is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        WorkerAttachmentMetadata item;
        if (!decode_attachment_metadata(reader, item) || !next.attachments.push(static_cast<WorkerAttachmentMetadata &&>(item)))
            return set_error(error, "draft snapshot attachment metadata is malformed");
    }
    if (reader.remaining()) return set_error(error, "draft snapshot contains trailing bytes");
    snapshot = static_cast<WorkerDraftSnapshot &&>(next);
    return true;
}

bool worker_encode_draft_list(const WorkerDraftList &drafts, Bytes &payload, Text &error) {
    error.clear();
    if (!drafts.account_generation || drafts.drafts.size() > MaxRowsPerPage)
        return set_error(error, "draft list identity or count is invalid");
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMDL", TaskPayloadVersion) ||
        !writer.u64(drafts.account_generation) ||
        !writer.u32(static_cast<uint32_t>(drafts.drafts.size())))
        return set_error(error, "draft list exceeds its bound");
    for (size_t i = 0; i < drafts.drafts.size(); ++i) {
        const WorkerDraftSummary &draft = drafts.drafts[i];
        if (!draft.id || !draft.revision || !valid_text(draft.to, 512) ||
            !valid_text(draft.subject, 4096) ||
            !writer.u64(draft.id) || !writer.u64(draft.revision) ||
            !writer.text(draft.to, 512) || !writer.text(draft.subject, 4096))
            return set_error(error, "draft list contains an invalid summary");
    }
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding draft list");
    return true;
}

bool worker_decode_draft_list(const unsigned char *payload, size_t size,
    WorkerDraftList &drafts, Text &error) {
    error.clear();
    Reader reader(payload, size);
    WorkerDraftList next;
    uint32_t count = 0;
    if (!get_magic(reader, "RMDL", TaskPayloadVersion) || !reader.u64(next.account_generation) ||
        !next.account_generation || !reader.u32(count) || count > MaxRowsPerPage)
        return set_error(error, "draft list header is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        WorkerDraftSummary summary;
        if (!reader.u64(summary.id) || !summary.id || !reader.u64(summary.revision) ||
            !summary.revision || !reader.text(summary.to, 512) ||
            !reader.text(summary.subject, 4096) ||
            !next.drafts.push(static_cast<WorkerDraftSummary &&>(summary)))
            return set_error(error, "draft list summary is malformed");
    }
    if (reader.remaining()) return set_error(error, "draft list contains trailing bytes");
    drafts = static_cast<WorkerDraftList &&>(next);
    return true;
}

bool worker_encode_compose_catalog(const WorkerComposeCatalog &catalog, Bytes &payload, Text &error) {
    error.clear();
    if (!catalog.account_generation || catalog.templates.size() > MaxComposeTemplateChoices ||
        catalog.contacts.size() > MaxComposeContactSuggestions ||
        catalog.has_signature != (catalog.signature_id != 0) ||
        !valid_text(catalog.signature_body, MaxEditableTextBytes) ||
        (!catalog.has_signature && !catalog.signature_body.empty()) ||
        (catalog.selected_template_id == 0 &&
            (!catalog.selected_template_subject.empty() || !catalog.selected_template_body.empty())) ||
        !valid_text(catalog.selected_template_subject, 4096) ||
        !valid_text(catalog.selected_template_body, MaxEditableTextBytes))
        return set_error(error, "compose catalog identity or text is invalid");
    Writer writer(2u * 1024u * 1024u);
    if (!put_magic(writer, "RMCC", 1) || !writer.u64(catalog.account_generation) ||
        !writer.u64(catalog.signature_id) || !writer.boolean(catalog.has_signature) ||
        !writer.text(catalog.signature_body, MaxEditableTextBytes) ||
        !writer.boolean(catalog.templates_truncated) ||
        !writer.u32(static_cast<uint32_t>(catalog.templates.size())))
        return set_error(error, "compose catalog exceeds its payload bound");
    for (size_t i = 0; i < catalog.templates.size(); ++i) {
        const WorkerComposeTemplateChoice &choice = catalog.templates[i];
        if (!choice.id || !valid_text(choice.name, 512, false) || !valid_text(choice.subject, 4096) ||
            !writer.u64(choice.id) || !writer.text(choice.name, 512) || !writer.text(choice.subject, 4096))
            return set_error(error, "compose template summary is invalid");
        for (size_t previous = 0; previous < i; ++previous)
            if (catalog.templates[previous].id == choice.id)
                return set_error(error, "compose template IDs are duplicated");
    }
    if (!writer.boolean(catalog.contacts_truncated) ||
        !writer.u32(static_cast<uint32_t>(catalog.contacts.size())))
        return set_error(error, "compose contact suggestions exceed their payload bound");
    for (size_t i = 0; i < catalog.contacts.size(); ++i) {
        const WorkerComposeContactSuggestion &contact = catalog.contacts[i];
        Array<Address> addresses;
        if (!valid_text(contact.display_name, 512, false) || !valid_text(contact.address, 998, false) ||
            !parse_addresses(contact.address.c_str(), contact.address.size(), addresses, error) ||
            addresses.size() != 1 || !writer.text(contact.display_name, 512) ||
            !writer.text(contact.address, 998))
            return set_error(error, "compose contact suggestion is invalid");
    }
    if (!writer.u64(catalog.selected_template_id) ||
        !writer.text(catalog.selected_template_subject, 4096) ||
        !writer.text(catalog.selected_template_body, MaxEditableTextBytes) ||
        !writer.finish(payload))
        return set_error(error, "compose catalog could not be encoded");
    return true;
}

bool worker_decode_compose_catalog(const unsigned char *payload, size_t size,
    WorkerComposeCatalog &catalog, Text &error) {
    error.clear();
    Reader reader(payload, size);
    WorkerComposeCatalog next;
    uint32_t count = 0;
    if (!get_magic(reader, "RMCC", 1) || !reader.u64(next.account_generation) ||
        !next.account_generation || !reader.u64(next.signature_id) ||
        !reader.boolean(next.has_signature) || !reader.text(next.signature_body, MaxEditableTextBytes) ||
        next.has_signature != (next.signature_id != 0) ||
        (!next.has_signature && !next.signature_body.empty()) ||
        !valid_utf8(next.signature_body.c_str(), next.signature_body.size()) ||
        !reader.boolean(next.templates_truncated) || !reader.u32(count) ||
        count > MaxComposeTemplateChoices)
        return set_error(error, "compose catalog header is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        WorkerComposeTemplateChoice choice;
        if (!reader.u64(choice.id) || !choice.id || !reader.text(choice.name, 512) ||
            choice.name.empty() || !reader.text(choice.subject, 4096) ||
            !valid_utf8(choice.name.c_str(), choice.name.size()) ||
            !valid_utf8(choice.subject.c_str(), choice.subject.size()) ||
            !next.templates.push(static_cast<WorkerComposeTemplateChoice &&>(choice)))
            return set_error(error, "compose template summary is malformed");
        for (size_t previous = 0; previous + 1 < next.templates.size(); ++previous)
            if (next.templates[previous].id == next.templates[next.templates.size() - 1].id)
                return set_error(error, "compose template IDs are duplicated");
    }
    if (!reader.boolean(next.contacts_truncated) || !reader.u32(count) ||
        count > MaxComposeContactSuggestions)
        return set_error(error, "compose contact count is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        WorkerComposeContactSuggestion contact;
        Array<Address> addresses;
        if (!reader.text(contact.display_name, 512) || contact.display_name.empty() ||
            !reader.text(contact.address, 998) || contact.address.empty() ||
            !valid_utf8(contact.display_name.c_str(), contact.display_name.size()) ||
            !parse_addresses(contact.address.c_str(), contact.address.size(), addresses, error) ||
            addresses.size() != 1 || !next.contacts.push(static_cast<WorkerComposeContactSuggestion &&>(contact)))
            return set_error(error, "compose contact suggestion is malformed");
    }
    if (!reader.u64(next.selected_template_id) ||
        !reader.text(next.selected_template_subject, 4096) ||
        !reader.text(next.selected_template_body, MaxEditableTextBytes) ||
        (!next.selected_template_id &&
            (!next.selected_template_subject.empty() || !next.selected_template_body.empty())) ||
        !valid_utf8(next.selected_template_subject.c_str(), next.selected_template_subject.size()) ||
        !valid_utf8(next.selected_template_body.c_str(), next.selected_template_body.size()) ||
        reader.remaining())
        return set_error(error, "selected compose template is malformed");
    catalog = static_cast<WorkerComposeCatalog &&>(next);
    return true;
}

bool worker_encode_startup_snapshot(const WorkerStartupSnapshot &snapshot, Bytes &payload, Text &error) {
    error.clear(); Writer writer(MaxTaskPayloadBytes);
    if (snapshot.accounts.size() > MaxAccounts || snapshot.folders.size() > MaxFoldersPerAccount ||
        !put_magic(writer, "RMSU", TaskPayloadVersion) || !writer.u64(snapshot.selected_account_id) ||
        !writer.u32(static_cast<uint32_t>(snapshot.accounts.size())))
        return set_error(error, "startup account snapshot exceeds its bound");
    for (size_t i = 0; i < snapshot.accounts.size(); ++i)
        if (!encode_account(snapshot.accounts[i], writer)) return set_error(error, "startup account data is invalid");
    if (!writer.u32(static_cast<uint32_t>(snapshot.folders.size()))) return set_error(error, "startup folder snapshot is too large");
    for (size_t i = 0; i < snapshot.folders.size(); ++i)
        if (!encode_folder(snapshot.folders[i], writer)) return set_error(error, "startup folder data is invalid");
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding startup snapshot");
    return true;
}

bool worker_decode_startup_snapshot(const unsigned char *payload, size_t size,
    WorkerStartupSnapshot &snapshot, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerStartupSnapshot next; uint32_t count = 0;
    if (!get_magic(reader, "RMSU", TaskPayloadVersion) || !reader.u64(next.selected_account_id) ||
        !reader.u32(count) || count > MaxAccounts) return set_error(error, "startup snapshot header is invalid");
    for (uint32_t i = 0; i < count; ++i) {
        Account account;
        if (!decode_account(reader, account) || !next.accounts.push(static_cast<Account &&>(account)))
            return set_error(error, "startup account snapshot is malformed");
    }
    if (!reader.u32(count) || count > MaxFoldersPerAccount) return set_error(error, "startup folder snapshot is invalid");
    for (uint32_t i = 0; i < count; ++i) {
        Folder folder;
        if (!decode_folder(reader, folder) || !next.folders.push(static_cast<Folder &&>(folder)))
            return set_error(error, "startup folder snapshot is malformed");
    }
    if (reader.remaining()) return set_error(error, "startup snapshot contains trailing bytes");
    snapshot = static_cast<WorkerStartupSnapshot &&>(next);
    return true;
}

bool worker_encode_vault_status(const WorkerVaultStatus &status, Bytes &payload, Text &error) {
    error.clear();
    Writer writer(32);
    if (!put_magic(writer, "RMVS", TaskPayloadVersion) || !writer.boolean(status.exists) ||
        !writer.finish(payload)) return set_error(error, "vault status result cannot be encoded");
    return true;
}

bool worker_decode_vault_status(const unsigned char *payload, size_t size,
    WorkerVaultStatus &status, Text &error) {
    error.clear();
    Reader reader(payload, size);
    WorkerVaultStatus next;
    if (!get_magic(reader, "RMVS", TaskPayloadVersion) || !reader.boolean(next.exists) || reader.remaining())
        return set_error(error, "vault status result is malformed");
    status = next;
    return true;
}

bool worker_encode_account_result(const WorkerAccountResult &account, Bytes &payload, Text &error) {
    error.clear(); Writer writer(1024u * 1024u);
    if (!put_magic(writer, "RMAU", TaskPayloadVersion) || !writer.boolean(account.deleted) ||
        !encode_account(account.account, writer) || !writer.finish(payload))
        return set_error(error, "account result is invalid");
    return true;
}

bool worker_decode_account_result(const unsigned char *payload, size_t size,
    WorkerAccountResult &account, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerAccountResult next;
    if (!get_magic(reader, "RMAU", TaskPayloadVersion) || !reader.boolean(next.deleted) ||
        !decode_account(reader, next.account) || reader.remaining()) return set_error(error, "account result is malformed");
    account = static_cast<WorkerAccountResult &&>(next); return true;
}

bool worker_encode_message_page(const WorkerMessagePage &page, Bytes &payload, Text &error) {
    error.clear(); Writer writer(MaxTaskPayloadBytes);
    if (page.rows.size() > MaxRowsPerPage || !valid_page_folder_identity(page) ||
        !valid_text(page.diagnostic, 4096) ||
        !put_magic(writer, "RMPG", TaskPayloadVersion) || !encode_page_query(page.query, writer) ||
        !writer.u64(page.folder_generation) || !writer.u64(page.folder_uidvalidity) ||
        !writer.u64(page.total_matches) || !writer.boolean(page.has_more) ||
        !writer.u64(page.downloaded_body_count) || !writer.u64(page.headers_only_count) ||
        !writer.text(page.diagnostic, 4096) || !writer.u32(static_cast<uint32_t>(page.rows.size())))
        return set_error(error, "message page result exceeds its bound");
    for (size_t i = 0; i < page.rows.size(); ++i) {
        const WorkerPageRow &row = page.rows[i];
        if (!row.profile_epoch || !row.account_generation || !row.folder_generation ||
            !encode_message(row.message, writer) || !writer.u64(row.profile_epoch) ||
            !writer.u64(row.ui_generation) || !writer.u64(row.account_generation) ||
            !writer.u64(row.folder_generation) || !writer.text(row.folder_display_name) ||
            !writer.boolean(row.attachment_presence_known) || !writer.boolean(row.has_attachments))
            return set_error(error, "message page row is malformed");
    }
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding message page");
    return true;
}

bool worker_decode_message_page(const unsigned char *payload, size_t size,
    WorkerMessagePage &page, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerMessagePage next; uint32_t count = 0;
    if (!get_magic(reader, "RMPG", TaskPayloadVersion) || !decode_page_query(reader, next.query) ||
        !reader.u64(next.folder_generation) || !reader.u64(next.folder_uidvalidity) ||
        !reader.u64(next.total_matches) || !reader.boolean(next.has_more) ||
        !reader.u64(next.downloaded_body_count) || !reader.u64(next.headers_only_count) ||
        !reader.text(next.diagnostic, 4096) || !reader.u32(count) || count > MaxRowsPerPage)
        return set_error(error, "message page result header is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        WorkerPageRow row;
        if (!decode_message(reader, row.message) || !reader.u64(row.profile_epoch) || !row.profile_epoch ||
            !reader.u64(row.ui_generation) || !reader.u64(row.account_generation) || !row.account_generation ||
            !reader.u64(row.folder_generation) || !row.folder_generation || !reader.text(row.folder_display_name) ||
            !reader.boolean(row.attachment_presence_known) || !reader.boolean(row.has_attachments) ||
            (row.has_attachments && !row.attachment_presence_known) ||
            !next.rows.push(static_cast<WorkerPageRow &&>(row)))
            return set_error(error, "message page row is malformed");
    }
    if (reader.remaining() || next.rows.size() > next.total_matches ||
        !valid_page_folder_identity(next))
        return set_error(error, "message page result is inconsistent");
    page = static_cast<WorkerMessagePage &&>(next); return true;
}

bool worker_clone_reader_result(const WorkerReaderResult &source, WorkerReaderResult &destination) {
    if (&source == &destination) return true;
    WorkerReaderResult next;
    next.key = source.key;
    next.headers_only = source.headers_only;
    next.from = source.from;
    next.reply_to = source.reply_to;
    next.to = source.to;
    next.cc = source.cc;
    next.bcc = source.bcc;
    next.subject = source.subject;
    next.date = source.date;
    next.message_id = source.message_id;
    next.in_reply_to = source.in_reply_to;
    next.references = source.references;
    next.warning = source.warning;
    next.plain_text = source.plain_text;
    next.plain_text_truncated = source.plain_text_truncated;
    next.html_text = source.html_text;
    next.html_warning = source.html_warning;
    if (!next.safe_links.clone_from(source.safe_links) ||
        !next.inline_images.clone_from(source.inline_images) ||
        !next.attachments.clone_from(source.attachments)) return false;
    destination = static_cast<WorkerReaderResult &&>(next);
    return true;
}

bool worker_encode_reader_result(const WorkerReaderResult &reader_result, Bytes &payload, Text &error) {
    error.clear();
    const size_t text_bytes = reader_result.plain_text.size() + reader_result.html_text.size();
    if (reader_result.plain_text.size() > MaxReaderPlainPreviewBytes ||
        !valid_utf8(reader_result.plain_text.c_str(), reader_result.plain_text.size()) ||
        (reader_result.plain_text_truncated && reader_result.headers_only) ||
        text_bytes > MaxRawMessageBytes || reader_result.html_text.size() > MaxReaderHtmlBytes ||
        reader_result.attachments.size() > MaxMimeParts || reader_result.safe_links.size() > MaxReaderSafeLinks ||
        reader_result.inline_images.size() > MaxReaderInlineImages ||
        (reader_result.html_text.empty() && (reader_result.safe_links.size() || reader_result.inline_images.size())))
        return set_error(error, "reader text or attachment metadata exceeds its bound");
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMRD", ReaderPayloadVersion) || !encode_key(reader_result.key, writer) ||
        !writer.boolean(reader_result.headers_only) || !writer.text(reader_result.from) ||
        !writer.text(reader_result.reply_to) ||
        !writer.text(reader_result.to) || !writer.text(reader_result.cc) || !writer.text(reader_result.bcc) ||
        !writer.text(reader_result.subject) || !writer.text(reader_result.date) ||
        !writer.text(reader_result.message_id, 998) || !writer.text(reader_result.in_reply_to, 998) ||
        !writer.text(reader_result.references, 8192) || !writer.text(reader_result.warning, 4096) ||
        !writer.text(reader_result.plain_text, MaxReaderPlainPreviewBytes) ||
        !writer.boolean(reader_result.plain_text_truncated) ||
        !writer.text(reader_result.html_text, MaxRawMessageBytes) ||
        !writer.boolean(reader_result.html_warning) ||
        !writer.u32(static_cast<uint32_t>(reader_result.safe_links.size())))
        return set_error(error, "reader result is invalid or exceeds the payload bound");
    for (size_t i = 0; i < reader_result.safe_links.size(); ++i) {
        const Text &link = reader_result.safe_links[i];
        if (!is_safe_external_web_link(link.c_str(), link.size()) || !writer.text(link, 160))
            return set_error(error, "reader link target is invalid");
    }
    if (!writer.u32(static_cast<uint32_t>(reader_result.inline_images.size())))
        return set_error(error, "reader image count exceeds the payload bound");
    size_t total_xpm_size = 0;
    for (size_t i = 0; i < reader_result.inline_images.size(); ++i) {
        const WorkerReaderInlineImage &image = reader_result.inline_images[i];
        if (!image.width || !image.height || image.width > MaxReaderInlineImageDimension ||
            image.height > MaxReaderInlineImageDimension ||
            static_cast<size_t>(image.width) * image.height > MaxReaderInlineImagePixels ||
            image.xpm.size() > MaxReaderXpmBytes || image.xpm.size() > MaxReaderXpmTotalBytes - total_xpm_size ||
            !validate_reader_xpm(image.xpm.c_str(), image.xpm.size(), image.width, image.height, error) ||
            !writer.u16(image.width) || !writer.u16(image.height) || !writer.text(image.xpm, MaxReaderXpmBytes))
            return set_error(error, "reader inline image is malformed or exceeds its bounds");
        total_xpm_size += image.xpm.size();
    }
    if (!writer.u32(static_cast<uint32_t>(reader_result.attachments.size())))
        return set_error(error, "reader attachment metadata exceeds its bound");
    uint64_t total_attachment_size = 0;
    for (size_t i = 0; i < reader_result.attachments.size(); ++i) {
        const WorkerAttachmentMetadata &attachment = reader_result.attachments[i];
        if (attachment.size > MaxRawMessageBytes - total_attachment_size || !encode_attachment_metadata(attachment, writer))
            return set_error(error, "reader attachment metadata is invalid");
        total_attachment_size += attachment.size;
    }
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding reader result");
    return true;
}

bool worker_decode_reader_result(const unsigned char *payload, size_t size,
    WorkerReaderResult &reader_result, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerReaderResult next; uint32_t count = 0;
    if (!get_magic(reader, "RMRD", ReaderPayloadVersion) || !decode_key(reader, next.key) ||
        !reader.boolean(next.headers_only) || !reader.text(next.from) || !reader.text(next.reply_to) ||
        !reader.text(next.to) ||
        !reader.text(next.cc) || !reader.text(next.bcc) || !reader.text(next.subject) ||
        !reader.text(next.date) || !reader.text(next.message_id, 998) || !reader.text(next.in_reply_to, 998) ||
        !reader.text(next.references, 8192) || !reader.text(next.warning, 4096) ||
        !reader.text(next.plain_text, MaxReaderPlainPreviewBytes) ||
        !reader.boolean(next.plain_text_truncated) ||
        (next.plain_text_truncated && next.headers_only) ||
        !valid_utf8(next.plain_text.c_str(), next.plain_text.size()) ||
        !reader.text(next.html_text, MaxRawMessageBytes) ||
        next.plain_text.size() + next.html_text.size() > MaxRawMessageBytes ||
        next.html_text.size() > MaxReaderHtmlBytes || !reader.boolean(next.html_warning) ||
        !reader.u32(count) || count > MaxReaderSafeLinks)
        return set_error(error, "reader result header is malformed");
    for (uint32_t i = 0; i < count; ++i) {
        Text link;
        if (!reader.text(link, 160) || !is_safe_external_web_link(link.c_str(), link.size()) ||
            !next.safe_links.push(static_cast<Text &&>(link)))
            return set_error(error, "reader link target is malformed");
    }
    if (!reader.u32(count) || count > MaxReaderInlineImages)
        return set_error(error, "reader image count is malformed");
    size_t total_xpm_size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        WorkerReaderInlineImage image;
        if (!reader.u16(image.width) || !reader.u16(image.height) ||
            !image.width || !image.height || image.width > MaxReaderInlineImageDimension ||
            image.height > MaxReaderInlineImageDimension ||
            static_cast<size_t>(image.width) * image.height > MaxReaderInlineImagePixels ||
            !reader.text(image.xpm, MaxReaderXpmBytes) ||
            image.xpm.size() > MaxReaderXpmTotalBytes - total_xpm_size ||
            !validate_reader_xpm(image.xpm.c_str(), image.xpm.size(), image.width, image.height, error) ||
            !next.inline_images.push(static_cast<WorkerReaderInlineImage &&>(image)))
            return set_error(error, "reader inline image is malformed or exceeds its bounds");
        total_xpm_size += next.inline_images[next.inline_images.size() - 1].xpm.size();
    }
    if (next.html_text.empty() && (next.safe_links.size() || next.inline_images.size()))
        return set_error(error, "reader resources have no HTML document");
    if (!reader.u32(count) || count > MaxMimeParts)
        return set_error(error, "reader attachment count is malformed");
    uint64_t total_attachment_size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        WorkerAttachmentMetadata attachment;
        if (!decode_attachment_metadata(reader, attachment) || attachment.size > MaxRawMessageBytes - total_attachment_size ||
            !next.attachments.push(static_cast<WorkerAttachmentMetadata &&>(attachment)))
            return set_error(error, "reader attachment metadata is malformed");
        total_attachment_size += attachment.size;
    }
    if (reader.remaining()) return set_error(error, "reader result contains trailing bytes");
    reader_result = static_cast<WorkerReaderResult &&>(next); return true;
}

bool worker_encode_chunk_result(const WorkerChunkResult &chunk, Bytes &payload, Text &error) {
    error.clear();
    if (chunk.bytes.size() > MaxChunkBytes || chunk.total_size > MaxRawMessageBytes ||
        chunk.offset > chunk.total_size || chunk.bytes.size() > chunk.total_size - chunk.offset)
        return set_error(error, "message chunk exceeds its bound");
    Writer writer(MaxChunkBytes + 128u * 1024u);
    if (!put_magic(writer, "RMCH", TaskPayloadVersion) || !encode_key(chunk.key, writer) ||
        !writer.u32(chunk.attachment_index) || !writer.boolean(chunk.headers_only) ||
        !writer.u64(chunk.offset) || !writer.u64(chunk.total_size) ||
        !writer.bytes(chunk.bytes, MaxChunkBytes) || !writer.finish(payload))
        return set_error(error, "message chunk is invalid");
    return true;
}

bool worker_decode_chunk_result(const unsigned char *payload, size_t size,
    WorkerChunkResult &chunk, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerChunkResult next;
    if (!get_magic(reader, "RMCH", TaskPayloadVersion) || !decode_key(reader, next.key) ||
        !reader.u32(next.attachment_index) || !reader.boolean(next.headers_only) ||
        !reader.u64(next.offset) || !reader.u64(next.total_size) ||
        next.total_size > MaxRawMessageBytes || next.offset > next.total_size ||
        !reader.bytes(next.bytes, MaxChunkBytes) || next.bytes.size() > next.total_size - next.offset || reader.remaining())
        return set_error(error, "message chunk is malformed");
    chunk = static_cast<WorkerChunkResult &&>(next); return true;
}

bool worker_encode_operation_result(const WorkerOperationResult &operation, Bytes &payload, Text &error) {
    error.clear(); Writer writer(16u * 1024u);
    if (static_cast<uint8_t>(operation.stage) > static_cast<uint8_t>(SendStage::ReviewedDelivered) ||
        operation.delivery_count > 1 || !valid_text(operation.message_id, 998) ||
        !valid_text(operation.diagnostic, 4096) || !put_magic(writer, "RMOP", TaskPayloadVersion) ||
        !writer.u64(operation.operation_id) || !writer.u8(static_cast<uint8_t>(operation.stage)) ||
        !writer.u32(operation.delivery_count) || !writer.text(operation.message_id, 998) ||
        !writer.text(operation.diagnostic, 4096) || !writer.finish(payload))
        return set_error(error, "send operation result is invalid");
    return true;
}

bool worker_decode_operation_result(const unsigned char *payload, size_t size,
    WorkerOperationResult &operation, Text &error) {
    error.clear(); Reader reader(payload, size); WorkerOperationResult next; uint8_t stage = 0;
    if (!get_magic(reader, "RMOP", TaskPayloadVersion) || !reader.u64(next.operation_id) ||
        !reader.u8(stage) || stage > static_cast<uint8_t>(SendStage::ReviewedDelivered) ||
        !reader.u32(next.delivery_count) || next.delivery_count > 1 || !reader.text(next.message_id, 998) ||
        !reader.text(next.diagnostic, 4096) || reader.remaining())
        return set_error(error, "send operation result is malformed");
    next.stage = static_cast<SendStage>(stage);
    operation = static_cast<WorkerOperationResult &&>(next); return true;
}

bool worker_encode_send_operation_page(const WorkerSendOperationPage &page,
    Bytes &payload, Text &error) {
    error.clear();
    if (!page.account_id || page.offset > static_cast<uint64_t>(INT64_MAX) ||
        page.operations.size() > MaxSendOperationPageSize)
        return set_error(error, "send operation page identity or size is invalid");
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMQ1", TaskPayloadVersion) || !writer.u64(page.account_id) ||
        !writer.u64(page.offset) || !writer.boolean(page.has_more) ||
        !writer.u16(static_cast<uint16_t>(page.operations.size())))
        return set_error(error, "send operation page exceeds its encoded bound");
    for (size_t i = 0; i < page.operations.size(); ++i) {
        const SendOperationSummary &operation = page.operations[i];
        const bool reviewed = operation.archive_review_generation != 0;
        if (!operation.id || operation.account_id != page.account_id || !operation.account_generation ||
            !operation.draft_id || !operation.draft_revision || !valid_text(operation.message_id, 998) ||
            operation.message_id.empty() || !valid_text(operation.sender, 4096) || operation.sender.empty() ||
            !valid_text(operation.recipients, MaxEditableTextBytes) || operation.recipients.empty() ||
            static_cast<uint8_t>(operation.stage) > static_cast<uint8_t>(SendStage::ReviewedDelivered) ||
            !send_operation_delivery_state_valid(operation.stage, operation.delivery_count,
                operation.archive_review_generation) || !valid_text(operation.archive_folder, 4096) ||
            !valid_text(operation.last_diagnostic, 4096) ||
            (reviewed && (!operation.archive_folder_generation || !operation.archive_folder_uidvalidity ||
                operation.archive_folder.empty())) ||
            (!reviewed && (operation.archive_folder_generation || operation.archive_folder_uidvalidity)) ||
            !writer.u64(operation.id) || !writer.u64(operation.account_generation) ||
            !writer.u64(operation.draft_id) || !writer.u64(operation.draft_revision) ||
            !writer.text(operation.message_id, 998) || !writer.text(operation.sender, 4096) ||
            !writer.text(operation.recipients, MaxEditableTextBytes) ||
            !writer.u8(static_cast<uint8_t>(operation.stage)) || !writer.u32(operation.delivery_count) ||
            !writer.text(operation.archive_folder, 4096) ||
            !writer.u64(operation.archive_review_generation) ||
            !writer.u64(operation.archive_folder_generation) ||
            !writer.u64(operation.archive_folder_uidvalidity) ||
            !writer.text(operation.last_diagnostic, 4096))
            return set_error(error, "send operation page contains invalid or oversized metadata");
    }
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding send operation page");
    return true;
}

bool worker_decode_send_operation_page(const unsigned char *payload, size_t size,
    WorkerSendOperationPage &page, Text &error) {
    error.clear();
    Reader reader(payload, size);
    WorkerSendOperationPage next;
    uint16_t count = 0;
    if (!payload || size > MaxTaskPayloadBytes || !get_magic(reader, "RMQ1", TaskPayloadVersion) ||
        !reader.u64(next.account_id) || !next.account_id || !reader.u64(next.offset) ||
        next.offset > static_cast<uint64_t>(INT64_MAX) || !reader.boolean(next.has_more) ||
        !reader.u16(count) || count > MaxSendOperationPageSize)
        return set_error(error, "send operation page payload is malformed");
    for (uint16_t i = 0; i < count; ++i) {
        SendOperationSummary operation;
        uint8_t stage = 0;
        if (!reader.u64(operation.id) || !operation.id ||
            !reader.u64(operation.account_generation) || !operation.account_generation ||
            !reader.u64(operation.draft_id) || !operation.draft_id ||
            !reader.u64(operation.draft_revision) || !operation.draft_revision ||
            !reader.text(operation.message_id, 998) || operation.message_id.empty() ||
            !reader.text(operation.sender, 4096) || operation.sender.empty() ||
            !reader.text(operation.recipients, MaxEditableTextBytes) || operation.recipients.empty() ||
            !reader.u8(stage) || stage > static_cast<uint8_t>(SendStage::ReviewedDelivered) ||
            !reader.u32(operation.delivery_count) || operation.delivery_count > 1 ||
            !reader.text(operation.archive_folder, 4096) ||
            !reader.u64(operation.archive_review_generation) ||
            !reader.u64(operation.archive_folder_generation) ||
            !reader.u64(operation.archive_folder_uidvalidity) ||
            !reader.text(operation.last_diagnostic, 4096))
            return set_error(error, "send operation page record is malformed");
        operation.account_id = next.account_id;
        operation.stage = static_cast<SendStage>(stage);
        const bool reviewed = operation.archive_review_generation != 0;
        if (!send_operation_delivery_state_valid(operation.stage, operation.delivery_count,
                operation.archive_review_generation) ||
            (reviewed && (!operation.archive_folder_generation || !operation.archive_folder_uidvalidity ||
                operation.archive_folder.empty())) ||
            (!reviewed && (operation.archive_folder_generation || operation.archive_folder_uidvalidity)) ||
            !next.operations.push(static_cast<SendOperationSummary &&>(operation)))
            return set_error(error, "send operation page record has invalid review identity");
    }
    if (reader.remaining()) return set_error(error, "send operation page has trailing data");
    page = static_cast<WorkerSendOperationPage &&>(next);
    return true;
}

bool worker_encode_task_input(WorkerTaskKind kind, const WorkerTaskInput &input,
    Bytes &payload, Text &error) {
    error.clear();
    if (static_cast<uint8_t>(input.search_field) > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        static_cast<uint8_t>(input.folder_action) > static_cast<uint8_t>(WorkerFolderAction::ClearTrash) ||
        static_cast<uint8_t>(input.send_review_action) > static_cast<uint8_t>(WorkerSendReviewAction::ConfirmNotDelivered) ||
        input.chunk_size > MaxChunkBytes || input.attachment_refs.size() > MaxMimeParts)
        return set_error(error, "worker task input contains an invalid enum or oversized field");
    if ((kind == WorkerTaskKind::ReviewArchiveTarget &&
            (input.send_review_action != WorkerSendReviewAction::SetArchiveTarget || !input.review_confirmed)) ||
        (kind == WorkerTaskKind::ResolveUncertainSend &&
            ((input.send_review_action != WorkerSendReviewAction::ConfirmDelivered &&
              input.send_review_action != WorkerSendReviewAction::ConfirmNotDelivered) || !input.review_confirmed)) ||
        ((kind != WorkerTaskKind::ReviewArchiveTarget && kind != WorkerTaskKind::ResolveUncertainSend) &&
            (input.send_review_action != WorkerSendReviewAction::None || input.review_confirmed)))
        return set_error(error, "send review input is not explicitly typed and confirmed");
#ifdef RELIEFMAIL_TESTING
    if (input.test_vault_fault > static_cast<uint8_t>(VaultFaultPoint::CleanupSync))
        return set_error(error, "test Vault fault value is invalid");
    if (input.test_pause_after_durable_commit &&
        !valid_text(input.test_commit_marker_path, PATH_MAX, false))
        return set_error(error, "test commit marker path is invalid");
#endif
    for (size_t i = 0; i < input.draft.attachments.size(); ++i)
        if (input.draft.attachments[i].source.read || !input.draft.attachments[i].data.empty())
            return set_error(error, "worker task drafts use attachment references, not callbacks or copied bytes");
    Writer writer(MaxTaskPayloadBytes);
    if (!put_magic(writer, "RMTI", TaskPayloadVersion) || !writer.u16(static_cast<uint16_t>(kind)) ||
        !writer.u64(input.item_id) || !writer.u64(input.item_revision) || !writer.u64(input.page_offset) ||
        !writer.u32(input.flags_add) || !writer.u32(input.flags_remove) || !writer.u8(input.search_mode) ||
        !writer.u8(static_cast<uint8_t>(input.search_field)) || !writer.u8(static_cast<uint8_t>(input.folder_action)) ||
        !encode_account(input.account, writer) || !encode_page_query(input.page_query, writer) ||
        !writer.u64(input.message_key.folder.account_id) || !writer.text(input.message_key.folder.server_name, 4096) ||
        !writer.u64(input.message_key.uidvalidity) || !writer.u32(input.message_key.uid) ||
        !writer.u64(input.chunk_offset) || !writer.u32(input.chunk_size) || !writer.u32(input.attachment_index) ||
        !writer.u64(input.destination_folder.account_id) || !writer.text(input.destination_folder.server_name, 4096) ||
        !writer.u64(input.destination_folder_generation) || !writer.u64(input.destination_folder_uidvalidity) ||
        !writer.text(input.query, 1024) || !writer.text(input.path, PATH_MAX) || !writer.boolean(input.has_draft) ||
        !encode_draft_fields(input.draft, writer) || !encode_refs(input.attachment_refs, writer) ||
        !writer.u8(static_cast<uint8_t>(input.send_review_action)) || !writer.boolean(input.review_confirmed)
#ifdef RELIEFMAIL_TESTING
        || !writer.boolean(input.test_pause_after_durable_commit) || !writer.u8(input.test_vault_fault) ||
        !writer.text(input.test_commit_marker_path, PATH_MAX)
#endif
        )
        return set_error(error, "worker task input is invalid or exceeds its payload limit");
    if (!writer.finish(payload)) return set_error(error, "out of memory encoding worker task input");
    return true;
}

bool worker_decode_task_input(WorkerTaskKind kind, const unsigned char *payload,
    size_t size, WorkerTaskInput &input, Text &error) {
    error.clear();
    if (!payload || size > MaxTaskPayloadBytes) return set_error(error, "worker task payload size is invalid");
    Reader reader(payload, size); uint16_t found_kind = 0; WorkerTaskInput next;
    uint8_t field = 0, action = 0, send_review_action = 0;
    if (!get_magic(reader, "RMTI", TaskPayloadVersion) || !reader.u16(found_kind) ||
        found_kind != static_cast<uint16_t>(kind) || !reader.u64(next.item_id) ||
        !reader.u64(next.item_revision) || !reader.u64(next.page_offset) ||
        !reader.u32(next.flags_add) || !reader.u32(next.flags_remove) || !reader.u8(next.search_mode) ||
        !reader.u8(field) || field > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        !reader.u8(action) || action > static_cast<uint8_t>(WorkerFolderAction::ClearTrash) ||
        !decode_account(reader, next.account) || !decode_page_query(reader, next.page_query) ||
        !reader.u64(next.message_key.folder.account_id) || !reader.text(next.message_key.folder.server_name, 4096) ||
        !reader.u64(next.message_key.uidvalidity) || !reader.u32(next.message_key.uid) ||
        !reader.u64(next.chunk_offset) || !reader.u32(next.chunk_size) || next.chunk_size > MaxChunkBytes ||
        !reader.u32(next.attachment_index) || !reader.u64(next.destination_folder.account_id) ||
        !reader.text(next.destination_folder.server_name, 4096) ||
        !reader.u64(next.destination_folder_generation) || !reader.u64(next.destination_folder_uidvalidity) ||
        !reader.text(next.query, 1024) ||
        !reader.text(next.path, PATH_MAX) || !reader.boolean(next.has_draft) ||
        !decode_draft_fields(reader, next.draft) || !decode_refs(reader, next.attachment_refs) ||
        !reader.u8(send_review_action) ||
        send_review_action > static_cast<uint8_t>(WorkerSendReviewAction::ConfirmNotDelivered) ||
        !reader.boolean(next.review_confirmed)
#ifdef RELIEFMAIL_TESTING
        || !reader.boolean(next.test_pause_after_durable_commit) || !reader.u8(next.test_vault_fault) ||
        !reader.text(next.test_commit_marker_path, PATH_MAX) ||
        next.test_vault_fault > static_cast<uint8_t>(VaultFaultPoint::CleanupSync)
#endif
        || reader.remaining())
        return set_error(error, "worker task payload is malformed");
    next.search_field = static_cast<WorkerSearchField>(field);
    next.folder_action = static_cast<WorkerFolderAction>(action);
    next.send_review_action = static_cast<WorkerSendReviewAction>(send_review_action);
    if ((kind == WorkerTaskKind::ReviewArchiveTarget &&
            (next.send_review_action != WorkerSendReviewAction::SetArchiveTarget || !next.review_confirmed)) ||
        (kind == WorkerTaskKind::ResolveUncertainSend &&
            ((next.send_review_action != WorkerSendReviewAction::ConfirmDelivered &&
              next.send_review_action != WorkerSendReviewAction::ConfirmNotDelivered) || !next.review_confirmed)) ||
        ((kind != WorkerTaskKind::ReviewArchiveTarget && kind != WorkerTaskKind::ResolveUncertainSend) &&
            (next.send_review_action != WorkerSendReviewAction::None || next.review_confirmed)))
        return set_error(error, "worker send review payload has no explicit confirmation");
    input = static_cast<WorkerTaskInput &&>(next);
    return true;
}

static bool encode_remembered_credentials(const CredentialInput *records, size_t count,
    SecretBytes &packet, Text &error) {
    error.clear();
    constexpr size_t MaxRecords = MaxAccounts * 3;
    if ((!records && count) || count > MaxRecords) return set_error(error, "credential record count is invalid");
    Writer writer(MaxSecretPacketBytes);
    if (!put_magic(writer, "RMCV", RememberedCredentialPayloadVersion) || !writer.u16(static_cast<uint16_t>(count)))
        return set_error(error, "credential packet exceeds its limit");
    for (size_t i = 0; i < count; ++i) {
        const CredentialInput &record = records[i];
        const Account &account = record.configuration;
        const Text &imap_user = account.username;
        const Text &smtp_user = account.smtp_username.empty() ? account.username : account.smtp_username;
        if ((account.id && !account.generation) || (!account.id && account.generation) ||
            static_cast<uint8_t>(record.purpose) < static_cast<uint8_t>(CredentialPurpose::Imap) ||
            static_cast<uint8_t>(record.purpose) > static_cast<uint8_t>(CredentialPurpose::Shared) ||
            !valid_text(record.username, 1024, false) || !record.secret_size || record.secret_size > 256u * 1024u ||
            (record.secret_size && !record.secret)) return set_error(error, "credential record is invalid");
        if ((record.purpose == CredentialPurpose::Imap && record.username != imap_user) ||
            (record.purpose == CredentialPurpose::Smtp && record.username != smtp_user) ||
            (record.purpose == CredentialPurpose::Shared && record.username != imap_user && record.username != smtp_user))
            return set_error(error, "credential username does not match the bound account configuration");
        if (!encode_account(account, writer) ||
            !writer.u8(static_cast<uint8_t>(record.purpose)) || !writer.text(record.username, 1024) ||
            !writer.u32(static_cast<uint32_t>(record.secret_size)) || writer.raw(record.secret, record.secret_size) == false)
            return set_error(error, "credential packet exceeds its limit");
    }
    if (!writer.finish(packet)) return set_error(error, "out of memory encoding credentials");
    return true;
}

static bool validate_remembered_credentials(const SecretBytes &packet, Text &error) {
    error.clear();
    if (packet.empty() || packet.size() > MaxSecretPacketBytes)
        return set_error(error, "remembered credential payload is empty or oversized");
    Reader reader(packet.data(), packet.size());
    uint16_t count = 0;
    if (!get_magic(reader, "RMCV", RememberedCredentialPayloadVersion) || !reader.u16(count) || count > MaxAccounts * 3)
        return set_error(error, "remembered credential payload header is invalid");
    for (uint16_t i = 0; i < count; ++i) {
        Account configuration;
        uint8_t purpose = 0;
        Text username;
        uint32_t size = 0;
        const unsigned char *bytes = nullptr;
        if (!decode_account(reader, configuration) ||
            (configuration.id && !configuration.generation) || (!configuration.id && configuration.generation) ||
            !reader.u8(purpose) || purpose < static_cast<uint8_t>(CredentialPurpose::Imap) ||
            purpose > static_cast<uint8_t>(CredentialPurpose::Shared) ||
            !reader.text(username, 1024) || !valid_text(username, 1024, false) ||
            !reader.u32(size) || !size || size > 256u * 1024u || !reader.view(bytes, size))
            return set_error(error, "remembered credential record is malformed");
        Writer configuration_identity(MaxTaskPayloadBytes);
        if (!encode_account(configuration, configuration_identity))
            return set_error(error, "remembered credential account identity is invalid");
        const Text &imap_user = configuration.username;
        const Text &smtp_user = configuration.smtp_username.empty() ? configuration.username : configuration.smtp_username;
        if ((purpose == static_cast<uint8_t>(CredentialPurpose::Imap) && username != imap_user) ||
            (purpose == static_cast<uint8_t>(CredentialPurpose::Smtp) && username != smtp_user) ||
            (purpose == static_cast<uint8_t>(CredentialPurpose::Shared) &&
                username != imap_user && username != smtp_user))
            return set_error(error, "remembered credential username does not match its configuration");
    }
    if (reader.remaining()) return set_error(error, "remembered credential payload has trailing bytes");
    return true;
}

static bool bind_remembered_credentials(SecretBytes remembered, uint64_t current_profile_epoch,
    SecretBytes &live_packet, Text &error) {
    error.clear();
    if (!current_profile_epoch || !validate_remembered_credentials(remembered, error))
        return current_profile_epoch ? false : set_error(error, "live credentials require a nonzero profile epoch");
    Writer writer(MaxSecretPacketBytes);
    if (!put_magic(writer, "RMCL", TaskPayloadVersion) || !writer.u64(current_profile_epoch) ||
        remembered.size() > UINT32_MAX || !writer.u32(static_cast<uint32_t>(remembered.size())) ||
        !writer.raw(remembered.data(), remembered.size()) || !writer.finish(live_packet))
        return set_error(error, "live credential packet exceeds its limit");
    return true;
}

static bool remembered_credentials_for_epoch(const SecretBytes &live_packet, uint64_t current_profile_epoch,
    SecretBytes &remembered, Text &error) {
    error.clear();
    remembered.clear();
    if (!current_profile_epoch || live_packet.empty() || live_packet.size() > MaxSecretPacketBytes)
        return set_error(error, "live credentials require a nonzero current profile epoch");
    Reader reader(live_packet.data(), live_packet.size());
    uint64_t packet_epoch = 0;
    uint32_t size = 0;
    const unsigned char *bytes = nullptr;
    if (!get_magic(reader, "RMCL", TaskPayloadVersion) || !reader.u64(packet_epoch) ||
        !packet_epoch || packet_epoch != current_profile_epoch || !reader.u32(size) ||
        size > MaxSecretPacketBytes || !reader.view(bytes, size) || reader.remaining())
        return set_error(error, "live credential packet is malformed or belongs to another profile epoch");
    SecretBytes next;
    if (!next.set(bytes, size) || !validate_remembered_credentials(next, error))
        return error.empty() ? set_error(error, "cannot decode remembered credentials") : false;
    remembered = static_cast<SecretBytes &&>(next);
    return true;
}

bool worker_encode_credentials(const CredentialInput *records, size_t count,
    uint64_t current_profile_epoch, SecretBytes &packet, Text &error) {
    if (!current_profile_epoch) return set_error(error, "live credentials require a nonzero profile epoch");
    SecretBytes remembered;
    if (!encode_remembered_credentials(records, count, remembered, error)) return false;
    return bind_remembered_credentials(static_cast<SecretBytes &&>(remembered), current_profile_epoch,
        packet, error);
}

bool worker_encode_account_credential(const Account &configuration, CredentialPurpose purpose,
    uint64_t current_profile_epoch, SecretBytes password, SecretBytes &packet, Text &error) {
    CredentialInput record;
    record.configuration = configuration;
    record.purpose = purpose;
    record.username = purpose == CredentialPurpose::Smtp && !configuration.smtp_username.empty() ?
        configuration.smtp_username : configuration.username;
    record.secret = password.data();
    record.secret_size = password.size();
    return worker_encode_credentials(&record, 1, current_profile_epoch, packet, error);
}

bool worker_find_credential(const SecretBytes &packet, const Account &configuration,
    CredentialPurpose purpose, const Text &username, uint64_t current_profile_epoch,
    SecretBytes &secret, Text &error) {
    error.clear(); secret.clear();
    SecretBytes remembered;
    if (!current_profile_epoch || (configuration.id && !configuration.generation) ||
        (!configuration.id && configuration.generation) ||
        (purpose != CredentialPurpose::Imap && purpose != CredentialPurpose::Smtp))
        return set_error(error, "credential lookup identity is invalid");
    if (!remembered_credentials_for_epoch(packet, current_profile_epoch,
            remembered, error)) return false;
    Writer expected(MaxTaskPayloadBytes);
    if (!encode_account(configuration, expected)) return set_error(error, "account configuration identity is invalid");
    Reader reader(remembered.data(), remembered.size()); uint16_t version = 0, count = 0; char magic[4];
    if (!reader.raw(magic, 4) || memcmp(magic, "RMCV", 4) || !reader.u16(version) ||
        version != RememberedCredentialPayloadVersion || !reader.u16(count) || count > MaxAccounts * 3)
        return set_error(error, "credential packet header is invalid");
    bool found = false; SecretBytes matched;
    for (uint16_t i = 0; i < count; ++i) {
        Account stored_configuration; uint8_t stored_purpose = 0; Text stored_user;
        uint32_t size = 0; const unsigned char *bytes = nullptr;
        if (!decode_account(reader, stored_configuration) || !reader.u8(stored_purpose) ||
            stored_purpose < static_cast<uint8_t>(CredentialPurpose::Imap) ||
            stored_purpose > static_cast<uint8_t>(CredentialPurpose::Shared) ||
            !reader.text(stored_user, 1024) || !reader.u32(size) || size > 256u * 1024u ||
            !reader.view(bytes, size)) return set_error(error, "credential packet record is malformed");
        const bool purpose_match = stored_purpose == static_cast<uint8_t>(purpose) ||
            stored_purpose == static_cast<uint8_t>(CredentialPurpose::Shared);
        Writer stored(MaxTaskPayloadBytes);
        if (!encode_account(stored_configuration, stored)) return set_error(error, "stored credential identity is malformed");
        const bool configuration_match = stored.size() == expected.size() &&
            !memcmp(stored.data(), expected.data(), expected.size());
        const bool username_match = stored_user == username ||
            (stored_purpose == static_cast<uint8_t>(CredentialPurpose::Shared) &&
             (stored_user == configuration.username ||
              stored_user == (configuration.smtp_username.empty() ? configuration.username : configuration.smtp_username)));
        if (configuration_match && purpose_match && username_match) {
            if (found || !matched.set(bytes, size)) return set_error(error, "credential packet has duplicate matching entries");
            found = true;
        }
    }
    if (reader.remaining() || !found) return set_error(error, "no credential matches this account configuration");
    secret = static_cast<SecretBytes &&>(matched);
    return true;
}

static bool credential_configuration_matches_default_metadata_change(
    const Account &remembered, const Account &current) {
    if (!remembered.id || remembered.id != current.id || !remembered.generation ||
        remembered.generation != current.generation) return false;
    Account remembered_identity = remembered;
    Account current_identity = current;
    remembered_identity.is_default = false;
    current_identity.is_default = false;
    Writer remembered_wire(MaxTaskPayloadBytes), current_wire(MaxTaskPayloadBytes);
    if (!encode_account(remembered_identity, remembered_wire) ||
        !encode_account(current_identity, current_wire)) return false;
    return remembered_wire.size() == current_wire.size() &&
        !memcmp(remembered_wire.data(), current_wire.data(), remembered_wire.size());
}

/* Called only after Vault::unlock authenticates the master password. Rebind
 * remembered records to current durable account snapshots only when account
 * ID, generation, and every setting other than the default-selection bit are
 * unchanged. Ordinary live-packet lookup remains byte-exact. */
static bool rebind_unlocked_vault_credentials(const SecretBytes &remembered,
    const AccountList &current_accounts, SecretBytes &rebound, Text &error) {
    if (!validate_remembered_credentials(remembered, error)) return false;
    Reader reader(remembered.data(), remembered.size());
    uint16_t count = 0;
    if (!get_magic(reader, "RMCV", RememberedCredentialPayloadVersion) || !reader.u16(count) || count > MaxAccounts * 3)
        return set_error(error, "remembered credential payload header is invalid");

    BoundedArray<CredentialInput, MaxAccounts * 3> records;
    if (!records.reserve(count)) return set_error(error, "cannot allocate rebound credential records");
    for (uint16_t i = 0; i < count; ++i) {
        Account remembered_account;
        uint8_t purpose = 0;
        Text username;
        uint32_t secret_size = 0;
        const unsigned char *secret = nullptr;
        if (!decode_account(reader, remembered_account) || !reader.u8(purpose) ||
            !reader.text(username, 1024) || !reader.u32(secret_size) ||
            !reader.view(secret, secret_size))
            return set_error(error, "remembered credential record is malformed");

        const Account *current_account = nullptr;
        for (size_t account_index = 0; account_index < current_accounts.size(); ++account_index) {
            if (!credential_configuration_matches_default_metadata_change(
                    remembered_account, current_accounts[account_index])) continue;
            if (current_account) return set_error(error, "remembered credential account identity is ambiguous");
            current_account = &current_accounts[account_index];
        }
        if (!current_account) continue; // Removed or otherwise edited accounts stay unusable.

        CredentialInput record;
        record.configuration = *current_account;
        record.purpose = static_cast<CredentialPurpose>(purpose);
        record.username = username;
        record.secret = secret;
        record.secret_size = secret_size;
        if (!records.push(static_cast<CredentialInput &&>(record)))
            return set_error(error, "rebound credential records exceed their limit");
    }
    if (reader.remaining()) return set_error(error, "remembered credential payload has trailing bytes");
    return encode_remembered_credentials(records.data(), records.size(), rebound, error);
}

bool worker_encode_secret(WorkerSecretPurpose purpose, SecretBytes first,
    SecretBytes second, SecretBytes &packet, Text &error) {
    error.clear();
    if (purpose == WorkerSecretPurpose::ImapPassword || purpose == WorkerSecretPurpose::SmtpPassword ||
        static_cast<uint8_t>(purpose) < static_cast<uint8_t>(WorkerSecretPurpose::VaultMaster) ||
        static_cast<uint8_t>(purpose) > static_cast<uint8_t>(WorkerSecretPurpose::BackupMaster) ||
        first.empty() || first.size() > MaxSecretPacketBytes || second.size() > MaxSecretPacketBytes ||
        first.size() + second.size() > MaxSecretPacketBytes - 16)
        return set_error(error, "secret packet exceeds its limit");
    const bool permits_second = purpose == WorkerSecretPurpose::VaultSave ||
        purpose == WorkerSecretPurpose::VaultPasswordChange;
    if ((permits_second == false && !second.empty()) ||
        (purpose == WorkerSecretPurpose::VaultPasswordChange && second.empty()))
        return set_error(error, "secret packet purpose and fields do not agree");
    Writer writer(MaxSecretPacketBytes);
    if (!put_magic(writer, "RMSS", TaskPayloadVersion) || !writer.u8(static_cast<uint8_t>(purpose)) ||
        !writer.u32(static_cast<uint32_t>(first.size())) || !writer.u32(static_cast<uint32_t>(second.size())) ||
        !writer.raw(first.data(), first.size()) || !writer.raw(second.data(), second.size()) || !writer.finish(packet))
        return set_error(error, "cannot encode secret packet");
    return true;
}

bool worker_decode_secret(SecretBytes packet, WorkerSecretPurpose expected,
    SecretBytes &first, SecretBytes &second, Text &error) {
    error.clear(); first.clear(); second.clear();
    if (packet.size() > MaxSecretPacketBytes ||
        static_cast<uint8_t>(expected) < static_cast<uint8_t>(WorkerSecretPurpose::VaultMaster) ||
        static_cast<uint8_t>(expected) > static_cast<uint8_t>(WorkerSecretPurpose::BackupMaster))
        return set_error(error, "secret packet exceeds its limit or requires an account-bound credential record");
    Reader reader(packet.data(), packet.size()); char magic[4]; uint16_t version = 0; uint8_t purpose = 0;
    uint32_t first_size = 0, second_size = 0; const unsigned char *first_data = nullptr, *second_data = nullptr;
    if (!reader.raw(magic, 4) || memcmp(magic, "RMSS", 4) || !reader.u16(version) ||
        version != TaskPayloadVersion || !reader.u8(purpose) || purpose != static_cast<uint8_t>(expected) ||
        !reader.u32(first_size) || !reader.u32(second_size) || first_size > MaxSecretPacketBytes ||
        second_size > MaxSecretPacketBytes || first_size + static_cast<uint64_t>(second_size) > MaxSecretPacketBytes ||
        !reader.view(first_data, first_size) || !reader.view(second_data, second_size) || reader.remaining())
        return set_error(error, "secret packet is malformed or has the wrong purpose");
    if (!first_size || (expected != WorkerSecretPurpose::VaultSave &&
            expected != WorkerSecretPurpose::VaultPasswordChange && second_size))
        return set_error(error, "secret packet fields do not match its purpose");
    if (expected == WorkerSecretPurpose::VaultPasswordChange && !second_size)
        return set_error(error, "new vault password is empty");
    if (!first.set(first_data, first_size) || !second.set(second_data, second_size)) {
        first.clear(); second.clear(); return set_error(error, "out of memory decoding secret packet");
    }
    return true;
}

namespace {

bool fail_task(WorkerResult &result, const char *message, WorkerStatus status = WorkerStatus::Failed) {
    result.status = status;
    result.diagnostic.set(message);
    return false;
}

bool decode_input(const WorkerRequest &request, WorkerTaskInput &input, Text &error) {
    if (request.payload.empty()) return true;
    return worker_decode_task_input(request.kind, request.payload.data(), request.payload.size(), input, error);
}

bool encode_startup_result(Store &store, uint64_t account_id, WorkerResult &result, Text &error) {
    WorkerStartupSnapshot snapshot;
    if (!store.list_accounts(snapshot.accounts, error)) return false;
    snapshot.selected_account_id = account_id;
    if (account_id && !store.list_folders(account_id, snapshot.folders, error)) return false;
    return worker_encode_startup_snapshot(snapshot, result.payload, error);
}

bool load_account(Store &store, const WorkerRequest &request, Account &account, Text &error) {
    if (!request.account_id || !request.account_generation || !store.get_account(request.account_id, account, error))
        return set_error(error, "account request is missing its durable identity");
    if (account.generation != request.account_generation)
        return set_error(error, "account configuration changed before network operation");
    return true;
}

bool credential_for(SecretBytes &packet, const Account &account, CredentialPurpose purpose,
    uint64_t current_profile_epoch, SecretBytes &password, Text &error) {
    const Text &username = purpose == CredentialPurpose::Imap || account.smtp_username.empty() ?
        account.username : account.smtp_username;
    return worker_find_credential(packet, account, purpose, username, current_profile_epoch, password, error);
}

bool serialize_mailboxes(const Envelope &envelope, Text &sender, Text &recipients, Text &error) {
    if (!serialize_address(envelope.sender, sender, error) || envelope.recipients.empty()) return false;
    Array<char> result;
    for (size_t i = 0; i < envelope.recipients.size(); ++i) {
        Text address;
        if (!serialize_address(envelope.recipients[i], address, error)) return false;
        if (i && !result.push(',')) return set_error(error, "out of memory storing envelope recipients");
        for (size_t j = 0; j < address.size(); ++j)
            if (!result.push(address.c_str()[j])) return set_error(error, "out of memory storing envelope recipients");
    }
    if (!recipients.set(result.data(), result.size())) return set_error(error, "out of memory storing envelope recipients");
    return true;
}

bool parse_envelope(const SendOperation &operation, Envelope &envelope, Text &error) {
    Array<Address> senders, recipients;
    if (!parse_addresses(operation.sender.c_str(), operation.sender.size(), senders, error) || senders.size() != 1 ||
        !parse_addresses(operation.recipients.c_str(), operation.recipients.size(), recipients, error) || recipients.empty())
        return set_error(error, "durable SMTP envelope is malformed");
    envelope.sender = static_cast<Address &&>(senders[0]);
    envelope.recipients = static_cast<Array<Address> &&>(recipients);
    return true;
}

struct ByteMessageSource { const Bytes *bytes; };
bool byte_message_read(const void *opaque, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size) {
    const ByteMessageSource *source = static_cast<const ByteMessageSource *>(opaque);
    if (!source || !source->bytes || offset > source->bytes->size()) return false;
    const size_t available = source->bytes->size() - offset;
    const size_t amount = available < capacity ? available : capacity;
    if (amount) memcpy(buffer, source->bytes->data() + offset, amount);
    *read_size = amount;
    return true;
}

struct SendStageContext {
    Store *store;
    uint64_t operation_id;
    uint64_t account_generation;
    SendStage current_stage;
    Text *error;
};
bool persist_smtp_stage(void *opaque, SendStage stage) {
    SendStageContext *context = static_cast<SendStageContext *>(opaque);
    return context->store->advance_send_operation(context->operation_id, context->account_generation,
        stage, stage == SendStage::Accepted, *context->error) &&
        (context->current_stage = stage, true);
}

struct WipeBytesGuard {
    Bytes *bytes;
    ~WipeBytesGuard() {
        if (bytes && bytes->data() && bytes->size())
            detail::wipe_memory(const_cast<unsigned char *>(bytes->data()), bytes->size());
    }
};

bool extract_finalized_identity(const Bytes &message, Text &message_id, Text &date, Text &from, Text &error) {
    message_id.clear(); date.clear(); from.clear(); error.clear();
    if (message.empty() || message.size() > MaxRawMessageBytes) return set_error(error, "finalized MIME spool is empty or too large");
    const size_t scan_limit = message.size() < MaxHeaderBytes ? message.size() : MaxHeaderBytes;
    size_t at = 0; unsigned message_id_count = 0, date_count = 0, from_count = 0;
    Array<char> from_value;
    bool previous_header_was_from = false;
    while (at < scan_limit) {
        size_t end = at;
        while (end + 1 < scan_limit && !(message.data()[end] == '\r' && message.data()[end + 1] == '\n')) ++end;
        if (end + 1 >= scan_limit) return set_error(error, "finalized MIME spool has an incomplete header line");
        if (end == at) break;
        if (message.data()[at] == ' ' || message.data()[at] == '\t') {
            if (previous_header_was_from) {
                size_t continuation = at;
                while (continuation < end &&
                    (message.data()[continuation] == ' ' || message.data()[continuation] == '\t'))
                    ++continuation;
                if (from_value.size() + 1 + end - continuation > MaxHeaderBytes ||
                    !from_value.push(' '))
                    return set_error(error, "cannot unfold finalized From header");
                for (size_t i = continuation; i < end; ++i)
                    if (!from_value.push(static_cast<char>(message.data()[i])))
                        return set_error(error, "cannot unfold finalized From header");
            }
            at = end + 2;
            continue;
        }
        previous_header_was_from = false;
        size_t colon = at;
        while (colon < end && message.data()[colon] != ':') ++colon;
        if (colon < end) {
            const size_t name_size = colon - at;
            size_t value = colon + 1;
            while (value < end && (message.data()[value] == ' ' || message.data()[value] == '\t')) ++value;
            const size_t value_size = end - value;
            auto header_is = [&](const char *name) {
                const size_t wanted = strlen(name);
                if (wanted != name_size) return false;
                for (size_t i = 0; i < wanted; ++i) {
                    unsigned char c = message.data()[at + i];
                    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
                    if (c != static_cast<unsigned char>(name[i])) return false;
                }
                return true;
            };
            if (header_is("message-id")) {
                ++message_id_count;
                if (!message_id.set(reinterpret_cast<const char *>(message.data() + value), value_size))
                    return set_error(error, "cannot save finalized Message-ID");
            } else if (header_is("date")) {
                ++date_count;
                if (!date.set(reinterpret_cast<const char *>(message.data() + value), value_size))
                    return set_error(error, "cannot save finalized Date");
            } else if (header_is("from")) {
                ++from_count;
                previous_header_was_from = true;
                if (from_count == 1) {
                    if (value_size > MaxHeaderBytes || !from_value.reserve(value_size))
                        return set_error(error, "finalized From header is too large");
                    for (size_t i = value; i < end; ++i)
                        if (!from_value.push(static_cast<char>(message.data()[i])))
                            return set_error(error, "cannot save finalized From header");
                }
            }
        }
        at = end + 2;
    }
    if (from_count == 1 && !from.set(from_value.data(), from_value.size()))
        return set_error(error, "cannot save finalized From header");
    if (message_id_count != 1 || date_count != 1 || from_count != 1 || message_id.empty() || date.empty() || from.empty())
        return set_error(error, "finalized MIME spool must contain one Date, Message-ID and From header");
    if (message_id.size() < 5 || message_id.c_str()[0] != '<' || message_id.c_str()[message_id.size() - 1] != '>')
        return set_error(error, "finalized MIME Message-ID is malformed");
    for (size_t i = 0; i < message_id.size(); ++i)
        if (message_id.c_str()[i] <= 0x20 || message_id.c_str()[i] >= 0x7f)
            return set_error(error, "finalized MIME Message-ID is malformed");
    return true;
}

int encoded_word_base64_value(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

bool ascii_equal_folded_bytes(const char *bytes, size_t length, const char *expected) {
    if (!bytes) return false;
    const size_t expected_length = strlen(expected);
    if (length != expected_length) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char actual = static_cast<unsigned char>(bytes[i]);
        unsigned char wanted = static_cast<unsigned char>(expected[i]);
        if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
        if (wanted >= 'A' && wanted <= 'Z') wanted += 'a' - 'A';
        if (actual != wanted) return false;
    }
    return true;
}

bool decode_utf8_base64_word(const char *bytes, size_t length, Array<char> &decoded) {
    if (!length || length % 4) return false;
    for (size_t at = 0; at < length; at += 4) {
        const bool final_quartet = at + 4 == length;
        const int a = encoded_word_base64_value(static_cast<unsigned char>(bytes[at]));
        const int b = encoded_word_base64_value(static_cast<unsigned char>(bytes[at + 1]));
        if (a < 0 || b < 0) return false;
        const bool pad_c = bytes[at + 2] == '=';
        const bool pad_d = bytes[at + 3] == '=';
        const int c = pad_c ? 0 : encoded_word_base64_value(static_cast<unsigned char>(bytes[at + 2]));
        const int d = pad_d ? 0 : encoded_word_base64_value(static_cast<unsigned char>(bytes[at + 3]));
        if (c < 0 || d < 0 || (pad_c && !pad_d) ||
            ((pad_c || pad_d) && !final_quartet)) return false;
        if (!decoded.push(static_cast<char>((a << 2) | (b >> 4)))) return false;
        if (pad_c) {
            if (b & 0x0f) return false;
            continue;
        }
        if (!decoded.push(static_cast<char>(((b & 0x0f) << 4) | (c >> 2)))) return false;
        if (pad_d) {
            if (c & 0x03) return false;
            continue;
        }
        if (!decoded.push(static_cast<char>(((c & 0x03) << 6) | d))) return false;
    }
    return valid_utf8(decoded.data(), decoded.size());
}

bool decode_finalized_display_word(const char *input, size_t length, Text &output) {
    if (!input || length < 12 || length > 75 || input[0] != '=' || input[1] != '?') return false;
    size_t charset_end = 2;
    while (charset_end < length && input[charset_end] != '?') ++charset_end;
    if (charset_end == length ||
        !ascii_equal_folded_bytes(input + 2, charset_end - 2, "utf-8") ||
        charset_end + 2 >= length ||
        (input[charset_end + 1] != 'B' && input[charset_end + 1] != 'b') ||
        input[charset_end + 2] != '?' || input[length - 2] != '?' || input[length - 1] != '=')
        return false;
    const size_t payload_start = charset_end + 3;
    const size_t payload_length = length - payload_start - 2;
    Array<char> decoded;
    if (!decode_utf8_base64_word(input + payload_start, payload_length, decoded)) return false;
    return output.set(decoded.data(), decoded.size());
}

bool decode_finalized_display_phrase(const Text &input, Text &output) {
    Array<char> decoded;
    size_t at = 0;
    bool previous_was_encoded = false;
    while (at < input.size()) {
        if (input.c_str()[at] == ' ' || input.c_str()[at] == '\t') {
            const size_t whitespace_start = at;
            while (at < input.size() &&
                (input.c_str()[at] == ' ' || input.c_str()[at] == '\t')) ++at;
            const bool next_is_encoded = at + 1 < input.size() &&
                input.c_str()[at] == '=' && input.c_str()[at + 1] == '?';
            if (!(previous_was_encoded && next_is_encoded))
                for (size_t i = whitespace_start; i < at; ++i)
                    if (!decoded.push(input.c_str()[i])) return false;
            continue;
        }
        const size_t token_start = at;
        while (at < input.size() && input.c_str()[at] != ' ' && input.c_str()[at] != '\t') ++at;
        const size_t token_length = at - token_start;
        const bool encoded_word = token_length >= 2 && input.c_str()[token_start] == '=' &&
            input.c_str()[token_start + 1] == '?';
        if (encoded_word) {
            Text word;
            if (!decode_finalized_display_word(input.c_str() + token_start, token_length, word)) return false;
            for (size_t i = 0; i < word.size(); ++i)
                if (!decoded.push(word.c_str()[i])) return false;
        } else {
            for (size_t i = token_start; i < at; ++i) {
                if ((i + 1 < at && input.c_str()[i] == '=' && input.c_str()[i + 1] == '?') ||
                    !decoded.push(input.c_str()[i])) return false;
            }
        }
        previous_was_encoded = encoded_word;
    }
    if (!valid_utf8(decoded.data(), decoded.size())) return false;
    return output.set(decoded.data(), decoded.size());
}

bool serialized_sender_matches(const Address &actual_address, const Address &expected) {
    Text actual, expected_value, parse_error;
    return serialize_address(actual_address, actual, parse_error) &&
        serialize_address(expected, expected_value, parse_error) && actual == expected_value;
}

bool finalized_sender_matches(const Text &header_value, const Address &expected) {
    Array<Address> actual_addresses;
    Text parse_error;
    if (!parse_addresses(header_value.c_str(), header_value.size(), actual_addresses, parse_error) ||
        actual_addresses.size() != 1) return false;
    const Address &original = actual_addresses[0];
    if (serialized_sender_matches(original, expected)) return true;
    if (original.display_name.empty() || original.display_name_quoted) return false;

    Text decoded_name;
    if (!decode_finalized_display_phrase(original.display_name, decoded_name)) return false;
    Address decoded = original;
    decoded.display_name = static_cast<Text &&>(decoded_name);
    decoded.display_name_quoted = false;
    return serialized_sender_matches(decoded, expected);
}

bool validate_operation_spool(const SendOperation &operation, const Bytes &spool,
    Envelope &envelope, Text &error) {
    Text message_id, date, from;
    if (spool.size() != operation.spool_size ||
        !extract_finalized_identity(spool, message_id, date, from, error) ||
        message_id != operation.message_id || !parse_envelope(operation, envelope, error))
        return set_error(error, "durable send spool does not match its validated identity");
    if (!finalized_sender_matches(from, envelope.sender))
        return set_error(error, "durable send spool sender differs from its SMTP envelope");
    return true;
}

bool finish_operation_result(const SendOperation &operation, const Text &diagnostic,
    WorkerStatus status, WorkerResult &result, Text &error) {
    WorkerOperationResult value;
    value.operation_id = operation.id;
    value.stage = operation.stage;
    value.delivery_count = operation.delivery_count;
    value.message_id = operation.message_id;
    value.diagnostic = diagnostic;
    if (!worker_encode_operation_result(value, result.payload, error)) return false;
    result.status = status;
    result.diagnostic = diagnostic;
    return true;
}

bool finish_operation_summary_result(const SendOperationSummary &operation, const Text &diagnostic,
    WorkerStatus status, WorkerResult &result, Text &error) {
    WorkerOperationResult value;
    value.operation_id = operation.id;
    value.stage = operation.stage;
    value.delivery_count = operation.delivery_count;
    value.message_id = operation.message_id;
    value.diagnostic = diagnostic;
    if (!worker_encode_operation_result(value, result.payload, error)) return false;
    result.status = status;
    result.diagnostic = diagnostic;
    return true;
}

enum class FolderUidGuard : uint8_t { Exact, Required };

bool account_and_folder_identity(Store &store, const WorkerRequest &request,
    const FolderKey &key, Folder &folder, Account &account, Text &error,
    FolderUidGuard uid_guard = FolderUidGuard::Exact, bool allow_hierarchy = false) {
    if (!request.folder_generation)
        return set_error(error, "folder request requires its original nonzero generation");
    if (!load_account(store, request, account, error) || !store.get_folder(key, folder, error)) return false;
    if ((!allow_hierarchy && !folder.selectable) || folder.generation != request.folder_generation)
        return set_error(error, "folder identity changed before operation");
    if ((uid_guard == FolderUidGuard::Required && !request.folder_uidvalidity) ||
        folder.uidvalidity != request.folder_uidvalidity)
        return set_error(error, "folder UIDVALIDITY is missing or changed");
    return true;
}

bool find_exact_message_id(ImapSession &imap, const FolderKey &folder, const Text &message_id,
    const OperationContext &context, bool &found, Text &error) {
    found = false;
    Array<MessageKey> keys;
    if (!imap.search_message_id(folder, message_id, keys, context, error)) return false;
    for (size_t i = 0; i < keys.size(); ++i) {
        Array<ImapMessage> messages;
        if (!imap.fetch(&keys[i], 1, messages, context, error)) return false;
        if (!messages.size() || messages[0].headers_only) continue;
        ParsedMessage parsed;
        if (!parse_message(reinterpret_cast<const char *>(messages[0].raw.data()),
                messages[0].raw.size(), parsed, error))
            return set_error(error, "cannot verify the decoded Message-ID in a Sent-folder candidate");
        if (parsed.message_id == message_id) {
            found = true;
            return true;
        }
    }
    return true;
}

bool archive_operation(Store &store, const Account &account,
    SendOperation &operation, const Bytes &spool, SecretBytes &credential_packet,
    uint64_t current_profile_epoch, const OperationContext &context, Text &diagnostic) {
    diagnostic.clear();
    if (!send_operation_delivery_state_valid(operation.stage, operation.delivery_count,
            operation.archive_review_generation))
        return set_error(diagnostic, "archive-only retry requires a confirmed or explicitly reviewed delivery");
    const uint64_t archive_generation = operation.archive_review_generation ?
        operation.archive_review_generation : operation.account_generation;
    if (account.generation != archive_generation)
        return set_error(diagnostic, "account settings changed; choose and confirm the archive destination in delivery review");
    Text folder_name = operation.archive_folder.empty() ? account.sent_folder : operation.archive_folder;
    if (folder_name.empty())
        return set_error(diagnostic, "Sent folder is not configured; choose a destination in delivery review");
    FolderKey key; key.account_id = account.id; key.server_name = folder_name;
    if (operation.archive_review_generation) {
        Folder reviewed_folder;
        if (!store.get_folder(key, reviewed_folder, diagnostic) || !reviewed_folder.selectable ||
            reviewed_folder.generation != operation.archive_folder_generation ||
            reviewed_folder.uidvalidity != operation.archive_folder_uidvalidity)
            return set_error(diagnostic, "reviewed archive folder changed; review its current identity again");
    }
    if (operation.stage == SendStage::Accepted || operation.stage == SendStage::Sent ||
        operation.stage == SendStage::ReviewedDelivered) {
        if (!store.advance_send_operation(operation.id, archive_generation,
                SendStage::ArchivePending, false, diagnostic)) return false;
        operation.stage = SendStage::ArchivePending;
    }
    if (operation.stage != SendStage::ArchivePending && operation.stage != SendStage::ArchiveFailed)
        return set_error(diagnostic, "send operation is not awaiting Sent-folder archive");
    if (operation.stage == SendStage::ArchiveFailed &&
        !store.advance_send_operation(operation.id, archive_generation,
            SendStage::ArchivePending, false, diagnostic)) return false;
    operation.stage = SendStage::ArchivePending;
    SecretBytes password;
    if (!credential_for(credential_packet, account, CredentialPurpose::Imap,
            current_profile_epoch, password, diagnostic)) return false;
    ImapSession imap;
    if (!imap.connect(account, context, diagnostic) ||
        !imap.login(SecretView{password.data(), password.size()}, context, diagnostic)) return false;
    Folder selected;
    if (!imap.select(key, selected, context, diagnostic)) return false;
    if (selected.uidvalidity == 0) return set_error(diagnostic, "Sent folder SELECT returned no UIDVALIDITY");
    if (operation.archive_review_generation &&
        selected.uidvalidity != operation.archive_folder_uidvalidity)
        return set_error(diagnostic, "reviewed archive folder UIDVALIDITY changed; review its current identity again");
    bool found = false;
    if (!find_exact_message_id(imap, key, operation.message_id, context, found, diagnostic)) return false;
    if (found) {
        if (!store.advance_send_operation(operation.id, archive_generation, SendStage::Archived, false, diagnostic)) return false;
        operation.stage = SendStage::Archived;
        return true;
    }
    ByteMessageSource bytes = {&spool};
    MessageSource source = {&bytes, byte_message_read, spool.size()};
    const ImapAppendOutcome outcome = imap.append(key, source, context, diagnostic);
    if (outcome == ImapAppendOutcome::Accepted) {
        if (!store.advance_send_operation(operation.id, archive_generation, SendStage::Archived, false, diagnostic)) return false;
        operation.stage = SendStage::Archived;
        return true;
    }
    Text stage_error;
    (void)store.advance_send_operation(operation.id, archive_generation, SendStage::ArchiveFailed, false, stage_error);
    operation.stage = SendStage::ArchiveFailed;
    return set_error(diagnostic, outcome == ImapAppendOutcome::Uncertain ?
        "SMTP delivery is confirmed; Sent APPEND outcome is uncertain and can only be retried in archive mode" :
        "SMTP delivery is confirmed; Sent APPEND failed and can only be retried in archive mode");
}

bool task_is_coordinator_owner(WorkerTaskKind kind) {
    return kind == WorkerTaskKind::Initialize || kind == WorkerTaskKind::BackupProfile ||
        kind == WorkerTaskKind::RestoreProfile || kind == WorkerTaskKind::ClearCache ||
        kind == WorkerTaskKind::UnlockVault || kind == WorkerTaskKind::SaveVault ||
        kind == WorkerTaskKind::ChangeVaultPassword || kind == WorkerTaskKind::VaultStatus;
}

bool persist_folder_snapshot(Store &store, const Account &account, FolderList &remote, Text &error) {
    FolderList previous;
    if (!store.list_folders(account.id, previous, error)) return false;
    for (size_t i = 0; i < remote.size(); ++i) {
        remote[i].key.account_id = account.id;
        uint64_t expected_generation = 0;
        for (size_t j = 0; j < previous.size(); ++j) {
            if (previous[j].key.server_name == remote[i].key.server_name) {
                expected_generation = previous[j].generation;
                break;
            }
        }
        if (!store.save_folder_metadata_guarded(remote[i], account.generation, expected_generation, error)) return false;
    }
    for (size_t i = 0; i < previous.size(); ++i) {
        bool still_present = false;
        for (size_t j = 0; j < remote.size(); ++j)
            if (previous[i].key.server_name == remote[j].key.server_name) { still_present = true; break; }
        if (!still_present && !store.delete_folder_guarded(previous[i].key, account.generation,
                previous[i].generation, error)) return false;
    }
    remote.clear();
    return store.list_folders(account.id, remote, error);
}

bool open_imap(const Account &account, SecretBytes &credential_packet,
    uint64_t current_profile_epoch, const OperationContext &context, ImapSession &session,
    SecretBytes &password, Text &error) {
    if (account.incoming_protocol != IncomingProtocol::Imap)
        return set_error(error, "POP3 is not available in this build");
    if (!credential_for(credential_packet, account, CredentialPurpose::Imap,
            current_profile_epoch, password, error) ||
        !session.connect(account, context, error) ||
        !session.login(SecretView{password.data(), password.size()}, context, error)) return false;
    return true;
}

bool request_folder_key(const WorkerRequest &request, const WorkerTaskInput &input, FolderKey &key) {
    if (!request.account_id || request.folder_server_name.empty()) return false;
    bool has_serialized_key = false;
    if (!input.message_key.folder.server_name.empty()) {
        key = input.message_key.folder;
        has_serialized_key = true;
    } else if (!input.page_query.folder.server_name.empty()) {
        key = input.page_query.folder;
        has_serialized_key = true;
    } else {
        key.account_id = request.account_id;
        key.server_name = request.folder_server_name;
    }
    return key.account_id == request.account_id && key.server_name == request.folder_server_name &&
        (!has_serialized_key || !key.server_name.empty());
}

bool request_fetch_message_key(const WorkerRequest &request, const WorkerTaskInput &input,
    MessageKey &key, Text &error) {
    if (!request.account_id || !request.account_generation || request.folder_server_name.empty() ||
        !request.folder_generation || !request.folder_uidvalidity || !request.message_uid ||
        !input.message_key.folder.account_id || input.message_key.folder.server_name.empty() ||
        !input.message_key.uidvalidity || !input.message_key.uid)
        return set_error(error, "FetchMessage requires complete account, folder, UIDVALIDITY and UID identity");
    if (input.message_key.folder.account_id != request.account_id ||
        input.message_key.folder.server_name != request.folder_server_name ||
        input.message_key.uidvalidity != request.folder_uidvalidity ||
        input.message_key.uid != request.message_uid)
        return set_error(error, "FetchMessage payload identity does not match its request frame");
    key = input.message_key;
    return true;
}

bool make_plain_text_preview(const Text &source, Text &preview, bool &truncated, Text &error) {
    if (!valid_utf8(source.c_str(), source.size()))
        return set_error(error, "decoded plain message text is not valid UTF-8");
    size_t preview_size = source.size();
    truncated = preview_size > MaxReaderPlainPreviewBytes;
    if (truncated) {
        preview_size = MaxReaderPlainPreviewBytes;
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(source.c_str());
        if ((static_cast<unsigned char>(bytes[preview_size]) & 0xc0u) == 0x80u) {
            while (preview_size &&
                (static_cast<unsigned char>(bytes[preview_size]) & 0xc0u) == 0x80u)
                --preview_size;
        }
    }
    if (!preview.set(source.c_str(), preview_size))
        return set_error(error, "out of memory creating the bounded plain-text preview");
    return true;
}

bool make_reader_result(const MessageKey &key, const MessageSummary &summary,
    const ParsedMessage &parsed, WorkerReaderResult &reader, Text &error) {
    WorkerReaderResult next;
    next.key = key;
    next.headers_only = summary.headers_only;
    next.from = parsed.from;
    next.reply_to = parsed.reply_to;
    next.to = parsed.to;
    next.cc = parsed.cc;
    next.bcc = parsed.bcc;
    next.subject = parsed.subject;
    next.date = parsed.date;
    next.message_id = parsed.message_id;
    next.in_reply_to = parsed.in_reply_to;
    next.references = parsed.references;
    next.warning = parsed.warning;
    if (!summary.headers_only) {
        if (!make_plain_text_preview(parsed.plain_text, next.plain_text,
                next.plain_text_truncated, error)) return false;
        if (!parsed.html_text.empty()) {
            ReaderRenderedDocument rendered;
            Text render_error;
            if (render_safe_reader_html(parsed.html_text.c_str(), parsed.html_text.size(),
                    parsed.attachments, rendered, render_error)) {
                next.html_text = static_cast<Text &&>(rendered.html);
                next.safe_links = static_cast<WorkerReaderSafeLinks &&>(rendered.links);
                for (size_t i = 0; i < rendered.images.size(); ++i) {
                    WorkerReaderInlineImage image;
                    image.width = rendered.images[i].width;
                    image.height = rendered.images[i].height;
                    image.xpm = rendered.images[i].xpm;
                    if (!next.inline_images.push(static_cast<WorkerReaderInlineImage &&>(image)))
                        return set_error(error, "reader inline image result exceeds its bound");
                }
                next.html_warning = !rendered.warning.empty();
            } else {
                next.html_warning = true;
            }
        }
    } else if (!next.warning.set("Only headers are cached; the complete original and body are unavailable offline.")) {
        return set_error(error, "cannot describe the header-only reader result");
    }
    for (size_t i = 0; i < parsed.attachments.size(); ++i) {
        const Attachment &part = parsed.attachments[i];
        WorkerAttachmentMetadata meta;
        meta.filename = part.filename;
        meta.content_type = part.content_type;
        meta.content_id = part.content_id;
        meta.size = part.data.size();
        meta.inline_part = part.inline_part;
        if (!next.attachments.push(static_cast<WorkerAttachmentMetadata &&>(meta)))
            return set_error(error, "reader attachment metadata exceeds its bound");
    }
    reader = static_cast<WorkerReaderResult &&>(next);
    return true;
}

bool folder_result(Store &store, uint64_t account_id, WorkerResult &result, Text &error) {
    return encode_startup_result(store, account_id, result, error);
}

void stamp_page_context(MessageQueryPage &page, const WorkerRequest &request) {
    for (size_t i = 0; i < page.rows.size(); ++i) {
        page.rows[i].profile_epoch = request.profile_epoch;
        page.rows[i].ui_generation = request.ui_generation;
    }
}

} // namespace

bool worker_execute_task(const WorkerRequest &request, SecretBytes secret,
    const OperationContext &context, WorkerResult &result) {
    result.status = WorkerStatus::Ok;
    WorkerTaskInput input;
    Text error;
#ifdef RELIEFMAIL_TESTING
    if (request.kind == WorkerTaskKind::TestProbe) {
        uint32_t fd = UINT32_MAX;
        if (request.payload.size() != 4) return fail_task(result, "test probe payload is invalid");
        fd = (static_cast<uint32_t>(request.payload.data()[0]) << 24) |
            (static_cast<uint32_t>(request.payload.data()[1]) << 16) |
            (static_cast<uint32_t>(request.payload.data()[2]) << 8) | request.payload.data()[3];
        const char expected[] = "worker-private-secret";
        const bool secret_matches = secret.size() == sizeof(expected) - 1 &&
            !memcmp(secret.data(), expected, sizeof(expected) - 1);
        const bool inherited_open = fd <= INT_MAX && fcntl(static_cast<int>(fd), F_GETFD) >= 0;
        bool secret_visible = false;
        if (environ) for (char **entry = environ; *entry; ++entry)
            if (strstr(*entry, expected)) secret_visible = true;
        const unsigned char response[] = {static_cast<unsigned char>(secret_matches),
            static_cast<unsigned char>(inherited_open), static_cast<unsigned char>(secret_visible)};
        result.payload.set(response, sizeof(response));
        result.status = WorkerStatus::Ok;
        return true;
    }
    if (request.kind == WorkerTaskKind::TestSecretOutput) {
        if (secret.empty() || !result.secret_payload.set(secret.data(), secret.size()))
            return fail_task(result, "test secret output is empty or could not be copied");
        result.status = WorkerStatus::Ok;
        return true;
    }
    if (request.kind == WorkerTaskKind::TestSecretInterleave) {
        static const unsigned char ordinary[] = {0x52, 0x4d, 0x57, 0x52, 0x41, 0x42, 0x43};
        static const char secret_output[] = "secret-before-ordinary-completion";
        if (!result.payload.set(ordinary, sizeof(ordinary)) ||
            !result.secret_payload.set(secret_output, sizeof(secret_output) - 1))
            return fail_task(result, "test interleave result allocation failed");
        result.status = WorkerStatus::Ok;
        return true;
    }
    if (request.kind == WorkerTaskKind::TestSecretPipeBlock) {
        if (secret.size() != 4096) return fail_task(result, "blocked-pipe test secret length changed");
        for (size_t i = 0; i < secret.size(); ++i)
            if (secret.data()[i] != 'S') return fail_task(result, "blocked-pipe test secret was shifted or corrupted");
        const unsigned char marker = 1;
        if (!result.payload.set(&marker, 1)) return fail_task(result, "blocked-pipe test result allocation failed");
        return true;
    }
    if (request.kind == WorkerTaskKind::TestCrash) _exit(17);
    if (request.kind == WorkerTaskKind::TestWait || request.kind == WorkerTaskKind::TestNetworkWait ||
        request.kind == WorkerTaskKind::TestMutationWait ||
        request.kind == WorkerTaskKind::TestNetworkMutationWait ||
        request.kind == WorkerTaskKind::TestMaintenanceWait) {
        for (unsigned i = 0; i < 10000; ++i) {
            if (cancelled(context)) return fail_task(result, "worker cancelled", WorkerStatus::Cancelled);
            usleep(1000);
        }
        return fail_task(result, "test wait watchdog expired");
    }
    if (request.kind == WorkerTaskKind::TestEof) return fail_task(result, "intentional incomplete test response");
#endif
    if (!decode_input(request, input, error)) return fail_task(result, error.c_str());

    if (request.kind == WorkerTaskKind::AccountTestImap || request.kind == WorkerTaskKind::AccountTestSmtp ||
        request.kind == WorkerTaskKind::AccountTest) {
        Account account = static_cast<Account &&>(input.account);
        Store account_store;
        if (request.account_id) {
            if (!request.account_generation || !account_store.open(request.profile_paths, StoreOpenMode::Worker, error) ||
                !load_account(account_store, request, account, error))
                return fail_task(result, error.empty() ? "stored account identity is invalid" : error.c_str(), WorkerStatus::InvalidRequest);
        } else if (account.id || account.generation) {
            return fail_task(result, "unsaved account tests must use an id-zero configuration candidate", WorkerStatus::InvalidRequest);
        }
        Account validation_account = account;
        if (!validation_account.id) {
            validation_account.id = 1;
            validation_account.generation = 1;
        }
        if (!validate_account(validation_account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        const bool smtp = request.kind == WorkerTaskKind::AccountTestSmtp;
        const CredentialPurpose purpose = smtp ? CredentialPurpose::Smtp : CredentialPurpose::Imap;
        SecretBytes password;
        const Text &username = smtp && !account.smtp_username.empty() ? account.smtp_username : account.username;
        if (!worker_find_credential(secret, account, purpose, username,
                request.profile_epoch, password, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        if (smtp) {
            SmtpSession session;
            if (!session.connect(account, context, error) ||
                !session.authenticate(SecretView{password.data(), password.size()}, context, error))
                return fail_task(result, error.c_str());
        } else {
            if (account.incoming_protocol != IncomingProtocol::Imap)
                return fail_task(result, "POP3 is not available in this build", WorkerStatus::InvalidRequest);
            ImapSession session;
            if (!session.connect(account, context, error) ||
                !session.login(SecretView{password.data(), password.size()}, context, error))
                return fail_task(result, error.c_str());
        }
        result.status = WorkerStatus::Ok;
        return true;
    }

    Store store;
    const StoreOpenMode open_mode = task_is_coordinator_owner(request.kind) ?
        StoreOpenMode::CoordinatedOwner : StoreOpenMode::Worker;
    if (!store.open(request.profile_paths, open_mode, error))
        return fail_task(result, error.c_str(), request.kind == WorkerTaskKind::Initialize ?
            WorkerStatus::RecoveryRequired : WorkerStatus::Failed);
    if (request.kind == WorkerTaskKind::VaultStatus) {
        Vault vault;
        if (!vault.open(request.profile_paths, error))
            return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        WorkerVaultStatus status;
        status.exists = vault.exists();
        if (!worker_encode_vault_status(status, result.payload, error))
            return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::ListSendOperations) {
        Account account;
        if (!load_account(store, request, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        WorkerSendOperationPage page;
        if (!store.list_send_operations(account.id, input.page_offset, page, error) ||
            !worker_encode_send_operation_page(page, result.payload, error))
            return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::ReviewArchiveTarget ||
        request.kind == WorkerTaskKind::ResolveUncertainSend) {
        Account account;
        if (!load_account(store, request, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        if (!input.item_id || !input.review_confirmed)
            return fail_task(result, "send history review requires its operation ID and explicit confirmation",
                WorkerStatus::InvalidRequest);
        if (request.kind == WorkerTaskKind::ReviewArchiveTarget) {
            if (input.send_review_action != WorkerSendReviewAction::SetArchiveTarget ||
                !input.item_revision || input.destination_folder.account_id != account.id ||
                input.destination_folder.server_name.empty() || !input.destination_folder_generation ||
                !input.destination_folder_uidvalidity)
                return fail_task(result, "archive review requires an exact same-account folder identity",
                    WorkerStatus::InvalidRequest);
            if (!store.review_send_archive_target(input.item_id, account.id, input.item_revision,
                    account.generation, input.destination_folder, input.destination_folder_generation,
                    input.destination_folder_uidvalidity, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            SendOperationSummary operation;
            if (!store.get_send_operation_summary(input.item_id, operation, error) ||
                operation.account_id != account.id || operation.archive_review_generation != account.generation)
                return fail_task(result, error.empty() ? "reviewed archive identity was not persisted" : error.c_str());
            Text diagnostic;
            diagnostic.set("Archive destination confirmed for the current account settings. Retry archive uses IMAP only.");
            if (!finish_operation_summary_result(operation, diagnostic, WorkerStatus::Ok, result, error))
                return fail_task(result, error.c_str());
            return true;
        }
        if (!input.item_revision)
            return fail_task(result, "uncertain send review requires the original operation generation",
                WorkerStatus::InvalidRequest);
        UncertainSendDecision decision;
        if (input.send_review_action == WorkerSendReviewAction::ConfirmDelivered)
            decision = UncertainSendDecision::ConfirmDelivered;
        else if (input.send_review_action == WorkerSendReviewAction::ConfirmNotDelivered)
            decision = UncertainSendDecision::ConfirmNotDelivered;
        else return fail_task(result, "uncertain send review requires a delivered or not-delivered decision",
            WorkerStatus::InvalidRequest);
        if (!store.resolve_uncertain_send(input.item_id, account.id, input.item_revision,
                account.generation, decision, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        SendOperationSummary operation;
        if (!store.get_send_operation_summary(input.item_id, operation, error) || operation.account_id != account.id)
            return fail_task(result, error.empty() ? "uncertain send review was not persisted" : error.c_str());
        Text diagnostic;
        const WorkerStatus review_status = decision == UncertainSendDecision::ConfirmDelivered ?
            WorkerStatus::CommittedWarning : WorkerStatus::Ok;
        diagnostic.set(decision == UncertainSendDecision::ConfirmDelivered ?
            "You reviewed this as delivered. SMTP acceptance remains unconfirmed; no SMTP retry was started." :
            "You reviewed this as not delivered. The message is bound to the current account settings and ready only for a separately confirmed SMTP send.");
        if (!finish_operation_summary_result(operation, diagnostic, review_status, result, error))
            return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::ListDrafts) {
        Account account;
        if (!load_account(store, request, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        DraftList stored;
        if (!store.list_drafts(account.id, stored, error)) return fail_task(result, error.c_str());
        WorkerDraftList summaries;
        summaries.account_generation = account.generation;
        bool encoded = true;
        for (size_t i = 0; i < stored.size(); ++i) {
            const Draft &draft = stored[i];
            WorkerDraftSummary summary;
            summary.id = draft.id;
            summary.revision = draft.revision;
            summary.subject = draft.subject;
            const Text *raw_to = draft.raw_recipient_input ? &draft.to_input : nullptr;
            if (raw_to) {
                size_t length = raw_to->size() < 512 ? raw_to->size() : 512;
                while (length && length < raw_to->size() &&
                    (static_cast<unsigned char>(raw_to->c_str()[length]) & 0xc0u) == 0x80u) --length;
                if (!summary.to.set(raw_to->c_str(), length)) encoded = false;
            } else if (draft.to.size()) {
                if (!serialize_address(draft.to[0], summary.to, error)) encoded = false;
                if (summary.to.size() > 512) {
                    size_t length = 512;
                    while (length && (static_cast<unsigned char>(summary.to.c_str()[length]) & 0xc0u) == 0x80u) --length;
                    Text prefix;
                    if (!prefix.set(summary.to.c_str(), length)) encoded = false;
                    else summary.to = static_cast<Text &&>(prefix);
                }
            }
            if (!encoded || !summaries.drafts.push(static_cast<WorkerDraftSummary &&>(summary))) {
                encoded = false;
                break;
            }
        }
        for (size_t i = 0; i < stored.size(); ++i) store.release_draft_sources(stored[i]);
        if (!encoded) return fail_task(result, "could not build the bounded draft list");
        if (!worker_encode_draft_list(summaries, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::ComposeCatalog) {
        Account account;
        if (!load_account(store, request, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        if (!valid_text(input.query, 256) || (input.item_id && !input.query.empty()))
            return fail_task(result, "compose catalog query or template identity is invalid", WorkerStatus::InvalidRequest);
        WorkerComposeCatalog catalog;
        catalog.account_generation = account.generation;
        if (account.signature_id) {
            Signature signature;
            if (!store.get_signature(account.signature_id, signature, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            if (signature.enabled) {
                catalog.has_signature = true;
                catalog.signature_id = signature.id;
                catalog.signature_body = signature.body;
            }
        }
        if (input.item_id) {
            Template selected;
            if (!store.get_template(input.item_id, selected, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            catalog.selected_template_id = selected.id;
            catalog.selected_template_subject = selected.subject;
            catalog.selected_template_body = selected.body;
        } else {
            TemplateList templates;
            if (!store.list_templates(templates, error)) return fail_task(result, error.c_str());
            for (size_t i = 0; i < templates.size(); ++i) {
                if (catalog.templates.size() == MaxComposeTemplateChoices) {
                    catalog.templates_truncated = true;
                    break;
                }
                WorkerComposeTemplateChoice choice;
                choice.id = templates[i].id;
                choice.name = templates[i].name;
                choice.subject = templates[i].subject;
                if (!catalog.templates.push(static_cast<WorkerComposeTemplateChoice &&>(choice)))
                    return fail_task(result, "compose template choices exceed their bound");
            }
            if (!input.query.empty()) {
                ContactList contacts;
                if (!store.list_contacts(contacts, error)) return fail_task(result, error.c_str());
                for (size_t i = 0; i < contacts.size(); ++i) {
                    const Contact &contact = contacts[i];
                    const bool name_match = compose_text_contains(contact.display_name, input.query) ||
                        compose_text_contains(contact.organization, input.query);
                    for (size_t j = 0; j < contact.addresses.size(); ++j) {
                        Text address_text;
                        if (!serialize_address(contact.addresses[j], address_text, error))
                            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
                        if (!name_match && !compose_text_contains(address_text, input.query)) continue;
                        bool duplicate = false;
                        for (size_t prior = 0; prior < catalog.contacts.size(); ++prior)
                            if (catalog.contacts[prior].address == address_text) { duplicate = true; break; }
                        if (duplicate) continue;
                        if (catalog.contacts.size() == MaxComposeContactSuggestions) {
                            catalog.contacts_truncated = true;
                            break;
                        }
                        WorkerComposeContactSuggestion suggestion;
                        suggestion.display_name = contact.display_name.empty() ? address_text : contact.display_name;
                        suggestion.address = static_cast<Text &&>(address_text);
                        if (suggestion.display_name.size() > 512) continue;
                        if (!catalog.contacts.push(static_cast<WorkerComposeContactSuggestion &&>(suggestion)))
                            return fail_task(result, "compose contact suggestions exceed their bound");
                    }
                    if (catalog.contacts_truncated) break;
                }
            }
        }
        if (!worker_encode_compose_catalog(catalog, result.payload, error))
            return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::Initialize) {
        if (!store.recover_interrupted_sends(error)) return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        Vault vault;
        if (!vault.open(request.profile_paths, error))
            return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        uint64_t selected = request.account_id;
        AccountList accounts;
        if (!store.list_accounts(accounts, error)) return fail_task(result, error.c_str());
        if (!selected) {
            for (size_t i = 0; i < accounts.size(); ++i)
                if (accounts[i].is_default) { selected = accounts[i].id; break; }
            if (!selected && accounts.size()) selected = accounts[0].id;
        }
        if (!encode_startup_result(store, selected, result, error)) return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::AccountList) {
        uint64_t selected_account_id = 0;
        if (request.account_id) {
            Account selected;
            if (!request.account_generation || !store.get_account(request.account_id, selected, error) ||
                selected.generation != request.account_generation)
                return fail_task(result, "selected account changed before its cached folders were loaded",
                    WorkerStatus::InvalidRequest);
            selected_account_id = request.account_id;
        } else if (request.account_generation) {
            return fail_task(result, "account list has a generation without an account identity",
                WorkerStatus::InvalidRequest);
        }
        if (!encode_startup_result(store, selected_account_id, result, error))
            return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::AccountCreate || request.kind == WorkerTaskKind::AccountUpdate ||
        request.kind == WorkerTaskKind::AccountSetDefault) {
        Account account = input.account;
        if (request.kind != WorkerTaskKind::AccountCreate && (!request.account_id || !request.account_generation))
            return fail_task(result, "account mutation requires a config generation", WorkerStatus::InvalidRequest);
        const uint64_t expected = request.kind == WorkerTaskKind::AccountCreate ? 0 : request.account_generation;
        if (request.kind == WorkerTaskKind::AccountSetDefault) {
            if (!store.get_account(request.account_id, account, error) ||
                (request.account_generation && account.generation != request.account_generation))
                return fail_task(result, "account changed before setting the default");
            account.is_default = true;
        }
        if (request.kind == WorkerTaskKind::AccountCreate && account.id)
            return fail_task(result, "account creation must not provide a stored ID", WorkerStatus::InvalidRequest);
        if (request.kind == WorkerTaskKind::AccountUpdate && account.id != request.account_id)
            return fail_task(result, "account update identity does not match the request", WorkerStatus::InvalidRequest);
        if (!store.save_account_guarded(account, expected, error)) return fail_task(result, error.c_str());
        WorkerAccountResult value; value.account = static_cast<Account &&>(account);
        if (!worker_encode_account_result(value, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::AccountDelete) {
        if (!request.account_id || !request.account_generation)
            return fail_task(result, "account deletion requires a config generation", WorkerStatus::InvalidRequest);
        if (!store.delete_account_guarded(request.account_id, request.account_generation, error))
            return fail_task(result, error.c_str());
        WorkerAccountResult value; value.account.id = request.account_id; value.deleted = true;
        if (!worker_encode_account_result(value, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::FolderList || request.kind == WorkerTaskKind::FolderSelect ||
        request.kind == WorkerTaskKind::HeaderSync) {
        Account account;
        if (!load_account(store, request, account, error)) return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::FolderList) {
            SecretBytes password;
            ImapSession imap;
            if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
                return fail_task(result, error.c_str());
            FolderList folders;
            if (!imap.list(folders, context, error) || !persist_folder_snapshot(store, account, folders, error) ||
                !folder_result(store, account.id, result, error)) return fail_task(result, error.c_str());
            return true;
        }
        FolderKey key;
        if (!request_folder_key(request, input, key))
            return fail_task(result, "folder task requires a folder from the requested account", WorkerStatus::InvalidRequest);
        Folder folder;
        if (!account_and_folder_identity(store, request, key, folder, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        SecretBytes password;
        ImapSession imap;
        if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
            return fail_task(result, error.c_str());
        Folder selected;
        if (!imap.select(key, selected, context, error)) return fail_task(result, error.c_str());
        if (!selected.uidvalidity || selected.key != key)
            return fail_task(result, "IMAP SELECT did not establish the requested folder identity");
        if (!store.update_folder_uidvalidity_guarded(key, selected.uidvalidity, account.generation,
                folder.generation, folder.uidvalidity, error)) return fail_task(result, error.c_str());
        selected.generation = folder.generation;
        if (!store.save_folder_metadata_guarded(selected, account.generation, folder.generation, error))
            return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::FolderSelect) {
            if (!folder_result(store, account.id, result, error)) return fail_task(result, error.c_str());
            return true;
        }
        Array<MessageKey> keys;
        if (!imap.search(key, ImapSearchMode::All, keys, context, error)) return fail_task(result, error.c_str());
        for (size_t at = 0; at < keys.size(); at += MaxImapUidBatch) {
            if (cancelled(context)) return fail_task(result, "header synchronization cancelled", WorkerStatus::Cancelled);
            const size_t amount = keys.size() - at < MaxImapUidBatch ? keys.size() - at : MaxImapUidBatch;
            Array<ImapMessage> messages;
            if (!imap.fetch_headers(keys.data() + at, amount, messages, context, error))
                return fail_task(result, error.c_str());
            for (size_t i = 0; i < messages.size(); ++i) {
                ParsedMessage parsed;
                if (!parse_message(reinterpret_cast<const char *>(messages[i].raw.data()),
                        messages[i].raw.size(), parsed, error)) return fail_task(result, error.c_str());
                MessageSummary summary;
                summary.key = messages[i].key;
                summary.from = parsed.from;
                summary.subject = parsed.subject;
                summary.internal_date = messages[i].internal_date;
                summary.size = messages[i].size;
                summary.flags = messages[i].flags;
                summary.headers_only = true;
                if (!store.save_header_message_guarded(summary, messages[i].raw,
                        account.generation, folder.generation, error)) return fail_task(result, error.c_str());
            }
        }
        MessageQuerySpec query = input.page_query;
        query.scope = MessageQueryScope::Folder;
        query.account_id = account.id;
        query.folder = key;
        if (input.page_offset) query.offset = input.page_offset;
        MessageQueryPage page;
        if (!store.query_message_page(query, page, error))
            return fail_task(result, error.c_str());
        page.folder_generation = folder.generation;
        page.folder_uidvalidity = selected.uidvalidity;
        stamp_page_context(page, request);
        if (
            !worker_encode_message_page(page, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::FetchMessage) {
        MessageKey key;
        if (!request_fetch_message_key(request, input, key, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        Account account;
        Folder folder;
        if (!account_and_folder_identity(store, request, key.folder, folder, account, error,
                FolderUidGuard::Required))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        SecretBytes password;
        ImapSession imap;
        if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
            return fail_task(result, error.c_str());
        Folder selected;
        if (!imap.select(key.folder, selected, context, error)) return fail_task(result, error.c_str());
        if (selected.key != key.folder || !selected.uidvalidity)
            return fail_task(result, "IMAP SELECT did not establish the requested folder identity", WorkerStatus::InvalidRequest);
        if (selected.uidvalidity != key.uidvalidity) {
            if (!store.update_folder_uidvalidity_guarded(key.folder, selected.uidvalidity,
                    account.generation, folder.generation, folder.uidvalidity, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            return fail_task(result, "message UIDVALIDITY changed after the request was queued", WorkerStatus::InvalidRequest);
        }
        Array<ImapMessage> messages;
        if (!imap.fetch(&key, 1, messages, context, error)) return fail_task(result, error.c_str());
        const ImapMessage *fetched = nullptr;
        for (size_t i = 0; i < messages.size(); ++i)
            if (messages[i].key == key) { fetched = &messages[i]; break; }
        if (!fetched) return fail_task(result, "requested message UID is no longer present", WorkerStatus::InvalidRequest);
        if (fetched->headers_only || fetched->raw.empty() || fetched->raw.size() > MaxRawMessageBytes)
            return fail_task(result, "IMAP full FETCH returned an invalid or header-only message");
        ParsedMessage parsed;
        if (!parse_message(reinterpret_cast<const char *>(fetched->raw.data()), fetched->raw.size(), parsed, error))
            return fail_task(result, error.c_str());
        MessageSummary summary;
        summary.key = key;
        summary.from = parsed.from;
        summary.subject = parsed.subject;
        summary.internal_date = fetched->internal_date;
        summary.size = fetched->size ? fetched->size : fetched->raw.size();
        summary.flags = fetched->flags;
        summary.headers_only = false;
        WorkerReaderResult reader;
        Bytes reader_payload;
        if (!make_reader_result(key, summary, parsed, reader, error) ||
            !worker_encode_reader_result(reader, reader_payload, error))
            return fail_task(result, error.c_str());
        if (cancelled(context)) return fail_task(result, "message fetch cancelled before cache commit", WorkerStatus::Cancelled);
        const AttachmentPresence attachment_presence = parsed.attachments.empty() ?
            AttachmentPresence::None : AttachmentPresence::Some;
        if (!store.save_message_guarded(summary, fetched->raw,
                account.generation, folder.generation, attachment_presence, error))
            return fail_task(result, error.c_str());
        result.payload = static_cast<Bytes &&>(reader_payload);
        return true;
    }

    if (request.kind == WorkerTaskKind::SaveDraft || request.kind == WorkerTaskKind::SaveDraftRevision ||
        request.kind == WorkerTaskKind::SaveForwardDraft || request.kind == WorkerTaskKind::SaveDraftCopy ||
        request.kind == WorkerTaskKind::LoadDraft || request.kind == WorkerTaskKind::DeleteDraft) {
        Account account;
        if (!load_account(store, request, account, error)) return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::LoadDraft) {
            Draft draft;
            if (!store.get_draft(input.item_id, draft, error)) return fail_task(result, error.c_str());
            if (draft.account_id != account.id) {
                store.release_draft_sources(draft);
                return fail_task(result, "draft belongs to a different account", WorkerStatus::InvalidRequest);
            }
            WorkerDraftSnapshot snapshot; snapshot.draft = static_cast<Draft &&>(draft);
            snapshot.account_generation = account.generation;
            for (size_t i = 0; i < snapshot.draft.attachments.size(); ++i) {
                const Attachment &part = snapshot.draft.attachments[i];
                WorkerAttachmentMetadata meta; meta.filename = part.filename; meta.content_type = part.content_type;
                meta.content_id = part.content_id; meta.size = part.source.read ? part.source.size : part.data.size();
                meta.inline_part = part.inline_part;
                if (!snapshot.attachments.push(static_cast<WorkerAttachmentMetadata &&>(meta))) {
                    store.release_draft_sources(snapshot.draft);
                    return fail_task(result, "draft metadata exceeds its bound");
                }
            }
            store.release_draft_sources(snapshot.draft);
            if (!worker_encode_draft_snapshot(snapshot, result.payload, error)) return fail_task(result, error.c_str());
            return true;
        }
        if (request.kind == WorkerTaskKind::DeleteDraft) {
            if (!input.item_id || !input.item_revision)
                return fail_task(result, "draft deletion requires an ID and nonzero revision", WorkerStatus::InvalidRequest);
            if (!store.delete_draft_guarded(input.item_id, account.id, account.generation,
                    input.item_revision, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            return true;
        }
        if (!input.has_draft || input.draft.account_id != account.id)
            return fail_task(result, "draft save request is missing a matching draft", WorkerStatus::InvalidRequest);
        if ((request.kind == WorkerTaskKind::SaveDraft || request.kind == WorkerTaskKind::SaveForwardDraft) &&
            (input.draft.id || input.draft.revision || input.item_id || input.item_revision))
            return fail_task(result, "new draft creation cannot carry an existing ID or revision", WorkerStatus::InvalidRequest);
        if (request.kind == WorkerTaskKind::SaveDraftCopy) {
            Account source_account;
            if (!input.item_id || !input.item_revision || input.draft.id || input.draft.revision ||
                !input.account.id || !input.account.generation || input.account.id == account.id ||
                input.draft.account_id != account.id)
                return fail_task(result,
                    "draft copy requires distinct source and target accounts and a new target draft",
                    WorkerStatus::InvalidRequest);
            if (!store.get_account(input.account.id, source_account, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            if (source_account.generation != input.account.generation)
                return fail_task(result, "source account configuration changed before draft copy",
                    WorkerStatus::InvalidRequest);
            Draft source_draft;
            if (!store.get_draft(input.item_id, source_draft, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            const bool source_matches = source_draft.id == input.item_id &&
                source_draft.revision == input.item_revision && source_draft.account_id == source_account.id;
            store.release_draft_sources(source_draft);
            if (!source_matches)
                return fail_task(result, "source draft changed or no longer belongs to the selected account",
                    WorkerStatus::InvalidRequest);
            for (size_t i = 0; i < input.attachment_refs.size(); ++i) {
                const WorkerDraftAttachmentRef &ref = input.attachment_refs[i];
                if (ref.kind == WorkerAttachmentRefKind::ExistingDraftPart &&
                    (ref.source_draft_id != input.item_id || ref.source_revision != input.item_revision))
                    return fail_task(result, "draft copy attachments must come from its exact source revision",
                        WorkerStatus::InvalidRequest);
            }
        }
        if (request.kind == WorkerTaskKind::SaveDraftRevision &&
            (!input.item_id || !input.item_revision || input.draft.id != input.item_id ||
             input.draft.revision != input.item_revision))
            return fail_task(result, "draft update requires a matching existing ID and nonzero revision", WorkerStatus::InvalidRequest);
        Draft draft = static_cast<Draft &&>(input.draft);
        if (request.kind == WorkerTaskKind::SaveForwardDraft) {
            if (input.message_key.folder.account_id != account.id ||
                input.message_key.folder.server_name.empty() || !input.message_key.uidvalidity ||
                !input.message_key.uid)
                return fail_task(result, "forward-as-attachment requires a complete cached message identity", WorkerStatus::InvalidRequest);
            MessageSummary original_summary;
            Bytes original;
            if (!store.load_message(input.message_key, original_summary, original, error)) {
                store.release_draft_sources(draft);
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            }
            if (original_summary.key != input.message_key || original_summary.headers_only ||
                original.empty() || original.size() > DefaultMaxAttachmentBytes) {
                store.release_draft_sources(draft);
                return fail_task(result, "the complete original message is unavailable or exceeds the attachment limit", WorkerStatus::InvalidRequest);
            }
            Attachment forwarded;
            if (!forwarded.filename.set("forwarded-message.eml") ||
                !forwarded.content_type.set("application/octet-stream")) {
                store.release_draft_sources(draft);
                return fail_task(result, "could not prepare the original message attachment");
            }
            forwarded.data = static_cast<Bytes &&>(original);
            if (!draft.attachments.push(static_cast<Attachment &&>(forwarded))) {
                store.release_draft_sources(draft);
                return fail_task(result, "forwarded original exceeds the attachment count limit");
            }
        }
        for (size_t i = 0; i < input.attachment_refs.size(); ++i) {
            const WorkerDraftAttachmentRef &ref = input.attachment_refs[i];
            Attachment part;
            if (ref.kind == WorkerAttachmentRefKind::SelectedFile) {
                if (!read_selected_file(ref, part, error)) { store.release_draft_sources(draft); return fail_task(result, error.c_str()); }
            } else {
                Draft source_draft;
                const uint64_t expected_source_account = request.kind == WorkerTaskKind::SaveDraftCopy ?
                    input.account.id : draft.account_id;
                if (!ref.source_revision || !store.get_draft(ref.source_draft_id, source_draft, error) ||
                    source_draft.revision != ref.source_revision || source_draft.account_id != expected_source_account ||
                    ref.source_position >= source_draft.attachments.size()) {
                    store.release_draft_sources(source_draft);
                    store.release_draft_sources(draft);
                    return fail_task(result, "existing draft attachment reference is stale or invalid");
                }
                part = source_draft.attachments[ref.source_position];
                if (!ref.filename.empty()) part.filename = ref.filename;
                if (!ref.content_type.empty()) part.content_type = ref.content_type;
                if (!ref.content_id.empty()) part.content_id = ref.content_id;
                part.inline_part = ref.inline_part;
                store.release_draft_sources(source_draft);
            }
            if (!draft.attachments.push(static_cast<Attachment &&>(part))) {
                store.release_draft_sources(draft);
                return fail_task(result, "draft attachment list exceeds its bound");
            }
        }
        const uint64_t expected_revision = request.kind == WorkerTaskKind::SaveDraftCopy ? 0 : input.item_revision;
        if (!store.save_draft_guarded(draft, account.generation, expected_revision, error)) {
            store.release_draft_sources(draft);
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        }
        WorkerDraftSnapshot snapshot; snapshot.draft = static_cast<Draft &&>(draft);
        snapshot.account_generation = account.generation;
        for (size_t i = 0; i < snapshot.draft.attachments.size(); ++i) {
            const Attachment &part = snapshot.draft.attachments[i];
            WorkerAttachmentMetadata meta; meta.filename = part.filename; meta.content_type = part.content_type;
            meta.content_id = part.content_id; meta.size = part.source.read ? part.source.size : part.data.size();
            meta.inline_part = part.inline_part;
            if (!snapshot.attachments.push(static_cast<WorkerAttachmentMetadata &&>(meta))) {
                store.release_draft_sources(snapshot.draft);
                return fail_task(result, "draft metadata exceeds its bound");
            }
        }
        store.release_draft_sources(snapshot.draft);
        if (!worker_encode_draft_snapshot(snapshot, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::LocalPage || request.kind == WorkerTaskKind::LocalSearch ||
        request.kind == WorkerTaskKind::MessagePage || request.kind == WorkerTaskKind::MessageSearch) {
        Folder folder_context;
        bool has_folder_context = false;
        if (input.page_query.scope == MessageQueryScope::Folder) {
            if (!request.account_id || input.page_query.account_id != request.account_id ||
                input.page_query.folder.account_id != request.account_id ||
                input.page_query.folder.server_name != request.folder_server_name)
                return fail_task(result, "folder query identity does not match its request frame", WorkerStatus::InvalidRequest);
            Account account;
            if (!account_and_folder_identity(store, request, input.page_query.folder,
                    folder_context, account, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            has_folder_context = true;
        }
        MessageQueryPage page;
        if (!store.query_message_page(input.page_query, page, error)) return fail_task(result, error.c_str());
        if (has_folder_context) {
            page.folder_generation = folder_context.generation;
            page.folder_uidvalidity = folder_context.uidvalidity;
        }
        stamp_page_context(page, request);
        if (!worker_encode_message_page(page, result.payload, error)) return fail_task(result, error.c_str());
        if (!page.diagnostic.empty()) result.diagnostic = page.diagnostic;
        return true;
    }

    if (request.kind == WorkerTaskKind::ServerSearch) {
        if (input.page_query.scope != MessageQueryScope::Folder || input.page_query.sort == MessageQuerySort::Sender ||
            input.page_query.sort == MessageQuerySort::Subject || input.page_query.required_flags ||
            input.page_query.excluded_flags || input.page_query.has_received_after ||
            input.page_query.has_received_before ||
            input.page_query.attachment_filter != AttachmentQueryFilter::Any)
            return fail_task(result, "server search requires one folder and date order; received-date and attachment filters are local only",
                WorkerStatus::InvalidRequest);
        FolderKey key = input.page_query.folder;
        if (input.page_query.account_id != request.account_id || key.account_id != request.account_id ||
            key.server_name != request.folder_server_name)
            return fail_task(result, "server-search identity does not match its request frame", WorkerStatus::InvalidRequest);
        Folder folder; Account account;
        if (!account_and_folder_identity(store, request, key, folder, account, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        SecretBytes password;
        ImapSession imap;
        if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
            return fail_task(result, error.c_str());
        Folder selected;
        if (!imap.select(key, selected, context, error) || !selected.uidvalidity)
            return fail_task(result, error.empty() ? "IMAP SELECT returned no UIDVALIDITY" : error.c_str());
        if (!store.update_folder_uidvalidity_guarded(key, selected.uidvalidity, account.generation,
                folder.generation, folder.uidvalidity, error)) return fail_task(result, error.c_str());
        Array<MessageKey> keys;
        const Text &query_text = input.page_query.query;
        if (query_text.empty()) {
            if (!imap.search(key, ImapSearchMode::All, keys, context, error)) return fail_task(result, error.c_str());
        } else if (input.page_query.search_field == MessageQueryField::From ||
            input.page_query.search_field == MessageQueryField::Subject ||
            input.page_query.search_field == MessageQueryField::Body) {
            const ImapTextSearchField field = input.page_query.search_field == MessageQueryField::From ? ImapTextSearchField::From :
                input.page_query.search_field == MessageQueryField::Subject ? ImapTextSearchField::Subject : ImapTextSearchField::Body;
            if (!imap.search_text(key, field, query_text, keys, context, error)) return fail_task(result, error.c_str());
        } else if (input.page_query.search_field == MessageQueryField::MessageId) {
            if (!imap.search_message_id(key, query_text, keys, context, error)) return fail_task(result, error.c_str());
        } else {
            Array<MessageKey> part;
            const ImapTextSearchField fields[] = {ImapTextSearchField::From, ImapTextSearchField::Subject, ImapTextSearchField::Body};
            for (size_t field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
                if (!imap.search_text(key, fields[field], query_text, part, context, error))
                    return fail_task(result, error.c_str());
                for (size_t i = 0; i < part.size(); ++i) {
                    bool duplicate = false;
                    for (size_t j = 0; j < keys.size(); ++j) if (keys[j].uid == part[i].uid) { duplicate = true; break; }
                    if (!duplicate && !keys.push(part[i])) return fail_task(result, "server search result exceeds memory bounds");
                }
                part.reset();
            }
        }
        WorkerMessagePage page;
        page.query = input.page_query;
        page.query.folder = key;
        page.query.account_id = account.id;
        page.folder_generation = folder.generation;
        page.folder_uidvalidity = selected.uidvalidity;
        page.total_matches = keys.size();
        const uint64_t offset = page.query.offset;
        const size_t limit = page.query.limit;
        if (!limit || limit > MaxRowsPerPage) return fail_task(result, "server search page limit is invalid", WorkerStatus::InvalidRequest);
        page.has_more = offset < page.total_matches && page.total_matches - offset > limit;
        const size_t available = offset < page.total_matches ? static_cast<size_t>(page.total_matches - offset) : 0;
        const size_t count = available < limit ? available : limit;
        Array<MessageKey> requested;
        if (!requested.reserve(count)) return fail_task(result, "cannot allocate server search page");
        for (size_t i = 0; i < count; ++i) {
            const uint64_t index = page.query.sort == MessageQuerySort::Newest ?
                page.total_matches - 1 - (offset + i) : offset + i;
            if (!requested.push(keys[static_cast<size_t>(index)])) return fail_task(result, "cannot build server search page");
        }
        Array<ImapMessage> messages;
        if (requested.size() && !imap.fetch_headers(requested.data(), requested.size(), messages, context, error))
            return fail_task(result, error.c_str());
        for (size_t i = 0; i < requested.size(); ++i) {
            const ImapMessage *message = nullptr;
            for (size_t j = 0; j < messages.size(); ++j) if (messages[j].key.uid == requested[i].uid) { message = &messages[j]; break; }
            if (!message) return fail_task(result, "server search page changed during FETCH");
            ParsedMessage parsed;
            if (!parse_message(reinterpret_cast<const char *>(message->raw.data()), message->raw.size(), parsed, error))
                return fail_task(result, error.c_str());
            MessageSummary summary;
            summary.key = message->key; summary.from = parsed.from; summary.subject = parsed.subject;
            summary.internal_date = message->internal_date; summary.size = message->size;
            summary.flags = message->flags; summary.headers_only = true;
            if (!store.save_header_message_guarded(summary, message->raw,
                    account.generation, folder.generation, error)) return fail_task(result, error.c_str());
            MessageQueryRow row; row.message = static_cast<MessageSummary &&>(summary);
            row.folder_display_name = folder.display_name;
            row.profile_epoch = request.profile_epoch;
            row.ui_generation = request.ui_generation;
            row.account_generation = account.generation;
            row.folder_generation = folder.generation;
            if (!page.rows.push(static_cast<MessageQueryRow &&>(row))) return fail_task(result, "server search page exceeds its bound");
            ++page.headers_only_count;
        }
        if (!worker_encode_message_page(page, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::SetFlags || request.kind == WorkerTaskKind::MoveMessage ||
        request.kind == WorkerTaskKind::CopyMessage || request.kind == WorkerTaskKind::DeleteMessage ||
        request.kind == WorkerTaskKind::FolderOperation) {
        Account account;
        if (!load_account(store, request, account, error)) return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::FolderOperation) {
            if (input.folder_action == WorkerFolderAction::List) {
                SecretBytes password; ImapSession imap;
                if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error)) return fail_task(result, error.c_str());
                FolderList folders;
                if (!imap.list(folders, context, error) || !persist_folder_snapshot(store, account, folders, error) ||
                    !folder_result(store, account.id, result, error)) return fail_task(result, error.c_str());
                return true;
            }
            FolderKey key;
            if (input.folder_action == WorkerFolderAction::Create) {
                key.account_id = account.id;
            } else if (!request_folder_key(request, input, key)) {
                return fail_task(result, "folder operation requires a folder identity", WorkerStatus::InvalidRequest);
            }
            if (input.folder_action != WorkerFolderAction::Create) {
                Folder current;
                if (!store.get_folder(key, current, error) ||
                    !request.folder_generation || current.generation != request.folder_generation ||
                    current.uidvalidity != request.folder_uidvalidity)
                    return fail_task(result, "folder changed before its operation", WorkerStatus::InvalidRequest);
                if (!current.selectable)
                    return fail_task(result, "nonselectable hierarchy folders cannot be mutated",
                        WorkerStatus::InvalidRequest);
                if ((input.folder_action == WorkerFolderAction::Rename ||
                        input.folder_action == WorkerFolderAction::Delete) &&
                    (current.role != FolderRole::None ||
                     key.server_name == account.inbox_folder || key.server_name == account.sent_folder ||
                     key.server_name == account.drafts_folder || key.server_name == account.trash_folder ||
                     key.server_name == account.junk_folder || key.server_name == account.archive_folder))
                    return fail_task(result, "special-role folders cannot be renamed or deleted here",
                        WorkerStatus::InvalidRequest);
            }
            if ((input.folder_action == WorkerFolderAction::Create ||
                    input.folder_action == WorkerFolderAction::Rename) &&
                (input.query.empty() || !valid_utf8(input.query.c_str(), input.query.size())))
                return fail_task(result, "create or rename requires a nonempty UTF-8 display name",
                    WorkerStatus::InvalidRequest);
            if (input.folder_action == WorkerFolderAction::ClearTrash) {
                Folder trash;
                if (!store.get_folder(key, trash, error) || !trash.selectable ||
                    (key.server_name != account.trash_folder && trash.role != FolderRole::Trash) ||
                    !request.folder_uidvalidity || trash.uidvalidity != request.folder_uidvalidity)
                    return fail_task(result, "clear-trash requires the exact configured selectable Trash folder",
                        WorkerStatus::InvalidRequest);
                SecretBytes password;
                ImapSession imap;
                if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
                    return fail_task(result, error.c_str());
                Array<MessageKey> keys;
                if (!imap.search(key, ImapSearchMode::All, keys, context, error))
                    return fail_task(result, error.c_str());
                if (keys.size() > MaxIndexedMessagesPerFolder)
                    return fail_task(result, "Trash exceeds the supported bounded clear limit", WorkerStatus::InvalidRequest);
                size_t removed = 0;
                for (size_t i = 0; i < keys.size(); ++i) {
                    if (cancelled(context)) {
                        result.status = removed ? WorkerStatus::CommittedWarning : WorkerStatus::Cancelled;
                        result.diagnostic.set(removed ?
                            "Trash was only partly cleared. Refresh the folder and review its current contents before another clear." :
                            "Trash clear was cancelled before deleting a message.");
                        return true;
                    }
                    if (keys[i].folder != key || keys[i].uidvalidity != trash.uidvalidity ||
                        !keys[i].uid || !imap.delete_message(keys[i], context, error)) {
                        result.status = removed ? WorkerStatus::CommittedWarning : WorkerStatus::Uncertain;
                        if (removed)
                            result.diagnostic.set("Trash clear stopped after partial server deletion. Refresh the folder before deciding whether to continue.");
                        else
                            result.diagnostic.set(error.empty() ?
                                "Trash deletion outcome is unknown. Refresh the folder before deciding whether to retry." :
                                error.c_str());
                        return true;
                    }
                    if (!store.delete_message_guarded(keys[i], account.generation, trash.generation, error)) {
                        result.status = WorkerStatus::CommittedWarning;
                        result.diagnostic.set("The server deleted a Trash message, but its local cache could not be updated. Refresh before continuing.");
                        return true;
                    }
                    ++removed;
                }
                result.status = WorkerStatus::Ok;
                result.diagnostic.set("Trash was cleared with exact UIDPLUS-targeted deletions.");
                return true;
            }
            ImapFolderOperation operation;
            switch (input.folder_action) {
            case WorkerFolderAction::Subscribe: operation = ImapFolderOperation::Subscribe; break;
            case WorkerFolderAction::Unsubscribe: operation = ImapFolderOperation::Unsubscribe; break;
            case WorkerFolderAction::Create: operation = ImapFolderOperation::Create; break;
            case WorkerFolderAction::Rename: operation = ImapFolderOperation::Rename; break;
            case WorkerFolderAction::Delete: operation = ImapFolderOperation::Delete; break;
            case WorkerFolderAction::ClearTrash: return fail_task(result, "clear-trash dispatch is inconsistent");
            case WorkerFolderAction::List: return fail_task(result, "folder list dispatch is inconsistent");
            }
            SecretBytes password; ImapSession imap;
            if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error))
                return fail_task(result, error.c_str());
            if (!imap.folder_operation(operation, key, input.query, context, error))
                return fail_task(result, error.c_str(), WorkerStatus::Uncertain);
            FolderList folders;
            if (!imap.list(folders, context, error) || !persist_folder_snapshot(store, account, folders, error) ||
                !folder_result(store, account.id, result, error)) {
                result.status = WorkerStatus::CommittedWarning;
                result.diagnostic.set("The server accepted the folder operation, but its result could not be refreshed. Refresh before repeating it.");
                return true;
            }
            return true;
        }
        FolderKey key;
        if (!request_folder_key(request, input, key))
            return fail_task(result, "message operation requires a folder identity", WorkerStatus::InvalidRequest);
        Folder folder;
        if (!account_and_folder_identity(store, request, key, folder, account, error,
                FolderUidGuard::Required))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        MessageKey message_key = input.message_key;
        if (!request.message_uid || !message_key.uid || message_key.folder != key ||
            message_key.uidvalidity != request.folder_uidvalidity ||
            message_key.uidvalidity != folder.uidvalidity || message_key.uid != request.message_uid)
            return fail_task(result, "message UID identity is stale or inconsistent", WorkerStatus::InvalidRequest);
        Folder destination;
        FolderKey destination_key;
        if (request.kind == WorkerTaskKind::MoveMessage || request.kind == WorkerTaskKind::CopyMessage) {
            destination_key = input.destination_folder;
            if (!destination_key.account_id || destination_key.account_id != account.id ||
                destination_key.server_name.empty() || !store.get_folder(destination_key, destination, error) ||
                !destination.selectable || !input.destination_folder_generation ||
                destination.generation != input.destination_folder_generation ||
                !destination.uidvalidity || !input.destination_folder_uidvalidity ||
                destination.uidvalidity != input.destination_folder_uidvalidity)
                return fail_task(result, "destination folder identity is stale or not selectable", WorkerStatus::InvalidRequest);
        }
        SecretBytes password; ImapSession imap;
        if (!open_imap(account, secret, request.profile_epoch, context, imap, password, error)) return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::SetFlags) {
            if (!imap.store(message_key, input.flags_add, input.flags_remove, context, error))
                return fail_task(result, error.c_str(), WorkerStatus::Uncertain);
            if (!store.update_message_flags_guarded(message_key, input.flags_add, input.flags_remove,
                    account.generation, folder.generation, error)) {
                result.status = WorkerStatus::CommittedWarning;
                result.diagnostic.set("IMAP accepted the flag change, but the local cache could not be updated. Refresh before repeating it.");
            }
            return true;
        }
        if (request.kind == WorkerTaskKind::DeleteMessage) {
            if (!imap.delete_message(message_key, context, error))
                return fail_task(result, error.c_str(), WorkerStatus::Uncertain);
            if (!store.delete_message_guarded(message_key, account.generation, folder.generation, error)) {
                result.status = WorkerStatus::CommittedWarning;
                result.diagnostic.set("The server accepted the targeted deletion, but the local cache could not be updated. Refresh before repeating it.");
            }
            return true;
        }
        if (request.kind == WorkerTaskKind::CopyMessage) {
            if (!imap.copy(message_key, destination_key, context, error))
                return fail_task(result, error.c_str(), WorkerStatus::Uncertain);
            return true;
        }
        if (!imap.move(message_key, destination_key, context, error))
            return fail_task(result, error.c_str(), WorkerStatus::Uncertain);
        if (!store.delete_message_guarded(message_key, account.generation, folder.generation, error)) {
            result.status = WorkerStatus::CommittedWarning;
            result.diagnostic.set("IMAP accepted the move, but the local cache could not be updated. Refresh before repeating it.");
        }
        return true;
    }

    if (request.kind == WorkerTaskKind::ParseMessage || request.kind == WorkerTaskKind::ReadMessageChunk) {
        const MessageKey key = input.message_key;
        if (!request.account_id || !request.account_generation || request.folder_server_name.empty() ||
            !request.folder_generation || !request.folder_uidvalidity || !request.message_uid ||
            key.folder.account_id != request.account_id || key.folder.server_name != request.folder_server_name ||
            key.uidvalidity != request.folder_uidvalidity || key.uid != request.message_uid)
            return fail_task(result, "reader request requires matching complete message and folder identities", WorkerStatus::InvalidRequest);
        Account account; Folder folder;
        if (!account_and_folder_identity(store, request, key.folder, folder, account, error,
                FolderUidGuard::Required))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        MessageSummary summary; Bytes raw;
        if (!store.load_message(key, summary, raw, error)) return fail_task(result, error.c_str());
        if (request.kind == WorkerTaskKind::ReadMessageChunk) {
            WorkerChunkResult chunk; chunk.key = key; chunk.attachment_index = input.attachment_index;
            chunk.headers_only = summary.headers_only;
            if (input.attachment_index == UINT32_MAX) {
                chunk.total_size = raw.size();
                chunk.offset = input.chunk_offset;
                if (chunk.offset > chunk.total_size) return fail_task(result, "message chunk offset is out of range", WorkerStatus::InvalidRequest);
                const size_t amount = input.chunk_size < chunk.total_size - chunk.offset ? input.chunk_size :
                    static_cast<size_t>(chunk.total_size - chunk.offset);
                if (amount && !chunk.bytes.set(raw.data() + chunk.offset, amount)) return fail_task(result, "out of memory reading message chunk");
            } else {
                if (summary.headers_only) return fail_task(result, "attachments are unavailable because only headers are cached");
                ParsedMessage parsed;
                if (!parse_message(reinterpret_cast<const char *>(raw.data()), raw.size(), parsed, error) ||
                    input.attachment_index >= parsed.attachments.size()) return fail_task(result, error.empty() ? "attachment index is invalid" : error.c_str());
                const Bytes &bytes = parsed.attachments[input.attachment_index].data;
                chunk.total_size = bytes.size(); chunk.offset = input.chunk_offset;
                if (chunk.offset > chunk.total_size) return fail_task(result, "attachment chunk offset is out of range", WorkerStatus::InvalidRequest);
                const size_t amount = input.chunk_size < chunk.total_size - chunk.offset ? input.chunk_size :
                    static_cast<size_t>(chunk.total_size - chunk.offset);
                if (amount && !chunk.bytes.set(bytes.data() + chunk.offset, amount)) return fail_task(result, "out of memory reading attachment chunk");
            }
            if (!worker_encode_chunk_result(chunk, result.payload, error)) return fail_task(result, error.c_str());
            return true;
        }
        ParsedMessage parsed;
        if (!parse_message(reinterpret_cast<const char *>(raw.data()), raw.size(), parsed, error)) return fail_task(result, error.c_str());
        if (!summary.headers_only) {
            const AttachmentPresence presence = parsed.attachments.empty() ?
                AttachmentPresence::None : AttachmentPresence::Some;
            if (!store.set_message_attachment_presence_guarded(key, presence,
                    account.generation, folder.generation, error)) return fail_task(result, error.c_str());
        }
        WorkerReaderResult reader;
        if (!make_reader_result(key, summary, parsed, reader, error) ||
            !worker_encode_reader_result(reader, result.payload, error)) return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::QueueSend) {
        Account account;
        if (!load_account(store, request, account, error)) return fail_task(result, error.c_str());
        if (!input.item_id || !input.item_revision)
            return fail_task(result, "queue intent requires a saved draft revision", WorkerStatus::InvalidRequest);
        SendOperation operation;
        Bytes prior_spool;
        WipeBytesGuard prior_wiper{&prior_spool};
        bool existing = false;
        if (!store.find_send_operation_for_draft(account.id, input.item_id, input.item_revision,
                existing, operation, prior_spool, error)) return fail_task(result, error.c_str());
        if (existing) {
            if (operation.account_generation != account.generation)
                return fail_task(result, "queued send belongs to a previous account configuration", WorkerStatus::InvalidRequest);
            if (!finish_operation_result(operation, Text(), WorkerStatus::Ok, result, error))
                return fail_task(result, error.c_str());
            return true;
        }
        Draft draft;
        if (!store.get_draft(input.item_id, draft, error)) return fail_task(result, error.c_str());
        if (draft.account_id != account.id || draft.revision != input.item_revision) {
            store.release_draft_sources(draft);
            return fail_task(result, "draft account or revision changed before queueing", WorkerStatus::InvalidRequest);
        }
        Bytes spool;
        WipeBytesGuard spool_wiper{&spool};
        Envelope envelope;
        if (!encode_message(draft, account, spool, envelope, error)) {
            store.release_draft_sources(draft);
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        }
        store.release_draft_sources(draft);
        Text message_id, date, from, sender, recipients;
        if (!extract_finalized_identity(spool, message_id, date, from, error) ||
            !serialize_mailboxes(envelope, sender, recipients, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        operation.account_id = account.id;
        operation.account_generation = account.generation;
        operation.draft_id = input.item_id;
        operation.draft_revision = input.item_revision;
        operation.message_id = message_id;
        operation.sender = sender;
        operation.recipients = recipients;
        operation.spool_size = spool.size();
        operation.archive_folder = account.sent_folder;
        Envelope verified_envelope;
        if (!validate_operation_spool(operation, spool, verified_envelope, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        if (!store.create_send_operation(account.id, account.generation, input.item_id, input.item_revision,
                message_id, sender, recipients, account.sent_folder, spool, operation, error)) {
            bool now_exists = false;
            Text lookup_error;
            if (store.find_send_operation_for_draft(account.id, input.item_id, input.item_revision,
                    now_exists, operation, prior_spool, lookup_error) && now_exists) {
                if (operation.account_generation != account.generation)
                    return fail_task(result, "queued send belongs to a previous account configuration", WorkerStatus::InvalidRequest);
            } else return fail_task(result, error.c_str());
        }
#ifdef RELIEFMAIL_TESTING
        if (!test_pause_after_durable_commit(input.test_pause_after_durable_commit,
                input.test_commit_marker_path))
            return fail_task(result, "test wait after durable QueueSend commit expired");
#endif
        if (!finish_operation_result(operation, Text(), WorkerStatus::Ok, result, error))
            return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::SendQueued || request.kind == WorkerTaskKind::RetryArchive ||
        request.kind == WorkerTaskKind::CancelQueuedSend) {
        Account account;
        if (!load_account(store, request, account, error)) return fail_task(result, error.c_str());
        if (!input.item_id) return fail_task(result, "send operation ID is required", WorkerStatus::InvalidRequest);
        SendOperation operation;
        Bytes spool;
        WipeBytesGuard spool_wiper{&spool};
        if (!store.get_send_operation(input.item_id, operation, spool, error)) return fail_task(result, error.c_str());
        if (operation.account_id != account.id)
            return fail_task(result, "send operation belongs to another account", WorkerStatus::InvalidRequest);
        const bool stale_ready_local_cancel = request.kind == WorkerTaskKind::CancelQueuedSend &&
            operation.stage == SendStage::Ready;
        if (request.kind != WorkerTaskKind::RetryArchive && !stale_ready_local_cancel &&
            operation.account_generation != account.generation)
            return fail_task(result, "send operation account configuration changed", WorkerStatus::InvalidRequest);
        if (request.kind == WorkerTaskKind::CancelQueuedSend) {
            if (operation.stage == SendStage::Ready) {
                if (!store.advance_send_operation(operation.id, operation.account_generation,
                        SendStage::Failed, false, error)) return fail_task(result, error.c_str());
                operation.stage = SendStage::Failed;
                Text diagnostic; diagnostic.set("queued send cancelled before SMTP submission");
                if (!finish_operation_result(operation, diagnostic, WorkerStatus::Ok, result, error))
                    return fail_task(result, error.c_str());
                return true;
            }
            const WorkerStatus state = operation.stage == SendStage::Uncertain ? WorkerStatus::Uncertain :
                operation.delivery_count ? WorkerStatus::CommittedWarning : WorkerStatus::Failed;
            Text diagnostic;
            diagnostic.set(operation.stage == SendStage::Uncertain ?
                "SMTP delivery is uncertain; cancellation cannot establish whether it was delivered" :
                operation.delivery_count ? "SMTP delivery is already confirmed and cannot be cancelled" :
                "send operation is no longer cancellable");
            if (!finish_operation_result(operation, diagnostic, state, result, error)) return fail_task(result, error.c_str());
            return true;
        }

        Envelope envelope;
        if (!validate_operation_spool(operation, spool, envelope, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        if (request.kind == WorkerTaskKind::RetryArchive) {
            if (operation.stage == SendStage::Archived) {
                if (!finish_operation_result(operation, Text(), WorkerStatus::Ok, result, error)) return fail_task(result, error.c_str());
                return true;
            }
            const uint64_t archive_generation = operation.archive_review_generation ?
                operation.archive_review_generation : operation.account_generation;
            if (account.generation != archive_generation) {
                Text diagnostic;
                diagnostic.set("SMTP delivery is already recorded. Review and confirm the archive destination for the current account settings; this action will not contact SMTP.");
                (void)finish_operation_result(operation, diagnostic, WorkerStatus::CommittedWarning, result, error);
                return error.empty();
            }
            if (!send_operation_delivery_state_valid(operation.stage, operation.delivery_count,
                    operation.archive_review_generation)) {
                const WorkerStatus state = operation.stage == SendStage::Uncertain ? WorkerStatus::Uncertain : WorkerStatus::Failed;
                Text diagnostic; diagnostic.set("archive retry is unavailable until delivery is confirmed or explicitly reviewed");
                if (!finish_operation_result(operation, diagnostic, state, result, error)) return fail_task(result, error.c_str());
                return true;
            }
            Text diagnostic;
            const bool archived = archive_operation(store, account, operation, spool, secret,
                request.profile_epoch, context, diagnostic);
            if (!archived && diagnostic.empty()) diagnostic.set("Sent-folder archive retry failed");
            if (!archived) {
                Text persist_error;
                (void)store.set_send_operation_diagnostic(operation.id, archive_generation,
                    diagnostic, persist_error);
            }
            if (!finish_operation_result(operation, diagnostic,
                    archived ? WorkerStatus::Ok : WorkerStatus::CommittedWarning, result, error))
                return fail_task(result, error.c_str());
            return true;
        }

        if (operation.stage != SendStage::Ready) {
            const WorkerStatus state = operation.stage == SendStage::Uncertain ? WorkerStatus::Uncertain :
                operation.delivery_count ? WorkerStatus::CommittedWarning : WorkerStatus::Failed;
            Text diagnostic;
            diagnostic.set(operation.stage == SendStage::Uncertain ?
                "send remains uncertain and will never be automatically resubmitted" :
                operation.delivery_count ? "SMTP delivery is already recorded; use archive-only retry" :
                "send operation is not in the ready stage");
            if (!finish_operation_result(operation, diagnostic, state, result, error)) return fail_task(result, error.c_str());
            return true;
        }
        SecretBytes password;
        if (!credential_for(secret, account, CredentialPurpose::Smtp,
                request.profile_epoch, password, error)) {
            if (!finish_operation_result(operation, error, WorkerStatus::Failed, result, error)) return fail_task(result, error.c_str());
            return true;
        }
        if (!store.advance_send_operation(operation.id, operation.account_generation,
                SendStage::Connecting, false, error)) return fail_task(result, error.c_str());
        operation.stage = SendStage::Connecting;
        SmtpSession smtp;
        if (!smtp.connect(account, context, error)) {
            Text stage_error;
            if (store.advance_send_operation(operation.id, operation.account_generation, SendStage::Ready, false, stage_error))
                operation.stage = SendStage::Ready;
            if (operation.stage == SendStage::Ready) {
                if (!finish_operation_result(operation, error, WorkerStatus::Failed, result, stage_error))
                    return fail_task(result, stage_error.c_str());
                return true;
            }
            return fail_task(result, stage_error.c_str());
        }
        if (!store.advance_send_operation(operation.id, operation.account_generation,
                SendStage::Authenticating, false, error)) return fail_task(result, error.c_str());
        operation.stage = SendStage::Authenticating;
        if (!smtp.authenticate(SecretView{password.data(), password.size()}, context, error)) {
            Text stage_error;
            if (store.advance_send_operation(operation.id, operation.account_generation, SendStage::Ready, false, stage_error))
                operation.stage = SendStage::Ready;
            if (operation.stage == SendStage::Ready) {
                if (!finish_operation_result(operation, error, WorkerStatus::Failed, result, stage_error))
                    return fail_task(result, stage_error.c_str());
                return true;
            }
            return fail_task(result, stage_error.c_str());
        }
        ByteMessageSource bytes = {&spool};
        MessageSource source = {&bytes, byte_message_read, spool.size()};
        Text stage_error;
        SendStageContext stage_context = {&store, operation.id, operation.account_generation,
            SendStage::Authenticating, &stage_error};
        SendObserver observer = {&stage_context, persist_smtp_stage};
        const SubmitOutcome outcome = smtp.submit(envelope, source, observer, context, error);
        operation.stage = stage_context.current_stage;
        if (operation.stage == SendStage::Accepted) operation.delivery_count = 1;
        if (outcome == SubmitOutcome::Uncertain) {
            if (operation.stage != SendStage::Uncertain) {
                Text persist_error;
                if (store.advance_send_operation(operation.id, operation.account_generation,
                        SendStage::Uncertain, false, persist_error)) operation.stage = SendStage::Uncertain;
                else stage_error = persist_error;
            }
            if (!finish_operation_result(operation, error, WorkerStatus::Uncertain, result, stage_error))
                return fail_task(result, stage_error.c_str());
            return true;
        }
        if (outcome == SubmitOutcome::Rejected) {
            const SendStage settled = stage_context.current_stage == SendStage::AwaitingFinalResponse ?
                SendStage::Failed : SendStage::Ready;
            Text persist_error;
            if (store.advance_send_operation(operation.id, operation.account_generation, settled, false, persist_error))
                operation.stage = settled;
            else stage_error = persist_error;
            if (!finish_operation_result(operation, error, WorkerStatus::Failed, result, stage_error))
                return fail_task(result, stage_error.c_str());
            return true;
        }
        if (stage_context.current_stage != SendStage::Accepted) {
            Text diagnostic; diagnostic.set("SMTP accepted the message but delivery confirmation could not be persisted");
            operation.stage = stage_context.current_stage;
            if (!finish_operation_result(operation, diagnostic, WorkerStatus::Uncertain, result, stage_error))
                return fail_task(result, stage_error.c_str());
            return true;
        }
        if (!store.advance_send_operation(operation.id, operation.account_generation,
                SendStage::ArchivePending, false, error)) {
            operation.stage = SendStage::Accepted;
            Text diagnostic; diagnostic.set("SMTP delivery is confirmed; durable Sent-folder archive scheduling needs recovery");
            if (!finish_operation_result(operation, diagnostic, WorkerStatus::CommittedWarning, result, error))
                return fail_task(result, error.c_str());
            return true;
        }
        operation.stage = SendStage::ArchivePending;
        Text archive_diagnostic;
        const bool archived = archive_operation(store, account, operation, spool, secret,
            request.profile_epoch, context, archive_diagnostic);
        if (!archived && archive_diagnostic.empty()) archive_diagnostic.set("SMTP delivery is confirmed; Sent-folder archive is pending");
        if (!finish_operation_result(operation, archive_diagnostic,
                archived ? WorkerStatus::Ok : WorkerStatus::CommittedWarning, result, error))
            return fail_task(result, error.c_str());
        return true;
    }

    if (request.kind == WorkerTaskKind::ClearCache) {
        if (!store.clear_cache(error)) return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::BackupProfile) {
        if (input.path.empty() || !store.backup_profile(input.path.c_str(), error)) return fail_task(result, error.c_str());
        return true;
    }
    if (request.kind == WorkerTaskKind::RestoreProfile) {
        SecretBytes backup_master;
        if (!secret.empty()) {
            SecretBytes ignored;
            if (!worker_decode_secret(static_cast<SecretBytes &&>(secret), WorkerSecretPurpose::BackupMaster,
                    backup_master, ignored, error)) return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
        }
        const RestoreResult restored = store.restore_profile_with_status(input.path.c_str(),
            static_cast<SecretBytes &&>(backup_master), error);
#ifdef RELIEFMAIL_TESTING
        if (restored != RestoreResult::Failed &&
            !test_pause_after_durable_commit(input.test_pause_after_durable_commit,
                input.test_commit_marker_path))
            return fail_task(result, "test wait after durable RestoreProfile commit expired");
#endif
        if (restored == RestoreResult::Restored) return true;
        if (restored == RestoreResult::RestoredCleanupPending) {
            result.status = WorkerStatus::CommittedWarning;
            result.diagnostic = error;
            return true;
        }
        if (restored == RestoreResult::RecoveryRequired)
            return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        return fail_task(result, error.c_str());
    }

    if (request.kind == WorkerTaskKind::UnlockVault || request.kind == WorkerTaskKind::SaveVault ||
        request.kind == WorkerTaskKind::ChangeVaultPassword) {
        Vault vault;
        if (!vault.open(request.profile_paths, error))
            return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        SecretBytes first, second;
        const WorkerSecretPurpose purpose = request.kind == WorkerTaskKind::UnlockVault ? WorkerSecretPurpose::VaultMaster :
            request.kind == WorkerTaskKind::SaveVault ? WorkerSecretPurpose::VaultSave : WorkerSecretPurpose::VaultPasswordChange;
        if (!worker_decode_secret(static_cast<SecretBytes &&>(secret), purpose, first, second, error))
            return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
#ifdef RELIEFMAIL_TESTING
        if (request.kind == WorkerTaskKind::ChangeVaultPassword && input.test_vault_fault)
            vault_test_fail_next(static_cast<VaultFaultPoint>(input.test_vault_fault));
#endif
        if (request.kind == WorkerTaskKind::UnlockVault) {
            SecretBytes remembered;
            if (!vault.unlock(static_cast<SecretBytes &&>(first), remembered, error)) return fail_task(result, error.c_str());
            Store account_store;
            AccountList current_accounts;
            if (!account_store.open(request.profile_paths, StoreOpenMode::Worker, error) ||
                !account_store.list_accounts(current_accounts, error))
                return fail_task(result, error.empty() ? "cannot read current accounts while unlocking the vault" : error.c_str(),
                    WorkerStatus::RecoveryRequired);
            account_store.close();
            SecretBytes rebound;
            if (!rebind_unlocked_vault_credentials(remembered, current_accounts, rebound, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            SecretBytes live;
            if (!bind_remembered_credentials(static_cast<SecretBytes &&>(rebound),
                    request.profile_epoch, live, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            result.secret_payload = static_cast<SecretBytes &&>(live);
            return true;
        }
        if (request.kind == WorkerTaskKind::SaveVault) {
            SecretBytes remembered;
            if (!remembered_credentials_for_epoch(second, request.profile_epoch, remembered, error))
                return fail_task(result, error.c_str(), WorkerStatus::InvalidRequest);
            const VaultSaveResult saved = vault.save_with_status(static_cast<SecretBytes &&>(first),
                static_cast<SecretBytes &&>(remembered), error);
            if (saved == VaultSaveResult::Committed) return true;
            if (saved == VaultSaveResult::CommittedCleanupPending) {
                result.status = WorkerStatus::CommittedWarning;
                result.diagnostic = error;
                return true;
            }
            if (saved == VaultSaveResult::RecoveryRequired)
                return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
            return fail_task(result, error.c_str());
        }
        const VaultPasswordChangeResult changed = vault.change_password_with_status(
            static_cast<SecretBytes &&>(first), static_cast<SecretBytes &&>(second), error);
        if (changed == VaultPasswordChangeResult::Committed) return true;
        if (changed == VaultPasswordChangeResult::CommittedCleanupPending) {
            result.status = WorkerStatus::CommittedWarning;
            result.diagnostic = error;
            return true;
        }
        if (changed == VaultPasswordChangeResult::RecoveryRequired)
            return fail_task(result, error.c_str(), WorkerStatus::RecoveryRequired);
        return fail_task(result, error.c_str());
    }

    return fail_task(result, "worker task kind has no backend implementation", WorkerStatus::InvalidRequest);
}

} // namespace rmail
