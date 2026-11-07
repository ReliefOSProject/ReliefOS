#if defined(RELIEFMAIL_TESTING) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "worker.h"
#include "tasks.h"
#include "vault.h"
#include "profile_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace rmail {
namespace {

constexpr size_t MaxSecretPacketBytes = 1024u * 1024u;
constexpr uint64_t ResolverWatchdogMs = 30000;
constexpr uint64_t CancelTermGraceMs = 250;
constexpr uint64_t CancelKillGraceMs = 250;
constexpr size_t MaxWorkerDiagnosticBytes = 4096;
constexpr size_t MaxPathBytes = 4096;
constexpr size_t MaxSecretPipeBytes = 1024u * 1024u;

/* Coordinator calls are confined to the GUI thread. Never recycle a profile
 * epoch inside this process, including across close()/open(). */
uint64_t next_process_profile_epoch = 1;

uint64_t allocate_profile_epoch() {
    if (!next_process_profile_epoch) return 0;
    const uint64_t result = next_process_profile_epoch;
    next_process_profile_epoch = result == UINT64_MAX ? 0 : result + 1;
    return result;
}

bool set_error(Text &error, const char *message) {
    error.clear();
    if (!error.set(message)) error.set("worker error");
    return false;
}

bool valid_task_kind(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::Initialize:
    case WorkerTaskKind::AccountTest:
    case WorkerTaskKind::FolderList:
    case WorkerTaskKind::FolderSelect:
    case WorkerTaskKind::HeaderSync:
    case WorkerTaskKind::LocalPage:
    case WorkerTaskKind::LocalSearch:
    case WorkerTaskKind::ServerSearch:
    case WorkerTaskKind::ParseMessage:
    case WorkerTaskKind::SaveDraft:
    case WorkerTaskKind::LoadDraft:
    case WorkerTaskKind::DeleteDraft:
    case WorkerTaskKind::QueueSend:
    case WorkerTaskKind::SendQueued:
    case WorkerTaskKind::RetryArchive:
    case WorkerTaskKind::SetFlags:
    case WorkerTaskKind::MoveMessage:
    case WorkerTaskKind::CopyMessage:
    case WorkerTaskKind::DeleteMessage:
    case WorkerTaskKind::FolderOperation:
    case WorkerTaskKind::ClearCache:
    case WorkerTaskKind::BackupProfile:
    case WorkerTaskKind::RestoreProfile:
    case WorkerTaskKind::UnlockVault:
    case WorkerTaskKind::SaveVault:
    case WorkerTaskKind::AccountList:
    case WorkerTaskKind::AccountCreate:
    case WorkerTaskKind::AccountUpdate:
    case WorkerTaskKind::AccountDelete:
    case WorkerTaskKind::AccountSetDefault:
    case WorkerTaskKind::AccountTestImap:
    case WorkerTaskKind::AccountTestSmtp:
    case WorkerTaskKind::MessagePage:
    case WorkerTaskKind::MessageSearch:
    case WorkerTaskKind::ReadMessageChunk:
    case WorkerTaskKind::SaveDraftRevision:
    case WorkerTaskKind::ChangeVaultPassword:
    case WorkerTaskKind::CancelQueuedSend:
    case WorkerTaskKind::FetchMessage:
    case WorkerTaskKind::VaultStatus:
    case WorkerTaskKind::ListDrafts:
    case WorkerTaskKind::SaveForwardDraft:
    case WorkerTaskKind::SaveDraftCopy:
    case WorkerTaskKind::ComposeCatalog:
    case WorkerTaskKind::ListSendOperations:
    case WorkerTaskKind::ReviewArchiveTarget:
    case WorkerTaskKind::ResolveUncertainSend:
#ifdef RELIEFMAIL_TESTING
    case WorkerTaskKind::TestProbe:
    case WorkerTaskKind::TestCrash:
    case WorkerTaskKind::TestWait:
    case WorkerTaskKind::TestEof:
    case WorkerTaskKind::TestSecretOutput:
    case WorkerTaskKind::TestSecretInterleave:
    case WorkerTaskKind::TestSecretPipeBlock:
    case WorkerTaskKind::TestNetworkWait:
    case WorkerTaskKind::TestMutationWait:
    case WorkerTaskKind::TestNetworkMutationWait:
    case WorkerTaskKind::TestMaintenanceWait:
#endif
        return true;
    }
    return false;
}

bool valid_utf8_field(const Text &value, size_t maximum, bool allow_empty = true) {
    if (value.size() > maximum || (!allow_empty && value.empty()) ||
        (value.size() && memchr(value.c_str(), 0, value.size()))) return false;
    return valid_utf8(value.c_str(), value.size());
}

class Writer {
    unsigned char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
    bool reserve(size_t extra) {
        if (extra > limit_ - size_) return false;
        const size_t wanted = size_ + extra;
        if (wanted <= capacity_) return true;
        size_t next = capacity_ ? capacity_ : 256;
        while (next < wanted) {
            if (next > limit_ / 2) { next = limit_; break; }
            next *= 2;
        }
        unsigned char *replacement = static_cast<unsigned char *>(realloc(bytes_, next));
        if (!replacement) return false;
        bytes_ = replacement;
        capacity_ = next;
        return true;
    }
public:
    explicit Writer(size_t limit) : limit_(limit) {}
    ~Writer() { free(bytes_); }
    bool raw(const void *source, size_t size) {
        if (!reserve(size)) return false;
        if (size) memcpy(bytes_ + size_, source, size);
        size_ += size;
        return true;
    }
    bool u8(uint8_t value) { return raw(&value, 1); }
    bool u16(uint16_t value) {
        unsigned char out[2] = {static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        return raw(out, sizeof(out));
    }
    bool u32(uint32_t value) {
        unsigned char out[4] = {static_cast<unsigned char>(value >> 24),
            static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 8),
            static_cast<unsigned char>(value)};
        return raw(out, sizeof(out));
    }
    bool u64(uint64_t value) {
        unsigned char out[8];
        for (size_t i = 0; i < 8; ++i) out[i] = static_cast<unsigned char>(value >> (56 - i * 8));
        return raw(out, sizeof(out));
    }
    bool text16(const Text &value) {
        return value.size() <= UINT16_MAX && u16(static_cast<uint16_t>(value.size())) &&
            raw(value.c_str(), value.size());
    }
    bool text32(const Text &value) {
        return value.size() <= UINT32_MAX && u32(static_cast<uint32_t>(value.size())) &&
            raw(value.c_str(), value.size());
    }
    bool bytes32(const Bytes &value) {
        return value.size() <= UINT32_MAX && u32(static_cast<uint32_t>(value.size())) &&
            raw(value.data(), value.size());
    }
    bool finish(Bytes &output) {
        const bool ok = output.set(bytes_, size_);
        if (bytes_) { detail::wipe_memory(bytes_, capacity_); free(bytes_); bytes_ = nullptr; }
        size_ = capacity_ = 0;
        return ok;
    }
    size_t size() const { return size_; }
    const unsigned char *data() const { return bytes_; }
};

class Reader {
    const unsigned char *bytes_;
    size_t size_;
    size_t at_ = 0;
public:
    Reader(const unsigned char *bytes, size_t size) : bytes_(bytes), size_(size) {}
    bool raw(void *output, size_t size) {
        if (size > size_ - at_) return false;
        if (size) memcpy(output, bytes_ + at_, size);
        at_ += size;
        return true;
    }
    bool view(const unsigned char *&output, size_t size) {
        if (size > size_ - at_) return false;
        output = bytes_ + at_;
        at_ += size;
        return true;
    }
    bool u8(uint8_t &value) { return raw(&value, 1); }
    bool u16(uint16_t &value) {
        unsigned char b[2]; if (!raw(b, 2)) return false;
        value = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]); return true;
    }
    bool u32(uint32_t &value) {
        unsigned char b[4]; if (!raw(b, 4)) return false;
        value = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
            (static_cast<uint32_t>(b[2]) << 8) | b[3]; return true;
    }
    bool u64(uint64_t &value) {
        unsigned char b[8]; if (!raw(b, 8)) return false;
        value = 0; for (size_t i = 0; i < 8; ++i) value = (value << 8) | b[i]; return true;
    }
    bool text16(Text &value, size_t maximum) {
        uint16_t size = 0; const unsigned char *data = nullptr;
        return u16(size) && size <= maximum && view(data, size) &&
            value.set(reinterpret_cast<const char *>(data), size);
    }
    bool text32(Text &value, size_t maximum) {
        uint32_t size = 0; const unsigned char *data = nullptr;
        return u32(size) && size <= maximum && view(data, size) &&
            value.set(reinterpret_cast<const char *>(data), size);
    }
    bool bytes32(Bytes &value, size_t maximum) {
        uint32_t size = 0; const unsigned char *data = nullptr;
        return u32(size) && size <= maximum && view(data, size) && value.set(data, size);
    }
    size_t remaining() const { return size_ - at_; }
};

bool frame_body(const unsigned char *frame, size_t size, const unsigned char *&body,
    size_t &body_size, Text &error) {
    if (!frame || size < 8 || size > MaxWorkerFrameBytes) return set_error(error, "worker frame size is invalid");
    size_t declared = 0;
    if (!worker_decode_frame_length(frame, declared, error) || declared != size - 4)
        return set_error(error, "worker frame length does not match its contents");
    body = frame + 4;
    body_size = declared;
    return true;
}

bool worker_write_all(int fd, const unsigned char *bytes, size_t size) {
    size_t at = 0;
    while (at < size) {
        const ssize_t count = write(fd, bytes + at, size - at);
        if (count > 0) { at += static_cast<size_t>(count); continue; }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool worker_read_all(int fd, unsigned char *bytes, size_t size, bool allow_clean_eof, bool &eof) {
    size_t at = 0;
    eof = false;
    while (at < size) {
        const ssize_t count = read(fd, bytes + at, size - at);
        if (count > 0) { at += static_cast<size_t>(count); continue; }
        if (!count) {
            eof = true;
            return allow_clean_eof && at == 0;
        }
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

uint64_t monotonic_ms() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000u + static_cast<uint64_t>(now.tv_nsec / 1000000u);
}

bool cancellation_check(void *context) {
    const int fd = *static_cast<int *>(context);
    unsigned char signal = 0;
    const ssize_t count = read(fd, &signal, 1);
    return count == 1;
}

#ifdef RELIEFMAIL_TESTING
bool wait_test_secret_release(unsigned char expected) {
    unsigned char release = 0;
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        const ssize_t count = read(4, &release, 1);
        if (count == 1) return release == expected;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
        return false;
    }
    return false;
}

bool read_secret_pipe(SecretBytes &secret, bool pause_before_header, bool *forced_body_eagain) {
    if (forced_body_eagain) *forced_body_eagain = false;
    if (pause_before_header && !wait_test_secret_release(0x5a)) { close(3); return false; }
    if (pause_before_header && forced_body_eagain)
        *forced_body_eagain = true;
#else
bool read_secret_pipe(SecretBytes &secret) {
#endif
    unsigned char header[4];
    bool eof = false;
    if (!worker_read_all(3, header, sizeof(header), false, eof)) return false;
    const uint32_t length = (static_cast<uint32_t>(header[0]) << 24) |
        (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8) | header[3];
    if (length > MaxSecretPacketBytes) return false;
#ifdef RELIEFMAIL_TESTING
    if (pause_before_header && !wait_test_secret_release(0x5b)) { close(3); return false; }
#endif
    unsigned char *bytes = length ? static_cast<unsigned char *>(malloc(length)) : nullptr;
    if (length && !bytes) return false;
    const bool read_ok = !length || worker_read_all(3, bytes, length, false, eof);
    const bool copied = read_ok && secret.set(bytes, length);
    if (bytes) { detail::wipe_memory(bytes, length); free(bytes); }
    close(3);
    return copied;
}

void close_child_descriptors() {
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576) maximum = 1048576;
    for (int fd = 6; fd < maximum; ++fd) close(fd);
}

bool set_close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool make_pipe(int pipe_fds[2]) {
    if (pipe(pipe_fds) < 0) return false;
    if (!set_close_on_exec(pipe_fds[0]) || !set_close_on_exec(pipe_fds[1])) {
        close(pipe_fds[0]); close(pipe_fds[1]); pipe_fds[0] = pipe_fds[1] = -1;
        return false;
    }
    return true;
}

int duplicate_child_end(int fd) {
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 10);
#else
    int copy = fcntl(fd, F_DUPFD, 10);
    if (copy >= 0 && !set_close_on_exec(copy)) { close(copy); return -1; }
    return copy;
#endif
}

struct Job {
    enum State : uint8_t { Free, Queued, Running, Complete } state = Free;
    uint64_t sequence = 0;
    WorkerRequest request;
    WorkerResult result;
    Bytes encoded_request;
    SecretBytes secret;
    SecretBytes received_secret;
    pid_t pid = -1;
    int request_fd = -1;
    int response_fd = -1;
    int secret_fd = -1;
    int secret_result_fd = -1;
    int cancel_fd = -1;
    size_t request_offset = 0;
    size_t secret_header_offset = 0;
    size_t secret_offset = 0;
    bool secret_header_sent = false;
    unsigned char secret_length[4] = {0, 0, 0, 0};
    unsigned char *response = nullptr;
    size_t response_size = 0;
    size_t response_capacity = 0;
    size_t response_expected = 0;
    bool response_eof = false;
    bool response_received = false;
    unsigned char *secret_response = nullptr;
    size_t secret_response_size = 0;
    size_t secret_response_capacity = 0;
    size_t secret_response_expected = SIZE_MAX;
    bool secret_response_eof = false;
    bool secret_response_received = false;
    bool cancelled = false;
    bool timed_out = false;
    bool io_failed = false;
    bool cancel_written = false;
    bool epoch_transition_result = false;
#ifdef RELIEFMAIL_TESTING
    bool interleave_ack_sent = false;
    bool secret_test_header_written = false;
    bool secret_test_body_blocked = false;
    bool secret_test_body_release_sent = false;
    bool secret_test_payload_release_sent = false;
    uint32_t secret_test_pump_count = 0;
    uint32_t secret_test_release_pump = 0;
#endif
    uint64_t started_ms = 0;
    uint64_t cancel_started_ms = 0;
    bool reaped = false;
    int wait_status = 0;
};

struct CoordinatorState {
    char executable[PATH_MAX];
    ProfilePaths paths;
    int config_fd = -1;
    int lock_fd = -1;
    Job jobs[MaxWorkerOutstanding];
    uint64_t last_request_id = 0;
    uint64_t next_sequence = 1;
    uint64_t epoch = 1;
    bool shutting_down = false;
    bool recovery_required = false;
    bool sigpipe_changed = false;
    struct sigaction old_sigpipe;
};

void close_fd(int &fd) { if (fd >= 0) close(fd); fd = -1; }

void clear_secret_response(Job &job) {
    if (job.secret_response) {
        detail::wipe_memory(job.secret_response, job.secret_response_capacity);
        free(job.secret_response);
    }
    job.secret_response = nullptr;
    job.secret_response_size = job.secret_response_capacity = 0;
    job.secret_response_expected = SIZE_MAX;
}

void cleanup_job(Job &job) {
    close_fd(job.request_fd);
    close_fd(job.response_fd);
    close_fd(job.secret_fd);
    close_fd(job.secret_result_fd);
    close_fd(job.cancel_fd);
    if (job.response) { detail::wipe_memory(job.response, job.response_capacity); free(job.response); }
    job.response = nullptr;
    job.response_size = job.response_capacity = job.response_expected = 0;
    clear_secret_response(job);
    job.secret.clear();
    job.received_secret.clear();
    job.encoded_request.clear();
    job.result = WorkerResult();
    job.request = WorkerRequest();
    job.state = Job::Free;
    job.pid = -1;
    job.sequence = 0;
    job.request_offset = 0;
    job.secret_header_offset = 0;
    job.secret_offset = 0;
    job.secret_header_sent = false;
    memset(job.secret_length, 0, sizeof(job.secret_length));
    job.response_expected = 0;
    job.response_eof = false;
    job.response_received = false;
    job.secret_response_expected = SIZE_MAX;
    job.secret_response_eof = false;
    job.secret_response_received = false;
    job.cancelled = false;
    job.timed_out = false;
    job.io_failed = false;
    job.cancel_written = false;
    job.epoch_transition_result = false;
#ifdef RELIEFMAIL_TESTING
    job.interleave_ack_sent = false;
    job.secret_test_header_written = false;
    job.secret_test_body_blocked = false;
    job.secret_test_body_release_sent = false;
    job.secret_test_payload_release_sent = false;
    job.secret_test_pump_count = 0;
    job.secret_test_release_pump = 0;
#endif
    job.started_ms = 0;
    job.cancel_started_ms = 0;
    job.reaped = false;
    job.wait_status = 0;
}

void complete_without_child(Job &job, WorkerStatus status, const char *message) {
    job.received_secret.clear();
    job.result = WorkerResult();
    job.result.request_id = job.request.request_id;
    job.result.profile_epoch = job.request.profile_epoch;
    job.result.ui_generation = job.request.ui_generation;
    job.result.account_id = job.request.account_id;
    job.result.account_generation = job.request.account_generation;
    job.result.folder_generation = job.request.folder_generation;
    job.result.folder_uidvalidity = job.request.folder_uidvalidity;
    job.result.kind = job.request.kind;
    job.result.status = status;
    job.result.diagnostic.set(message);
    job.state = Job::Complete;
    job.epoch_transition_result = false;
    job.secret.clear();
    job.encoded_request.clear();
}

bool task_is_network(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::AccountTest:
    case WorkerTaskKind::AccountTestImap:
    case WorkerTaskKind::AccountTestSmtp:
    case WorkerTaskKind::FolderList:
    case WorkerTaskKind::FolderSelect:
    case WorkerTaskKind::HeaderSync:
    case WorkerTaskKind::ServerSearch:
    case WorkerTaskKind::SendQueued:
    case WorkerTaskKind::RetryArchive:
    case WorkerTaskKind::SetFlags:
    case WorkerTaskKind::MoveMessage:
    case WorkerTaskKind::CopyMessage:
    case WorkerTaskKind::DeleteMessage:
    case WorkerTaskKind::FolderOperation:
    case WorkerTaskKind::FetchMessage:
        return true;
#ifdef RELIEFMAIL_TESTING
    case WorkerTaskKind::TestNetworkWait:
    case WorkerTaskKind::TestNetworkMutationWait:
        return true;
#endif
    default: return false;
    }
}

bool task_is_mutation(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::AccountCreate:
    case WorkerTaskKind::AccountUpdate:
    case WorkerTaskKind::AccountDelete:
    case WorkerTaskKind::AccountSetDefault:
    case WorkerTaskKind::FolderList:
    case WorkerTaskKind::FolderSelect:
    case WorkerTaskKind::HeaderSync:
    case WorkerTaskKind::SaveDraft:
    case WorkerTaskKind::SaveDraftRevision:
    case WorkerTaskKind::SaveForwardDraft:
    case WorkerTaskKind::SaveDraftCopy:
    case WorkerTaskKind::DeleteDraft:
    case WorkerTaskKind::QueueSend:
    case WorkerTaskKind::SendQueued:
    case WorkerTaskKind::RetryArchive:
    case WorkerTaskKind::SetFlags:
    case WorkerTaskKind::MoveMessage:
    case WorkerTaskKind::CopyMessage:
    case WorkerTaskKind::DeleteMessage:
    case WorkerTaskKind::FolderOperation:
    case WorkerTaskKind::ServerSearch:
    case WorkerTaskKind::FetchMessage:
    case WorkerTaskKind::CancelQueuedSend:
    case WorkerTaskKind::ReviewArchiveTarget:
    case WorkerTaskKind::ResolveUncertainSend:
        return true;
#ifdef RELIEFMAIL_TESTING
    case WorkerTaskKind::TestMutationWait:
    case WorkerTaskKind::TestNetworkMutationWait:
        return true;
#endif
    default: return false;
    }
}

bool task_is_maintenance(WorkerTaskKind kind) {
#ifdef RELIEFMAIL_TESTING
    if (kind == WorkerTaskKind::TestMaintenanceWait) return true;
#endif
    return kind == WorkerTaskKind::Initialize || kind == WorkerTaskKind::ClearCache ||
        kind == WorkerTaskKind::BackupProfile || kind == WorkerTaskKind::RestoreProfile ||
        kind == WorkerTaskKind::SaveVault || kind == WorkerTaskKind::UnlockVault ||
        kind == WorkerTaskKind::ChangeVaultPassword || kind == WorkerTaskKind::VaultStatus;
}

bool active_account_mutation(const CoordinatorState &state, uint64_t account_id) {
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        const Job &job = state.jobs[i];
        if (job.state == Job::Running && task_is_mutation(job.request.kind) &&
            (!account_id || !job.request.account_id || job.request.account_id == account_id)) return true;
    }
    return false;
}

size_t active_count(const CoordinatorState &state) {
    size_t count = 0;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state.jobs[i].state == Job::Running) ++count;
    return count;
}

size_t active_network_count(const CoordinatorState &state) {
    size_t count = 0;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state.jobs[i].state == Job::Running && task_is_network(state.jobs[i].request.kind)) ++count;
    return count;
}

bool before_first_maintenance(const CoordinatorState &state, const Job &candidate) {
    uint64_t barrier = UINT64_MAX;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        const Job &job = state.jobs[i];
        if ((job.state == Job::Queued || job.state == Job::Running) && task_is_maintenance(job.request.kind) &&
            job.sequence < barrier) barrier = job.sequence;
    }
    return candidate.sequence <= barrier;
}

size_t active_local_count(const CoordinatorState &state) {
    size_t count = 0;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state.jobs[i].state == Job::Running && !task_is_network(state.jobs[i].request.kind)) ++count;
    return count;
}

bool eligible_to_start(const CoordinatorState &state, const Job &candidate) {
    if (candidate.state != Job::Queued || !before_first_maintenance(state, candidate) ||
        active_count(state) >= 4) return false;
    if (task_is_maintenance(candidate.request.kind)) return active_count(state) == 0;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state.jobs[i].state == Job::Running && task_is_maintenance(state.jobs[i].request.kind)) return false;
    if (task_is_network(candidate.request.kind)) {
        if (active_network_count(state) >= 2) return false;
    } else if (active_local_count(state) >= 2) return false;
    return !task_is_mutation(candidate.request.kind) ||
        !active_account_mutation(state, candidate.request.account_id);
}

size_t next_eligible_job(const CoordinatorState &state) {
    size_t candidate = MaxWorkerOutstanding;
    uint64_t sequence = UINT64_MAX;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        const Job &job = state.jobs[i];
        if (job.sequence < sequence && eligible_to_start(state, job)) {
            candidate = i;
            sequence = job.sequence;
        }
    }
    return candidate;
}

int spawn_job(CoordinatorState &state, Job &job) {
    int requests[2] = {-1, -1};
    int responses[2] = {-1, -1};
    int secrets[2] = {-1, -1};
    int secret_results[2] = {-1, -1};
    int cancellations[2] = {-1, -1};
    if (!make_pipe(requests) || !make_pipe(responses) ||
        !make_pipe(secrets) || !make_pipe(secret_results) || !make_pipe(cancellations)) {
        close_fd(requests[0]); close_fd(requests[1]); close_fd(responses[0]); close_fd(responses[1]);
        close_fd(secrets[0]); close_fd(secrets[1]); close_fd(cancellations[0]); close_fd(cancellations[1]);
        close_fd(secret_results[0]); close_fd(secret_results[1]);
        return -1;
    }
#ifdef RELIEFMAIL_TESTING
    if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock &&
        fcntl(secrets[1], F_SETPIPE_SZ, 4096) < 4096) {
        close_fd(requests[0]); close_fd(requests[1]); close_fd(responses[0]); close_fd(responses[1]);
        close_fd(secrets[0]); close_fd(secrets[1]); close_fd(cancellations[0]); close_fd(cancellations[1]);
        close_fd(secret_results[0]); close_fd(secret_results[1]);
        return -1;
    }
#endif
    const int child_in = duplicate_child_end(requests[0]);
    const int child_out = duplicate_child_end(responses[1]);
    const int child_secret = duplicate_child_end(secrets[0]);
    const int child_secret_out = duplicate_child_end(secret_results[1]);
    const int child_cancel = duplicate_child_end(cancellations[0]);
    if (child_in < 0 || child_out < 0 || child_secret < 0 || child_secret_out < 0 || child_cancel < 0) {
        close_fd(requests[0]); close_fd(requests[1]); close_fd(responses[0]); close_fd(responses[1]);
        close_fd(secrets[0]); close_fd(secrets[1]); close_fd(cancellations[0]); close_fd(cancellations[1]);
        if (child_in >= 0) close(child_in);
        if (child_out >= 0) close(child_out);
        if (child_secret >= 0) close(child_secret);
        if (child_cancel >= 0) close(child_cancel);
        if (child_secret_out >= 0) close(child_secret_out);
        close_fd(secret_results[0]); close_fd(secret_results[1]);
        return -1;
    }
    if (!set_nonblocking(requests[1]) || !set_nonblocking(responses[0]) ||
        !set_nonblocking(secrets[1]) || !set_nonblocking(secret_results[0]) || !set_nonblocking(cancellations[0]) ||
        !set_nonblocking(cancellations[1])) {
        close(requests[0]); close(requests[1]); close(responses[0]); close(responses[1]);
        close(secrets[0]); close(secrets[1]); close(cancellations[0]); close(cancellations[1]);
        close(secret_results[0]); close(secret_results[1]);
        close(child_in); close(child_out); close(child_secret); close(child_secret_out); close(child_cancel);
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(requests[0]); close(requests[1]); close(responses[0]); close(responses[1]);
        close(secrets[0]); close(secrets[1]); close(cancellations[0]); close(cancellations[1]);
        close(secret_results[0]); close(secret_results[1]);
        close(child_in); close(child_out); close(child_secret); close(child_secret_out); close(child_cancel);
        return -1;
    }
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (dup2(child_in, STDIN_FILENO) < 0 || dup2(child_out, STDOUT_FILENO) < 0 ||
            dup2(child_secret, 3) < 0 || dup2(child_cancel, 4) < 0 || dup2(child_secret_out, 5) < 0 ||
            (null_fd >= 0 && dup2(null_fd, STDERR_FILENO) < 0)) _exit(126);
        close_child_descriptors();
        char *const arguments[] = {const_cast<char *>("reliefmail"),
            const_cast<char *>("--reliefmail-worker"), nullptr};
        execve(state.executable, arguments, environ);
        _exit(127);
    }
    close(requests[0]); close(responses[1]); close(secrets[0]); close(secret_results[1]); close(cancellations[0]);
    close(child_in); close(child_out); close(child_secret); close(child_secret_out); close(child_cancel);
    job.pid = pid;
    job.request_fd = requests[1];
    job.response_fd = responses[0];
    job.secret_fd = secrets[1];
    job.secret_result_fd = secret_results[0];
    job.cancel_fd = cancellations[1];
    job.started_ms = monotonic_ms();
    job.secret_length[0] = static_cast<unsigned char>(job.secret.size() >> 24);
    job.secret_length[1] = static_cast<unsigned char>(job.secret.size() >> 16);
    job.secret_length[2] = static_cast<unsigned char>(job.secret.size() >> 8);
    job.secret_length[3] = static_cast<unsigned char>(job.secret.size());
    job.state = Job::Running;
    return 0;
}

bool write_pending(int &fd, const unsigned char *bytes, size_t size, size_t &offset,
    bool close_when_done = true) {
    if (fd < 0) return true;
    while (offset < size) {
        const ssize_t written = write(fd, bytes + offset, size - offset);
        if (written > 0) { offset += static_cast<size_t>(written); continue; }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    if (close_when_done) close_fd(fd);
    return true;
}

bool advance_secret_pipe(Job &job) {
    if (job.secret_fd < 0) return true;
    const size_t header_size = sizeof(job.secret_length);
    if (!job.secret_header_sent) {
        size_t offset = job.secret_header_offset;
        if (!write_pending(job.secret_fd, job.secret_length, header_size, offset, false)) return false;
        job.secret_header_offset = offset;
        if (job.secret_fd < 0) return job.secret.empty();
        if (job.secret_header_offset < header_size) return true;
        job.secret_header_sent = true;
#ifdef RELIEFMAIL_TESTING
        if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock)
            job.secret_test_header_written = true;
#endif
    }
    if (job.secret.empty()) { close_fd(job.secret_fd); return true; }
    size_t offset = job.secret_offset;
    const bool ok = write_pending(job.secret_fd, job.secret.data(), job.secret.size(), offset);
    job.secret_offset = offset;
    if (!ok) return false;
#ifdef RELIEFMAIL_TESTING
    if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock && job.secret_fd >= 0 &&
        job.secret_offset == 0 && !job.secret.empty())
        job.secret_test_body_blocked = true;
#endif
    if (job.secret_fd < 0) job.secret.clear();
    return true;
}

bool append_response(Job &job, const unsigned char *bytes, size_t size) {
    if (size > MaxWorkerFrameBytes - job.response_size) return false;
    const size_t wanted = job.response_size + size;
    if (wanted > job.response_capacity) {
        size_t next = job.response_capacity ? job.response_capacity : 4096;
        while (next < wanted) {
            if (next > MaxWorkerFrameBytes / 2) { next = MaxWorkerFrameBytes; break; }
            next *= 2;
        }
        unsigned char *replacement = static_cast<unsigned char *>(realloc(job.response, next));
        if (!replacement) return false;
        job.response = replacement;
        job.response_capacity = next;
    }
    memcpy(job.response + job.response_size, bytes, size);
    job.response_size = wanted;
    if (!job.response_expected && job.response_size >= 4) {
        size_t body_size = 0;
        Text error;
        if (!worker_decode_frame_length(job.response, body_size, error)) return false;
        job.response_expected = body_size + 4;
    }
    if (job.response_expected && job.response_size > job.response_expected) return false;
    if (job.response_expected && job.response_size == job.response_expected) {
        Text error;
        WorkerResult decoded;
        if (!worker_decode_result(job.response, job.response_size, decoded, error) ||
            decoded.request_id != job.request.request_id ||
            decoded.profile_epoch != job.request.profile_epoch ||
            decoded.ui_generation != job.request.ui_generation ||
            decoded.account_id != job.request.account_id ||
            decoded.account_generation != job.request.account_generation ||
            decoded.folder_generation != job.request.folder_generation ||
            decoded.folder_uidvalidity != job.request.folder_uidvalidity ||
            decoded.kind != job.request.kind) return false;
        job.result = static_cast<WorkerResult &&>(decoded);
        job.response_received = true;
    }
    return true;
}

bool advance_response(Job &job) {
    unsigned char buffer[16384];
    for (;;) {
        const ssize_t count = read(job.response_fd, buffer, sizeof(buffer));
        if (count > 0) {
            if (!append_response(job, buffer, static_cast<size_t>(count))) return false;
            continue;
        }
        if (!count) { job.response_eof = true; close_fd(job.response_fd); return true; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

bool append_secret_response(Job &job, const unsigned char *bytes, size_t size) {
    if (size > MaxSecretPipeBytes + 4 - job.secret_response_size) return false;
    const size_t wanted = job.secret_response_size + size;
    if (wanted > job.secret_response_capacity) {
        size_t next = job.secret_response_capacity ? job.secret_response_capacity : 256;
        while (next < wanted) {
            if (next > (MaxSecretPipeBytes + 4) / 2) { next = MaxSecretPipeBytes + 4; break; }
            next *= 2;
        }
        unsigned char *replacement = static_cast<unsigned char *>(malloc(next));
        if (!replacement) return false;
        if (job.secret_response_size) memcpy(replacement, job.secret_response, job.secret_response_size);
        if (job.secret_response) {
            detail::wipe_memory(job.secret_response, job.secret_response_capacity);
            free(job.secret_response);
        }
        job.secret_response = replacement;
        job.secret_response_capacity = next;
    }
    if (size) memcpy(job.secret_response + job.secret_response_size, bytes, size);
    job.secret_response_size = wanted;
    if (job.secret_response_expected == SIZE_MAX && job.secret_response_size >= 4) {
        const uint32_t decoded = (static_cast<uint32_t>(job.secret_response[0]) << 24) |
            (static_cast<uint32_t>(job.secret_response[1]) << 16) |
            (static_cast<uint32_t>(job.secret_response[2]) << 8) | job.secret_response[3];
        if (decoded > MaxSecretPipeBytes) return false;
        job.secret_response_expected = static_cast<size_t>(decoded) + 4;
    }
    return job.secret_response_expected == SIZE_MAX || job.secret_response_size <= job.secret_response_expected;
}

bool advance_secret_response(Job &job) {
    unsigned char buffer[4096];
    for (;;) {
        const ssize_t count = read(job.secret_result_fd, buffer, sizeof(buffer));
        if (count > 0) {
            const bool appended = append_secret_response(job, buffer, static_cast<size_t>(count));
            detail::wipe_memory(buffer, sizeof(buffer));
            if (!appended) return false;
            continue;
        }
        if (!count) {
            detail::wipe_memory(buffer, sizeof(buffer));
            job.secret_response_eof = true;
            close_fd(job.secret_result_fd);
            if (job.secret_response_expected == SIZE_MAX ||
                job.secret_response_size != job.secret_response_expected) {
                clear_secret_response(job);
                return false;
            }
            const size_t secret_size = job.secret_response_expected - 4;
            const bool copied = job.received_secret.set(job.secret_response + 4, secret_size);
            clear_secret_response(job);
            if (!copied) return false;
            job.secret_response_received = true;
            return true;
        }
        detail::wipe_memory(buffer, sizeof(buffer));
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

void mark_cancel(Job &job, bool timed_out) {
    if (job.state != Job::Running || job.cancelled) return;
    job.cancelled = true;
    job.received_secret.clear();
    job.timed_out = timed_out;
    job.cancel_started_ms = monotonic_ms();
    if (job.request_fd >= 0) close_fd(job.request_fd);
    if (job.secret_fd >= 0) close_fd(job.secret_fd);
    job.secret.clear();
    if (job.cancel_fd >= 0) {
        const unsigned char byte = 1;
        const ssize_t ignored = write(job.cancel_fd, &byte, 1);
        (void)ignored;
        job.cancel_written = true;
    }
}

void fail_worker_io(Job &job) {
    if (job.state != Job::Running || job.cancelled) return;
    job.io_failed = true;
    close_fd(job.request_fd);
    close_fd(job.secret_fd);
    job.secret.clear();
    mark_cancel(job, false);
}

void cancellation_escalation(Job &job, uint64_t now) {
    if (!job.cancelled || job.reaped) return;
    const uint64_t elapsed = now - job.cancel_started_ms;
    if (elapsed >= CancelTermGraceMs && elapsed < CancelTermGraceMs + CancelKillGraceMs)
        kill(job.pid, SIGTERM);
    else if (elapsed >= CancelTermGraceMs + CancelKillGraceMs)
        kill(job.pid, SIGKILL);
}

void collect_exit(Job &job) {
    if (job.reaped || job.pid <= 0) return;
    const pid_t result = waitpid(job.pid, &job.wait_status, WNOHANG);
    if (result == job.pid || (result < 0 && errno == ECHILD)) job.reaped = true;
}

void invalidate_old_epoch_jobs(CoordinatorState &state, Job &transition, bool require_reopen) {
    const uint64_t next_epoch = allocate_profile_epoch();
    state.epoch = next_epoch;
    state.recovery_required = require_reopen || !next_epoch;
    transition.epoch_transition_result = true;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        Job &pending = state.jobs[i];
        if (&pending == &transition || pending.state == Job::Free) continue;
        if (pending.state == Job::Queued) {
            complete_without_child(pending, WorkerStatus::Cancelled,
                "profile generation changed before the queued request could start");
        } else if (pending.state == Job::Running) {
            mark_cancel(pending, false);
        } else if (pending.state == Job::Complete) {
            pending.result.secret_payload.clear();
            pending.received_secret.clear();
            pending.result.payload.clear();
            pending.result.status = WorkerStatus::Cancelled;
            pending.result.diagnostic.set("profile generation changed; completed result was invalidated");
            pending.epoch_transition_result = false;
        }
    }
}

void finalize_job(CoordinatorState &state, Job &job) {
    if (job.state != Job::Running || !job.reaped) return;
    const bool child_ok = WIFEXITED(job.wait_status) && WEXITSTATUS(job.wait_status) == 0;
    if (!job.response_received || !job.secret_response_received || !child_ok) {
        const uint64_t request_id = job.request.request_id;
        WorkerRequest request = static_cast<WorkerRequest &&>(job.request);
        WorkerResult failed;
        failed.request_id = request_id;
        failed.profile_epoch = request.profile_epoch;
        failed.ui_generation = request.ui_generation;
        failed.account_id = request.account_id;
        failed.account_generation = request.account_generation;
        failed.folder_generation = request.folder_generation;
        failed.folder_uidvalidity = request.folder_uidvalidity;
        failed.kind = request.kind;
        failed.status = job.io_failed ? WorkerStatus::Failed :
            job.cancelled && !job.timed_out ? WorkerStatus::Cancelled : WorkerStatus::Failed;
        failed.diagnostic.set(job.timed_out ? "worker operation exceeded its bounded deadline" :
            job.io_failed ? "worker IPC failed before a complete valid result was received" :
            job.cancelled ? "worker cancelled after bounded escalation" :
            "worker exited without a complete valid result");
        job.result = static_cast<WorkerResult &&>(failed);
    }
    const bool stale_epoch = job.request.profile_epoch != state.epoch;
    if (stale_epoch) {
        job.received_secret.clear();
        job.result.secret_payload.clear();
        job.result.payload.clear();
        job.result.status = WorkerStatus::Cancelled;
        job.result.diagnostic.set("worker result belongs to an invalidated profile generation");
    } else if (job.result.ok()) {
        job.result.secret_payload = static_cast<SecretBytes &&>(job.received_secret);
    }
    else job.received_secret.clear();
    if (!stale_epoch && job.request.kind == WorkerTaskKind::RestoreProfile &&
        (job.result.status == WorkerStatus::Ok || job.result.status == WorkerStatus::CommittedWarning ||
         job.result.status == WorkerStatus::RecoveryRequired)) {
        invalidate_old_epoch_jobs(state, job, true);
    } else if (!stale_epoch && job.result.status == WorkerStatus::RecoveryRequired) {
        invalidate_old_epoch_jobs(state, job, true);
    } else if (job.request.kind == WorkerTaskKind::Initialize &&
        job.result.status == WorkerStatus::Ok && !stale_epoch) {
        state.recovery_required = false;
    }
    close_fd(job.request_fd); close_fd(job.response_fd); close_fd(job.secret_fd);
    close_fd(job.secret_result_fd); close_fd(job.cancel_fd);
    job.secret.clear();
    job.encoded_request.clear();
    if (job.response) { detail::wipe_memory(job.response, job.response_capacity); free(job.response); }
    job.response = nullptr; job.response_size = job.response_capacity = 0;
    job.state = Job::Complete;
}

} // namespace

bool worker_decode_frame_length(const unsigned char header[4], size_t &length, Text &error) {
    error.clear();
    if (!header) return set_error(error, "worker frame header is missing");
    const uint32_t decoded = (static_cast<uint32_t>(header[0]) << 24) |
        (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8) | header[3];
    if (decoded < 4 || static_cast<size_t>(decoded) > MaxWorkerFrameBytes - 4)
        return set_error(error, "worker frame length exceeds its bound");
    length = decoded;
    return true;
}

bool worker_encode_request(const WorkerRequest &request, Bytes &frame, Text &error) {
    error.clear();
    if (!request.request_id || !request.profile_epoch || !valid_task_kind(request.kind) ||
        request.payload.size() > MaxWorkerRequestPayloadBytes ||
        !valid_utf8_field(request.folder_server_name, MaxEditableTextBytes) ||
        !valid_utf8_field(request.profile_paths.config_dir, MaxPathBytes) ||
        !valid_utf8_field(request.profile_paths.data_dir, MaxPathBytes) ||
        !valid_utf8_field(request.profile_paths.cache_dir, MaxPathBytes))
        return set_error(error, "worker request identity or payload is invalid");
    Writer body(MaxWorkerFrameBytes - 4);
    const unsigned char magic[4] = {'R', 'M', 'W', 'Q'};
    if (!body.raw(magic, sizeof(magic)) || !body.u16(WorkerProtocolVersion) ||
        !body.u16(static_cast<uint16_t>(request.kind)) || !body.u64(request.request_id) ||
        !body.u64(request.profile_epoch) || !body.u64(request.ui_generation) ||
        !body.u64(request.account_id) || !body.u64(request.account_generation) ||
        !body.u64(request.folder_generation) || !body.u64(request.folder_uidvalidity) ||
        !body.u32(request.message_uid) || !body.u64(request.deadline_monotonic_ms) ||
        !body.text16(request.folder_server_name) || !body.text16(request.profile_paths.config_dir) ||
        !body.text16(request.profile_paths.data_dir) || !body.text16(request.profile_paths.cache_dir) ||
        !body.bytes32(request.payload)) return set_error(error, "worker request exceeds its serialized bound");
    Bytes body_bytes;
    if (!body.finish(body_bytes)) return set_error(error, "out of memory serializing worker request");
    Writer full(MaxWorkerFrameBytes);
    if (!full.u32(static_cast<uint32_t>(body_bytes.size())) ||
        !full.raw(body_bytes.data(), body_bytes.size()) || !full.finish(frame))
        return set_error(error, "out of memory framing worker request");
    return true;
}

bool worker_decode_request(const unsigned char *frame, size_t size, WorkerRequest &request, Text &error) {
    error.clear();
    const unsigned char *body = nullptr;
    size_t body_size = 0;
    if (!frame_body(frame, size, body, body_size, error)) return false;
    Reader reader(body, body_size);
    unsigned char magic[4]; uint16_t version = 0, kind = 0;
    WorkerRequest next;
    if (!reader.raw(magic, 4) || memcmp(magic, "RMWQ", 4) || !reader.u16(version) ||
        version != WorkerProtocolVersion || !reader.u16(kind)) return set_error(error, "worker request header is invalid");
    next.kind = static_cast<WorkerTaskKind>(kind);
    if (!valid_task_kind(next.kind) || !reader.u64(next.request_id) || !reader.u64(next.profile_epoch) ||
        !reader.u64(next.ui_generation) || !reader.u64(next.account_id) ||
        !reader.u64(next.account_generation) || !reader.u64(next.folder_generation) ||
        !reader.u64(next.folder_uidvalidity) || !reader.u32(next.message_uid) ||
        !reader.u64(next.deadline_monotonic_ms) ||
        !reader.text16(next.folder_server_name, MaxEditableTextBytes) ||
        !reader.text16(next.profile_paths.config_dir, MaxPathBytes) ||
        !reader.text16(next.profile_paths.data_dir, MaxPathBytes) ||
        !reader.text16(next.profile_paths.cache_dir, MaxPathBytes) ||
        !reader.bytes32(next.payload, MaxWorkerRequestPayloadBytes) || reader.remaining() ||
        !next.request_id || !next.profile_epoch ||
        !valid_utf8_field(next.folder_server_name, MaxEditableTextBytes) ||
        !valid_utf8_field(next.profile_paths.config_dir, MaxPathBytes) ||
        !valid_utf8_field(next.profile_paths.data_dir, MaxPathBytes) ||
        !valid_utf8_field(next.profile_paths.cache_dir, MaxPathBytes))
        return set_error(error, "worker request body is malformed or exceeds its bound");
    request = static_cast<WorkerRequest &&>(next);
    return true;
}

bool worker_encode_result(const WorkerResult &result, Bytes &frame, Text &error) {
    error.clear();
    if (!valid_task_kind(result.kind) || static_cast<unsigned>(result.status) >
        static_cast<unsigned>(WorkerStatus::RecoveryRequired) ||
        result.diagnostic.size() > MaxWorkerDiagnosticBytes ||
        result.payload.size() > MaxWorkerResultPayloadBytes)
        return set_error(error, "worker result is invalid or too large");
    Writer body(MaxWorkerFrameBytes - 4);
    const unsigned char magic[4] = {'R', 'M', 'W', 'R'};
    if (!body.raw(magic, 4) || !body.u16(WorkerProtocolVersion) ||
        !body.u16(static_cast<uint16_t>(result.kind)) || !body.u8(static_cast<uint8_t>(result.status)) ||
        !body.u8(0) || !body.u64(result.request_id) || !body.u64(result.profile_epoch) ||
        !body.u64(result.ui_generation) || !body.u64(result.account_id) ||
        !body.u64(result.account_generation) || !body.u64(result.folder_generation) ||
        !body.u64(result.folder_uidvalidity) || !body.text16(result.diagnostic) ||
        !body.bytes32(result.payload)) return set_error(error, "worker result exceeds its serialized bound");
    Bytes body_bytes;
    if (!body.finish(body_bytes)) return set_error(error, "out of memory serializing worker result");
    Writer full(MaxWorkerFrameBytes);
    if (!full.u32(static_cast<uint32_t>(body_bytes.size())) ||
        !full.raw(body_bytes.data(), body_bytes.size()) || !full.finish(frame))
        return set_error(error, "out of memory framing worker result");
    return true;
}

bool worker_decode_result(const unsigned char *frame, size_t size, WorkerResult &result, Text &error) {
    error.clear();
    const unsigned char *body = nullptr; size_t body_size = 0;
    if (!frame_body(frame, size, body, body_size, error)) return false;
    Reader reader(body, body_size);
    unsigned char magic[4], status = 0, reserved = 0;
    uint16_t version = 0, kind = 0;
    WorkerResult next;
    if (!reader.raw(magic, 4) || memcmp(magic, "RMWR", 4) || !reader.u16(version) ||
        version != WorkerProtocolVersion || !reader.u16(kind) || !reader.u8(status) ||
        !reader.u8(reserved) || reserved || status > static_cast<uint8_t>(WorkerStatus::RecoveryRequired))
        return set_error(error, "worker result header is invalid");
    next.kind = static_cast<WorkerTaskKind>(kind);
    if (!valid_task_kind(next.kind) || !reader.u64(next.request_id) || !reader.u64(next.profile_epoch) ||
        !reader.u64(next.ui_generation) || !reader.u64(next.account_id) ||
        !reader.u64(next.account_generation) || !reader.u64(next.folder_generation) ||
        !reader.u64(next.folder_uidvalidity) ||
        !reader.text16(next.diagnostic, MaxWorkerDiagnosticBytes) ||
        !reader.bytes32(next.payload, MaxWorkerResultPayloadBytes) || reader.remaining())
        return set_error(error, "worker result body is malformed or exceeds its bound");
    next.status = static_cast<WorkerStatus>(status);
    result = static_cast<WorkerResult &&>(next);
    return true;
}

WorkerCoordinator::~WorkerCoordinator() { close(); }

bool WorkerCoordinator::open(const char *executable_path, const ProfilePaths &paths, Text &error) {
    error.clear();
    close();
    if (!executable_path || !*executable_path || strlen(executable_path) >= PATH_MAX ||
        access(executable_path, X_OK) < 0) return set_error(error, "worker executable path is invalid");
    void *memory = calloc(1, sizeof(CoordinatorState));
    if (!memory) return set_error(error, "out of memory creating worker coordinator");
    CoordinatorState *state = new (memory, Placement{}) CoordinatorState();
    memcpy(state->executable, executable_path, strlen(executable_path) + 1);
    state->paths = paths;
    if (!paths.config_dir.empty()) {
        if (!detail::open_secure_profile_directory(paths.config_dir, state->config_fd, error) ||
            !detail::open_private_file_at(state->config_fd, "instance.lock", O_RDWR | O_CREAT,
                state->lock_fd, error) || flock(state->lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (error.empty()) set_error(error, "another ReliefMail instance owns this profile");
            if (state->lock_fd >= 0) ::close(state->lock_fd);
            if (state->config_fd >= 0) ::close(state->config_fd);
            state->~CoordinatorState(); free(memory);
            return false;
        }
    }
    state->epoch = allocate_profile_epoch();
    if (!state->epoch) {
        if (state->lock_fd >= 0) ::close(state->lock_fd);
        if (state->config_fd >= 0) ::close(state->config_fd);
        state->~CoordinatorState(); free(memory);
        return set_error(error, "profile epoch space is exhausted for this process");
    }
    struct sigaction ignore_pipe;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, &state->old_sigpipe) == 0) state->sigpipe_changed = true;
    state_ = state;
    return true;
}

bool WorkerCoordinator::submit(const WorkerRequest &request, SecretBytes secret, Text &error) {
    error.clear();
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state || state->shutting_down || !request.request_id || request.request_id <= state->last_request_id ||
        request.payload.size() > MaxWorkerRequestPayloadBytes || secret.size() > MaxSecretPacketBytes)
        return set_error(error, "worker request ID, payload, secret, or coordinator state is invalid");
    if (!state->epoch || !request.profile_epoch || request.profile_epoch != state->epoch)
        return set_error(error, "worker request is missing the current profile epoch");
    if (state->recovery_required && request.kind != WorkerTaskKind::Initialize)
        return set_error(error, "profile recovery must complete before new work can start");
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        const Job &pending = state->jobs[i];
        if ((pending.state == Job::Queued || pending.state == Job::Running) &&
            task_is_maintenance(pending.request.kind) && !task_is_maintenance(request.kind))
            return set_error(error, "profile maintenance is active; retry after its barrier completes");
    }
    size_t slot = MaxWorkerOutstanding;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state->jobs[i].state == Job::Free) { slot = i; break; }
    if (slot == MaxWorkerOutstanding) return set_error(error, "worker queue is full");
    WorkerRequest copy = request;
    copy.profile_paths = state->paths;
    Bytes encoded;
    if (!worker_encode_request(copy, encoded, error)) return false;
    Job &job = state->jobs[slot];
    cleanup_job(job);
    job.state = Job::Queued;
    job.sequence = state->next_sequence++;
    job.request = static_cast<WorkerRequest &&>(copy);
    job.encoded_request = static_cast<Bytes &&>(encoded);
    job.secret = static_cast<SecretBytes &&>(secret);
    state->last_request_id = request.request_id;
    return true;
}

bool WorkerCoordinator::cancel(uint64_t request_id, Text &error) {
    error.clear();
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state || !request_id) return set_error(error, "worker cancellation request is invalid");
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        Job &job = state->jobs[i];
        if ((job.state == Job::Queued || job.state == Job::Running) && job.request.request_id == request_id) {
            if (job.state == Job::Queued) complete_without_child(job, WorkerStatus::Cancelled, "worker cancelled before start");
            else mark_cancel(job, false);
            return true;
        }
    }
    return set_error(error, "worker request is no longer pending");
}

bool WorkerCoordinator::pump(Text &error) {
    error.clear();
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state) return set_error(error, "worker coordinator is closed");
    const uint64_t now = monotonic_ms();
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        Job &job = state->jobs[i];
        if (job.state != Job::Running) continue;
#ifdef RELIEFMAIL_TESTING
        if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock &&
            job.secret_test_pump_count < UINT32_MAX) ++job.secret_test_pump_count;
#endif
        if (!job.cancelled && ((job.request.deadline_monotonic_ms != UINT64_MAX &&
                now >= job.request.deadline_monotonic_ms) || now - job.started_ms >= ResolverWatchdogMs))
            mark_cancel(job, true);
        cancellation_escalation(job, now);
        if (!job.cancelled) {
            if (!write_pending(job.request_fd, job.encoded_request.data(), job.encoded_request.size(),
                    job.request_offset) || !advance_secret_pipe(job)) fail_worker_io(job);
        }
        if (job.cancelled && !job.cancel_written && job.cancel_fd >= 0) {
            const unsigned char byte = 1;
            const ssize_t count = write(job.cancel_fd, &byte, 1);
            if (count == 1 || (count < 0 && errno == EPIPE)) { job.cancel_written = true; close_fd(job.cancel_fd); }
        }
        if (job.response_fd >= 0 && !advance_response(job)) {
            close_fd(job.response_fd);
            job.response_eof = true;
            fail_worker_io(job);
        }
        if (job.secret_result_fd >= 0 && !advance_secret_response(job)) {
            close_fd(job.secret_result_fd);
            job.secret_response_eof = true;
            fail_worker_io(job);
        }
#ifdef RELIEFMAIL_TESTING
        if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock && job.secret_test_header_written &&
            job.secret_test_body_blocked && !job.secret_test_body_release_sent &&
            job.cancel_fd >= 0) {
            const unsigned char release = 0x5a;
            const ssize_t count = write(job.cancel_fd, &release, 1);
            if (count == 1) {
                job.secret_test_body_release_sent = true;
                job.secret_test_release_pump = job.secret_test_pump_count;
            } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                fail_worker_io(job);
            }
        }
        if (job.request.kind == WorkerTaskKind::TestSecretPipeBlock &&
            job.secret_test_body_release_sent && !job.secret_test_payload_release_sent &&
            job.secret_test_pump_count > job.secret_test_release_pump && job.cancel_fd >= 0) {
            const unsigned char release = 0x5b;
            const ssize_t count = write(job.cancel_fd, &release, 1);
            if (count == 1) {
                job.secret_test_payload_release_sent = true;
                close_fd(job.cancel_fd);
            } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                fail_worker_io(job);
            }
        }
        if (job.request.kind == WorkerTaskKind::TestSecretInterleave &&
            job.secret_response_received && !job.interleave_ack_sent && job.cancel_fd >= 0) {
            const unsigned char acknowledged = 1;
            const ssize_t count = write(job.cancel_fd, &acknowledged, 1);
            if (count == 1) {
                job.interleave_ack_sent = true;
                close_fd(job.cancel_fd);
            } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                fail_worker_io(job);
            }
        }
#endif
        collect_exit(job);
        if (job.reaped && job.response_fd >= 0 && !advance_response(job)) {
            close_fd(job.response_fd);
            job.response_eof = true;
        }
        if (job.reaped && job.secret_result_fd >= 0 && !advance_secret_response(job)) {
            close_fd(job.secret_result_fd);
            job.secret_response_eof = true;
        }
        if (job.reaped && job.response_eof && job.secret_response_eof) finalize_job(*state, job);
    }

    if (!state->shutting_down) {
        for (;;) {
            const size_t candidate = next_eligible_job(*state);
            if (candidate == MaxWorkerOutstanding) break;
            Job &job = state->jobs[candidate];
            const bool maintenance = task_is_maintenance(job.request.kind);
            if (spawn_job(*state, job) < 0) {
                complete_without_child(job, WorkerStatus::Failed, "cannot create worker pipes or process");
                continue;
            }
            if (maintenance) break;
        }
    }
    return true;
}

bool WorkerCoordinator::take_result(WorkerResult &result) {
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state) return false;
    size_t chosen = MaxWorkerOutstanding;
    uint64_t sequence = UINT64_MAX;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state->jobs[i].state == Job::Complete && state->jobs[i].sequence < sequence) {
            chosen = i; sequence = state->jobs[i].sequence;
        }
    if (chosen == MaxWorkerOutstanding) return false;
    Job &completed = state->jobs[chosen];
    if (!completed.epoch_transition_result && completed.result.profile_epoch != state->epoch) {
        completed.result.secret_payload.clear();
        completed.result.payload.clear();
        completed.result.status = WorkerStatus::Cancelled;
        completed.result.diagnostic.set("profile generation changed; completed result was invalidated");
    }
    result = static_cast<WorkerResult &&>(completed.result);
    cleanup_job(completed);
    return true;
}

bool WorkerCoordinator::status(WorkerCoordinatorStatus &snapshot) const {
    const CoordinatorState *state = static_cast<const CoordinatorState *>(state_);
    if (!state) return false;
    WorkerCoordinatorStatus next;
    next.profile_epoch = state->epoch;
    next.shutting_down = state->shutting_down;
    next.recovery_required = state->recovery_required;
    uint64_t current_sequence = UINT64_MAX;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        const Job &job = state->jobs[i];
        if (job.state == Job::Queued) ++next.queued;
        if (job.state == Job::Running) {
            ++next.active;
            if (task_is_network(job.request.kind)) ++next.active_network;
            if (job.cancelled) ++next.cancelling;
        }
        if ((job.state == Job::Queued || job.state == Job::Running) &&
            task_is_maintenance(job.request.kind)) next.maintenance_waiting = true;
        if ((job.state == Job::Queued || job.state == Job::Running) && job.sequence < current_sequence) {
            current_sequence = job.sequence;
            next.current_request_id = job.request.request_id;
            next.current_kind = job.request.kind;
        }
    }
    snapshot = next;
    return true;
}

bool WorkerCoordinator::begin_shutdown(Text &error) {
    error.clear();
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state) return set_error(error, "worker coordinator is closed");
    state->shutting_down = true;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i) {
        Job &job = state->jobs[i];
        if (job.state == Job::Queued)
            complete_without_child(job, WorkerStatus::Cancelled, "worker cancelled during shutdown");
        else if (job.state == Job::Running) mark_cancel(job, false);
    }
    return true;
}

bool WorkerCoordinator::shutdown_complete() const {
    const CoordinatorState *state = static_cast<const CoordinatorState *>(state_);
    if (!state || !state->shutting_down) return false;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state->jobs[i].state == Job::Running || state->jobs[i].state == Job::Queued) return false;
    return true;
}

bool WorkerCoordinator::finish_shutdown(Text &error) {
    error.clear();
    CoordinatorState *state = static_cast<CoordinatorState *>(state_);
    if (!state || !shutdown_complete()) return set_error(error, "worker shutdown has not drained all children");
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if (state->jobs[i].state != Job::Free) cleanup_job(state->jobs[i]);
    if (state->lock_fd >= 0) ::close(state->lock_fd);
    if (state->config_fd >= 0) ::close(state->config_fd);
    if (state->sigpipe_changed) sigaction(SIGPIPE, &state->old_sigpipe, nullptr);
    state->~CoordinatorState();
    free(state);
    state_ = nullptr;
    return true;
}

bool WorkerCoordinator::maintenance_active() const {
    const CoordinatorState *state = static_cast<const CoordinatorState *>(state_);
    if (!state) return false;
    for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
        if ((state->jobs[i].state == Job::Queued || state->jobs[i].state == Job::Running) &&
            task_is_maintenance(state->jobs[i].request.kind)) return true;
    return false;
}

uint64_t WorkerCoordinator::profile_epoch() const {
    const CoordinatorState *state = static_cast<const CoordinatorState *>(state_);
    return state ? state->epoch : 0;
}

void WorkerCoordinator::close() {
    if (!state_) return;
    Text error;
    if (!begin_shutdown(error)) return;
    while (!shutdown_complete()) {
        if (!pump(error)) {
            CoordinatorState *state = static_cast<CoordinatorState *>(state_);
            if (!state) return;
            for (size_t i = 0; i < MaxWorkerOutstanding; ++i)
                if (state->jobs[i].state == Job::Running && state->jobs[i].pid > 0)
                    kill(state->jobs[i].pid, SIGKILL);
        }
        usleep(1000);
    }
    (void)finish_shutdown(error);
}

bool worker_mode_requested(int argc, const char *const argv[]) {
    return argc == 2 && argv && argv[1] && !strcmp(argv[1], "--reliefmail-worker");
}

int worker_entry_main() {
    unsigned char length_bytes[4];
    bool eof = false;
    if (!worker_read_all(STDIN_FILENO, length_bytes, sizeof(length_bytes), false, eof)) return 2;
    size_t body_size = 0; Text error;
    if (!worker_decode_frame_length(length_bytes, body_size, error)) return 2;
    unsigned char *frame = static_cast<unsigned char *>(malloc(body_size + 4));
    if (!frame) return 2;
    memcpy(frame, length_bytes, 4);
    const bool got_frame = worker_read_all(STDIN_FILENO, frame + 4, body_size, false, eof);
    WorkerRequest request;
    const bool decoded = got_frame && worker_decode_request(frame, body_size + 4, request, error);
    detail::wipe_memory(frame, body_size + 4);
    free(frame);
    if (!decoded) return 2;
    SecretBytes secret;
#ifdef RELIEFMAIL_TESTING
    bool forced_secret_body_eagain = false;
    if (!read_secret_pipe(secret, request.kind == WorkerTaskKind::TestSecretPipeBlock,
            &forced_secret_body_eagain) ||
        (request.kind == WorkerTaskKind::TestSecretPipeBlock && !forced_secret_body_eagain)) return 2;
#else
    if (!read_secret_pipe(secret)) return 2;
#endif
    int cancellation_fd = 4;
    OperationContext context;
    context.deadline_monotonic_ms = request.deadline_monotonic_ms;
    context.is_cancelled = cancellation_check;
    context.cancellation_context = &cancellation_fd;
    WorkerResult result;
    const bool ok = worker_execute_task(request, static_cast<SecretBytes &&>(secret), context, result);
    result.request_id = request.request_id;
    result.profile_epoch = request.profile_epoch;
    result.ui_generation = request.ui_generation;
    result.account_id = request.account_id;
    result.account_generation = request.account_generation;
    result.folder_generation = request.folder_generation;
    result.folder_uidvalidity = request.folder_uidvalidity;
    result.kind = request.kind;
    if (!ok && result.status == WorkerStatus::Ok) result.status = WorkerStatus::Failed;
    Bytes response;
    if (!worker_encode_result(result, response, error)) return 3;
#ifdef RELIEFMAIL_TESTING
    if (request.kind == WorkerTaskKind::TestSecretInterleave) {
        const size_t split = response.size() / 2;
        const bool prefix_sent = worker_write_all(STDOUT_FILENO, response.data(), split);
        unsigned char secret_length[4] = {
            static_cast<unsigned char>(result.secret_payload.size() >> 24),
            static_cast<unsigned char>(result.secret_payload.size() >> 16),
            static_cast<unsigned char>(result.secret_payload.size() >> 8),
            static_cast<unsigned char>(result.secret_payload.size())};
        const bool secret_sent = prefix_sent && result.secret_payload.size() <= MaxSecretPipeBytes &&
            worker_write_all(5, secret_length, sizeof(secret_length)) &&
            worker_write_all(5, result.secret_payload.data(), result.secret_payload.size());
        result.secret_payload.clear();
        close(5);
        unsigned char acknowledged = 0;
        bool got_ack = false;
        for (unsigned attempt = 0; secret_sent && !got_ack && attempt < 5000; ++attempt) {
            const ssize_t count = read(4, &acknowledged, 1);
            if (count == 1) got_ack = acknowledged == 1;
            else if (count < 0 && errno == EINTR) continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) usleep(1000);
            else break;
        }
        const bool suffix_sent = got_ack && worker_write_all(STDOUT_FILENO,
            response.data() + split, response.size() - split);
        if (response.data()) detail::wipe_memory(const_cast<unsigned char *>(response.data()), response.size());
        close(4);
        return suffix_sent ? 0 : 4;
    }
#endif
    const bool sent = worker_write_all(STDOUT_FILENO, response.data(), response.size());
    if (response.data()) detail::wipe_memory(const_cast<unsigned char *>(response.data()), response.size());
    unsigned char secret_length[4] = {
        static_cast<unsigned char>(result.secret_payload.size() >> 24),
        static_cast<unsigned char>(result.secret_payload.size() >> 16),
        static_cast<unsigned char>(result.secret_payload.size() >> 8),
        static_cast<unsigned char>(result.secret_payload.size())};
    const bool secret_sent = sent && result.secret_payload.size() <= MaxSecretPipeBytes &&
        worker_write_all(5, secret_length, sizeof(secret_length)) &&
        worker_write_all(5, result.secret_payload.data(), result.secret_payload.size());
    result.secret_payload.clear();
    close(5);
    close(4);
    return secret_sent ? 0 : 4;
}

} // namespace rmail
