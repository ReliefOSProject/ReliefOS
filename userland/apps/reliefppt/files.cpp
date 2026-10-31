#include "../fltk_support.h"
#include "canvas.h"
#include "render.h"
#include "window.h"
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rp {
namespace {
void remove_copy(Text &path, int &fd) {
    if (!path.empty() && fd >= 0) {
        unlink(path.c_str());
        char side[4096];
        int n = snprintf(side, sizeof(side), "%s.lock", path.c_str());
        if (n > 0 && n < int(sizeof(side)))
            unlink(side);
    }
    unlock_recovery_path(fd);
    path.clear();
}
bool clear_reservation(Text &path) {
    if (path.empty())
        return true;
    struct stat st{};
    if (lstat(path.c_str(), &st) < 0) {
        path.clear();
        return true;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid())
        return false;
    if (unlink(path.c_str()) < 0)
        return false;
    path.clear();
    return true;
}
bool same_identity(const FileStamp &a, const FileStamp &b) {
    return a.exists && b.exists && a.device == b.device && a.inode == b.inode;
}
struct ProgressDialog {
    Fl_Progress *bar;
    bool cancelled = false;
};
bool progress(void *data, unsigned current, unsigned total) {
    auto *p = static_cast<ProgressDialog *>(data);
    p->bar->value(total ? float(current) / total : 0);
    Fl::check();
    return !p->cancelled;
}
} // namespace
bool PptWindow::confirm_discard() {
    if (!commit_inputs())
        return false;
    if (!dirty())
        return true;
    int choice =
        fl_choice("%s", T("Cancel"), T("Save"), T("Discard"), T("Save changes to this presentation?"));
    if (choice == 0)
        return false;
    return choice == 2 || save(false);
}
bool PptWindow::choose_path(Text &out, bool saving, const char *extension) {
    const bool native = !strcmp(extension, "rppt"), image = !strcmp(extension, "image");
    const char *filter = native                      ? "*.{rppt,RPPT}"
                         : image                     ? "*.{png,PNG,bmp,BMP}"
                         : !strcmp(extension, "png") ? "*.{png,PNG}"
                                                     : "*.{html,HTML,htm,HTM}";
    char initial[4096];
    if (saving) {
        const char *name = native                      ? "presentation.rppt"
                           : !strcmp(extension, "png") ? "slide.png"
                                                       : "presentation.html";
        snprintf(initial, sizeof(initial), "%s", name);
        if (!path_.empty()) {
            const char *slash = strrchr(path_.c_str(), '/');
            if (slash && size_t(slash - path_.c_str()) + 1 + strlen(name) < sizeof(initial)) {
                size_t n = size_t(slash - path_.c_str()) + 1;
                memcpy(initial, path_.c_str(), n);
                strcpy(initial + n, name);
            }
        }
    } else
        snprintf(initial, sizeof(initial), "%s", path_.empty() ? "." : path_.c_str());
    Fl_File_Chooser chooser(initial, filter, saving ? Fl_File_Chooser::CREATE : Fl_File_Chooser::SINGLE,
                            saving  ? T("Save presentation or export")
                            : image ? T("Insert picture")
                                    : T("Open presentation"));
    chooser.preview(0);
    chooser.show();
    while (chooser.shown())
        Fl::wait();
    const char *value = chooser.value();
    if (!value || !*value)
        return false;
    if (strlen(value) >= 4000) {
        fl_alert("%s", T("File path is too long"));
        return false;
    }
    char result[4096];
    snprintf(result, sizeof(result), "%s", value);
    if (saving) {
        const char *dot = strrchr(result, '.'), *slash = strrchr(result, '/');
        if (!dot || (slash && dot < slash) || strcasecmp(dot + 1, extension)) {
            size_t n = strlen(result);
            snprintf(result + n, sizeof(result) - n, ".%s", extension);
        }
    }
    if (!out.set(result)) {
        fl_alert("%s", T("Not enough memory"));
        return false;
    }
    return true;
}
void PptWindow::discard_recovery() {
    if (recovery_fd_ >= 0)
        remove_copy(recovery_path_, recovery_fd_);
    else
        clear_reservation(recovery_path_);
    remove_copy(recovered_from_, recovered_fd_);
    recovery_revision_ = 0;
    recovery_epoch_ = 0;
}
bool PptWindow::save(bool as_new) {
    if (!commit_inputs())
        return false;
    Text target = path_, error;
    if ((as_new || target.empty()) && !choose_path(target, true, "rppt"))
        return false;
    FileStamp expected;
    if (target == path_ && !as_new)
        expected = stamp_;
    else {
        if (!file_stamp(target.c_str(), expected, error)) {
            fl_alert("%s", T(error.c_str()));
            return false;
        }
        if (expected.exists && fl_choice(T("Replace the existing file?\n%s"), T("Cancel"), T("Replace"),
                                         nullptr, target.c_str()) != 1)
            return false;
    }
    busy_ = true;
    cursor(FL_CURSOR_WAIT);
    message(T("Saving presentation..."));
    Fl::flush();
    bool ok = save_deck(target.c_str(), editor.deck, error, &expected);
    busy_ = false;
    cursor(FL_CURSOR_DEFAULT);
    if (!ok) {
        FileStamp current;
        Text check;
        if (file_stamp(target.c_str(), current, check) && !same_stamp(current, expected)) {
            int choice =
                fl_choice("%s", T("Cancel"), T("Save a copy..."), T("Replace"),
                          T("The file changed outside ReliefPPT. Save a copy or explicitly replace it."));
            if (choice == 1)
                return save(true);
            if (choice == 2) {
                busy_ = true;
                ok = save_deck(target.c_str(), editor.deck, error, &current);
                busy_ = false;
            }
        }
    }
    if (!ok) {
        fl_alert("%s", T(error.c_str()));
        message(T("Presentation was not saved"));
        return false;
    }
    path_ = target;
    if (!file_stamp(target.c_str(), stamp_, error)) {
        fl_alert("%s", T(error.c_str()));
        return false;
    }
    saved_revision_ = editor.deck.revision;
    discard_recovery();
    title();
    selected();
    message(T("Presentation saved"));
    canvas->take_focus();
    return true;
}
void PptWindow::open_file(const char *path) {
    Text candidate_path, error;
    if (!candidate_path.set(path)) {
        fl_alert("%s", T("Not enough memory"));
        return;
    }
    FileStamp before, after;
    if (!file_stamp(path, before, error)) {
        fl_alert("%s", T(error.c_str()));
        return;
    }
    Deck candidate;
    busy_ = true;
    cursor(FL_CURSOR_WAIT);
    message(T("Opening presentation..."));
    Fl::flush();
    bool ok = load_deck(path, candidate, error) && file_stamp(path, after, error);
    if (ok && !same_stamp(before, after)) {
        error.set("The file changed while it was being opened");
        ok = false;
    }
    busy_ = false;
    cursor(FL_CURSOR_DEFAULT);
    if (ok)
        ok = editor.replace_deck(static_cast<Deck &&>(candidate));
    if (!ok) {
        fl_alert("%s", T(error.empty() ? editor.error.c_str() : error.c_str()));
        message(T("Could not open the presentation"));
        return;
    }
    discard_recovery();
    path_ = static_cast<Text &&>(candidate_path);
    stamp_ = after;
    saved_revision_ = editor.deck.revision;
    notes_page_ = SIZE_MAX;
    pending_epoch_ = 0;
    changed();
    canvas->fit();
    message(T("Presentation opened"));
    canvas->take_focus();
}
void PptWindow::load_initial(const char *path) {
    if (path && *path && confirm_discard())
        open_file(path);
}
void PptWindow::export_file(bool html) {
    Text target, error;
    if (!choose_path(target, true, html ? "html" : "png"))
        return;
    FileStamp expected, current;
    if (!file_stamp(target.c_str(), expected, error)) {
        fl_alert("%s", T(error.c_str()));
        return;
    }
    if (!path_.empty() && (target == path_ || same_identity(expected, stamp_))) {
        fl_alert("%s", T("Choose an export file different from the editable presentation"));
        return;
    }
    if (expected.exists && fl_choice(T("Replace the existing file?\n%s"), T("Cancel"), T("Replace"), nullptr,
                                     target.c_str()) != 1)
        return;
    busy_ = true;
    cursor(FL_CURSOR_WAIT);
    message(T("Exporting presentation..."));
    Fl::flush();
    Text bytes;
    bool ok;
    if (html) {
        Fl_Window dialog(410, 140, T("Exporting presentation"));
        Fl_Progress bar(20, 25, 370, 26);
        bar.minimum(0);
        bar.maximum(1);
        Fl_Button cancel(288, 91, 102, 30, T("Cancel"));
        ProgressDialog state{&bar, false};
        cancel.callback([](Fl_Widget *, void *c) { static_cast<ProgressDialog *>(c)->cancelled = true; },
                        &state);
        dialog.callback([](Fl_Widget *, void *c) { static_cast<ProgressDialog *>(c)->cancelled = true; },
                        &state);
        dialog.end();
        reliefos_fltk::style(&dialog);
        dialog.set_modal();
        dialog.show();
        Fl::flush();
        ok = render_html(editor.deck, bytes, error, progress, &state);
        dialog.hide();
    } else
        ok = render_png(editor.deck, editor.current, bytes, error);
    if (ok) {
        if (!path_.empty() && file_stamp(path_.c_str(), current, error) && same_identity(expected, current)) {
            error.set("Choose an export file different from the editable presentation");
            ok = false;
        } else
            ok = atomic_write(target.c_str(), bytes.c_str(), bytes.size(), error, &expected);
    }
    busy_ = false;
    cursor(FL_CURSOR_DEFAULT);
    if (!ok)
        fl_alert("%s", T(error.c_str()));
    message(ok ? T("Export complete; the editable presentation remains open")
               : T("Export was not completed"));
    canvas->take_focus();
}
void PptWindow::recovery_tick(void *data) {
    auto *w = static_cast<PptWindow *>(data);
    if (!w->busy_ && w->shown() && w->dirty() &&
        (w->editor.deck.revision != w->recovery_revision_ || w->pending_epoch_ != w->recovery_epoch_)) {
        Text error;
        Deck candidate;
        bool ok = candidate.clone_from(w->editor.deck) && w->canvas->snapshot(candidate, error);
        if (ok && w->notes_page_ == w->editor.current)
            ok = candidate.slides[w->editor.current].notes.set(w->notes_->value());
        if (ok)
            ok = candidate.validate(error);
        if (ok && w->recovery_fd_ < 0)
            ok = clear_reservation(w->recovery_path_);
        if (ok && w->recovery_path_.empty()) {
            ok = reserve_recovery_path(w->recovery_path_, error);
            if (ok && !lock_recovery_path(w->recovery_path_.c_str(), w->recovery_fd_, error)) {
                clear_reservation(w->recovery_path_);
                ok = false;
            }
        }
        if (ok && w->recovery_fd_ >= 0) {
            w->busy_ = true;
            ok = save_deck(w->recovery_path_.c_str(), candidate, error);
            w->busy_ = false;
        } else
            ok = false;
        if (ok) {
            // A recovered source remains the durable generation until this
            // new snapshot has been atomically committed. If cleanup fails,
            // startup still selects the newer valid snapshot by timestamp.
            remove_copy(w->recovered_from_, w->recovered_fd_);
            w->recovery_revision_ = w->editor.deck.revision;
            w->recovery_epoch_ = w->pending_epoch_;
            w->message(T("Recovery copy saved"));
        } else
            w->message(T("Could not save the recovery copy"));
    }
    Fl::repeat_timeout(60, recovery_tick, w);
}
void PptWindow::offer_recovery() {
    Text error;
    Text candidate;
    int fd = -1;
    Deck recovered;
    if (!find_latest_recovery(candidate, fd, recovered, error) || candidate.empty())
        return;
    int choice =
        fl_choice("%s", T("Later"), T("Recover"), T("Discard"),
                  T("An unsaved ReliefPPT recovery copy is available. Recover it into a new presentation?"));
    if (choice == 0) {
        unlock_recovery_path(fd);
        return;
    }
    if (choice == 2) {
        remove_copy(candidate, fd);
        return;
    }
    if (!editor.replace_deck(static_cast<Deck &&>(recovered))) {
        unlock_recovery_path(fd);
        fl_alert("%s", T(error.empty() ? editor.error.c_str() : error.c_str()));
        return;
    }
    recovered_from_ = candidate;
    recovered_fd_ = fd;
    path_.clear();
    stamp_ = {};
    saved_revision_ = 0;
    notes_page_ = SIZE_MAX;
    pending_epoch_ = 0;
    changed();
    canvas->fit();
    message(T("Recovered — save this presentation to a new file"));
}
} // namespace rp
