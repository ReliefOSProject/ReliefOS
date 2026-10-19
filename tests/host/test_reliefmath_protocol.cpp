#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

rm::Frame cancel_frame(uint64_t request_id) {
    rm::Frame frame;
    frame.kind = rm::Kind::Cancel;
    frame.request_id = request_id;
    frame.formula_id = 7;
    frame.generation = 9;
    return frame;
}

void set_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void set_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes[n] = static_cast<uint8_t>(value >> (8 * n));
}

void test_frame_round_trips_one_byte_at_a_time() {
    rm::Frame original = cancel_frame(42);
    rm::Bytes wire;
    rm::Error error;
    assert(rm::encode_frame(original, wire, error));
    assert(wire.size() == rm::kHeaderBytes);
    assert(memcmp(wire.data(), "RMIP", 4) == 0);

    rm::Decoder decoder;
    for (size_t n = 0; n < wire.size(); ++n)
        assert(decoder.feed(wire.data() + n, 1, error));
    rm::Frame received;
    assert(decoder.take(received));
    assert(received.version == 1);
    assert(received.kind == rm::Kind::Cancel);
    assert(received.request_id == 42);
    assert(received.formula_id == 7);
    assert(received.generation == 9);
    assert(received.payload.size() == 0);
    assert(!decoder.take(received));
    assert(decoder.finish(error));
}

void test_split_header_and_merged_frames_preserve_order() {
    rm::Frame first = cancel_frame(100);
    rm::Frame second = cancel_frame(101);
    rm::Bytes one, two, merged;
    rm::Error error;
    assert(rm::encode_frame(first, one, error));
    assert(rm::encode_frame(second, two, error));
    assert(merged.append(one.data(), one.size()));
    assert(merged.append(two.data(), two.size()));

    rm::Decoder decoder;
    assert(decoder.feed(merged.data(), 20, error));
    rm::Frame received;
    assert(!decoder.take(received));
    assert(decoder.feed(merged.data() + 20, merged.size() - 20, error));
    assert(decoder.take(received) && received.request_id == 100);
    assert(decoder.take(received) && received.request_id == 101);
    assert(!decoder.take(received));
    assert(decoder.finish(error));
}

void test_full_64k_source_chunk_is_independent_of_begin_payload() {
    rm::Snapshot snapshot;
    snapshot.session_id = 5;
    snapshot.request_id = 42;
    snapshot.formula.id = 7;
    snapshot.formula.generation = 9;
    snapshot.artifacts = rm::WantSvg;
    char source[rm::kMaxSourceBytes];
    memset(source, 'x', sizeof(source));
    assert(snapshot.formula.source.set(source, sizeof(source)));

    rm::Bytes begin_payload;
    rm::Error error;
    assert(rm::encode_begin_payload(snapshot, begin_payload, error));
    assert(begin_payload.size() < rm::kMaxFramePayloadBytes);

    rm::Frame chunk;
    chunk.kind = rm::Kind::SourceChunk;
    chunk.request_id = snapshot.request_id;
    chunk.formula_id = snapshot.formula.id;
    chunk.generation = snapshot.formula.generation;
    assert(chunk.payload.append(source, sizeof(source)));
    rm::Bytes wire;
    assert(rm::encode_frame(chunk, wire, error));
    assert(wire.size() == rm::kHeaderBytes + sizeof(source));

    rm::Decoder decoder;
    for (size_t offset = 0; offset < wire.size();) {
        size_t count = wire.size() - offset;
        if (count > 997) count = 997;
        assert(decoder.feed(wire.data() + offset, count, error));
        offset += count;
    }
    rm::Frame decoded;
    assert(decoder.take(decoded));
    assert(decoded.payload.size() == rm::kMaxSourceBytes);
    assert(memcmp(decoded.payload.data(), source, sizeof(source)) == 0);
}

void expect_bad_header(uint16_t version, uint16_t kind, uint32_t payload_size,
                       uint32_t flags, rm::Code expected) {
    uint8_t header[rm::kHeaderBytes] = {};
    memcpy(header, "RMIP", 4);
    set_u16(header + 4, version);
    set_u16(header + 6, kind);
    set_u32(header + 8, payload_size);
    set_u32(header + 12, flags);
    set_u32(header + 16, 1);
    set_u32(header + 24, 1);
    set_u32(header + 32, 1);
    rm::Decoder decoder;
    rm::Error error;
    assert(!decoder.feed(header, sizeof(header), error));
    assert(error.code == expected);
}

void test_version_kind_flags_and_length_are_checked_before_payload_allocation() {
    expect_bad_header(2, static_cast<uint16_t>(rm::Kind::Cancel), 0, 0, rm::Code::Version);
    expect_bad_header(1, 99, 0, 0, rm::Code::Protocol);
    expect_bad_header(1, static_cast<uint16_t>(rm::Kind::Cancel), 0, 1, rm::Code::Protocol);
    expect_bad_header(1, static_cast<uint16_t>(rm::Kind::SourceChunk),
                      static_cast<uint32_t>(rm::kMaxFramePayloadBytes + 1), 0,
                      rm::Code::Limit);
}

void test_eof_rejects_half_header_and_partial_payload() {
    rm::Frame frame = cancel_frame(88);
    rm::Bytes wire;
    rm::Error error;
    assert(rm::encode_frame(frame, wire, error));
    rm::Decoder header_decoder;
    assert(header_decoder.feed(wire.data(), 19, error));
    assert(!header_decoder.finish(error));
    assert(error.code == rm::Code::Protocol);

    rm::Frame payload_frame;
    payload_frame.kind = rm::Kind::SourceChunk;
    payload_frame.request_id = 88;
    payload_frame.formula_id = 7;
    payload_frame.generation = 9;
    const char text[] = "source";
    assert(payload_frame.payload.append(text, sizeof(text) - 1));
    rm::Bytes payload_wire;
    assert(rm::encode_frame(payload_frame, payload_wire, error));
    rm::Decoder payload_decoder;
    assert(payload_decoder.feed(payload_wire.data(), payload_wire.size() - 2, error));
    assert(!payload_decoder.finish(error));
    assert(error.code == rm::Code::Protocol);
}

void test_artifact_descriptor_rejects_rgba_overflow_and_invalid_extent() {
    rm::ArtifactInfo info;
    info.type = rm::ArtifactType::Rgba;
    info.width = 16384;
    info.height = 16384;
    info.stride = UINT32_MAX;
    info.bytes = UINT64_MAX;
    rm::Bytes payload;
    rm::Error error;
    assert(!rm::encode_artifact_info(info, payload, error));
    assert(error.code == rm::Code::Limit);
}

} // namespace

int main() {
    test_frame_round_trips_one_byte_at_a_time();
    test_split_header_and_merged_frames_preserve_order();
    test_full_64k_source_chunk_is_independent_of_begin_payload();
    test_version_kind_flags_and_length_are_checked_before_payload_allocation();
    test_eof_rejects_half_header_and_partial_payload();
    test_artifact_descriptor_rejects_rgba_overflow_and_invalid_extent();
    puts("ReliefMath protocol tests passed");
    return 0;
}
