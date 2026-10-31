#ifndef RELIEFMATH_DOCUMENT_H
#define RELIEFMATH_DOCUMENT_H

#include "model.h"

namespace rm {

constexpr size_t kMaxDocumentBytes = 16u * 1024u * 1024u;

/** Encodes a validated document as the fixed little-endian .rmath v1 format. */
bool encode_document(const Document &document, Bytes &output, Error &error) noexcept;

/** Decodes one exact .rmath file and commits output only after full validation. */
bool decode_document(const uint8_t *bytes, size_t size, Document &output,
                     Error &error) noexcept;

} // namespace rm

#endif
