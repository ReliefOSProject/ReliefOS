#ifndef RELIEFMATH_STORAGE_H
#define RELIEFMATH_STORAGE_H

#include "limits.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace rm {
struct Placement {};
}

inline void *operator new(size_t, void *memory, rm::Placement) noexcept {
    return memory;
}
inline void operator delete(void *, void *, rm::Placement) noexcept {}

namespace rm {
namespace detail {

inline size_t &allocation_failure_countdown() noexcept {
    static size_t countdown = SIZE_MAX;
    return countdown;
}

inline void *allocate(size_t size) noexcept {
#ifdef RM_TESTING
    size_t &countdown = allocation_failure_countdown();
    if (countdown != SIZE_MAX) {
        if (countdown == 0) {
            countdown = SIZE_MAX;
            return nullptr;
        }
        --countdown;
    }
#endif
    return malloc(size ? size : 1);
}

inline bool valid_utf8(const char *bytes, size_t size, size_t *bad_offset = nullptr) noexcept {
    if (!bytes && size) {
        if (bad_offset) *bad_offset = 0;
        return false;
    }
    size_t at = 0;
    while (at < size) {
        const uint8_t first = static_cast<uint8_t>(bytes[at]);
        size_t length = 0;
        uint8_t second_min = 0x80;
        uint8_t second_max = 0xbf;
        if (first == 0) {
            if (bad_offset) *bad_offset = at;
            return false;
        }
        if (first <= 0x7f) {
            ++at;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            if (first == 0xe0) second_min = 0xa0;
            if (first == 0xed) second_max = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            if (first == 0xf0) second_min = 0x90;
            if (first == 0xf4) second_max = 0x8f;
        } else {
            if (bad_offset) *bad_offset = at;
            return false;
        }
        if (length > size - at) {
            if (bad_offset) *bad_offset = at;
            return false;
        }
        const uint8_t second = static_cast<uint8_t>(bytes[at + 1]);
        if (second < second_min || second > second_max) {
            if (bad_offset) *bad_offset = at + 1;
            return false;
        }
        for (size_t n = 2; n < length; ++n) {
            const uint8_t continuation = static_cast<uint8_t>(bytes[at + n]);
            if (continuation < 0x80 || continuation > 0xbf) {
                if (bad_offset) *bad_offset = at + n;
                return false;
            }
        }
        at += length;
    }
    return true;
}

} // namespace detail

#ifdef RM_TESTING
namespace testing {
inline void fail_allocations_after(size_t successful_allocations) noexcept {
    detail::allocation_failure_countdown() = successful_allocations;
}
inline void allow_allocations() noexcept {
    detail::allocation_failure_countdown() = SIZE_MAX;
}
} // namespace testing
#endif

/* Immutable reference-counted UTF-8 text with explicit allocation failure. */
class Text {
    struct Block {
        size_t references;
        size_t size;
        char bytes[1];
    };

    Block *block_ = nullptr;

    void retain() noexcept {
        if (block_) __atomic_add_fetch(&block_->references, size_t(1), __ATOMIC_RELAXED);
    }

    void release() noexcept {
        if (block_ && __atomic_sub_fetch(&block_->references, size_t(1), __ATOMIC_ACQ_REL) == 0)
            free(block_);
        block_ = nullptr;
    }

public:
    Text() = default;
    Text(const Text &other) noexcept : block_(other.block_) { retain(); }
    Text(Text &&other) noexcept : block_(other.block_) { other.block_ = nullptr; }
    ~Text() { release(); }

    Text &operator=(const Text &other) noexcept {
        if (this == &other) return *this;
        if (other.block_) __atomic_add_fetch(&other.block_->references, size_t(1), __ATOMIC_RELAXED);
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

    bool set(const char *bytes, size_t size) noexcept {
        if ((!bytes && size) || size > kMaxTextBytes) return false;
        if (!size) {
            clear();
            return true;
        }
        if (!detail::valid_utf8(bytes, size)) return false;
        const size_t header = offsetof(Block, bytes);
        if (size > SIZE_MAX - header - 1) return false;
        Block *next = static_cast<Block *>(detail::allocate(header + size + 1));
        if (!next) return false;
        next->references = 1;
        next->size = size;
        memcpy(next->bytes, bytes, size);
        next->bytes[size] = '\0';
        release();
        block_ = next;
        return true;
    }

    bool set(const char *bytes) noexcept {
        if (!bytes) return false;
        size_t size = 0;
        while (size <= kMaxTextBytes && bytes[size]) ++size;
        if (size > kMaxTextBytes) return false;
        return set(bytes, size);
    }

    void clear() noexcept { release(); }

    void swap(Text &other) noexcept {
        Block *saved = block_;
        block_ = other.block_;
        other.block_ = saved;
    }

    const char *c_str() const noexcept { return block_ ? block_->bytes : ""; }
    size_t size() const noexcept { return block_ ? block_->size : 0; }
    bool empty() const noexcept { return size() == 0; }

    bool operator==(const Text &other) const noexcept {
        return size() == other.size() && memcmp(c_str(), other.c_str(), size()) == 0;
    }
    bool operator==(const char *other) const noexcept {
        if (!other) return false;
        size_t other_size = 0;
        while (other_size <= kMaxTextBytes && other[other_size]) ++other_size;
        return other_size <= kMaxTextBytes && size() == other_size &&
               memcmp(c_str(), other, size()) == 0;
    }
    bool operator!=(const Text &other) const noexcept { return !(*this == other); }
};

/* Move-only array whose growth and copying report allocation failure. */
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
        Item *saved_data = data_;
        data_ = other.data_;
        other.data_ = saved_data;
        size_t saved = size_;
        size_ = other.size_;
        other.size_ = saved;
        saved = capacity_;
        capacity_ = other.capacity_;
        other.capacity_ = saved;
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    Item *data() noexcept { return data_; }
    const Item *data() const noexcept { return data_; }
    Item &operator[](size_t index) noexcept { return data_[index]; }
    const Item &operator[](size_t index) const noexcept { return data_[index]; }

    bool reserve(size_t requested) noexcept {
        if (requested <= capacity_) return true;
        if (requested > SIZE_MAX / sizeof(Item)) return false;
        size_t next_capacity = capacity_ ? capacity_ : 8;
        while (next_capacity < requested) {
            if (next_capacity > SIZE_MAX / 2) {
                next_capacity = requested;
                break;
            }
            next_capacity *= 2;
        }
        if (next_capacity > SIZE_MAX / sizeof(Item)) next_capacity = requested;
        Item *replacement = static_cast<Item *>(detail::allocate(next_capacity * sizeof(Item)));
        if (!replacement) return false;
        for (size_t index = 0; index < size_; ++index)
            new (replacement + index, Placement{}) Item(static_cast<Item &&>(data_[index]));
        for (size_t index = 0; index < size_; ++index) data_[index].~Item();
        free(data_);
        data_ = replacement;
        capacity_ = next_capacity;
        return true;
    }

    bool resize(size_t requested) noexcept {
        if (requested > size_ && !reserve(requested)) return false;
        while (size_ > requested) data_[--size_].~Item();
        while (size_ < requested) {
            new (data_ + size_, Placement{}) Item();
            ++size_;
        }
        return true;
    }

    bool push(const Item &value) noexcept {
        if (size_ == SIZE_MAX) return false;
        Item saved(value);
        if (!reserve(size_ + 1)) return false;
        new (data_ + size_, Placement{}) Item(static_cast<Item &&>(saved));
        ++size_;
        return true;
    }

    bool push(Item &&value) noexcept {
        if (size_ == SIZE_MAX) return false;
        size_t alias_index = size_;
        if (size_ == capacity_) {
            for (size_t index = 0; index < size_; ++index) {
                if (&value == data_ + index) {
                    alias_index = index;
                    break;
                }
            }
        }
        // Allocate before moving; relocate an element alias after successful growth.
        if (!reserve(size_ + 1)) return false;
        Item &source = alias_index < size_ ? data_[alias_index] : value;
        new (data_ + size_, Placement{}) Item(static_cast<Item &&>(source));
        ++size_;
        return true;
    }

    bool insert(size_t at, const Item &value) noexcept {
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

    void erase(size_t at, size_t count = 1) noexcept {
        if (at >= size_) return;
        if (count > size_ - at) count = size_ - at;
        for (size_t index = at; index + count < size_; ++index)
            data_[index] = static_cast<Item &&>(data_[index + count]);
        while (count--) data_[--size_].~Item();
    }

    bool clone_from(const Array &other) noexcept {
        Array<Item> candidate;
        if (!candidate.reserve(other.size_)) return false;
        for (size_t index = 0; index < other.size_; ++index)
            if (!candidate.push(other.data_[index])) return false;
        swap(candidate);
        return true;
    }

    void clear() noexcept {
        while (size_) data_[--size_].~Item();
    }

    void reset() noexcept {
        clear();
        free(data_);
        data_ = nullptr;
        capacity_ = 0;
    }
};

/* Binary storage with no string terminator semantics. */
class Bytes {
    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;

    bool reserve(size_t requested) noexcept {
        if (requested <= capacity_) return true;
        size_t next_capacity = capacity_ ? capacity_ : 64;
        while (next_capacity < requested) {
            if (next_capacity > SIZE_MAX / 2) {
                next_capacity = requested;
                break;
            }
            next_capacity *= 2;
        }
        uint8_t *replacement = static_cast<uint8_t *>(detail::allocate(next_capacity));
        if (!replacement) return false;
        if (size_) memcpy(replacement, data_, size_);
        free(data_);
        data_ = replacement;
        capacity_ = next_capacity;
        return true;
    }

public:
    Bytes() = default;
    Bytes(const Bytes &) = delete;
    Bytes &operator=(const Bytes &) = delete;
    Bytes(Bytes &&other) noexcept { swap(other); }
    Bytes &operator=(Bytes &&other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }
    ~Bytes() { reset(); }

    void swap(Bytes &other) noexcept {
        uint8_t *saved_data = data_;
        data_ = other.data_;
        other.data_ = saved_data;
        size_t saved = size_;
        size_ = other.size_;
        other.size_ = saved;
        saved = capacity_;
        capacity_ = other.capacity_;
        other.capacity_ = saved;
    }

    uint8_t *data() noexcept { return data_; }
    const uint8_t *data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }

    bool resize(size_t requested) noexcept {
        if (requested > size_ && !reserve(requested)) return false;
        if (requested > size_) memset(data_ + size_, 0, requested - size_);
        size_ = requested;
        return true;
    }

    bool append(const void *bytes, size_t length) noexcept {
        if (!length) return true;
        if (!bytes || length > SIZE_MAX - size_) return false;
        const size_t old_size = size_;
        const uintptr_t start = reinterpret_cast<uintptr_t>(data_);
        const uintptr_t source = reinterpret_cast<uintptr_t>(bytes);
        bool aliases = false;
        size_t offset = 0;
        if (data_ && source >= start && source - start <= size_) {
            offset = static_cast<size_t>(source - start);
            if (length > size_ - offset) return false;
            aliases = true;
        }
        if (!resize(old_size + length)) return false;
        const void *source_after_resize = aliases ? data_ + offset : bytes;
        memmove(data_ + old_size, source_after_resize, length);
        return true;
    }

    bool clone_from(const Bytes &other) noexcept {
        Bytes candidate;
        if (!candidate.append(other.data_, other.size_)) return false;
        swap(candidate);
        return true;
    }

    void clear() noexcept { size_ = 0; }
    void reset() noexcept {
        free(data_);
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }
};

} // namespace rm

#endif
