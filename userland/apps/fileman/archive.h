#ifndef RELIEFOS_FILEMAN_ARCHIVE_H
#define RELIEFOS_FILEMAN_ARCHIVE_H
#include <stddef.h>
int fileman_tar_create(const char *archive, const char *directory,
                       const char *const *members, size_t count);
int fileman_tar_extract(const char *archive, const char *directory);
#endif
