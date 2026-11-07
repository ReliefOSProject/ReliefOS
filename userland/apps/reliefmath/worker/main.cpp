#include "renderer.h"

#include "../protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

struct RequestIdentity {
    uint64_t request_id = 0;
    uint64_t formula_id = 0;
    uint64_t generation = 0;
};

void diagnostic(const char *message) {
    (void)dprintf(STDERR_FILENO, "[reliefmath-render] %s\n", message ? message : "worker error");
}

bool write_all(int fd, const uint8_t *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, bytes + offset, size - offset);
        if (count > 0) offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool send_frame(rm::Kind kind, const RequestIdentity &identity,
                const rm::Bytes *payload = nullptr) {
    rm::Frame frame;
    frame.kind = kind;
    frame.request_id = identity.request_id;
    frame.formula_id = identity.formula_id;
    frame.generation = identity.generation;
    if (payload && !frame.payload.append(payload->data(), payload->size())) return false;
    rm::Bytes wire;
    rm::Error error;
    return rm::encode_frame(frame, wire, error) && write_all(STDOUT_FILENO, wire.data(), wire.size());
}

bool send_failure(const RequestIdentity &identity, const rm::Error &cause) {
    rm::Error failure = cause;
    if (failure.code == rm::Code::None)
        failure.set(rm::Code::Protocol, "Renderer worker rejected the request");
    rm::Bytes payload;
    rm::Error encode_error;
    return rm::encode_failure_payload(failure, payload, encode_error) &&
           send_frame(rm::Kind::Failure, identity, &payload);
}

bool read_frame(rm::Decoder &decoder, rm::Frame &frame, bool &eof, rm::Error &error) {
    eof = false;
    if (decoder.take(frame)) return true;
    uint8_t buffer[8192];
    for (;;) {
        const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count > 0) {
            if (!decoder.feed(buffer, static_cast<size_t>(count), error)) return false;
            if (decoder.take(frame)) return true;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) {
            eof = true;
            return decoder.finish(error);
        }
        error.set(rm::Code::Io, "Cannot read render protocol input");
        return false;
    }
}

bool same_identity(const rm::Frame &frame, const RequestIdentity &identity) {
    return frame.request_id == identity.request_id && frame.formula_id == identity.formula_id &&
           frame.generation == identity.generation;
}

bool send_artifact(rm::ArtifactType type, const rm::Bytes &bytes,
                   uint32_t width, uint32_t height, uint32_t stride,
                   const RequestIdentity &identity) {
    rm::ArtifactInfo info;
    info.type = type;
    info.width = width;
    info.height = height;
    info.stride = stride;
    info.bytes = bytes.size();
    rm::Bytes descriptor;
    rm::Error error;
    if (!rm::encode_artifact_info(info, descriptor, error) ||
        !send_frame(rm::Kind::ArtifactBegin, identity, &descriptor)) return false;

    size_t offset = 0;
    while (offset < bytes.size()) {
        size_t count = bytes.size() - offset;
        if (count > rm::kMaxFramePayloadBytes) count = rm::kMaxFramePayloadBytes;
        rm::Bytes payload;
        if (!payload.append(bytes.data() + offset, count) ||
            !send_frame(rm::Kind::ArtifactChunk, identity, &payload)) return false;
        offset += count;
    }
    return send_frame(rm::Kind::ArtifactEnd, identity);
}

bool send_rendered(const rm::Rendered &rendered, const RequestIdentity &identity) {
    const rm::Snapshot &snapshot = rendered.snapshot;
    if ((snapshot.artifacts & rm::WantSvg) &&
        !send_artifact(rm::ArtifactType::Svg, rendered.svg, 0, 0, 0, identity)) return false;
    if ((snapshot.artifacts & rm::WantPng) &&
        !send_artifact(rm::ArtifactType::Png, rendered.png, 0, 0, 0, identity)) return false;
    if ((snapshot.artifacts & rm::WantRaster) &&
        !send_artifact(rm::ArtifactType::Rgba, rendered.raster.rgba,
                       rendered.raster.width, rendered.raster.height,
                       rendered.raster.stride, identity)) return false;
    rm::Bytes done;
    rm::Error error;
    return rm::encode_done_payload(rendered.baseline, done, error) &&
           send_frame(rm::Kind::Done, identity, &done);
}

bool handle_one_request(rm::Decoder &decoder, rm::Renderer &renderer,
                        bool &initialized, bool &input_eof) {
    rm::Frame begin;
    rm::Error error;
    bool eof = false;
    if (!read_frame(decoder, begin, eof, error)) {
        if (eof && error.code == rm::Code::None) {
            input_eof = true;
            return true;
        }
        diagnostic(error.message.c_str());
        return false;
    }
    if (eof) {
        input_eof = true;
        return true;
    }
    RequestIdentity identity{begin.request_id, begin.formula_id, begin.generation};
    rm::Snapshot snapshot;
    uint32_t expected_source = 0;
    if (!rm::decode_begin_payload(begin, snapshot, expected_source, error)) {
        diagnostic(error.message.c_str());
        return false;
    }

    rm::Bytes source;
    if (!source.resize(expected_source)) {
        error.set(rm::Code::NoMemory, "Cannot reserve bounded formula source");
        return send_failure(identity, error) && false;
    }
    size_t source_received = 0;
    for (;;) {
        rm::Frame frame;
        if (!read_frame(decoder, frame, eof, error)) {
            if (!eof) diagnostic(error.message.c_str());
            if (eof) return false;
            return send_failure(identity, error) && false;
        }
        if (eof) {
            error.set(rm::Code::Protocol, "EOF before request End frame");
            return send_failure(identity, error) && false;
        }
        if (!same_identity(frame, identity)) {
            error.set(rm::Code::Protocol, "Request frame identity does not match Begin");
            return send_failure(identity, error) && false;
        }
        if (frame.kind == rm::Kind::SourceChunk) {
            if (!frame.payload.size() || frame.payload.size() > expected_source - source_received) {
                error.set(rm::Code::Protocol, "SourceChunk exceeds declared source length");
                return send_failure(identity, error) && false;
            }
            memcpy(source.data() + source_received, frame.payload.data(), frame.payload.size());
            source_received += frame.payload.size();
            continue;
        }
        if (frame.kind == rm::Kind::End && !frame.payload.size() && source_received == expected_source)
            break;
        error.set(rm::Code::Protocol, "Expected SourceChunk or complete End frame");
        return send_failure(identity, error) && false;
    }

    size_t bad_offset = 0;
    const char *source_text = reinterpret_cast<const char *>(source.data());
    if (!rm::detail::valid_utf8(source_text, source.size(), &bad_offset)) {
        error.set(rm::Code::InvalidUtf8, "Formula source is not valid UTF-8", bad_offset,
                  bad_offset + 1);
        if (!send_failure(identity, error)) return false;
        return true;
    }
    if (!snapshot.formula.source.set(source_text, source.size())) {
        error.set(rm::Code::NoMemory, "Cannot store formula source");
        if (!send_failure(identity, error)) return false;
        return true;
    }

    if (!initialized) {
        if (!renderer.initialize("/usr/share/reliefmath", error)) {
            if (!send_failure(identity, error)) return false;
            return true;
        }
        initialized = true;
    }
    rm::Rendered rendered;
    error.clear();
    if (!renderer.render(snapshot, rendered, error)) {
        if (error.begin > source.size() || error.end > source.size() || error.begin > error.end) {
            error.begin = 0;
            error.end = 0;
        }
        if (!send_failure(identity, error)) return false;
        return true;
    }
    if (!send_rendered(rendered, identity)) {
        diagnostic("Cannot write artifact response");
        return false;
    }
    return true;
}

int worker_main() {
    rm::Decoder decoder;
    rm::Renderer renderer;
    bool initialized = false;
    for (;;) {
        bool eof = false;
        if (!handle_one_request(decoder, renderer, initialized, eof)) return 2;
        if (eof) return 0;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2 || !argv || !argv[1] || strcmp(argv[1], "--render-worker") != 0) {
        diagnostic("expected --render-worker");
        return 2;
    }
    return worker_main();
}
