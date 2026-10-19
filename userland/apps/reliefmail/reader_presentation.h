#ifndef RELIEFMAIL_READER_PRESENTATION_H
#define RELIEFMAIL_READER_PRESENTATION_H

#include <string.h>

namespace rmail {

using ReaderNoticeTranslator = const char *(*)(const char *);

inline const char *reader_notice_for_display(const char *warning,
    ReaderNoticeTranslator translate) {
    static const char header_only_notice[] =
        "Only headers are cached; the complete original and body are unavailable offline.";
    static const char mime_notice[] = "Some message content could not be fully interpreted.";
    if (!warning || !translate) return warning;
    if (strcmp(warning, header_only_notice) != 0 && strcmp(warning, mime_notice) != 0)
        return warning;
    const char *localized = translate(warning);
    return localized && *localized ? localized : warning;
}

}

#endif
