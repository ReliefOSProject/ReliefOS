#ifndef RELIEFDIAGRAM_DOCUMENT_H
#define RELIEFDIAGRAM_DOCUMENT_H
#include "model.h"
namespace rd {
bool encode(const Document &, Bytes &, Text &);
bool decode(const char *, size_t, Document &, Text &);
bool markdown(const Document &, Bytes &, Text &);
} // namespace rd
#endif
