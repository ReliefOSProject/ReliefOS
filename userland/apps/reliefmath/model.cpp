#include "model.h"

#include <string.h>

namespace rm {
namespace {

void fail(Error &error, Code code, const char *message,
          size_t begin = 0, size_t end = 0) noexcept {
    error.set(code, message, begin, end);
}

bool style_equal(const Style &left, const Style &right) noexcept {
    return left.rgba == right.rgba && left.point_size == right.point_size &&
           left.padding == right.padding && left.display == right.display;
}

bool valid_style(const Style &style) noexcept {
    const uint8_t alpha = static_cast<uint8_t>(style.rgba & 0xffu);
    return alpha != 0 && style.point_size >= kMinPointSize &&
           style.point_size <= kMaxPointSize && style.padding <= kMaxPadding;
}

bool measure_input(const char *value, size_t maximum, bool allow_empty,
                   size_t &length, Error &error, const char *field) noexcept {
    if (!value) {
        fail(error, Code::Format, field);
        return false;
    }
    length = 0;
    while (length <= maximum && value[length]) ++length;
    if (length > maximum) {
        fail(error, Code::Limit, field, maximum, maximum + 1);
        return false;
    }
    if (!allow_empty && length == 0) {
        fail(error, Code::Format, field);
        return false;
    }
    size_t bad_offset = 0;
    if (!detail::valid_utf8(value, length, &bad_offset)) {
        fail(error, Code::InvalidUtf8, field, bad_offset, bad_offset + 1);
        return false;
    }
    return true;
}

bool set_text(Text &target, const char *value, size_t length,
              Error &error, const char *field) noexcept {
    Text candidate;
    if (!candidate.set(value, length)) {
        fail(error, Code::NoMemory, field);
        return false;
    }
    target.swap(candidate);
    return true;
}

bool find_formula(const Document &document, FormulaId id, size_t &index) noexcept {
    for (size_t at = 0; at < document.formulas.size(); ++at) {
        if (document.formulas[at].id == id) {
            index = at;
            return true;
        }
    }
    return false;
}

bool can_advance_revision(const Document &document, Error &error) noexcept {
    if (document.revision == UINT64_MAX) {
        fail(error, Code::Limit, "document revision exhausted");
        return false;
    }
    return true;
}

bool can_advance_generation(const Formula &formula, Error &error) noexcept {
    if (formula.generation == UINT64_MAX) {
        fail(error, Code::Limit, "formula generation exhausted");
        return false;
    }
    return true;
}

void advance_revision(Document &document) noexcept { ++document.revision; }

bool text_equals(const Text &left, const char *right, size_t length) noexcept {
    return left.size() == length && memcmp(left.c_str(), right, length) == 0;
}

} // namespace

bool validate_document(const Document &document, Error &error) noexcept {
    if (document.formulas.size() > kMaxFormulaCount) {
        fail(error, Code::Limit, "too many formulas");
        return false;
    }
    if (document.title.size() > kMaxTitleBytes ||
        !detail::valid_utf8(document.title.c_str(), document.title.size())) {
        fail(error, Code::Format, "invalid document title");
        return false;
    }

    FormulaId maximum_id = 0;
    for (size_t at = 0; at < document.formulas.size(); ++at) {
        const Formula &formula = document.formulas[at];
        if (!formula.id || !formula.generation) {
            fail(error, Code::Format, "formula ID and generation must be nonzero");
            return false;
        }
        if (formula.name.empty() || formula.name.size() > kMaxNameBytes ||
            !detail::valid_utf8(formula.name.c_str(), formula.name.size())) {
            fail(error, Code::Format, "invalid formula name");
            return false;
        }
        if (formula.source.size() > kMaxSourceBytes ||
            !detail::valid_utf8(formula.source.c_str(), formula.source.size())) {
            fail(error, Code::Format, "invalid formula source");
            return false;
        }
        if (!valid_style(formula.style)) {
            fail(error, Code::Format, "invalid formula style");
            return false;
        }
        if (formula.id > maximum_id) maximum_id = formula.id;
        for (size_t previous = 0; previous < at; ++previous) {
            if (document.formulas[previous].id == formula.id) {
                fail(error, Code::Format, "duplicate formula ID");
                return false;
            }
        }
    }
    if (document.next_id && document.next_id <= maximum_id) {
        fail(error, Code::Format, "next formula ID is not monotonic");
        return false;
    }
    error.clear();
    return true;
}

bool clone_document(const Document &source, Document &destination, Error &error) noexcept {
    if (!validate_document(source, error)) return false;
    if (&source == &destination) {
        error.clear();
        return true;
    }
    Document candidate;
    candidate.title = source.title;
    if (!candidate.formulas.clone_from(source.formulas)) {
        fail(error, Code::NoMemory, "document snapshot allocation failed");
        return false;
    }
    candidate.next_id = source.next_id;
    candidate.revision = source.revision;
    destination.swap(candidate);
    error.clear();
    return true;
}

bool add_formula(Document &document, const char *name, const char *source,
                 FormulaId &out_id, Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (document.formulas.size() >= kMaxFormulaCount) {
        fail(error, Code::Limit, "formula limit reached");
        return false;
    }
    if (!document.next_id) {
        fail(error, Code::Limit, "formula ID space exhausted");
        return false;
    }
    if (!can_advance_revision(document, error)) return false;
    size_t name_size = 0;
    size_t source_size = 0;
    if (!measure_input(name, kMaxNameBytes, false, name_size, error,
                       "invalid formula name")) return false;
    if (!measure_input(source, kMaxSourceBytes, true, source_size, error,
                       "invalid formula source")) return false;

    Document candidate;
    if (!clone_document(document, candidate, error)) return false;
    Formula formula;
    formula.id = document.next_id;
    formula.generation = 1;
    if (!set_text(formula.name, name, name_size, error, "formula name allocation failed"))
        return false;
    if (!set_text(formula.source, source, source_size, error,
                  "formula source allocation failed")) return false;
    if (!candidate.formulas.push(static_cast<Formula &&>(formula))) {
        fail(error, Code::NoMemory, "formula array allocation failed");
        return false;
    }
    candidate.next_id = document.next_id == UINT64_MAX ? 0 : document.next_id + 1;
    candidate.revision = document.revision + 1;
    const FormulaId assigned_id = document.next_id;
    document.swap(candidate);
    out_id = assigned_id;
    error.clear();
    return true;
}

bool copy_formula(Document &document, FormulaId source_id,
                  FormulaId &out_id, Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (document.formulas.size() >= kMaxFormulaCount) {
        fail(error, Code::Limit, "formula limit reached");
        return false;
    }
    if (!document.next_id) {
        fail(error, Code::Limit, "formula ID space exhausted");
        return false;
    }
    if (!can_advance_revision(document, error)) return false;
    size_t source_index = 0;
    if (!find_formula(document, source_id, source_index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    Formula copied = document.formulas[source_index];
    copied.id = document.next_id;
    copied.generation = 1;

    Document candidate;
    if (!clone_document(document, candidate, error)) return false;
    if (!candidate.formulas.push(static_cast<Formula &&>(copied))) {
        fail(error, Code::NoMemory, "formula array allocation failed");
        return false;
    }
    candidate.next_id = document.next_id == UINT64_MAX ? 0 : document.next_id + 1;
    candidate.revision = document.revision + 1;
    const FormulaId assigned_id = document.next_id;
    document.swap(candidate);
    out_id = assigned_id;
    error.clear();
    return true;
}

bool delete_formula(Document &document, FormulaId id, Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    size_t index = 0;
    if (!find_formula(document, id, index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    if (!can_advance_revision(document, error)) return false;
    document.formulas.erase(index);
    advance_revision(document);
    error.clear();
    return true;
}

bool rename_formula(Document &document, FormulaId id, const char *name,
                    Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    size_t index = 0;
    if (!find_formula(document, id, index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    size_t name_size = 0;
    if (!measure_input(name, kMaxNameBytes, false, name_size, error,
                       "invalid formula name")) return false;
    Formula &formula = document.formulas[index];
    if (text_equals(formula.name, name, name_size)) {
        error.clear();
        return true;
    }
    if (!can_advance_revision(document, error)) return false;
    Text next_name;
    if (!set_text(next_name, name, name_size, error, "formula name allocation failed"))
        return false;
    formula.name.swap(next_name);
    advance_revision(document);
    error.clear();
    return true;
}

bool move_formula(Document &document, FormulaId id, size_t index,
                  Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (index >= document.formulas.size()) {
        fail(error, Code::Limit, "formula index out of range");
        return false;
    }
    size_t source_index = 0;
    if (!find_formula(document, id, source_index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    if (source_index == index) {
        error.clear();
        return true;
    }
    if (!can_advance_revision(document, error)) return false;
    Formula saved(static_cast<Formula &&>(document.formulas[source_index]));
    if (source_index < index) {
        for (size_t at = source_index; at < index; ++at)
            document.formulas[at] = static_cast<Formula &&>(document.formulas[at + 1]);
    } else {
        for (size_t at = source_index; at > index; --at)
            document.formulas[at] = static_cast<Formula &&>(document.formulas[at - 1]);
    }
    document.formulas[index] = static_cast<Formula &&>(saved);
    advance_revision(document);
    error.clear();
    return true;
}

bool sort_formulas(Document &document, const FormulaId *ordered_ids,
                   size_t count, Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (count != document.formulas.size() || (count && !ordered_ids)) {
        fail(error, Code::Format, "formula ordering must contain every ID");
        return false;
    }
    bool unchanged = true;
    for (size_t at = 0; at < count; ++at) {
        if (ordered_ids[at] != document.formulas[at].id) unchanged = false;
        for (size_t previous = 0; previous < at; ++previous) {
            if (ordered_ids[previous] == ordered_ids[at]) {
                fail(error, Code::Format, "duplicate ID in formula ordering");
                return false;
            }
        }
        size_t ignored = 0;
        if (!find_formula(document, ordered_ids[at], ignored)) {
            fail(error, Code::NotFound, "formula ID not found in ordering");
            return false;
        }
    }
    if (unchanged) {
        error.clear();
        return true;
    }
    if (!can_advance_revision(document, error)) return false;

    Document candidate;
    if (!clone_document(document, candidate, error)) return false;
    Array<Formula> ordered;
    if (!ordered.reserve(count)) {
        fail(error, Code::NoMemory, "formula ordering allocation failed");
        return false;
    }
    for (size_t at = 0; at < count; ++at) {
        size_t source_index = 0;
        (void)find_formula(document, ordered_ids[at], source_index);
        if (!ordered.push(document.formulas[source_index])) {
            fail(error, Code::NoMemory, "formula ordering allocation failed");
            return false;
        }
    }
    candidate.formulas.swap(ordered);
    candidate.revision = document.revision + 1;
    document.swap(candidate);
    error.clear();
    return true;
}

bool set_formula_source(Document &document, FormulaId id, const char *source,
                        Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    size_t index = 0;
    if (!find_formula(document, id, index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    size_t source_size = 0;
    if (!measure_input(source, kMaxSourceBytes, true, source_size, error,
                       "invalid formula source")) return false;
    Formula &formula = document.formulas[index];
    if (text_equals(formula.source, source, source_size)) {
        error.clear();
        return true;
    }
    if (!can_advance_revision(document, error) ||
        !can_advance_generation(formula, error)) return false;
    Text next_source;
    if (!set_text(next_source, source, source_size, error,
                  "formula source allocation failed")) return false;
    formula.source.swap(next_source);
    ++formula.generation;
    advance_revision(document);
    error.clear();
    return true;
}

bool set_formula_style(Document &document, FormulaId id, const Style &style,
                       Error &error) noexcept {
    if (!validate_document(document, error)) return false;
    if (!valid_style(style)) {
        fail(error, Code::Format, "invalid formula style");
        return false;
    }
    size_t index = 0;
    if (!find_formula(document, id, index)) {
        fail(error, Code::NotFound, "formula ID not found");
        return false;
    }
    Formula &formula = document.formulas[index];
    if (style_equal(formula.style, style)) {
        error.clear();
        return true;
    }
    if (!can_advance_revision(document, error) ||
        !can_advance_generation(formula, error)) return false;
    formula.style = style;
    ++formula.generation;
    advance_revision(document);
    error.clear();
    return true;
}

bool same_document_content(const Document &left, const Document &right) noexcept {
    if (left.title != right.title || left.formulas.size() != right.formulas.size())
        return false;
    for (size_t at = 0; at < left.formulas.size(); ++at) {
        const Formula &a = left.formulas[at];
        const Formula &b = right.formulas[at];
        if (a.id != b.id || a.name != b.name || a.source != b.source ||
            !style_equal(a.style, b.style)) return false;
    }
    return true;
}

bool valid_raster_extent(uint32_t width, uint32_t height) noexcept {
    if (!width || !height || width > kMaxRasterDimension ||
        height > kMaxRasterDimension) return false;
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <=
           kMaxRasterPixels;
}

} // namespace rm
