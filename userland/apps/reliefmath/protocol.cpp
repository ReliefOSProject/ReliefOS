#include "protocol.h"

#include <math.h>
#include <string.h>

namespace rm {
namespace {

constexpr size_t kBeginPayloadBytes = 34;
constexpr size_t kArtifactInfoBytes = 24;
constexpr size_t kFailurePrefixBytes = 24;

void set_error(Error &error, Code code, const char *message) noexcept {
    error.set(code, message);
}

uint16_t get_u16(const uint8_t *bytes) noexcept {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t get_u32(const uint8_t *bytes) noexcept {
    uint32_t value = 0;
    for (unsigned n = 0; n < 4; ++n) value |= uint32_t(bytes[n]) << (8 * n);
    return value;
}

uint64_t get_u64(const uint8_t *bytes) noexcept {
    uint64_t value = 0;
    for (unsigned n = 0; n < 8; ++n) value |= uint64_t(bytes[n]) << (8 * n);
    return value;
}

void put_u16(uint8_t *bytes, uint16_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *bytes, uint32_t value) noexcept {
    for (unsigned n = 0; n < 4; ++n) bytes[n] = static_cast<uint8_t>(value >> (8 * n));
}

void put_u64(uint8_t *bytes, uint64_t value) noexcept {
    for (unsigned n = 0; n < 8; ++n) bytes[n] = static_cast<uint8_t>(value >> (8 * n));
}

bool valid_kind(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(Kind::Begin) &&
           value <= static_cast<uint16_t>(Kind::Cancel);
}

bool valid_style(const Style &style) noexcept {
    return (style.rgba & 0xffu) != 0 && style.point_size >= kMinPointSize &&
           style.point_size <= kMaxPointSize && style.padding <= kMaxPadding;
}

bool valid_snapshot_settings(const Snapshot &snapshot) noexcept {
    return isfinite(snapshot.scale) && snapshot.scale > 0.0 && snapshot.scale <= 64.0 &&
           snapshot.dpi >= 24 && snapshot.dpi <= 2400 && snapshot.artifacts != 0 &&
           (snapshot.artifacts & static_cast<uint8_t>(~7u)) == 0 &&
           snapshot.formula.source.size() <= kMaxSourceBytes &&
           snapshot.request_id != 0 && snapshot.formula.id != 0 &&
           snapshot.formula.generation != 0 && valid_style(snapshot.formula.style);
}

bool valid_artifact_info(const ArtifactInfo &info, Error &error) noexcept {
    switch (info.type) {
        case ArtifactType::Rgba: {
            if (!info.width || !info.height || info.width > kMaxRasterDimension ||
                info.height > kMaxRasterDimension || info.stride != uint64_t(info.width) * 4) {
                set_error(error, Code::Limit, "Invalid RGBA extent or stride");
                return false;
            }
            const uint64_t pixels = uint64_t(info.width) * info.height;
            if (pixels > kMaxRasterPixels || info.bytes != uint64_t(info.stride) * info.height ||
                info.bytes > kMaxRgbaBytes) {
                set_error(error, Code::Limit, "RGBA output exceeds its buffer limit");
                return false;
            }
            return true;
        }
        case ArtifactType::Svg:
            if (info.width || info.height || info.stride || !info.bytes || info.bytes > kMaxSvgBytes) {
                set_error(error, Code::Limit, "Invalid SVG output declaration");
                return false;
            }
            return true;
        case ArtifactType::Png:
            if (info.width || info.height || info.stride || !info.bytes || info.bytes > kMaxPngBytes) {
                set_error(error, Code::Limit, "Invalid PNG output declaration");
                return false;
            }
            return true;
    }
    set_error(error, Code::Protocol, "Unknown artifact type");
    return false;
}

} // namespace

bool encode_frame(const Frame &frame, Bytes &wire, Error &error) noexcept {
    error.clear();
    if (frame.version != kProtocolVersion) {
        set_error(error, Code::Version, "Unsupported ReliefMath protocol version");
        return false;
    }
    if (!valid_kind(static_cast<uint16_t>(frame.kind)) || frame.flags != 0) {
        set_error(error, Code::Protocol, "Invalid frame kind or flags");
        return false;
    }
    if (frame.payload.size() > kMaxFramePayloadBytes) {
        set_error(error, Code::Limit, "Frame payload exceeds 65536 bytes");
        return false;
    }

    uint8_t header[kHeaderBytes]{};
    memcpy(header, "RMIP", 4);
    put_u16(header + 4, frame.version);
    put_u16(header + 6, static_cast<uint16_t>(frame.kind));
    put_u32(header + 8, static_cast<uint32_t>(frame.payload.size()));
    put_u32(header + 12, frame.flags);
    put_u64(header + 16, frame.request_id);
    put_u64(header + 24, frame.formula_id);
    put_u64(header + 32, frame.generation);
    wire.clear();
    if (!wire.append(header, sizeof(header)) ||
        !wire.append(frame.payload.data(), frame.payload.size())) {
        wire.clear();
        set_error(error, Code::NoMemory, "Cannot encode frame");
        return false;
    }
    return true;
}

bool Decoder::feed(const uint8_t *bytes, size_t size, Error &error) noexcept {
    error.clear();
    if (failed_) {
        set_error(error, Code::Protocol, "Frame decoder is in a failed state");
        return false;
    }
    if (!bytes && size) {
        failed_ = true;
        set_error(error, Code::Protocol, "Null frame input");
        return false;
    }

    size_t offset = 0;
    while (offset < size) {
        if (header_size_ < kHeaderBytes) {
            if (header_size_ == 0 && ready_.size() >= kMaxDecodedFrames) {
                failed_ = true;
                set_error(error, Code::Limit, "Too many complete frames buffered");
                return false;
            }
            size_t count = kHeaderBytes - header_size_;
            if (count > size - offset) count = size - offset;
            memcpy(header_ + header_size_, bytes + offset, count);
            header_size_ += count;
            offset += count;
            if (header_size_ != kHeaderBytes) continue;

            if (memcmp(header_, "RMIP", 4) != 0) {
                failed_ = true;
                set_error(error, Code::Protocol, "Invalid frame magic");
                return false;
            }
            const uint16_t version = get_u16(header_ + 4);
            if (version != kProtocolVersion) {
                failed_ = true;
                set_error(error, Code::Version, "Unsupported ReliefMath protocol version");
                return false;
            }
            const uint16_t kind = get_u16(header_ + 6);
            if (!valid_kind(kind) || get_u32(header_ + 12) != 0) {
                failed_ = true;
                set_error(error, Code::Protocol, "Unknown frame kind or flags");
                return false;
            }
            payload_size_ = get_u32(header_ + 8);
            if (payload_size_ > kMaxFramePayloadBytes) {
                failed_ = true;
                set_error(error, Code::Limit, "Frame payload exceeds 65536 bytes");
                return false;
            }

            pending_ = Frame();
            pending_.version = version;
            pending_.kind = static_cast<Kind>(kind);
            pending_.flags = 0;
            pending_.request_id = get_u64(header_ + 16);
            pending_.formula_id = get_u64(header_ + 24);
            pending_.generation = get_u64(header_ + 32);
            payload_read_ = 0;
            if (!pending_.payload.resize(payload_size_)) {
                failed_ = true;
                set_error(error, Code::NoMemory, "Cannot allocate bounded frame payload");
                return false;
            }
            if (payload_size_ == 0) {
                if (!ready_.push(static_cast<Frame &&>(pending_))) {
                    failed_ = true;
                    set_error(error, Code::NoMemory, "Cannot queue decoded frame");
                    return false;
                }
                header_size_ = 0;
                payload_size_ = 0;
                pending_ = Frame();
            }
            continue;
        }

        size_t count = payload_size_ - payload_read_;
        if (count > size - offset) count = size - offset;
        memcpy(pending_.payload.data() + payload_read_, bytes + offset, count);
        payload_read_ += count;
        offset += count;
        if (payload_read_ == payload_size_) {
            if (!ready_.push(static_cast<Frame &&>(pending_))) {
                failed_ = true;
                set_error(error, Code::NoMemory, "Cannot queue decoded frame");
                return false;
            }
            header_size_ = 0;
            payload_size_ = 0;
            payload_read_ = 0;
            pending_ = Frame();
        }
    }
    return true;
}

bool Decoder::take(Frame &frame) noexcept {
    if (ready_.empty()) return false;
    frame = static_cast<Frame &&>(ready_[0]);
    ready_.erase(0);
    return true;
}

bool Decoder::finish(Error &error) noexcept {
    error.clear();
    if (failed_) {
        set_error(error, Code::Protocol, "Frame decoder is in a failed state");
        return false;
    }
    if (header_size_ || payload_read_ != payload_size_) {
        set_error(error, Code::Protocol, "EOF in a partial frame");
        return false;
    }
    return true;
}

void Decoder::reset() noexcept {
    header_size_ = 0;
    payload_size_ = 0;
    payload_read_ = 0;
    pending_ = Frame();
    ready_.clear();
    failed_ = false;
    memset(header_, 0, sizeof(header_));
}

bool encode_begin_payload(const Snapshot &snapshot, Bytes &payload, Error &error) noexcept {
    error.clear();
    if (!valid_snapshot_settings(snapshot)) {
        set_error(error, Code::Limit, "Invalid render snapshot metadata");
        return false;
    }
    uint8_t bytes[kBeginPayloadBytes]{};
    put_u64(bytes, snapshot.session_id);
    put_u32(bytes + 8, snapshot.formula.style.rgba);
    put_u16(bytes + 12, snapshot.formula.style.point_size);
    put_u16(bytes + 14, snapshot.formula.style.padding);
    bytes[16] = snapshot.formula.style.display ? 1 : 0;
    uint64_t scale_bits = 0;
    static_assert(sizeof(scale_bits) == sizeof(snapshot.scale), "protocol requires binary64 double");
    memcpy(&scale_bits, &snapshot.scale, sizeof(scale_bits));
    put_u64(bytes + 17, scale_bits);
    put_u32(bytes + 25, snapshot.dpi);
    bytes[29] = snapshot.artifacts;
    put_u32(bytes + 30, static_cast<uint32_t>(snapshot.formula.source.size()));
    payload.clear();
    if (!payload.append(bytes, sizeof(bytes))) {
        set_error(error, Code::NoMemory, "Cannot encode Begin metadata");
        return false;
    }
    return true;
}

bool decode_begin_payload(const Frame &frame, Snapshot &snapshot,
                          uint32_t &source_bytes, Error &error) noexcept {
    error.clear();
    if (frame.kind != Kind::Begin || frame.flags || frame.payload.size() != kBeginPayloadBytes ||
        !frame.request_id || !frame.formula_id || !frame.generation) {
        set_error(error, Code::Protocol, "Invalid Begin frame or metadata length");
        return false;
    }
    Snapshot candidate;
    candidate.request_id = frame.request_id;
    candidate.session_id = get_u64(frame.payload.data());
    candidate.formula.id = frame.formula_id;
    candidate.formula.generation = frame.generation;
    candidate.formula.style.rgba = get_u32(frame.payload.data() + 8);
    candidate.formula.style.point_size = get_u16(frame.payload.data() + 12);
    candidate.formula.style.padding = get_u16(frame.payload.data() + 14);
    const uint8_t display = frame.payload.data()[16];
    if (display > 1) {
        set_error(error, Code::Protocol, "Invalid Begin display flag");
        return false;
    }
    candidate.formula.style.display = display != 0;
    const uint64_t scale_bits = get_u64(frame.payload.data() + 17);
    memcpy(&candidate.scale, &scale_bits, sizeof(candidate.scale));
    candidate.dpi = get_u32(frame.payload.data() + 25);
    candidate.artifacts = frame.payload.data()[29];
    source_bytes = get_u32(frame.payload.data() + 30);
    if (source_bytes > kMaxSourceBytes || !valid_snapshot_settings(candidate)) {
        set_error(error, Code::Limit, "Begin metadata exceeds render limits");
        return false;
    }
    snapshot = static_cast<Snapshot &&>(candidate);
    return true;
}

bool encode_artifact_info(const ArtifactInfo &info, Bytes &payload, Error &error) noexcept {
    error.clear();
    if (!valid_artifact_info(info, error)) return false;
    uint8_t bytes[kArtifactInfoBytes]{};
    bytes[0] = static_cast<uint8_t>(info.type);
    put_u32(bytes + 4, info.width);
    put_u32(bytes + 8, info.height);
    put_u32(bytes + 12, info.stride);
    put_u64(bytes + 16, info.bytes);
    payload.clear();
    if (!payload.append(bytes, sizeof(bytes))) {
        set_error(error, Code::NoMemory, "Cannot encode artifact descriptor");
        return false;
    }
    return true;
}

bool decode_artifact_info(const Frame &frame, ArtifactInfo &info, Error &error) noexcept {
    error.clear();
    if (frame.kind != Kind::ArtifactBegin || frame.flags || frame.payload.size() != kArtifactInfoBytes ||
        frame.payload.data()[1] || frame.payload.data()[2] || frame.payload.data()[3]) {
        set_error(error, Code::Protocol, "Invalid ArtifactBegin metadata");
        return false;
    }
    ArtifactInfo candidate;
    candidate.type = static_cast<ArtifactType>(frame.payload.data()[0]);
    candidate.width = get_u32(frame.payload.data() + 4);
    candidate.height = get_u32(frame.payload.data() + 8);
    candidate.stride = get_u32(frame.payload.data() + 12);
    candidate.bytes = get_u64(frame.payload.data() + 16);
    if (!valid_artifact_info(candidate, error)) return false;
    info = candidate;
    return true;
}

bool encode_done_payload(double baseline, Bytes &payload, Error &error) noexcept {
    error.clear();
    if (!isfinite(baseline)) {
        set_error(error, Code::Protocol, "Invalid renderer baseline");
        return false;
    }
    uint64_t bits = 0;
    memcpy(&bits, &baseline, sizeof(bits));
    uint8_t bytes[8];
    put_u64(bytes, bits);
    payload.clear();
    if (!payload.append(bytes, sizeof(bytes))) {
        set_error(error, Code::NoMemory, "Cannot encode Done metadata");
        return false;
    }
    return true;
}

bool decode_done_payload(const Frame &frame, double &baseline, Error &error) noexcept {
    error.clear();
    if (frame.kind != Kind::Done || frame.flags || frame.payload.size() != 8) {
        set_error(error, Code::Protocol, "Invalid Done metadata");
        return false;
    }
    const uint64_t bits = get_u64(frame.payload.data());
    double candidate = 0;
    memcpy(&candidate, &bits, sizeof(candidate));
    if (!isfinite(candidate)) {
        set_error(error, Code::Protocol, "Invalid renderer baseline");
        return false;
    }
    baseline = candidate;
    return true;
}

bool encode_failure_payload(const Error &failure, Bytes &payload, Error &error) noexcept {
    error.clear();
    const uint8_t code = static_cast<uint8_t>(failure.code);
    if (!code || code > static_cast<uint8_t>(Code::NotFound) || failure.begin > failure.end ||
        failure.message.size() > kMaxFramePayloadBytes - kFailurePrefixBytes) {
        set_error(error, Code::Limit, "Failure details exceed protocol limits");
        return false;
    }
    uint8_t prefix[kFailurePrefixBytes]{};
    prefix[0] = code;
    put_u64(prefix + 4, static_cast<uint64_t>(failure.begin));
    put_u64(prefix + 12, static_cast<uint64_t>(failure.end));
    put_u32(prefix + 20, static_cast<uint32_t>(failure.message.size()));
    payload.clear();
    if (!payload.append(prefix, sizeof(prefix)) ||
        !payload.append(failure.message.c_str(), failure.message.size())) {
        payload.clear();
        set_error(error, Code::NoMemory, "Cannot encode Failure metadata");
        return false;
    }
    return true;
}

bool decode_failure_payload(const Frame &frame, size_t source_bytes,
                            Error &failure, Error &error) noexcept {
    error.clear();
    if (frame.kind != Kind::Failure || frame.flags || frame.payload.size() < kFailurePrefixBytes ||
        frame.payload.data()[1] || frame.payload.data()[2] || frame.payload.data()[3]) {
        set_error(error, Code::Protocol, "Invalid Failure metadata");
        return false;
    }
    const uint8_t code = frame.payload.data()[0];
    const uint64_t begin = get_u64(frame.payload.data() + 4);
    const uint64_t end = get_u64(frame.payload.data() + 12);
    const uint32_t message_bytes = get_u32(frame.payload.data() + 20);
    if (!code || code > static_cast<uint8_t>(Code::NotFound) || begin > end ||
        end > source_bytes || begin > SIZE_MAX || end > SIZE_MAX ||
        message_bytes != frame.payload.size() - kFailurePrefixBytes) {
        set_error(error, Code::Protocol, "Failure range or message length is invalid");
        return false;
    }
    const uint8_t *message = frame.payload.data() + kFailurePrefixBytes;
    if (!detail::valid_utf8(reinterpret_cast<const char *>(message), message_bytes)) {
        set_error(error, Code::Protocol, "Failure message is not valid UTF-8");
        return false;
    }
    failure.clear();
    failure.code = static_cast<Code>(code);
    failure.begin = static_cast<size_t>(begin);
    failure.end = static_cast<size_t>(end);
    if (!failure.message.set(reinterpret_cast<const char *>(message), message_bytes)) {
        failure.clear();
        set_error(error, Code::NoMemory, "Cannot store Failure message");
        return false;
    }
    return true;
}

} // namespace rm
