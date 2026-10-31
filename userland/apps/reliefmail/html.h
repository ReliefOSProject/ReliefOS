#ifndef RELIEFMAIL_HTML_H
#define RELIEFMAIL_HTML_H

#include "model.h"

namespace rmail {

bool sanitize_html(const char *bytes, size_t length, Text &safe_html,
    Text &warning, Text &error);
/* Fl_Help_View reader output: safe formatting and links with all image
 * resources removed before the widget can invoke its file loaders. */
bool sanitize_html_for_reader(const char *bytes, size_t length, Text &safe_html,
    Text &warning, Text &error);

/* Shared external-navigation policy: only bounded HTTP(S) URLs can leave the
 * reader after the UI shows the exact decoded target. */
bool is_safe_external_web_link(const char *target, size_t length);

}
#endif
