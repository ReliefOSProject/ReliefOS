#include "model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rp;

#ifdef RP_TEST_TRACK_ALLOCATIONS
extern "C" void *__real_malloc(size_t);
static bool track_malloc_bytes = false;
static size_t tracked_malloc_bytes = 0;
static long fail_malloc_call = -1;
static long malloc_call = 0;

extern "C" void *__wrap_malloc(size_t size) {
    if (fail_malloc_call >= 0 && ++malloc_call == fail_malloc_call) return nullptr;
    if (track_malloc_bytes) tracked_malloc_bytes += size;
    return __real_malloc(size);
}
#endif

static bool has_text(const Text &value, const char *expected) {
    const size_t length = strlen(expected);
    return value.size() == length && (!length || !memcmp(value.c_str(), expected, length));
}

static Text error;

static Object make_object(const char *text, float x, float y, float w = 120, float h = 60) {
    Object object;
    object.kind = Kind::Text;
    object.rect = {x, y, w, h};
    assert(object.text.set(text, strlen(text)));
    return object;
}

static uint32_t add_object(Editor &editor, Object object) {
    uint32_t id = 0;
    assert(editor.insert_object(static_cast<Object &&>(object), &id));
    assert(id != 0);
    return id;
}

static void test_default_deck_layouts_and_slide_lifecycle() {
    Deck deck;
    assert(deck.init());
    assert(deck.width == 960 && deck.height == 540);
    assert(deck.slides.size() == 1 && deck.slides[0].objects.empty());
    Text error;
    assert(deck.validate(error));

    Editor editor;
    assert(editor.add_slide(Layout::Title));
    assert(editor.current == 1 && editor.deck.slides.size() == 2);
    assert(editor.deck.slides[1].objects.size() == 1);
    assert(editor.deck.slides[1].objects[0].text == "标题");
    assert(editor.add_slide(Layout::Content));
    assert(editor.deck.slides[2].objects.size() == 2);
    assert(editor.add_slide(Layout::TwoColumns));
    assert(editor.deck.slides[3].objects.size() == 3);
    const uint32_t last_id = editor.deck.slides[3].objects[0].id;
    assert(editor.duplicate_slide());
    assert(editor.current == 4 && editor.deck.slides.size() == 5);
    assert(editor.deck.slides[4].objects.size() == 3);
    assert(editor.deck.slides[4].objects[0].id != last_id);
    assert(editor.move_slide(-1));
    assert(editor.current == 3 && editor.deck.slides[3].objects[0].id != last_id);
    assert(editor.delete_slide());
    assert(editor.deck.slides.size() == 4 && editor.current == 3);
    assert(editor.deck.slides[3].objects[0].id == last_id);
    while (editor.deck.slides.size() > 1) assert(editor.delete_slide());
    assert(editor.delete_slide());
    assert(editor.deck.slides.size() == 1 && editor.current == 0);
    assert(editor.deck.slides[0].objects.empty());
    assert(editor.deck.slides[0].notes.empty());
    assert(editor.deck.slides[0].background == 0xffffff);
}

static void test_object_ids_text_and_immutable_image_copy_paste() {
    Editor source;
    const char rgba[] = {char(0), char(1), char(2), char(255), char(5), char(6), char(7), char(8)};
    uint32_t image_id = 0;
    assert(source.insert_image(2, 1, rgba, sizeof(rgba), {40, 50, 80, 40}, &image_id));
    assert(source.deck.object(0, image_id));
    assert(source.deck.assets.size() == 1 && source.deck.assets[0].rgba.size() == sizeof(rgba));
    assert(!memcmp(source.deck.assets[0].rgba.c_str(), rgba, sizeof(rgba)));
    uint32_t text_id = add_object(source, make_object("中文", 200, 60));
    source.select(image_id);
    source.select(text_id, true);
    Deck fragment;
    assert(source.copy_selected(fragment));
    assert(fragment.slides.size() == 1 && fragment.slides[0].objects.size() == 2);
    assert(fragment.assets.size() == 1);
    assert(fragment.slides[0].objects[0].id == image_id);
    assert(fragment.slides[0].objects[0].asset == 0);
    assert(fragment.slides[0].objects[1].id == text_id);

    Editor target;
    const char other_rgba[] = {char(9), char(8), char(7), char(6)};
    assert(target.insert_image(1, 1, other_rgba, sizeof(other_rgba), {8, 8, 24, 24}));
    assert(target.paste_objects(fragment));
    assert(target.deck.slides[0].objects.size() == 3);
    const Object &pasted_image = target.deck.slides[0].objects[1];
    const Object &pasted_text = target.deck.slides[0].objects[2];
    assert(pasted_image.id != image_id && pasted_text.id != text_id);
    assert(pasted_image.asset == 1 && target.deck.assets.size() == 2);
    assert(target.deck.assets[1].rgba.size() == sizeof(rgba));
    assert(!memcmp(target.deck.assets[1].rgba.c_str(), rgba, sizeof(rgba)));
    assert(pasted_image.rect.x == 56 && pasted_image.rect.y == 66);
    assert(pasted_text.rect.x == 216 && pasted_text.rect.y == 76);
    assert(target.deck.validate(error));
}

static void test_atomic_rejection_validation_and_binary_text_equality() {
    Text binary_a, binary_b;
    const char bytes_a[] = {'a', 0, 'b'};
    const char bytes_b[] = {'a', 0, 'c'};
    assert(binary_a.set(bytes_a, sizeof(bytes_a)) && binary_b.set(bytes_b, sizeof(bytes_b)));
    assert(binary_a != binary_b);
    assert(!(binary_a == "a"));

    Editor editor;
    const uint32_t id = add_object(editor, make_object("keep", 20, 20));
    const uint64_t revision = editor.deck.revision;
    const size_t history_can_undo = editor.can_undo();
    const char nul_text[] = {'x', 0, 'y'};
    const char control[] = {'x', 1, 'y'};
    const char invalid_utf8[] = {char(0xe4), char(0xb8)};
    const char cr_text[] = {'x', '\r', 'y'};
    assert(!editor.set_text(id, nul_text, sizeof(nul_text)));
    assert(!editor.set_text(id, control, sizeof(control)));
    assert(!editor.set_text(id, invalid_utf8, sizeof(invalid_utf8)));
    assert(!editor.set_text(id, cr_text, sizeof(cr_text)));
    assert(!editor.set_text(id, nullptr, 1));
    char oversized[MaxTextBytes + 1];
    memset(oversized, 'x', sizeof(oversized));
    assert(!editor.set_text(id, oversized, sizeof(oversized)));
    assert(!editor.set_notes("bad\vnotes", 9));
    assert(!editor.set_background(0x1000000));
    assert(editor.deck.revision == revision && editor.can_undo() == (history_can_undo != 0));
    assert(has_text(editor.deck.object(0, id)->text, "keep"));

    Deck invalid;
    assert(invalid.clone_from(editor.deck));
    invalid.slides[0].objects[0].rect.x = NAN;
    assert(!editor.commit(static_cast<Deck &&>(invalid)));
    assert(editor.deck.revision == revision && has_text(editor.deck.object(0, id)->text, "keep"));
    assert(invalid.init());
    invalid.slides[0].objects.push(make_object("outside", 950, 20, 20, 40));
    assert(!invalid.validate(error));
    assert(invalid.init());
    Object invalid_style = make_object("style", 10, 10);
    invalid_style.style.font_size = INFINITY;
    assert(invalid.slides[0].objects.push(invalid_style));
    assert(!invalid.validate(error));

    const char rgba[] = {char(1), char(2), char(3), char(4)};
    assert(!editor.insert_image(1, 1, rgba, 3, {0, 0, 20, 20}));
    assert(!editor.insert_image(0, 1, rgba, 0, {0, 0, 20, 20}));
    assert(editor.deck.revision == revision);
}

static void test_all_alignment_and_layer_operations_preserve_selection_order() {
    Editor editor;
    const uint32_t a = add_object(editor, make_object("a", 10, 10, 40, 20));
    const uint32_t b = add_object(editor, make_object("b", 80, 50, 60, 30));
    const uint32_t c = add_object(editor, make_object("c", 180, 100, 80, 40));
    const uint32_t d = add_object(editor, make_object("d", 260, 150, 40, 20));
    editor.select(a); editor.select(b, true); editor.select(c, true);

    assert(editor.align_selected(Align::Left));
    assert(editor.deck.object(0, a)->rect.x == 10);
    assert(editor.deck.object(0, b)->rect.x == 10 && editor.deck.object(0, c)->rect.x == 10);
    assert(editor.align_selected(Align::CenterX));
    assert(editor.deck.object(0, a)->rect.x + 20 == editor.deck.object(0, b)->rect.x + 30);
    assert(editor.deck.object(0, b)->rect.x + 30 == editor.deck.object(0, c)->rect.x + 40);
    assert(editor.align_selected(Align::Right));
    assert(editor.deck.object(0, a)->rect.x + 40 == editor.deck.object(0, b)->rect.x + 60);
    assert(editor.deck.object(0, b)->rect.x + 60 == editor.deck.object(0, c)->rect.x + 80);
    assert(editor.align_selected(Align::Top));
    assert(editor.deck.object(0, a)->rect.y == editor.deck.object(0, c)->rect.y);
    assert(editor.align_selected(Align::CenterY));
    assert(editor.deck.object(0, a)->rect.y + 10 == editor.deck.object(0, c)->rect.y + 20);
    assert(editor.align_selected(Align::Bottom));
    assert(editor.deck.object(0, a)->rect.y + 20 == editor.deck.object(0, b)->rect.y + 30);
    assert(editor.deck.object(0, b)->rect.y + 30 == editor.deck.object(0, c)->rect.y + 40);
    assert(editor.deck.validate(error));

    editor.clear_selection(); editor.select(b); editor.select(c, true);
    assert(editor.layer_selected(Layer::Top));
    assert(editor.deck.slides[0].objects[0].id == a);
    assert(editor.deck.slides[0].objects[1].id == d);
    assert(editor.deck.slides[0].objects[2].id == b);
    assert(editor.deck.slides[0].objects[3].id == c);
    assert(editor.layer_selected(Layer::Bottom));
    assert(editor.deck.slides[0].objects[0].id == b);
    assert(editor.deck.slides[0].objects[1].id == c);
    assert(editor.deck.slides[0].objects[2].id == a);
    assert(editor.deck.slides[0].objects[3].id == d);
    assert(editor.layer_selected(Layer::Up));
    assert(editor.deck.slides[0].objects[0].id == a);
    assert(editor.deck.slides[0].objects[1].id == b);
    assert(editor.deck.slides[0].objects[2].id == c);
    assert(editor.deck.slides[0].objects[3].id == d);
    assert(editor.layer_selected(Layer::Down));
    assert(editor.deck.slides[0].objects[0].id == b);
    assert(editor.deck.slides[0].objects[1].id == c);
    assert(editor.deck.slides[0].objects[2].id == a);
    assert(editor.deck.slides[0].objects[3].id == d);

    editor.clear_selection();
    editor.select(c); editor.select(a, true);
    const size_t before = editor.deck.slides[0].objects.size();
    assert(editor.duplicate_selected());
    assert(editor.deck.slides[0].objects.size() == before + 2);
    assert(editor.deck.slides[0].objects[before].text == "c");
    assert(editor.deck.slides[0].objects[before + 1].text == "a");
    assert(editor.selected.size() == 2);
    assert(editor.deck.object(0, editor.selected[0])->text == "c");
    assert(editor.deck.object(0, editor.selected[1])->text == "a");
}

static void test_noops_history_branch_revisions_and_replace_deck() {
    Editor editor;
    uint64_t rev = editor.deck.revision;
    assert(!editor.can_undo());
    assert(editor.set_background(0xffffff));
    assert(editor.deck.revision == rev && !editor.can_undo());
    assert(editor.set_background(0x123456));
    const uint64_t rev2 = editor.deck.revision;
    assert(rev2 > rev && editor.can_undo());
    assert(editor.set_background(0xabcdef));
    const uint64_t rev3 = editor.deck.revision;
    assert(rev3 > rev2);
    assert(editor.undo() && editor.deck.revision == rev2);
    assert(editor.set_background(0x55aa55));
    const uint64_t branch_revision = editor.deck.revision;
    assert(branch_revision > rev3 && branch_revision != rev3);
    assert(!editor.can_redo());
    assert(editor.undo() && editor.can_redo());
    assert(editor.redo() && editor.deck.revision == branch_revision);

    Deck replacement;
    assert(replacement.init());
    const uint64_t old_high = branch_revision;
    assert(editor.replace_deck(static_cast<Deck &&>(replacement)));
    assert(editor.deck.revision > old_high);
    assert(!editor.can_undo() && !editor.can_redo());
    assert(editor.current == 0 && editor.selected.empty());
}

static void test_update_objects_duplicate_delete_and_selection_pruning() {
    Editor editor;
    const uint32_t a = add_object(editor, make_object("a", 10, 10));
    const uint32_t b = add_object(editor, make_object("b", 180, 10));
    editor.select(a); editor.select(b, true);
    Array<Object> replacements;
    Object moved = *editor.deck.object(0, a);
    moved.rect.x = 44;
    assert(replacements.push(moved));
    assert(editor.update_objects(replacements));
    assert(editor.deck.object(0, a)->rect.x == 44);
    const uint64_t revision = editor.deck.revision;
    Array<Object> invalid;
    Object unknown = moved; unknown.id = UINT32_MAX;
    assert(invalid.push(unknown));
    assert(!editor.update_objects(invalid));
    assert(editor.deck.revision == revision && editor.deck.object(0, a)->rect.x == 44);
    assert(editor.delete_selected());
    assert(editor.deck.slides[0].objects.empty() && editor.selected.empty());
    assert(editor.undo());
    assert(editor.deck.object(0, a) && editor.deck.object(0, b));
}

static void test_example_deck_is_self_contained_and_valid() {
    Deck deck;
    Text error;
    assert(example_deck(deck, error));
    assert(deck.slides.size() == 3);
    assert(deck.assets.size() == 1);
    assert(deck.assets[0].width && deck.assets[0].height);
    bool has_image = false, has_shape = false, has_chinese = false;
    for (size_t s = 0; s < deck.slides.size(); ++s)
        for (size_t i = 0; i < deck.slides[s].objects.size(); ++i) {
            const Object &object = deck.slides[s].objects[i];
            has_image |= object.kind == Kind::Image;
            has_shape |= object.kind == Kind::Rectangle || object.kind == Kind::Ellipse ||
                object.kind == Kind::Line || object.kind == Kind::Arrow;
            has_chinese |= object.text.size() != 0 && valid_utf8(object.text.c_str(), object.text.size());
        }
    assert(has_image && has_shape && has_chinese);
    assert(deck.validate(error));
}

static void test_deleted_images_release_asset_slots() {
    Editor editor;
    char rgba[4] = {};
    for (size_t i = 0; i < MaxAssets; ++i) {
        rgba[0] = static_cast<char>(i);
        rgba[1] = static_cast<char>(i >> 1);
        rgba[2] = static_cast<char>(i >> 2);
        rgba[3] = static_cast<char>(i ^ 0xa5);
        uint32_t id = 0;
        assert(editor.insert_image(1, 1, rgba, sizeof(rgba), {20, 20, 40, 40}, &id));
        assert(editor.selected.size() == 1 && editor.selected[0] == id);
        assert(editor.delete_selected());
    }
    rgba[0] = 0x55;
    assert(editor.insert_image(1, 1, rgba, sizeof(rgba), {20, 20, 40, 40}));
    assert(editor.deck.assets.size() == 1);
    assert(editor.deck.validate(error));
}

static void test_nonfinal_slide_deletion_compacts_assets_and_undo_keeps_them() {
    Editor editor;
    const char first_rgba[] = {1, 2, 3, 4};
    const char second_rgba[] = {5, 6, 7, 8};
    uint32_t first_id = 0, second_id = 0;
    assert(editor.insert_image(1, 1, first_rgba, sizeof(first_rgba), {10, 10, 20, 20}, &first_id));
    assert(editor.add_slide(Layout::Blank));
    assert(editor.insert_image(1, 1, second_rgba, sizeof(second_rgba), {30, 30, 20, 20}, &second_id));
    assert(editor.deck.assets.size() == 2);
    editor.current = 0;
    assert(editor.delete_slide());
    assert(editor.deck.slides.size() == 1 && editor.deck.assets.size() == 1);
    assert(editor.deck.object(0, second_id));
    assert(editor.deck.object(0, second_id)->asset == 0);
    assert(!memcmp(editor.deck.assets[0].rgba.c_str(), second_rgba, sizeof(second_rgba)));
    assert(editor.deck.validate(error));
    assert(editor.undo());
    assert(editor.deck.slides.size() == 2 && editor.deck.assets.size() == 2);
    assert(editor.deck.object(0, first_id) && editor.deck.object(1, second_id));
    assert(editor.deck.object(1, second_id)->asset == 1);
    assert(!memcmp(editor.deck.assets[1].rgba.c_str(), second_rgba, sizeof(second_rgba)));
}

static void test_undo_restores_live_selection_and_current_page() {
    Editor editor;
    const uint32_t a = add_object(editor, make_object("a", 20, 20));
    add_object(editor, make_object("b", 200, 20));
    editor.select(a);
    Object moved = *editor.deck.object(0, a);
    moved.rect.x = 50;
    Array<Object> replacements;
    assert(replacements.push(moved));
    assert(editor.update_objects(replacements));
    assert(editor.undo());
    assert(editor.deck.object(0, a)->rect.x == 20);
    assert(editor.selected.size() == 1 && editor.selected[0] == a);

    Editor direct_page;
    const uint32_t page_object = add_object(direct_page, make_object("page", 20, 20));
    assert(direct_page.add_slide(Layout::Blank));
    direct_page.current = 0;
    assert(direct_page.selected.push(page_object));
    Object page_moved = *direct_page.deck.object(0, page_object);
    page_moved.rect.x = 60;
    Array<Object> page_replacements;
    assert(page_replacements.push(page_moved));
    assert(direct_page.update_objects(page_replacements));
    assert(direct_page.undo());
    assert(direct_page.current == 0);
    assert(direct_page.selected.size() == 1 && direct_page.selected[0] == page_object);
    assert(direct_page.deck.object(0, page_object)->rect.x == 20);
}

#ifdef RP_TEST_TRACK_ALLOCATIONS
static void test_history_commit_does_not_reclone_retained_decks() {
    Editor editor;
    for (unsigned i = 0; i < 80; ++i) {
        Object shape;
        shape.kind = Kind::Rectangle;
        shape.rect = {static_cast<float>((i % 20) * 44), static_cast<float>((i / 20) * 44), 32, 32};
        assert(editor.insert_object(static_cast<Object &&>(shape)));
    }
    tracked_malloc_bytes = 0;
    track_malloc_bytes = true;
    const bool committed = editor.set_background(0x123456);
    track_malloc_bytes = false;
    assert(committed);
    assert(tracked_malloc_bytes < 128u * 1024u);
}

static void test_failed_history_commits_preserve_document_and_redo() {
    bool saw_failure = false;
    bool saw_success = false;
    for (long fail_at = 1; fail_at <= 32; ++fail_at) {
        Editor editor;
        const uint32_t id = add_object(editor, make_object("keep", 20, 20));
        assert(editor.set_background(0x112233));
        assert(editor.undo() && editor.can_redo());
        const uint64_t revision = editor.deck.revision;
        malloc_call = 0;
        fail_malloc_call = fail_at;
        const bool committed = editor.set_background(0x445566);
        fail_malloc_call = -1;
        if (!committed) {
            saw_failure = true;
            assert(editor.deck.slides[0].background == 0xffffff);
            assert(editor.deck.revision == revision);
            assert(editor.current == 0 && editor.selected.size() == 1 && editor.selected[0] == id);
            assert(editor.can_undo() && editor.can_redo());
            assert(editor.redo());
            assert(editor.deck.slides[0].background == 0x112233);
            assert(editor.undo());
            assert(editor.deck.slides[0].background == 0xffffff);
        } else {
            saw_success = true;
            assert(editor.deck.slides[0].background == 0x445566);
            assert(!editor.can_redo());
            break;
        }
    }
    assert(saw_failure && saw_success);
}

static void test_example_deck_propagates_body_text_allocation_failure() {
    Deck deck;
    Text error;
    malloc_call = 0;
    fail_malloc_call = 5; // The body.text.set allocation after init, heading, object array, and notes.
    const bool built = example_deck(deck, error);
    fail_malloc_call = -1;
    assert(!built);
}
#endif

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "assets")) test_deleted_images_release_asset_slots();
        else if (!strcmp(argv[1], "slide-assets")) test_nonfinal_slide_deletion_compacts_assets_and_undo_keeps_them();
        else if (!strcmp(argv[1], "undo-selection")) test_undo_restores_live_selection_and_current_page();
#ifdef RP_TEST_TRACK_ALLOCATIONS
        else if (!strcmp(argv[1], "history-allocation")) test_history_commit_does_not_reclone_retained_decks();
        else if (!strcmp(argv[1], "history-failure-atomic")) test_failed_history_commits_preserve_document_and_redo();
        else if (!strcmp(argv[1], "example-allocation")) test_example_deck_propagates_body_text_allocation_failure();
#endif
        else assert(false && "unknown test selector");
        puts("Targeted ReliefPPT model regression passed");
        return 0;
    }
    test_default_deck_layouts_and_slide_lifecycle();
    test_object_ids_text_and_immutable_image_copy_paste();
    test_atomic_rejection_validation_and_binary_text_equality();
    test_all_alignment_and_layer_operations_preserve_selection_order();
    test_noops_history_branch_revisions_and_replace_deck();
    test_update_objects_duplicate_delete_and_selection_pruning();
    test_example_deck_is_self_contained_and_valid();
    test_deleted_images_release_asset_slots();
    test_nonfinal_slide_deletion_compacts_assets_and_undo_keeps_them();
    test_undo_restores_live_selection_and_current_page();
#ifdef RP_TEST_TRACK_ALLOCATIONS
    test_history_commit_does_not_reclone_retained_decks();
    test_failed_history_commits_preserve_document_and_redo();
    test_example_deck_propagates_body_text_allocation_failure();
#endif
    puts("ReliefPPT model tests passed");
    return 0;
}
