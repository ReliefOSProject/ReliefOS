#include "process.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

int fake_failure(int code) {
    fprintf(stderr, "fake worker protocol/test failure %d\n", code);
    return code;
}

bool write_all(int fd, const uint8_t *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count > 0) offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool send_frame(rm::Kind kind, const rm::Frame &identity,
                const rm::Bytes *payload = nullptr) {
    rm::Frame frame;
    frame.kind = kind;
    frame.request_id = identity.request_id;
    frame.formula_id = identity.formula_id;
    frame.generation = identity.generation;
    rm::Error error;
    if (payload && !frame.payload.append(payload->data(), payload->size())) return false;
    rm::Bytes wire;
    return rm::encode_frame(frame, wire, error) &&
           write_all(STDOUT_FILENO, wire.data(), wire.size());
}

bool send_artifact(const rm::Frame &identity, rm::ArtifactType type,
                   uint64_t total_bytes, uint32_t width, uint32_t height,
                   uint32_t stride, uint8_t fill) {
    rm::ArtifactInfo info;
    info.type = type;
    info.width = width;
    info.height = height;
    info.stride = stride;
    info.bytes = total_bytes;
    rm::Bytes descriptor;
    rm::Error error;
    if (!rm::encode_artifact_info(info, descriptor, error) ||
        !send_frame(rm::Kind::ArtifactBegin, identity, &descriptor)) return false;
    uint8_t block[rm::kMaxFramePayloadBytes];
    memset(block, fill, sizeof(block));
    while (total_bytes) {
        const size_t count = total_bytes > sizeof(block) ? sizeof(block) :
            static_cast<size_t>(total_bytes);
        rm::Bytes payload;
        if (!payload.append(block, count) ||
            !send_frame(rm::Kind::ArtifactChunk, identity, &payload)) return false;
        total_bytes -= count;
    }
    return send_frame(rm::Kind::ArtifactEnd, identity);
}

int fake_worker() {
    const char *mode_value = getenv("RM_TEST_WORKER_MODE");
    const char *mode = mode_value ? mode_value : "normal";
    if (strcmp(mode, "crash-immediate") == 0) return 17;
    if (strcmp(mode, "start-marker") == 0) {
        const char *path = getenv("RM_TEST_WORKER_PID_FILE");
        if (path) {
            FILE *file = fopen(path, "w");
            if (file) { fprintf(file, "%ld\n", static_cast<long>(getpid())); fclose(file); }
        }
        for (;;) pause();
    }
    if (strcmp(mode, "sleep-before-read") == 0) sleep(11);
    if (strcmp(mode, "slow-read") == 0) usleep(100000);

    rm::Decoder decoder;
    rm::Frame request_identity;
    rm::Snapshot snapshot;
    rm::Bytes source;
    uint32_t expected_source = 0;
    bool began = false;
    bool ended = false;
    uint8_t buffer[4096];
    while (!ended) {
        ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fake_failure(2);
        rm::Error error;
        if (!decoder.feed(buffer, static_cast<size_t>(count), error)) return fake_failure(3);
        rm::Frame frame;
        while (decoder.take(frame)) {
            if (!began) {
                if (frame.kind != rm::Kind::Begin ||
                    !rm::decode_begin_payload(frame, snapshot, expected_source, error)) return fake_failure(4);
                request_identity.request_id = frame.request_id;
                request_identity.formula_id = frame.formula_id;
                request_identity.generation = frame.generation;
                began = true;
                continue;
            }
            if (frame.request_id != request_identity.request_id ||
                frame.formula_id != request_identity.formula_id ||
                frame.generation != request_identity.generation) return fake_failure(5);
            if (frame.kind == rm::Kind::SourceChunk) {
                if (frame.payload.size() > expected_source - source.size() ||
                    !source.append(frame.payload.data(), frame.payload.size())) return fake_failure(6);
            } else if (frame.kind == rm::Kind::End) {
                if (source.size() != expected_source ||
                    !snapshot.formula.source.set(
                        reinterpret_cast<const char *>(source.data()), source.size())) return fake_failure(7);
                ended = true;
                break;
            } else if (frame.kind == rm::Kind::Cancel) {
                return fake_failure(8);
            } else {
                return fake_failure(9);
            }
        }
    }

    if (strcmp(mode, "crash") == 0) return 23;
    if (strcmp(mode, "ignore-cancel") == 0) {
        const char *path = getenv("RM_TEST_WORKER_PID_FILE");
        if (path) {
            FILE *file = fopen(path, "w");
            if (file) { fprintf(file, "%ld\n", static_cast<long>(getpid())); fclose(file); }
        }
        for (;;) pause();
    }

    if (strcmp(mode, "out-of-order") == 0) {
        rm::Bytes unexpected;
        const char byte = 'x';
        if (!unexpected.append(&byte, 1)) return fake_failure(10);
        if (!send_frame(rm::Kind::ArtifactChunk, request_identity, &unexpected)) return fake_failure(11);
        return 0;
    }
    if (strcmp(mode, "wrong-identity") == 0) {
        rm::Frame wrong_identity;
        wrong_identity.request_id = request_identity.request_id + 1;
        wrong_identity.formula_id = request_identity.formula_id;
        wrong_identity.generation = request_identity.generation;
        rm::Bytes done_payload;
        rm::Error done_error;
        if (!rm::encode_done_payload(0, done_payload, done_error) ||
            !send_frame(rm::Kind::Done, wrong_identity, &done_payload)) return fake_failure(16);
        return 0;
    }
    if (strcmp(mode, "failure") == 0) {
        rm::Error failure;
        failure.set(rm::Code::Parse, "unexpected token", 2, 5);
        rm::Bytes payload;
        rm::Error encode_error;
        if (!rm::encode_failure_payload(failure, payload, encode_error) ||
            !send_frame(rm::Kind::Failure, request_identity, &payload)) return fake_failure(19);
        return 0;
    }

    rm::Error error;
    if (strcmp(mode, "incomplete-artifact") == 0) {
        rm::ArtifactInfo info;
        info.type = rm::ArtifactType::Rgba;
        info.width = 256;
        info.height = 256;
        info.stride = 256 * 4;
        info.bytes = uint64_t(info.stride) * info.height;
        rm::Bytes descriptor, partial;
        const uint8_t byte = 0x5a;
        if (!rm::encode_artifact_info(info, descriptor, error) ||
            !send_frame(rm::Kind::ArtifactBegin, request_identity, &descriptor) ||
            !partial.append(&byte, 1) ||
            !send_frame(rm::Kind::ArtifactChunk, request_identity, &partial)) return fake_failure(18);
        return 0;
    }

    if ((snapshot.artifacts & rm::WantSvg) &&
        !send_artifact(request_identity, rm::ArtifactType::Svg, 65536, 0, 0, 0, 0x53))
        return fake_failure(12);
    if ((snapshot.artifacts & rm::WantPng) &&
        !send_artifact(request_identity, rm::ArtifactType::Png, 70000, 0, 0, 0, 0x50))
        return fake_failure(13);
    if ((snapshot.artifacts & rm::WantRaster) &&
        !send_artifact(request_identity, rm::ArtifactType::Rgba, 256u * 256u * 4u,
                       256, 256, 256 * 4, 0x5a)) return fake_failure(14);
    rm::Bytes done_payload;
    if (!rm::encode_done_payload(12.5, done_payload, error) ||
        !send_frame(rm::Kind::Done, request_identity, &done_payload)) return fake_failure(15);
    return 0;
}

rm::Snapshot make_snapshot(uint64_t request_id, size_t source_size = 1) {
    rm::Snapshot snapshot;
    snapshot.request_id = request_id;
    snapshot.session_id = 55;
    snapshot.formula.id = 17;
    snapshot.formula.generation = request_id + 100;
    snapshot.artifacts = rm::WantRaster;
    char *source = static_cast<char *>(malloc(source_size ? source_size : 1));
    assert(source);
    memset(source, 'x', source_size);
    assert(snapshot.formula.source.set(source, source_size));
    free(source);
    return snapshot;
}

bool wait_result(rm::RenderClient &client, rm::Rendered &rendered,
                 rm::Error &error, unsigned timeout_ms = 6000) {
    const uint64_t until = static_cast<uint64_t>(time(nullptr)) * 1000 + timeout_ms;
    for (;;) {
        client.pump();
        if (client.take(rendered, error)) return true;
        if (static_cast<uint64_t>(time(nullptr)) * 1000 > until) return false;
        usleep(1000);
    }
}

void set_mode(const char *mode) {
    assert(setenv("RM_TEST_WORKER_MODE", mode, 1) == 0);
}

void test_large_source_and_maximum_rgba_chunks_survive_pipe_backpressure(const char *path) {
    set_mode("slow-read");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(1, rm::kMaxSourceBytes);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    if (error.code != rm::Code::None)
        fprintf(stderr, "large render failure: code=%u message=%s request=%llu raster=%u/%u/%zu\n",
                static_cast<unsigned>(error.code), error.message.c_str(),
                static_cast<unsigned long long>(rendered.request_id), rendered.raster.width,
                rendered.raster.height, rendered.raster.rgba.size());
    assert(error.code == rm::Code::None);
    assert(rm::same_snapshot(snapshot, rendered.snapshot));
    assert(rendered.raster.width == 256 && rendered.raster.height == 256);
    assert(rendered.raster.rgba.size() == 256u * 256u * 4u);
    assert(rendered.raster.rgba.data()[0] == 0x5a);
    assert(!client.busy());
}

void test_svg_png_and_rgba_streams_are_reassembled_independently(const char *path) {
    set_mode("normal");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(2);
    snapshot.artifacts = rm::WantRaster | rm::WantSvg | rm::WantPng;
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::None);
    assert(rendered.svg.size() == 65536);
    assert(rendered.svg.data()[0] == 0x53);
    assert(rendered.png.size() == 70000);
    assert(rendered.png.data()[65536] == 0x50);
    assert(rendered.raster.rgba.size() == 256u * 256u * 4u);
}

void test_busy_submit_replaces_old_snapshot_and_cancels_only_owned_worker(const char *path) {
    char marker[] = "/tmp/reliefmath-worker-pid.XXXXXX";
    int marker_fd = mkstemp(marker);
    assert(marker_fd >= 0);
    close(marker_fd);
    unlink(marker);
    assert(setenv("RM_TEST_WORKER_PID_FILE", marker, 1) == 0);
    set_mode("ignore-cancel");

    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot stale = make_snapshot(10);
    assert(client.submit(stale, error));
    for (unsigned i = 0; i < 1000 && access(marker, F_OK) != 0; ++i) {
        client.pump();
        usleep(1000);
    }
    assert(access(marker, F_OK) == 0);
    FILE *file = fopen(marker, "r");
    assert(file);
    long child = 0;
    assert(fscanf(file, "%ld", &child) == 1);
    fclose(file);

    set_mode("normal");
    rm::Snapshot latest = make_snapshot(11);
    assert(client.submit(latest, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::None);
    assert(rendered.request_id == latest.request_id);
    assert(rendered.snapshot.formula.source == latest.formula.source);
    for (unsigned i = 0; i < 1000; ++i) {
        if (kill(static_cast<pid_t>(child), 0) < 0 && errno == ESRCH) break;
        client.pump();
        usleep(1000);
    }
    assert(kill(static_cast<pid_t>(child), 0) < 0 && errno == ESRCH);
    unlink(marker);
    unsetenv("RM_TEST_WORKER_PID_FILE");
}

void test_explicit_cancel_reports_cancelled_and_reaps_only_its_worker(const char *path) {
    char marker[] = "/tmp/reliefmath-worker-cancel.XXXXXX";
    int marker_fd = mkstemp(marker);
    assert(marker_fd >= 0);
    close(marker_fd);
    unlink(marker);
    assert(setenv("RM_TEST_WORKER_PID_FILE", marker, 1) == 0);
    set_mode("ignore-cancel");

    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(15);
    assert(client.submit(snapshot, error));
    for (unsigned i = 0; i < 1000 && access(marker, F_OK) != 0; ++i) {
        client.pump();
        usleep(1000);
    }
    assert(access(marker, F_OK) == 0);
    FILE *file = fopen(marker, "r");
    assert(file);
    long child = 0;
    assert(fscanf(file, "%ld", &child) == 1);
    fclose(file);

    client.cancel();
    assert(!client.busy());
    rm::Rendered rendered;
    assert(client.take(rendered, error));
    assert(error.code == rm::Code::Cancelled);
    assert(rendered.request_id == snapshot.request_id);
    for (unsigned i = 0; i < 1000; ++i) {
        if (kill(static_cast<pid_t>(child), 0) < 0 && errno == ESRCH) break;
        client.pump();
        usleep(1000);
    }
    assert(kill(static_cast<pid_t>(child), 0) < 0 && errno == ESRCH);
    unlink(marker);
    unsetenv("RM_TEST_WORKER_PID_FILE");
}

void test_crashing_worker_is_bounded_and_a_later_request_can_start(const char *path) {
    set_mode("crash");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot first = make_snapshot(20);
    assert(client.submit(first, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::Io || error.code == rm::Code::Protocol);

    set_mode("normal");
    rm::Snapshot next = make_snapshot(21);
    assert(client.submit(next, error));
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::None);
    assert(rendered.request_id == 21);
}

void test_out_of_order_frames_fail_as_protocol_error(const char *path) {
    set_mode("out-of-order");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(30);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::Protocol);
    assert(rendered.request_id == snapshot.request_id);
}

void test_mismatched_identity_is_rejected(const char *path) {
    set_mode("wrong-identity");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(35);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::Protocol);
    assert(rendered.request_id == snapshot.request_id);
    assert(rendered.formula_id == snapshot.formula.id);
    assert(rendered.generation == snapshot.formula.generation);
}

void test_eof_inside_artifact_is_rejected(const char *path) {
    set_mode("incomplete-artifact");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(36);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::Protocol);
}

void test_worker_failure_keeps_source_range_and_local_snapshot(const char *path) {
    set_mode("failure");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(37, 8);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code == rm::Code::Parse);
    assert(error.begin == 2 && error.end == 5);
    assert(strcmp(error.message.c_str(), "unexpected token") == 0);
    assert(rm::same_snapshot(snapshot, rendered.snapshot));
    assert(rendered.request_id == snapshot.request_id);
}

void test_sigpipe_is_contained_when_worker_exits_before_reading(const char *path) {
    signal(SIGPIPE, SIG_DFL);
    set_mode("crash-immediate");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(40, rm::kMaxSourceBytes);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error));
    assert(error.code != rm::Code::None);
}

void test_worker_timeout_is_reported_without_blocking_pump(const char *path) {
    set_mode("sleep-before-read");
    rm::RenderClient client;
    rm::Error error;
    assert(client.start(path, error));
    rm::Snapshot snapshot = make_snapshot(50);
    assert(client.submit(snapshot, error));
    rm::Rendered rendered;
    assert(wait_result(client, rendered, error, 12500));
    assert(error.code == rm::Code::Timeout);
}

void test_worker_stdio_survives_closed_parent_standard_fds(const char *path) {
    set_mode("normal");
    const pid_t helper = fork();
    assert(helper >= 0);
    if (helper == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        bool okay = false;
        {
            rm::RenderClient client;
            rm::Error error;
            rm::Rendered rendered;
            rm::Snapshot snapshot = make_snapshot(60);
            okay = client.start(path, error) && client.submit(snapshot, error) &&
                   wait_result(client, rendered, error) && error.code == rm::Code::None &&
                   rendered.request_id == snapshot.request_id;
        }
        _exit(okay ? 0 : 1);
    }

    int status = 0;
    pid_t waited;
    do { waited = waitpid(helper, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == helper);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void test_start_launches_worker_without_waiting(const char *path) {
    char marker[] = "/tmp/reliefmath-worker-start.XXXXXX";
    int marker_fd = mkstemp(marker);
    assert(marker_fd >= 0);
    close(marker_fd);
    unlink(marker);
    assert(setenv("RM_TEST_WORKER_PID_FILE", marker, 1) == 0);
    set_mode("start-marker");

    long child = 0;
    {
        rm::RenderClient client;
        rm::Error error;
        assert(client.start(path, error));
        assert(!client.busy());
        for (unsigned i = 0; i < 1000 && access(marker, F_OK) != 0; ++i) usleep(1000);
        assert(access(marker, F_OK) == 0);
        FILE *file = fopen(marker, "r");
        assert(file);
        assert(fscanf(file, "%ld", &child) == 1);
        fclose(file);
    }
    assert(child > 0);
    assert(kill(static_cast<pid_t>(child), 0) < 0 && errno == ESRCH);
    unlink(marker);
    unsetenv("RM_TEST_WORKER_PID_FILE");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--render-worker") == 0) return fake_worker();
    signal(SIGPIPE, SIG_DFL);
    char executable[4096];
    assert(realpath(argv[0], executable));
    test_start_launches_worker_without_waiting(executable);
    test_large_source_and_maximum_rgba_chunks_survive_pipe_backpressure(executable);
    test_svg_png_and_rgba_streams_are_reassembled_independently(executable);
    test_explicit_cancel_reports_cancelled_and_reaps_only_its_worker(executable);
    test_busy_submit_replaces_old_snapshot_and_cancels_only_owned_worker(executable);
    test_crashing_worker_is_bounded_and_a_later_request_can_start(executable);
    test_out_of_order_frames_fail_as_protocol_error(executable);
    test_mismatched_identity_is_rejected(executable);
    test_eof_inside_artifact_is_rejected(executable);
    test_worker_failure_keeps_source_range_and_local_snapshot(executable);
    test_sigpipe_is_contained_when_worker_exits_before_reading(executable);
    test_worker_timeout_is_reported_without_blocking_pump(executable);
    test_worker_stdio_survives_closed_parent_standard_fds(executable);
    puts("ReliefMath process tests passed");
    return 0;
}
