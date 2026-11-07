#ifndef RELIEFMATH_LIMITS_H
#define RELIEFMATH_LIMITS_H

#include <stddef.h>
#include <stdint.h>

namespace rm {

constexpr size_t kMaxFormulaCount = 256;
constexpr size_t kMaxNameBytes = 256;
constexpr size_t kMaxTitleBytes = 256;
constexpr size_t kMaxSourceBytes = 64u * 1024u;
constexpr size_t kMaxTextBytes = kMaxSourceBytes;

constexpr uint16_t kMinPointSize = 1;
constexpr uint16_t kMaxPointSize = 512;
constexpr uint16_t kMaxPadding = 4096;

constexpr uint32_t kMaxRasterDimension = 16384;
constexpr uint64_t kMaxRasterPixels = 16u * 1024u * 1024u;

} // namespace rm

#endif
