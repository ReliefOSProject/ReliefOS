#include "config.h"
#include "model.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rs;
static bool fail_directory_sync;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    struct stat st{};
    if (fail_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    Array<Output> outputs;
    Text error;
    const char *query =
        "Screen 0: minimum 320 x 200, current 1920 x 1080, maximum 8192 x 8192\n"
        "Virtual-1 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis)\n"
        "   1920x1080     60.00*+  59.94\n"
        "   1280x720      60.00 +  50.00\n"
        "Virtual-2 disconnected (normal left inverted right x axis y axis)\n"
        "Virtual-3 connected (normal left inverted right x axis y axis)\n"
        "   1024x768      60.00 +\n";
    assert(parse_outputs(query, outputs, error));
    assert(outputs.size() == 2);
    assert(outputs[0].name == "Virtual-1");
    assert(outputs[0].modes.size() == 4);
    assert(outputs[0].current == 0);
    assert(outputs[0].modes[1].rate == "59.94");
    assert(outputs[1].current == -1);
    Text rate1, rate2;
    assert(rate1.set("60") && rate2.set("60.00") && rate_equal(rate1, rate2));
    assert(rate1.set("59.94") && !rate_equal(rate1, rate2));
    assert(!parse_outputs("Screen 0: none\n", outputs, error));

    Array<KeyboardLayout> layouts;
    assert(parse_layouts("! model\n  pc105  Generic 105-key PC\n! layout\n"
                         "  us English (US)\n  de German\n! variant\n"
                         "  intl us: English (US, intl., with dead keys)\n"
                         "  nodeadkeys de: German (no dead keys)\n! option\n",
                         layouts, error));
    assert(layouts.size() == 2 && layouts[0].id == "us");
    assert(layouts[0].variants.size() == 1);
    assert(layouts[0].variants[0].id == "intl");
    assert(layouts[1].variants[0].id == "nodeadkeys");

    Desktop desktop;
    assert(parse_desktop("# personal settings\nXKB_LAYOUT=us\nXKB_VARIANT=intl\n"
                         "DISPLAY=Virtual-1 1920x1080 60.00\n",
                         desktop, error));
    assert(desktop.layout == "us" && desktop.variant == "intl");
    assert(desktop.displays.size() == 1);
    Text encoded;
    assert(encode_desktop(desktop, encoded, error));
    Desktop roundtrip;
    assert(parse_desktop(encoded.c_str(), roundtrip, error));
    assert(roundtrip.displays[0].mode == "1920x1080");
    assert(!parse_desktop("XKB_LAYOUT=$(touch /oops)\n", roundtrip, error));
    assert(!parse_desktop("DISPLAY=--off 1920x1080 60\n", roundtrip, error));
    assert(!parse_desktop("DISPLAY=Virtual-1 1920x1080 60\nDISPLAY=Virtual-1 1280x720 60\n",
                          roundtrip, error));
    assert(!valid_zone("../etc/passwd"));
    assert(!valid_zone("Asia//Shanghai"));
    assert(valid_zone("Asia/Shanghai") && valid_zone("Etc/GMT+8"));
    assert(parse_desktop("XKB_LAYOUT=us\nXKB_VARIANT=intl\nXKB_LAYOUT=\nXKB_VARIANT=\n", roundtrip,
                         error));
    assert(roundtrip.layout.empty() && roundtrip.variant.empty());

    Text language;
    assert(read_language("LC_TIME=C\nLANG=\"zh_CN.UTF-8\"\n", language, error));
    assert(language == "zh_CN.UTF-8");
    assert(
        update_language("# custom\nLC_TIME=C\nLANG=en_US.UTF-8\n", "zh_CN.UTF-8", encoded, error));
    assert(strstr(encoded.c_str(), "LC_TIME=C\n"));
    assert(strstr(encoded.c_str(), "LANG=zh_CN.UTF-8\n"));
    assert(!update_language("", "zh_CN.UTF-8\nEXEC=oops", encoded, error));

    Store store(argv[1]);
    Snapshot before, after;
    assert(store.read("desktop.conf", before, error) && !before.exists);
    assert(store.save("desktop.conf", before, Text{}, error));
    assert(store.read("desktop.conf", after, error) && after.exists);
    assert(!store.save("desktop.conf", before, Text{}, error));
    assert(error == "The settings changed in another program. Reload and try again.");
    Text content;
    assert(content.set("XKB_LAYOUT=de\n"));
    assert(store.save("desktop.conf", after, content, error));
    assert(store.read("desktop.conf", after, error) && after.bytes == "XKB_LAYOUT=de\n");
    Text dotted_path;
    assert(dotted_path.set(argv[1]) && append(dotted_path, "/./"));
    Store dotted(dotted_path.c_str());
    assert(dotted.read("desktop.conf", after, error) && after.bytes == "XKB_LAYOUT=de\n");
    bool published = false;
    assert(content.set("XKB_LAYOUT=fr\n"));
    fail_directory_sync = true;
    assert(!store.save("desktop.conf", after, content, error, &published) && published);
    fail_directory_sync = false;
    assert(store.read("desktop.conf", after, error) && after.bytes == "XKB_LAYOUT=fr\n");
    int desktop_lock = store.lock_desktop(error);
    assert(desktop_lock >= 0);
    // Saving the file must preserve the separate desktop-operation lock.
    assert(store.save("desktop.conf", after, content, error));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        Text problem;
        int lock = store.lock_desktop(problem);
        if (lock >= 0)
            close(lock);
        _exit(lock < 0 ? 0 : 1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(desktop_lock);
    char path[4096];
    snprintf(path, sizeof(path), "%s/desktop.conf", argv[1]);
    struct stat st{};
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(unlink(path) == 0 && symlink("victim", path) == 0);
    assert(!store.read("desktop.conf", after, error));
    assert(!store.read("../victim", after, error));
    assert(!store.save("../victim", before, content, error));
    snprintf(path, sizeof(path), "%s/etc", argv[1]);
    assert(mkdir(path, 0700) == 0);
    Text etc;
    assert(etc.set(path));
    snprintf(path, sizeof(path), "%s/zones", argv[1]);
    assert(mkdir(path, 0700) == 0);
    Text zones;
    assert(zones.set(path));
    snprintf(path, sizeof(path), "%s/zones/UTC", argv[1]);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "TZif", 4) == 4 && close(fd) == 0);
    assert(set_timezone("UTC", error, etc.c_str(), zones.c_str()));
    snprintf(path, sizeof(path), "%s/etc/localtime", argv[1]);
    char target[4096]{};
    assert(readlink(path, target, sizeof(target) - 1) > 0);
    Text want;
    assert(want.set(zones.c_str()) && append(want, "/UTC"));
    assert(!strcmp(target, want.c_str()));
    assert(!set_timezone("../desktop.conf", error, etc.c_str(), zones.c_str()));
    fail_directory_sync = true;
    published = false;
    assert(!set_timezone("UTC", error, etc.c_str(), zones.c_str(), &published) && published);
    fail_directory_sync = false;
    puts("ReliefSettings configuration and X11 data tests passed");
}
