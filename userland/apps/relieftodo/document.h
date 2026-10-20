#ifndef RELIEFTODO_DOCUMENT_H
#define RELIEFTODO_DOCUMENT_H
#include "model.h"
namespace rt {
using Bytes = Array<char>;
bool encode_document(const Document &, Bytes &, Text &error);
bool decode_document(const char *, size_t, Document &, Text &error);
// Exports preserve UTF-8. CSV cells beginning with spreadsheet formula markers
// are prefixed with an apostrophe so opening a task cannot execute a formula.
bool export_csv(const Document &, Bytes &, Text &error);
bool export_markdown(const Document &, Bytes &, Text &error);
}
#endif
