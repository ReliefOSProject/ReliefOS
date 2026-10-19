#include "vault.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rmail;

static bool same_secret(const SecretBytes &value, const char *expected) {
    const size_t length = strlen(expected);
    return value.size() == length && (!length || memcmp(value.data(), expected, length) == 0);
}

static bool remove_tree(const char *path) {
    struct stat info;
    if (lstat(path, &info) < 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        const int count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (count < 0 || static_cast<size_t>(count) >= sizeof(child) || !remove_tree(child)) ok = false;
    }
    if (closedir(directory) < 0) ok = false;
    if (rmdir(path) < 0) ok = false;
    return ok;
}

static SecretBytes secret(const char *value) {
    SecretBytes result;
    assert(result.set(value, strlen(value)));
    return result;
}

static bool read_file(const char *path, Bytes &out) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info;
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > Vault::MaxFileBytes) {
        close(fd);
        return false;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    unsigned char *buffer = size ? static_cast<unsigned char *>(malloc(size)) : nullptr;
    if (size && !buffer) { close(fd); return false; }
    size_t at = 0;
    while (at < size) {
        const ssize_t count = read(fd, buffer + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { free(buffer); close(fd); return false; }
        at += static_cast<size_t>(count);
    }
    const bool ok = out.set(buffer, size);
    free(buffer);
    close(fd);
    return ok;
}

static bool write_file(const char *path, const void *bytes, size_t size) {
    const int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    size_t at = 0;
    while (at < size) {
        const ssize_t count = write(fd, static_cast<const unsigned char *>(bytes) + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return false; }
        at += static_cast<size_t>(count);
    }
    const bool ok = fsync(fd) == 0 && close(fd) == 0;
    return ok;
}

struct VaultFixture {
    char root[PATH_MAX]{};
    ProfilePaths paths;
    Vault vault;

    VaultFixture() {
        char current[PATH_MAX];
        assert(getcwd(current, sizeof(current)));
        const int count = snprintf(root, sizeof(root), "%s/out/tmp/reliefmail-vault-XXXXXX", current);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(root));
        assert(mkdtemp(root));
        Text error;
        assert(profile_paths_from_env(root, nullptr, nullptr, nullptr, paths, error));
        assert(vault.open(paths, error));
    }

    ~VaultFixture() { assert(remove_tree(root)); }

    void file_path(char *path, size_t capacity) const {
        const int count = snprintf(path, capacity, "%s/credentials.vault", paths.config_dir.c_str());
        assert(count > 0 && static_cast<size_t>(count) < capacity);
    }
};

static void test_independent_pbkdf2_and_aes_gcm_vectors() {
    SecretBytes derived;
    Text error;
    assert(vault_test_pbkdf2("password", 8, "salt", 4, 1, derived, error));
    const unsigned char expected_key[32] = {
        0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c,0x43,0xe7,0x22,0x52,0x56,0xc4,0xf8,0x37,
        0xa8,0x65,0x48,0xc9,0x2c,0xcc,0x35,0x48,0x08,0x05,0x98,0x7c,0xb7,0x0b,0xe1,0x7b
    };
    assert(derived.size() == sizeof(expected_key));
    assert(memcmp(derived.data(), expected_key, sizeof(expected_key)) == 0);

    unsigned char key[32]{};
    unsigned char nonce[12]{};
    const unsigned char plaintext[16]{};
    Bytes ciphertext;
    unsigned char tag[16]{};
    assert(vault_test_aes_gcm_encrypt(key, nonce, nullptr, 0, plaintext, sizeof(plaintext),
        ciphertext, tag, error));
    const unsigned char expected_ciphertext[16] = {
        0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18
    };
    const unsigned char expected_tag[16] = {
        0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19
    };
    assert(ciphertext.size() == sizeof(expected_ciphertext));
    assert(memcmp(ciphertext.data(), expected_ciphertext, sizeof(expected_ciphertext)) == 0);
    assert(memcmp(tag, expected_tag, sizeof(expected_tag)) == 0);
}

static void test_randomized_authenticated_vault_round_trip_and_password_change() {
    VaultFixture fixture;
    Text error;
    const char marker[] = "REC0GNIZABLE-CREDENTIAL-DO-NOT-STORE-IN-PLAINTEXT";
    assert(fixture.vault.save(secret("master pass phrase"), secret(marker), error));
    char path[PATH_MAX];
    fixture.file_path(path, sizeof(path));
    Bytes first;
    assert(read_file(path, first));
    assert(first.size() > 64 && !first.empty());
    assert((first.data()[0] == 'R') && (first.data()[1] == 'M'));
    for (size_t at = 0; at + sizeof(marker) - 1 <= first.size(); ++at)
        assert(memcmp(first.data() + at, marker, sizeof(marker) - 1) != 0);

    SecretBytes unlocked;
    assert(fixture.vault.unlock(secret("master pass phrase"), unlocked, error));
    assert(same_secret(unlocked, marker));
    assert(!fixture.vault.unlock(secret("wrong password"), unlocked, error));
    assert(read_file(path, first));
    const Bytes before_second_save = first;
    assert(fixture.vault.save(secret("master pass phrase"), secret(marker), error));
    Bytes second;
    assert(read_file(path, second));
    assert(second != before_second_save);

    assert(fixture.vault.change_password(secret("master pass phrase"),
        secret("new master pass phrase"), error));
    assert(!fixture.vault.unlock(secret("master pass phrase"), unlocked, error));
    assert(fixture.vault.unlock(secret("new master pass phrase"), unlocked, error));
    assert(same_secret(unlocked, marker));
}

static void test_authenticated_metadata_tag_ciphertext_and_size_failures_are_read_only() {
    VaultFixture fixture;
    Text error;
    assert(fixture.vault.save(secret("master"), secret("secret payload"), error));
    char path[PATH_MAX];
    fixture.file_path(path, sizeof(path));
    Bytes valid;
    assert(read_file(path, valid));

    const size_t mutation_offsets[] = {8, 12, 48, valid.size() - 1};
    for (size_t index = 0; index < sizeof(mutation_offsets) / sizeof(mutation_offsets[0]); ++index) {
        Bytes tampered;
        assert(tampered.set(valid.data(), valid.size()));
        unsigned char *changed = const_cast<unsigned char *>(tampered.data());
        changed[mutation_offsets[index]] ^= 0x40;
        assert(write_file(path, tampered.data(), tampered.size()));
        Bytes exact_before;
        assert(read_file(path, exact_before) && exact_before == tampered);
        SecretBytes unlocked;
        assert(!fixture.vault.unlock(secret("master"), unlocked, error));
        Bytes exact_after;
        assert(read_file(path, exact_after) && exact_after == exact_before);
    }

    assert(write_file(path, valid.data(), valid.size() - 1));
    SecretBytes unlocked;
    assert(!fixture.vault.unlock(secret("master"), unlocked, error));
    struct stat info;
    assert(stat(path, &info) == 0 && static_cast<size_t>(info.st_size) == valid.size() - 1);

    unsigned char *oversized = static_cast<unsigned char *>(malloc(Vault::MaxFileBytes + 1));
    assert(oversized);
    memset(oversized, 0, Vault::MaxFileBytes + 1);
    assert(write_file(path, oversized, Vault::MaxFileBytes + 1));
    free(oversized);
    assert(!fixture.vault.unlock(secret("master"), unlocked, error));
    assert(stat(path, &info) == 0 && static_cast<uint64_t>(info.st_size) == Vault::MaxFileBytes + 1);
}

static void test_iteration_bounds_and_atomic_replacement_failures_keep_old_vault() {
    VaultFixture fixture;
    Text error;
    assert(fixture.vault.save(secret("master"), secret("old credentials"), error));
    char path[PATH_MAX];
    fixture.file_path(path, sizeof(path));
    Bytes valid;
    assert(read_file(path, valid));

    Bytes out_of_range;
    assert(out_of_range.set(valid.data(), valid.size()));
    unsigned char *metadata = const_cast<unsigned char *>(out_of_range.data());
    metadata[12] = 0xff;
    metadata[13] = 0xff;
    metadata[14] = 0xff;
    metadata[15] = 0xff;
    assert(write_file(path, out_of_range.data(), out_of_range.size()));
    SecretBytes unlocked;
    assert(!fixture.vault.unlock(secret("master"), unlocked, error));
    assert(read_file(path, out_of_range));

    assert(write_file(path, valid.data(), valid.size()));
    const VaultFaultPoint faults[] = {
        VaultFaultPoint::TempWrite, VaultFaultPoint::Rename, VaultFaultPoint::AfterRename,
        VaultFaultPoint::RollbackLinkSync, VaultFaultPoint::InstalledTargetSync,
        VaultFaultPoint::CommitMarkerRename, VaultFaultPoint::CommitMarkerSync
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        vault_test_fail_next(faults[i]);
        assert(!fixture.vault.save(secret("master"), secret("new credentials"), error));
        Bytes still_valid;
        assert(read_file(path, still_valid) && still_valid == valid);
        assert(fixture.vault.unlock(secret("master"), unlocked, error));
        assert(same_secret(unlocked, "old credentials"));
    }

    vault_test_fail_next(VaultFaultPoint::Rename);
    assert(!fixture.vault.change_password(secret("master"), secret("new master"), error));
    Bytes still_valid;
    assert(read_file(path, still_valid) && still_valid == valid);
    assert(fixture.vault.unlock(secret("master"), unlocked, error));
    assert(!fixture.vault.unlock(secret("new master"), unlocked, error));
}

static void test_vault_cleanup_failures_report_committed_state_and_recover() {
    VaultFixture fixture;
    Text error;
    assert(fixture.vault.save(secret("old master"), secret("old credentials"), error));
    const VaultFaultPoint cleanup_faults[] = {VaultFaultPoint::CleanupUnlink, VaultFaultPoint::CleanupSync};
    for (size_t i = 0; i < sizeof(cleanup_faults) / sizeof(cleanup_faults[0]); ++i) {
        vault_test_fail_next(cleanup_faults[i]);
        const VaultSaveResult result = fixture.vault.save_with_status(secret("new master"),
            secret("new credentials"), error);
        assert(result == VaultSaveResult::CommittedCleanupPending);
        assert(!error.empty());
        SecretBytes unlocked;
        assert(fixture.vault.unlock(secret("new master"), unlocked, error));
        assert(same_secret(unlocked, "new credentials"));
        char marker[PATH_MAX];
        assert(snprintf(marker, sizeof(marker), "%s/.credentials.vault.committed-old",
            fixture.paths.config_dir.c_str()) > 0);
        if (cleanup_faults[i] == VaultFaultPoint::CleanupUnlink)
            assert(access(marker, F_OK) == 0);
        Vault reopened;
        assert(reopened.open(fixture.paths, error));
        assert(reopened.unlock(secret("new master"), unlocked, error));
        assert(same_secret(unlocked, "new credentials"));
        assert(access(marker, F_OK) < 0 && errno == ENOENT);
        assert(snprintf(marker, sizeof(marker), "%s/.credentials.vault.rollback",
            fixture.paths.config_dir.c_str()) > 0);
        assert(access(marker, F_OK) < 0 && errno == ENOENT);
    }
}

static void test_typed_password_change_preserves_commit_and_recovery_outcomes() {
    VaultFixture committed;
    Text error;
    assert(committed.vault.save(secret("old master"), secret("remembered payload"), error));
    vault_test_fail_next(VaultFaultPoint::CleanupUnlink);
    const VaultPasswordChangeResult cleanup = committed.vault.change_password_with_status(
        secret("old master"), secret("new master"), error);
    assert(cleanup == VaultPasswordChangeResult::CommittedCleanupPending);
    assert(!error.empty());
    Vault reopened;
    assert(reopened.open(committed.paths, error));
    SecretBytes payload;
    assert(!reopened.unlock(secret("old master"), payload, error));
    assert(reopened.unlock(secret("new master"), payload, error));
    assert(same_secret(payload, "remembered payload"));

    VaultFixture recovery;
    assert(recovery.vault.save(secret("stable master"), secret("stable payload"), error));
    char marker[PATH_MAX];
    assert(snprintf(marker, sizeof(marker), "%s/.credentials.vault.rollback",
        recovery.paths.config_dir.c_str()) > 0);
    assert(symlink("unsafe-marker-target", marker) == 0);
    Bytes before;
    char vault_path[PATH_MAX];
    recovery.file_path(vault_path, sizeof(vault_path));
    assert(read_file(vault_path, before));
    const VaultPasswordChangeResult unresolved = recovery.vault.change_password_with_status(
        secret("stable master"), secret("replacement master"), error);
    assert(unresolved == VaultPasswordChangeResult::RecoveryRequired);
    Bytes after;
    assert(read_file(vault_path, after) && after == before);
    assert(unlink(marker) == 0);
    Vault recovered;
    assert(recovered.open(recovery.paths, error));
    assert(recovered.unlock(secret("stable master"), payload, error));
    assert(same_secret(payload, "stable payload"));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--task5-password-change")) {
        test_typed_password_change_preserves_commit_and_recovery_outcomes();
        puts("ReliefMail Task5 Vault password-change suite passed");
        return 0;
    }
    test_independent_pbkdf2_and_aes_gcm_vectors();
    test_randomized_authenticated_vault_round_trip_and_password_change();
    test_authenticated_metadata_tag_ciphertext_and_size_failures_are_read_only();
    test_iteration_bounds_and_atomic_replacement_failures_keep_old_vault();
    test_vault_cleanup_failures_report_committed_state_and_recover();
    puts("ReliefMail vault suite passed");
    return 0;
}
