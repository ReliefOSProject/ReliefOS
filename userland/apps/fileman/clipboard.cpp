#include "fileman.h"
#include "../clipboard/selection.h"
#include <FL/Fl.H>
#include <errno.h>
#include <unistd.h>

namespace {
clip::Selection selection;
bool initialized, fetching, cut, handoff_done;
clip::Array<clip::Text> paths, remaining;
clip::Text original, destination;
enum class Phase { Gnome, Uri, Kde, Verify };
Phase phase;
Window source;

void status(const char *message)
{
    set_status(message);
    fileman_refresh_view();
}

bool publish(const clip::Array<clip::Text> &files, bool move)
{
    clip::Entry entry;
    if (!clip::file_payload(files, move, entry) || !selection.publish(entry)) {
        status(T("Could not copy files to the system clipboard"));
        return false;
    }
    return true;
}

void received(int, const clip::Received &, void *);
void execute();
void execute_timeout(void *) { execute(); }

bool request(const char *target, Phase next)
{
    phase = next;
    return selection.fetch(selection.atom(target), received, nullptr, source);
}

void execute()
{
    if (!initialized) return;
    if (!(destination == current_path)) {
        fetching = false;
        status(T("The folder changed or is busy. Paste again."));
        return;
    }
    if (!fileman_clipboard_ready()) { Fl::add_timeout(.05, execute_timeout); return; }
    fetching = false;
    if (paths.empty()) { status(T("Clipboard is empty")); return; }
    clip::Array<const char *> pointers;
    clip::Array<uint8_t> moved;
    if (paths.size() > FILEMAN_MAX_ENTRIES || !moved.resize(paths.size())) {
        status(T("Clipboard contains too many files")); return;
    }
    for (const auto &path : paths) {
        if (path.size() >= RELIEFOS_FS_PATH_LEN || !pointers.push(path.c_str())) {
            status(T("A clipboard path is too long")); return;
        }
    }
    fileman_paste_paths(pointers.data(), uint32_t(pointers.size()), cut, moved.data());
    fileman_clipboard_refresh();
    if (cut && !original.empty()) {
        remaining.clear();
        for (size_t i = 0; i < paths.size(); ++i) if (!moved[i] && !remaining.push(paths[i])) return;
        if (remaining.size() != paths.size() && selection.owner() == source)
            (void)request("x-special/gnome-copied-files", Phase::Verify);
    }
}

void received(int result, const clip::Received &value, void *)
{
    if (phase == Phase::Verify) {
        if (!result && value.format == 8 && value.bytes == original) publish(remaining, !remaining.empty());
        return;
    }
    if (phase == Phase::Gnome && result == -ENODATA) {
        if (request("text/uri-list", Phase::Uri)) return;
    } else if (phase == Phase::Kde) {
        cut = !result && value.format == 8 && value.bytes == "1";
        execute();
        return;
    } else if (!result && value.format == 8 && clip::parse_files(value.bytes, phase == Phase::Gnome, paths, cut)) {
        if (phase == Phase::Gnome) original = value.bytes;
        else if (request("application/x-kde-cutselection", Phase::Kde)) return;
        execute();
        return;
    }
    fetching = false;
    status(T("The clipboard does not contain usable local files"));
}

void events(const XEvent &event, void *)
{
    if (event.type == SelectionNotify && event.xselection.target == selection.atom("SAVE_TARGETS")) handoff_done = true;
}

void pump(void *)
{
    selection.pump();
    if (initialized) Fl::repeat_timeout(.1, pump);
}
}

extern "C" int fileman_clipboard_initialize()
{
    initialized = selection.open();
    if (!initialized) return -1;
    selection.events = events;
    Fl::add_fd(selection.fd(), [](int, void *) { selection.pump(); });
    Fl::add_timeout(.1, pump);
    return 0;
}

extern "C" void fileman_clipboard_shutdown()
{
    if (!initialized) return;
    if (selection.owns() && XGetSelectionOwner(selection.display(), selection.atom("CLIPBOARD_MANAGER"))) {
        handoff_done = false;
        XConvertSelection(selection.display(), selection.atom("CLIPBOARD_MANAGER"), selection.atom("SAVE_TARGETS"),
            selection.atom("_RELIEFOS_FILEMAN_SAVE"), selection.window(), CurrentTime);
        XFlush(selection.display());
        double deadline = clip::now() + 10;
        while (!handoff_done && clip::now() < deadline) { selection.pump(); usleep(1000); }
    }
    initialized = false;
    Fl::remove_timeout(execute_timeout);
    Fl::remove_timeout(pump);
    Fl::remove_fd(selection.fd());
}

extern "C" int fileman_clipboard_available()
{
    return initialized && !fetching && selection.owner() != None;
}

extern "C" void copy_selected_entries(uint8_t move)
{
    if (!initialized) { status(T("System clipboard is unavailable")); return; }
    clip::Array<clip::Text> files;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (!fileman_entry_marked(i) && (fileman_selected_count() || i != uint32_t(file_list.selected))) continue;
        if (fileman_entry_is_device(i)) continue;
        char path[RELIEFOS_FS_PATH_LEN];
        build_child_path(path, sizeof(path), entries[i].name);
        if (!files.push(ra::text(path))) { status(T("Could not copy files to the system clipboard")); return; }
    }
    if (files.empty()) { status(T("Select an item")); return; }
    selection.cancel();
    Fl::remove_timeout(execute_timeout);
    fetching = false;
    if (publish(files, move))
        status(move ? T("Items cut. Open a folder and paste.") : T("Items copied. Open a folder and paste."));
}

extern "C" void paste_clipboard()
{
    if (!initialized || fetching || fileman_operation_active) return;
    original.clear();
    destination.set(current_path);
    source = selection.owner();
    fetching = true;
    if (!request("x-special/gnome-copied-files", Phase::Gnome)) {
        fetching = false;
        status(T("Clipboard is empty"));
    }
}
