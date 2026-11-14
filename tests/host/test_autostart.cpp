#include "taskmgr/autostart.h"
#include <glib.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace {
void put(const char *directory, const char *id, const char *data)
{
    char *path = nullptr;
    assert(asprintf(&path, "%s/%s", directory, id) >= 0);
    FILE *file = fopen(path, "w");
    assert(file && fputs(data, file) >= 0 && !fclose(file));
    free(path);
}
autostart::Entry entry(const autostart::Snapshot &snapshot, const char *id)
{
    for (const auto &item : snapshot.entries) if (item.id == id) return item;
    assert(false);
    return {};
}
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--run-autostart")) return autostart::run_session();
    assert(argc == 2);
    setenv("LC_ALL", "C", 1);
    setenv("LANGUAGE", "C", 1);
    using namespace autostart;
    assert(!chdir(argv[1]));
    const char *folders[] = {"user", "system1", "system2", "user/autostart", "system1/autostart", "system2/autostart"};
    for (const char *name : folders)
        assert(!mkdir(name, 0700));
    char *personal = realpath("user", nullptr), *first = realpath("system1", nullptr), *second = realpath("system2", nullptr);
    char *directories = nullptr;
    assert(asprintf(&directories, "%s:%s", first, second) >= 0);
    setenv("XDG_CONFIG_HOME", personal, 1); setenv("XDG_CONFIG_DIRS", directories, 1);
    setenv("XDG_CURRENT_DESKTOP", "ICEWM:ReliefOS", 1);
    Paths paths; Text error;
    assert(paths.initialize(error));
    put("system1/autostart", "shared.desktop", "[Desktop Entry]\nType=Application\nName=System first\nExec=/bin/true\nNoDisplay=true\n");
    put("system2/autostart", "shared.desktop", "[Desktop Entry]\nType=Application\nName=System second\nExec=/bin/false\n");
    put("user/autostart", "hidden.desktop", "[Desktop Entry]\nHidden=true\n");
    put("system1/autostart", "other.desktop", "[Desktop Entry]\nType=Application\nName=Other\nExec=/bin/true\nOnlyShowIn=GNOME;\n");
    put("system1/autostart", "missing.desktop", "[Desktop Entry]\nType=Application\nName=Missing\nExec=/bin/true\nTryExec=reliefos-does-not-exist\n");
    put("system1/autostart", "invalid.desktop", "[Desktop Entry]\nType=Application\nName=Invalid\nExec=/bin/true %z\n");
    Snapshot snapshot;
    assert(scan(paths, snapshot));
    assert(entry(snapshot, "shared.desktop").name == "System first");
    assert(entry(snapshot, "shared.desktop").state == State::Ready);
    assert(entry(snapshot, "hidden.desktop").state == State::Disabled);
    assert(entry(snapshot, "other.desktop").state == State::OtherDesktop);
    assert(entry(snapshot, "missing.desktop").state == State::MissingProgram);
    assert(entry(snapshot, "invalid.desktop").state == State::Invalid);
    put("system1/autostart", "relative.desktop", "[Desktop Entry]\nType=Application\nName=Relative\nExec=/bin/true\nPath=.\n");
    put("system1/autostart", "empty-desktop.desktop", "[Desktop Entry]\nType=Application\nName=Empty desktop\nExec=/bin/true\nOnlyShowIn=\n");
    assert(scan(paths, snapshot));
    assert(entry(snapshot, "relative.desktop").state == State::Ready);
    assert(entry(snapshot, "empty-desktop.desktop").state == State::OtherDesktop);
    put("user/autostart", "shared.desktop", "# preserved comment\n[Desktop Entry]\nType=Application\nName=Personal\nName[ja]=元の名前\nExec=/bin/true\nX-Vendor-Setting=preserved\n");
    assert(scan(paths, snapshot));
    Request edit_request;
    edit_request.action = Action::Edit;
    edit_request.original = entry(snapshot, "shared.desktop");
    edit_request.edit.name.set("Edited"); edit_request.edit.command.set("/bin/echo hello");
    Text edited, edit_error;
    assert(change(paths, edit_request, edited, edit_error));
    assert(scan(paths, snapshot));
    Entry edited_entry = entry(snapshot, "shared.desktop");
    assert(edited_entry.name == "Edited" && strstr(edited_entry.data.c_str(), "X-Vendor-Setting=preserved") &&
           strstr(edited_entry.data.c_str(), "# preserved comment") && strstr(edited_entry.data.c_str(), "Name[ja]=元の名前"));
    edit_request.action = Action::Remove; edit_request.original = edited_entry;
    assert(change(paths, edit_request, edited, edit_error));
    assert(scan(paths, snapshot));
    Request request;
    request.action = Action::Toggle; request.original = entry(snapshot, "shared.desktop");
    request.edit.enabled = false;
    Text selected;
    assert(change(paths, request, selected, error));
    assert(scan(paths, snapshot));
    Entry disabled = entry(snapshot, "shared.desktop");
    assert(disabled.personal && disabled.fallback && disabled.state == State::Disabled);
    // A stale view must not overwrite a more recent user change.
    assert(!change(paths, request, selected, error));
    request.action = Action::Remove; request.original = disabled;
    assert(change(paths, request, selected, error));
    assert(scan(paths, snapshot) && entry(snapshot, "shared.desktop").state == State::Ready);
    // Other XDG managers commonly disable a system entry with a minimal tombstone.
    put("user/autostart", "shared.desktop", "# personal setting\n[Desktop Entry]\nHidden=true\nX-Personal=keep\n");
    assert(scan(paths, snapshot));
    request.action = Action::Toggle; request.original = entry(snapshot, "shared.desktop"); request.edit.enabled = true;
    assert(change(paths, request, selected, error));
    assert(scan(paths, snapshot));
    Entry enabled = entry(snapshot, "shared.desktop");
    assert(enabled.state == State::Ready && enabled.name == "System first");
    assert(strstr(enabled.data.c_str(), "# personal setting") && strstr(enabled.data.c_str(), "X-Personal=keep"));
    request = Request{};
    request.action = Action::Create;
    request.edit.name.set("中文启动程序"); request.edit.command.set("/bin/true");
    request.edit.comment.set("User entry");
    assert(change(paths, request, selected, error) && !selected.empty());
    assert(scan(paths, snapshot));
    Entry created = entry(snapshot, selected.c_str());
    assert(created.name == "中文启动程序" && created.personal && !created.fallback);
    Text encoded; Snapshot decoded;
    assert(encode(snapshot, encoded) && decode(encoded, decoded));
    assert(decoded.entries.size() == snapshot.entries.size());
    assert(entry(decoded, selected.c_str()).data == created.data);
    assert(!decode(ra::text("broken"), decoded));
    request.action = Action::Remove; request.original = created;
    assert(change(paths, request, selected, error));
    const char *dbus_entries[] = {
        "[Desktop Entry]\nType=Application\nName=DBus\nDBusActivatable=true\nExec=/bin/true\n",
        "[Desktop Entry]\nType=Application\nName=DBus\nDBusActivatable=true\n"
    };
    for (const char *definition : dbus_entries) {
        put("user/autostart", "org.reliefos.Qa.desktop", definition);
        assert(scan(paths, snapshot));
        Request metadata;
        metadata.action = Action::Edit; metadata.original = entry(snapshot, "org.reliefos.Qa.desktop");
        metadata.edit.name.set("DBus metadata"); metadata.edit.command = metadata.original.command;
        metadata.edit.comment.set("Only metadata changed");
        assert(change(paths, metadata, selected, error));
        assert(scan(paths, snapshot));
        Entry updated = entry(snapshot, "org.reliefos.Qa.desktop");
        assert(strstr(updated.data.c_str(), "DBusActivatable=true"));
        metadata.action = Action::Remove; metadata.original = updated;
        assert(change(paths, metadata, selected, error));
    }
    put("user/autostart", "localized.desktop", "[Desktop Entry]\nType=Application\nName=English name\nName[ja]=元の名前\nComment=English comment\nComment[ja]=元の説明\nExec=/bin/true\n");
    setenv("LANGUAGE", "ja", 1);
    assert(scan(paths, snapshot));
    Request localized;
    localized.action = Action::Edit; localized.original = entry(snapshot, "localized.desktop");
    assert(localized.original.name == "元の名前");
    localized.edit.name.set("新しい名前"); localized.edit.comment.set("新しい説明"); localized.edit.command = localized.original.command;
    assert(change(paths, localized, selected, error));
    assert(scan(paths, snapshot));
    Entry translated = entry(snapshot, "localized.desktop");
    assert(translated.name == "新しい名前" && strstr(translated.data.c_str(), "Name=English name") &&
           strstr(translated.data.c_str(), "Comment=English comment"));
    localized.action = Action::Remove; localized.original = translated;
    assert(change(paths, localized, selected, error));
    setenv("LANGUAGE", "C", 1);
    assert(valid_command("/bin/echo \"中文\" %% %c", error));
    assert(!valid_command("/bin/echo %z", error));
    assert(!valid_command("/bin/echo %f %U", error));
    assert(!valid_command("/bin/echo \"%u\"", error));
    assert(!valid_command("/bin/echo '%u'", error));
    assert(!valid_command("/bin/echo \\%z", error));
    assert(valid_command("/bin/true %d %D %n %N %v %m", error));
    // Launch with real GIO expansion and keep ownership through session end.
    assert(!mkdir("runtime", 0700));
    char *runtime = realpath("runtime", nullptr);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("DISPLAY", ":autostart-test", 1);
    put(".", "record", "#!/bin/sh\nprintf '%s' \"$1\" > runner-args\npwd > runner-cwd\nexec sleep 60\n");
    assert(!chmod("record", 0700));
    put(".", "% recorder", "#!/bin/sh\nprintf '%s' \"$1\" > percent-args\n");
    assert(!chmod("% recorder", 0700));
    char *cwd = getcwd(nullptr, 0), *command = nullptr;
    assert(asprintf(&command, "[Desktop Entry]\nType=Application\nName=Runner\nExec=%s/record \"hello 中文\"\nPath=%s\n", cwd, cwd) >= 0);
    put("user/autostart", "runner.desktop", command);
    char *percent_path = nullptr;
    assert(asprintf(&percent_path, "%s/%% recorder", cwd) >= 0);
    Text percent_command = executable_command(percent_path);
    // Serialize the command through KeyFile so its string-level escapes are correct.
    Request percent_request; percent_request.action = Action::Create;
    percent_request.edit.name.set("Percent");
    char *with_argument = nullptr;
    assert(asprintf(&with_argument, "%s \"percent 中文\"", percent_command.c_str()) >= 0);
    percent_request.edit.command.set(with_argument); percent_request.edit.directory.set(cwd);
    assert(change(paths, percent_request, selected, error));
    pid_t runner = fork();
    assert(runner >= 0);
    if (!runner) {
        g_idle_add([](gpointer) -> gboolean { return G_SOURCE_CONTINUE; }, nullptr);
        _exit(run_session());
    }
    for (unsigned n = 0; n < 100 && access("runner-args", F_OK); ++n) usleep(50000);
    assert(!access("runner-args", F_OK));
    char content[256] = {};
    FILE *recorded = fopen("runner-args", "r");
    assert(recorded && fread(content, 1, sizeof(content) - 1, recorded) > 0 && !fclose(recorded));
    assert(!strcmp(content, "hello 中文"));
    for (unsigned n = 0; n < 100 && access("percent-args", F_OK); ++n) usleep(50000);
    assert(!access("percent-args", F_OK));
    recorded = fopen("percent-args", "r");
    memset(content, 0, sizeof(content));
    assert(recorded && fread(content, 1, sizeof(content) - 1, recorded) > 0 && !fclose(recorded));
    assert(!strcmp(content, "percent 中文"));
    pid_t duplicate = fork();
    assert(duplicate >= 0);
    if (!duplicate) _exit(run_session());
    int status = 0;
    assert(waitpid(duplicate, &status, 0) == duplicate && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!kill(runner, SIGTERM));
    bool stopped = false;
    for (unsigned n = 0; n < 100; ++n) {
        if (waitpid(runner, &status, WNOHANG) == runner) { stopped = true; break; }
        usleep(20000);
    }
    if (!stopped) { kill(runner, SIGKILL); waitpid(runner, &status, 0); }
    assert(stopped && WIFEXITED(status) && !WEXITSTATUS(status));
    free(runtime); free(cwd); free(command); free(percent_path); free(with_argument);
    puts("PASS XDG precedence, Hidden/OnlyShowIn/TryExec/NoDisplay, user overrides, stale changes and UTF-8 state");
    free(personal); free(first); free(second); free(directories);
}
