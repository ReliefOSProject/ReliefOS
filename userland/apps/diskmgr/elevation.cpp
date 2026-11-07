#include "elevation.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace dm {
static const char Ready[] = "[diskmgr.elf] elevated FLTK window ready\n";

/** @brief Erase a password buffer through volatile writes before releasing its stack storage. */
static void wipe(void *buffer, size_t count)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(buffer);
    while (count--) *p++ = 0;
}

Elevation::~Elevation()
{
    if (input_ >= 0) close(input_);
    if (pid_ > 0) (void)waitpid(pid_, nullptr, WNOHANG);
}

int Elevation::start(const char *password)
{
    if (busy()) return -EBUSY;
    if (!password || !*password || strlen(password) > 255 || strchr(password, '\n') || strchr(password, '\r'))
        return -EINVAL;
    int in[2], out[2];
    if (pipe(in) < 0) return -errno;
    if (pipe(out) < 0) { int error = errno; close(in[0]); close(in[1]); return -error; }
    int result = 0;
    const int owned[] = {in[0], in[1], out[0], out[1]};
    for (int fd : owned)
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) result = errno;
    if (!result && fcntl(out[0], F_SETFL, O_NONBLOCK) < 0) result = errno;
    posix_spawn_file_actions_t actions;
    bool initialized = false;
    if (!result) { result = posix_spawn_file_actions_init(&actions); initialized = !result; }
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
    char *args[] = {const_cast<char *>("/usr/bin/sudo"), const_cast<char *>("-S"),
                    const_cast<char *>("-k"), const_cast<char *>("-p"), const_cast<char *>(""),
                    const_cast<char *>("--"),
                    const_cast<char *>("/usr/lib/reliefos/apps/diskmgr/diskmgr.elf"),
                    const_cast<char *>("--elevated"), nullptr};
    pid_t child = -1;
    if (!result) result = posix_spawn(&child, args[0], &actions, nullptr, args, environ);
    if (initialized) posix_spawn_file_actions_destroy(&actions);
    close(in[0]); close(out[1]);
    if (result) { close(in[1]); close(out[0]); return -result; }
    char secret[257];
    size_t length = strlen(password);
    memcpy(secret, password, length); secret[length++] = '\n';
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(in[1], secret + sent, length - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    wipe(secret, sizeof(secret));
    close(in[1]);
    pid_ = child; input_ = out[0]; size_ = 0; output_[0] = 0;
    return 0;
}

int Elevation::poll()
{
    if (!busy()) return 0;
    for (;;) {
        if (size_ == sizeof(output_) - 1) {
            memmove(output_, output_ + 2048, size_ - 2048);
            size_ -= 2048;
        }
        ssize_t n = read(input_, output_ + size_, sizeof(output_) - size_ - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_ += static_cast<size_t>(n); output_[size_] = 0;
    }
    // sudo's optional PTY normalizes LF to CRLF. Accept the marker text,
    // which can only be emitted by the fixed, root-owned executable above.
    if (strstr(output_, "[diskmgr.elf] elevated FLTK window ready")) {
        close(input_); input_ = -1; pid_ = -1;
        return 2;
    }
    int status;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (!result || (result < 0 && errno == EINTR)) return 0;
    close(input_); input_ = -1; pid_ = -1;
    return 1;
}

void elevated_ready()
{
    fputs(Ready, stdout); fflush(stdout);
    int log = open("/var/log/diskmgr.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat info{};
    if (log >= 0 && (fstat(log, &info) < 0 || !S_ISREG(info.st_mode) || info.st_uid ||
                     info.st_nlink != 1 || (info.st_mode & 0077))) { close(log); log = -1; }
    if (log < 0) log = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (log >= 0) { (void)dup2(log, STDOUT_FILENO); (void)dup2(log, STDERR_FILENO); close(log); }
}
}
