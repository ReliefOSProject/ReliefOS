#include "profile_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rmail {
namespace {

bool assign_error(Text &error, const char *message) {
    error.clear();
    if (!error.set(message)) return false;
    return false;
}

bool absolute_safe_path(const char *path) {
    if (!path || path[0] != '/') return false;
    size_t length = strlen(path);
    if (!length || length >= PATH_MAX) return false;
    size_t at = 1;
    while (at < length) {
        while (at < length && path[at] == '/') ++at;
        const size_t start = at;
        while (at < length && path[at] != '/') ++at;
        const size_t count = at - start;
        if (!count) continue;
        if ((count == 1 && path[start] == '.') ||
            (count == 2 && path[start] == '.' && path[start + 1] == '.')) return false;
    }
    return true;
}

bool join_profile_path(Text &output, const char *base, const char *suffix) {
    if (!absolute_safe_path(base)) return false;
    size_t base_size = strlen(base);
    while (base_size > 1 && base[base_size - 1] == '/') --base_size;
    const size_t suffix_size = strlen(suffix);
    if (base_size > PATH_MAX - suffix_size - 2) return false;
    char path[PATH_MAX];
    memcpy(path, base, base_size);
    size_t at = base_size;
    if (!at || path[at - 1] != '/') path[at++] = '/';
    memcpy(path + at, suffix, suffix_size);
    at += suffix_size;
    path[at] = 0;
    return output.set(path, at);
}

const char *choose_xdg(const char *xdg, const char *fallback) {
    return xdg && xdg[0] == '/' ? xdg : fallback;
}

bool checked_component(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..") ||
        strchr(name, '/') || strlen(name) > NAME_MAX) return false;
    return true;
}

bool write_all(int fd, const unsigned char *bytes, size_t size, size_t fail_after) {
    size_t at = 0;
    while (at < size) {
        if (fail_after != SIZE_MAX && at >= fail_after) { errno = EIO; return false; }
        size_t amount = size - at;
        if (fail_after != SIZE_MAX && amount > fail_after - at) amount = fail_after - at;
        if (!amount) { errno = EIO; return false; }
        const ssize_t count = write(fd, bytes + at, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        at += static_cast<size_t>(count);
    }
    if (fail_after != SIZE_MAX && fail_after == size) { errno = EIO; return false; }
    return true;
}

bool create_temp_file(int directory_fd, char *name, size_t capacity, int &fd, Text &error) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        unsigned char random[16];
        if (!detail::random_bytes(random, sizeof(random), error)) return false;
        if (capacity < 5 + sizeof(random) * 2 + 1) {
            detail::wipe_memory(random, sizeof(random));
            return assign_error(error, "temporary file name buffer is too small");
        }
        memcpy(name, ".tmp-", 5);
        for (size_t i = 0; i < sizeof(random); ++i) {
            name[5 + i * 2] = hex[random[i] >> 4];
            name[6 + i * 2] = hex[random[i] & 15];
        }
        name[5 + sizeof(random) * 2] = 0;
        detail::wipe_memory(random, sizeof(random));
        fd = openat(directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) return true;
        if (errno != EEXIST) return assign_error(error, "cannot create private temporary file");
    }
    return assign_error(error, "cannot allocate unique temporary file");
}

bool validate_existing_file(int directory_fd, const char *name, bool required, Text &error) {
    int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (!required && errno == ENOENT) return true;
        return assign_error(error, "profile file is missing or unsafe");
    }
    struct stat info;
    const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
        fchmod(fd, 0600) == 0;
    const int close_result = close(fd);
    if (!valid || close_result < 0) return assign_error(error, "profile file owner or type is unsafe");
    return true;
}

bool write_temporary(int directory_fd, const void *bytes, size_t size,
    detail::IoFaultPoint fault, char *temporary, size_t temporary_capacity, Text &error) {
    int fd = -1;
    if (!create_temp_file(directory_fd, temporary, temporary_capacity, fd, error)) return false;
    const size_t fail_after = fault == detail::IoFaultPoint::Write ? size / 2 : SIZE_MAX;
    bool ok = write_all(fd, static_cast<const unsigned char *>(bytes), size, fail_after);
    if (ok && fsync(fd) < 0) ok = false;
    const int saved_errno = errno;
    if (close(fd) < 0) ok = false;
    if (!ok) {
        unlinkat(directory_fd, temporary, 0);
        errno = saved_errno;
        return assign_error(error, "cannot write private file");
    }
    return true;
}

} // namespace

bool profile_paths_from_env(const char *home, const char *xdg_config_home,
    const char *xdg_data_home, const char *xdg_cache_home,
    ProfilePaths &paths, Text &error) {
    error.clear();
    if (!home || !absolute_safe_path(home)) return assign_error(error, "HOME must be an absolute safe path");
    char fallback_config[PATH_MAX];
    char fallback_data[PATH_MAX];
    char fallback_cache[PATH_MAX];
    const size_t home_size = strlen(home);
    const char *config_suffix = ".config";
    const char *data_suffix = ".local/share";
    const char *cache_suffix = ".cache";
    if (home_size > PATH_MAX - 32) return assign_error(error, "HOME path is too long");
    snprintf(fallback_config, sizeof(fallback_config), "%s/%s", home, config_suffix);
    snprintf(fallback_data, sizeof(fallback_data), "%s/%s", home, data_suffix);
    snprintf(fallback_cache, sizeof(fallback_cache), "%s/%s", home, cache_suffix);
    const char *config_base = choose_xdg(xdg_config_home, fallback_config);
    const char *data_base = choose_xdg(xdg_data_home, fallback_data);
    const char *cache_base = choose_xdg(xdg_cache_home, fallback_cache);
    if (!absolute_safe_path(config_base) || !absolute_safe_path(data_base) ||
        !absolute_safe_path(cache_base)) return assign_error(error, "XDG path must be absolute and safe");
    ProfilePaths next;
    if (!join_profile_path(next.config_dir, config_base, "reliefmail") ||
        !join_profile_path(next.data_dir, data_base, "reliefmail") ||
        !join_profile_path(next.cache_dir, cache_base, "reliefmail"))
        return assign_error(error, "profile path is too long");
    paths.config_dir = static_cast<Text &&>(next.config_dir);
    paths.data_dir = static_cast<Text &&>(next.data_dir);
    paths.cache_dir = static_cast<Text &&>(next.cache_dir);
    return true;
}

bool profile_paths(ProfilePaths &paths, Text &error) {
    return profile_paths_from_env(getenv("HOME"), getenv("XDG_CONFIG_HOME"),
        getenv("XDG_DATA_HOME"), getenv("XDG_CACHE_HOME"), paths, error);
}

namespace detail {

bool open_secure_profile_directory(const Text &path, int &fd, Text &error) {
    fd = -1;
    if (!absolute_safe_path(path.c_str())) return assign_error(error, "profile directory path is unsafe");
    const size_t length = path.size();
    size_t leaf_start = length;
    while (leaf_start && path.c_str()[leaf_start - 1] != '/') --leaf_start;
    if (length - leaf_start != strlen("reliefmail") ||
        memcmp(path.c_str() + leaf_start, "reliefmail", strlen("reliefmail")))
        return assign_error(error, "profile directory must be named reliefmail");

    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) return assign_error(error, "cannot open filesystem root");
    size_t at = 1;
    while (at < length) {
        while (at < length && path.c_str()[at] == '/') ++at;
        if (at >= length) break;
        size_t end = at;
        while (end < length && path.c_str()[end] != '/') ++end;
        const size_t count = end - at;
        char component[NAME_MAX + 1];
        if (!count || count > NAME_MAX) {
            close(current);
            return assign_error(error, "profile path component is invalid");
        }
        memcpy(component, path.c_str() + at, count);
        component[count] = 0;
        int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(current, component, 0700) < 0 && errno != EEXIST) {
                close(current);
                return assign_error(error, "cannot create profile directory");
            }
            next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (next < 0) {
            close(current);
            return assign_error(error, "profile path contains a symlink or non-directory");
        }
        close(current);
        current = next;
        at = end;
    }
    struct stat info;
    if (fstat(current, &info) < 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()) {
        close(current);
        return assign_error(error, "profile directory owner is unsafe");
    }
    if ((info.st_mode & 0777) != 0700 && fchmod(current, 0700) < 0) {
        close(current);
        return assign_error(error, "cannot secure profile directory permissions");
    }
    fd = current;
    return true;
}

bool open_private_file_at(int directory_fd, const char *name, int flags, int &fd, Text &error) {
    fd = -1;
    if (!checked_component(name)) return assign_error(error, "profile file name is unsafe");
    int file = openat(directory_fd, name, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0) return assign_error(error, "cannot open private profile file");
    struct stat info;
    if (fstat(file, &info) < 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        fchmod(file, 0600) < 0) {
        close(file);
        return assign_error(error, "profile file owner or permissions are unsafe");
    }
    fd = file;
    return true;
}

bool random_bytes(unsigned char *output, size_t size, Text &error) {
    if (size && !output) return assign_error(error, "random output buffer is invalid");
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return assign_error(error, "operating-system random source is unavailable");
    size_t at = 0;
    while (at < size) {
        const ssize_t count = read(fd, output + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            return assign_error(error, "operating-system random source failed");
        }
        at += static_cast<size_t>(count);
    }
    if (close(fd) < 0) return assign_error(error, "operating-system random source close failed");
    return true;
}

bool read_private_file_at(int directory_fd, const char *name, size_t maximum_size,
    unsigned char **output, size_t &output_size, Text &error) {
    *output = nullptr;
    output_size = 0;
    int fd;
    if (!open_private_file_at(directory_fd, name, O_RDONLY, fd, error)) return false;
    struct stat info;
    if (fstat(fd, &info) < 0 || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > maximum_size) {
        close(fd);
        return assign_error(error, "private file size is invalid");
    }
    const size_t size = static_cast<size_t>(info.st_size);
    unsigned char *bytes = size ? static_cast<unsigned char *>(malloc(size)) : nullptr;
    if (size && !bytes) {
        close(fd);
        return assign_error(error, "out of memory reading private file");
    }
    size_t at = 0;
    while (at < size) {
        const ssize_t count = read(fd, bytes + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(bytes);
            close(fd);
            return assign_error(error, "private file is truncated");
        }
        at += static_cast<size_t>(count);
    }
    if (close(fd) < 0) {
        free(bytes);
        return assign_error(error, "private file close failed");
    }
    *output = bytes;
    output_size = size;
    return true;
}

bool write_unique_file_at(int directory_fd, const char *name,
    const void *bytes, size_t size, IoFaultPoint fault, Text &error) {
    if (!checked_component(name)) return assign_error(error, "cache file name is unsafe");
    char temporary[64];
    int fd = -1;
    if (!create_temp_file(directory_fd, temporary, sizeof(temporary), fd, error)) return false;
    const size_t fail_after = fault == IoFaultPoint::Write ? size / 2 : SIZE_MAX;
    bool ok = write_all(fd, static_cast<const unsigned char *>(bytes), size, fail_after);
    if (ok && fsync(fd) < 0) ok = false;
    const int saved_errno = errno;
    if (close(fd) < 0) ok = false;
    if (!ok) {
        unlinkat(directory_fd, temporary, 0);
        errno = saved_errno;
        return assign_error(error, "cannot write cache file");
    }
    if (fault == IoFaultPoint::LinkOrRename ||
        linkat(directory_fd, temporary, directory_fd, name, 0) < 0) {
        unlinkat(directory_fd, temporary, 0);
        return assign_error(error, "cannot publish cache file");
    }
    if (unlinkat(directory_fd, temporary, 0) < 0 || fsync(directory_fd) < 0) {
        unlinkat(directory_fd, name, 0);
        return assign_error(error, "cannot synchronize cache directory");
    }
    return true;
}

static bool sync_directory(int directory_fd, IoFaultPoint &fault, IoFaultPoint boundary) {
    if (fault == boundary || (boundary == IoFaultPoint::InstalledTargetSync &&
            fault == IoFaultPoint::AfterReplace)) {
        fault = IoFaultPoint::None;
        errno = EIO;
        return false;
    }
    return fsync(directory_fd) == 0;
}

static bool marker_exists(int directory_fd, const char *name, bool &exists, Text &error) {
    struct stat info;
    if (fstatat(directory_fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_uid != geteuid())
            return assign_error(error, "private recovery marker owner or type is unsafe");
        exists = true;
        return true;
    }
    if (errno == ENOENT) {
        exists = false;
        return true;
    }
    return assign_error(error, "cannot inspect private recovery marker");
}

static bool unlink_optional(int directory_fd, const char *name) {
    return unlinkat(directory_fd, name, 0) == 0 || errno == ENOENT;
}

static bool restore_from_marker(int directory_fd, const char *name, const char *marker, Text &error) {
    if (!validate_existing_file(directory_fd, marker, true, error) ||
        !validate_existing_file(directory_fd, name, false, error)) return false;
    if (!unlink_optional(directory_fd, name))
        return assign_error(error, "cannot remove uncommitted private file for rollback");
    if (linkat(directory_fd, marker, directory_fd, name, 0) < 0)
        return assign_error(error, "cannot restore previous private file from recovery marker");
    if (fsync(directory_fd) < 0)
        return assign_error(error, "previous private file is restored but directory sync failed; recovery marker retained");
    if (unlinkat(directory_fd, marker, 0) < 0)
        return assign_error(error, "previous private file is restored but recovery marker cleanup failed");
    if (fsync(directory_fd) < 0)
        return assign_error(error, "previous private file is restored but marker cleanup sync failed");
    return true;
}

ReplaceOutcome replace_private_file_at(int directory_fd, const char *name, const char *rollback_name,
    const char *committed_name, const void *bytes, size_t size, IoFaultPoint fault, Text &error) {
    if (!checked_component(name) || !checked_component(rollback_name) || !checked_component(committed_name)) {
        assign_error(error, "private replacement file name is unsafe");
        return ReplaceOutcome::Failed;
    }
    if (!recover_private_replace_at(directory_fd, name, rollback_name, committed_name, error))
        return ReplaceOutcome::RecoveryRequired;
    char temporary[64];
    if (!write_temporary(directory_fd, bytes, size,
            fault == IoFaultPoint::Write ? IoFaultPoint::Write : IoFaultPoint::None,
            temporary, sizeof(temporary), error)) return ReplaceOutcome::Failed;
    if (fault == IoFaultPoint::LinkOrRename) {
        unlinkat(directory_fd, temporary, 0);
        assign_error(error, "injected atomic replacement failure");
        return ReplaceOutcome::Failed;
    }
    if (!validate_existing_file(directory_fd, name, false, error)) {
        unlinkat(directory_fd, temporary, 0);
        return ReplaceOutcome::Failed;
    }
    bool had_old = false;
    if (linkat(directory_fd, name, directory_fd, rollback_name, 0) == 0) {
        had_old = true;
    } else if (errno != ENOENT) {
        unlinkat(directory_fd, temporary, 0);
        assign_error(error, "cannot preserve previous vault");
        return ReplaceOutcome::Failed;
    }
    if (had_old && !sync_directory(directory_fd, fault, IoFaultPoint::RollbackLinkSync)) {
        const bool removed = unlink_optional(directory_fd, rollback_name);
        const bool synced = fsync(directory_fd) == 0;
        if (!removed || !synced) {
            assign_error(error, "vault was not replaced; previous file remains active and rollback cleanup is pending");
            return ReplaceOutcome::RecoveryRequired;
        }
        unlinkat(directory_fd, temporary, 0);
        assign_error(error, "cannot synchronize previous-vault recovery link");
        return ReplaceOutcome::Failed;
    }
    if (renameat(directory_fd, temporary, directory_fd, name) < 0) {
        const bool cleaned = !had_old || unlink_optional(directory_fd, rollback_name);
        const bool synced = fsync(directory_fd) == 0;
        unlinkat(directory_fd, temporary, 0);
        if (!cleaned || !synced) {
            assign_error(error, "vault rename failed; previous file remains active with recovery cleanup pending");
            return ReplaceOutcome::RecoveryRequired;
        }
        assign_error(error, "cannot atomically replace private file");
        return ReplaceOutcome::Failed;
    }
    if (!sync_directory(directory_fd, fault, IoFaultPoint::InstalledTargetSync)) {
        bool restored = false;
        if (had_old) restored = restore_from_marker(directory_fd, name, rollback_name, error);
        else restored = unlink_optional(directory_fd, name) && fsync(directory_fd) == 0;
        if (restored) {
            assign_error(error, "vault replacement did not reach its installed-file sync boundary");
            return ReplaceOutcome::Failed;
        }
        assign_error(error, "vault replacement needs recovery; the previous file remains in its checked rollback marker");
        return ReplaceOutcome::RecoveryRequired;
    }

    if (had_old) {
        const bool renamed = fault != IoFaultPoint::CommitMarkerRename &&
            renameat(directory_fd, rollback_name, directory_fd, committed_name) == 0;
        if (!renamed) {
            if (fault == IoFaultPoint::CommitMarkerRename) { fault = IoFaultPoint::None; errno = EIO; }
            const bool restored = restore_from_marker(directory_fd, name, rollback_name, error);
            if (restored) {
                assign_error(error, "vault commit marker could not be installed; previous file was restored");
                return ReplaceOutcome::Failed;
            }
            assign_error(error, "vault commit marker failed; recovery is required and previous data remains marked");
            return ReplaceOutcome::RecoveryRequired;
        }
        if (!sync_directory(directory_fd, fault, IoFaultPoint::CommitMarkerSync)) {
            const bool reversed = renameat(directory_fd, committed_name, directory_fd, rollback_name) == 0 &&
                fsync(directory_fd) == 0;
            const bool restored = reversed && restore_from_marker(directory_fd, name, rollback_name, error);
            if (restored) {
                assign_error(error, "vault commit-marker sync failed; previous file was restored");
                return ReplaceOutcome::Failed;
            }
            assign_error(error, "vault commit state needs recovery; the previous file remains in a recovery marker");
            return ReplaceOutcome::RecoveryRequired;
        }
    }

    if (had_old) {
        const bool unlinked = fault != IoFaultPoint::CleanupUnlink && unlinkat(directory_fd, committed_name, 0) == 0;
        if (!unlinked) {
            if (fault == IoFaultPoint::CleanupUnlink) { fault = IoFaultPoint::None; errno = EIO; }
            assign_error(error, "vault update is committed; removal of the retained previous copy is pending");
            return ReplaceOutcome::CommittedCleanupPending;
        }
    }
    if (!sync_directory(directory_fd, fault, IoFaultPoint::CleanupSync)) {
        assign_error(error, "vault update is committed; cleanup directory sync is pending");
        return ReplaceOutcome::CommittedCleanupPending;
    }
    return ReplaceOutcome::Committed;
}

bool recover_private_replace_at(int directory_fd, const char *name,
    const char *rollback_name, const char *committed_name, Text &error) {
    if (!checked_component(name) || !checked_component(rollback_name) || !checked_component(committed_name))
        return assign_error(error, "private recovery file name is unsafe");
    bool committed_exists = false, rollback_exists = false, target_exists = false;
    if (!marker_exists(directory_fd, committed_name, committed_exists, error) ||
        !marker_exists(directory_fd, rollback_name, rollback_exists, error)) return false;
    struct stat target_info;
    if (fstatat(directory_fd, name, &target_info, AT_SYMLINK_NOFOLLOW) == 0) target_exists = true;
    else if (errno != ENOENT) return assign_error(error, "cannot inspect private replacement target");
    if (target_exists && !validate_existing_file(directory_fd, name, true, error)) return false;

    if (committed_exists) {
        if (!validate_existing_file(directory_fd, committed_name, true, error)) return false;
        if (!target_exists) {
            if (!restore_from_marker(directory_fd, name, committed_name, error)) return false;
        } else {
            if (rollback_exists && !unlink_optional(directory_fd, rollback_name))
                return assign_error(error, "cannot remove obsolete private rollback marker");
            if (!unlink_optional(directory_fd, committed_name) || fsync(directory_fd) < 0)
                return assign_error(error, "committed private file is active but cleanup is pending");
        }
        return true;
    }
    if (rollback_exists) return restore_from_marker(directory_fd, name, rollback_name, error);
    return true;
}

void wipe_memory(void *bytes, size_t size) {
    volatile unsigned char *cursor = static_cast<volatile unsigned char *>(bytes);
    while (size--) *cursor++ = 0;
}

} // namespace detail
} // namespace rmail
