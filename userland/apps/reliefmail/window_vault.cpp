#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/fl_ask.H>

namespace rmail {

void MailWindow::handle_vault_status(bool exists) {
    vault_exists_ = exists;
    if (exists) set_status(T("Encrypted password vault is locked or available from Passwords..."));
    else set_status(T("No encrypted vault is saved; account credentials remain session-only."));
}

void MailWindow::start_vault_flow() {
    if (!coordinator_open_ || closing_) return;
    if (!vault_exists_) {
        if (credentials_.empty()) {
            set_status(T("Enter account passwords first, then save them to an encrypted vault if desired."));
            return;
        }
        MasterPasswordDialog dialog(T("Create encrypted password vault"), true);
        SecretBytes master, confirmation;
        if (!dialog.run(master, confirmation)) return;
        save_vault(static_cast<SecretBytes &&>(master));
        return;
    }

    const bool unlocked = !credentials_.empty();
    const int choice = unlocked ?
        fl_choice(T("Encrypted password vault"), T("Save/update"), T("Change password"), T("Unlock again")) :
        fl_choice(T("Encrypted password vault"), T("Unlock"), T("Change password"), T("Cancel"));
    if (choice < 0 || (!unlocked && choice == 2)) return;
    if (unlocked && choice == 0) {
        MasterPasswordDialog dialog(T("Save encrypted credentials"), true);
        SecretBytes master, confirmation;
        if (dialog.run(master, confirmation)) save_vault(static_cast<SecretBytes &&>(master));
        return;
    }
    if (unlocked && choice == 1) { change_vault_password(); return; }
    if (unlocked && choice == 2) { prompt_unlock(); return; }
    if (!unlocked && choice == 1) { change_vault_password(); return; }
    prompt_unlock();
}

void MailWindow::prompt_unlock(bool remember_after_unlock, SecretBytes master) {
    if (!vault_exists_) {
        set_status(T("No encrypted password vault is available to unlock."));
        return;
    }
    SecretBytes confirmation;
    if (master.empty()) {
        MasterPasswordDialog dialog(T("Unlock encrypted password vault"), false);
        if (!dialog.run(master, confirmation)) return;
    }
    SecretBytes retained_master;
    if (remember_after_unlock && !retained_master.set(master.data(), master.size())) {
        set_status(T("Not enough memory to retain the vault password for this operation."));
        return;
    }
    SecretBytes secret;
    Text error;
    if (!worker_encode_secret(WorkerSecretPurpose::VaultMaster,
            static_cast<SecretBytes &&>(master), SecretBytes(), secret, error)) {
        set_status(error.c_str());
        return;
    }
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::UnlockVault;
    pending.remember_after_unlock = remember_after_unlock;
    if (remember_after_unlock) pending.master_password = static_cast<SecretBytes &&>(retained_master);
    if (!submit_task(WorkerTaskKind::UnlockVault, request, nullptr,
            static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.empty() ? T("Could not queue vault unlock") : error.c_str());
}

void MailWindow::save_vault(SecretBytes master) {
    SecretBytes live_credentials, secret;
    Text error;
    if (credentials_.empty() || !make_live_credentials(live_credentials, error)) {
        set_status(error.empty() ? T("Unlock or enter at least one account password before saving the vault.") : error.c_str());
        return;
    }
    if (!worker_encode_secret(WorkerSecretPurpose::VaultSave,
            static_cast<SecretBytes &&>(master), static_cast<SecretBytes &&>(live_credentials), secret, error)) {
        set_status(error.c_str());
        return;
    }
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::SaveVault;
    if (!submit_task(WorkerTaskKind::SaveVault, request, nullptr,
            static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.empty() ? T("Could not queue encrypted credential storage") : error.c_str());
}

void MailWindow::change_vault_password() {
    if (!vault_exists_) {
        set_status(T("Create an encrypted vault before changing its password."));
        return;
    }
    MasterPasswordDialog old_dialog(T("Verify current vault password"), false);
    SecretBytes old_password, ignored;
    if (!old_dialog.run(old_password, ignored)) return;
    MasterPasswordDialog new_dialog(T("Choose a new vault password"), true);
    SecretBytes new_password, confirmation;
    if (!new_dialog.run(new_password, confirmation)) return;
    SecretBytes secret;
    Text error;
    if (!worker_encode_secret(WorkerSecretPurpose::VaultPasswordChange,
            static_cast<SecretBytes &&>(old_password), static_cast<SecretBytes &&>(new_password), secret, error)) {
        set_status(error.c_str());
        return;
    }
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::ChangeVaultPassword;
    if (!submit_task(WorkerTaskKind::ChangeVaultPassword, request, nullptr,
            static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.empty() ? T("Could not queue vault password change") : error.c_str());
}

}
