#ifndef RELIEFMAIL_LOCALIZATION_H
#define RELIEFMAIL_LOCALIZATION_H

#include <libintl.h>
#include <stdio.h>
#include <string.h>

namespace rmail {

/* Diagnostics remain canonical in the worker and storage; translate only
 * when presenting them.  gettext("") returns the catalog header. */
inline const char *localize_diagnostic(const char *message) {
    return message && *message ? gettext(message) : "";
}

/* Keep worker and stored diagnostics intact.  Some include runtime values,
 * so translate only their known fixed parts and retain the original details. */
class DiagnosticForDisplay {
    char buffer_[4096];
    const char *text_;

    void join(const char *message, const char *split, size_t skip, const char *separator) {
        const size_t length = static_cast<size_t>(split - message);
        char prefix[1024];
        if (length >= sizeof(prefix)) return;
        memcpy(prefix, message, length);
        prefix[length] = 0;
        const char *first = localize_diagnostic(prefix);
        const char *second = localize_diagnostic(split + skip);
        const int count = snprintf(buffer_, sizeof(buffer_), "%s%s%s", first, separator, second);
        if (count >= 0 && static_cast<size_t>(count) < sizeof(buffer_)) text_ = buffer_;
    }

public:
    explicit DiagnosticForDisplay(const char *message) : text_(localize_diagnostic(message)) {
        if (!message || !*message) return;
        const char *split = strstr(message, "; SMTP RSET failed: ");
        if (split) {
            static const char delimiter[] = "; SMTP RSET failed: ";
            join(message, split, sizeof(delimiter) - 1, localize_diagnostic(delimiter));
            return;
        }
        split = strstr(message, "; SMTP RSET was rejected (code ");
        if (split) {
            static const char delimiter[] = "; SMTP RSET was rejected (code ";
            join(message, split, sizeof(delimiter) - 1, localize_diagnostic(delimiter));
            return;
        }
        if (!strncmp(message, "[TLS:", 5)) split = strstr(message, " (TLS=");
        else if (!strncmp(message, "[CA:invalid] no usable CA certificates",
                     sizeof("[CA:invalid] no usable CA certificates") - 1))
            split = strstr(message, " (parse=");
        else if (!strncmp(message, "[CA:unavailable] ", 17)) {
            split = strstr(message, " CA file:");
            if (split) split += sizeof(" CA file:") - 1;
        }
        else split = nullptr;
        if (split) join(message, split, 0, "");
    }
    DiagnosticForDisplay(const DiagnosticForDisplay &) = delete;
    DiagnosticForDisplay &operator=(const DiagnosticForDisplay &) = delete;
    const char *c_str() const { return text_; }
};

}

#endif
