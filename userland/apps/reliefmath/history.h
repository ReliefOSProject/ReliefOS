#ifndef RELIEFMATH_HISTORY_H
#define RELIEFMATH_HISTORY_H

#include "model.h"

namespace rm {

class History {
    struct State {
        Document document;
        State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;
        State(State &&) noexcept = default;
        State &operator=(State &&) noexcept = default;
    };

    Array<State> states_;
    size_t cursor_ = 0;
    bool active_ = false;
    bool has_saved_ = false;
    Document before_;
    Document saved_;
    FormulaId high_water_next_id_ = 1;
    bool ids_exhausted_ = false;
    uint64_t generation_clock_ = 0;

    void record_clocks(const Document &document) noexcept;
    uint64_t maximum_generation(const Document &document) const noexcept;
    bool restore(const Document &target, Document &document, Error &error) noexcept;
    void rollback(Document &document) noexcept;

public:
    History() = default;
    History(const History &) = delete;
    History &operator=(const History &) = delete;
    History(History &&) noexcept = default;
    History &operator=(History &&) noexcept = default;

    bool begin(const Document &document, Error &error) noexcept;
    // Failure restores content and revision while retaining runtime clock high-water marks.
    bool commit(Document &document, Error &error) noexcept;
    bool undo(Document &document, Error &error) noexcept;
    bool redo(Document &document, Error &error) noexcept;
    bool mark_saved(const Document &document, Error &error) noexcept;
    bool is_dirty(const Document &document) const noexcept;
    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    bool in_transaction() const noexcept { return active_; }
};

} // namespace rm

#endif
