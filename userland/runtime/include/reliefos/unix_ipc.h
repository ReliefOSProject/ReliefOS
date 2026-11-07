#ifndef RELIEFOS_UNIX_IPC_H
#define RELIEFOS_UNIX_IPC_H

#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define RELIEFOS_IPC_MAGIC 0x554e4c4cU /* 'LNXU' */
#define RELIEFOS_IPC_VERSION 1U
/* Maximum encoded frame size. The reader preserves partial frames and the
 * sender handles short writes; SOCK_STREAM has no atomic frame boundary. */
#define RELIEFOS_IPC_ATOMIC_FRAME_CAP 8192u

#define RELIEFOS_IPC_SOCK_INPUT_METHOD "/run/leonos/input-method.sock"
#define RELIEFOS_IPC_SOCK_SESSION "/run/leonos/session.sock"
#define RELIEFOS_IPC_SOCK_DEVICE "/run/leonos/devman.sock"

struct reliefos_ipc_frame {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
};

int reliefos_ipc_connect(const char *path);
int reliefos_ipc_bind_listen(const char *path, int backlog);
int reliefos_ipc_bind_listen_mode(const char *path, int backlog, uint32_t mode);
int reliefos_ipc_accept(int listen_fd, struct ucred *peer);
/* On nonblocking streams, success may retain one partially sent frame.
 * Call flush from the event loop until it succeeds (EAGAIN means backpressure).
 * A new send drains that frame first; EAGAIN rejects the new message entirely.
 * SCM_RIGHTS is attached once. Release pending state with reliefos_ipc_close. */
int reliefos_ipc_flush(int fd);
int reliefos_ipc_send(int fd, uint32_t type, const void *payload, uint32_t length);
int reliefos_ipc_send_fd(int fd, uint32_t type, const void *payload,
                       uint32_t length, int send_fd);
int reliefos_ipc_recv(int fd, uint32_t *type, void *payload, uint32_t capacity,
                    uint32_t *length);
int reliefos_ipc_recv_fd(int fd, uint32_t *type, void *payload, uint32_t capacity,
                       uint32_t *length, int *received_fd);
/* Requires SO_PASSCRED before receiving. Every fragment must carry exactly
 * the expected kernel-supplied credentials; partial-frame retries preserve it. */
int reliefos_ipc_recv_cred_fd(int fd, uint32_t *type, void *payload, uint32_t capacity,
                           uint32_t *length, int *received_fd, const struct ucred *expected);
int reliefos_ipc_set_nonblock(int fd, int enabled);
int reliefos_ipc_peer_credentials(int fd, struct ucred *credentials);
int reliefos_ipc_close(int fd);

#endif
