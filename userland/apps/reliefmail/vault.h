#ifndef RELIEFMAIL_VAULT_H
#define RELIEFMAIL_VAULT_H

#include "store.h"

namespace rmail {

enum class VaultSaveResult : uint8_t { Failed, Committed, CommittedCleanupPending, RecoveryRequired };
enum class VaultPasswordChangeResult : uint8_t {
    Failed, Committed, CommittedCleanupPending, RecoveryRequired
};

/* Move-only memory whose owned bytes are wiped before release. */
class SecretBytes {
    unsigned char *data_ = nullptr;
    size_t size_ = 0;

    SecretBytes(const SecretBytes &) = delete;
    SecretBytes &operator=(const SecretBytes &) = delete;

public:
    SecretBytes() = default;
    SecretBytes(SecretBytes &&other) noexcept;
    SecretBytes &operator=(SecretBytes &&other) noexcept;
    ~SecretBytes();

    bool set(const void *bytes, size_t size);
    void clear();
    const unsigned char *data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
};

class Vault {
    int directory_fd_ = -1;
    Vault(const Vault &) = delete;
    Vault &operator=(const Vault &) = delete;

public:
    static constexpr size_t MaxFileBytes = 1024u * 1024u;

    Vault() = default;
    Vault(Vault &&other) noexcept;
    Vault &operator=(Vault &&other) noexcept;
    ~Vault();

    bool open(const ProfilePaths &paths, Text &error);
    void close();
    bool exists() const;
    bool unlock(SecretBytes master_password, SecretBytes &credentials, Text &error);
    /* Compatibility wrapper: true includes committed cleanup-pending; inspect error/result overload for detail. */
    bool save(SecretBytes master_password, SecretBytes credentials, Text &error);
    VaultSaveResult save_with_status(SecretBytes master_password, SecretBytes credentials, Text &error);
    VaultPasswordChangeResult change_password_with_status(
        SecretBytes old_password, SecretBytes new_password, Text &error);
    /* Compatibility wrapper returns true for committed and cleanup-pending outcomes. */
    bool change_password(SecretBytes old_password, SecretBytes new_password, Text &error);
    static bool validate_blob(const unsigned char *bytes, size_t size, Text &error);
    /* Consumes the master password and verifies the GCM tag without retaining plaintext. */
    static bool authenticate_blob(const unsigned char *bytes, size_t size,
        SecretBytes master_password, Text &error);
};

#ifdef RELIEFMAIL_TESTING
enum class VaultFaultPoint : uint8_t {
    None, TempWrite, Rename, AfterRename, RollbackLinkSync, InstalledTargetSync,
    CommitMarkerRename, CommitMarkerSync, CleanupUnlink, CleanupSync
};
void vault_test_fail_next(VaultFaultPoint point);
bool vault_test_pbkdf2(const void *password, size_t password_size,
    const void *salt, size_t salt_size, uint32_t iterations,
    SecretBytes &output, Text &error);
bool vault_test_aes_gcm_encrypt(const unsigned char key[32], const unsigned char nonce[12],
    const void *aad, size_t aad_size, const void *plaintext, size_t plaintext_size,
    Bytes &ciphertext, unsigned char tag[16], Text &error);
#endif

}

#endif
