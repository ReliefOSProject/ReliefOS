#ifndef RELIEFOS_ASKPASS_PASSWORD_IO_H
#define RELIEFOS_ASKPASS_PASSWORD_IO_H

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* The protocol is one byte-bounded line. FLTK limits UTF-8 characters instead. */
static int askpass_write_password(const char *password)
{
    if (!password) { errno = EINVAL; return 1; }
    size_t length = strnlen(password, 256);
    if (length > 255) { errno = E2BIG; return 1; }
    if (memchr(password, '\n', length) || memchr(password, '\r', length)) {
        errno = EINVAL;
        return 1;
    }
    char response[257] = {0};
    memcpy(response, password, length);
    response[length++] = '\n';
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(STDOUT_FILENO, response + done, length - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += (size_t)count;
    }
    explicit_bzero(response, sizeof(response));
    return done == length ? 0 : 1;
}

#endif
