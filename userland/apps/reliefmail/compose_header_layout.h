#ifndef RELIEFMAIL_COMPOSE_HEADER_LAYOUT_H
#define RELIEFMAIL_COMPOSE_HEADER_LAYOUT_H

#include <string.h>

namespace rmail {

struct ComposeHeaderVisibility {
    bool cc_visible = false;
    bool bcc_visible = false;
};

struct ComposeHeaderLayout {
    int from_y = 0;
    int to_y = 0;
    int cc_y = -1;
    int bcc_y = -1;
    int subject_y = 0;
    int attachment_y = 0;
    int body_y = 0;
    int body_height = 0;
};

inline void compose_header_visibility_new(ComposeHeaderVisibility &visibility) {
    visibility.cc_visible = false;
    visibility.bcc_visible = false;
}

inline void compose_header_visibility_restore(ComposeHeaderVisibility &visibility,
    const char *cc_value, const char *bcc_value) {
    visibility.cc_visible = cc_value && cc_value[0] != '\0';
    visibility.bcc_visible = bcc_value && bcc_value[0] != '\0';
}

inline bool compose_header_toggle_cc(ComposeHeaderVisibility &visibility, const char *value) {
    if (visibility.cc_visible) {
        if (value && value[0] != '\0') return false;
        visibility.cc_visible = false;
    } else {
        visibility.cc_visible = true;
    }
    return true;
}

inline bool compose_header_toggle_bcc(ComposeHeaderVisibility &visibility, const char *value) {
    if (visibility.bcc_visible) {
        if (value && value[0] != '\0') return false;
        visibility.bcc_visible = false;
    } else {
        visibility.bcc_visible = true;
    }
    return true;
}

inline ComposeHeaderLayout compose_header_layout(int fields_top, int field_height,
    int field_gap, const ComposeHeaderVisibility &visibility, bool has_attachments,
    int window_height, int status_height) {
    ComposeHeaderLayout layout;
    layout.from_y = fields_top;
    int next_y = fields_top + field_height + field_gap;
    layout.to_y = next_y;
    next_y += field_height + field_gap;
    if (visibility.cc_visible) {
        layout.cc_y = next_y;
        next_y += field_height + field_gap;
    }
    if (visibility.bcc_visible) {
        layout.bcc_y = next_y;
        next_y += field_height + field_gap;
    }
    layout.subject_y = next_y;
    layout.attachment_y = layout.subject_y + field_height + 4;
    layout.body_y = layout.attachment_y + (has_attachments ? 46 : 0);
    layout.body_height = window_height - status_height - layout.body_y - 4;
    return layout;
}

} // namespace rmail

#endif
