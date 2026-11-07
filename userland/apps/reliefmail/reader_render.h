#ifndef RELIEFMAIL_READER_RENDER_H
#define RELIEFMAIL_READER_RENDER_H

#include "tasks.h"

namespace rmail {

constexpr size_t MaxReaderHtmlBytes = 256u * 1024u;

struct ReaderRenderedImage {
    Text content_id;
    uint16_t width = 0;
    uint16_t height = 0;
    Text xpm;
};
using ReaderRenderedImages = BoundedArray<ReaderRenderedImage, MaxReaderInlineImages>;
using ReaderRenderedLinks = BoundedArray<Text, MaxReaderSafeLinks>;

struct ReaderRenderedDocument {
    Text html;
    Text warning;
    ReaderRenderedLinks links;
    ReaderRenderedImages images;
};

/* Sanitizes common HTML, rewrites every link/image to message-local opaque
 * tokens, and decodes only small matching inline PNG parts on the worker. */
bool render_safe_reader_html(const char *html, size_t size,
    const AttachmentList &attachments, ReaderRenderedDocument &document,
    Text &error);

/* Validates the bounded XPM format before a GUI process exposes it to FLTK's
 * file loader. The input must be the complete worker-generated XPM document. */
bool validate_reader_xpm(const char *xpm, size_t size, uint16_t width,
    uint16_t height, Text &error);

}

#endif
