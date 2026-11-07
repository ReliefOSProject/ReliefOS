#ifndef RELIEFMATH_ERROR_H
#define RELIEFMATH_ERROR_H

#include "storage.h"

namespace rm {

enum class Code : uint8_t {
    None,
    InvalidUtf8,
    Limit,
    Format,
    Version,
    Io,
    Conflict,
    Cancelled,
    Timeout,
    Protocol,
    Unsupported,
    NoMemory,
    Font,
    Parse,
    NotFound
};

struct Error {
    Code code = Code::None;
    size_t begin = 0;
    size_t end = 0;
    Text message;

    void clear() noexcept {
        code = Code::None;
        begin = 0;
        end = 0;
        message.clear();
    }

    void set(Code next_code, const char *next_message,
             size_t next_begin = 0, size_t next_end = 0) noexcept {
        code = next_code;
        begin = next_begin;
        end = next_end;
        message.clear();
        if (next_message) (void)message.set(next_message);
    }
};

} // namespace rm

#endif
