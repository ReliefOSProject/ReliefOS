#include "manager.h"
#include <X11/Xatom.h>
#include <errno.h>

namespace clip {
bool Manager::initialize()
{
    if (!selection.open()) { error_.set("Could not connect to the X11 display"); return false; }
    Display *display = selection.display();
    manager_ = selection.atom("CLIPBOARD_MANAGER");
    acquired_ = selection.server_time();
    XGrabServer(display);
    if (XGetSelectionOwner(display, manager_)) {
        XUngrabServer(display);
        error_.set("A clipboard manager is already running");
        return false;
    }
    XSetSelectionOwner(display, manager_, selection.window(), acquired_);
    bool claimed = XGetSelectionOwner(display, manager_) == selection.window();
    XUngrabServer(display);
    if (!claimed) { error_.set("Could not register the clipboard manager"); return false; }
    unsigned long version = 1;
    XChangeProperty(display, selection.window(), selection.atom("_RELIEFOS_CLIPBOARD_VERSION"),
        XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&version), 1);
    XEvent announcement{};
    announcement.xclient.type = ClientMessage;
    announcement.xclient.window = DefaultRootWindow(display);
    announcement.xclient.message_type = selection.atom("MANAGER");
    announcement.xclient.format = 32;
    announcement.xclient.data.l[0] = acquired_;
    announcement.xclient.data.l[1] = manager_;
    announcement.xclient.data.l[2] = selection.window();
    XSendEvent(display, DefaultRootWindow(display), False, StructureNotifyMask, &announcement);
    selection.events = event;
    selection.context = this;
    changed();
    return true;
}

void Manager::reply(const XSelectionRequestEvent &request, bool success)
{
    if (success && request.property && request.target == selection.atom("SAVE_TARGETS"))
        XChangeProperty(selection.display(), request.requestor, request.property, selection.atom("NULL"),
            8, PropModeReplace, nullptr, 0);
    XEvent response{};
    response.xselection.type = SelectionNotify;
    response.xselection.display = selection.display();
    response.xselection.requestor = request.requestor;
    response.xselection.selection = request.selection;
    response.xselection.target = request.target;
    response.xselection.time = request.time;
    response.xselection.property = success ? request.property : None;
    XSendEvent(selection.display(), request.requestor, False, NoEventMask, &response);
}

void Manager::request(const XSelectionRequestEvent &request)
{
    Atom property = request.property ? request.property : request.target;
    if (request.target == selection.atom("TARGETS")) {
        unsigned long targets[] = {selection.atom("TARGETS"), selection.atom("TIMESTAMP"), selection.atom("SAVE_TARGETS")};
        XChangeProperty(selection.display(), request.requestor, property, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<unsigned char *>(targets), 3);
        XSelectionRequestEvent fixed = request;
        fixed.property = property;
        reply(fixed, true);
    } else if (request.target == selection.atom("TIMESTAMP")) {
        unsigned long time = acquired_;
        XChangeProperty(selection.display(), request.requestor, property, XA_INTEGER, 32, PropModeReplace,
            reinterpret_cast<unsigned char *>(&time), 1);
        XSelectionRequestEvent fixed = request;
        fixed.property = property;
        reply(fixed, true);
    } else if (request.target == selection.atom("SAVE_TARGETS")) {
        if (selection.owns()) { reply(request, true); return; }
        if (!capturing) changed();
        if (!capturing || saves_.size() >= 16 || !selection.owner() || !saves_.push(request))
            reply(request, false);
    } else reply(request, false);
}

void Manager::event(const XEvent &event, void *context)
{
    auto *self = static_cast<Manager *>(context);
    if (event.type == SelectionRequest && event.xselectionrequest.selection == self->manager_ &&
        event.xselectionrequest.owner == self->selection.window()) self->request(event.xselectionrequest);
    else if (event.type == SelectionClear && event.xselectionclear.selection == self->manager_) self->stopped = true;
    else if (event.type == ClientMessage && event.xclient.window == self->selection.window() &&
        event.xclient.message_type == self->selection.atom("_RELIEFOS_CLIPBOARD_SHOW") && self->show)
        self->show(self->context);
    if (self->events) self->events(event, self->context);
}

void Manager::changed()
{
    Window source = selection.owner();
    if (source == selection.window()) { observed_ = source; return; }
    if (!source) {
        if (valid_ && !capturing && selection.publish(last_)) observed_ = selection.window();
        else observed_ = None;
        return;
    }
    if (source != source_) {
        for (const auto &request : saves_) reply(request, false);
        saves_.clear();
    }
    observed_ = source_ = source;
    valid_ = false;
    capturing = true;
    capturing_ = Entry{};
    targets_.clear();
    next_target_ = 0;
    timestamp_ = 0;
    checking_timestamp_ = false;
    if (!selection.fetch(selection.atom("TIMESTAMP"), timestamp, this, source)) finish();
}

void Manager::timestamp(int result, const Received &value, void *context)
{
    auto *self = static_cast<Manager *>(context);
    uint32_t time = !result && value.format == 32 && value.bytes.size() == 4 ? Selection::word(value.bytes) : 0;
    if (self->checking_timestamp_) {
        if (self->timestamp_ && self->timestamp_ != time) {
            self->capturing_ = Entry{};
            self->finish();
            self->changed();
        } else self->finish();
        return;
    }
    self->timestamp_ = time;
    if (!self->selection.fetch(self->selection.atom("TARGETS"), targets, self, self->source_)) self->finish();
}

void Manager::targets(int result, const Received &received, void *context)
{
    auto *self = static_cast<Manager *>(context);
    if (!result && received.type == XA_ATOM && received.format == 32 && received.bytes.size() % 4 == 0) {
        for (size_t i = 0; i < received.bytes.size() / 4 && self->targets_.size() < MaxFormats; ++i) {
            Atom target = Selection::word(received.bytes, i);
            char *name = XGetAtomName(self->selection.display(), target);
            if (!name) continue;
            const char *metadata[] = {"TARGETS", "TIMESTAMP", "MULTIPLE", "SAVE_TARGETS", "DELETE",
                "INSERT_PROPERTY", "INSERT_SELECTION", "PIXMAP", "BITMAP", "DRAWABLE", "COLORMAP",
                "TARGET_SIZES", "INCR", "_NET_MAX_SELECTION_SIZE"};
            bool safe = *name && strlen(name) <= 255;
            for (const char *skip : metadata) if (!strcmp(skip, name)) safe = false;
            XFree(name);
            bool duplicate = false;
            for (Atom old : self->targets_) if (old == target) duplicate = true;
            if (safe && !duplicate) self->targets_.push(target);
        }
    }
    self->next();
}

void Manager::next()
{
    if (selection.owner() != source_) { finish(); return; }
    if (next_target_ >= targets_.size()) {
        checking_timestamp_ = true;
        if (!selection.fetch(selection.atom("TIMESTAMP"), timestamp, this, source_)) finish();
        return;
    }
    Atom target = targets_[next_target_++];
    if (!selection.fetch(target, payload, this, source_)) finish();
}

void Manager::payload(int result, const Received &received, void *context)
{
    auto *self = static_cast<Manager *>(context);
    if (!result && received.format == 8 && received.type != XA_ATOM &&
        received.bytes.size() <= MaxEntryBytes - self->capturing_.size()) {
        char *target = XGetAtomName(self->selection.display(), self->targets_[self->next_target_ - 1]);
        char *type = XGetAtomName(self->selection.display(), received.type);
        if (target && type) self->capturing_.add(target, type, received.bytes.c_str(), received.bytes.size());
        if (target) XFree(target);
        if (type) XFree(type);
    }
    self->next();
}

void Manager::finish()
{
    capturing = false;
    bool current = selection.owner() == source_;
    bool saved = current && !capturing_.formats.empty() && last_.clone(capturing_);
    if (saved) {
        valid_ = true;
        if (!paused && !history.add(static_cast<Entry &&>(capturing_))) error_.set("Could not record clipboard history");
    }
    if (!saves_.empty()) {
        saved = saved && selection.publish(last_);
        for (const auto &request : saves_) reply(request, saved);
        saves_.clear();
        if (saved) observed_ = selection.window();
    }
}

void Manager::pump()
{
    selection.pump();
    Window current = selection.owner();
    if (current != observed_) changed();
}

bool Manager::use(size_t index)
{
    if (index >= history.entries.size() || !selection.publish(history.entries[index])) return false;
    selection.cancel();
    capturing = false;
    valid_ = false;
    for (const auto &request : saves_) reply(request, false);
    saves_.clear();
    observed_ = selection.window();
    return true;
}

void Manager::clear()
{
    selection.cancel();
    capturing = false;
    valid_ = false;
    last_ = Entry{};
    capturing_ = Entry{};
    for (const auto &request : saves_) reply(request, false);
    saves_.clear();
    history.clear();
    Entry empty;
    empty.add("UTF8_STRING", "UTF8_STRING", "", 0);
    if (selection.publish(empty)) observed_ = selection.window();
}
} // namespace clip
