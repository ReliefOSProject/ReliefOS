#include "compose_header_layout.h"
#include <assert.h>

using namespace rmail;

static void test_new_compose_starts_with_optional_rows_collapsed() {
    ComposeHeaderVisibility visibility;
    compose_header_visibility_new(visibility);
    assert(!visibility.cc_visible);
    assert(!visibility.bcc_visible);
}

static void test_empty_optional_rows_can_expand_and_collapse() {
    ComposeHeaderVisibility visibility;
    compose_header_visibility_new(visibility);

    assert(compose_header_toggle_cc(visibility, ""));
    assert(visibility.cc_visible);
    assert(compose_header_toggle_bcc(visibility, ""));
    assert(visibility.bcc_visible);
    assert(compose_header_toggle_cc(visibility, ""));
    assert(!visibility.cc_visible);
    assert(compose_header_toggle_bcc(visibility, ""));
    assert(!visibility.bcc_visible);
}

static void test_restored_and_populated_raw_values_remain_visible() {
    ComposeHeaderVisibility visibility;
    compose_header_visibility_restore(visibility, "", " ");
    assert(!visibility.cc_visible);
    assert(visibility.bcc_visible);
    assert(!compose_header_toggle_bcc(visibility, " "));
    assert(visibility.bcc_visible);

    assert(compose_header_toggle_cc(visibility, "recipient@example.test"));
    assert(visibility.cc_visible);
    assert(!compose_header_toggle_cc(visibility, "recipient@example.test"));
    assert(visibility.cc_visible);
}

static void test_collapsed_rows_save_space_and_minimum_body_remains_readable() {
    ComposeHeaderVisibility collapsed;
    compose_header_visibility_new(collapsed);
    ComposeHeaderVisibility expanded;
    compose_header_visibility_restore(expanded, "cc@example.test", "bcc@example.test");

    const ComposeHeaderLayout short_header = compose_header_layout(
        138, 28, 3, collapsed, true, 540, 28);
    const ComposeHeaderLayout full_header = compose_header_layout(
        138, 28, 3, expanded, true, 540, 28);
    assert(short_header.cc_y == -1 && short_header.bcc_y == -1);
    assert(full_header.cc_y >= 0 && full_header.bcc_y > full_header.cc_y);
    assert(short_header.subject_y < full_header.subject_y);
    assert(short_header.attachment_y < full_header.attachment_y);
    assert(full_header.body_height >= 160);
}

int main() {
    test_new_compose_starts_with_optional_rows_collapsed();
    test_empty_optional_rows_can_expand_and_collapse();
    test_restored_and_populated_raw_values_remain_visible();
    test_collapsed_rows_save_space_and_minimum_body_remains_readable();
}
