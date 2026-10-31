#include "process.h"

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace rm {
namespace {

constexpr size_t kWorkerPathBytes = 4096;
constexpr size_t kPumpIoBytes = 16384;
constexpr uint64_t kRequestTimeoutMs = 10000;
constexpr uint64_t kRestartWindowMs = 30000;
constexpr unsigned kMaxAutomaticRestarts = 2;

struct ClientState {
    char worker_path[kWorkerPathBytes]{};
    bool configured = false;
    pid_t child = -1;
    uint64_t child_request_id = 0;
    uint64_t child_formula_id = 0;
    uint64_t child_generation = 0;
    int input_fd = -1;
    int output_fd = -1;
    bool child_reaped = false;
    bool child_status_known = false;
    int child_status = 0;
    bool intentional_stop = false;

    bool active = false;
    Snapshot snapshot;
    Bytes request_wire;
    size_t request_offset = 0;
    bool request_sent = false;
    uint64_t started_ms = 0;

    Decoder decoder;
    Rendered candidate;
    ArtifactInfo current_artifact;
    bool artifact_active = false;
    uint64_t artifact_received = 0;
    uint8_t artifacts_received = 0;
    double baseline = 0;

    bool result_ready = false;
    Error result_error;
    uint64_t restart_times[kMaxAutomaticRestarts]{};
    unsigned restart_count = 0;
};

void set_error(Error &error, Code code, const char *message,
               size_t begin = 0, size_t end = 0) noexcept {
    error.set(code, message, begin, end);
}

uint64_t monotonic_ms() noexcept {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0) return 0;
    return uint64_t(now.tv_sec) * 1000u + uint64_t(now.tv_nsec / 1000000u);
}

void close_fd(int &fd) noexcept {
    if (fd >= 0) close(fd);
    fd = -1;
}

bool set_fd_flag(int fd, int get_command, int set_command, int bit) noexcept {
    const int flags = fcntl(fd, get_command);
    return flags >= 0 && fcntl(fd, set_command, flags | bit) == 0;
}

bool prepare_pipe(int fds[2]) noexcept {
    fds[0] = fds[1] = -1;
    if (pipe(fds) < 0) return false;
    if (!set_fd_flag(fds[0], F_GETFD, F_SETFD, FD_CLOEXEC) ||
        !set_fd_flag(fds[1], F_GETFD, F_SETFD, FD_CLOEXEC)) {
        close(fds[0]);
        close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
}

bool close_child_descriptors() noexcept {
    return syscall(SYS_close_range, 3u, ~0u, 0u) == 0;
}

bool set_nonblocking(int fd) noexcept {
    return set_fd_flag(fd, F_GETFL, F_SETFL, O_NONBLOCK);
}

bool append_frame(Bytes &stream, const Frame &frame, Error &error) noexcept {
    Bytes encoded;
    if (!encode_frame(frame, encoded, error)) return false;
    if (!stream.append(encoded.data(), encoded.size())) {
        set_error(error, Code::NoMemory, "Cannot queue render request");
        return false;
    }
    return true;
}

bool make_request(const Snapshot &snapshot, Bytes &wire, Error &error) noexcept {
    wire.clear();
    Frame begin;
    begin.kind = Kind::Begin;
    begin.request_id = snapshot.request_id;
    begin.formula_id = snapshot.formula.id;
    begin.generation = snapshot.formula.generation;
    if (!encode_begin_payload(snapshot, begin.payload, error) || !append_frame(wire, begin, error))
        return false;

    if (!snapshot.formula.source.empty()) {
        Frame source;
        source.kind = Kind::SourceChunk;
        source.request_id = snapshot.request_id;
        source.formula_id = snapshot.formula.id;
        source.generation = snapshot.formula.generation;
        if (!source.payload.append(snapshot.formula.source.c_str(), snapshot.formula.source.size())) {
            set_error(error, Code::NoMemory, "Cannot copy source chunk");
            return false;
        }
        if (!append_frame(wire, source, error)) return false;
    }

    Frame end;
    end.kind = Kind::End;
    end.request_id = snapshot.request_id;
    end.formula_id = snapshot.formula.id;
    end.generation = snapshot.formula.generation;
    return append_frame(wire, end, error);
}

ClientState *state_of(void *state) noexcept {
    return static_cast<ClientState *>(state);
}

void retry_or_fail(ClientState &state) noexcept;

void finish_reaped_worker(ClientState &state) noexcept {
    const bool intentional = state.intentional_stop;
    const bool same_request = state.active &&
        state.child_request_id == state.snapshot.request_id &&
        state.child_formula_id == state.snapshot.formula.id &&
        state.child_generation == state.snapshot.formula.generation;
    if (same_request && !intentional) retry_or_fail(state);
    state.child_request_id = 0;
    state.child_formula_id = 0;
    state.child_generation = 0;
    state.child_reaped = false;
    state.child_status_known = false;
    state.child_status = 0;
    state.intentional_stop = false;
}

void make_identity_result(ClientState &state) noexcept {
    Rendered result;
    result.snapshot = state.snapshot;
    result.formula_id = state.snapshot.formula.id;
    result.request_id = state.snapshot.request_id;
    result.generation = state.snapshot.formula.generation;
    state.candidate = static_cast<Rendered &&>(result);
}

void clear_request(ClientState &state) noexcept {
    state.active = false;
    state.request_wire.clear();
    state.request_offset = 0;
    state.request_sent = false;
    state.started_ms = 0;
    state.artifact_active = false;
    state.artifact_received = 0;
    state.artifacts_received = 0;
    state.baseline = 0;
}

void complete_failure(ClientState &state, const Error &failure) noexcept {
    make_identity_result(state);
    state.result_error = failure;
    state.result_ready = true;
    clear_request(state);
}

void complete_success(ClientState &state) noexcept {
    state.candidate.baseline = state.baseline;
    state.result_error.clear();
    state.result_ready = true;
    state.active = false;
    state.request_wire.clear();
    state.request_offset = 0;
    state.request_sent = false;
    state.started_ms = 0;
    state.artifact_active = false;
    state.artifact_received = 0;
    state.artifacts_received = 0;
}

void request_worker_stop(ClientState &state) noexcept {
    if (state.child > 0) {
        state.intentional_stop = true;
        (void)kill(state.child, SIGKILL);
    } else if (state.child_reaped) {
        state.intentional_stop = true;
    }
    close_fd(state.input_fd);
    close_fd(state.output_fd);
    if (state.child < 0 && state.child_reaped) finish_reaped_worker(state);
}

void reap_nonblocking(ClientState &state) noexcept {
    if (state.child <= 0) return;
    int status = 0;
    const pid_t result = waitpid(state.child, &status, WNOHANG);
    if (result < 0 && errno == EINTR) return;
    if (result == 0) return;

    state.child = -1;
    state.child_reaped = true;
    state.child_status_known = result > 0;
    state.child_status = status;
    close_fd(state.input_fd);
    if (state.output_fd < 0) finish_reaped_worker(state);
}

bool spawn_worker(ClientState &state, Error &error) noexcept {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    if (!prepare_pipe(input) || !prepare_pipe(output)) {
        const int saved = errno;
        if (input[0] >= 0) close(input[0]);
        if (input[1] >= 0) close(input[1]);
        if (output[0] >= 0) close(output[0]);
        if (output[1] >= 0) close(output[1]);
        errno = saved;
        set_error(error, Code::Io, "Cannot create renderer pipes");
        return false;
    }
    if (!set_nonblocking(input[1]) || !set_nonblocking(output[0])) {
        const int saved = errno;
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        errno = saved;
        set_error(error, Code::Io, "Cannot make renderer pipes nonblocking");
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        const int saved = errno;
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        errno = saved;
        set_error(error, Code::Io, "Cannot start renderer worker");
        return false;
    }
    if (child == 0) {
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            fcntl(STDIN_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) < 0 || !close_child_descriptors()) _exit(126);
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGPIPE, &action, nullptr);
        char *const arguments[] = {state.worker_path, const_cast<char *>("--render-worker"), nullptr};
        execv(state.worker_path, arguments);
        _exit(127);
    }

    close(input[0]);
    close(output[1]);
    state.child = child;
    state.child_request_id = state.active ? state.snapshot.request_id : 0;
    state.child_formula_id = state.active ? state.snapshot.formula.id : 0;
    state.child_generation = state.active ? state.snapshot.formula.generation : 0;
    state.child_reaped = false;
    state.child_status_known = false;
    state.child_status = 0;
    state.input_fd = input[1];
    state.output_fd = output[0];
    state.intentional_stop = false;
    return true;
}

ssize_t write_without_sigpipe(int fd, const uint8_t *bytes, size_t size) noexcept {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0) return -1;
    const bool already_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
    const ssize_t result = write(fd, bytes, size);
    const int saved_error = errno;
    if (result < 0 && saved_error == EPIPE && !already_pending) {
        const timespec immediate{0, 0};
        while (sigtimedwait(&blocked, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    (void)sigprocmask(SIG_SETMASK, &previous, nullptr);
    errno = saved_error;
    return result;
}

uint8_t artifact_bit(ArtifactType type) noexcept {
    switch (type) {
        case ArtifactType::Rgba: return WantRaster;
        case ArtifactType::Svg: return WantSvg;
        case ArtifactType::Png: return WantPng;
    }
    return 0;
}

Bytes *artifact_bytes(Rendered &rendered, ArtifactType type) noexcept {
    switch (type) {
        case ArtifactType::Rgba: return &rendered.raster.rgba;
        case ArtifactType::Svg: return &rendered.svg;
        case ArtifactType::Png: return &rendered.png;
    }
    return nullptr;
}

void protocol_failure(ClientState &state, const char *message) noexcept {
    Error failure;
    set_error(failure, Code::Protocol, message);
    state.active = false;
    request_worker_stop(state);
    complete_failure(state, failure);
}

bool matches_request(const Frame &frame, const ClientState &state) noexcept {
    return frame.request_id == state.snapshot.request_id &&
           frame.formula_id == state.snapshot.formula.id &&
           frame.generation == state.snapshot.formula.generation;
}

bool handle_frame(ClientState &state, Frame &frame) noexcept {
    if (!state.active || !state.request_sent || !matches_request(frame, state)) {
        protocol_failure(state, "Renderer response identity or sequence mismatch");
        return false;
    }
    switch (frame.kind) {
        case Kind::ArtifactBegin: {
            if (state.artifact_active) {
                protocol_failure(state, "Nested ArtifactBegin frame");
                return false;
            }
            ArtifactInfo info;
            Error error;
            if (!decode_artifact_info(frame, info, error)) {
                protocol_failure(state, error.message.c_str());
                return false;
            }
            const uint8_t bit = artifact_bit(info.type);
            if (!bit || !(state.snapshot.artifacts & bit) || (state.artifacts_received & bit)) {
                protocol_failure(state, "Renderer declared an unrequested or duplicate artifact");
                return false;
            }
            Bytes *storage = artifact_bytes(state.candidate, info.type);
            if (!storage || !storage->resize(static_cast<size_t>(info.bytes))) {
                Error failure;
                set_error(failure, Code::NoMemory, "Cannot allocate declared render artifact");
                state.active = false;
                request_worker_stop(state);
                complete_failure(state, failure);
                return false;
            }
            if (info.type == ArtifactType::Rgba) {
                state.candidate.raster.width = info.width;
                state.candidate.raster.height = info.height;
                state.candidate.raster.stride = info.stride;
            }
            state.current_artifact = info;
            state.artifact_received = 0;
            state.artifact_active = true;
            return true;
        }
        case Kind::ArtifactChunk: {
            if (!state.artifact_active || !frame.payload.size() ||
                frame.payload.size() > state.current_artifact.bytes - state.artifact_received) {
                protocol_failure(state, "ArtifactChunk has no matching bounded artifact");
                return false;
            }
            Bytes *storage = artifact_bytes(state.candidate, state.current_artifact.type);
            if (!storage) {
                protocol_failure(state, "ArtifactChunk type is invalid");
                return false;
            }
            memcpy(storage->data() + static_cast<size_t>(state.artifact_received),
                   frame.payload.data(), frame.payload.size());
            state.artifact_received += frame.payload.size();
            return true;
        }
        case Kind::ArtifactEnd:
            if (!state.artifact_active || state.artifact_received != state.current_artifact.bytes ||
                frame.payload.size()) {
                protocol_failure(state, "ArtifactEnd arrived before the declared bytes");
                return false;
            }
            state.artifacts_received |= artifact_bit(state.current_artifact.type);
            state.artifact_active = false;
            state.artifact_received = 0;
            return true;
        case Kind::Done: {
            if (state.artifact_active || state.artifacts_received != state.snapshot.artifacts) {
                protocol_failure(state, "Done arrived before all requested artifacts");
                return false;
            }
            Error error;
            if (!decode_done_payload(frame, state.baseline, error)) {
                protocol_failure(state, error.message.c_str());
                return false;
            }
            complete_success(state);
            return true;
        }
        case Kind::Failure: {
            if (state.artifact_active || state.artifacts_received) {
                protocol_failure(state, "Failure interrupted an artifact stream");
                return false;
            }
            Error failure;
            Error decode_error;
            if (!decode_failure_payload(frame, state.snapshot.formula.source.size(), failure, decode_error)) {
                protocol_failure(state, decode_error.message.c_str());
                return false;
            }
            complete_failure(state, failure);
            return true;
        }
        default:
            protocol_failure(state, "Unexpected frame from renderer worker");
            return false;
    }
}

bool record_restart(ClientState &state, uint64_t now) noexcept {
    if (state.restart_count && now >= state.restart_times[0] &&
        now - state.restart_times[0] >= kRestartWindowMs) {
        if (state.restart_count > 1) state.restart_times[0] = state.restart_times[1];
        --state.restart_count;
    }
    if (state.restart_count >= kMaxAutomaticRestarts) return false;
    state.restart_times[state.restart_count++] = now;
    return true;
}

void retry_or_fail(ClientState &state) noexcept {
    const uint64_t now = monotonic_ms();
    if (!record_restart(state, now)) {
        Error failure;
        char message[128];
        if (state.child_status_known && WIFSIGNALED(state.child_status))
            (void)snprintf(message, sizeof(message),
                "Renderer worker died from signal %d; submit again to retry",
                WTERMSIG(state.child_status));
        else if (state.child_status_known && WIFEXITED(state.child_status))
            (void)snprintf(message, sizeof(message),
                "Renderer worker exited with status %d; submit again to retry",
                WEXITSTATUS(state.child_status));
        else
            (void)snprintf(message, sizeof(message),
                "Renderer worker disappeared; submit again to retry");
        set_error(failure, Code::Io, message);
        complete_failure(state, failure);
        return;
    }
    state.request_offset = 0;
    state.request_sent = false;
    state.decoder.reset();
    state.artifact_active = false;
    state.artifact_received = 0;
    state.artifacts_received = 0;
    state.baseline = 0;
    state.candidate = Rendered();
    state.candidate.snapshot = state.snapshot;
    state.candidate.formula_id = state.snapshot.formula.id;
    state.candidate.request_id = state.snapshot.request_id;
    state.candidate.generation = state.snapshot.formula.generation;
}

void read_worker(ClientState &state) noexcept {
    uint8_t buffer[kPumpIoBytes];
    const ssize_t received = read(state.output_fd, buffer, sizeof(buffer));
    if (received > 0) {
        if (!state.active) {
            if (state.result_ready && state.snapshot.request_id)
                protocol_failure(state, "Renderer emitted data after request completion");
            else
                request_worker_stop(state);
            return;
        }
        Error error;
        if (!state.decoder.feed(buffer, static_cast<size_t>(received), error)) {
            protocol_failure(state, error.message.c_str());
            return;
        }
        Frame frame;
        while (state.decoder.take(frame)) {
            if (!handle_frame(state, frame)) return;
        }
        if (!state.active && state.result_ready && state.decoder.has_partial_frame())
            protocol_failure(state, "Trailing partial frame after render completion");
        return;
    }
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (received < 0) {
        close_fd(state.output_fd);
        close_fd(state.input_fd);
        if (state.child > 0) (void)kill(state.child, SIGKILL);
        if (state.child_reaped) finish_reaped_worker(state);
        return;
    }

    if (state.active) {
        if (state.artifact_active) {
            protocol_failure(state, "EOF before the declared artifact completed");
            return;
        }
        Error error;
        if (!state.decoder.finish(error)) {
            protocol_failure(state, error.message.c_str());
            return;
        }
    }
    close_fd(state.output_fd);
    if (state.child_reaped) finish_reaped_worker(state);
    else if (state.child > 0) {
        close_fd(state.input_fd);
        if (!state.active) state.intentional_stop = true;
        (void)kill(state.child, SIGKILL);
    }
}

void write_worker(ClientState &state) noexcept {
    if (state.input_fd < 0 || !state.active || state.request_offset >= state.request_wire.size()) return;
    size_t count = state.request_wire.size() - state.request_offset;
    if (count > kPumpIoBytes) count = kPumpIoBytes;
    const ssize_t written = write_without_sigpipe(state.input_fd,
        state.request_wire.data() + state.request_offset, count);
    if (written > 0) {
        state.request_offset += static_cast<size_t>(written);
        if (state.request_offset == state.request_wire.size()) state.request_sent = true;
        return;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (state.active) retry_or_fail(state);
    request_worker_stop(state);
}

} // namespace

RenderClient::~RenderClient() {
    ClientState *state = state_of(state_);
    if (!state) return;
    request_worker_stop(*state);
    if (state->child > 0) {
        int status = 0;
        pid_t result;
        do { result = waitpid(state->child, &status, 0); }
        while (result < 0 && errno == EINTR);
        state->child = -1;
    }
    state->~ClientState();
    free(state);
    state_ = nullptr;
}

bool RenderClient::start(const char *worker_path, Error &error) noexcept {
    error.clear();
    if (!worker_path || !worker_path[0]) {
        set_error(error, Code::Format, "Renderer worker path is empty");
        return false;
    }
    size_t length = 0;
    while (length < kWorkerPathBytes && worker_path[length]) ++length;
    if (length == kWorkerPathBytes) {
        set_error(error, Code::Limit, "Renderer worker path is too long");
        return false;
    }
    ClientState *state = state_of(state_);
    if (!state) {
        void *memory = detail::allocate(sizeof(ClientState));
        if (!memory) {
            set_error(error, Code::NoMemory, "Cannot allocate renderer client");
            return false;
        }
        state = new (memory, Placement{}) ClientState();
        state_ = state;
    }
    if (state->active) {
        set_error(error, Code::Conflict, "Cannot change worker path during a render");
        return false;
    }
    const bool same_path = state->configured && strcmp(state->worker_path, worker_path) == 0;
    if (same_path && (state->child > 0 || state->output_fd >= 0)) return true;
    if (!same_path && (state->child > 0 || state->child_reaped || state->output_fd >= 0)) {
        set_error(error, Code::Conflict, "Cannot change worker path while its child is being reaped");
        return false;
    }
    if (state->child_reaped) finish_reaped_worker(*state);
    memcpy(state->worker_path, worker_path, length + 1);
    state->configured = true;
    return spawn_worker(*state, error);
}

bool RenderClient::submit(const Snapshot &snapshot, Error &error) noexcept {
    error.clear();
    ClientState *state = state_of(state_);
    if (!state || !state->configured) {
        set_error(error, Code::Conflict, "Renderer worker path has not been configured");
        return false;
    }

    Bytes request;
    if (!make_request(snapshot, request, error)) return false;

    Snapshot retained = snapshot;
    const bool replacing_active = state->active;
    if (replacing_active) request_worker_stop(*state);
    else if (state->child > 0) {
        state->child_request_id = snapshot.request_id;
        state->child_formula_id = snapshot.formula.id;
        state->child_generation = snapshot.formula.generation;
    }
    state->result_ready = false;
    state->result_error.clear();
    state->snapshot = static_cast<Snapshot &&>(retained);
    state->request_wire = static_cast<Bytes &&>(request);
    state->request_offset = 0;
    state->request_sent = false;
    state->started_ms = monotonic_ms();
    state->decoder.reset();
    state->candidate = Rendered();
    state->candidate.snapshot = state->snapshot;
    state->candidate.formula_id = snapshot.formula.id;
    state->candidate.request_id = snapshot.request_id;
    state->candidate.generation = snapshot.formula.generation;
    state->artifact_active = false;
    state->artifact_received = 0;
    state->artifacts_received = 0;
    state->baseline = 0;
    state->active = true;
    return true;
}

void RenderClient::pump() noexcept {
    ClientState *state = state_of(state_);
    if (!state) return;

    reap_nonblocking(*state);
    const uint64_t now = monotonic_ms();
    if (state->active && now >= state->started_ms &&
        now - state->started_ms >= kRequestTimeoutMs) {
        Error failure;
        set_error(failure, Code::Timeout, "Renderer request exceeded 10 seconds");
        state->active = false;
        request_worker_stop(*state);
        complete_failure(*state, failure);
    }

    if (state->active && state->child < 0 && state->output_fd < 0) {
        Error failure;
        if (!spawn_worker(*state, failure)) {
            complete_failure(*state, failure);
            return;
        }
    }
    if (state->child < 0 && state->output_fd < 0) return;

    pollfd descriptors[2]{};
    nfds_t count = 0;
    int input_index = -1;
    int output_index = -1;
    if (state->input_fd >= 0 && state->active && state->request_offset < state->request_wire.size()) {
        input_index = static_cast<int>(count);
        descriptors[count].fd = state->input_fd;
        descriptors[count].events = POLLOUT;
        ++count;
    }
    if (state->output_fd >= 0) {
        output_index = static_cast<int>(count);
        descriptors[count].fd = state->output_fd;
        descriptors[count].events = POLLIN;
        ++count;
    }
    if (count) {
        const int ready = poll(descriptors, count, 0);
        if (ready > 0) {
            if (input_index >= 0 &&
                (descriptors[input_index].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
                write_worker(*state);
            if (output_index >= 0 && state->output_fd >= 0 &&
                (descriptors[output_index].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
                read_worker(*state);
        } else if (ready < 0 && errno != EINTR) {
            if (state->active) {
                Error failure;
                set_error(failure, Code::Io, "Cannot poll renderer pipes");
                state->active = false;
                request_worker_stop(*state);
                complete_failure(*state, failure);
            } else {
                request_worker_stop(*state);
            }
            return;
        }
    }
    reap_nonblocking(*state);
    if (state->active && state->child < 0 && state->output_fd < 0) {
        Error failure;
        if (!spawn_worker(*state, failure)) complete_failure(*state, failure);
    }
}

bool RenderClient::take(Rendered &rendered, Error &error) noexcept {
    ClientState *state = state_of(state_);
    if (!state || !state->result_ready) return false;
    rendered = static_cast<Rendered &&>(state->candidate);
    error = state->result_error;
    state->candidate = Rendered();
    state->result_error.clear();
    state->result_ready = false;
    if (!state->active) {
        state->snapshot = Snapshot();
        state->decoder.reset();
    }
    return true;
}

bool RenderClient::busy() const noexcept {
    const ClientState *state = state_of(state_);
    return state && state->active;
}

void RenderClient::cancel() noexcept {
    ClientState *state = state_of(state_);
    if (!state || !state->active) return;
    Error failure;
    set_error(failure, Code::Cancelled, "Render request cancelled");
    state->active = false;
    request_worker_stop(*state);
    complete_failure(*state, failure);
    reap_nonblocking(*state);
}

} // namespace rm
