#include "autostart.h"
#include <gio/gdesktopappinfo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace autostart {
namespace {
constexpr const char *Group = "Desktop Entry";
struct KeyFile {
    GKeyFile *value = g_key_file_new();
    ~KeyFile() { g_key_file_unref(value); }
};
struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) close(value); }
};

bool fail(Text &error, const char *message)
{
    error.set(message);
    return false;
}

Text joined(const char *directory, const char *name)
{
    char *path = g_build_filename(directory, name, nullptr);
    Text result = ra::text(path);
    g_free(path);
    return result;
}

bool add_directory(Paths &paths, const char *base)
{
    if (!base || base[0] != '/') return true;
    Text path = joined(base, "autostart");
    for (const auto &existing : paths.directories) if (existing == path) return true;
    return paths.directories.push(path);
}

bool load(const char *path, Text &data, Text &error)
{
    Descriptor fd(open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (fd.value < 0) return fail(error, "Cannot read startup entry.");
    struct stat st;
    if (fstat(fd.value, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > MaxFileBytes)
        return fail(error, "Startup entry is not a regular file or exceeds 1 MiB.");
    char *buffer = static_cast<char *>(malloc(MaxFileBytes + 1));
    if (!buffer) return fail(error, "Not enough memory.");
    size_t used = 0;
    for (;;) {
        ssize_t count = read(fd.value, buffer + used, MaxFileBytes + 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || used + static_cast<size_t>(count) > MaxFileBytes) {
            free(buffer);
            return fail(error, "Cannot read startup entry.");
        }
        if (!count) break;
        used += static_cast<size_t>(count);
    }
    bool ok = data.set(buffer, used);
    free(buffer);
    return ok || fail(error, "Not enough memory.");
}

Text key(GKeyFile *file, const char *name, bool localized = false)
{
    char *value = localized ? g_key_file_get_locale_string(file, Group, name, nullptr, nullptr)
                            : g_key_file_get_string(file, Group, name, nullptr);
    Text result = ra::text(value);
    g_free(value);
    return result;
}

bool desktop_matches(const Text &list, const Text &desktop)
{
    gchar **items = g_strsplit(list.c_str(), ";", -1);
    gchar **desktops = g_strsplit(desktop.c_str(), ":", -1);
    bool match = false;
    for (size_t i = 0; items[i]; ++i)
        for (size_t n = 0; desktops[n]; ++n)
            if (*items[i] && !strcmp(items[i], desktops[n])) match = true;
    g_strfreev(items);
    g_strfreev(desktops);
    return match;
}

bool executable(const Text &name)
{
    if (name.empty()) return true;
    if (name.c_str()[0] == '/') return !access(name.c_str(), X_OK) && !g_file_test(name.c_str(), G_FILE_TEST_IS_DIR);
    if (strchr(name.c_str(), '/')) return false;
    char *path = g_find_program_in_path(name.c_str());
    bool found = path != nullptr;
    g_free(path);
    return found;
}

void parse(Entry &entry, const Paths &paths)
{
    KeyFile file;
    entry.name = entry.id;
    if (memchr(entry.data.c_str(), 0, entry.data.size()) ||
        !g_key_file_load_from_data(file.value, entry.data.c_str(), entry.data.size(),
                                  G_KEY_FILE_KEEP_COMMENTS, nullptr)) return;
    entry.hidden = g_key_file_get_boolean(file.value, Group, "Hidden", nullptr);
    Text name = key(file.value, "Name", true);
    if (!name.empty()) entry.name = name;
    entry.command = key(file.value, "Exec");
    entry.comment = key(file.value, "Comment", true);
    entry.directory = key(file.value, "Path");
    entry.try_exec = key(file.value, "TryExec");
    entry.only_show = key(file.value, "OnlyShowIn");
    entry.not_show = key(file.value, "NotShowIn");
    entry.icon = key(file.value, "Icon");
    entry.terminal = g_key_file_get_boolean(file.value, Group, "Terminal", nullptr);
    entry.dbus = g_key_file_get_boolean(file.value, Group, "DBusActivatable", nullptr);
    if (entry.hidden) { entry.state = State::Disabled; return; }
    Text error;
    bool has_only = g_key_file_has_key(file.value, Group, "OnlyShowIn", nullptr);
    bool has_not = g_key_file_has_key(file.value, Group, "NotShowIn", nullptr);
    if (!(key(file.value, "Type") == "Application") || name.empty() ||
        (has_only && has_not) ||
        ((!entry.dbus || !entry.command.empty()) && !valid_command(entry.command.c_str(), error))) return;
    if ((has_only && !desktop_matches(entry.only_show, paths.desktop)) ||
        desktop_matches(entry.not_show, paths.desktop)) {
        entry.state = State::OtherDesktop;
        return;
    }
    if (!executable(entry.try_exec)) { entry.state = State::MissingProgram; return; }
    GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(entry.source.c_str());
    if (!app) { entry.state = State::MissingProgram; return; }
    g_object_unref(app);
    entry.state = State::Ready;
}

bool safe_id(const Text &id)
{
    return !id.empty() && !strchr(id.c_str(), '/') && g_str_has_suffix(id.c_str(), ".desktop") &&
           !memchr(id.c_str(), 0, id.size());
}

bool edit_keyfile(GKeyFile *file, const Edit &edit, Text &error)
{
    if (edit.name.empty() || !g_utf8_validate(edit.name.c_str(), edit.name.size(), nullptr))
        return fail(error, "Enter a valid UTF-8 name.");
    bool dbus = g_key_file_get_boolean(file, Group, "DBusActivatable", nullptr);
    bool command_changed = key(file, "Exec") != edit.command;
    if (!(dbus && !command_changed && edit.command.empty()) && !valid_command(edit.command.c_str(), error)) return false;
    if (!edit.try_exec.empty() && edit.try_exec.c_str()[0] != '/' && strchr(edit.try_exec.c_str(), '/'))
        return fail(error, "TryExec must be an absolute path or a program name.");
    char *locale = g_key_file_get_locale_for_key(file, Group, "Name", nullptr);
    if (locale) g_key_file_set_locale_string(file, Group, "Name", locale, edit.name.c_str());
    else g_key_file_set_string(file, Group, "Name", edit.name.c_str());
    g_free(locale);
    g_key_file_set_string(file, Group, "Type", "Application");
    if (!edit.command.empty()) g_key_file_set_string(file, Group, "Exec", edit.command.c_str());
    else g_key_file_remove_key(file, Group, "Exec", nullptr);
    locale = g_key_file_get_locale_for_key(file, Group, "Comment", nullptr);
    if (locale) g_key_file_set_locale_string(file, Group, "Comment", locale, edit.comment.c_str());
    else g_key_file_set_string(file, Group, "Comment", edit.comment.c_str());
    g_free(locale);
    g_key_file_set_string(file, Group, "Path", edit.directory.c_str());
    if (edit.directory.empty()) g_key_file_remove_key(file, Group, "Path", nullptr);
    g_key_file_set_string(file, Group, "TryExec", edit.try_exec.c_str());
    if (edit.try_exec.empty()) g_key_file_remove_key(file, Group, "TryExec", nullptr);
    g_key_file_set_boolean(file, Group, "Terminal", edit.terminal);
    g_key_file_set_boolean(file, Group, "Hidden", !edit.enabled);
    // An explicitly changed command takes effect. Metadata edits preserve activation.
    if (command_changed) g_key_file_set_boolean(file, Group, "DBusActivatable", false);
    return true;
}

bool inherit_definition(const Paths &paths, const Text &id, GKeyFile *personal, Text &error)
{
    // Minimal Hidden overrides are standard. Re-enable them using the current
    // highest-priority system definition, keeping all existing personal keys.
    for (size_t n = 1; n < paths.directories.size(); ++n) {
        Text source = joined(paths.directories[n].c_str(), id.c_str());
        struct stat st;
        if (lstat(source.c_str(), &st)) {
            if (errno == ENOENT) continue;
            return fail(error, "Cannot read startup entry.");
        }
        Text data;
        KeyFile system;
        if (!load(source.c_str(), data, error)) return false;
        if (!g_key_file_load_from_data(system.value, data.c_str(), data.size(),
                                      G_KEY_FILE_KEEP_TRANSLATIONS, nullptr))
            return fail(error, "The startup entry cannot be parsed. Remove it and add a new entry.");
        gchar **groups = g_key_file_get_groups(system.value, nullptr);
        for (size_t group = 0; groups[group]; ++group) {
            gchar **keys = g_key_file_get_keys(system.value, groups[group], nullptr, nullptr);
            for (size_t k = 0; keys && keys[k]; ++k) {
                if (g_key_file_has_key(personal, groups[group], keys[k], nullptr)) continue;
                char *value = g_key_file_get_value(system.value, groups[group], keys[k], nullptr);
                if (value) g_key_file_set_value(personal, groups[group], keys[k], value);
                g_free(value);
            }
            g_strfreev(keys);
        }
        g_strfreev(groups);
        return true;
    }
    return fail(error, "This entry changed. Refresh and try again.");
}

void add_bytes(GVariantBuilder *builder, const char *name, const Text &value)
{
    g_variant_builder_add(builder, "{sv}", name,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.c_str(), value.size(), 1));
}

bool get_bytes(GVariant *record, const char *name, Text &value)
{
    GVariant *bytes = g_variant_lookup_value(record, name, G_VARIANT_TYPE("ay"));
    if (!bytes) return false;
    gsize size;
    const char *data = static_cast<const char *>(g_variant_get_fixed_array(bytes, &size, 1));
    bool ok = value.set(data, size);
    g_variant_unref(bytes);
    return ok;
}
} // namespace

bool Paths::initialize(Text &error)
{
    directories.clear();
    const char *home = getenv("HOME"), *config = getenv("XDG_CONFIG_HOME");
    if (!config || config[0] != '/') {
        if (!home || home[0] != '/') return fail(error, "The account has no absolute home directory.");
        Text base = joined(home, ".config");
        personal = joined(base.c_str(), "autostart");
    } else personal = joined(config, "autostart");
    if (!directories.push(personal)) return fail(error, "Not enough memory.");
    const char *system = getenv("XDG_CONFIG_DIRS");
    gchar **bases = g_strsplit(system && *system ? system : "/etc/xdg", ":", -1);
    bool ok = true;
    for (size_t i = 0; bases[i]; ++i) if (!add_directory(*this, bases[i])) ok = false;
    g_strfreev(bases);
    const char *environment = getenv("XDG_CURRENT_DESKTOP");
    desktop.set(environment && *environment ? environment : "ICEWM");
    return ok || fail(error, "Not enough memory.");
}

bool valid_command(const char *command, Text &error)
{
    if (!command || !*command || !g_utf8_validate(command, -1, nullptr))
        return fail(error, "Enter a valid UTF-8 command.");
    bool quoted = false, escaped = false;
    unsigned file_codes = 0;
    for (const char *p = command; *p; ++p) {
        if (escaped) {
            escaped = false;
            if (*p != '%') continue;
        }
        if (*p == '\\') { escaped = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (!quoted && strchr("'`><~|&;$*?#()", *p))
            return fail(error, "Quote special characters with double quotes.");
        if (*p != '%') continue;
        char code = *++p;
        if (!code || !strchr("%fFuUickdDnNvm", code) || (quoted && code != '%'))
            return fail(error, "The command has an unsupported field code.");
        if (strchr("fFuU", code) && ++file_codes > 1)
            return fail(error, "Use only one file or URL field code.");
        if (strchr("FUi", code) && ((p > command + 1 && p[-2] != ' ' && p[-2] != '\t') ||
                                    (p[1] && p[1] != ' ' && p[1] != '\t')))
            return fail(error, "Multi-argument field codes must be separate arguments.");
    }
    if (quoted || escaped) return fail(error, "The command has an unfinished quote or escape.");
    int argc = 0;
    gchar **argv = nullptr;
    bool ok = g_shell_parse_argv(command, &argc, &argv, nullptr) && argc && argv[0][0];
    g_strfreev(argv);
    return ok || fail(error, "Enter a command and valid arguments.");
}

Text executable_command(const char *path)
{
    // GIO checks the executable before expanding %%. A constant shell wrapper
    // preserves literal percent filenames while passing the path as one argv.
    GString *out = g_string_new(strchr(path, '%') ? "/bin/sh -c \"exec \\\"$@\\\"\" autostart \"" : "\"");
    for (const char *p = path; *p; ++p) {
        if (strchr("\"`$\\", *p)) g_string_append_c(out, '\\');
        if (*p == '%') g_string_append_c(out, '%');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    Text result = ra::text(out->str);
    g_string_free(out, true);
    return result;
}

bool scan(const Paths &paths, Snapshot &snapshot)
{
    snapshot.entries.clear();
    snapshot.error.clear();
    size_t bytes = 0;
    for (size_t n = 0; n < paths.directories.size(); ++n) {
        DIR *directory = opendir(paths.directories[n].c_str());
        if (!directory) {
            if (errno == ENOENT) continue;
            return fail(snapshot.error, "Cannot read an autostart directory.");
        }
        struct dirent *child;
        bool ok = true;
        errno = 0;
        while ((child = readdir(directory))) {
            Text id = ra::text(child->d_name);
            if (!safe_id(id)) { errno = 0; continue; }
            Entry *existing = nullptr;
            for (auto &entry : snapshot.entries) if (entry.id == id) { existing = &entry; break; }
            if (existing) { if (n && existing->personal) existing->fallback = true; errno = 0; continue; }
            if (snapshot.entries.size() >= MaxEntries) { ok = fail(snapshot.error, "Too many startup entries (maximum 2048)."); break; }
            Entry entry;
            entry.id = id;
            entry.source = joined(paths.directories[n].c_str(), child->d_name);
            entry.personal = n == 0;
            Text ignored;
            if (load(entry.source.c_str(), entry.data, ignored)) parse(entry, paths);
            else entry.name = entry.id;
            bytes += entry.data.size();
            if (bytes > MaxSnapshotBytes / 2) { ok = fail(snapshot.error, "Startup entries exceed the memory limit."); break; }
            if (!snapshot.entries.push(static_cast<Entry &&>(entry))) { ok = fail(snapshot.error, "Not enough memory."); break; }
            errno = 0;
        }
        if (ok && errno) ok = fail(snapshot.error, "Cannot read an autostart directory.");
        closedir(directory);
        if (!ok) return false;
    }
    // Stable ordering also keeps refreshes from moving the selected row.
    for (size_t n = 1; n < snapshot.entries.size(); ++n) {
        size_t at = n;
        while (at && strcmp(snapshot.entries[at - 1].name.c_str(), snapshot.entries[at].name.c_str()) > 0) {
            Entry saved = snapshot.entries[at - 1];
            snapshot.entries[at - 1] = snapshot.entries[at];
            snapshot.entries[at] = saved;
            --at;
        }
    }
    return true;
}

bool change(const Paths &paths, Request &request, Text &selected, Text &error)
{
    if (g_mkdir_with_parents(paths.personal.c_str(), 0700)) return fail(error, "Cannot create the personal autostart directory.");
    Descriptor directory(open(paths.personal.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.value < 0) return fail(error, "Cannot open the personal autostart directory.");
    Descriptor lock(openat(directory.value, ".taskmgr.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.value < 0 || flock(lock.value, LOCK_EX)) return fail(error, "Cannot lock the personal autostart directory.");
    Snapshot current;
    if (!scan(paths, current)) { error = current.error; return false; }
    if (request.action != Action::Create) {
        const Entry *found = nullptr;
        for (const auto &entry : current.entries) if (entry.id == request.original.id) found = &entry;
        if (!found || found->source != request.original.source || found->data != request.original.data ||
            found->personal != request.original.personal || found->fallback != request.original.fallback)
            return fail(error, "This entry changed. Refresh and try again.");
        if (!safe_id(request.original.id)) return fail(error, "Invalid startup filename.");
        selected = request.original.id;
    } else {
        if (current.entries.size() >= MaxEntries) return fail(error, "Too many startup entries (maximum 2048).");
        char id[80];
        for (unsigned n = 1; n <= MaxEntries + 1; ++n) {
            snprintf(id, sizeof(id), "reliefos-startup-%u.desktop", n);
            bool exists = false;
            for (const auto &entry : current.entries) if (entry.id == id) exists = true;
            if (!exists) { selected.set(id); break; }
        }
        if (selected.empty()) return fail(error, "Cannot allocate a startup filename.");
    }
    if (request.action == Action::Remove) {
        if (!request.original.personal) return fail(error, "Use Disable to override a system startup entry.");
        if (unlinkat(directory.value, selected.c_str(), 0)) return fail(error, "Cannot remove the personal startup entry.");
        if (fsync(directory.value)) return fail(error, "The change was applied, but directory synchronization failed.");
        return true;
    }
    KeyFile file;
    if (request.action != Action::Create &&
        !g_key_file_load_from_data(file.value, request.original.data.c_str(), request.original.data.size(),
                                  GKeyFileFlags(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), nullptr))
        return fail(error, "The startup entry cannot be parsed. Remove it and add a new entry.");
    if (request.action == Action::Toggle) {
        bool incomplete = key(file.value, "Type").empty() ||
            (key(file.value, "Exec").empty() && !g_key_file_get_boolean(file.value, Group, "DBusActivatable", nullptr));
        if (request.edit.enabled && request.original.fallback && incomplete &&
            !inherit_definition(paths, request.original.id, file.value, error)) return false;
        g_key_file_set_boolean(file.value, Group, "Hidden", !request.edit.enabled);
    } else if (!edit_keyfile(file.value, request.edit, error)) return false;
    gsize size;
    char *data = g_key_file_to_data(file.value, &size, nullptr);
    if (!data || size > MaxFileBytes) { g_free(data); return fail(error, "Startup entry exceeds 1 MiB."); }
    char temporary[96];
    snprintf(temporary, sizeof(temporary), ".taskmgr-%ld-XXXXXX", static_cast<long>(getpid()));
    Text full = joined(paths.personal.c_str(), temporary);
    char *template_path = g_strdup(full.c_str());
    Descriptor output(g_mkstemp_full(template_path, O_RDWR | O_CLOEXEC, 0600));
    const char *leaf = strrchr(template_path, '/') + 1;
    bool ok = output.value >= 0;
    size_t used = 0;
    while (ok && used < size) {
        ssize_t count = write(output.value, data + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false;
        else used += static_cast<size_t>(count);
    }
    if (ok) ok = !fsync(output.value);
    if (ok) ok = !renameat(directory.value, leaf, directory.value, selected.c_str());
    if (!ok) { unlinkat(directory.value, leaf, 0); fail(error, "Cannot save the personal startup entry."); }
    else if (fsync(directory.value)) ok = fail(error, "The change was applied, but directory synchronization failed.");
    g_free(template_path);
    g_free(data);
    return ok;
}

bool switch_user(uid_t uid, Text &error)
{
    if (getuid() == uid && geteuid() == uid) return true;
    if (geteuid()) return fail(error, "Only root can manage another user's startup entries.");
    struct passwd *account = getpwuid(uid);
    if (!account || account->pw_dir[0] != '/') return fail(error, "The selected account has no home directory.");
    setenv("HOME", account->pw_dir, 1);
    setenv("USER", account->pw_name, 1);
    setenv("LOGNAME", account->pw_name, 1);
    const char *variables[] = {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"};
    for (const char *name : variables) unsetenv(name);
    if (initgroups(account->pw_name, account->pw_gid) || setgid(account->pw_gid) || setuid(uid))
        return fail(error, "Cannot switch to the selected account.");
    return true;
}

bool encode(const Snapshot &snapshot, Text &output)
{
    GVariantBuilder root, rows;
    g_variant_builder_init(&root, G_VARIANT_TYPE_VARDICT);
    add_bytes(&root, "error", snapshot.error);
    add_bytes(&root, "selected", snapshot.selected);
    g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
    for (const auto &entry : snapshot.entries) {
        GVariantBuilder row;
        g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
#define SAVE(field) add_bytes(&row, #field, entry.field)
        SAVE(id); SAVE(source); SAVE(name); SAVE(command); SAVE(comment); SAVE(directory);
        SAVE(try_exec); SAVE(only_show); SAVE(not_show); SAVE(icon); SAVE(data);
#undef SAVE
        guint32 flags = static_cast<guint32>(entry.state) << 5 | entry.personal |
                        entry.fallback << 1 | entry.hidden << 2 | entry.terminal << 3 | entry.dbus << 4;
        g_variant_builder_add(&row, "{sv}", "flags", g_variant_new_uint32(flags));
        g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
    }
    g_variant_builder_add(&root, "{sv}", "rows", g_variant_builder_end(&rows));
    GVariant *value = g_variant_ref_sink(g_variant_builder_end(&root));
    bool ok = g_variant_get_size(value) <= MaxSnapshotBytes &&
              output.set(static_cast<const char *>(g_variant_get_data(value)), g_variant_get_size(value));
    g_variant_unref(value);
    return ok;
}

bool decode(const Text &input, Snapshot &snapshot)
{
    if (input.empty() || input.size() > MaxSnapshotBytes) return false;
    GBytes *bytes = g_bytes_new(input.c_str(), input.size());
    GVariant *root = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE_VARDICT, bytes, false));
    g_bytes_unref(bytes);
    Snapshot next;
    bool ok = g_variant_is_normal_form(root) && get_bytes(root, "error", next.error) &&
              get_bytes(root, "selected", next.selected);
    GVariant *rows = ok ? g_variant_lookup_value(root, "rows", G_VARIANT_TYPE("aa{sv}")) : nullptr;
    if (!rows || g_variant_n_children(rows) > MaxEntries) ok = false;
    for (size_t n = 0; ok && n < g_variant_n_children(rows); ++n) {
        GVariant *row = g_variant_get_child_value(rows, n);
        Entry entry;
#define READ(field) ok = get_bytes(row, #field, entry.field) && ok
        READ(id); READ(source); READ(name); READ(command); READ(comment); READ(directory);
        READ(try_exec); READ(only_show); READ(not_show); READ(icon); READ(data);
#undef READ
        guint32 flags = 0;
        ok = g_variant_lookup(row, "flags", "u", &flags) && (flags >> 5) <= static_cast<unsigned>(State::Invalid) && ok;
        entry.personal = flags & 1; entry.fallback = flags & 2;
        entry.hidden = flags & 4; entry.terminal = flags & 8;
        entry.dbus = flags & 16;
        entry.state = static_cast<State>(flags >> 5);
        ok = ok && safe_id(entry.id) && next.entries.push(static_cast<Entry &&>(entry));
        g_variant_unref(row);
    }
    if (rows) g_variant_unref(rows);
    g_variant_unref(root);
    if (ok) snapshot = static_cast<Snapshot &&>(next);
    return ok;
}
} // namespace autostart
