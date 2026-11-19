#ifndef RELIEFOS_INSTALLER_JOB_H
#define RELIEFOS_INSTALLER_JOB_H

#include <stddef.h>
#include <sys/types.h>
#include "installer_update.h"

#ifdef __cplusplus
extern "C" {
#endif

struct installer_job_packet {
    unsigned complete, progress;
    int result, selected, root, esp;
    unsigned automatic, replaced, format_esp, success;
    char message[256], detail[768];
    struct installer_update_info info;
};

struct installer_job {
    pid_t pid;
    int fd, eof, exited, status, error, complete;
    size_t received;
    struct installer_job_packet incoming, latest, final;
};

typedef int (*installer_job_fn)(void *, struct installer_job_packet *);
/* The child owns disk I/O. Only the parent may access FLTK or curses. */
int installer_job_start(struct installer_job *, installer_job_fn, void *, const char *log);
/* Bounded nonblocking drain: 1 running, 0 complete, or negative errno. */
int installer_job_step(struct installer_job *);
/* Coalesce telemetry when the reader is slow; never wait in a copy callback. */
void installer_job_publish(const struct installer_job_packet *);
int installer_job_is_worker(void);

#ifdef __cplusplus
}
#endif
#endif
