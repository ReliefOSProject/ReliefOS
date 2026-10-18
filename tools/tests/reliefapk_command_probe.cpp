#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Emit the frontend's actual argv, without executing or elevating it. The host
// integration test supplies a private --root before executing the pinned apk.
int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    ra::Settings settings;
    settings.offline = true;
    settings.fetch_dependencies = true;
    ra::Command command;
    ra::Text error;
    bool ok;
    if (!strcmp(argv[1], "query")) {
        ok = ra::query_command(atoi(argv[2]), settings, command, error);
    } else if (!strcmp(argv[1], "metadata")) {
        ok = ra::metadata_command(argv[2], settings, command, error);
    } else {
        ra::Array<ra::Text> targets;
        for (int i = 3; i < argc; ++i) {
            ra::Text value;
            if (!value.set(argv[i]) || !targets.push(value))
                return 1;
        }
        ok = ra::make_command(static_cast<ra::Action>(atoi(argv[2])), targets,
                              settings, !strcmp(argv[1], "simulate"), command, error);
    }
    if (!ok) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    for (size_t i = 0; i < command.args.size(); ++i)
        if (fwrite(command.args[i].c_str(), 1, command.args[i].size() + 1, stdout) !=
            command.args[i].size() + 1)
            return 1;
    return 0;
}
