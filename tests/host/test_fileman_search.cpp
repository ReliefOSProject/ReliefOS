#include "search.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static void join_path(char *destination, size_t capacity, const char *parent,
                      const char *name)
{
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    assert(parent_length + name_length + 2 <= capacity);
    memcpy(destination, parent, parent_length);
    destination[parent_length] = '/';
    memcpy(destination + parent_length + 1, name, name_length + 1);
}

static void make_dir(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void put_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    size_t length = strlen(contents);
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, contents + written, length - written);
        assert(result > 0);
        written += static_cast<size_t>(result);
    }
    assert(close(fd) == 0);
}

static void put_file_at(int dirfd, const char *name, const char *contents)
{
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    size_t length = strlen(contents);
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, contents + written, length - written);
        assert(result > 0);
        written += static_cast<size_t>(result);
    }
    assert(close(fd) == 0);
}

static void finish(FileSearch *search)
{
    unsigned long previous = 0;
    while (search->running()) {
        unsigned long before = search->scanned();
        search->step(32);
        assert(search->scanned() >= before);
        assert(++previous < 1000000UL);
    }
}

static unsigned find_suffix(const FileSearch *search, const char *suffix,
                            bool *directory = 0)
{
    size_t suffix_length = strlen(suffix);
    for (unsigned i = 0; i < search->result_count(); ++i) {
        const FileSearchResult *result = search->result(i);
        size_t path_length = strlen(result->path);
        if (path_length >= suffix_length &&
            !memcmp(result->path + path_length - suffix_length, suffix,
                    suffix_length)) {
            if (directory) *directory = result->directory;
            return i;
        }
    }
    return search->result_count();
}

static unsigned count_open_fds(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != 0) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    }
    assert(closedir(directory) == 0);
    return count;
}

static void test_recursive_current_and_names(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "basic");
    make_dir(root);
    char nested[512];
    join_path(nested, sizeof(nested), root, "sub");
    make_dir(nested);
    char path[512];
    join_path(path, sizeof(path), root, "alpha-report.TXT");
    put_file(path, "current");
    join_path(path, sizeof(path), nested, "中文报告.txt");
    put_file(path, "unicode");
    join_path(path, sizeof(path), nested, "no-match.bin");
    put_file(path, "other");
    join_path(path, sizeof(path), root, "needle-parent");
    make_dir(path);
    char same_parent[512];
    join_path(same_parent, sizeof(same_parent), path, "same.txt");
    put_file(same_parent, "same one");
    join_path(nested, sizeof(nested), path, "not-a-match");
    make_dir(nested);
    join_path(path, sizeof(path), nested, "same.txt");
    put_file(path, "same two");

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "ALPHA", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/alpha-report.TXT") < search.result_count());
    assert(!search.result(0)->directory);

    options.ignore_ascii_case = false;
    assert(search.start(root, "ALPHA", options) == 0);
    finish(&search);
    assert(search.result_count() == 0);

    options.ignore_ascii_case = true;
    assert(search.start(root, "中文报", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/中文报告.txt") < search.result_count());

    options.recursive = false;
    assert(search.start(root, "中文报", options) == 0);
    finish(&search);
    assert(search.result_count() == 0);
    assert(find_suffix(&search, "/needle-parent/not-a-match") ==
           search.result_count());

    options.recursive = true;
    assert(search.start(root, "needle", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    bool directory = false;
    assert(find_suffix(&search, "/needle-parent", &directory) <
           search.result_count());
    assert(directory);

    assert(search.start(root, "same", options) == 0);
    finish(&search);
    assert(search.result_count() == 2);
    assert(find_suffix(&search, "/needle-parent/not-a-match/same.txt") <
           search.result_count());
    assert(find_suffix(&search, "/needle-parent/same.txt") <
           search.result_count());
    assert(strcmp(search.result(0)->path, search.result(1)->path));

    char root_alias[512];
    join_path(root_alias, sizeof(root_alias), base, "basic/../basic");
    assert(search.start(root_alias, "alpha", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(strstr(search.result(0)->path, "/../") == 0);
}

static void test_hidden_entries(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "hidden");
    make_dir(root);
    char path[512];
    join_path(path, sizeof(path), root, ".secret.txt");
    put_file(path, "hidden file");
    join_path(path, sizeof(path), root, ".private");
    make_dir(path);
    char hidden_parent[512];
    join_path(hidden_parent, sizeof(hidden_parent), root, ".private");
    join_path(path, sizeof(path), hidden_parent, "inside-secret.txt");
    put_file(path, "hidden child");

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "secret", options) == 0);
    finish(&search);
    assert(search.result_count() == 0);

    options.show_hidden = true;
    assert(search.start(root, "secret", options) == 0);
    finish(&search);
    assert(search.result_count() == 2);
    assert(find_suffix(&search, "/.secret.txt") < search.result_count());
    assert(find_suffix(&search, "/.private/inside-secret.txt") <
           search.result_count());
}

static void test_more_than_directory_listing_capacity(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "many");
    make_dir(root);
    char path[512];
    char name[64];
    for (unsigned i = 0; i < 80; ++i) {
        snprintf(name, sizeof(name), "match-%03u.txt", i);
        join_path(path, sizeof(path), root, name);
        put_file(path, "match");
    }

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "MATCH-", options) == 0);
    finish(&search);
    assert(search.result_count() == 80);
    assert(!search.limited());
}

static void test_symlink_entries_are_not_traversed(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "link-scope");
    make_dir(root);
    char inside[512];
    join_path(inside, sizeof(inside), root, "inside");
    make_dir(inside);
    char outside[512];
    join_path(outside, sizeof(outside), base, "outside");
    make_dir(outside);
    char path[512];
    join_path(path, sizeof(path), outside, "needle-external.txt");
    put_file(path, "external");
    join_path(path, sizeof(path), inside, "loop");
    assert(symlink("..", path) == 0);
    join_path(path, sizeof(path), inside, "external");
    assert(symlink(outside, path) == 0);
    join_path(path, sizeof(path), inside, "needle-inside.txt");
    put_file(path, "inside");

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "needle", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/needle-inside.txt") < search.result_count());

    assert(search.start(root, "loop", options) == 0);
    finish(&search);
    bool directory = true;
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/loop", &directory) < search.result_count());
    assert(!directory);

    assert(search.start(root, "external", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/external", &directory) < search.result_count());
    assert(!directory);
}

static void test_cancel_restart_and_fd_cleanup(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "cancel");
    make_dir(root);
    char child[512];
    join_path(child, sizeof(child), root, "child");
    make_dir(child);
    char path[512];
    char name[64];
    for (unsigned i = 0; i < 24; ++i) {
        snprintf(name, sizeof(name), "keep-%02u.txt", i);
        join_path(path, sizeof(path), child, name);
        put_file(path, "keep");
    }

    FileSearch search;
    FileSearchOptions options;
    unsigned baseline = count_open_fds();
    assert(search.start(root, "keep", options) == 0);
    while (search.running() && !search.result_count()) search.step(1);
    assert(search.running());
    assert(search.result_count() > 0);
    assert(count_open_fds() > baseline);
    search.cancel();
    assert(search.canceled());
    assert(!search.running());
    assert(search.result_count() > 0);
    assert(count_open_fds() == baseline);

    assert(search.start(root, "missing", options) == 0);
    assert(search.result_count() == 0);
    assert(!search.canceled());
    finish(&search);
    assert(search.result_count() == 0);
    assert(count_open_fds() == baseline);
}

static void test_scan_result_and_depth_limits(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "limits");
    make_dir(root);
    char path[512];
    char name[64];
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(name, sizeof(name), "hit-%u", i);
        join_path(path, sizeof(path), root, name);
        put_file(path, "hit");
    }
    char nested[512];
    join_path(nested, sizeof(nested), root, "child");
    make_dir(nested);
    join_path(path, sizeof(path), nested, "hit-deeper");
    put_file(path, "hit");

    FileSearch search;
    FileSearchOptions options;
    options.max_results = 2;
    assert(search.start(root, "hit", options) == 0);
    finish(&search);
    assert(search.result_count() == 2);
    assert(search.limited());

    options.max_results = 9;
    assert(search.start(root, "hit", options) == 0);
    finish(&search);
    assert(search.result_count() == 9);
    assert(!search.limited());

    options.max_results = 0;
    assert(search.start(root, "hit", options) == 0);
    finish(&search);
    assert(search.result_count() == 0);
    assert(search.limited());

    options.max_results = UINT_MAX;
    assert(search.start(root, "hit-deeper", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(!search.limited());

    options.max_results = 1000;
    options.max_scanned = 4;
    assert(search.start(root, "never", options) == 0);
    finish(&search);
    assert(search.limited());
    assert(search.scanned() == 4);

    options.max_scanned = 1000000UL;
    options.max_depth = 0;
    assert(search.start(root, "hit-deeper", options) == 0);
    finish(&search);
    assert(search.result_count() == 0);
    assert(!search.limited());
    assert(search.skipped() > 0);
}

static void test_path_length_boundary(const char *base)
{
    char parent[512];
    join_path(parent, sizeof(parent), base, "path-boundary");
    make_dir(parent);
    size_t parent_length = strlen(parent);
    assert(parent_length + 1 < 251);
    size_t component_length = 251 - parent_length - 1;
    assert(component_length < 256);
    char component[256];
    memset(component, 'r', component_length);
    component[component_length] = 0;
    char root[512];
    join_path(root, sizeof(root), parent, component);
    make_dir(root);
    assert(strlen(root) == 251);
    int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(rootfd >= 0);
    put_file_at(rootfd, "hit", "boundary");
    put_file_at(rootfd, "hita", "too long");
    assert(close(rootfd) == 0);

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "hit", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(strlen(search.result(0)->path) == 255);
    assert(!strcmp(strrchr(search.result(0)->path, '/') + 1, "hit"));
    assert(!search.limited());
    assert(search.skipped() > 0);

    size_t too_long_component_length = 256 - parent_length - 1;
    assert(too_long_component_length < sizeof(component));
    memset(component, 'x', too_long_component_length);
    component[too_long_component_length] = 0;
    char too_long_root[512];
    join_path(too_long_root, sizeof(too_long_root), parent, component);
    make_dir(too_long_root);
    assert(strlen(too_long_root) == 256);
    assert(search.start(too_long_root, "x", options) == -ENAMETOOLONG);
}

static void test_long_query_fails_and_clears_state(const char *base)
{
    char root[512];
    join_path(root, sizeof(root), base, "long-query");
    make_dir(root);
    char path[512];
    join_path(path, sizeof(path), root, "needle.txt");
    put_file(path, "match");

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "needle", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);

    unsigned baseline = count_open_fds();
    assert(search.start(root, "needle", options) == 0);
    assert(search.running());
    assert(search.result_count() == 0);
    assert(count_open_fds() > baseline);

    char long_query[257];
    memset(long_query, 'q', sizeof(long_query) - 1);
    long_query[sizeof(long_query) - 1] = 0;
    assert(strlen(long_query) == sizeof(FileSearchResult::path));
    assert(search.start(root, long_query, options) == -ENAMETOOLONG);
    assert(!search.running());
    assert(!search.canceled());
    assert(search.result_count() == 0);
    assert(count_open_fds() == baseline);
}

static void test_permission_skip_when_not_root(const char *base)
{
    if (geteuid() == 0) {
        puts("SKIP permission-denied search case: root bypasses directory permissions");
        return;
    }
    char root[512];
    join_path(root, sizeof(root), base, "permissions");
    make_dir(root);
    char denied[512];
    join_path(denied, sizeof(denied), root, "denied");
    make_dir(denied);
    char path[512];
    join_path(path, sizeof(path), denied, "needle-hidden.txt");
    put_file(path, "permission");
    assert(chmod(denied, 0000) == 0);
    join_path(path, sizeof(path), root, "needle-sibling.txt");
    put_file(path, "sibling");

    FileSearch search;
    FileSearchOptions options;
    assert(search.start(root, "needle", options) == 0);
    finish(&search);
    assert(search.result_count() == 1);
    assert(find_suffix(&search, "/needle-sibling.txt") < search.result_count());
    assert(search.skipped() > 0);
    assert(!search.limited());
    assert(chmod(denied, 0700) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(chdir(argv[1]) == 0);
    char base[512];
    assert(getcwd(base, sizeof(base)));

    unsigned baseline = count_open_fds();
    test_recursive_current_and_names(base);
    test_hidden_entries(base);
    test_more_than_directory_listing_capacity(base);
    test_symlink_entries_are_not_traversed(base);
    test_cancel_restart_and_fd_cleanup(base);
    test_scan_result_and_depth_limits(base);
    test_path_length_boundary(base);
    test_long_query_fails_and_clears_state(base);
    test_permission_skip_when_not_root(base);
    assert(count_open_fds() == baseline);
    char current[512];
    assert(getcwd(current, sizeof(current)));
    assert(!strcmp(current, base));
    puts("ok - recursive filename search, Unicode, hidden items, limits, links and cancellation");
    return 0;
}
