#ifndef RELIEFMATH_PROTOCOL_H
#define RELIEFMATH_PROTOCOL_H

#include "render_types.h"

namespace rm {

constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHeaderBytes = 40;
constexpr size_t kMaxFramePayloadBytes = 65536;
constexpr size_t kMaxDecodedFrames = 16;
constexpr uint64_t kMaxSvgBytes = 16u * 1024u * 1024u;
constexpr uint64_t kMaxPngBytes = 64u * 1024u * 1024u;
constexpr uint64_t kMaxRgbaBytes = 64u * 1024u * 1024u;

enum class Kind : uint16_t {
    Begin = 1,
    SourceChunk = 2,
    End = 3,
    ArtifactBegin = 4,
    ArtifactChunk = 5,
    ArtifactEnd = 6,
    Done = 7,
    Failure = 8,
    Cancel = 9
};

struct Frame {
    uint16_t version = kProtocolVersion;
    Kind kind = Kind::Cancel;
    uint32_t flags = 0;
    uint64_t request_id = 0;
    uint64_t formula_id = 0;
    uint64_t generation = 0;
    Bytes payload;
};

/** Encodes one RMIP frame in its fixed 40-byte little-endian header format. */
bool encode_frame(const Frame &frame, Bytes &wire, Error &error) noexcept;

/** Bounded incremental decoder; frame payload memory is allocated only after header validation. */
class Decoder {
    uint8_t header_[kHeaderBytes]{};
    size_t header_size_ = 0;
    size_t payload_size_ = 0;
    size_t payload_read_ = 0;
    Frame pending_;
    Array<Frame> ready_;
    bool failed_ = false;

public:
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    /** Accepts any prefix or concatenation of frames without waiting for more bytes. */
    bool feed(const uint8_t *bytes, size_t size, Error &error) noexcept;
    /** Moves out the next complete frame in stream order. */
    bool take(Frame &frame) noexcept;
    /** Reports whether a trailing partial frame is buffered after complete frames. */
    bool has_partial_frame() const noexcept { return header_size_ != 0; }
    /** Rejects EOF in a partial header or payload. */
    bool finish(Error &error) noexcept;
    /** Discards queued/partial data and permits decoding a new worker stream. */
    void reset() noexcept;
};

enum class ArtifactType : uint8_t { Rgba = 1, Svg = 2, Png = 3 };

struct ArtifactInfo {
    ArtifactType type = ArtifactType::Rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint64_t bytes = 0;
};

/** Encodes the independently bounded Begin metadata (source bytes travel in SourceChunk frames). */
bool encode_begin_payload(const Snapshot &snapshot, Bytes &payload, Error &error) noexcept;
/** Decodes Begin metadata and reports the exact source byte count expected before End. */
bool decode_begin_payload(const Frame &frame, Snapshot &snapshot,
                          uint32_t &source_bytes, Error &error) noexcept;

/** Encodes and validates ArtifactBegin metadata, including RGBA dimensions and capacity. */
bool encode_artifact_info(const ArtifactInfo &info, Bytes &payload, Error &error) noexcept;
bool decode_artifact_info(const Frame &frame, ArtifactInfo &info, Error &error) noexcept;

/** Done carries the renderer baseline as an IEEE-754 binary64 little-endian value. */
bool encode_done_payload(double baseline, Bytes &payload, Error &error) noexcept;
bool decode_done_payload(const Frame &frame, double &baseline, Error &error) noexcept;

/** Failure payload preserves the error code, UTF-8 message and source byte range. */
bool encode_failure_payload(const Error &failure, Bytes &payload, Error &error) noexcept;
bool decode_failure_payload(const Frame &frame, size_t source_bytes,
                            Error &failure, Error &error) noexcept;

} // namespace rm

#endif
