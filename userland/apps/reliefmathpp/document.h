#ifndef RELIEFMATHPP_DOCUMENT_H
#define RELIEFMATHPP_DOCUMENT_H
#include "model.h"
namespace rmp {
bool encode(const Document &, Bytes &, Text &error);
bool decode(const char *, size_t, Document &, Text &error);
} // namespace rmp
#endif
