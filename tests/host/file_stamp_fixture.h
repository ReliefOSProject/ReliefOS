#ifndef RELIEFOS_TEST_FILE_STAMP_FIXTURE_H
#define RELIEFOS_TEST_FILE_STAMP_FIXTURE_H

#include <assert.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>

// Filesystems can assign the same ctime to consecutive operations. Construct
// an observable metadata change before asking an application to reject it;
// retry the real mutation rather than assuming a fixed delay is sufficient.
template <typename Change>
void change_file_metadata(const char *path, Change change) {
    struct stat before, after;
    struct timespec start, now;
    assert(lstat(path, &before) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        change();
        assert(lstat(path, &after) == 0);
#if defined(__APPLE__)
        if (before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
            before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) return;
#else
        if (before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return;
#endif
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 3 && "fixture could not advance the file ctime");
        assert(sched_yield() == 0);
    }
}

#endif
