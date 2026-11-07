/* Exercise the public directory adapter against real Linux getdents cookies. */
#include <reliefos/fs.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    const char *names[] = {"a", "second.txt", "中文文件.txt", "subdir", "link", "fifo"};
    for (unsigned n = 0; n < 3; ++n) {
        int fd = open(names[n], O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0 && close(fd) == 0);
    }
    assert(mkdir(names[3], 0700) == 0 && symlink(names[0], names[4]) == 0);
    assert(mkfifo(names[5], 0600) == 0);
    unsigned expected[] = {RELIEFOS_FS_TYPE_FILE, RELIEFOS_FS_TYPE_FILE,
        RELIEFOS_FS_TYPE_FILE, RELIEFOS_FS_TYPE_DIR, RELIEFOS_FS_TYPE_SYMLINK,
        RELIEFOS_FS_TYPE_FIFO};
    int fd = open(".", O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    int alias = dup(fd);
    assert(alias >= 0);
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned seen = 0, count = 0;
        struct reliefos_dir_entry entry;
        int result;
        while ((result = reliefos_readdir(count & 1u ? alias : fd, &entry)) > 0) {
            ++count;
            if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
            unsigned n;
            for (n = 0; n < 6 && strcmp(entry.name, names[n]); ++n) {}
            assert(n < 6 && !(seen & (1u << n)) && entry.type == expected[n]);
            seen |= 1u << n;
        }
        assert(result == 0 && seen == 63 && count == 8);
        assert(reliefos_readdir(fd, &entry) == 0);
        assert(lseek(fd, 0, SEEK_SET) == 0);
    }
    assert(close(alias) == 0);
    assert(close(fd) == 0);
    struct reliefos_dir_entry entries[16];
    uint32_t count = 99;
    assert(reliefos_list_dir(".", entries, 16, &count) == 0 && count == 8);
    assert(reliefos_list_dir(".", NULL, 0, &count) == 0 && count == 0);
    assert(reliefos_list_dir(NULL, entries, 16, &count) < 0 && count == 0);
    fd = open("a", O_RDONLY);
    assert(fd >= 0 && reliefos_readdir(fd, entries) == -ENOTDIR);
    assert(close(fd) == 0);
    assert(reliefos_readdir(-1, entries) == -EBADF);
    assert(reliefos_readdir(-1, NULL) == -EFAULT);
    char long_name[130];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    fd = open(long_name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    fd = open(".", O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    int result;
    while ((result = reliefos_readdir(fd, entries)) > 0) {}
    assert(result == -ENAMETOOLONG && errno == ENAMETOOLONG);
    assert(close(fd) == 0);
    puts("PASS directory adapter: real entries, UTF-8, types, cookies, rewind, EOF and errno");
    return 0;
}
