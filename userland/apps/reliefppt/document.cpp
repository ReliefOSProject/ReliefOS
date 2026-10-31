#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "document.h"

#include <errno.h>
#include <dirent.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rp {
namespace {

constexpr size_t IoQuantum = 32u * 1024u;
constexpr size_t MaxAtomicWriteBytes = 256u * 1024u * 1024u;
constexpr size_t MaxPathBytes = 4000;
constexpr size_t MaxComponentBytes = 255;
constexpr size_t HeaderBytes = 16;
constexpr uint16_t NativeVersion = 1;
constexpr uint8_t ObjectStyleFlags = 0x3f;

static_assert(sizeof(float) == 4, "ReliefPPT v1 requires 32-bit float");
static_assert(sizeof(float) == sizeof(uint32_t), "ReliefPPT v1 requires 32-bit float");
static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MIN_EXP == -125 && FLT_MAX_EXP == 128,
    "ReliefPPT v1 requires IEEE-754 binary32 float");

bool set_error(Text &error, const char *message) {
    if (!error.set(message, strlen(message))) error.clear();
    return false;
}

bool set_errno_error(Text &error, const char *operation) {
    const int saved = errno;
    const char *detail = strerror(saved);
    char message[320];
    int n = snprintf(message, sizeof(message), "%s：%s", operation,
        detail ? detail : "I/O 错误");
    errno = saved;
    if (n < 0 || static_cast<size_t>(n) >= sizeof(message)) return set_error(error, operation);
    return set_error(error, message);
}

void store_u16(unsigned char *at, uint16_t value) {
    at[0] = static_cast<unsigned char>(value);
    at[1] = static_cast<unsigned char>(value >> 8);
}

void store_u32(unsigned char *at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) at[i] = static_cast<unsigned char>(value >> (i * 8));
}

void store_u64(unsigned char *at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) at[i] = static_cast<unsigned char>(value >> (i * 8));
}

uint16_t load_u16(const unsigned char *at) {
    return static_cast<uint16_t>(at[0]) | static_cast<uint16_t>(uint16_t(at[1]) << 8);
}

uint32_t load_u32(const unsigned char *at) {
    return uint32_t(at[0]) | (uint32_t(at[1]) << 8) | (uint32_t(at[2]) << 16) |
        (uint32_t(at[3]) << 24);
}

uint64_t load_u64(const unsigned char *at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(at[i]) << (i * 8);
    return value;
}

uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

struct ByteBuffer {
    char *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;

    ~ByteBuffer() { free(data); }

    bool reserve(size_t requested) {
        if (requested <= capacity) return true;
        if (requested > MaxFileBytes) return false;
        size_t grown = capacity ? capacity : 512;
        while (grown < requested) {
            if (grown > MaxFileBytes / 2) { grown = requested; break; }
            grown *= 2;
        }
        if (grown > MaxFileBytes) grown = requested;
        char *replacement = static_cast<char *>(realloc(data, grown));
        if (!replacement) return false;
        data = replacement;
        capacity = grown;
        return true;
    }

    bool append(const void *bytes, size_t length) {
        if (length > MaxFileBytes - size || !reserve(size + length)) return false;
        if (length) memcpy(data + size, bytes, length);
        size += length;
        return true;
    }

    bool u8(uint8_t value) { return append(&value, 1); }
    bool u16(uint16_t value) {
        unsigned char bytes[2];
        store_u16(bytes, value);
        return append(bytes, sizeof(bytes));
    }
    bool u32(uint32_t value) {
        unsigned char bytes[4];
        store_u32(bytes, value);
        return append(bytes, sizeof(bytes));
    }
    bool u64(uint64_t value) {
        unsigned char bytes[8];
        store_u64(bytes, value);
        return append(bytes, sizeof(bytes));
    }
    bool f32(float value) { return u32(float_bits(value)); }
    bool string(const Text &text) {
        return text.size() <= UINT32_MAX && u32(static_cast<uint32_t>(text.size())) &&
            append(text.c_str(), text.size());
    }
};

bool append_object(ByteBuffer &bytes, const Object &object) {
    const uint8_t flags = (object.style.bold ? 1u : 0u) |
        (object.style.italic ? 2u : 0u) | (object.style.underline ? 4u : 0u) |
        (object.style.bullets ? 8u : 0u) | (object.style.filled ? 16u : 0u) |
        (object.style.outlined ? 32u : 0u);
    return bytes.u32(object.id) && bytes.u8(static_cast<uint8_t>(object.kind)) &&
        bytes.u8(object.style.family) && bytes.u8(flags) &&
        bytes.u8(static_cast<uint8_t>(object.style.alignment)) &&
        bytes.f32(object.rect.x) && bytes.f32(object.rect.y) &&
        bytes.f32(object.rect.w) && bytes.f32(object.rect.h) &&
        bytes.u32(object.style.text) && bytes.u32(object.style.fill) &&
        bytes.u32(object.style.stroke) && bytes.f32(object.style.font_size) &&
        bytes.f32(object.style.line_width) && bytes.u32(object.asset) && bytes.string(object.text);
}

bool encode_native(const Deck &deck, Text &output, Text &error) {
    if (!deck.validate(error)) return false;
    ByteBuffer bytes;
    unsigned char header[HeaderBytes] = {'R', 'P', 'P', 'T', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!bytes.append(header, sizeof(header)) || !bytes.f32(deck.width) ||
        !bytes.f32(deck.height) || !bytes.u32(deck.next_id) || !bytes.u64(deck.revision) ||
        !bytes.string(deck.title) || !bytes.string(deck.author) ||
        deck.assets.size() > UINT32_MAX || !bytes.u32(static_cast<uint32_t>(deck.assets.size())))
        return set_error(error, "内存不足或演示文稿超出文件大小限制");
    for (size_t i = 0; i < deck.assets.size(); ++i) {
        const Asset &asset = deck.assets[i];
        if (asset.rgba.size() > UINT32_MAX || !bytes.u32(asset.width) || !bytes.u32(asset.height) ||
            !bytes.u32(static_cast<uint32_t>(asset.rgba.size())) ||
            !bytes.append(asset.rgba.c_str(), asset.rgba.size()))
            return set_error(error, "内存不足或图片资源超出文件大小限制");
    }
    if (deck.slides.size() > UINT32_MAX || !bytes.u32(static_cast<uint32_t>(deck.slides.size())))
        return set_error(error, "内存不足或幻灯片超出文件大小限制");
    for (size_t s = 0; s < deck.slides.size(); ++s) {
        const Slide &slide = deck.slides[s];
        if (slide.objects.size() > UINT32_MAX || !bytes.u32(slide.background) ||
            !bytes.string(slide.notes) || !bytes.u32(static_cast<uint32_t>(slide.objects.size())))
            return set_error(error, "内存不足或幻灯片内容超出文件大小限制");
        for (size_t i = 0; i < slide.objects.size(); ++i)
            if (!append_object(bytes, slide.objects[i]))
                return set_error(error, "内存不足或幻灯片对象超出文件大小限制");
    }
    if (bytes.size < HeaderBytes || bytes.size > MaxFileBytes)
        return set_error(error, "演示文稿文件超过 96 MiB 限制");
    memcpy(bytes.data, "RPPT", 4);
    store_u16(reinterpret_cast<unsigned char *>(bytes.data) + 4, NativeVersion);
    store_u16(reinterpret_cast<unsigned char *>(bytes.data) + 6, 0);
    store_u64(reinterpret_cast<unsigned char *>(bytes.data) + 8,
        static_cast<uint64_t>(bytes.size - HeaderBytes));
    if (!output.set(bytes.data, bytes.size))
        return set_error(error, "内存不足，无法返回演示文稿数据");
    error.clear();
    return true;
}

struct Reader {
    const unsigned char *bytes = nullptr;
    size_t size = 0;
    size_t at = 0;
    Text *error = nullptr;

    bool read(void *target, size_t count) {
        if (at > size || count > size - at) return set_error(*error, "演示文稿文件已截断");
        if (count) memcpy(target, bytes + at, count);
        at += count;
        return true;
    }
    bool u8(uint8_t &value) { return read(&value, 1); }
    bool u16(uint16_t &value) {
        unsigned char b[2];
        if (!read(b, sizeof(b))) return false;
        value = load_u16(b);
        return true;
    }
    bool u32(uint32_t &value) {
        unsigned char b[4];
        if (!read(b, sizeof(b))) return false;
        value = load_u32(b);
        return true;
    }
    bool u64(uint64_t &value) {
        unsigned char b[8];
        if (!read(b, sizeof(b))) return false;
        value = load_u64(b);
        return true;
    }
    bool f32(float &value) {
        uint32_t bits;
        if (!u32(bits)) return false;
        value = bits_float(bits);
        return true;
    }
    bool string(Text &value, size_t limit, const char *description) {
        uint32_t length;
        if (!u32(length)) return false;
        if (length > limit || at > size || length > size - at)
            return set_error(*error, description);
        const char *source = reinterpret_cast<const char *>(bytes + at);
        if (!valid_utf8(source, length)) return set_error(*error, "演示文稿包含无效的 UTF-8 文本");
        if (!value.set(source, length)) return set_error(*error, "内存不足，无法读取演示文稿文本");
        at += length;
        return true;
    }
};

bool read_object(Reader &reader, Object &object) {
    uint8_t kind, family, flags, alignment;
    if (!reader.u32(object.id) || !reader.u8(kind) || !reader.u8(family) ||
        !reader.u8(flags) || !reader.u8(alignment)) return false;
    if (kind > static_cast<uint8_t>(Kind::Image) || family > 2 ||
        (flags & static_cast<uint8_t>(~ObjectStyleFlags)) ||
        alignment > static_cast<uint8_t>(Alignment::Right))
        return set_error(*reader.error, "演示文稿包含未知对象类型或样式值");
    object.kind = static_cast<Kind>(kind);
    object.style.family = family;
    object.style.bold = (flags & 1u) != 0;
    object.style.italic = (flags & 2u) != 0;
    object.style.underline = (flags & 4u) != 0;
    object.style.bullets = (flags & 8u) != 0;
    object.style.filled = (flags & 16u) != 0;
    object.style.outlined = (flags & 32u) != 0;
    object.style.alignment = static_cast<Alignment>(alignment);
    if (!reader.f32(object.rect.x) || !reader.f32(object.rect.y) ||
        !reader.f32(object.rect.w) || !reader.f32(object.rect.h) ||
        !reader.u32(object.style.text) || !reader.u32(object.style.fill) ||
        !reader.u32(object.style.stroke) || !reader.f32(object.style.font_size) ||
        !reader.f32(object.style.line_width) || !reader.u32(object.asset) ||
        !reader.string(object.text, MaxTextBytes, "对象文本超过长度限制"))
        return false;
    return true;
}

bool decode_native(const char *input, size_t length, Deck &output, Text &error) {
    if (!input || length < HeaderBytes || length > MaxFileBytes)
        return set_error(error, "演示文稿文件大小无效");
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(input);
    if (memcmp(raw, "RPPT", 4) || load_u16(raw + 6) != 0 ||
        load_u64(raw + 8) != static_cast<uint64_t>(length - HeaderBytes))
        return set_error(error, "演示文稿签名、标志或长度无效");
    if (load_u16(raw + 4) != NativeVersion)
        return set_error(error, "不支持此原生演示文稿版本");

    Deck candidate;
    if (!candidate.init()) return set_error(error, "内存不足，无法创建演示文稿");
    Reader reader{raw + HeaderBytes, length - HeaderBytes, 0, &error};
    if (!reader.f32(candidate.width) || !reader.f32(candidate.height) ||
        !reader.u32(candidate.next_id) || !reader.u64(candidate.revision) ||
        !reader.string(candidate.title, MaxTextBytes, "演示文稿标题超过长度限制") ||
        !reader.string(candidate.author, MaxTextBytes, "演示文稿作者字段超过长度限制"))
        return false;

    uint32_t count;
    if (!reader.u32(count)) return false;
    if (count > MaxAssets || count > (reader.size - reader.at) / 12)
        return set_error(error, "演示文稿图片数量无效");
    if (!candidate.assets.resize(0) || !candidate.assets.reserve(count))
        return set_error(error, "内存不足，无法读取演示文稿图片");
    uint64_t asset_total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Asset asset;
        uint32_t length_bytes;
        if (!reader.u32(asset.width) || !reader.u32(asset.height) || !reader.u32(length_bytes))
            return false;
        if (!asset.width || !asset.height || asset.width > 4096 || asset.height > 4096)
            return set_error(error, "演示文稿图片尺寸无效");
        const uint64_t expected_bytes = uint64_t(asset.width) * uint64_t(asset.height) * 4u;
        if (expected_bytes > MaxAssetBytes || expected_bytes != length_bytes ||
            expected_bytes > MaxAssetBytes - asset_total || reader.at > reader.size ||
            length_bytes > reader.size - reader.at)
            return set_error(error, "演示文稿图片数据长度无效");
        asset_total += expected_bytes;
        if (!asset.rgba.set(reinterpret_cast<const char *>(reader.bytes + reader.at), length_bytes))
            return set_error(error, "内存不足，无法读取图片像素");
        reader.at += length_bytes;
        if (!candidate.assets.push(static_cast<Asset &&>(asset)))
            return set_error(error, "内存不足，无法保存演示文稿图片");
    }

    if (!reader.u32(count)) return false;
    if (!count || count > MaxSlides || count > (reader.size - reader.at) / 12)
        return set_error(error, "演示文稿幻灯片数量无效");
    if (!candidate.slides.resize(0) || !candidate.slides.reserve(count))
        return set_error(error, "内存不足，无法读取演示文稿幻灯片");
    size_t object_total = 0;
    for (uint32_t s = 0; s < count; ++s) {
        Slide slide;
        uint32_t object_count;
        if (!reader.u32(slide.background) ||
            !reader.string(slide.notes, MaxNotesBytes, "幻灯片备注超过长度限制") ||
            !reader.u32(object_count)) return false;
        if (object_count > MaxObjectsPerSlide || object_count > MaxObjects - object_total ||
            object_count > (reader.size - reader.at) / 52)
            return set_error(error, "演示文稿对象数量无效");
        object_total += object_count;
        if (!slide.objects.reserve(object_count))
            return set_error(error, "内存不足，无法读取幻灯片对象");
        for (uint32_t i = 0; i < object_count; ++i) {
            Object object;
            if (!read_object(reader, object)) return false;
            if (!slide.objects.push(static_cast<Object &&>(object)))
                return set_error(error, "内存不足，无法保存幻灯片对象");
        }
        if (!candidate.slides.push(static_cast<Slide &&>(slide)))
            return set_error(error, "内存不足，无法保存演示文稿幻灯片");
    }
    if (reader.at != reader.size) return set_error(error, "演示文稿文件包含多余数据");
    if (!candidate.validate(error)) return false;
    output.swap(candidate);
    error.clear();
    return true;
}

FileStamp stamp_from_stat(const struct stat &st) {
    FileStamp stamp;
    stamp.exists = true;
    stamp.device = static_cast<uint64_t>(st.st_dev);
    stamp.inode = static_cast<uint64_t>(st.st_ino);
    stamp.size = st.st_size < 0 ? 0 : static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    stamp.seconds = static_cast<int64_t>(st.st_mtimespec.tv_sec);
    stamp.nanoseconds = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
    stamp.change_seconds = static_cast<int64_t>(st.st_ctimespec.tv_sec);
    stamp.change_nanoseconds = static_cast<int64_t>(st.st_ctimespec.tv_nsec);
#else
    stamp.seconds = static_cast<int64_t>(st.st_mtim.tv_sec);
    stamp.nanoseconds = static_cast<int64_t>(st.st_mtim.tv_nsec);
    stamp.change_seconds = static_cast<int64_t>(st.st_ctim.tv_sec);
    stamp.change_nanoseconds = static_cast<int64_t>(st.st_ctim.tv_nsec);
#endif
    return stamp;
}

bool stat_is_regular(const struct stat &st) { return S_ISREG(st.st_mode) && st.st_size >= 0; }

bool file_stamp_at(int directory, const char *base, FileStamp &stamp, Text &error) {
    stamp = FileStamp{};
    struct stat st;
    if (fstatat(directory, base, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) { error.clear(); return true; }
        return set_errno_error(error, "无法检查文件");
    }
    if (!stat_is_regular(st)) return set_error(error, "文件时间戳仅支持非符号链接的普通文件");
    stamp = stamp_from_stat(st);
    error.clear();
    return true;
}

bool split_path(const char *path, char *directory, size_t directory_capacity,
                const char *&base, Text &error) {
    if (!path || !*path) return set_error(error, "文件路径为空");
    size_t length = strlen(path);
    if (length > MaxPathBytes) return set_error(error, "文件路径过长");
    const char *slash = strrchr(path, '/');
    base = slash ? slash + 1 : path;
    size_t base_length = strlen(base);
    if (!base_length || base_length > MaxComponentBytes || !strcmp(base, ".") || !strcmp(base, ".."))
        return set_error(error, "文件名无效或过长");
    size_t directory_length;
    if (!slash) {
        directory_length = 1;
        if (directory_capacity < 2) return set_error(error, "文件路径过长");
        memcpy(directory, ".", 2);
    } else if (slash == path) {
        directory_length = 1;
        if (directory_capacity < 2) return set_error(error, "文件路径过长");
        memcpy(directory, "/", 2);
    } else {
        directory_length = static_cast<size_t>(slash - path);
        if (directory_length + 1 > directory_capacity) return set_error(error, "文件路径过长");
        memcpy(directory, path, directory_length);
        directory[directory_length] = 0;
    }
    (void)directory_length;
    error.clear();
    return true;
}

int open_parent(const char *path, const char *&base, Text &error) {
    char directory[MaxPathBytes + 1];
    if (!split_path(path, directory, sizeof(directory), base, error)) return -1;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) set_errno_error(error, "无法打开目标目录");
    return fd;
}

bool write_all(int fd, const char *bytes, size_t length, Text &error, const char *operation) {
    size_t offset = 0;
    while (offset < length) {
        size_t request = length - offset;
        if (request > IoQuantum) request = IoQuantum;
        ssize_t written = write(fd, bytes + offset, request);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return set_errno_error(error, operation);
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool copy_rollback(int directory, const char *base, const FileStamp &expected,
                   char *backup_name, size_t backup_capacity, bool &backup_exists,
                   Text &error) {
    backup_exists = false;
    int source = openat(directory, base, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0) return set_errno_error(error, "无法打开现有保存文件");
    struct stat before;
    if (fstat(source, &before) < 0) {
        int saved = errno; close(source); errno = saved;
        return set_errno_error(error, "无法检查现有保存文件");
    }
    if (!stat_is_regular(before) || !same_stamp(expected, stamp_from_stat(before))) {
        close(source);
        return set_error(error, "文件已在 ReliefPPT 外部更改，已保留原文件");
    }
    if (static_cast<uint64_t>(before.st_size) > MaxAtomicWriteBytes) {
        close(source);
        return set_error(error, "现有输出文件超过 256 MiB 回滚保护限制");
    }

    int backup = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        static uint64_t serial = 0;
        uint64_t id = ++serial;
        int n = snprintf(backup_name, backup_capacity, ".rppt-old-%ld-%llu",
            static_cast<long>(getpid()), static_cast<unsigned long long>(id));
        if (n < 0 || static_cast<size_t>(n) >= backup_capacity) break;
        backup = openat(directory, backup_name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (backup >= 0) break;
        if (errno != EEXIST) break;
    }
    if (backup < 0) {
        int saved = errno; close(source); errno = saved;
        return set_errno_error(error, "无法创建原文件回滚副本");
    }
    backup_exists = true;
    bool ok = true;
    unsigned char buffer[IoQuantum];
    size_t remaining = static_cast<size_t>(before.st_size);
    while (ok && remaining) {
        size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t got = read(source, buffer, request);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (!got) set_error(error, "保存原文件时发现文件已被截断");
            else set_errno_error(error, "无法读取现有保存文件");
            ok = false;
            break;
        }
        if (!write_all(backup, reinterpret_cast<const char *>(buffer),
                       static_cast<size_t>(got), error, "无法写入原文件回滚副本")) {
            ok = false;
            break;
        }
        remaining -= static_cast<size_t>(got);
    }
    struct stat after;
    if (ok && fstat(source, &after) < 0) {
        set_errno_error(error, "无法重新检查现有保存文件");
        ok = false;
    } else if (ok && !same_stamp(expected, stamp_from_stat(after))) {
        set_error(error, "文件已在 ReliefPPT 外部更改，已保留原文件");
        ok = false;
    }
    if (ok) {
        struct stat named;
        if (fstatat(directory, base, &named, AT_SYMLINK_NOFOLLOW) < 0) {
            set_errno_error(error, "无法重新检查现有保存文件");
            ok = false;
        } else if (!same_stamp(expected, stamp_from_stat(named))) {
            set_error(error, "文件已在 ReliefPPT 外部更改，已保留原文件");
            ok = false;
        }
    }
    if (ok && fchmod(backup, before.st_mode & 07777) < 0) {
        set_errno_error(error, "无法保留原文件权限");
        ok = false;
    }
    if (ok && fsync(backup) < 0) {
        set_errno_error(error, "无法同步原文件回滚副本");
        ok = false;
    }
    int saved = errno;
    if (close(source) < 0 && ok) {
        set_errno_error(error, "无法关闭现有保存文件");
        ok = false;
    }
    if (close(backup) < 0 && ok) {
        set_errno_error(error, "无法关闭原文件回滚副本");
        ok = false;
    } else errno = saved;
    if (!ok) {
        unlinkat(directory, backup_name, 0);
        backup_exists = false;
        return false;
    }
    return true;
}

bool make_unique_file(int directory, const char *prefix, char *name, size_t capacity,
                      int &fd, Text &error) {
    fd = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        static uint64_t serial = 0;
        uint64_t id = ++serial;
        int n = snprintf(name, capacity, "%s-%ld-%llu", prefix,
            static_cast<long>(getpid()), static_cast<unsigned long long>(id));
        if (n < 0 || static_cast<size_t>(n) >= capacity)
            return set_error(error, "临时文件名过长");
        fd = openat(directory, name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) return true;
        if (errno != EEXIST) return set_errno_error(error, "无法创建临时文件");
    }
    return set_error(error, "无法分配唯一临时文件名");
}

bool safe_current_stamp(int directory, const char *base, const FileStamp &initial, Text &error) {
    FileStamp latest;
    if (!file_stamp_at(directory, base, latest, error)) return false;
    if (!same_stamp(initial, latest))
        return set_error(error, "文件已在 ReliefPPT 外部更改，已保留原文件");
    return true;
}

bool renameat2_can_fallback(int error_number) {
    if (error_number == ENOSYS || error_number == EINVAL) return true;
#ifdef EOPNOTSUPP
    if (error_number == EOPNOTSUPP) return true;
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
    if (error_number == ENOTSUP) return true;
#endif
    return false;
}

int rename_without_replace(int directory, const char *from, const char *to, bool &renamed) {
    renamed = false;
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    if (syscall(SYS_renameat2, directory, from, directory, to, RENAME_NOREPLACE) == 0) {
        renamed = true;
        return 0;
    }
    const int rename_error = errno;
    if (!renameat2_can_fallback(rename_error)) {
        errno = rename_error;
        return -1;
    }
#endif
    if (linkat(directory, from, directory, to, 0) < 0) return -1;
    if (unlinkat(directory, from, 0) == 0) renamed = true;
    return 0;
}

bool valid_absolute_directory(const char *path) {
    return path && path[0] == '/' && strlen(path) <= MaxPathBytes;
}

int walk_directory(const char *path, bool create, Text &error) {
    if (!valid_absolute_directory(path)) {
        set_error(error, "状态目录必须使用绝对路径");
        return -1;
    }
    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) {
        set_errno_error(error, "无法打开文件系统根目录");
        return -1;
    }
    const char *at = path + 1;
    while (*at) {
        while (*at == '/') ++at;
        if (!*at) break;
        const char *end = at;
        while (*end && *end != '/') ++end;
        size_t length = static_cast<size_t>(end - at);
        if (!length || length > MaxComponentBytes ||
            (length == 1 && at[0] == '.') || (length == 2 && at[0] == '.' && at[1] == '.')) {
            close(current);
            set_error(error, "状态目录包含无效路径段");
            return -1;
        }
        char component[MaxComponentBytes + 1];
        memcpy(component, at, length);
        component[length] = 0;
        int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(current, component, 0700) < 0 && errno != EEXIST) {
                int saved = errno; close(current); errno = saved;
                set_errno_error(error, "无法创建状态目录");
                return -1;
            }
            next = openat(current, component,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (next < 0) {
            int saved = errno; close(current); errno = saved;
            set_errno_error(error, "状态路径包含符号链接或非目录文件");
            return -1;
        }
        close(current);
        current = next;
        at = end;
    }
    error.clear();
    return current;
}

bool secure_private_directory(const char *path, Text &error) {
    int fd = walk_directory(path, true, error);
    if (fd < 0) return false;
    struct stat st;
    bool ok = true;
    if (fstat(fd, &st) < 0) ok = set_errno_error(error, "无法检查恢复目录");
    else if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        ok = set_error(error, "恢复目录必须属于当前用户");
    else if (fchmod(fd, 0700) < 0)
        ok = set_errno_error(error, "无法设置恢复目录权限");
    if (close(fd) < 0 && ok) ok = set_errno_error(error, "无法关闭恢复目录");
    if (ok) error.clear();
    return ok;
}

bool make_child_path(const char *base, const char *child, char *output,
                     size_t capacity, Text &error) {
    size_t base_length = strlen(base), child_length = strlen(child);
    if (base_length + 1 + child_length > MaxPathBytes ||
        base_length + 1 + child_length + 1 > capacity)
        return set_error(error, "状态目录路径过长");
    int n = snprintf(output, capacity, "%s/%s", base, child);
    if (n < 0 || static_cast<size_t>(n) >= capacity)
        return set_error(error, "状态目录路径过长");
    return true;
}

bool absolute_base_directory(char *base, size_t capacity, Text &error) {
    const char *configured = getenv("XDG_STATE_HOME");
    if (valid_absolute_directory(configured)) {
        size_t length = strlen(configured);
        if (length + 1 > capacity) return set_error(error, "状态目录路径过长");
        memcpy(base, configured, length + 1);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return set_error(error, "找不到当前用户的状态目录");
    size_t length = strlen(home);
    static const char suffix[] = "/.local/state";
    if (length + sizeof(suffix) - 1 > MaxPathBytes || length + sizeof(suffix) > capacity)
        return set_error(error, "状态目录路径过长");
    int n = snprintf(base, capacity, "%s%s", home, suffix);
    if (n < 0 || static_cast<size_t>(n) >= capacity)
        return set_error(error, "状态目录路径过长");
    return true;
}

bool write_recovery_path(Text &output, const char *directory, const char *name, Text &error) {
    size_t directory_length = strlen(directory), name_length = strlen(name);
    if (directory_length + 1 + name_length > MaxPathBytes)
        return set_error(error, "恢复文件路径过长");
    char path[MaxPathBytes + 1];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
        return set_error(error, "恢复文件路径过长");
    if (!output.set(path)) return set_error(error, "内存不足，无法返回恢复文件路径");
    return true;
}

bool recovery_stamp_newer(const FileStamp &candidate, const char *candidate_path,
                          const FileStamp &current, const Text &current_path) {
    if (candidate.seconds != current.seconds) return candidate.seconds > current.seconds;
    if (candidate.nanoseconds != current.nanoseconds)
        return candidate.nanoseconds > current.nanoseconds;
    if (candidate.change_seconds != current.change_seconds)
        return candidate.change_seconds > current.change_seconds;
    if (candidate.change_nanoseconds != current.change_nanoseconds)
        return candidate.change_nanoseconds > current.change_nanoseconds;
    return strcmp(candidate_path, current_path.c_str()) > 0;
}

} // namespace

bool file_stamp(const char *path, FileStamp &stamp, Text &error) {
    stamp = FileStamp{};
    if (!path || !*path) return set_error(error, "文件路径为空");
    if (strlen(path) > MaxPathBytes) return set_error(error, "文件路径过长");
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) { error.clear(); return true; }
        return set_errno_error(error, "无法检查文件");
    }
    if (!stat_is_regular(st)) return set_error(error, "文件时间戳仅支持非符号链接的普通文件");
    stamp = stamp_from_stat(st);
    error.clear();
    return true;
}

bool same_stamp(const FileStamp &a, const FileStamp &b) {
    return a.exists == b.exists && (!a.exists ||
        (a.device == b.device && a.inode == b.inode && a.size == b.size &&
         a.seconds == b.seconds && a.nanoseconds == b.nanoseconds &&
         a.change_seconds == b.change_seconds && a.change_nanoseconds == b.change_nanoseconds));
}

bool encode_deck(const Deck &deck, Text &bytes, Text &error) {
    return encode_native(deck, bytes, error);
}

bool decode_deck(const char *bytes, size_t length, Deck &out, Text &error) {
    return decode_native(bytes, length, out, error);
}

bool load_deck(const char *path, Deck &out, Text &error) {
    if (!path || !*path) return set_error(error, "演示文稿路径为空");
    if (strlen(path) > MaxPathBytes) return set_error(error, "演示文稿路径过长");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return set_errno_error(error, "无法打开演示文稿");
    struct stat before;
    if (fstat(fd, &before) < 0) {
        int saved = errno; close(fd); errno = saved;
        return set_errno_error(error, "无法检查演示文稿");
    }
    if (!stat_is_regular(before) || static_cast<uint64_t>(before.st_size) > MaxFileBytes) {
        close(fd);
        return set_error(error, "演示文稿类型无效或文件超过 96 MiB");
    }
    size_t length = static_cast<size_t>(before.st_size);
    char *data = static_cast<char *>(malloc(length ? length : 1));
    if (!data) { close(fd); return set_error(error, "内存不足，无法读取演示文稿"); }
    bool ok = true;
    size_t offset = 0;
    while (offset < length) {
        size_t request = length - offset;
        if (request > IoQuantum) request = IoQuantum;
        ssize_t got = read(fd, data + offset, request);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (!got) set_error(error, "读取演示文稿时文件被截断");
            else set_errno_error(error, "无法读取演示文稿");
            ok = false;
            break;
        }
        offset += static_cast<size_t>(got);
    }
    struct stat after;
    if (ok && fstat(fd, &after) < 0) {
        set_errno_error(error, "无法重新检查演示文稿");
        ok = false;
    } else if (ok && !same_stamp(stamp_from_stat(before), stamp_from_stat(after))) {
        set_error(error, "打开期间演示文稿已发生更改");
        ok = false;
    }
    int saved = errno;
    if (close(fd) < 0 && ok) {
        set_errno_error(error, "无法关闭演示文稿");
        ok = false;
    } else errno = saved;
    if (ok) ok = decode_native(data, length, out, error);
    free(data);
    if (ok) error.clear();
    return ok;
}

bool atomic_write(const char *path, const char *bytes, size_t length, Text &error,
                  const FileStamp *expected) {
    if (!path || !*path) return set_error(error, "保存路径为空");
    if ((!bytes && length) || length > MaxAtomicWriteBytes)
        return set_error(error, "输出数据超过 256 MiB 限制");
    const char *base = nullptr;
    int directory = open_parent(path, base, error);
    if (directory < 0) return false;

    FileStamp initial;
    if (!file_stamp_at(directory, base, initial, error)) { close(directory); return false; }
    if (expected && !same_stamp(*expected, initial)) {
        close(directory);
        return set_error(error, "文件已在 ReliefPPT 外部更改，已保留原文件");
    }
    if (initial.exists && initial.size > MaxAtomicWriteBytes) {
        close(directory);
        return set_error(error, "现有输出文件超过 256 MiB 回滚保护限制");
    }

    char temporary[128] = {};
    int fd = -1;
    if (!make_unique_file(directory, ".rppt-tmp", temporary, sizeof(temporary), fd, error)) {
        close(directory);
        return false;
    }
    bool temporary_exists = true;
    bool backup_exists = false;
    char backup[128] = {};
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok) set_errno_error(error, "无法安全创建临时输出文件");
    if (ok) ok = write_all(fd, bytes ? bytes : "", length, error, "无法写入临时输出文件");
    if (ok && fsync(fd) < 0) { set_errno_error(error, "无法同步临时输出文件"); ok = false; }
    int saved_errno = errno;
    if (close(fd) < 0 && ok) { set_errno_error(error, "无法关闭临时输出文件"); ok = false; }
    else errno = saved_errno;

    if (ok && initial.exists)
        ok = copy_rollback(directory, base, initial, backup, sizeof(backup), backup_exists, error);
    if (ok && initial.exists && fsync(directory) < 0) {
        set_errno_error(error, "无法同步原文件回滚目录项");
        ok = false;
    }

    bool target_created = false;
    bool keep_backup = false;
    if (ok) {
        if (initial.exists) {
            // POSIX rename has no stamp-conditional form; keep this final check adjacent
            // to replacement to minimize (but not eliminate) the non-cooperating-writer race.
            if (!safe_current_stamp(directory, base, initial, error)) {
                ok = false;
            } else if (renameat(directory, temporary, directory, base) < 0) {
                set_errno_error(error, "无法替换演示文稿文件");
                ok = false;
            } else temporary_exists = false;
        } else {
            bool renamed = false;
            if (rename_without_replace(directory, temporary, base, renamed) < 0) {
                set_errno_error(error, "无法创建输出文件");
                ok = false;
            } else {
                target_created = true;
                temporary_exists = !renamed;
            }
        }
    }

    if (ok && fsync(directory) < 0) {
        set_errno_error(error, "无法同步输出目录");
        Text commit_error = error;
        bool restored = false;
        if (initial.exists && backup_exists) {
            if (renameat(directory, backup, directory, base) == 0) {
                backup_exists = false;
                restored = true;
            }
        } else if (!initial.exists && target_created) {
            if (unlinkat(directory, base, 0) == 0 || errno == ENOENT) restored = true;
        }
        if (restored) {
            const int sync_result = fsync(directory);
            char message[640];
            if (sync_result == 0) {
                snprintf(message, sizeof(message), "%s；%s", commit_error.c_str(),
                    initial.exists ? "原文件已恢复" : "新目标已删除");
            } else {
                const int saved = errno;
                const char *detail = strerror(saved);
                snprintf(message, sizeof(message), "%s；%s，但恢复目录项同步失败：%s",
                    commit_error.c_str(), initial.exists ? "原文件已恢复" : "新目标已删除",
                    detail ? detail : "I/O 错误");
                errno = saved;
            }
            set_error(error, message);
        } else {
            keep_backup = backup_exists;
            char message[640];
            if (initial.exists && backup_exists) {
                snprintf(message, sizeof(message),
                    "无法同步输出目录或恢复原文件；回滚副本保留在同目录：%s", backup);
            } else if (initial.exists) {
                snprintf(message, sizeof(message), "无法同步输出目录或恢复原文件");
            } else {
                snprintf(message, sizeof(message),
                    "无法同步输出目录或删除新目标；目标文件可能仍存在");
            }
            set_error(error, message);
        }
        ok = false;
    }

    if (temporary_exists) unlinkat(directory, temporary, 0);
    if (backup_exists && !keep_backup) unlinkat(directory, backup, 0);
    if (ok) {
        Text ignored;
        fsync(directory); // Persist cleanup of the private rollback copy, if any.
        error.clear();
    }
    close(directory);
    return ok;
}

bool save_deck(const char *path, const Deck &deck, Text &error, const FileStamp *expected) {
    Text encoded;
    if (!encode_native(deck, encoded, error)) return false;
    return atomic_write(path, encoded.c_str(), encoded.size(), error, expected);
}

bool recovery_directory(Text &output, Text &error) {
    char base[MaxPathBytes + 1];
    if (!absolute_base_directory(base, sizeof(base), error)) return false;
    int state = walk_directory(base, true, error);
    if (state < 0) return false;
    if (close(state) < 0) return set_errno_error(error, "无法关闭状态目录");

    char app[MaxPathBytes + 1], recovery[MaxPathBytes + 1];
    if (!make_child_path(base, "reliefppt", app, sizeof(app), error) ||
        !make_child_path(app, "recovery", recovery, sizeof(recovery), error)) return false;
    if (!secure_private_directory(app, error) || !secure_private_directory(recovery, error)) return false;
    if (!output.set(recovery)) return set_error(error, "内存不足，无法返回恢复目录");
    error.clear();
    return true;
}

bool reserve_recovery_path(Text &output, Text &error) {
    Text directory_path;
    if (!recovery_directory(directory_path, error)) return false;
    int directory = walk_directory(directory_path.c_str(), false, error);
    if (directory < 0) return false;
    struct stat directory_stat;
    if (fstat(directory, &directory_stat) < 0 || !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != geteuid() || (directory_stat.st_mode & 0777) != 0700) {
        int saved = errno; close(directory); errno = saved;
        return set_error(error, "恢复目录权限不够安全");
    }

    char name[128];
    bool reserved = false;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        static uint64_t serial = 0;
        uint64_t id = ++serial;
        int n = snprintf(name, sizeof(name), "session-%ld-%llu.rppt",
            static_cast<long>(getpid()), static_cast<unsigned long long>(id));
        if (n < 0 || static_cast<size_t>(n) >= sizeof(name)) {
            close(directory);
            return set_error(error, "恢复文件名过长");
        }
        int fd = openat(directory, name,
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            int saved = errno; close(directory); errno = saved;
            return set_errno_error(error, "无法创建恢复文件");
        }
        struct stat st;
        bool ok = fchmod(fd, 0600) == 0 && fstat(fd, &st) == 0 &&
            stat_is_regular(st) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0600 &&
            st.st_nlink == 1;
        if (ok && fsync(fd) < 0) ok = false;
        int saved = errno;
        if (close(fd) < 0 && ok) { saved = errno; ok = false; }
        if (ok && fsync(directory) < 0) { saved = errno; ok = false; }
        if (!ok) {
            unlinkat(directory, name, 0);
            close(directory);
            errno = saved;
            return set_errno_error(error, "无法设置恢复文件权限");
        }
        reserved = true;
        break;
    }
    int saved = errno;
    if (!reserved) {
        close(directory);
        errno = saved;
        return set_error(error, "无法分配唯一恢复文件名");
    }
    bool ok = write_recovery_path(output, directory_path.c_str(), name, error);
    if (!ok) unlinkat(directory, name, 0);
    if (close(directory) < 0 && ok) ok = set_errno_error(error, "无法关闭恢复目录");
    if (ok) error.clear();
    return ok;
}

bool lock_recovery_path(const char *path, int &descriptor, Text &error) {
    descriptor = -1;
    if (!path || !*path) return set_error(error, "恢复文件路径为空");
    Text directory_path;
    if (!recovery_directory(directory_path, error)) return false;
    char directory_name[MaxPathBytes + 1];
    const char *base = nullptr;
    if (!split_path(path, directory_name, sizeof(directory_name), base, error)) return false;
    if (strcmp(directory_name, directory_path.c_str()))
        return set_error(error, "恢复文件不在私有恢复目录中");
    size_t base_length = strlen(base);
    if (base_length <= 5 || strcmp(base + base_length - 5, ".rppt"))
        return set_error(error, "恢复文件必须使用 .rppt 扩展名");
    if (base_length + sizeof(".lock") - 1 > MaxComponentBytes)
        return set_error(error, "恢复锁文件名过长");

    int directory = walk_directory(directory_path.c_str(), false, error);
    if (directory < 0) return false;
    struct stat directory_stat;
    if (fstat(directory, &directory_stat) < 0 || directory_stat.st_uid != geteuid() ||
        !S_ISDIR(directory_stat.st_mode) || (directory_stat.st_mode & 0777) != 0700) {
        close(directory);
        return set_error(error, "恢复目录权限不够安全");
    }
    int recovery = openat(directory, base, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (recovery < 0) {
        int saved = errno; close(directory); errno = saved;
        return set_errno_error(error, "无法检查恢复文件");
    }
    struct stat recovery_stat;
    bool valid_recovery = fstat(recovery, &recovery_stat) == 0 && stat_is_regular(recovery_stat) &&
        recovery_stat.st_uid == geteuid() && (recovery_stat.st_mode & 0777) == 0600 &&
        recovery_stat.st_nlink == 1;
    close(recovery);
    if (!valid_recovery) {
        close(directory);
        return set_error(error, "恢复文件必须是私有普通文件");
    }

    char lock_name[MaxComponentBytes + 1];
    int n = snprintf(lock_name, sizeof(lock_name), "%s.lock", base);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(lock_name)) {
        close(directory);
        return set_error(error, "恢复锁文件名过长");
    }
    int fd = openat(directory, lock_name,
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        int saved = errno; close(directory); errno = saved;
        return set_errno_error(error, "无法打开恢复文件锁");
    }
    struct stat lock_stat;
    bool valid_lock = fstat(fd, &lock_stat) == 0 && stat_is_regular(lock_stat) &&
        lock_stat.st_uid == geteuid() && lock_stat.st_nlink == 1;
    if (!valid_lock || fchmod(fd, 0600) < 0 || fstat(fd, &lock_stat) < 0 ||
        (lock_stat.st_mode & 0777) != 0600) {
        int saved = errno; close(fd); close(directory); errno = saved;
        return set_error(error, "恢复锁必须是私有普通文件");
    }
    int result;
    do { result = flock(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
    if (result < 0) {
        int saved = errno; close(fd); close(directory); errno = saved;
        if (saved == EWOULDBLOCK || saved == EAGAIN)
            return set_error(error, "恢复文件已被其他进程使用");
        return set_errno_error(error, "无法锁定恢复文件");
    }
    if (close(directory) < 0) {
        int saved = errno; close(fd); errno = saved;
        return set_errno_error(error, "无法关闭恢复目录");
    }
    descriptor = fd;
    error.clear();
    return true;
}

void unlock_recovery_path(int &descriptor) {
    int fd = descriptor;
    descriptor = -1;
    if (fd >= 0) close(fd);
}

bool find_latest_recovery(Text &path, int &descriptor, Deck &deck, Text &error) {
    path.clear();
    descriptor = -1;
    Text directory_path;
    if (!recovery_directory(directory_path, error)) return false;
    DIR *directory = opendir(directory_path.c_str());
    if (!directory) return set_errno_error(error, "无法扫描恢复目录");

    Text best_path;
    FileStamp best_stamp{};
    int best_fd = -1;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(directory);
        if (!entry) {
            int saved = errno;
            if (saved) {
                closedir(directory);
                unlock_recovery_path(best_fd);
                errno = saved;
                return set_errno_error(error, "无法扫描恢复目录");
            }
            break;
        }
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".rppt")) continue;
        char candidate_path[MaxPathBytes + 1];
        int n = snprintf(candidate_path, sizeof(candidate_path), "%s/%s",
            directory_path.c_str(), entry->d_name);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(candidate_path)) continue;

        int candidate_fd = -1;
        Text candidate_error;
        if (!lock_recovery_path(candidate_path, candidate_fd, candidate_error)) continue;
        FileStamp before;
        if (!file_stamp(candidate_path, before, candidate_error) || !before.exists || !before.size) {
            unlock_recovery_path(candidate_fd);
            continue;
        }
        Deck candidate_deck;
        if (!load_deck(candidate_path, candidate_deck, candidate_error)) {
            const bool allocation_failure = strstr(candidate_error.c_str(), "内存不足") != nullptr;
            unlock_recovery_path(candidate_fd);
            if (allocation_failure) {
                closedir(directory);
                unlock_recovery_path(best_fd);
                error = static_cast<Text &&>(candidate_error);
                return false;
            }
            continue;
        }
        FileStamp after;
        if (!file_stamp(candidate_path, after, candidate_error) || !same_stamp(before, after)) {
            unlock_recovery_path(candidate_fd);
            continue;
        }
        if (best_fd >= 0 && !recovery_stamp_newer(after, candidate_path, best_stamp, best_path)) {
            unlock_recovery_path(candidate_fd);
            continue;
        }
        Text next_path;
        if (!next_path.set(candidate_path)) {
            unlock_recovery_path(candidate_fd);
            closedir(directory);
            unlock_recovery_path(best_fd);
            return set_error(error, "内存不足，无法保存恢复文件路径");
        }
        unlock_recovery_path(best_fd);
        best_path = static_cast<Text &&>(next_path);
        best_stamp = after;
        best_fd = candidate_fd;
    }
    if (closedir(directory) < 0) {
        int saved = errno;
        unlock_recovery_path(best_fd);
        errno = saved;
        return set_errno_error(error, "无法关闭恢复目录");
    }
    if (best_fd < 0) {
        error.clear();
        return true;
    }

    FileStamp before, after;
    Deck selected;
    if (!file_stamp(best_path.c_str(), before, error) || !same_stamp(best_stamp, before) ||
        !load_deck(best_path.c_str(), selected, error) ||
        !file_stamp(best_path.c_str(), after, error) || !same_stamp(before, after)) {
        if (error.empty()) set_error(error, "恢复文件在扫描期间发生更改");
        unlock_recovery_path(best_fd);
        return false;
    }
    path = best_path;
    descriptor = best_fd;
    deck.swap(selected);
    error.clear();
    return true;
}

} // namespace rp
