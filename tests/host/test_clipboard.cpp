#include "clipboard/history.h"
#include "clipboard/selection.h"
#include "clipboard/manager.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool finished;
int status;
clip::Received received;
bool multiple_done;
void multiple_event(const XEvent &event, void *)
{
    if (event.type == SelectionNotify) multiple_done = true;
}
void completed(int result, const clip::Received &value, void *)
{
    status = result;
    received = value;
    finished = true;
}
void transfer(clip::Selection &a, clip::Selection &b, const char *target)
{
    finished = false;
    assert(b.fetch(b.atom(target), completed, nullptr));
    double deadline = clip::now() + 5;
    while (!finished && clip::now() < deadline) { a.pump(); b.pump(); usleep(1000); }
    assert(finished);
}
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    using namespace clip;
    Text path;
    const char *uri = "file:///tmp/%E4%B8%AD%E6%96%87%20a%0A%25";
    assert(local_uri(uri, strlen(uri), path));
    assert(path == "/tmp/中文 a\n%");
    const char *invalid_uris[] = {"file://remote/a", "file:///a%00x", "file:///a%", "http:///a", "file:relative", "file:///a#x"};
    for (const char *invalid : invalid_uris)
        assert(!local_uri(invalid, strlen(invalid), path));
    Array<Text> paths;
    assert(paths.push(ra::text("/tmp/中文 a\n%")) && paths.push(ra::text("/tmp/b")));
    Entry files;
    assert(file_payload(paths, true, files));
    Array<Text> decoded;
    bool cut = false;
    assert(parse_files(files.find("x-special/gnome-copied-files")->bytes, true, decoded, cut));
    assert(cut && decoded.size() == 2 && decoded[0] == paths[0]);
    History history;
    assert(history.add(static_cast<Entry &&>(files)));
    const char binary[] = {'a', 0, 'b', '\n'};
    Entry image;
    assert(image.add("image/png", "image/png", binary, sizeof(binary)));
    assert(history.add(static_cast<Entry &&>(image)));
    assert(!save_history(argv[1], history));
    History loaded;
    assert(!load_history(argv[1], loaded));
    assert(loaded.entries.size() == 2 && same(loaded.entries[0], history.entries[0]) &&
           same(loaded.entries[1], history.entries[1]));
    assert(!save_preferences(argv[1], false));
    bool persist = true;
    assert(!load_preferences(argv[1], persist) && !persist);
    char filename[4096];
    snprintf(filename, sizeof(filename), "%s/history", argv[1]);
    int fd = open(filename, O_WRONLY | O_APPEND);
    assert(fd >= 0 && write(fd, "x", 1) == 1);
    close(fd);
    assert(load_history(argv[1], loaded) == -EINVAL && loaded.entries.size() == 2);
    assert(!delete_history(argv[1]) && load_history(argv[1], loaded) == -ENOENT);
    puts("PASS clipboard URI encoding, binary history, permissions and atomic state parsing");
    if (!getenv("DISPLAY")) return 0;
    Selection owner, consumer;
    assert(owner.open() && consumer.open());
    Entry text;
    Array<char> large;
    assert(large.resize(300000));
    memset(large.data(), 'Q', large.size());
    memcpy(large.data(), "中文", strlen("中文"));
    assert(text.add("UTF8_STRING", "UTF8_STRING", large.data(), large.size()));
    assert(owner.publish(text));
    transfer(owner, consumer, "TARGETS");
    assert(!status && received.format == 32 && received.bytes.size() == 16);
    transfer(owner, consumer, "UTF8_STRING");
    assert(!status && received.format == 8 && received.bytes == text.formats[0].bytes);
    transfer(owner, consumer, "TIMESTAMP");
    assert(!status && received.format == 32 && received.bytes.size() == 4 && Selection::word(received.bytes));
    transfer(owner, consumer, "application/unsupported");
    assert(status == -ENODATA);
    Atom pair_property = consumer.atom("QA_PAIRS"), first_property = consumer.atom("QA_TEXT");
    unsigned long pairs[] = {owner.atom("TIMESTAMP"), first_property,
        owner.atom("application/unsupported"), consumer.atom("QA_MISSING")};
    XChangeProperty(consumer.display(), consumer.window(), pair_property, consumer.atom("ATOM_PAIR"), 32,
        PropModeReplace, reinterpret_cast<unsigned char *>(pairs), 4);
    consumer.events = multiple_event;
    XConvertSelection(consumer.display(), consumer.atom("CLIPBOARD"), consumer.atom("MULTIPLE"),
        pair_property, consumer.window(), CurrentTime);
    XFlush(consumer.display());
    double multiple_deadline = now() + 5;
    while (!multiple_done && now() < multiple_deadline) { owner.pump(); consumer.pump(); usleep(1000); }
    assert(multiple_done);
    Atom actual; int format; unsigned long count, left; unsigned char *reply = nullptr;
    assert(XGetWindowProperty(consumer.display(), consumer.window(), pair_property, 0, 4, False,
        consumer.atom("ATOM_PAIR"), &actual, &format, &count, &left, &reply) == Success);
    assert(format == 32 && count == 4 && reinterpret_cast<unsigned long *>(reply)[3] == None);
    XFree(reply);
    consumer.events = nullptr;
    puts("PASS independent X11 clients, UTF-8, TARGETS, TIMESTAMP and INCR large transfer");
    puts("PASS MULTIPLE conversion with independent failed target reporting");
    Manager manager;
    assert(manager.initialize());
    double until = now() + 5;
    while (manager.history.entries.empty() && now() < until) { owner.pump(); manager.pump(); usleep(1000); }
    assert(manager.history.entries.size() == 1 && owner.owns());
    {
        Selection temporary;
        assert(temporary.open());
        Entry copied;
        assert(copied.add("UTF8_STRING", "UTF8_STRING", "来源退出后仍可粘贴", strlen("来源退出后仍可粘贴")));
        assert(temporary.publish(copied));
        until = now() + 5;
        while (manager.history.entries.size() < 2 && now() < until) { temporary.pump(); manager.pump(); usleep(1000); }
        assert(manager.history.entries.size() == 2 && temporary.owns());
    }
    manager.pump();
    assert(manager.selection.owns());
    transfer(manager.selection, consumer, "UTF8_STRING");
    assert(!status && received.bytes == "来源退出后仍可粘贴");
    assert(manager.use(1));
    transfer(manager.selection, consumer, "UTF8_STRING");
    assert(!status && received.bytes.size() == large.size());
    Entry exit_copy;
    assert(exit_copy.add("UTF8_STRING", "UTF8_STRING", "退出交接", strlen("退出交接")));
    assert(owner.publish(exit_copy));
    Atom save = owner.atom("SAVE_TARGETS");
    XConvertSelection(owner.display(), owner.atom("CLIPBOARD_MANAGER"), save, save, owner.window(), CurrentTime);
    XSync(owner.display(), False);
    manager.pump();
    // A duplicate owner notification must preserve an in-flight SAVE_TARGETS request.
    manager.changed();
    until = now() + 5;
    while (!manager.selection.owns() && now() < until) { owner.pump(); manager.pump(); usleep(1000); }
    assert(manager.selection.owns());
    reply = nullptr;
    assert(XGetWindowProperty(owner.display(), owner.window(), save, 0, 1, False, AnyPropertyType,
        &actual, &format, &count, &left, &reply) == Success);
    assert(actual == owner.atom("NULL") && format == 8 && count == 0 && left == 0);
    if (reply) XFree(reply);
    transfer(manager.selection, consumer, "UTF8_STRING");
    assert(!status && received.bytes == "退出交接");
    size_t before_pause = manager.history.entries.size();
    manager.paused = true;
    assert(owner.publish(text));
    until = now() + 1;
    while (now() < until) { owner.pump(); manager.pump(); usleep(1000); }
    assert(manager.history.entries.size() == before_pause);
    puts("PASS manager history, live-owner preservation, owner exit handoff and history selection");
    puts("PASS CLIPBOARD_MANAGER/SAVE_TARGETS handoff and paused history recording");
}
