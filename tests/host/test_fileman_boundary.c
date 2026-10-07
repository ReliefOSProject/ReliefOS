#include "boundary.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *invalid[] = {"", ".", "..", "a/b", "line\nname", "tab\tname", "bad\rname"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!fileman_name_valid(invalid[i]));
    assert(fileman_name_valid("New Folder"));
    assert(fileman_name_valid(".hidden"));
    assert(fileman_name_valid("a;$'b"));
    char path[32];
    assert(fileman_join_path(path, sizeof(path), "/", "hello") == 0);
    assert(!strcmp(path, "/hello"));
    assert(fileman_join_path(path, sizeof(path), "/home/test/", "hello") == 0);
    assert(!strcmp(path, "/home/test/hello"));
    assert(fileman_join_path(path, 8, "/tmp", "abcd") < 0);
    assert(path[0] == 0);
    assert(fileman_join_path(path, sizeof(path), "/tmp", "../escape") < 0);
    assert(!strcmp(fileman_x11_default_app("/tmp/Hello.HTML"), "/usr/bin/dillo"));
    assert(!strcmp(fileman_x11_default_app("/tmp/test.txt"), "/usr/bin/nedit"));
    assert(!strcmp(fileman_x11_default_app("/tmp/.hidden"), "/usr/bin/nedit"));
    assert(!fileman_x11_default_app("/tmp/program.elf"));
    assert(fileman_path_contains("/home/test", "/home/test/dir"));
    assert(fileman_path_contains("/", "/tmp"));
    assert(!fileman_path_contains("/home/test", "/home/testing"));
    puts("ok - file manager names, path bounds and X11 file handlers");
    return 0;
}
