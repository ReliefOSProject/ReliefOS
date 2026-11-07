/* Transitional compatibility forwarder (ReliefOS / ReliefNT rename).
 * Canonical declarations: <reliefos/unix_ipc.h>.
 * No second layout definition exists behind these names. */
#ifndef LEONOS_UNIX_IPC_H
#define LEONOS_UNIX_IPC_H
#include <reliefos/unix_ipc.h>

/* Old names alias the single canonical declaration. */
#define LEONOS_IPC_ATOMIC_FRAME_CAP RELIEFOS_IPC_ATOMIC_FRAME_CAP
#define LEONOS_IPC_MAGIC RELIEFOS_IPC_MAGIC
#define LEONOS_IPC_SOCK_DEVICE RELIEFOS_IPC_SOCK_DEVICE
#define LEONOS_IPC_SOCK_INPUT_METHOD RELIEFOS_IPC_SOCK_INPUT_METHOD
#define LEONOS_IPC_SOCK_SESSION RELIEFOS_IPC_SOCK_SESSION
#define LEONOS_IPC_VERSION RELIEFOS_IPC_VERSION
#define leonos_ipc_accept reliefos_ipc_accept
#define leonos_ipc_bind_listen reliefos_ipc_bind_listen
#define leonos_ipc_bind_listen_mode reliefos_ipc_bind_listen_mode
#define leonos_ipc_close reliefos_ipc_close
#define leonos_ipc_connect reliefos_ipc_connect
#define leonos_ipc_flush reliefos_ipc_flush
#define leonos_ipc_frame reliefos_ipc_frame
#define leonos_ipc_peer_credentials reliefos_ipc_peer_credentials
#define leonos_ipc_recv reliefos_ipc_recv
#define leonos_ipc_recv_cred_fd reliefos_ipc_recv_cred_fd
#define leonos_ipc_recv_fd reliefos_ipc_recv_fd
#define leonos_ipc_send reliefos_ipc_send
#define leonos_ipc_send_fd reliefos_ipc_send_fd
#define leonos_ipc_set_nonblock reliefos_ipc_set_nonblock
#endif /* LEONOS_UNIX_IPC_H */
