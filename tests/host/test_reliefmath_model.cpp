#include "model.h"
#include "history.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void test_storage_contracts() {
    rm::Error error;
    error.set(rm::Code::Parse, "parse error", 3, 9);
    assert(error.code == rm::Code::Parse && error.begin == 3 && error.end == 9);
    assert(strcmp(error.message.c_str(), "parse error") == 0);
    error.set(rm::Code::Unsupported, "unsupported");
    assert(error.begin == 0 && error.end == 0);

    rm::Text text;
    assert(text.set("fraction", 8));
    assert(text.size() == 8);
    assert(strcmp(text.c_str(), "fraction") == 0);
    assert(!text.set("\xc0\xaf", 2));
    assert(text.size() == 8 && strcmp(text.c_str(), "fraction") == 0);
    rm::testing::fail_allocations_after(0);
    assert(!text.set("replacement", 11));
    rm::testing::allow_allocations();
    assert(text.size() == 8 && strcmp(text.c_str(), "fraction") == 0);

    rm::Array<uint32_t> left;
    assert(left.push(11));
    assert(left.push(22));
    assert(left.resize(4));
    assert(left.size() == 4 && left[0] == 11 && left[1] == 22);
    assert(left[2] == 0 && left[3] == 0);
    rm::Array<uint32_t> copy;
    assert(copy.clone_from(left));
    rm::Array<uint32_t> right;
    right.swap(copy);
    assert(right.size() == 4 && right[1] == 22 && copy.size() == 0);
    rm::Array<uint32_t> full;
    for (uint32_t value = 0; value < 8; ++value) assert(full.push(value));
    rm::testing::fail_allocations_after(0);
    assert(!full.push(8));
    rm::testing::allow_allocations();
    assert(full.size() == 8 && full[7] == 7);

    rm::Bytes bytes;
    const uint8_t initial[] = {0x10, 0x20, 0x30};
    assert(bytes.append(initial, sizeof(initial)));
    assert(bytes.size() == 3 && bytes.data()[2] == 0x30);
    assert(bytes.append(bytes.data(), bytes.size()));
    assert(bytes.size() == 6 && bytes.data()[3] == 0x10 && bytes.data()[5] == 0x30);
    assert(bytes.resize(8));
    assert(bytes.data()[6] == 0 && bytes.data()[7] == 0);
    assert(bytes.resize(64));
    for (size_t at = 8; at < 64; ++at) bytes.data()[at] = static_cast<uint8_t>(at);
    const uint8_t extra = 0xee;
    rm::testing::fail_allocations_after(0);
    assert(!bytes.append(&extra, 1));
    rm::testing::allow_allocations();
    assert(bytes.size() == 64 && bytes.data()[63] == 63);
}

void test_rvalue_push_failure_preserves_owned_input_and_aliases() {
    const uint8_t payload[] = {0x42, 0x73};
    rm::Array<rm::Bytes> array;
    assert(array.resize(8));
    rm::Bytes input;
    assert(input.append(payload, sizeof(payload)));
    const uint8_t *owned = input.data();
    rm::testing::fail_allocations_after(0);
    assert(!array.push(static_cast<rm::Bytes &&>(input)));
    rm::testing::allow_allocations();
    assert(array.size() == 8);
    assert(input.data() == owned && input.size() == 2);
    assert(memcmp(input.data(), payload, sizeof(payload)) == 0);
    assert(array.push(static_cast<rm::Bytes &&>(input)));
    assert(input.size() == 0 && array.size() == 9);
    assert(array[8].data() == owned && array[8].size() == 2);

    rm::Array<rm::Bytes> aliases;
    assert(aliases.resize(8));
    assert(aliases[0].append(payload, sizeof(payload)));
    owned = aliases[0].data();
    rm::testing::fail_allocations_after(0);
    assert(!aliases.push(static_cast<rm::Bytes &&>(aliases[0])));
    rm::testing::allow_allocations();
    assert(aliases.size() == 8);
    assert(aliases[0].data() == owned && aliases[0].size() == 2);
    assert(memcmp(aliases[0].data(), payload, sizeof(payload)) == 0);
    assert(aliases.push(static_cast<rm::Bytes &&>(aliases[0])));
    assert(aliases.size() == 9 && aliases[0].size() == 0);
    assert(aliases[8].data() == owned && aliases[8].size() == 2);
    assert(memcmp(aliases[8].data(), payload, sizeof(payload)) == 0);
    for (size_t at = 1; at < 8; ++at) assert(aliases[at].size() == 0);
}

void test_clone_and_sort_allocation_failures_are_atomic() {
    rm::Document source;
    rm::Document destination;
    rm::Error error;
    rm::FormulaId first = 0;
    rm::FormulaId second = 0;
    rm::FormulaId existing = 0;
    assert(rm::add_formula(source, "one", "1", first, error));
    assert(rm::add_formula(source, "two", "2", second, error));
    assert(rm::add_formula(destination, "old", "old text", existing, error));
    rm::testing::fail_allocations_after(0);
    assert(!rm::clone_document(source, destination, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(destination.formulas.size() == 1);
    assert(destination.formulas[0].id == existing);
    assert(strcmp(destination.formulas[0].source.c_str(), "old text") == 0);

    const uint64_t revision = source.revision;
    rm::FormulaId reverse[] = {second, first};
    rm::testing::fail_allocations_after(0);
    assert(!rm::sort_formulas(source, reverse, 2, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(source.revision == revision);
    assert(source.formulas[0].id == first && source.formulas[1].id == second);
}

void test_multiple_formulas_and_stable_ids() {
    rm::Document document;
    rm::Error error;
    rm::FormulaId first = 0;
    rm::FormulaId second = 0;
    assert(rm::add_formula(document, "分数", "\\frac{1}{2}", first, error));
    assert(rm::add_formula(document, "积分", "\\int_0^1 x\\,dx", second, error));
    assert(first == 1 && second == 2);
    assert(document.formulas.size() == 2);

    rm::FormulaId copied = 0;
    assert(rm::copy_formula(document, first, copied, error));
    assert(copied == 3 && document.formulas[2].id == copied);
    assert(strcmp(document.formulas[2].source.c_str(), "\\frac{1}{2}") == 0);
    assert(rm::delete_formula(document, second, error));
    assert(document.formulas.size() == 2);
    assert(document.formulas[0].id == first && document.formulas[1].id == copied);
    assert(rm::add_formula(document, "新公式", "x+y", second, error));
    assert(second == 4);
    assert(document.formulas[0].id == first && document.formulas[1].id == copied);
    assert(document.formulas[2].id == second);
}

void test_operations_use_ids_and_advance_revisions() {
    rm::Document document;
    rm::Error error;
    rm::FormulaId a = 0;
    rm::FormulaId b = 0;
    assert(rm::add_formula(document, "A", "a+b", a, error));
    assert(rm::add_formula(document, "B", "b+c", b, error));
    uint64_t revision = document.revision;
    assert(rm::move_formula(document, b, 0, error));
    assert(document.revision == revision + 1 && document.formulas[0].id == b);
    assert(document.formulas[1].id == a);

    uint64_t generation = document.formulas[1].generation;
    revision = document.revision;
    assert(rm::rename_formula(document, a, "A renamed", error));
    assert(document.revision == revision + 1);
    assert(document.formulas[1].generation == generation);
    assert(rm::set_formula_source(document, a, "a-b", error));
    assert(document.revision == revision + 2);
    assert(document.formulas[1].generation == generation + 1);

    rm::Style style = document.formulas[1].style;
    style.rgba = 0x336699ff;
    style.point_size = 36;
    style.padding = 16;
    style.display = false;
    assert(rm::set_formula_style(document, a, style, error));
    assert(document.revision == revision + 3);
    assert(document.formulas[1].generation == generation + 2);
    assert(document.formulas[1].style.rgba == 0x336699ff);
    assert(!rm::rename_formula(document, 999, "missing", error));
    assert(error.code == rm::Code::NotFound);

    rm::FormulaId order[] = {a, b};
    assert(rm::sort_formulas(document, order, 2, error));
    assert(document.formulas[0].id == a && document.formulas[1].id == b);
    const uint64_t before_bad_order = document.revision;
    rm::FormulaId invalid_order[] = {a, a};
    assert(!rm::sort_formulas(document, invalid_order, 2, error));
    assert(document.revision == before_bad_order);
    assert(document.formulas[0].id == a && document.formulas[1].id == b);
}

void test_limits_utf8_and_style_validation() {
    rm::Document empty;
    rm::Error error;
    assert(rm::validate_document(empty, error));
    assert(!rm::valid_raster_extent(0, 1));
    assert(!rm::valid_raster_extent(16384, 16384));
    assert(rm::valid_raster_extent(16384, 1024));

    rm::Document document;
    rm::FormulaId id = 0;
    assert(!rm::add_formula(document, "bad", "\xc0\xaf", id, error));
    assert(error.code == rm::Code::InvalidUtf8);
    assert(document.formulas.size() == 0 && id == 0);

    assert(rm::add_formula(document, "ok", "x", id, error));
    rm::Style invalid = document.formulas[0].style;
    invalid.rgba = 0x00000000;
    assert(!rm::set_formula_style(document, id, invalid, error));
    assert(error.code == rm::Code::Format);
    invalid = document.formulas[0].style;
    invalid.point_size = 0;
    assert(!rm::set_formula_style(document, id, invalid, error));
    assert(error.code == rm::Code::Format);

    char *source = static_cast<char *>(malloc(rm::kMaxSourceBytes + 2));
    assert(source);
    memset(source, 'x', rm::kMaxSourceBytes + 1);
    source[rm::kMaxSourceBytes] = 0;
    assert(rm::set_formula_source(document, id, source, error));
    source[rm::kMaxSourceBytes] = 'x';
    source[rm::kMaxSourceBytes + 1] = 0;
    assert(!rm::set_formula_source(document, id, source, error));
    assert(error.code == rm::Code::Limit);
    assert(document.formulas[0].source.size() == rm::kMaxSourceBytes);
    free(source);

    rm::Document many;
    for (size_t i = 0; i < rm::kMaxFormulaCount; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "formula-%zu", i);
        rm::FormulaId added = 0;
        assert(rm::add_formula(many, name, "x", added, error));
        assert(added == i + 1);
    }
    assert(many.formulas.size() == 256);
    const rm::FormulaId old_next = many.next_id;
    assert(!rm::add_formula(many, "overflow", "y", id, error));
    assert(error.code == rm::Code::Limit);
    assert(many.formulas.size() == 256 && many.next_id == old_next);
}

void test_allocation_failure_and_id_overflow_are_atomic() {
    rm::Document document;
    rm::Error error;
    rm::FormulaId id = 0;
    assert(rm::add_formula(document, "kept", "original", id, error));
    const uint64_t revision = document.revision;
    rm::testing::fail_allocations_after(0);
    rm::FormulaId failed_id = 77;
    assert(!rm::add_formula(document, "lost", "candidate", failed_id, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(failed_id == 77 && document.formulas.size() == 1);
    assert(document.formulas[0].id == id);
    assert(strcmp(document.formulas[0].name.c_str(), "kept") == 0);
    assert(strcmp(document.formulas[0].source.c_str(), "original") == 0);
    assert(document.revision == revision);

    document.next_id = UINT64_MAX;
    rm::FormulaId last = 0;
    assert(rm::add_formula(document, "last", "z", last, error));
    assert(last == UINT64_MAX && document.next_id == 0);
    const size_t count = document.formulas.size();
    assert(!rm::add_formula(document, "wrapped", "z", failed_id, error));
    assert(error.code == rm::Code::Limit);
    assert(document.formulas.size() == count && document.next_id == 0);
}

void test_history_noop_undo_redo_branch_and_generation() {
    rm::Document document;
    rm::Error error;
    rm::FormulaId a = 0;
    rm::FormulaId b = 0;
    assert(rm::add_formula(document, "A", "a", a, error));
    assert(rm::add_formula(document, "B", "b", b, error));

    rm::History no_op;
    assert(no_op.begin(document, error));
    assert(no_op.commit(document, error));
    assert(!no_op.can_undo() && !no_op.can_redo());

    rm::History history;
    assert(history.mark_saved(document, error));
    assert(!history.is_dirty(document));
    const uint64_t old_generation = document.formulas[1].generation;
    assert(history.begin(document, error));
    assert(rm::move_formula(document, b, 0, error));
    assert(history.commit(document, error));
    assert(document.formulas[0].id == b && history.can_undo());
    assert(history.is_dirty(document));

    const uint64_t before_failed_undo_revision = document.revision;
    rm::testing::fail_allocations_after(0);
    assert(!history.undo(document, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(document.revision == before_failed_undo_revision);
    assert(document.formulas[0].id == b && history.can_undo());

    assert(history.undo(document, error));
    assert(document.formulas[0].id == a && document.formulas[1].id == b);
    assert(document.formulas[1].generation > old_generation);
    assert(!history.is_dirty(document));
    const uint64_t undone_generation = document.formulas[1].generation;
    assert(history.redo(document, error));
    assert(document.formulas[0].id == b && document.formulas[1].id == a);
    assert(document.formulas[1].generation > undone_generation);
    assert(history.undo(document, error));

    assert(history.begin(document, error));
    rm::FormulaId created = 0;
    assert(rm::add_formula(document, "branch", "q", created, error));
    assert(created == 3);
    assert(history.commit(document, error));
    assert(!history.can_redo());
    assert(history.begin(document, error));
    rm::FormulaId later = 0;
    assert(rm::add_formula(document, "after branch", "r", later, error));
    assert(later == 4);
    assert(history.commit(document, error));
}

void test_history_failures_restore_document() {
    rm::Document document;
    rm::Error error;
    rm::FormulaId id = 0;
    assert(rm::add_formula(document, "unchanged", "before", id, error));
    const uint64_t revision = document.revision;

    rm::History begin_failure;
    rm::testing::fail_allocations_after(0);
    assert(!begin_failure.begin(document, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(document.revision == revision && document.formulas.size() == 1);
    assert(strcmp(document.formulas[0].source.c_str(), "before") == 0);

    rm::History history;
    assert(history.begin(document, error));
    assert(rm::set_formula_source(document, id, "after", error));
    rm::testing::fail_allocations_after(0);
    assert(!history.commit(document, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(document.revision == revision && document.formulas.size() == 1);
    assert(strcmp(document.formulas[0].name.c_str(), "unchanged") == 0);
    assert(strcmp(document.formulas[0].source.c_str(), "before") == 0);
}

void test_failed_commit_never_reissues_formula_ids() {
    for (size_t failed_allocation = 0; failed_allocation < 2; ++failed_allocation) {
        for (size_t copy_case = 0; copy_case < 2; ++copy_case) {
            const bool copy = copy_case != 0;
            rm::Document document;
            rm::Error error;
            rm::FormulaId original = 0;
            assert(rm::add_formula(document, "original", "x", original, error));
            const uint64_t revision = document.revision;
            rm::History history;
            assert(history.mark_saved(document, error));
            assert(history.begin(document, error));
            rm::FormulaId issued = 0;
            assert(copy ? rm::copy_formula(document, original, issued, error)
                        : rm::add_formula(document, "discarded", "y", issued, error));
            rm::testing::fail_allocations_after(failed_allocation);
            assert(!history.commit(document, error));
            rm::testing::allow_allocations();
            assert(error.code == rm::Code::NoMemory);
            assert(document.revision == revision && document.formulas.size() == 1);
            assert(document.formulas[0].id == original);
            assert(!history.in_transaction() && !history.can_undo());
            assert(!history.is_dirty(document));

            assert(history.begin(document, error));
            rm::FormulaId next = 0;
            assert(copy ? rm::copy_formula(document, original, next, error)
                        : rm::add_formula(document, "kept", "z", next, error));
            assert(next > issued);
            assert(history.commit(document, error));
            assert(history.undo(document, error));
            assert(history.redo(document, error));
            assert(document.formulas[1].id == next);
        }
    }

    rm::Document document;
    rm::Error error;
    rm::FormulaId original = 0;
    assert(rm::add_formula(document, "original", "x", original, error));
    document.next_id = UINT64_MAX;
    rm::History history;
    assert(history.begin(document, error));
    rm::FormulaId issued = 0;
    assert(rm::copy_formula(document, original, issued, error));
    assert(issued == UINT64_MAX);
    rm::testing::fail_allocations_after(0);
    assert(!history.commit(document, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory && document.formulas.size() == 1);
    assert(!rm::add_formula(document, "exhausted", "y", issued, error));
    assert(error.code == rm::Code::Limit);
}

void test_failed_commit_never_reissues_render_generations() {
    for (size_t failed_allocation = 0; failed_allocation < 2; ++failed_allocation) {
        for (size_t edit_case = 0; edit_case < 4; ++edit_case) {
            const bool style_edit = (edit_case & 1) != 0;
            rm::Document document;
            rm::Error error;
            rm::FormulaId original = 0;
            rm::FormulaId other = 0;
            assert(rm::add_formula(document, "original", "before", original, error));
            assert(rm::add_formula(document, "other", "y", other, error));
            const uint64_t revision = document.revision;
            rm::History history;
            assert(history.mark_saved(document, error));
            assert(history.begin(document, error));
            rm::Style changed = document.formulas[0].style;
            changed.point_size = 36;
            assert(style_edit ? rm::set_formula_style(document, original, changed, error)
                              : rm::set_formula_source(document, original, "discarded", error));
            const uint64_t issued = document.formulas[0].generation;
            if (edit_case >= 2) assert(rm::delete_formula(document, original, error));
            rm::testing::fail_allocations_after(failed_allocation);
            assert(!history.commit(document, error));
            rm::testing::allow_allocations();
            assert(error.code == rm::Code::NoMemory);
            assert(document.revision == revision && document.formulas.size() == 2);
            assert(document.formulas[0].id == original && document.formulas[1].id == other);
            assert(strcmp(document.formulas[0].source.c_str(), "before") == 0);
            assert(document.formulas[0].style.point_size == 24);
            assert(!history.in_transaction() && !history.is_dirty(document));
            const uint64_t restored = document.formulas[0].generation;
            assert(history.begin(document, error));
            assert(style_edit ? rm::set_formula_style(document, original, changed, error)
                              : rm::set_formula_source(document, original, "kept", error));
            assert(document.formulas[0].generation > issued);
            assert(restored > issued);
            assert(document.formulas[0].generation > restored);
            assert(history.commit(document, error));
        }
    }

    rm::Document document;
    rm::Error error;
    rm::FormulaId original = 0;
    assert(rm::add_formula(document, "original", "before", original, error));
    document.formulas[0].generation = UINT64_MAX - 1;
    rm::History history;
    assert(history.begin(document, error));
    assert(rm::set_formula_source(document, original, "discarded", error));
    assert(document.formulas[0].generation == UINT64_MAX);
    rm::testing::fail_allocations_after(0);
    assert(!history.commit(document, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(strcmp(document.formulas[0].source.c_str(), "before") == 0);
    assert(!rm::set_formula_source(document, original, "kept", error));
    assert(error.code == rm::Code::Limit);
}

} // namespace

int main() {
    test_storage_contracts();
    test_rvalue_push_failure_preserves_owned_input_and_aliases();
    test_clone_and_sort_allocation_failures_are_atomic();
    test_multiple_formulas_and_stable_ids();
    test_operations_use_ids_and_advance_revisions();
    test_limits_utf8_and_style_validation();
    test_allocation_failure_and_id_overflow_are_atomic();
    test_history_noop_undo_redo_branch_and_generation();
    test_history_failures_restore_document();
    test_failed_commit_never_reissues_formula_ids();
    test_failed_commit_never_reissues_render_generations();
    return 0;
}
