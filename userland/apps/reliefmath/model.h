#ifndef RELIEFMATH_MODEL_H
#define RELIEFMATH_MODEL_H

#include "error.h"
#include "limits.h"
#include "storage.h"

#include <stdint.h>

namespace rm {

using FormulaId = uint64_t;

struct Style {
    uint32_t rgba = 0x000000ff; // RRGGBBAA
    uint16_t point_size = 24;
    uint16_t padding = 12;
    bool display = true;
};

struct Formula {
    FormulaId id = 0;
    Text name;
    Text source;
    Style style;
    uint64_t generation = 1;
};

struct Document {
    Text title;
    Array<Formula> formulas;
    FormulaId next_id = 1;
    uint64_t revision = 0;

    Document() = default;
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;
    Document(Document &&) noexcept = default;
    Document &operator=(Document &&) noexcept = default;

    void swap(Document &other) noexcept {
        title.swap(other.title);
        formulas.swap(other.formulas);
        FormulaId saved_id = next_id;
        next_id = other.next_id;
        other.next_id = saved_id;
        uint64_t saved_revision = revision;
        revision = other.revision;
        other.revision = saved_revision;
    }
};

/** Validates stable IDs, UTF-8 fields, style bounds and document capacity. */
bool validate_document(const Document &document, Error &error) noexcept;
/** Copies the document with shared immutable text; failure leaves destination intact. */
bool clone_document(const Document &source, Document &destination, Error &error) noexcept;
/** Adds a formula and returns a monotonically assigned stable ID. */
bool add_formula(Document &document, const char *name, const char *source,
                 FormulaId &out_id, Error &error) noexcept;
/** Copies one formula under a new stable ID. */
bool copy_formula(Document &document, FormulaId source_id,
                  FormulaId &out_id, Error &error) noexcept;
/** Removes a formula without renumbering any remaining IDs. */
bool delete_formula(Document &document, FormulaId id, Error &error) noexcept;
/** Changes a formula name selected by stable ID. */
bool rename_formula(Document &document, FormulaId id, const char *name,
                    Error &error) noexcept;
/** Places the selected stable ID at its requested final index. */
bool move_formula(Document &document, FormulaId id, size_t index,
                  Error &error) noexcept;
/** Applies an explicit complete ordering of stable IDs atomically. */
bool sort_formulas(Document &document, const FormulaId *ordered_ids,
                   size_t count, Error &error) noexcept;
/** Changes source text and invalidates prior renders by advancing generation. */
bool set_formula_source(Document &document, FormulaId id, const char *source,
                        Error &error) noexcept;
/** Changes visual style and invalidates prior renders by advancing generation. */
bool set_formula_style(Document &document, FormulaId id, const Style &style,
                       Error &error) noexcept;
/** Compares persisted content, ignoring runtime generations and revision counters. */
bool same_document_content(const Document &left, const Document &right) noexcept;
/** Checks nonzero dimensions, per-axis limits, and the total pixel budget. */
bool valid_raster_extent(uint32_t width, uint32_t height) noexcept;

} // namespace rm

#endif
