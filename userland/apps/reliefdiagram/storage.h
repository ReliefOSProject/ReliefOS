#ifndef RELIEFDIAGRAM_STORAGE_H
#define RELIEFDIAGRAM_STORAGE_H
#include "../relieftodo/storage.h"
namespace rd {
using rt::Text;
template <class T> using Array = rt::Array<T>;
using Bytes = Array<char>;
constexpr size_t MaxNodes = 512, MaxEdges = 2048,
                 MaxFileBytes = 8 * 1024 * 1024;
} // namespace rd
#endif
