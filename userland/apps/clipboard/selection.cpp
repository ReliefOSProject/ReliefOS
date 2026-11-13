#include "selection.h"
#include <X11/Xatom.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

namespace clip {
namespace {
Display *protected_displays[8];
size_t display_count;
XErrorHandler original_error;
int xerror(Display *display, XErrorEvent *error)
{
    for (size_t i = 0; i < display_count; ++i) if (display == protected_displays[i]) return 0;
    return original_error ? original_error(display, error) : 0;
}
Bool clock_event(Display *, XEvent *event, XPointer context)
{
    auto *match = reinterpret_cast<XPropertyEvent *>(context);
    return event->type == PropertyNotify && event->xproperty.window == match->window &&
        event->xproperty.atom == match->atom;
}
constexpr size_t Chunk = 32768;
}

double now()
{
    struct timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return double(time.tv_sec) + double(time.tv_nsec) / 1000000000.;
}

Selection::~Selection()
{
    if (!display_) return;
    cancel();
    XDestroyWindow(display_, window_);
    XCloseDisplay(display_);
    for (size_t i = 0; i < display_count; ++i) {
        if (protected_displays[i] != display_) continue;
        protected_displays[i] = protected_displays[--display_count];
        break;
    }
    if (!display_count) XSetErrorHandler(original_error);
}

bool Selection::open()
{
    display_ = XOpenDisplay(nullptr);
    if (!display_) return false;
    if (display_count == 8) { XCloseDisplay(display_); display_ = nullptr; return false; }
    if (!display_count) original_error = XSetErrorHandler(xerror);
    protected_displays[display_count++] = display_;
    window_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display_, window_, PropertyChangeMask | StructureNotifyMask);
    clipboard_ = atom("CLIPBOARD");
    property_ = atom("_RELIEFOS_CLIPBOARD_DATA");
    incr_ = atom("INCR");
    XFlush(display_);
    return window_ != None;
}

Atom Selection::atom(const char *name) const { return XInternAtom(display_, name, False); }
int Selection::fd() const { return ConnectionNumber(display_); }
Window Selection::owner() const { return XGetSelectionOwner(display_, clipboard_); }
bool Selection::owns() const { return owner() == window_; }

Time Selection::server_time()
{
    Atom clock = atom("_RELIEFOS_CLIPBOARD_CLOCK");
    XChangeProperty(display_, window_, clock, XA_INTEGER, 8, PropModeReplace, nullptr, 0);
    XPropertyEvent match{};
    match.window = window_;
    match.atom = clock;
    XEvent event{};
    XIfEvent(display_, &event, clock_event, reinterpret_cast<XPointer>(&match));
    return event.xproperty.time;
}

bool Selection::publish(const Entry &entry)
{
    Entry next;
    if (!next.clone(entry)) return false;
    Time time = server_time();
    XSetSelectionOwner(display_, clipboard_, window_, time);
    if (!owns()) return false;
    acquired_ = time;
    owned_ = static_cast<Entry &&>(next);
    XFlush(display_);
    return true;
}

bool Selection::fetch(Atom target, Completion done, void *data, Window expected)
{
    cancel();
    Window source = owner();
    if (!source || (expected && expected != source)) return false;
    incoming_.window = XCreateSimpleWindow(display_, DefaultRootWindow(display_), 0, 0, 1, 1, 0, 0, 0);
    incoming_.owner = source;
    incoming_.target = target;
    incoming_.complete = done;
    incoming_.data = data;
    incoming_.deadline = now() + 10;
    XSelectInput(display_, incoming_.window, PropertyChangeMask | StructureNotifyMask);
    XConvertSelection(display_, clipboard_, target, property_, incoming_.window, CurrentTime);
    XFlush(display_);
    return true;
}

void Selection::cancel()
{
    if (incoming_.window) XDestroyWindow(display_, incoming_.window);
    incoming_ = Incoming{};
}

uint32_t Selection::word(const Text &bytes, size_t index)
{
    if (index >= bytes.size() / 4) return 0;
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.c_str()) + index * 4;
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

int Selection::read_property(Window window, Atom property, Received &out)
{
    unsigned long count = 0, remaining = 0;
    unsigned char *raw = nullptr;
    Atom type;
    int format;
    int status = XGetWindowProperty(display_, window, property, 0, MaxEntryBytes / 4,
        False, AnyPropertyType, &type, &format, &count, &remaining, &raw);
    if (status != Success || !type || (format != 8 && format != 16 && format != 32)) {
        if (raw) XFree(raw);
        return -EPROTO;
    }
    if (remaining || count > MaxEntryBytes / (unsigned(format) / 8)) {
        XFree(raw);
        return -EFBIG;
    }
    size_t size = count * size_t(format / 8);
    bool ok;
    if (format == 32) {
        Array<unsigned char> encoded;
        ok = encoded.resize(size);
        if (ok) {
            auto *values = reinterpret_cast<unsigned long *>(raw);
            for (size_t i = 0; i < count; ++i)
                for (size_t j = 0; j < 4; ++j) encoded[i * 4 + j] = values[i] >> (j * 8);
            ok = out.bytes.set(reinterpret_cast<const char *>(encoded.data()), size);
        }
    } else ok = out.bytes.set(reinterpret_cast<const char *>(raw), size);
    XFree(raw);
    if (!ok) return -ENOMEM;
    out.type = type;
    out.format = format;
    return 0;
}

void Selection::complete(int result)
{
    Completion done = incoming_.complete;
    void *data = incoming_.data;
    Received received;
    received.type = incoming_.type;
    received.format = incoming_.format;
    if (!result && !received.bytes.set(reinterpret_cast<const char *>(incoming_.bytes.data()), incoming_.bytes.size()))
        result = -ENOMEM;
    cancel();
    if (done) done(result, received, data);
}

bool Selection::append(const Received &received)
{
    if (incoming_.type && (incoming_.type != received.type || incoming_.format != received.format)) return false;
    size_t offset = incoming_.bytes.size();
    if (received.bytes.size() > MaxEntryBytes - offset || !incoming_.bytes.resize(offset + received.bytes.size()))
        return false;
    if (received.bytes.size()) memcpy(incoming_.bytes.data() + offset, received.bytes.c_str(), received.bytes.size());
    incoming_.type = received.type;
    incoming_.format = received.format;
    incoming_.deadline = now() + 10;
    return true;
}

void Selection::notify(const XSelectionEvent &event)
{
    if (event.requestor != incoming_.window || event.selection != clipboard_ || event.target != incoming_.target)
        return;
    if (!event.property) { complete(-ENODATA); return; }
    Received value;
    int result = read_property(incoming_.window, property_, value);
    if (result) { complete(result); return; }
    if (value.type == incr_) {
        if (value.format != 32 || value.bytes.size() != 4 || word(value.bytes) > MaxEntryBytes) {
            complete(-EFBIG);
            return;
        }
        incoming_.incremental = true;
        XDeleteProperty(display_, incoming_.window, property_);
        XFlush(display_);
        return;
    }
    if (!append(value)) { complete(-EFBIG); return; }
    complete(0);
}

bool Selection::convert(Window requestor, Atom target, Atom property)
{
    if (target == atom("TARGETS")) {
        Array<unsigned long> targets;
        if (!targets.push(atom("TARGETS")) || !targets.push(atom("TIMESTAMP")) || !targets.push(atom("MULTIPLE")))
            return false;
        for (const Payload &p : owned_.formats) if (!targets.push(atom(p.target.c_str()))) return false;
        XChangeProperty(display_, requestor, property, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<const unsigned char *>(targets.data()), int(targets.size()));
        return true;
    }
    if (target == atom("TIMESTAMP")) {
        unsigned long time = acquired_;
        XChangeProperty(display_, requestor, property, XA_INTEGER, 32, PropModeReplace,
            reinterpret_cast<const unsigned char *>(&time), 1);
        return true;
    }
    const Payload *source = nullptr;
    for (const Payload &p : owned_.formats) if (atom(p.target.c_str()) == target) { source = &p; break; }
    if (!source) return false;
    Atom type = atom(source->type.c_str());
    if (source->bytes.size() <= Chunk) {
        XChangeProperty(display_, requestor, property, type, 8, PropModeReplace,
            reinterpret_cast<const unsigned char *>(source->bytes.c_str()), int(source->bytes.size()));
        return true;
    }
    for (size_t i = 0; i < outgoing_.size(); ++i)
        if (outgoing_[i].window == requestor && outgoing_[i].property == property) { outgoing_.erase(i); break; }
    if (outgoing_.size() >= 32) return false;
    Outgoing transfer;
    transfer.window = requestor;
    transfer.property = property;
    transfer.type = type;
    transfer.bytes = source->bytes;
    transfer.deadline = now() + 10;
    if (!outgoing_.push(static_cast<Outgoing &&>(transfer))) return false;
    XSelectInput(display_, requestor, PropertyChangeMask | StructureNotifyMask);
    unsigned long size = source->bytes.size();
    XChangeProperty(display_, requestor, property, incr_, 32, PropModeReplace,
        reinterpret_cast<const unsigned char *>(&size), 1);
    return true;
}

void Selection::request(const XSelectionRequestEvent &request)
{
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = display_;
    reply.xselection.requestor = request.requestor;
    reply.xselection.selection = request.selection;
    reply.xselection.target = request.target;
    reply.xselection.time = request.time;
    Atom property = request.property ? request.property : request.target;
    bool ok = false;
    if (request.target == atom("MULTIPLE") && request.property) {
        Received pairs;
        if (!read_property(request.requestor, property, pairs) && pairs.format == 32 &&
            pairs.type == atom("ATOM_PAIR") && pairs.bytes.size() % 8 == 0 && pairs.bytes.size() <= 8192) {
            Array<unsigned long> values;
            if (values.resize(pairs.bytes.size() / 4)) {
                for (size_t i = 0; i < values.size(); ++i) values[i] = word(pairs.bytes, i);
                for (size_t i = 0; i < values.size(); i += 2) {
                    if (!values[i + 1] || values[i + 1] == property || values[i] == request.target ||
                        !convert(request.requestor, values[i], values[i + 1])) values[i + 1] = None;
                }
                XChangeProperty(display_, request.requestor, property, atom("ATOM_PAIR"), 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(values.data()), int(values.size()));
                ok = true;
            }
        }
    } else if (request.target != atom("MULTIPLE")) ok = convert(request.requestor, request.target, property);
    reply.xselection.property = ok ? property : None;
    XSendEvent(display_, request.requestor, False, NoEventMask, &reply);
}

void Selection::property(const XPropertyEvent &event)
{
    if (event.window == incoming_.window && event.atom == property_ &&
        event.state == PropertyNewValue && incoming_.incremental) {
        Received value;
        int result = read_property(event.window, property_, value);
        if (result) { complete(result); return; }
        bool final = value.bytes.empty();
        if (!append(value)) { complete(-EFBIG); return; }
        XDeleteProperty(display_, event.window, property_);
        XFlush(display_);
        if (final) complete(0);
        return;
    }
    if (event.state != PropertyDelete) return;
    for (size_t i = 0; i < outgoing_.size(); ++i) {
        Outgoing &out = outgoing_[i];
        if (event.window != out.window || event.atom != out.property) continue;
        size_t size = out.bytes.size() - out.offset;
        if (size > Chunk) size = Chunk;
        XChangeProperty(display_, out.window, out.property, out.type, 8, PropModeReplace,
            reinterpret_cast<const unsigned char *>(out.bytes.c_str() + out.offset), int(size));
        out.offset += size;
        out.deadline = now() + 10;
        if (!size) outgoing_.erase(i);
        break;
    }
}

void Selection::pump()
{
    unsigned handled = 0;
    while (handled++ < 256 && XPending(display_)) {
        XEvent event{};
        XNextEvent(display_, &event);
        if (event.type == SelectionRequest && event.xselectionrequest.selection == clipboard_ &&
            event.xselectionrequest.owner == window_) request(event.xselectionrequest);
        else if (event.type == SelectionNotify && event.xselection.requestor == incoming_.window)
            notify(event.xselection);
        else if (event.type == PropertyNotify) property(event.xproperty);
        else if (event.type == DestroyNotify) {
            for (size_t i = outgoing_.size(); i > 0; --i)
                if (outgoing_[i - 1].window == event.xdestroywindow.window) outgoing_.erase(i - 1);
        }
        if (events) events(event, context);
    }
    if (receiving() && now() > incoming_.deadline) complete(-ETIMEDOUT);
    for (size_t i = outgoing_.size(); i > 0; --i)
        if (now() > outgoing_[i - 1].deadline) outgoing_.erase(i - 1);
    XFlush(display_);
}
} // namespace clip
