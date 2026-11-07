#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <reliefos/unix_ipc.h>

static void credential_frames(void)
{
    int pair[2], source[2], enabled = 1, received = -1, status;
    uint32_t type, length;
    char payload[8];
    struct ucred expected = {getpid(), getuid(), getgid()};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    assert(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    assert(pipe(source) == 0);
    assert(reliefos_ipc_send_fd(pair[0], 31, "safe", 5, source[0]) == 0);
    assert(reliefos_ipc_recv_cred_fd(pair[1], &type, payload, sizeof(payload), &length,
                                    &received, &expected) == 0);
    assert(type == 31 && length == 5 && !strcmp(payload, "safe"));
    assert(received >= 0 && (fcntl(received, F_GETFD) & FD_CLOEXEC));
    close(received);
    /* SO_PEERCRED still identifies the creator, while this frame comes from
     * its child. Reject the frame and release the only transferred read FD. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) _exit(reliefos_ipc_send_fd(pair[0], 32, "bad", 4, source[0]) != 0);
    close(source[0]);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(reliefos_ipc_recv_cred_fd(pair[1], &type, payload, sizeof(payload), &length,
                                    &received, &expected) == -1 && errno == EPERM);
    assert(received == -1 && length == 0);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    assert(write(source[1], "x", 1) == -1 && errno == EPIPE);
    close(source[1]);
    assert(reliefos_ipc_close(pair[0]) == 0 && reliefos_ipc_close(pair[1]) == 0);

    struct {
        struct reliefos_ipc_frame header;
        uint32_t type;
        char payload[4];
    } frame = {{RELIEFOS_IPC_MAGIC, RELIEFOS_IPC_VERSION, 8}, 33, "bad"};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    assert(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    assert(write(pair[0], &frame, 5) == 5);
    assert(reliefos_ipc_recv_cred_fd(pair[1], &type, payload, sizeof(payload), &length,
                                    &received, &expected) == -1 && errno == EAGAIN);
    child = fork();
    assert(child >= 0);
    if (!child) _exit(write(pair[0], (char *)&frame + 5, sizeof(frame) - 5) != sizeof(frame) - 5);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(reliefos_ipc_recv_cred_fd(pair[1], &type, payload, sizeof(payload), &length,
                                    &received, &expected) == -1 && errno == EPERM);
    assert(received == -1 && length == 0);
    assert(reliefos_ipc_close(pair[0]) == 0 && reliefos_ipc_close(pair[1]) == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    assert(reliefos_ipc_send(pair[0], 34, "bad", 4) == 0);
    assert(reliefos_ipc_recv_cred_fd(pair[1], &type, payload, sizeof(payload), &length,
                                    &received, &expected) == -1 && errno == EPROTO);
    assert(reliefos_ipc_close(pair[0]) == 0 && reliefos_ipc_close(pair[1]) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[ipc-credentials] BEGIN real framing, credentials and received FD cleanup");
    credential_frames();
    int pair[2], first[2], second[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    assert(pipe(first) == 0 && pipe(second) == 0);
    struct {
        struct reliefos_ipc_frame frame;
        uint32_t type;
        char data[4];
    } encoded = {{RELIEFOS_IPC_MAGIC, RELIEFOS_IPC_VERSION, 8}, 17, "one"};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec vector = {&encoded, 5};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &first[0], sizeof(int));
    assert(sendmsg(pair[0], &message, 0) == 5);
    assert(close(first[0]) == 0);
    char output[8];
    uint32_t type, size;
    int received = -1;
    assert(reliefos_ipc_recv_fd(pair[1], &type, output, sizeof(output), &size, &received) == -1 && errno == EAGAIN);
    assert(received == -1);
    assert(write(pair[0], (char *)&encoded + 5, sizeof(encoded) - 5) == sizeof(encoded) - 5);
    assert(reliefos_ipc_send_fd(pair[0], 18, "two", 4, second[0]) == 0);
    assert(close(second[0]) == 0);
    assert(reliefos_ipc_recv_fd(pair[1], &type, output, sizeof(output), &size, &received) == 0);
    assert(type == 17 && size == 4 && !strcmp(output, "one") && received >= 0);
    assert(fcntl(received, F_GETFD) & FD_CLOEXEC);
    assert(write(first[1], "a", 1) == 1 && read(received, output, 1) == 1 && output[0] == 'a');
    close(received);
    assert(reliefos_ipc_recv_fd(pair[1], &type, output, sizeof(output), &size, &received) == 0);
    assert(type == 18 && size == 4 && !strcmp(output, "two") && received >= 0);
    assert(write(second[1], "b", 1) == 1 && read(received, output, 1) == 1 && output[0] == 'b');
    close(received);
    assert(reliefos_ipc_close(pair[0]) == 0 && reliefos_ipc_close(pair[1]) == 0);
    close(first[1]); close(second[1]);
    char path[108];
    host_tmp_path(path, sizeof(path), "ipc-XXXXXX");
    int reservation = mkstemp(path);
    assert(reservation >= 0);
    close(reservation);
    assert(unlink(path) == 0);
    int listener = reliefos_ipc_bind_listen(path, 2);
    assert(listener >= 0);
    struct stat node;
    assert(stat(path, &node) == 0 && (node.st_mode & 0777) == 0600);
    assert(reliefos_ipc_bind_listen(path, 2) == -1 && errno == EADDRINUSE);
    close(listener);
    mode_t previous_mask = umask(0077);
    listener = reliefos_ipc_bind_listen_mode(path, 2, 0666);
    assert(listener >= 0);
    assert(stat(path, &node) == 0 && (node.st_mode & 0777) == 0666);
    assert(umask(previous_mask) == 0077);
    close(listener);
    unlink(path);
    puts("PASS IPC on Linux: partial headers, exact frame/SCM_RIGHTS association, CLOEXEC, restart, active-listener protection");
    puts("[ipc-credentials] DONE failures=0");
}
