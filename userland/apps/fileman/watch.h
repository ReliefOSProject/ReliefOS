#ifndef RELIEFOS_FILEMAN_WATCH_H
#define RELIEFOS_FILEMAN_WATCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fileman_watch {
    int fd;
    int wd;
};

enum {
    FILEMAN_WATCH_CHANGED = 1U,
    FILEMAN_WATCH_OVERFLOW = 2U,
    FILEMAN_WATCH_LOST = 4U,
    FILEMAN_WATCH_ERROR = 8U
};

int fileman_watch_open(struct fileman_watch *watch);
int fileman_watch_set(struct fileman_watch *watch, const char *path);
unsigned fileman_watch_consume(const struct fileman_watch *watch, const void *data, size_t size);
unsigned fileman_watch_drain(struct fileman_watch *watch);
void fileman_watch_close(struct fileman_watch *watch);

#ifdef __cplusplus
}
#endif
#endif
