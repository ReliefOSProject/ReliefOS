#include "boundary.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *fileman_x11_app_for_path(const char *path, const char *user_association);

int main(void)
{
    const char *invalid[] = {"", ".", "..", "a/b", "line\nname", "tab\tname", "bad\rname"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!fileman_name_valid(invalid[i]));
    assert(fileman_name_valid("New Folder"));
    assert(fileman_name_valid(".hidden"));
    assert(fileman_name_valid("a;$'b"));
    assert(fileman_name_valid("中文文件.txt"));
    char chinese_path[128];
    assert(fileman_join_path(chinese_path, sizeof(chinese_path), "/home/test/中文目录", "中文文件.txt") == 0);
    assert(!strcmp(chinese_path, "/home/test/中文目录/中文文件.txt"));
    assert(fileman_join_path(chinese_path, 12, "/home/test", "中文文件.txt") < 0);
    assert(!chinese_path[0]);
    char path[32];
    assert(fileman_join_path(path, sizeof(path), "/", "hello") == 0);
    assert(!strcmp(path, "/hello"));
    assert(fileman_join_path(path, sizeof(path), "/home/test/", "hello") == 0);
    assert(!strcmp(path, "/home/test/hello"));
    assert(fileman_join_path(path, 8, "/tmp", "abcd") < 0);
    assert(path[0] == 0);
    assert(fileman_join_path(path, sizeof(path), "/tmp", "../escape") < 0);
    assert(!strcmp(fileman_x11_default_app("/tmp/Hello.HTML"), "/usr/bin/dillo"));
    assert(!strcmp(fileman_x11_default_app("/tmp/budget.rexl"), "/usr/bin/reliefexcel"));
    assert(!strcmp(fileman_x11_default_app("/tmp/export.CSV"), "/usr/bin/reliefexcel"));
    assert(!strcmp(fileman_x11_default_app("/tmp/report.RWORD"), "/usr/bin/reliefword"));
    assert(!strcmp(fileman_x11_default_app("/tmp/show.RPPT"), "/usr/bin/reliefppt"));
    assert(!strcmp(fileman_x11_default_app("/tmp/equation.rmath"), "/usr/bin/reliefmath"));
    assert(!strcmp(fileman_x11_default_app("/tmp/EQUATION.RMATH"), "/usr/bin/reliefmath"));
    assert(!strcmp(fileman_x11_default_app("/tmp/tasks.rtodo"), "/usr/bin/relieftodo"));
    assert(!strcmp(fileman_x11_default_app("/tmp/TASKS.RTODO"), "/usr/bin/relieftodo"));
    assert(!strcmp(fileman_x11_default_app("/tmp/plan.rdiagram"), "/usr/bin/reliefdiagram"));
    assert(!strcmp(fileman_x11_default_app("/tmp/PLAN.RDIAGRAM"), "/usr/bin/reliefdiagram"));
    assert(!strcmp(fileman_x11_default_app("/tmp/music.mp3"), "/usr/bin/reliefplayer"));
    assert(!strcmp(fileman_x11_default_app("/tmp/MUSIC.WAV"), "/usr/bin/reliefplayer"));
    assert(!strcmp(fileman_x11_app_for_path("/tmp/equation.rmath", "/usr/bin/custom-math"),
                   "/usr/bin/custom-math"));
    assert(!strcmp(fileman_x11_app_for_path("/tmp/equation.rmath", ""), "/usr/bin/reliefmath"));
    assert(!strcmp(fileman_x11_default_app("/tmp/test.txt"), "/usr/bin/nedit"));
    assert(!strcmp(fileman_x11_default_app("/tmp/.hidden"), "/usr/bin/nedit"));
    assert(!fileman_x11_default_app("/tmp/program.elf"));
    assert(fileman_path_contains("/home/test", "/home/test/dir"));
    assert(fileman_path_contains("/", "/tmp"));
    assert(!fileman_path_contains("/home/test", "/home/testing"));
    puts("ok - file manager names, path bounds and X11 file handlers");
    return 0;
}
