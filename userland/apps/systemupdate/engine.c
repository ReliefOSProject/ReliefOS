/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static int make_directory(const char *path)
{
    int fd = update_directory(path, 1); if (fd < 0) return fd;
    struct stat status; int result = fstat(fd, &status) ? -errno :
        status.st_uid != geteuid() || (status.st_mode & 0077) ? -EPERM : 0;
    if (!result && fsync(fd)) result = -errno;
    close(fd); return result;
}

static int terminal(const char *phase)
{
    return !strcmp(phase, "complete") || !strcmp(phase, "stale") || !strcmp(phase, "ready");
}

static int mutation_allowed(const char *root)
{
    if (geteuid()) return -EPERM;
    if (!strcmp(root, "/")) {
        struct utsname system;
        if (uname(&system) || strcmp(system.sysname, "ReliefNT")) return -EPERM;
    }
    return 0;
}

int update_context_init(struct update_context *context, const char *root, int writing)
{
    context->lock = -1;
    if (!root || root[0] != '/' || strlen(root) >= sizeof(context->root)) return -EINVAL;
    int directory = update_directory(root, 0); if (directory < 0) return directory; close(directory);
    if (!realpath(root, context->root)) return -errno;
    context->isolated = strcmp(context->root, "/") != 0;
    if (writing) { int result = mutation_allowed(context->root); if (result) return result; }
    int result = update_path(context->base, context->root, "var/lib/reliefos/update");
    if (!result) result = update_path(context->apk, context->root, "sbin/apk");
    if (!result) result = update_path(context->fetch, context->root, "usr/sbin/rprfetch");
    if (!result) result = update_path(context->bundle, context->root, "usr/libexec/reliefos/kernel-bundle");
    if (!result && writing) result = make_directory(context->base);
    if (!result && writing) {
        char path[PATH_MAX]; result = update_path(path, context->base, "lock");
        if (!result) {
            context->lock = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
            if (context->lock < 0) result = -errno;
        }
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
        if (!result && fcntl(context->lock, F_SETLK, &lock)) result = errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;
    }
    return result;
}

int update_plan_open(struct update_context *context, const char *id)
{
    if (!update_token(id)) return -EINVAL;
    char member[80]; snprintf(member, sizeof(member), "plans/%s", id);
    int result = update_path(context->plan, context->base, member);
    if (!result) { int fd = update_directory(context->plan, 0); if (fd < 0) result = fd; else close(fd); }
    if (!result) result = update_path(context->cache, context->plan, "cache");
    if (!result) result = update_path(context->apk, context->plan, "tools/apk.static");
    if (!result) {
        char saved[PATH_MAX]; result = update_path(saved, context->plan, "tools/kernel-bundle");
        if (!result && !update_regular(saved, UPDATE_METADATA_MAX)) strcpy(context->bundle, saved);
    }
    if (!result) strcpy(context->id, id);
    return result;
}

/* APK uses flock on its writer lock. Hold a shared lock while snapshotting or
 * inspecting a failed commit, so an orphaned live APK cannot race recovery. */
static int database_lock(struct update_context *context)
{
    char path[PATH_MAX]; int result = update_path(path, context->root, "lib/apk/db/lock");
    if (result) return result;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    if (flock(fd, LOCK_SH | LOCK_NB)) { result = errno == EWOULDBLOCK ? -EBUSY : -errno; close(fd); return result; }
    return fd;
}

static int target_matches(struct update_context *context)
{
    char current[65]; int result = update_target_hash(context, current);
    if (!result && strcmp(current, context->state.target)) result = -ESTALE;
    if (result == -ESTALE) fprintf(stderr, "[reliefos-update] installed packages, world, repositories or trust changed; prepare a new plan\n");
    return result;
}

static int policy_matches(struct update_context *context)
{
    char current[65]; int result = update_policy_hash(context, current);
    if (!result && strcmp(current, context->state.policy)) result = -ESTALE;
    if (result == -ESTALE)
        fputs("[reliefos-update] world, repository policy or trust changed since preparation; reconcile these changes before continuing the frozen transaction\n", stderr);
    return result;
}

static int snapshot(struct update_context *context)
{
    int database = database_lock(context); if (database < 0) return database;
    int result = update_target_hash(context, context->state.target);
    if (!result) result = update_policy_hash(context, context->state.policy);
    const char *members[] = {"etc/apk", "lib/apk/db", "lib/apk/repositories.d"};
    for (size_t i = 0; !result && i < sizeof(members)/sizeof(members[0]); ++i) {
        char source[PATH_MAX], destination[PATH_MAX], suffix[128];
        result = update_path(source, context->root, members[i]);
        snprintf(suffix, sizeof(suffix), "snapshot/%s", members[i]);
        if (!result) result = update_path(destination, context->plan, suffix);
        if (!result) result = update_copy_tree(source, destination);
        if (result == -ENOENT && i == 2) result = 0;
    }
    if (!result) result = target_matches(context);
    close(database); return result;
}

static int new_plan(struct update_context *context)
{
    int active = update_active(context, 0);
    if (!active) {
        int result = update_state_load(context);
        if (result) return result;
        if (!terminal(context->state.phase)) {
            fprintf(stderr, "[reliefos-update] unfinished transaction %s (%s); use status, resume or repair\n", context->id, context->state.phase);
            return -EBUSY;
        }
    } else if (active != -ENOENT) return active;
    memset(&context->state, 0, sizeof(context->state));
    char id[65], plans[PATH_MAX], source_apk[PATH_MAX], source_bundle[PATH_MAX];
    int result = update_random_id(id);
    if (!result) result = update_path(plans, context->base, "plans");
    if (!result) result = make_directory(plans);
    if (!result) result = update_path(context->plan, plans, id);
    if (!result && mkdir(context->plan, 0700)) result = -errno;
    if (!result) {
        int fd = update_directory(plans, 0);
        if (fd < 0) result = fd;
        else { if (fsync(fd)) result = -errno; close(fd); }
    }
    if (!result) strcpy(context->id, id);
    char tools[PATH_MAX];
    if (!result) result = update_path(tools, context->plan, "tools");
    if (!result) result = make_directory(tools);
    if (!result) result = update_path(context->cache, context->plan, "cache");
    if (!result) result = make_directory(context->cache);
    if (!result) result = update_path(source_apk, context->root, "sbin/apk");
    if (!result) result = update_path(context->apk, tools, "apk.static");
    if (!result) result = update_copy(source_apk, context->apk, 0700);
    if (!result) {
        char source[PATH_MAX], recovery[PATH_MAX];
        result = update_path(source, context->root, "usr/sbin/reliefos-update");
        if (!result) result = update_path(recovery, tools, "reliefos-update");
        if (!result) result = update_copy(source, recovery, 0700);
    }
    if (!result) result = update_path(source_bundle, context->root, "usr/libexec/reliefos/kernel-bundle");
    if (!result && !update_regular(source_bundle, UPDATE_METADATA_MAX)) {
        result = update_path(context->bundle, tools, "kernel-bundle");
        if (!result) result = update_copy(source_bundle, context->bundle, 0700);
    }
    if (!result) result = snapshot(context);
    if (!result) result = update_sources(context);
    if (!result) result = update_state_save(context, "created");
    if (!result) result = update_active(context, 1);
    return result;
}

static int simulation(struct update_context *context, const char *output, int repairing)
{
    char root[PATH_MAX]; int result = update_path(root, context->plan, "snapshot");
    const char *arguments[] = {"upgrade", "--no-self-upgrade", "--simulate", NULL};
    if (!result) result = update_apk(context, repairing ? context->root : root, 0, output, arguments);
    return result;
}

static int kernel_changed(struct update_context *context)
{
    char path[PATH_MAX], *bytes; size_t size;
    int result = update_path(path, context->plan, "preview");
    if (!result) result = update_read(path, &bytes, &size, UPDATE_OUTPUT_MAX);
    if (result) return result;
    int changed = strstr(bytes, "Upgrading reliefos-kernel (") || strstr(bytes, "Installing reliefos-kernel (") ||
        strstr(bytes, "Downgrading reliefos-kernel (") || strstr(bytes, "Reinstalling reliefos-kernel (");
    free(bytes); return changed;
}

static int boot_preflight(struct update_context *context)
{
    return update_kernel_preflight(context);
}

static int defer_activation(struct update_context *context, int enabled)
{
    static const char marker[] = "ReliefOS online update transaction\n";
    char path[PATH_MAX]; int result = update_path(path, context->root, "run/reliefos-kernel-activation.defer");
    if (result) return result;
    char *existing; size_t size;
    result = update_read(path, &existing, &size, 128);
    if (!result) {
        if (size != sizeof(marker)-1 || memcmp(existing, marker, size)) result = -EBUSY;
        free(existing);
    } else if (result == -ENOENT) result = enabled ? 0 : 1;
    if (result) return result == 1 ? 0 : result;
    if (enabled) return update_write(path, marker, sizeof(marker)-1, 0600);
    return unlink(path) ? -errno : 0;
}

static void whitespace(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\n' || **cursor == '\t' || **cursor == '\r') ++*cursor;
}

/* Parse only APK's deliberately requested three-field JSON record. Unknown
 * keys, escapes, duplicate records, missing values and overflow fail closed. */
static int package_sizes(const char *bytes, const char *identifier, uint64_t *installed, uint64_t *archive)
{
    const char *cursor = bytes; whitespace(&cursor);
    if (*cursor++ != '[') return -EINVAL;
    whitespace(&cursor); if (*cursor++ != '{') return -EINVAL;
    unsigned seen = 0;
    for (;;) {
        whitespace(&cursor); if (*cursor++ != '"') return -EINVAL;
        const char *key = cursor; while (*cursor && *cursor != '"') ++cursor;
        size_t length = (size_t)(cursor - key); if (*cursor++ != '"') return -EINVAL;
        whitespace(&cursor); if (*cursor++ != ':') return -EINVAL; whitespace(&cursor);
        unsigned bit;
        if (length == 7 && !memcmp(key, "package", 7)) {
            bit = 1; if (*cursor++ != '"') return -EINVAL;
            size_t size = strlen(identifier);
            if (strncmp(cursor, identifier, size) || cursor[size] != '"') return -EINVAL;
            cursor += size + 1;
        } else {
            uint64_t *output;
            if (length == 14 && !memcmp(key, "installed-size", 14)) { bit = 2; output = installed; }
            else if (length == 9 && !memcmp(key, "file-size", 9)) { bit = 4; output = archive; }
            else return -EINVAL;
            if (*cursor < '0' || *cursor > '9') return -EINVAL;
            uint64_t value = 0;
            do {
                unsigned digit = (unsigned)(*cursor++ - '0');
                if (value > (UINT64_MAX - digit)/10) return -EOVERFLOW;
                value = value*10 + digit;
            } while (*cursor >= '0' && *cursor <= '9');
            *output = value;
        }
        if (seen & bit) return -EINVAL;
        seen |= bit;
        whitespace(&cursor);
        if (*cursor == '}') { ++cursor; break; }
        if (*cursor++ != ',') return -EINVAL;
    }
    whitespace(&cursor); if (*cursor++ != ']') return -EINVAL;
    whitespace(&cursor); return !*cursor && seen == 7 ? 0 : -EINVAL;
}

static int space_preflight(struct update_context *context)
{
    struct dirent **entries; int count = scandir(context->cache, &entries, NULL, alphasort);
    if (count < 0) return -errno;
    uint64_t required = UPDATE_RESERVE; int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name; size_t length = strlen(name);
        if (!result && length > 13 && !strcmp(name + length - 4, ".apk")) {
            char identifier[NAME_MAX + 1], output[PATH_MAX], *bytes; size_t size;
            memcpy(identifier, name, length - 13); identifier[length - 13] = 0;
            const char *suffix = name + length - 13;
            if (*suffix != '.' || strspn(suffix + 1, "0123456789abcdef") != 8 ||
                strspn(identifier, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(identifier)) result = -EINVAL;
            if (!result) result = update_path(output, context->plan, "package-size");
            const char *query[] = {"query", "--from", "repositories", "--match", "package", "--all-matches", "--format", "json",
                                   "--fields", "package,installed-size,file-size", identifier, NULL};
            char solver[PATH_MAX]; if (!result) result = update_path(solver, context->plan, "snapshot");
            if (!result) result = update_apk(context, solver, 0, output, query);
            if (!result) result = update_read(output, &bytes, &size, 8192);
            uint64_t installed = 0, archive = 0;
            if (!result) { result = package_sizes(bytes, identifier, &installed, &archive); free(bytes); }
            if (!result && installed > UINT64_MAX-required) result = -EOVERFLOW;
            if (!result) required += installed;
        }
        free(entries[i]);
    }
    free(entries);
    char modules[PATH_MAX]; uint64_t retained = 0;
    if (!result) result = update_path(modules, context->root, "lib/modules");
    if (!result) { result = update_tree_bytes(modules, &retained); if (result == -ENOENT) result = 0; }
    if (!result) result = update_path(modules, context->root, "usr/lib/reliefos/kernel");
    if (!result) { result = update_tree_bytes(modules, &retained); if (result == -ENOENT) result = 0; }
    if (!result && retained > UINT64_MAX-required) result = -EOVERFLOW;
    if (!result) result = update_space(context->root, required + retained);
    if (!result) result = update_space(context->plan, UPDATE_RESERVE);
    if (!result && context->state.reboot) {
        char extracted[PATH_MAX], boot[PATH_MAX]; uint64_t incoming = 0;
        result = update_path(extracted, context->plan, "kernel-extracted/usr/lib/reliefos/kernel");
        if (!result) result = update_tree_bytes(extracted, &incoming);
        if (!result) result = update_path(boot, context->root, "boot");
        if (!result && incoming > UINT64_MAX-UPDATE_RESERVE) result = -EOVERFLOW;
        if (!result) result = update_space(boot, incoming + UPDATE_RESERVE);
    }
    return result;
}

int update_prepare(struct update_context *context, int resume)
{
    int result;
    if (!resume) result = new_plan(context);
    else result = update_state_load(context);
    if (result) return result;
    if (resume && !strcmp(context->state.phase, "ready")) {
        result = target_matches(context);
        if (!result) {
            char seal[65]; result = update_plan_seal(context, seal);
            if (!result && strcmp(seal, context->state.seal)) result = -EBADMSG;
        }
        if (!result) printf("plan_id=%s\n", context->id);
        return result;
    }
    if (resume && strcmp(context->state.phase, "created") && strcmp(context->state.phase, "indexes-failed") &&
        strcmp(context->state.phase, "indexes-ready") && strcmp(context->state.phase, "downloading") &&
        strcmp(context->state.phase, "download-failed") && strcmp(context->state.phase, "verifying")) return -EINVAL;
    result = target_matches(context);
    char solver[PATH_MAX], output[PATH_MAX];
    if (!result) result = update_path(solver, context->plan, "snapshot");
    if (!result) result = update_space(context->plan, UPDATE_RESERVE);
    if (!result && (!strcmp(context->state.phase, "created") || !strcmp(context->state.phase, "indexes-failed"))) {
        update_event(context, "refreshing-signed-indexes");
        result = update_path(output, context->plan, "index.log");
        const char *refresh[] = {"update", NULL};
        if (!result) result = update_apk(context, solver, 1, output, refresh);
        if (!result) result = update_cache_verify(context, 1);
        if (!result) result = update_state_save(context, "indexes-ready");
        if (result) { (void)update_state_save(context, "indexes-failed"); return result; }
    }
    if (!result) result = update_cache_verify(context, 0);
    if (!result) result = update_state_save(context, "downloading");
    update_event(context, "downloading-exact-upgrade");
    if (!result) result = update_path(output, context->plan, "download.log");
    const char *download[] = {"cache", "--upgrade", "download", NULL};
    /* Retry the native cache operation: already validated complete archives
     * remain reusable; a failed attempt never mutates the installed database. */
    if (!result) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            result = update_apk(context, solver, 1, output, download);
            if (!result || update_cancelled) break;
        }
    }
    if (result) { (void)update_state_save(context, "download-failed"); return result; }
    result = update_state_save(context, "verifying");
    if (!result) result = update_cache_verify(context, 0);
    if (!result) result = update_path(output, context->plan, "preview");
    if (!result) result = simulation(context, output, 0);
    if (!result) {
        int changed = kernel_changed(context);
        if (changed < 0) result = changed;
        else context->state.reboot = changed;
    }
    if (!result && context->state.reboot) result = update_kernel_check(context, 0);
    if (!result && context->state.reboot) result = boot_preflight(context);
    if (!result) result = space_preflight(context);
    if (!result) result = target_matches(context);
    if (!result) result = update_plan_seal(context, context->state.seal);
    if (!result) result = update_state_save(context, "ready");
    if (!result) {
        update_event(context, "plan-ready");
        printf("plan_id=%s\n", context->id); fflush(stdout);
    } else if (result == -ESTALE) (void)update_state_save(context, "stale");
    return result;
}

static int plan_valid(struct update_context *context)
{
    char digest[65]; int result = update_plan_seal(context, digest);
    if (!result && strcmp(digest, context->state.seal)) result = -EBADMSG;
    return result;
}

static int repair_broken(struct update_context *context)
{
    char recovery_names[2][NAME_MAX + 1] = {{0}};
    const char *arguments[134] = {"fix", "--reinstall", "--upgrade"}; size_t used = 3;
    const char *members[] = {"/sbin/apk", "/usr/sbin/reliefos-update"};
    /* Bootstrap copies must never be mistaken for the final package version.
     * Reinstall their real owners from the frozen signed cohort on every
     * repair, including recovery interrupted after restoring those copies. */
    for (size_t i = 0; i < 2; ++i) {
        char report[PATH_MAX], *record; size_t length;
        int result = update_path(report, context->plan, "recovery-owner");
        const char *query[] = {"query", "--from", "installed", "--match", "owner", "--format", "json", "--fields", "name", members[i], NULL};
        if (!result) result = update_apk(context, context->root, 0, report, query);
        if (!result) result = update_read(report, &record, &length, 1024);
        if (result) return result;
        const char *at = record; whitespace(&at);
        if (*at++ != '[') result = -EINVAL;
        if (!result) whitespace(&at);
        if (!result && *at == ']' && context->isolated) { ++at; whitespace(&at); if (*at) result = -EINVAL; }
        else if (!result) {
            if (*at++ != '{') result = -EINVAL;
            if (!result) whitespace(&at);
            if (!result && strncmp(at, "\"name\"", 6)) result = -EINVAL;
            if (!result) { at += 6; whitespace(&at); if (*at++ != ':') result = -EINVAL; }
            if (!result) whitespace(&at);
            if (!result && *at++ != '"') result = -EINVAL;
            if (!result) {
                size_t size = strspn(at, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-");
                if (!size || size > NAME_MAX || at[size] != '"') result = -EINVAL;
                else { memcpy(recovery_names[i], at, size); at += size + 1; }
            }
            if (!result) { whitespace(&at); if (*at++ != '}') result = -EINVAL; }
            if (!result) { whitespace(&at); if (*at++ != ']') result = -EINVAL; }
            if (!result) { whitespace(&at); if (*at) result = -EINVAL; }
        }
        free(record);
        if (result) return result;
        if (*recovery_names[i] && (!i || strcmp(recovery_names[0], recovery_names[i]))) arguments[used++] = recovery_names[i];
    }
    char path[PATH_MAX], *bytes; size_t size; int result = update_path(path, context->root, "lib/apk/db/installed");
    if (!result) result = update_read(path, &bytes, &size, UPDATE_METADATA_MAX);
    if (result) return result;
    char *name = NULL, *save = NULL;
    for (char *line = strtok_r(bytes, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "P:", 2)) name = line + 2;
        if (!strncmp(line, "f:", 2) && strpbrk(line + 2, "fs")) {
            if (!name || !*name || strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(name) || used >= 130) {
                result = -E2BIG; break;
            }
            int duplicate = 0;
            for (size_t i = 3; i < used; ++i) if (!strcmp(arguments[i], name)) duplicate = 1;
            if (!duplicate) arguments[used++] = name;
        }
    }
    arguments[used] = NULL;
    if (!result && used > 3) result = update_apk(context, context->root, 0, NULL, arguments);
    free(bytes); return result;
}

static int restore_recovery_programs(struct update_context *context)
{
    const char *members[] = {"sbin/apk", "usr/sbin/reliefos-update"};
    const char *saved[] = {"tools/apk.static", "tools/reliefos-update"};
    for (size_t i = 0; i < sizeof(members)/sizeof(members[0]); ++i) {
        char destination[PATH_MAX], source[PATH_MAX], expected[65], actual[65]; int changed = 0;
        int result = update_path(destination, context->root, members[i]);
        if (!result) result = update_path(source, context->plan, saved[i]);
        if (!result) result = update_regular(source, UPDATE_METADATA_MAX);
        if (!result) result = update_hash(source, expected);
        if (result) return result;
        result = update_regular(destination, UPDATE_METADATA_MAX);
        if (!result) result = update_hash(destination, actual);
        else if (result == -ENOENT) actual[0] = 0;
        else return result;
        if (result && result != -ENOENT) return result;
        if (!actual[0] || strcmp(actual, expected)) {
            /* APK may already have installed a newer updater successfully.
             * Keep runnable replacements; the saved copy is only a bootstrap
             * for missing or broken commands, never an implicit downgrade. */
            if (actual[0]) {
                char *probe[] = {destination, i ? "--help" : "--version", NULL};
                context->timeout_seconds = 5;
                result = update_run(context, probe, NULL, 0);
                context->timeout_seconds = 0;
                if (!result) continue;
                if (update_cancelled) return -ECANCELED;
            }
            result = update_copy(source, destination, 0755);
            changed = !result;
        }
        else result = 0;
        if (result) return result;
        if (changed) fprintf(stderr, "[reliefos-update] restored recovery executable %s from the sealed plan\n", members[i]);
    }
    return 0;
}

int update_apply(struct update_context *context, int repair)
{
    int result = update_state_load(context); if (result) return result;
    char active[PATH_MAX], *record; size_t record_size;
    result = update_path(active, context->base, "active");
    if (!result) result = update_read(active, &record, &record_size, 65);
    if (!result) {
        if (record_size != 65 || record[64] != '\n' || memcmp(record, context->id, 64)) result = -ESTALE;
        free(record);
    }
    if (result) return result;
    if (!strcmp(context->state.phase, "complete")) {
        printf("result=success\nreboot_required=%d\n", context->state.reboot); return 0;
    }
    if (!repair && strcmp(context->state.phase, "ready")) return -EINVAL;
    if (repair && strcmp(context->state.phase, "committing") && strcmp(context->state.phase, "apply-failed") &&
        strcmp(context->state.phase, "repairing") && strcmp(context->state.phase, "verify-failed")) return -EINVAL;
    result = plan_valid(context);
    if (!result && repair) result = policy_matches(context);
    if (!result && !repair) result = target_matches(context);
    if (!result) result = update_cache_verify(context, 0);
    if (!result) result = space_preflight(context);
    if (!result && context->state.reboot && !repair) result = boot_preflight(context);
    if (result) return result;
    int database = database_lock(context); if (database < 0) return database;
    if (repair) {
        result = policy_matches(context);
        if (!result) result = update_target_hash(context, context->state.target);
    }
    else result = target_matches(context);
    if (!result) result = update_state_save(context, repair ? "repairing" : "committing");
    if (!result && context->state.reboot) result = defer_activation(context, 1);
    close(database);
    if (result) return result;
    context->committing = 1;
    update_event(context, repair ? "repairing-packages" : "committing-packages");
    const char *upgrade[] = {"upgrade", "--no-self-upgrade", NULL};
    char output[PATH_MAX]; result = update_path(output, context->plan, "commit.log");
    if (repair) {
        result = restore_recovery_programs(context);
        if (!result) result = repair_broken(context);
        if (!result) {
            database = database_lock(context);
            if (database < 0) result = database;
            else {
                result = policy_matches(context);
                if (!result) result = update_target_hash(context, context->state.target);
                close(database);
            }
        }
        if (!result) result = update_state_save(context, "repairing");
    }
    if (!result) result = update_apk(context, context->root, 0, output, upgrade);
    if (result) {
        if (!repair || !policy_matches(context)) (void)update_target_hash(context, context->state.target);
        (void)update_state_save(context, "apply-failed");
        fputs("[reliefos-update] package transaction failed; installed userspace may be partly updated. Keep this plan and run reliefos-update repair.\n", stderr);
        context->committing = 0; return result;
    }
    /* Flush the actual installed products before making a new boot pair
     * visible. APK's database directory alone is not the installed system. */
    int root_fd = update_directory(context->root, 0);
    if (root_fd < 0) result = root_fd;
    else { if (syncfs(root_fd)) result = -errno; close(root_fd); }
    if (!result && context->state.reboot) {
        result = update_kernel_check(context, 1);
        if (!result) result = update_kernel_publish(context);
        if (!result) result = defer_activation(context, 0);
    }
    if (!result) {
        char path[PATH_MAX]; result = update_path(path, context->root, "lib/apk/db");
        int directory = result ? -1 : update_directory(path, 0);
        if (directory < 0 && !result) result = directory;
        if (directory >= 0) { if (fsync(directory)) result = -errno; close(directory); }
    }
    if (!result) result = update_state_save(context, "complete");
    if (result) (void)update_state_save(context, "verify-failed");
    context->committing = 0;
    if (!result) {
        printf("result=success\nreboot_required=%d\n", context->state.reboot); fflush(stdout);
    }
    return result;
}

int update_status(struct update_context *context)
{
    int result = update_active(context, 0);
    if (result == -ENOENT) { puts("phase=idle\nreboot_required=0"); return 0; }
    if (!result) result = update_state_load(context);
    if (!result) printf("plan_id=%s\nphase=%s\nreboot_required=%d\n", context->id, context->state.phase, context->state.reboot);
    return result;
}

int update_discard(struct update_context *context)
{
    int result = update_active(context, 0);
    if (!result) result = update_state_load(context);
    if (result) return result;
    if (!strcmp(context->state.phase, "committing") || !strcmp(context->state.phase, "repairing") ||
        !strcmp(context->state.phase, "apply-failed") || !strcmp(context->state.phase, "verify-failed")) {
        fputs("[reliefos-update] package installation has started; this transaction requires repair and cannot be discarded\n", stderr);
        return -EBUSY;
    }
    if (!strcmp(context->state.phase, "complete")) return -EINVAL;
    result = update_state_save(context, "stale");
    if (!result) update_event(context, "uncommitted-plan-discarded");
    return result;
}

/* Runs inside APK's own write lock, closing the last check-to-commit race.
 * This handler performs no mutation, so it is also safe in Linux QA chroots. */
int update_guard(struct update_context *context)
{
    int result = update_active(context, 0);
    if (result == -ENOENT) return 0;
    if (!result) result = update_state_load(context);
    if (result) return result;
    int pending = !strcmp(context->state.phase, "committing") || !strcmp(context->state.phase, "repairing") ||
        !strcmp(context->state.phase, "apply-failed") || !strcmp(context->state.phase, "verify-failed");
    if (!pending) return 0;
    const char *authorized = getenv("RELIEFOS_UPDATE_PLAN");
    if (!authorized || strcmp(authorized, context->id)) {
        fputs("[reliefos-update] an unfinished system update owns APK writes; use reliefos-update repair\n", stderr);
        return -EBUSY;
    }
    result = policy_matches(context);
    return result ? result : target_matches(context);
}
