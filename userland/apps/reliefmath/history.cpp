#include "history.h"

namespace rm {
namespace {

void fail(Error &error, Code code, const char *message) noexcept {
    error.set(code, message);
}

void clear_document(Document &document) noexcept {
    Document empty;
    document.swap(empty);
}

} // namespace

void History::record_clocks(const Document &document) noexcept {
    if (!document.next_id) {
        ids_exhausted_ = true;
    } else if (!ids_exhausted_ && document.next_id > high_water_next_id_) {
        high_water_next_id_ = document.next_id;
    }
    for (size_t at = 0; at < document.formulas.size(); ++at) {
        if (document.formulas[at].generation > generation_clock_)
            generation_clock_ = document.formulas[at].generation;
    }
    if (active_ && document.revision > before_.revision) {
        uint64_t previous_maximum = 0;
        for (size_t at = 0; at < before_.formulas.size(); ++at) {
            if (before_.formulas[at].generation > previous_maximum)
                previous_maximum = before_.formulas[at].generation;
        }
        // Every model edit advances revision, including edits to formulas later deleted.
        const uint64_t edits = document.revision - before_.revision;
        const uint64_t upper_bound = previous_maximum > UINT64_MAX - edits
            ? UINT64_MAX : previous_maximum + edits;
        if (upper_bound > generation_clock_) generation_clock_ = upper_bound;
    }
}

uint64_t History::maximum_generation(const Document &document) const noexcept {
    uint64_t maximum = generation_clock_;
    for (size_t at = 0; at < document.formulas.size(); ++at) {
        if (document.formulas[at].generation > maximum)
            maximum = document.formulas[at].generation;
    }
    for (size_t state = 0; state < states_.size(); ++state) {
        const Document &saved = states_[state].document;
        for (size_t at = 0; at < saved.formulas.size(); ++at) {
            if (saved.formulas[at].generation > maximum)
                maximum = saved.formulas[at].generation;
        }
    }
    return maximum;
}

void History::rollback(Document &document) noexcept {
    record_clocks(document);
    document.swap(before_);
    if (ids_exhausted_) {
        document.next_id = 0;
    } else if (document.next_id < high_water_next_id_) {
        document.next_id = high_water_next_id_;
    }
    if (document.formulas.size()) {
        // Retire discarded render requests before exposing the restored content.
        const uint64_t epoch = generation_clock_ == UINT64_MAX
            ? UINT64_MAX : generation_clock_ + 1;
        for (size_t at = 0; at < document.formulas.size(); ++at)
            document.formulas[at].generation = epoch;
        generation_clock_ = epoch;
    }
    active_ = false;
    clear_document(before_);
}

bool History::begin(const Document &document, Error &error) noexcept {
    if (active_) {
        fail(error, Code::Conflict, "history transaction already active");
        return false;
    }
    if (states_.size() && !same_document_content(document, states_[cursor_].document)) {
        fail(error, Code::Conflict, "document differs from current history state");
        return false;
    }

    Document backup;
    if (!clone_document(document, backup, error)) return false;
    if (!states_.size()) {
        State initial;
        if (!clone_document(document, initial.document, error)) return false;
        Array<State> initial_states;
        if (!initial_states.reserve(1) || !initial_states.push(static_cast<State &&>(initial))) {
            fail(error, Code::NoMemory, "history baseline allocation failed");
            return false;
        }
        states_.swap(initial_states);
        cursor_ = 0;
    }
    before_.swap(backup);
    active_ = true;
    record_clocks(document);
    error.clear();
    return true;
}

bool History::commit(Document &document, Error &error) noexcept {
    if (!active_) {
        fail(error, Code::Conflict, "no active history transaction");
        return false;
    }
    record_clocks(document);
    if (!validate_document(document, error)) {
        rollback(document);
        return false;
    }
    if (same_document_content(before_, document)) {
        record_clocks(document);
        active_ = false;
        clear_document(before_);
        error.clear();
        return true;
    }

    State next_state;
    if (!clone_document(document, next_state.document, error)) {
        rollback(document);
        return false;
    }
    Array<State> next_states;
    if (cursor_ > SIZE_MAX - 2 || !next_states.reserve(cursor_ + 2)) {
        fail(error, Code::NoMemory, "history state allocation failed");
        rollback(document);
        return false;
    }
    for (size_t at = 0; at <= cursor_; ++at) {
        if (!next_states.push(static_cast<State &&>(states_[at]))) {
            fail(error, Code::NoMemory, "history state allocation failed");
            rollback(document);
            return false;
        }
    }
    if (!next_states.push(static_cast<State &&>(next_state))) {
        fail(error, Code::NoMemory, "history state allocation failed");
        rollback(document);
        return false;
    }
    states_.swap(next_states);
    cursor_ = states_.size() - 1;
    active_ = false;
    clear_document(before_);
    record_clocks(document);
    error.clear();
    return true;
}

bool History::restore(const Document &target, Document &document, Error &error) noexcept {
    Document candidate;
    if (!clone_document(target, candidate, error)) return false;
    if (document.revision == UINT64_MAX) {
        fail(error, Code::Limit, "document revision exhausted");
        return false;
    }
    const uint64_t maximum = maximum_generation(document);
    if (candidate.formulas.size() && maximum == UINT64_MAX) {
        fail(error, Code::Limit, "formula generation exhausted");
        return false;
    }
    const uint64_t epoch = candidate.formulas.size() ? maximum + 1 : maximum;
    for (size_t at = 0; at < candidate.formulas.size(); ++at)
        candidate.formulas[at].generation = epoch;
    candidate.revision = document.revision + 1;
    if (ids_exhausted_) {
        candidate.next_id = 0;
    } else if (candidate.next_id < high_water_next_id_) {
        candidate.next_id = high_water_next_id_;
    }
    if (!validate_document(candidate, error)) return false;
    document.swap(candidate);
    if (epoch > generation_clock_) generation_clock_ = epoch;
    error.clear();
    return true;
}

bool History::undo(Document &document, Error &error) noexcept {
    if (active_) {
        fail(error, Code::Conflict, "cannot undo an active transaction");
        return false;
    }
    if (!can_undo()) {
        fail(error, Code::Conflict, "nothing to undo");
        return false;
    }
    if (!restore(states_[cursor_ - 1].document, document, error)) return false;
    --cursor_;
    return true;
}

bool History::redo(Document &document, Error &error) noexcept {
    if (active_) {
        fail(error, Code::Conflict, "cannot redo an active transaction");
        return false;
    }
    if (!can_redo()) {
        fail(error, Code::Conflict, "nothing to redo");
        return false;
    }
    if (!restore(states_[cursor_ + 1].document, document, error)) return false;
    ++cursor_;
    return true;
}

bool History::mark_saved(const Document &document, Error &error) noexcept {
    if (active_) {
        fail(error, Code::Conflict, "cannot mark an active transaction saved");
        return false;
    }
    Document candidate;
    if (!clone_document(document, candidate, error)) return false;
    saved_.swap(candidate);
    has_saved_ = true;
    error.clear();
    return true;
}

bool History::is_dirty(const Document &document) const noexcept {
    return !has_saved_ || !same_document_content(document, saved_);
}

bool History::can_undo() const noexcept { return states_.size() && cursor_ > 0; }

bool History::can_redo() const noexcept {
    return states_.size() && cursor_ + 1 < states_.size();
}

} // namespace rm
