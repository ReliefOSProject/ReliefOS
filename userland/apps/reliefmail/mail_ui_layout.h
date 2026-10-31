#ifndef RELIEFMAIL_MAIL_UI_LAYOUT_H
#define RELIEFMAIL_MAIL_UI_LAYOUT_H

namespace rmail {

struct SearchStripLayout {
    int input_width;
    int used_width;
    bool show_field;
    bool show_scope;
};

inline SearchStripLayout search_strip_layout(int width) {
    const bool scope = width >= 404;
    const bool field = width >= 480;
    const int fixed = 16 + 52 + 58 + 58 + 12 +
        (scope ? 84 + 4 : 0) + (field ? 72 + 4 : 0);
    return {width - fixed, width, field, scope};
}

/* Follow the Office navigation chain past disabled folder containers. The
 * node interface is deliberately tiny so this policy stays host-testable. */
template<class Node>
Node *navigation_next_active(Node *candidate, bool forward) {
    while (candidate && !candidate->active())
        candidate = candidate->navigation_neighbor(forward);
    return candidate;
}

inline bool reader_action_row_fits_inline(int pane_width, int leading_offset,
    int view_width, int fetch_width, int source_width, int gap, int right_margin) {
    if (pane_width < 0 || leading_offset < 0 || view_width < 0 || fetch_width < 0 ||
        source_width < 0 || gap < 0 || right_margin < 0) return false;
    const long long required = static_cast<long long>(leading_offset) + view_width +
        fetch_width + source_width + 2LL * gap + right_margin;
    return required <= pane_width;
}

}

#endif
