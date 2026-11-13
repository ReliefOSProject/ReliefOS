#include "volume_ui.h"
#include "../fltk_support.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

namespace fm {
VolumePanel::VolumePanel(int x, int y, int w, int h, const VolumeCallbacks &callbacks)
    : Fl_Group(x, y, w, h), callbacks_(callbacks)
{
    heading_ = new Fl_Box(FL_THIN_UP_BOX, x, y, w, 24, T("Devices"));
    heading_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    heading_->labelfont(FL_HELVETICA_BOLD);
    list_ = new Fl_Hold_Browser(x, y + 24, w, h - 58);
    list_->format_char(0);
    list_->textsize(15);
    list_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY);
    list_->callback(selected, this);
    list_->add(T("Looking for devices..."));
    list_->tooltip(T("Click a removable volume to mount and open it."));
    open_ = new Fl_Button(x, y + h - 30, (w - 6) / 2, 30, T("Open"));
    open_->callback(open_clicked, this);
    eject_ = new Fl_Button(x + (w + 6) / 2, y + h - 30, (w - 6) / 2, 30, T("Eject"));
    eject_->callback(eject_clicked, this);
    eject_->tooltip(T("Unmount every volume on this device before unplugging it."));
    end();
    controls();
    Fl::add_timeout(.05, timer, this);
}

VolumePanel::~VolumePanel() { stop(); }
void VolumePanel::stop()
{
    stopped_ = true;
    Fl::remove_timeout(timer, this);
    if (job_ == Scan && process_.running()) process_.cancel();
}

void VolumePanel::resize(int x, int y, int w, int h)
{
    Fl_Group::resize(x, y, w, h);
    heading_->resize(x, y, w, 24);
    list_->resize(x, y + 24, w, h - 58);
    open_->resize(x, y + h - 30, (w - 6) / 2, 30);
    eject_->resize(x + (w + 6) / 2, y + h - 30, (w - 6) / 2, 30);
}

void VolumePanel::timer(void *context)
{
    auto *panel = static_cast<VolumePanel *>(context);
    panel->tick();
    if (!panel->stopped_) Fl::repeat_timeout(.1, timer, panel);
}
void VolumePanel::selected(Fl_Widget *, void *context)
{
    auto *panel = static_cast<VolumePanel *>(context);
    panel->controls();
    panel->request(Mount);
}
void VolumePanel::open_clicked(Fl_Widget *, void *context)
{
    static_cast<VolumePanel *>(context)->request(Mount);
}
void VolumePanel::eject_clicked(Fl_Widget *, void *context)
{
    static_cast<VolumePanel *>(context)->request(Eject);
}

void VolumePanel::controls()
{
    int row = list_->value();
    bool valid = row > 0 && static_cast<size_t>(row) <= volumes_.size() && !acting();
    if (valid) open_->activate(); else open_->deactivate();
    if (valid && !volumes_[row - 1].system) eject_->activate(); else eject_->deactivate();
    if (acting()) list_->deactivate(); else list_->activate();
    if (valid) {
        const Volume &volume = volumes_[row - 1];
        list_->copy_tooltip(volume.mounts ? volume.mount.c_str() : T("Not mounted"));
    }
}

void VolumePanel::request(Job action)
{
    if (acting() || !callbacks_.ready()) return;
    int row = list_->value();
    if (row <= 0 || static_cast<size_t>(row) > volumes_.size()) return;
    target_ = volumes_[row - 1];
    if (action == Mount && target_.mounts) {
        callbacks_.open(target_.mount.c_str());
        return;
    }
    if (target_.system) return;
    pending_ = action;
    controls();
    if (job_ == Idle) start_action();
}

void VolumePanel::start_action()
{
    Job action = pending_;
    pending_ = Idle;
    if (action == Eject) {
        for (const Volume &volume : volumes_)
            if (volume.disk == target_.disk && volume.mounts) callbacks_.leave(volume);
    }
    char sequence[32];
    snprintf(sequence, sizeof(sequence), "%llu", static_cast<unsigned long long>(target_.sequence));
    const char *locale = setlocale(LC_MESSAGES, nullptr);
    const char *args[] = {Program, "--volume-auth", action == Mount ? "mount" : "eject",
                          target_.name.c_str(), sequence, locale ? locale : "C", nullptr};
    if (!process_.start(args, false, ra::AuthMode::Sudo, false)) {
        callbacks_.status(T("Cannot start the device operation."));
        job_ = Idle;
    } else {
        job_ = action;
        callbacks_.status(action == Mount ? T("Mounting device...") : T("Preparing device for removal..."));
    }
    controls();
}

void VolumePanel::refresh()
{
    Array<Volume> fresh;
    if (!parse_volumes(process_.output(), process_.output_size(), fresh)) {
        callbacks_.status(T("Cannot read the device list."));
        return;
    }
    Volume selected;
    int row = list_->value();
    bool have_selection = row > 0 && static_cast<size_t>(row) <= volumes_.size();
    if (have_selection) selected = volumes_[row - 1];
    for (const Volume &old : volumes_) {
        if (!old.mounts) continue;
        bool still_mounted = false;
        for (const Volume &volume : fresh)
            if (same_volume(old, volume) && old.mount == volume.mount) still_mounted = true;
        if (!still_mounted) callbacks_.leave(old);
    }
    volumes_.swap(fresh);
    list_->clear();
    if (volumes_.empty()) list_->add(T("No removable devices"));
    for (size_t i = 0; i < volumes_.size(); ++i) {
        const Volume &volume = volumes_[i];
        char label[NAME_MAX + 64];
        double capacity = volume.bytes / (1024. * 1024.);
        const char *unit = "MiB";
        if (capacity >= 1024) { capacity /= 1024; unit = "GiB"; }
        snprintf(label, sizeof(label), "%s%s  %.1f %s", volume.mounts ? "* " : "",
                 volume.name.c_str(), capacity, unit);
        list_->add(label);
        if (have_selection && same_volume(selected, volume)) list_->value(i + 1);
    }
    controls();
}

void VolumePanel::tick()
{
    if (process_.running()) process_.step(65536);
    if (!callbacks_.ready() || Fl::modal() || Fl::grab()) return;
    if (job_ != Idle && !process_.running()) {
        bool ok = !process_.exit_code() && !process_.exec_error() && !process_.truncated();
        if (job_ == Scan) {
            if (ok) {
                available_ = true;
                refresh();
            } else if (available_) {
                available_ = false;
                callbacks_.status(T("Cannot read the device list."));
            }
        } else {
            Text path, users;
            int result = process_.truncated() ? -EOVERFLOW :
                parse_volume_reply(process_.output(), process_.output_size(), path, users);
            if (!ok && !result) result = -EIO;
            if (!result && job_ == Mount && path.empty()) result = -EPROTO;
            if (!result) {
                if (job_ == Mount) {
                    if (callbacks_.open(path.c_str())) callbacks_.status(T("Device mounted."));
                } else callbacks_.status(T("You can safely unplug the device."));
            } else {
                const char *message = result == -EBUSY ? T("Device is busy. Close files and terminals on it, then try again.") :
                    result == -ESTALE || result == -ENODEV || result == -ENOENT ? T("The device changed or is unavailable. Refresh and try again.") :
                    result == -EPROTO ? T("Authentication was cancelled or the device operation failed.") :
                    result == -EOPNOTSUPP ? T("This filesystem is not supported.") :
                    result == -EPERM || result == -EACCES ? T("Administrator permission is required for this device operation.") :
                    strerror(-result);
                callbacks_.status(message);
                if (!users.empty()) fl_alert("%s\n%s", message, users.c_str());
                else if (*process_.errors()) fl_alert("%s\n\n%s", message, process_.errors());
                else fl_alert("%s", message);
                fprintf(stderr, "[fileman] volume operation failed (%d)\n", -result);
                if (*process_.errors()) fputs(process_.errors(), stderr);
            }
            countdown_ = 0;
        }
        job_ = Idle;
        controls();
    }
    if (job_ != Idle) return;
    if (pending_ != Idle) { start_action(); return; }
    if (countdown_) { --countdown_; return; }
    const char *args[] = {Program, "--volume-list", nullptr};
    if (process_.start(args)) job_ = Scan;
    countdown_ = 20;
}
} // namespace fm
