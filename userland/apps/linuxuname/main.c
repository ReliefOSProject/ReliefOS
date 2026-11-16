#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <reliefos/prctl.h>

static void usage(FILE *stream)
{
    fputs("Usage: linuxuname [--] PROGRAM [ARGUMENT...]\n"
          "Run PROGRAM with uname reporting Linux as the kernel name.\n", stream);
}

int main(int argc, char **argv)
{
    int command = 1;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--")) ++command;
    if (command >= argc) {
        usage(stderr);
        return 125;
    }
    if (prctl(RELIEFOS_PR_SET_UNAME_LINUX, 1UL, 0UL, 0UL, 0UL) < 0) {
        perror("linuxuname: cannot select Linux uname");
        return 125;
    }
    execvp(argv[command], argv + command);
    int error = errno;
    fprintf(stderr, "linuxuname: %s: %s\n", argv[command], strerror(error));
    return error == ENOENT ? 127 : 126;
}
