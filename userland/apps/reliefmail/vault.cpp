#include "vault.h"
#include "profile_io.h"

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rmail {
namespace {

constexpr size_t HeaderBytes = 48;
constexpr size_t SaltBytes = 16;
constexpr size_t NonceBytes = 12;
constexpr size_t TagBytes = 16;
constexpr size_t KeyBytes = 32;
constexpr uint32_t VaultIterations = 600000;
constexpr uint32_t MinimumIterations = 600000;
constexpr uint32_t MaximumIterations = 1200000;
constexpr size_t MaximumPasswordBytes = 4096;
constexpr unsigned char VaultVersion = 1;
constexpr unsigned char VaultAes256Gcm = 1;
constexpr unsigned char VaultPbkdf2Sha256 = 1;

#ifdef RELIEFMAIL_TESTING
VaultFaultPoint next_vault_fault = VaultFaultPoint::None;

detail::IoFaultPoint consume_vault_fault() {
    const VaultFaultPoint point = next_vault_fault;
    next_vault_fault = VaultFaultPoint::None;
    switch (point) {
    case VaultFaultPoint::TempWrite: return detail::IoFaultPoint::Write;
    case VaultFaultPoint::Rename: return detail::IoFaultPoint::LinkOrRename;
    case VaultFaultPoint::AfterRename: return detail::IoFaultPoint::AfterReplace;
    case VaultFaultPoint::RollbackLinkSync: return detail::IoFaultPoint::RollbackLinkSync;
    case VaultFaultPoint::InstalledTargetSync: return detail::IoFaultPoint::InstalledTargetSync;
    case VaultFaultPoint::CommitMarkerRename: return detail::IoFaultPoint::CommitMarkerRename;
    case VaultFaultPoint::CommitMarkerSync: return detail::IoFaultPoint::CommitMarkerSync;
    case VaultFaultPoint::CleanupUnlink: return detail::IoFaultPoint::CleanupUnlink;
    case VaultFaultPoint::CleanupSync: return detail::IoFaultPoint::CleanupSync;
    case VaultFaultPoint::None: break;
    }
    return detail::IoFaultPoint::None;
}
#endif

bool set_error(Text &error, const char *message) {
    error.clear();
    error.set(message);
    return false;
}

uint32_t read_u32_be(const unsigned char *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

void write_u32_be(unsigned char *bytes, uint32_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 24);
    bytes[1] = static_cast<unsigned char>(value >> 16);
    bytes[2] = static_cast<unsigned char>(value >> 8);
    bytes[3] = static_cast<unsigned char>(value);
}

bool derive_key(const SecretBytes &password, const unsigned char *salt, uint32_t iterations,
    SecretBytes &key, Text &error) {
    if (password.empty() || password.size() > MaximumPasswordBytes ||
        password.size() > UINT_MAX || iterations < MinimumIterations || iterations > MaximumIterations)
        return set_error(error, "vault password or KDF parameters are invalid");
    unsigned char output[KeyBytes];
    const int code = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        password.data(), static_cast<uint32_t>(password.size()), salt, SaltBytes,
        iterations, KeyBytes, output);
    if (code != 0) {
        mbedtls_platform_zeroize(output, sizeof(output));
        return set_error(error, "vault key derivation failed");
    }
    const bool copied = key.set(output, sizeof(output));
    mbedtls_platform_zeroize(output, sizeof(output));
    return copied || set_error(error, "out of memory deriving vault key");
}

bool encrypt_payload(const unsigned char key[KeyBytes], const unsigned char nonce[NonceBytes],
    const unsigned char *aad, size_t aad_size, const unsigned char *plaintext, size_t plaintext_size,
    unsigned char *ciphertext, unsigned char tag[TagBytes]) {
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int code = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
    unsigned char empty = 0;
    const unsigned char *input = plaintext_size ? plaintext : &empty;
    unsigned char *output = plaintext_size ? ciphertext : &empty;
    if (code == 0)
        code = mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT, plaintext_size,
            nonce, NonceBytes, aad, aad_size, input, output, TagBytes, tag);
    mbedtls_gcm_free(&context);
    return code == 0;
}

bool decrypt_payload(const unsigned char key[KeyBytes], const unsigned char nonce[NonceBytes],
    const unsigned char *aad, size_t aad_size, const unsigned char *ciphertext,
    size_t ciphertext_size, const unsigned char tag[TagBytes], unsigned char *plaintext) {
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int code = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
    unsigned char empty = 0;
    const unsigned char *input = ciphertext_size ? ciphertext : &empty;
    unsigned char *output = ciphertext_size ? plaintext : &empty;
    if (code == 0)
        code = mbedtls_gcm_auth_decrypt(&context, ciphertext_size, nonce, NonceBytes,
            aad, aad_size, tag, TagBytes, input, output);
    mbedtls_gcm_free(&context);
    return code == 0;
}

bool build_blob(const SecretBytes &password, const SecretBytes &credentials,
    Bytes &blob, Text &error) {
    if (credentials.size() > Vault::MaxFileBytes - HeaderBytes - TagBytes ||
        credentials.size() > UINT32_MAX)
        return set_error(error, "vault credentials exceed the file size limit");
    const size_t total = HeaderBytes + credentials.size() + TagBytes;
    unsigned char *bytes = static_cast<unsigned char *>(malloc(total));
    if (!bytes) return set_error(error, "out of memory creating vault data");
    memset(bytes, 0, HeaderBytes);
    bytes[0] = 'R'; bytes[1] = 'M'; bytes[2] = 'V'; bytes[3] = 'L';
    bytes[4] = VaultVersion;
    bytes[5] = VaultAes256Gcm;
    bytes[6] = VaultPbkdf2Sha256;
    bytes[7] = 0;
    if (!detail::random_bytes(bytes + 8, 4, error) ||
        !detail::random_bytes(bytes + 16, SaltBytes, error) ||
        !detail::random_bytes(bytes + 32, NonceBytes, error)) {
        detail::wipe_memory(bytes, total);
        free(bytes);
        return false;
    }
    write_u32_be(bytes + 12, VaultIterations);
    write_u32_be(bytes + 44, static_cast<uint32_t>(credentials.size()));
    SecretBytes key;
    if (!derive_key(password, bytes + 16, VaultIterations, key, error)) {
        detail::wipe_memory(bytes, total);
        free(bytes);
        return false;
    }
    if (!encrypt_payload(key.data(), bytes + 32, bytes, HeaderBytes,
            credentials.data(), credentials.size(), bytes + HeaderBytes,
            bytes + HeaderBytes + credentials.size())) {
        detail::wipe_memory(bytes, total);
        free(bytes);
        return set_error(error, "vault encryption failed");
    }
    const bool copied = blob.set(bytes, total);
    detail::wipe_memory(bytes, total);
    free(bytes);
    return copied || set_error(error, "out of memory creating vault data");
}

bool decrypt_blob(const unsigned char *bytes, size_t size, const SecretBytes &password,
    SecretBytes &credentials, Text &error) {
    if (!Vault::validate_blob(bytes, size, error)) return false;
    const size_t payload_size = read_u32_be(bytes + 44);
    SecretBytes key;
    if (!derive_key(password, bytes + 16, read_u32_be(bytes + 12), key, error)) return false;
    unsigned char *plaintext = payload_size ? static_cast<unsigned char *>(malloc(payload_size)) : nullptr;
    if (payload_size && !plaintext) return set_error(error, "out of memory unlocking vault");
    const unsigned char *ciphertext = bytes + HeaderBytes;
    const unsigned char *tag = ciphertext + payload_size;
    const bool authenticated = decrypt_payload(key.data(), bytes + 32, bytes, HeaderBytes,
        ciphertext, payload_size, tag, plaintext);
    if (!authenticated) {
        if (plaintext) {
            mbedtls_platform_zeroize(plaintext, payload_size);
            free(plaintext);
        }
        return set_error(error, "vault password is wrong or stored data is damaged");
    }
    const bool copied = credentials.set(plaintext, payload_size);
    if (plaintext) {
        mbedtls_platform_zeroize(plaintext, payload_size);
        free(plaintext);
    }
    return copied || set_error(error, "out of memory unlocking vault");
}

} // namespace

SecretBytes::SecretBytes(SecretBytes &&other) noexcept {
    data_ = other.data_; other.data_ = nullptr;
    size_ = other.size_; other.size_ = 0;
}

SecretBytes &SecretBytes::operator=(SecretBytes &&other) noexcept {
    if (this == &other) return *this;
    clear();
    data_ = other.data_; other.data_ = nullptr;
    size_ = other.size_; other.size_ = 0;
    return *this;
}

SecretBytes::~SecretBytes() { clear(); }

bool SecretBytes::set(const void *bytes, size_t size) {
    if (!size) { clear(); return true; }
    if (!bytes) return false;
    unsigned char *next = static_cast<unsigned char *>(malloc(size));
    if (!next) return false;
    memcpy(next, bytes, size);
    clear();
    data_ = next;
    size_ = size;
    return true;
}

void SecretBytes::clear() {
    if (data_) {
        mbedtls_platform_zeroize(data_, size_);
        free(data_);
    }
    data_ = nullptr;
    size_ = 0;
}

Vault::Vault(Vault &&other) noexcept {
    directory_fd_ = other.directory_fd_;
    other.directory_fd_ = -1;
}

Vault &Vault::operator=(Vault &&other) noexcept {
    if (this == &other) return *this;
    close();
    directory_fd_ = other.directory_fd_;
    other.directory_fd_ = -1;
    return *this;
}

Vault::~Vault() { close(); }

bool Vault::open(const ProfilePaths &paths, Text &error) {
    close();
    error.clear();
    if (!detail::open_secure_profile_directory(paths.config_dir, directory_fd_, error)) return false;
    if (!detail::recover_private_replace_at(directory_fd_, "credentials.vault",
            ".credentials.vault.rollback", ".credentials.vault.committed-old", error)) {
        close();
        return false;
    }
    return true;
}

void Vault::close() {
    if (directory_fd_ >= 0) ::close(directory_fd_);
    directory_fd_ = -1;
}

bool Vault::exists() const {
    if (directory_fd_ < 0) return false;
    struct stat info;
    return fstatat(directory_fd_, "credentials.vault", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(info.st_mode) && info.st_uid == geteuid();
}

bool Vault::validate_blob(const unsigned char *bytes, size_t size, Text &error) {
    error.clear();
    if (!bytes || size < HeaderBytes + TagBytes || size > MaxFileBytes ||
        memcmp(bytes, "RMVL", 4) || bytes[4] != VaultVersion ||
        bytes[5] != VaultAes256Gcm || bytes[6] != VaultPbkdf2Sha256 || bytes[7] != 0)
        return set_error(error, "vault format or size is unsupported");
    const uint32_t iterations = read_u32_be(bytes + 12);
    const uint32_t payload_size = read_u32_be(bytes + 44);
    if (iterations < MinimumIterations || iterations > MaximumIterations ||
        static_cast<size_t>(payload_size) != size - HeaderBytes - TagBytes)
        return set_error(error, "vault metadata is invalid");
    return true;
}

bool Vault::authenticate_blob(const unsigned char *bytes, size_t size,
    SecretBytes master_password, Text &error) {
    error.clear();
    SecretBytes discarded_credentials;
    return decrypt_blob(bytes, size, master_password, discarded_credentials, error);
}

bool Vault::unlock(SecretBytes master_password, SecretBytes &credentials, Text &error) {
    error.clear();
    if (directory_fd_ < 0) return set_error(error, "vault is closed");
    unsigned char *blob = nullptr;
    size_t size = 0;
    if (!detail::read_private_file_at(directory_fd_, "credentials.vault", MaxFileBytes,
            &blob, size, error)) return false;
    SecretBytes next;
    const bool ok = decrypt_blob(blob, size, master_password, next, error);
    detail::wipe_memory(blob, size);
    free(blob);
    if (!ok) return false;
    credentials = static_cast<SecretBytes &&>(next);
    return true;
}

bool Vault::save(SecretBytes master_password, SecretBytes credentials, Text &error) {
    const VaultSaveResult result = save_with_status(static_cast<SecretBytes &&>(master_password),
        static_cast<SecretBytes &&>(credentials), error);
    return result == VaultSaveResult::Committed || result == VaultSaveResult::CommittedCleanupPending;
}

VaultSaveResult Vault::save_with_status(SecretBytes master_password, SecretBytes credentials, Text &error) {
    error.clear();
    if (directory_fd_ < 0) {
        set_error(error, "vault is closed");
        return VaultSaveResult::Failed;
    }
    Bytes blob;
    if (!build_blob(master_password, credentials, blob, error)) return VaultSaveResult::Failed;
    detail::IoFaultPoint fault = detail::IoFaultPoint::None;
#ifdef RELIEFMAIL_TESTING
    fault = consume_vault_fault();
#endif
    const detail::ReplaceOutcome outcome = detail::replace_private_file_at(directory_fd_, "credentials.vault",
        ".credentials.vault.rollback", ".credentials.vault.committed-old",
        blob.data(), blob.size(), fault, error);
    if (outcome == detail::ReplaceOutcome::RecoveryRequired) close();
    switch (outcome) {
    case detail::ReplaceOutcome::Failed: return VaultSaveResult::Failed;
    case detail::ReplaceOutcome::Committed: return VaultSaveResult::Committed;
    case detail::ReplaceOutcome::CommittedCleanupPending: return VaultSaveResult::CommittedCleanupPending;
    case detail::ReplaceOutcome::RecoveryRequired: return VaultSaveResult::RecoveryRequired;
    }
    set_error(error, "vault replacement outcome is invalid");
    return VaultSaveResult::RecoveryRequired;
}

bool Vault::change_password(SecretBytes old_password, SecretBytes new_password, Text &error) {
    const VaultPasswordChangeResult result = change_password_with_status(
        static_cast<SecretBytes &&>(old_password), static_cast<SecretBytes &&>(new_password), error);
    return result == VaultPasswordChangeResult::Committed ||
        result == VaultPasswordChangeResult::CommittedCleanupPending;
}

VaultPasswordChangeResult Vault::change_password_with_status(
    SecretBytes old_password, SecretBytes new_password, Text &error) {
    error.clear();
    SecretBytes credentials;
    if (!unlock(static_cast<SecretBytes &&>(old_password), credentials, error))
        return VaultPasswordChangeResult::Failed;
    const VaultSaveResult saved = save_with_status(static_cast<SecretBytes &&>(new_password),
        static_cast<SecretBytes &&>(credentials), error);
    switch (saved) {
    case VaultSaveResult::Failed: return VaultPasswordChangeResult::Failed;
    case VaultSaveResult::Committed: return VaultPasswordChangeResult::Committed;
    case VaultSaveResult::CommittedCleanupPending: return VaultPasswordChangeResult::CommittedCleanupPending;
    case VaultSaveResult::RecoveryRequired: return VaultPasswordChangeResult::RecoveryRequired;
    }
    set_error(error, "vault password-change outcome is invalid");
    return VaultPasswordChangeResult::RecoveryRequired;
}

#ifdef RELIEFMAIL_TESTING
void vault_test_fail_next(VaultFaultPoint point) { next_vault_fault = point; }

bool vault_test_pbkdf2(const void *password, size_t password_size,
    const void *salt, size_t salt_size, uint32_t iterations,
    SecretBytes &output, Text &error) {
    error.clear();
    if ((!password && password_size) || (!salt && salt_size) || password_size > UINT_MAX ||
        salt_size > UINT_MAX || !iterations) return set_error(error, "test KDF input is invalid");
    unsigned char derived[KeyBytes];
    const int code = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        static_cast<const unsigned char *>(password), static_cast<uint32_t>(password_size),
        static_cast<const unsigned char *>(salt), static_cast<uint32_t>(salt_size),
        iterations, KeyBytes, derived);
    if (code != 0) {
        mbedtls_platform_zeroize(derived, sizeof(derived));
        return set_error(error, "test KDF failed");
    }
    const bool copied = output.set(derived, sizeof(derived));
    mbedtls_platform_zeroize(derived, sizeof(derived));
    return copied || set_error(error, "out of memory");
}

bool vault_test_aes_gcm_encrypt(const unsigned char key[32], const unsigned char nonce[12],
    const void *aad, size_t aad_size, const void *plaintext, size_t plaintext_size,
    Bytes &ciphertext, unsigned char tag[16], Text &error) {
    error.clear();
    if (!key || !nonce || (!aad && aad_size) || (!plaintext && plaintext_size) || !tag)
        return set_error(error, "test AES-GCM input is invalid");
    unsigned char *output = plaintext_size ? static_cast<unsigned char *>(malloc(plaintext_size)) : nullptr;
    if (plaintext_size && !output) return set_error(error, "out of memory");
    const bool encrypted = encrypt_payload(key, nonce, static_cast<const unsigned char *>(aad), aad_size,
        static_cast<const unsigned char *>(plaintext), plaintext_size, output, tag);
    const bool copied = encrypted && ciphertext.set(output, plaintext_size);
    if (output) { mbedtls_platform_zeroize(output, plaintext_size); free(output); }
    if (!encrypted) return set_error(error, "test AES-GCM failed");
    return copied || set_error(error, "out of memory");
}
#endif

} // namespace rmail
