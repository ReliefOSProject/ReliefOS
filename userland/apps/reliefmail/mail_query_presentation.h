#ifndef RELIEFMAIL_MAIL_QUERY_PRESENTATION_H
#define RELIEFMAIL_MAIL_QUERY_PRESENTATION_H

#include "model.h"
#include <stdio.h>

namespace rmail {

struct QuerySearchSelection {
    MessageQueryField field = MessageQueryField::All;
    MessageQueryScope scope = MessageQueryScope::Unified;
};

inline MessageQueryField message_query_field_from_choice(int choice) {
    switch (choice) {
    case 1: return MessageQueryField::From;
    case 2: return MessageQueryField::Subject;
    case 3: return MessageQueryField::Body;
    case 4: return MessageQueryField::MessageId;
    default: return MessageQueryField::All;
    }
}

inline int message_query_field_choice(MessageQueryField field) {
    switch (field) {
    case MessageQueryField::From: return 1;
    case MessageQueryField::Subject: return 2;
    case MessageQueryField::Body: return 3;
    case MessageQueryField::MessageId: return 4;
    case MessageQueryField::All: return 0;
    }
    return 0;
}

inline MessageQueryScope message_query_scope_from_choice(int choice) {
    switch (choice) {
    case 1: return MessageQueryScope::Account;
    case 2: return MessageQueryScope::Folder;
    case 3: return MessageQueryScope::AllMail;
    default: return MessageQueryScope::Unified;
    }
}

inline int message_query_scope_choice(MessageQueryScope scope) {
    switch (scope) {
    case MessageQueryScope::Account: return 1;
    case MessageQueryScope::Folder: return 2;
    case MessageQueryScope::AllMail: return 3;
    case MessageQueryScope::Unified: return 0;
    }
    return 0;
}

inline QuerySearchSelection search_selection_from_choices(int field, int scope) {
    QuerySearchSelection selection;
    selection.field = message_query_field_from_choice(field);
    selection.scope = message_query_scope_from_choice(scope);
    return selection;
}

inline void apply_search_selection(MessageQuerySpec &query,
    const QuerySearchSelection &selection) {
    query.search_field = selection.field;
    query.scope = selection.scope;
}

inline bool format_search_selection_summary(const QuerySearchSelection &selection,
    const char *format, const char *const *field_labels, const char *const *scope_labels,
    char *output, size_t output_size) {
    if (!output || !output_size) return false;
    output[0] = 0;
    const int field = message_query_field_choice(selection.field);
    const int scope = message_query_scope_choice(selection.scope);
    if (!format || !field_labels || !scope_labels || !field_labels[field] ||
        !scope_labels[scope]) return false;
    const int count = snprintf(output, output_size, format, field_labels[field], scope_labels[scope]);
    return count >= 0 && static_cast<size_t>(count) < output_size;
}

}

#endif
