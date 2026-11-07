#ifndef RELIEFMAIL_STORAGE_H
#define RELIEFMAIL_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

namespace rmail { struct Placement {}; }
inline void *operator new(size_t, void *place, rmail::Placement) noexcept { return place; }
inline void operator delete(void *, void *, rmail::Placement) noexcept {}

namespace rmail {

/*
 * Text and Bytes backing blocks are process-local and thread-confined. Their
 * reference counts are non-atomic: do not copy, destroy, or share handles for
 * one backing block across threads. The GUI is single-threaded, and worker
 * processes exchange serialized bytes; never transfer these handles or backing
 * pointers across a thread or process boundary. Reconstruct values after the
 * receiving side decodes the serialized data.
 */
/* Immutable, reference-counted UTF-8 storage with allocation failure reporting. */
class Text {
    struct Block { size_t refs, size; char bytes[1]; };
    Block *block_ = nullptr;

    void release() {
        if (block_ && !--block_->refs) free(block_);
    }

public:
    Text() = default;
    Text(const Text &other) : block_(other.block_) {
        if (block_) ++block_->refs;
    }
    Text(Text &&other) noexcept : block_(other.block_) { other.block_ = nullptr; }
    ~Text() { release(); }

    Text &operator=(const Text &other) {
        if (other.block_) ++other.block_->refs;
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
        if (!size) {
            clear();
            return true;
        }
        if (!bytes || size > SIZE_MAX - sizeof(Block)) return false;
        Block *next = static_cast<Block *>(malloc(sizeof(Block) + size));
        if (!next) return false;
        next->refs = 1;
        next->size = size;
        memcpy(next->bytes, bytes, size);
        next->bytes[size] = 0;
        release();
        block_ = next;
        return true;
    }

    bool set(const char *bytes) {
        return bytes && set(bytes, strlen(bytes));
    }

    void clear() {
        release();
        block_ = nullptr;
    }

    const char *c_str() const { return block_ ? block_->bytes : ""; }
    size_t size() const { return block_ ? block_->size : 0; }
    bool empty() const { return size() == 0; }

    bool operator==(const Text &other) const {
        return size() == other.size() && (!size() || memcmp(c_str(), other.c_str(), size()) == 0);
    }
    bool operator==(const char *other) const {
        if (!other) return empty();
        const size_t length = strlen(other);
        return size() == length && (!length || memcmp(c_str(), other, length) == 0);
    }
    bool operator!=(const Text &other) const { return !(*this == other); }
    bool operator!=(const char *other) const { return !(*this == other); }
};

/* Binary storage is length-aware and never uses a terminator or string comparison. */
class Bytes {
    struct Block { size_t refs, size; unsigned char bytes[1]; };
    Block *block_ = nullptr;

    void release() {
        if (block_ && !--block_->refs) free(block_);
    }

public:
    Bytes() = default;
    Bytes(const Bytes &other) : block_(other.block_) {
        if (block_) ++block_->refs;
    }
    Bytes(Bytes &&other) noexcept : block_(other.block_) { other.block_ = nullptr; }
    ~Bytes() { release(); }

    Bytes &operator=(const Bytes &other) {
        if (other.block_) ++other.block_->refs;
        release();
        block_ = other.block_;
        return *this;
    }

    Bytes &operator=(Bytes &&other) noexcept {
        if (this != &other) {
            release();
            block_ = other.block_;
            other.block_ = nullptr;
        }
        return *this;
    }

    bool set(const void *bytes, size_t size) {
        if (!size) {
            clear();
            return true;
        }
        if (!bytes || size > SIZE_MAX - sizeof(Block)) return false;
        Block *next = static_cast<Block *>(malloc(sizeof(Block) + size));
        if (!next) return false;
        next->refs = 1;
        next->size = size;
        memcpy(next->bytes, bytes, size);
        release();
        block_ = next;
        return true;
    }

    void clear() {
        release();
        block_ = nullptr;
    }

    const unsigned char *data() const { return block_ ? block_->bytes : nullptr; }
    size_t size() const { return block_ ? block_->size : 0; }
    bool empty() const { return size() == 0; }

    bool operator==(const Bytes &other) const {
        return size() == other.size() && (!size() || memcmp(data(), other.data(), size()) == 0);
    }
    bool operator!=(const Bytes &other) const { return !(*this == other); }
};

/* Explicit failure-returning dynamic storage for no-exceptions builds. */
template<class Item> class Array {
    Item *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;

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
        size_t count = size_;
        size_ = other.size_;
        other.size_ = count;
        count = capacity_;
        capacity_ = other.capacity_;
        other.capacity_ = count;
    }

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    Item *data() { return data_; }
    const Item *data() const { return data_; }
    Item &operator[](size_t index) { return data_[index]; }
    const Item &operator[](size_t index) const { return data_[index]; }

    bool reserve(size_t requested) {
        if (requested <= capacity_) return true;
        if (requested > SIZE_MAX / sizeof(Item)) return false;
        size_t next = capacity_ ? capacity_ : 8;
        while (next < requested) {
            if (next > SIZE_MAX / 2) {
                next = requested;
                break;
            }
            next *= 2;
        }
        if (next > SIZE_MAX / sizeof(Item)) next = requested;
        Item *replacement = static_cast<Item *>(malloc(next * sizeof(Item)));
        if (!replacement) return false;
        for (size_t index = 0; index < size_; ++index)
            new (replacement + index, Placement{}) Item(static_cast<Item &&>(data_[index]));
        for (size_t index = 0; index < size_; ++index) data_[index].~Item();
        free(data_);
        data_ = replacement;
        capacity_ = next;
        return true;
    }

    bool resize(size_t requested) {
        if (!reserve(requested)) return false;
        while (size_ > requested) data_[--size_].~Item();
        while (size_ < requested) {
            new (data_ + size_, Placement{}) Item();
            ++size_;
        }
        return true;
    }

    bool push(const Item &value) {
        if (size_ == SIZE_MAX) return false;
        Item saved(value);
        if (!reserve(size_ + 1)) return false;
        new (data_ + size_, Placement{}) Item(static_cast<Item &&>(saved));
        ++size_;
        return true;
    }

    bool push(Item &&value) {
        if (size_ == SIZE_MAX) return false;
        size_t alias = SIZE_MAX;
        if (data_) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(data_);
            const uintptr_t source = reinterpret_cast<uintptr_t>(&value);
            if (source >= base) {
                const uintptr_t offset = source - base;
                if (offset % sizeof(Item) == 0 && offset / sizeof(Item) < size_)
                    alias = static_cast<size_t>(offset / sizeof(Item));
            }
        }
        if (!reserve(size_ + 1)) return false;
        Item *source = alias == SIZE_MAX ? &value : data_ + alias;
        new (data_ + size_, Placement{}) Item(static_cast<Item &&>(*source));
        ++size_;
        return true;
    }

    bool insert(size_t at, const Item &value) {
        if (at > size_ || size_ == SIZE_MAX) return false;
        Item saved(value);
        if (!reserve(size_ + 1)) return false;
        new (data_ + size_, Placement{}) Item();
        ++size_;
        for (size_t index = size_ - 1; index > at; --index)
            data_[index] = static_cast<Item &&>(data_[index - 1]);
        data_[at] = static_cast<Item &&>(saved);
        return true;
    }

    void erase(size_t at, size_t count = 1) {
        if (at >= size_ || !count) return;
        if (count > size_ - at) count = size_ - at;
        for (size_t index = at; index < size_ - count; ++index)
            data_[index] = static_cast<Item &&>(data_[index + count]);
        while (count--) data_[--size_].~Item();
    }

    bool clone_from(const Array &other) {
        Array next;
        if (!next.reserve(other.size_)) return false;
        for (size_t index = 0; index < other.size_; ++index)
            if (!next.push(other[index])) return false;
        swap(next);
        return true;
    }

    void clear() {
        while (size_) data_[--size_].~Item();
    }

    void reset() {
        clear();
        free(data_);
        data_ = nullptr;
        capacity_ = 0;
    }
};

template<class Item, size_t Limit> class BoundedArray {
    Array<Item> items_;

public:
    BoundedArray() = default;
    BoundedArray(const BoundedArray &) = delete;
    BoundedArray &operator=(const BoundedArray &) = delete;
    BoundedArray(BoundedArray &&other) noexcept : items_(static_cast<Array<Item> &&>(other.items_)) {}
    BoundedArray &operator=(BoundedArray &&other) noexcept {
        items_ = static_cast<Array<Item> &&>(other.items_);
        return *this;
    }

    static constexpr size_t limit() { return Limit; }
    size_t size() const { return items_.size(); }
    size_t capacity() const { return items_.capacity(); }
    bool empty() const { return items_.empty(); }
    Item *data() { return items_.data(); }
    const Item *data() const { return items_.data(); }
    Item &operator[](size_t index) { return items_[index]; }
    const Item &operator[](size_t index) const { return items_[index]; }
    bool reserve(size_t requested) { return requested <= Limit && items_.reserve(requested); }
    bool resize(size_t requested) { return requested <= Limit && items_.resize(requested); }
    bool push(const Item &value) { return size() < Limit && items_.push(value); }
    bool push(Item &&value) { return size() < Limit && items_.push(static_cast<Item &&>(value)); }
    bool insert(size_t at, const Item &value) { return size() < Limit && items_.insert(at, value); }
    void erase(size_t at, size_t count = 1) { items_.erase(at, count); }
    bool clone_from(const BoundedArray &other) {
        if (other.size() > Limit) return false;
        Array<Item> next;
        if (!next.clone_from(other.items_)) return false;
        items_.swap(next);
        return true;
    }
    void clear() { items_.clear(); }
    void reset() { items_.reset(); }
    void swap(BoundedArray &other) noexcept { items_.swap(other.items_); }
};

}
#endif
