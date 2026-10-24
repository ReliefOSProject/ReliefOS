#ifndef RELIEFMAIL_WORKER_H
#define RELIEFMAIL_WORKER_H

#include "vault.h"

namespace rmail {

class SecretBytes;

constexpr uint32_t WorkerProtocolVersion = 2;
constexpr size_t MaxWorkerRequestPayloadBytes = MaxRawMessageBytes + 2u * 1024u * 1024u;
constexpr size_t MaxWorkerResultPayloadBytes = MaxRawMessageBytes + 2u * 1024u * 1024u;
constexpr size_t MaxWorkerFrameBytes = 68u * 1024u * 1024u;
constexpr size_t MaxWorkerOutstanding = 32;

enum class WorkerTaskKind : uint16_t {
    Initialize = 1,
    AccountTest,
    FolderList,
    FolderSelect,
    HeaderSync,
    LocalPage,
    LocalSearch,
    ServerSearch,
    ParseMessage,
    SaveDraft,
    LoadDraft,
    DeleteDraft,
    QueueSend,
    SendQueued,
    RetryArchive,
    SetFlags,
    MoveMessage,
    CopyMessage,
    DeleteMessage,
    FolderOperation,
    ClearCache,
    BackupProfile,
    RestoreProfile,
    UnlockVault,
    SaveVault,
    AccountList,
    AccountCreate,
    AccountUpdate,
    AccountDelete,
    AccountSetDefault,
    AccountTestImap,
    AccountTestSmtp,
    MessagePage,
    MessageSearch,
    ReadMessageChunk,
    SaveDraftRevision,
    ChangeVaultPassword,
    CancelQueuedSend,
    FetchMessage,
    VaultStatus,
    ListDrafts,
    SaveForwardDraft,
    SaveDraftCopy,
    ComposeCatalog,
    ListSendOperations,
    ReviewArchiveTarget,
    ResolveUncertainSend,
#ifdef RELIEFMAIL_TESTING
    TestProbe,
    TestCrash,
    TestWait,
    TestEof,
    TestSecretOutput,
    TestSecretInterleave,
    TestSecretPipeBlock,
    TestNetworkWait,
    TestMutationWait,
    TestNetworkMutationWait,
    TestMaintenanceWait
#endif
};

enum class WorkerStatus : uint8_t {
    Ok,
    InvalidRequest,
    Failed,
    Cancelled,
    Uncertain,
    CommittedWarning,
    RecoveryRequired
};

/* Every field crossing the process boundary is serialized; model handles and
 * callbacks remain local to the process that created them. */
struct WorkerRequest {
    uint64_t request_id = 0;
    uint64_t profile_epoch = 0;
    uint64_t ui_generation = 0;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    Text folder_server_name;
    uint64_t folder_generation = 0;
    uint64_t folder_uidvalidity = 0;
    uint32_t message_uid = 0;
    WorkerTaskKind kind = WorkerTaskKind::Initialize;
    ProfilePaths profile_paths;
    Bytes payload;
    uint64_t deadline_monotonic_ms = UINT64_MAX;
};

struct WorkerResult {
    uint64_t request_id = 0;
    uint64_t profile_epoch = 0;
    uint64_t ui_generation = 0;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    uint64_t folder_generation = 0;
    uint64_t folder_uidvalidity = 0;
    WorkerTaskKind kind = WorkerTaskKind::Initialize;
    WorkerStatus status = WorkerStatus::Failed;
    Text diagnostic;
    Bytes payload;
    SecretBytes secret_payload;
    bool ok() const { return status == WorkerStatus::Ok || status == WorkerStatus::CommittedWarning; }
};

struct WorkerCoordinatorStatus {
    size_t queued = 0;
    size_t active = 0;
    size_t active_network = 0;
    size_t cancelling = 0;
    bool maintenance_waiting = false;
    bool shutting_down = false;
    bool recovery_required = false;
    uint64_t profile_epoch = 0;
    uint64_t current_request_id = 0;
    WorkerTaskKind current_kind = WorkerTaskKind::Initialize;
};

bool worker_encode_request(const WorkerRequest &request, Bytes &frame, Text &error);
bool worker_decode_request(const unsigned char *frame, size_t size,
    WorkerRequest &request, Text &error);
bool worker_encode_result(const WorkerResult &result, Bytes &frame, Text &error);
bool worker_decode_result(const unsigned char *frame, size_t size,
    WorkerResult &result, Text &error);
bool worker_decode_frame_length(const unsigned char header[4], size_t &length, Text &error);

class WorkerCoordinator {
    void *state_ = nullptr;
    WorkerCoordinator(const WorkerCoordinator &) = delete;
    WorkerCoordinator &operator=(const WorkerCoordinator &) = delete;

public:
    WorkerCoordinator() = default;
    ~WorkerCoordinator();
    bool open(const char *executable_path, const ProfilePaths &paths, Text &error);
    /* Coordinator ownership and its process-unique profile epoch live from
     * open through finish_shutdown; call these methods on one UI thread. */
    uint64_t profile_epoch() const;
    /* submit requires request.profile_epoch to equal profile_epoch(). */
    bool submit(const WorkerRequest &request, SecretBytes secret, Text &error);
    bool cancel(uint64_t request_id, Text &error);
    /* Nonblocking progress: advances pipes, cancellation escalation and queue scheduling. */
    bool pump(Text &error);
    bool take_result(WorkerResult &result);
    bool status(WorkerCoordinatorStatus &status) const;
    /* Starts bounded asynchronous cancellation; call pump until shutdown_complete. */
    bool begin_shutdown(Text &error);
    bool shutdown_complete() const;
    /* Releases the profile singleton only after all child processes have exited. */
    bool finish_shutdown(Text &error);
    /* Waits for a maintenance barrier to drain and rejects new work until finished. */
    bool maintenance_active() const;
    void close();
};

/* A committed RestoreProfile or any RecoveryRequired result advances the
 * coordinator epoch before delivery and invalidates all other old-epoch
 * results. The transition result itself retains its request epoch and status;
 * successful RestoreProfile keeps the gate closed until Initialize succeeds. */

/* Called by the executable's fixed --reliefmail-worker mode. Uses stdin/stdout,
 * fd 3 for wipeable secret input, fd 4 for cancellation, and fd 5 for wipeable
 * secret output (separate from normal result frames). */
bool worker_mode_requested(int argc, const char *const argv[]);
int worker_entry_main();

}

#endif
