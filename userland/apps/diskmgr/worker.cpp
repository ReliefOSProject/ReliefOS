#include "worker.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dm {
static constexpr uint32_t ReplyMagic = 0x444D5231U;

Worker::~Worker()
{
    if (input_ >= 0) close(input_);
    if (pid_ > 0) (void)waitpid(pid_, nullptr, WNOHANG);
}

int Worker::start(const Request &request)
{
    if (busy()) return -EBUSY;
    int channel[2];
    if (pipe(channel) < 0) return -errno;
    if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno; close(channel[0]); close(channel[1]); return -error;
    }
    pid_t child = fork();
    if (child < 0) { int error = errno; close(channel[0]); close(channel[1]); return -error; }
    if (!child) {
        close(channel[0]);
        Reply reply{};
        perform_request(request, reply);
        reply.magic = ReplyMagic;
        size_t written = 0;
        while (written < sizeof(reply)) {
            ssize_t n = write(channel[1], reinterpret_cast<const char *>(&reply) + written,
                              sizeof(reply) - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(1);
            written += static_cast<size_t>(n);
        }
        close(channel[1]);
        _exit(0);
    }
    close(channel[1]);
    pid_ = child;
    input_ = channel[0];
    received_ = 0;
    transport_error_ = 0;
    eof_ = false;
    memset(&reply_, 0, sizeof(reply_));
    return 0;
}

void Worker::drain()
{
    while (!eof_) {
        char extra;
        void *target = received_ < sizeof(reply_) ? reinterpret_cast<char *>(&reply_) + received_ : &extra;
        size_t left = received_ < sizeof(reply_) ? sizeof(reply_) - received_ : 1;
        ssize_t n = read(input_, target, left);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) { transport_error_ = -errno; eof_ = true; break; }
        if (!n) { eof_ = true; break; }
        if (received_ >= sizeof(reply_)) transport_error_ = -EPROTO;
        else received_ += static_cast<size_t>(n);
    }
}

int Worker::poll()
{
    if (!busy()) return 0;
    drain();
    int status = 0;
    pid_t reaped = waitpid(pid_, &status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR)) return 0;
    // The child can finish between the first EAGAIN and waitpid. Its final
    // bytes are now available: re-drain before deciding that a reply is short.
    if (reaped > 0) drain();
    if (reaped < 0 || !WIFEXITED(status) || WEXITSTATUS(status) ||
        received_ != sizeof(reply_) || reply_.magic != ReplyMagic || transport_error_ ||
        reply_.snapshot.disk_count > RELIEFOS_BLOCK_MAX_DISKS ||
        reply_.snapshot.partition_count > RELIEFOS_BLOCK_MAX_PARTITIONS) {
        memset(&reply_, 0, sizeof(reply_));
        reply_.snapshot.selected_disk = -1;
        reply_.status = transport_error_ ? transport_error_ : -EIO;
    }
    close(input_); input_ = -1; pid_ = -1;
    return 1;
}
}
