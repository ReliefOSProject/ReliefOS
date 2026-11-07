#include "mail_ui_layout.h"
#include "mail_query_presentation.h"
#include <assert.h>
#include <string.h>

using namespace rmail;

struct NavigationProbe {
    bool enabled = true;
    NavigationProbe *previous = nullptr;
    NavigationProbe *next = nullptr;

    bool active() const { return enabled; }
    NavigationProbe *navigation_neighbor(bool forward) const {
        return forward ? next : previous;
    }
};

static void test_navigation_skips_inactive_rows_in_both_directions() {
    NavigationProbe inbox;
    NavigationProbe container;
    NavigationProbe projects;
    NavigationProbe disabled_leaf;
    inbox.next = &container;
    container.previous = &inbox;
    container.next = &projects;
    projects.previous = &container;
    projects.next = &disabled_leaf;
    disabled_leaf.previous = &projects;
    container.enabled = false;
    disabled_leaf.enabled = false;

    assert(navigation_next_active(&container, true) == &projects);
    assert(navigation_next_active(&container, false) == &inbox);
    assert(navigation_next_active(&disabled_leaf, true) == nullptr);
    assert(navigation_next_active(projects.navigation_neighbor(false), false) == &inbox);
}

static void test_reader_action_row_stacks_before_controls_clip() {
    constexpr int leading = 64;
    constexpr int view = 90;
    constexpr int fetch = 76;
    constexpr int source = 100;
    constexpr int gap = 6;
    constexpr int right_margin = 12;

    assert(!reader_action_row_fits_inline(320, leading, view, fetch, source, gap, right_margin));
    assert(!reader_action_row_fits_inline(341, leading, view, fetch, source, gap, right_margin));
    assert(!reader_action_row_fits_inline(353, leading, view, fetch, source, gap, right_margin));
    assert(reader_action_row_fits_inline(354, leading, view, fetch, source, gap, right_margin));
}

static void test_search_keeps_a_usable_input_at_narrow_and_wide_sizes() {
    const int widths[] = {320, 420, 580, 700, 940, 1220};
    for (int width : widths) {
        const SearchStripLayout layout = search_strip_layout(width);
        assert(layout.input_width >= 120);
        assert(layout.used_width <= width);
        assert(layout.show_scope || !layout.show_field);
    }
    const SearchStripLayout scope_at_minimum = search_strip_layout(404);
    assert(!scope_at_minimum.show_field);
    assert(scope_at_minimum.show_scope);
    assert(scope_at_minimum.input_width == 120);
    assert(search_strip_layout(420).show_scope);
    const SearchStripLayout all_controls_at_minimum = search_strip_layout(480);
    assert(all_controls_at_minimum.show_field && all_controls_at_minimum.show_scope);
    assert(all_controls_at_minimum.input_width == 120);
    assert(search_strip_layout(580).show_field);
    assert(search_strip_layout(640).show_field);
    assert(search_strip_layout(940).show_field);
    assert(search_strip_layout(940).show_scope);
}

static void test_filter_dialog_selection_preserves_query_field_and_scope() {
    const MessageQueryField fields[] = {
        MessageQueryField::All, MessageQueryField::From,
        MessageQueryField::Subject, MessageQueryField::Body,
        MessageQueryField::MessageId
    };
    for (int choice = 0; choice < 5; ++choice) {
        const MessageQueryField field = message_query_field_from_choice(choice);
        assert(field == fields[choice]);
        assert(message_query_field_choice(field) == choice);
    }

    const MessageQueryScope scopes[] = {
        MessageQueryScope::Unified, MessageQueryScope::Account,
        MessageQueryScope::Folder, MessageQueryScope::AllMail
    };
    for (int choice = 0; choice < 4; ++choice) {
        const MessageQueryScope scope = message_query_scope_from_choice(choice);
        assert(scope == scopes[choice]);
        assert(message_query_scope_choice(scope) == choice);
    }

    const QuerySearchSelection selection = search_selection_from_choices(3, 2);
    MessageQuerySpec query;
    query.search_field = MessageQueryField::Subject;
    query.scope = MessageQueryScope::Account;
    query.sort = MessageQuerySort::Sender;
    apply_search_selection(query, selection);
    assert(query.search_field == MessageQueryField::Body);
    assert(query.scope == MessageQueryScope::Folder);
    assert(query.sort == MessageQuerySort::Sender);

    const char *field_labels[] = {"All", "From", "Subject", "Body", "Message-ID"};
    const char *scope_labels[] = {"Unified", "Account", "Folder", "All mail"};
    char summary[128];
    assert(format_search_selection_summary(selection, "Field: %s · Scope: %s",
        field_labels, scope_labels, summary, sizeof(summary)));
    assert(strcmp(summary, "Field: Body · Scope: Folder") == 0);
}

int main() {
    test_search_keeps_a_usable_input_at_narrow_and_wide_sizes();
    test_filter_dialog_selection_preserves_query_field_and_scope();
    test_navigation_skips_inactive_rows_in_both_directions();
    test_reader_action_row_stacks_before_controls_clip();
}
