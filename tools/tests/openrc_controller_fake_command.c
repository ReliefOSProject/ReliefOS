/* Private host test fixture; never installed into a ReliefOS image. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[2], "wait")) {
        printf("CHILD %ld\n", (long)getpid()); fflush(stdout);
        for (unsigned i = 0; i < 300; ++i) { usleep(100000); puts("working"); fflush(stdout); }
    }
    if (argc >= 3 && !strcmp(argv[2], "fail")) { puts("intentional failure"); return 7; }
    puts("probe = started 00:00:01 (1)");
    return 0;
}
