#include "archive.h"
#include "filesystem.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void content(const char *path, const char *want)
{
    char buffer[80] = {0};
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t size = fread(buffer, 1, sizeof(buffer), file);
    assert(size == strlen(want) && !memcmp(buffer, want, size));
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(mkdir("source", 0700) == 0);
    assert(mkdir("source/nested", 0700) == 0);
    assert(mkdir("source/empty", 0700) == 0);
    put("source/nested/inside.txt", "directory contents");
    put("source/-odd 'quote';$.txt", "literal filename");
    assert(chmod("source/nested/inside.txt", 0640) == 0);
    char archive[512];
    assert(getcwd(archive, sizeof(archive)));
    strcat(archive, "/packed.tar");
    const char *directory[] = {"."};
    assert(fileman_tar_create(archive, "source", directory, 1) == 0);
    assert(mkdir("extracted", 0700) == 0);
    mode_t previous_umask = umask(0077);
    assert(fileman_tar_extract(archive, "extracted") == 0);
    umask(previous_umask);
    content("extracted/nested/inside.txt", "directory contents");
    content("extracted/-odd 'quote';$.txt", "literal filename");
    struct stat st;
    assert(stat("extracted/empty", &st) == 0 && S_ISDIR(st.st_mode));
    assert(stat("extracted/nested/inside.txt", &st) == 0 && (st.st_mode & 0777) == 0640);
    const char *batch[] = {"nested", "-odd 'quote';$.txt", "empty"};
    assert(fileman_tar_create(archive, "source", batch, 3) == 0);
    assert(mkdir("batch", 0700) == 0);
    assert(fileman_tar_extract(archive, "batch") == 0);
    content("batch/nested/inside.txt", "directory contents");
    content("batch/-odd 'quote';$.txt", "literal filename");
    assert(stat("batch/empty", &st) == 0 && S_ISDIR(st.st_mode));
    assert(fileman_tar_extract("/no-such-fileman-archive", "batch") != 0);
    const char *missing[] = {"missing.txt"};
    assert(fileman_tar_create(archive, "source", missing, 1) != 0);
    assert(fileman_tar_extract(archive, "batch") == 0);
    content("batch/nested/inside.txt", "directory contents");
    put("invalid.tar", "not a tar archive");
    assert(fileman_tar_extract("invalid.tar", "batch") != 0);
    const char *self[] = {"."};
    assert(fileman_tar_create("source/self.tar", "source", self, 1) == -EINVAL);
    assert(lstat("source/self.tar", &st) < 0 && errno == ENOENT);
    const char *unsafe[] = {"../source"};
    assert(fileman_tar_create(archive, "source", unsafe, 1) == -EINVAL);
    assert(mkdir("outside", 0700) == 0);
    put("outside/keep.txt", "keep");
    assert(symlink("../outside", "extracted/link") == 0);
    assert(symlink("/no-such-fileman-target", "extracted/dangling") == 0);
    assert(fileman_remove_tree("extracted") == 0);
    assert(lstat("extracted", &st) < 0 && errno == ENOENT);
    content("outside/keep.txt", "keep");
    assert(symlink("outside", "link") == 0);
    assert(fileman_remove_tree("link/") == 0);
    content("outside/keep.txt", "keep");
    assert(fileman_remove_tree(".") == -EINVAL);
    assert(fileman_remove_tree("/") == -EINVAL);
    assert(fileman_remove_tree("///") == -EINVAL);
    assert(fileman_remove_tree("outside/..") == -EINVAL);
    content("outside/keep.txt", "keep");
    assert(fileman_remove_tree("outside/keep.txt") == 0);
    assert(lstat("outside/keep.txt", &st) < 0 && errno == ENOENT);
    assert(fileman_remove_tree("outside") == 0);
    puts("ok - real tar round trips, nested/empty directories, literal names, failures and direct symlink-safe deletion");
    return 0;
}
