#include "document.h"

#include <string.h>

namespace rm {
namespace {

constexpr uint8_t kMagic[8] = {'R', 'M', 'A', 'T', 'H', '\r', '\n', 0};
constexpr size_t kHeaderBytes = 32;
constexpr size_t kRecordHeaderBytes = 32;

void fail(Error &error, Code code, const char *message) noexcept {
    error.set(code, message);
}

uint16_t load_u16(const uint8_t *bytes) noexcept {
    return static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t load_u32(const uint8_t *bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t load_u64(const uint8_t *bytes) noexcept {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    return value;
}

void store_u16(uint8_t *bytes, uint16_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void store_u32(uint8_t *bytes, uint32_t value) noexcept {
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
}

void store_u64(uint8_t *bytes, uint64_t value) noexcept {
    for (unsigned index = 0; index < 8; ++index)
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
}

bool set_decoded_text(Text &text, const uint8_t *bytes, size_t size,
                      Error &error, const char *field) noexcept {
    if (!detail::valid_utf8(reinterpret_cast<const char *>(bytes), size)) {
        fail(error, Code::InvalidUtf8, field);
        return false;
    }
    if (size && !text.set(reinterpret_cast<const char *>(bytes), size)) {
        fail(error, Code::NoMemory, "document text allocation failed");
        return false;
    }
    if (!size) text.clear();
    return true;
}

} // namespace

bool encode_document(const Document &document, Bytes &output, Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (document.formulas.size() > UINT32_MAX || document.title.size() > UINT32_MAX) {
        fail(error, Code::Limit, "document fields exceed the .rmath v1 range");
        return false;
    }

    size_t total = kHeaderBytes;
    if (document.title.size() > kMaxDocumentBytes - total) {
        fail(error, Code::Limit, "document exceeds the 16 MiB limit");
        return false;
    }
    total += document.title.size();
    for (size_t index = 0; index < document.formulas.size(); ++index) {
        const Formula &formula = document.formulas[index];
        if (formula.name.size() > UINT32_MAX || formula.source.size() > UINT32_MAX ||
            total > kMaxDocumentBytes - kRecordHeaderBytes ||
            formula.name.size() > kMaxDocumentBytes - total - kRecordHeaderBytes ||
            formula.source.size() > kMaxDocumentBytes - total - kRecordHeaderBytes -
                formula.name.size()) {
            fail(error, Code::Limit, "document exceeds the 16 MiB limit");
            return false;
        }
        total += kRecordHeaderBytes + formula.name.size() + formula.source.size();
    }
    if (total > UINT32_MAX) {
        fail(error, Code::Limit, "document exceeds the .rmath v1 length range");
        return false;
    }

    Bytes candidate;
    if (!candidate.resize(total)) {
        fail(error, Code::NoMemory, "document encoding allocation failed");
        return false;
    }
    uint8_t *bytes = candidate.data();
    memcpy(bytes, kMagic, sizeof(kMagic));
    store_u32(bytes + 8, 1);
    store_u32(bytes + 12, static_cast<uint32_t>(total));
    store_u32(bytes + 16, static_cast<uint32_t>(document.formulas.size()));
    store_u32(bytes + 20, static_cast<uint32_t>(document.title.size()));
    store_u64(bytes + 24, document.next_id);

    size_t at = kHeaderBytes;
    if (document.title.size()) {
        memcpy(bytes + at, document.title.c_str(), document.title.size());
        at += document.title.size();
    }
    for (size_t index = 0; index < document.formulas.size(); ++index) {
        const Formula &formula = document.formulas[index];
        store_u64(bytes + at, formula.id);
        store_u32(bytes + at + 8, static_cast<uint32_t>(formula.name.size()));
        store_u32(bytes + at + 12, static_cast<uint32_t>(formula.source.size()));
        store_u32(bytes + at + 16, formula.style.rgba);
        store_u16(bytes + at + 20, formula.style.point_size);
        store_u16(bytes + at + 22, formula.style.padding);
        bytes[at + 24] = formula.style.display ? 1 : 0;
        memset(bytes + at + 25, 0, 7);
        at += kRecordHeaderBytes;
        memcpy(bytes + at, formula.name.c_str(), formula.name.size());
        at += formula.name.size();
        memcpy(bytes + at, formula.source.c_str(), formula.source.size());
        at += formula.source.size();
    }

    output.swap(candidate);
    error.clear();
    return true;
}

bool decode_document(const uint8_t *bytes, size_t size, Document &output,
                     Error &error) noexcept {
    if (!bytes && size) {
        fail(error, Code::Format, "document input is null");
        return false;
    }
    if (size > kMaxDocumentBytes) {
        fail(error, Code::Limit, "document exceeds the 16 MiB limit");
        return false;
    }
    if (size < kHeaderBytes) {
        fail(error, Code::Format, "document header is truncated");
        return false;
    }
    if (memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
        fail(error, Code::Format, "document magic is invalid");
        return false;
    }
    const uint32_t version = load_u32(bytes + 8);
    if (version != 1) {
        fail(error, Code::Version, "unsupported .rmath document version");
        return false;
    }
    if (load_u32(bytes + 12) != size) {
        fail(error, Code::Format, "document length does not match its header");
        return false;
    }
    const uint32_t count = load_u32(bytes + 16);
    const uint32_t title_size = load_u32(bytes + 20);
    if (count > kMaxFormulaCount) {
        fail(error, Code::Limit, "document formula count exceeds the limit");
        return false;
    }
    if (title_size > kMaxTitleBytes) {
        fail(error, Code::Limit, "document title exceeds the limit");
        return false;
    }
    if (title_size > size - kHeaderBytes) {
        fail(error, Code::Format, "document title is truncated");
        return false;
    }

    Document candidate;
    candidate.next_id = load_u64(bytes + 24);
    size_t at = kHeaderBytes;
    if (!set_decoded_text(candidate.title, bytes + at, title_size, error,
                          "document title is not valid UTF-8")) return false;
    at += title_size;
    if (!candidate.formulas.reserve(count)) {
        fail(error, Code::NoMemory, "document formula allocation failed");
        return false;
    }

    for (uint32_t index = 0; index < count; ++index) {
        if (size - at < kRecordHeaderBytes) {
            fail(error, Code::Format, "document formula record is truncated");
            return false;
        }
        const uint8_t *record = bytes + at;
        const uint32_t name_size = load_u32(record + 8);
        const uint32_t source_size = load_u32(record + 12);
        if (name_size > kMaxNameBytes || source_size > kMaxSourceBytes) {
            fail(error, Code::Limit, "formula text exceeds the field limit");
            return false;
        }
        if (!name_size) {
            fail(error, Code::Format, "formula name is empty");
            return false;
        }
        if (record[24] > 1) {
            fail(error, Code::Format, "formula display flag is invalid");
            return false;
        }
        for (size_t reserved = 25; reserved < kRecordHeaderBytes; ++reserved) {
            if (record[reserved]) {
                fail(error, Code::Format, "formula reserved bytes must be zero");
                return false;
            }
        }
        at += kRecordHeaderBytes;
        if (name_size > size - at || source_size > size - at - name_size) {
            fail(error, Code::Format, "formula text is truncated");
            return false;
        }

        Formula formula;
        formula.id = load_u64(record);
        formula.style.rgba = load_u32(record + 16);
        formula.style.point_size = load_u16(record + 20);
        formula.style.padding = load_u16(record + 22);
        formula.style.display = record[24] != 0;
        formula.generation = 1;
        if (!set_decoded_text(formula.name, bytes + at, name_size, error,
                              "formula name is not valid UTF-8")) return false;
        at += name_size;
        if (!set_decoded_text(formula.source, bytes + at, source_size, error,
                              "formula source is not valid UTF-8")) return false;
        at += source_size;
        if (!candidate.formulas.push(static_cast<Formula &&>(formula))) {
            fail(error, Code::NoMemory, "document formula allocation failed");
            return false;
        }
    }
    if (at != size) {
        fail(error, Code::Format, "document contains trailing bytes");
        return false;
    }
    if (!validate_document(candidate, error)) return false;

    output.swap(candidate);
    error.clear();
    return true;
}

} // namespace rm
