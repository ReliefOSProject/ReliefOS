#ifndef RELIEFMAIL_PROFILE_IO_H
#define RELIEFMAIL_PROFILE_IO_H

#include "store.h"

namespace rmail { namespace detail {

enum class IoFaultPoint : uint8_t {
    None, Write, LinkOrRename, AfterReplace, RollbackLinkSync, InstalledTargetSync,
    CommitMarkerRename, CommitMarkerSync, CleanupUnlink, CleanupSync
};
enum class ReplaceOutcome : uint8_t { Failed, Committed, CommittedCleanupPending, RecoveryRequired };

bool open_secure_profile_directory(const Text &path, int &fd, Text &error);
bool open_private_file_at(int directory_fd, const char *name, int flags,
    int &fd, Text &error);
bool random_bytes(unsigned char *output, size_t size, Text &error);
bool read_private_file_at(int directory_fd, const char *name, size_t maximum_size,
    unsigned char **output, size_t &output_size, Text &error);
bool write_unique_file_at(int directory_fd, const char *name,
    const void *bytes, size_t size, IoFaultPoint fault, Text &error);
ReplaceOutcome replace_private_file_at(int directory_fd, const char *name, const char *rollback_name,
    const char *committed_name,
    const void *bytes, size_t size, IoFaultPoint fault, Text &error);
bool recover_private_replace_at(int directory_fd, const char *name,
    const char *rollback_name, const char *committed_name, Text &error);
void wipe_memory(void *bytes, size_t size);

} }

#endif
