#define main suite_main
#include "authd_sudo_test.c"
#undef main
#include "../../userland/apps/authd/authd_sudo.c"
#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"

static int collected = -1;
static int result_send(void *context, uint32_t type, const void *payload, uint32_t length, int fd)
{
    const struct reliefos_authd_wait_ack *ack = payload;
    (void)context;
    assert(type == RELIEFOS_AUTHD_MSG_WAIT && length == sizeof(*ack) && ack->code == 0);
    collected = dup(fd);
    assert(collected >= 0);
    return 0;
}

int main(void)
{
    char path[256];
    host_tmp_path(path, sizeof(path), "sudo-result-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0 && write(fd, "result", 6) == 6);
    close(fd);
    struct authd_sudo_slot *slot = authd_sudo_alloc_slot(1234, 1000);
    assert(slot);
    slot->owner_pid = 42;
    slot->reaped = 1;
    slot->result_path_used = 1;
    strcpy(slot->result_path, path);
    struct authd_sudo_channel channel = {.send = channel_send, .send_fd = result_send, .owner_pid = 43};
    struct reliefos_authd_wait request = {.child_pid = 1234};
    assert(authd_sudo_wait_from_peer(1000, (const uint8_t *)&request, sizeof(request), &channel) == -ESRCH);
    assert(collected == -1 && access(path, F_OK) == 0);
    channel.owner_pid = 42;
    assert(authd_sudo_wait_from_peer(1000, (const uint8_t *)&request, sizeof(request), &channel) == 0);
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    char text[8] = {0};
    assert(read(collected, text, sizeof(text)) == 6 && !strcmp(text, "result"));
    close(collected);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { for (;;) pause(); }
    slot = authd_sudo_alloc_slot((uint32_t)child, 1000);
    assert(slot);
    slot->owner_pid = 42;
    struct reliefos_authd_run_signal signal_request = {(uint32_t)child, SIGTERM};
    channel.owner_pid = 43;
    reset_logs();
    assert(authd_sudo_signal_from_peer(1000, (const uint8_t *)&signal_request,
                                      sizeof(signal_request), &channel) == 0);
    struct reliefos_authd_ack ack;
    memcpy(&ack, last_reply(NULL, NULL), sizeof(ack));
    assert(ack.code == -ESRCH && kill(child, 0) == 0);
    channel.owner_pid = 42;
    assert(authd_sudo_signal_from_peer(1000, (const uint8_t *)&signal_request,
                                      sizeof(signal_request), &channel) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    memset(slot, 0, sizeof(*slot));
    puts("sudo result: owner-PID isolation and readable fd after private-file unlink PASS");
    return 0;
}
