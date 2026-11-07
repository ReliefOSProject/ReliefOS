#include "repositories.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}
int main() {
    ra::Repositories r;
    ra::Text out, error;
    const char *source = "# 注释\nhttps://example.org/main\n@testing https://example.org/testing\n# "
                         "https://example.org/off\nunknown syntax keep\n";
    assert(r.parse(source, strlen(source)));
    assert(r.lines.size() == 5);
    assert(r.lines[1].recognized && r.lines[1].enabled);
    assert(!strcmp(r.lines[2].tag.c_str(), "testing") && !r.lines[3].enabled);
    assert(r.serialize(out) && !strcmp(out.c_str(), source));
    assert(r.replace(1, "stable", "https://example.org/new", false));
    assert(r.serialize(out));
    assert(strstr(out.c_str(), "# @stable https://example.org/new\n"));
    assert(strstr(out.c_str(), "unknown syntax keep\n"));
    assert(!r.add("bad tag", "https://example.org/main"));
    assert(!r.add("", "https://example.org/a\nhttps://evil"));
    assert(!ra::valid_repository("", "javascript:evil"));
    assert(ra::valid_repository("local", "/usr/share/reliefos/apk/repository"));
    assert(ra::standard_repository_path("/etc/apk/repositories"));
    assert(ra::standard_repository_path("/etc/apk/repositories.d/local.list"));
    assert(!ra::standard_repository_path("/etc/apk/repositories.d/../sudoers.list"));
    assert(!ra::standard_repository_path("/etc/sudoers"));
    char temp[] = "/tmp/reliefapk-repositories-XXXXXX";
    // The shell runner sets TMPDIR; mkdtemp needs a template using it.
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/repos-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    (void)temp;
    assert(mkdtemp(dir));
    char path[1200], link[1200], backup[1250];
    snprintf(path, sizeof(path), "%s/repositories", dir);
    snprintf(link, sizeof(link), "%s/link", dir);
    snprintf(backup, sizeof(backup), "%s.reliefapk.bak", path);
    struct stat root_st;
    assert(!stat("/", &root_st));
    if (root_st.st_uid == 0 && !(root_st.st_mode & 0022))
        assert(ra::secure_cache_directory("/", error));
    assert(!ra::secure_cache_directory(dir, error));
    put(path, source);
    assert(!chmod(path, 0640));
    assert(ra::save_repository_file(path, source, "https://example.org/new\n", error));
    assert(r.load(path) && !strcmp(r.original.c_str(), "https://example.org/new\n"));
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0640);
    assert(r.load(backup) && !strcmp(r.original.c_str(), source));
    put(path, "external\n");
    assert(!ra::save_repository_file(path, "https://example.org/new\n", "replacement\n", error));
    assert(r.load(path) && !strcmp(r.original.c_str(), "external\n"));
    assert(!symlink(path, link));
    assert(!ra::save_repository_file(link, "external\n", "replacement\n", error));
    const char invalid[] = {'a', 0, 'b'};
    assert(!r.parse(invalid, 3));
    unlink(link);
    unlink(path);
    unlink(backup);
    assert(!rmdir(dir));
    puts(
        "ReliefAPK repositories: preservation, validation, atomic backup, conflict and symlink tests passed");
}
