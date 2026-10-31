#ifndef NETWORK_CONTROLLER_STORAGE_H
#define NETWORK_CONTROLLER_STORAGE_H
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace nc {
struct Placement {};
} // namespace nc
inline void *operator new(size_t, void *p, nc::Placement) noexcept { return p; }
inline void operator delete(void *, void *, nc::Placement) noexcept {}

namespace nc {
/* Immutable, reference-counted UTF-8 bytes. Copies cannot allocate or fail. */
class Text {
  struct Block {
    size_t refs, size;
    char bytes[1];
  };
  Block *block_ = nullptr;
  void release() {
    if (block_ && !--block_->refs)
      free(block_);
  }

public:
  Text() = default;
  Text(const Text &other) : block_(other.block_) {
    if (block_)
      ++block_->refs;
  }
  Text(Text &&other) noexcept : block_(other.block_) { other.block_ = nullptr; }
  ~Text() { release(); }
  Text &operator=(const Text &other) {
    if (other.block_)
      ++other.block_->refs;
    release();
    block_ = other.block_;
    return *this;
  }
  Text &operator=(Text &&other) noexcept {
    if (this != &other) {
      release();
      block_ = other.block_;
      other.block_ = nullptr;
    }
    return *this;
  }
  bool set(const char *bytes, size_t size) {
    if (!bytes || size > SIZE_MAX - sizeof(Block))
      return false;
    if (!size) {
      clear();
      return true;
    }
    Block *next = static_cast<Block *>(malloc(sizeof(Block) + size));
    if (!next)
      return false;
    next->refs = 1;
    next->size = size;
    memcpy(next->bytes, bytes, size);
    next->bytes[size] = 0;
    release();
    block_ = next;
    return true;
  }
  bool set(const char *bytes) { return bytes && set(bytes, strlen(bytes)); }
  void clear() {
    release();
    block_ = nullptr;
  }
  const char *c_str() const { return block_ ? block_->bytes : ""; }
  size_t size() const { return block_ ? block_->size : 0; }
  bool empty() const { return !size(); }
  bool operator==(const Text &other) const {
    return !strcmp(c_str(), other.c_str());
  }
  bool operator==(const char *other) const {
    return other && !strcmp(c_str(), other);
  }
  bool operator!=(const Text &other) const { return !(*this == other); }
};

/* Explicit failure-returning storage, compatible with the no-exceptions ABI. */
template <class Item> class Array {
  Item *data_ = nullptr;
  size_t size_ = 0, capacity_ = 0;

public:
  Array() = default;
  Array(const Array &) = delete;
  Array &operator=(const Array &) = delete;
  Array(Array &&other) noexcept { swap(other); }
  Array &operator=(Array &&other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }
  ~Array() { reset(); }
  void swap(Array &other) noexcept {
    Item *data = data_;
    data_ = other.data_;
    other.data_ = data;
    size_t n = size_;
    size_ = other.size_;
    other.size_ = n;
    n = capacity_;
    capacity_ = other.capacity_;
    other.capacity_ = n;
  }
  size_t size() const { return size_; }
  bool empty() const { return !size_; }
  Item *data() { return data_; }
  const Item *data() const { return data_; }
  Item &operator[](size_t n) { return data_[n]; }
  const Item &operator[](size_t n) const { return data_[n]; }
  bool reserve(size_t requested) {
    if (requested <= capacity_)
      return true;
    if (requested > SIZE_MAX / sizeof(Item))
      return false;
    size_t next = capacity_ ? capacity_ : 8;
    while (next < requested) {
      if (next > SIZE_MAX / 2) {
        next = requested;
        break;
      }
      next *= 2;
    }
    if (next > SIZE_MAX / sizeof(Item))
      next = requested;
    Item *replacement = static_cast<Item *>(malloc(next * sizeof(Item)));
    if (!replacement)
      return false;
    for (size_t n = 0; n < size_; ++n)
      new (replacement + n, Placement{}) Item(static_cast<Item &&>(data_[n]));
    for (size_t n = 0; n < size_; ++n)
      data_[n].~Item();
    free(data_);
    data_ = replacement;
    capacity_ = next;
    return true;
  }
  bool resize(size_t size) {
    if (!reserve(size))
      return false;
    while (size_ > size)
      data_[--size_].~Item();
    while (size_ < size) {
      new (data_ + size_, Placement{}) Item();
      ++size_;
    }
    return true;
  }
  bool push(const Item &value) {
    Item saved(value);
    if (!reserve(size_ + 1))
      return false;
    new (data_ + size_, Placement{}) Item(static_cast<Item &&>(saved));
    ++size_;
    return true;
  }
  bool push(Item &&value) {
    Item saved(static_cast<Item &&>(value));
    if (!reserve(size_ + 1))
      return false;
    new (data_ + size_, Placement{}) Item(static_cast<Item &&>(saved));
    ++size_;
    return true;
  }
  bool insert(size_t at, const Item &value) {
    Item saved(value);
    if (at > size_ || !reserve(size_ + 1))
      return false;
    new (data_ + size_, Placement{}) Item();
    ++size_;
    for (size_t n = size_ - 1; n > at; --n)
      data_[n] = static_cast<Item &&>(data_[n - 1]);
    data_[at] = static_cast<Item &&>(saved);
    return true;
  }
  void erase(size_t at, size_t count = 1) {
    if (at >= size_)
      return;
    if (count > size_ - at)
      count = size_ - at;
    for (size_t n = at; n + count < size_; ++n)
      data_[n] = static_cast<Item &&>(data_[n + count]);
    while (count--)
      data_[--size_].~Item();
  }
  bool clone_from(const Array &other) {
    Array next;
    if (!next.reserve(other.size_))
      return false;
    for (size_t n = 0; n < other.size_; ++n)
      if (!next.push(other[n]))
        return false;
    swap(next);
    return true;
  }
  void clear() {
    while (size_)
      data_[--size_].~Item();
  }
  void reset() {
    clear();
    free(data_);
    data_ = nullptr;
    capacity_ = 0;
  }
};
} // namespace nc
#endif
