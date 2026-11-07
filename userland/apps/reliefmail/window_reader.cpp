#include "window.h"
#include "ui_adapters.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rmail {
namespace {

bool set_error(Text &error, const char *message) {
    error.set(message ? message : T("The operation failed."));
    return false;
}

bool suggested_attachment_name(const WorkerReaderResult &message, uint32_t index,
    Text &name) {
    if (index >= message.attachments.size()) return false;
    const Text &source = message.attachments[index].filename;
    char safe[256];
    size_t written = 0;
    for (size_t i = 0; i < source.size() && written < sizeof(safe) - 1; ++i) {
        const unsigned char c = static_cast<unsigned char>(source.c_str()[i]);
        if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7f) safe[written++] = '_';
        else safe[written++] = static_cast<char>(c);
    }
    while (written && (safe[written - 1] == '.' || safe[written - 1] == ' ')) --written;
    size_t first = 0;
    while (first < written && (safe[first] == '.' || safe[first] == ' ')) ++first;
    if (first) {
        memmove(safe, safe + first, written - first);
        written -= first;
    }
    if (!written || (written == 1 && safe[0] == '.') ||
        (written == 2 && safe[0] == '.' && safe[1] == '.')) {
        snprintf(safe, sizeof(safe), "attachment-%u.bin", static_cast<unsigned>(index + 1));
        written = strlen(safe);
    }
    safe[written] = 0;
    return name.set(safe, written);
}

bool private_replaceable_file(const struct stat &info) {
    return S_ISREG(info.st_mode) && info.st_uid == getuid() && info.st_nlink == 1;
}

}

void MailWindow::reader_fetch_cb(void *context) {
    static_cast<MailWindow *>(context)->fetch_selected_message();
}

void MailWindow::reader_preview_cb(void *context) {
    static_cast<MailWindow *>(context)->preview_original();
}

void MailWindow::reader_export_cb(void *context) {
    static_cast<MailWindow *>(context)->export_original(UINT32_MAX);
}

void MailWindow::reader_attachment_cb(void *context, uint32_t index) {
    static_cast<MailWindow *>(context)->export_original(index);
}

void MailWindow::reader_link_cb(void *context, const char *url, size_t length) {
    static_cast<MailWindow *>(context)->open_safe_link(url, length);
}

void MailWindow::fetch_selected_message() {
    if (!selected_row_valid_) {
        set_status(T("Select a message before fetching its body."));
        return;
    }
    if (!selected_row_.message.headers_only) {
        set_status(T("This message body is already cached."));
        return;
    }
    const Account *account = nullptr;
    for (size_t i = 0; i < startup_.accounts.size(); ++i)
        if (startup_.accounts[i].id == selected_row_.message.key.folder.account_id) account = &startup_.accounts[i];
    if (!account || account->generation != selected_row_.account_generation) {
        set_status(T("The message account changed. Refresh the folder before fetching."));
        return;
    }
    SecretBytes packet, check;
    Text error;
    if (!make_live_credentials(packet, error) ||
        !worker_find_credential(packet, *account, CredentialPurpose::Imap, account->username,
            coordinator_.profile_epoch(), check, error)) {
        set_status(T("Unlock the password vault or enter this account's incoming password before fetching."));
        return;
    }
    WorkerTaskInput input;
    input.message_key = selected_row_.message.key;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::FetchMessage;
    pending.row = selected_row_;
    pending.selection_generation = selection_generation_;
    if (!submit_row_task(WorkerTaskKind::FetchMessage, selected_row_, input,
            static_cast<SecretBytes &&>(packet), PendingPurpose::FetchMessage,
            selection_generation_, error, static_cast<PendingUiRequest &&>(pending)))
        set_status(error.empty() ? T("Could not queue message fetch") : error.c_str());
}

void MailWindow::preview_original() {
    if (!selected_row_valid_) {
        set_status(T("Select a message before viewing its original source."));
        return;
    }
    WorkerTaskInput input;
    input.message_key = selected_row_.message.key;
    input.chunk_offset = 0;
    input.chunk_size = 64u * 1024u;
    input.attachment_index = UINT32_MAX;
    Text error;
    if (!submit_row_task(WorkerTaskKind::ReadMessageChunk, selected_row_, input,
            SecretBytes(), PendingPurpose::SourcePreview, selection_generation_, error,
            PendingUiRequest()))
        set_status(error.empty() ? T("Could not load the bounded original preview") : error.c_str());
}

void MailWindow::export_original(uint32_t attachment_index) {
    if (!selected_row_valid_ || !reader_pane_->has_message()) {
        set_status(T("Select a loaded message before saving its content."));
        return;
    }
    if (attachment_index != UINT32_MAX && selected_row_.message.headers_only) {
        set_status(T("Attachments are not available until the full message is fetched."));
        return;
    }
    Text suggested;
    char fallback[64];
    if (attachment_index != UINT32_MAX) {
        if (!suggested_attachment_name(reader_pane_->message(), attachment_index, suggested)) {
            set_status(T("The selected attachment is no longer available."));
            return;
        }
    } else {
        snprintf(fallback, sizeof(fallback), "message-%u.%s", selected_row_.message.key.uid,
            selected_row_.message.headers_only ? "headers.txt" : "eml");
        if (!suggested.set(fallback)) {
            set_status(T("Not enough memory to choose an export name."));
            return;
        }
    }
    char *selected_path = fl_file_chooser(attachment_index == UINT32_MAX ?
        T("Save message data") : T("Save attachment"), "*", suggested.c_str(), 1);
    if (!selected_path || !*selected_path) return;

    Text path;
    if (!path.set(selected_path) || path.size() >= PATH_MAX) {
        set_status(T("The selected file path is too long."));
        return;
    }
    struct stat existing;
    bool replace_existing = false;
    if (lstat(path.c_str(), &existing) == 0) {
        if (!private_replaceable_file(existing)) {
            set_status(T("Choose a regular file owned by this user; links and shared files cannot be replaced."));
            return;
        }
        if (!literal_confirmation(T("Confirm action"), T("Replace the existing file?"), path.c_str(),
                T("Cancel"), T("Replace"))) return;
        replace_existing = true;
    } else if (errno != ENOENT) {
        set_status(T("Could not inspect the selected file safely."));
        return;
    }

    const char *slash = strrchr(path.c_str(), '/');
    char pattern[PATH_MAX];
    if (slash) {
        const size_t directory_length = slash == path.c_str() ? 1 : static_cast<size_t>(slash - path.c_str());
        char directory[PATH_MAX];
        memcpy(directory, path.c_str(), directory_length);
        directory[directory_length] = 0;
        if (snprintf(pattern, sizeof(pattern), "%s/.reliefmail-export-XXXXXX", directory) >=
            static_cast<int>(sizeof(pattern))) {
            set_status(T("The selected file path is too long.")); return;
        }
    } else if (snprintf(pattern, sizeof(pattern), "./.reliefmail-export-XXXXXX") >=
        static_cast<int>(sizeof(pattern))) {
        set_status(T("The selected file path is too long.")); return;
    }
    const int fd = mkstemp(pattern);
    if (fd < 0) {
        set_status(T("Could not create a private temporary export beside the selected file."));
        return;
    }
    ExportState next;
    next.fd = fd;
    next.path = static_cast<Text &&>(path);
    if (!next.temp_path.set(pattern)) {
        close(fd); unlink(pattern);
        set_status(T("Not enough memory to prepare this export.")); return;
    }
    next.row = selected_row_;
    next.attachment_index = attachment_index;
    next.active = true;
    next.replace_existing = replace_existing;
    if (replace_existing) {
        next.target_device = existing.st_dev;
        next.target_inode = existing.st_ino;
    }
    abort_export(T("The previous export was replaced by a new one."));
    export_ = static_cast<ExportState &&>(next);

    WorkerTaskInput input;
    input.message_key = export_.row.message.key;
    input.chunk_size = 256u * 1024u;
    input.attachment_index = attachment_index;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::ExportChunk;
    pending.row = export_.row;
    Text error;
    if (!submit_row_task(WorkerTaskKind::ReadMessageChunk, export_.row, input,
            SecretBytes(), PendingPurpose::ExportChunk, selection_generation_, error,
            static_cast<PendingUiRequest &&>(pending))) {
        abort_export(error.empty() ? T("Could not start the selected export") : error.c_str());
        return;
    }
    set_status(attachment_index == UINT32_MAX ? T("Saving message data in bounded background chunks...") :
        T("Saving the selected attachment in bounded background chunks..."));
}

void MailWindow::open_safe_link(const char *url, size_t length) {
    if (!task6_is_safe_web_link(url, length)) {
        set_status(T("Only bounded HTTP or HTTPS links can be opened."));
        return;
    }
    Text exact;
    if (!exact.set(url, length)) {
        set_status(T("Could not display this link safely."));
        return;
    }
    if (!literal_confirmation(T("Open link"), T("Open this exact link in the system browser?"),
            exact.c_str(), T("Cancel"), T("Open link"))) return;
    char error[256] = {0};
    if (!task6_uri_handoff_succeeded(fl_open_uri(exact.c_str(), error, sizeof(error)))) {
        set_status(error[0] ? error : T("The system browser could not open this link."));
        return;
    }
    set_status(T("The confirmed link was handed to the system browser."));
}

bool MailWindow::write_export_chunk(const WorkerChunkResult &chunk, Text &error) {
    if (!export_.active || export_.fd < 0 || chunk.key != export_.row.message.key ||
        chunk.attachment_index != export_.attachment_index || chunk.offset != export_.offset ||
        chunk.total_size > MaxRawMessageBytes || chunk.bytes.size() > 256u * 1024u ||
        chunk.offset > chunk.total_size || chunk.bytes.size() > chunk.total_size - chunk.offset ||
        (export_.offset && export_.total_size != chunk.total_size) ||
        (!chunk.bytes.size() && chunk.offset < chunk.total_size))
        return set_error(error, T("The worker returned an inconsistent export chunk."));
    if (!export_.offset) export_.total_size = chunk.total_size;
    size_t written = 0;
    while (written < chunk.bytes.size()) {
        const ssize_t amount = write(export_.fd, chunk.bytes.data() + written, chunk.bytes.size() - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return set_error(error, T("Could not write the temporary export file."));
        written += static_cast<size_t>(amount);
    }
    error.clear();
    return true;
}

void MailWindow::finish_export() {
    if (!export_.active) return;
    if (close(export_.fd) != 0) {
        export_.fd = -1;
        abort_export(T("Could not close the temporary export file."));
        return;
    }
    export_.fd = -1;
    bool committed = false;
    if (export_.replace_existing) {
        struct stat current;
        if (lstat(export_.path.c_str(), &current) == 0 && private_replaceable_file(current) &&
            current.st_dev == export_.target_device && current.st_ino == export_.target_inode)
            committed = rename(export_.temp_path.c_str(), export_.path.c_str()) == 0;
    } else {
        committed = link(export_.temp_path.c_str(), export_.path.c_str()) == 0;
        if (committed) (void)unlink(export_.temp_path.c_str());
    }
    const bool attachment = export_.attachment_index != UINT32_MAX;
    if (!committed) {
        abort_export(T("The destination changed or could not be installed safely; the original file was kept."));
        return;
    }
    export_.active = false;
    export_.temp_path.clear();
    export_.path.clear();
    export_.offset = 0;
    export_.total_size = 0;
    set_status(attachment ? T("Attachment saved to the selected path.") : T("Message data saved to the selected path."));
}

void MailWindow::abort_export(const char *message) {
    if (!export_.active && export_.fd < 0) return;
    if (export_.fd >= 0) close(export_.fd);
    export_.fd = -1;
    if (!export_.temp_path.empty()) (void)unlink(export_.temp_path.c_str());
    export_.active = false;
    export_.temp_path.clear();
    export_.path.clear();
    export_.offset = 0;
    export_.total_size = 0;
    if (message && *message) set_status(message);
}

}
