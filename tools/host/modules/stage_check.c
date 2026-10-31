/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int kernel_stage_check(int argc, char **argv)
{
    if (argc != 3) return 2;
    json_value document = {0};
    int root = open(argv[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool valid = root >= 0 && !kernel_stage_document(root, argv[1], &document) &&
                 !kernel_stage_verify(root, json_member(&document, "files"));
    if (root >= 0) close(root);
    json_free(&document);
    struct stat status;
    int exists = lstat(argv[2], &status);
    if (!exists && !S_ISREG(status.st_mode)) { fputs("kernel-tools: unsafe presence stamp\n", stderr); return 1; }
    if (valid && !exists) return 0;
    struct kernel_stage temporary = {.root_fd = -1, .parent_fd = -1};
    int result = kernel_stage_begin(&temporary, argv[2]);
    if (!result) {
        int fd = openat(temporary.parent_fd, temporary.leaf, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
        if (fd < 0) result = -1;
        else {
            result = fstat(fd, &status) || !S_ISREG(status.st_mode) || futimens(fd, NULL) || fsync(fd);
            close(fd);
        }
    }
    kernel_stage_end(&temporary);
    if (result) fprintf(stderr, "kernel-tools: cannot update presence stamp: %s\n", strerror(errno));
    return result ? 1 : 0;
}
