/* Private chroot process fixture: the actual worker owns exec/cancel/reaping.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "ping")) {
    puts("active");
    fflush(stdout);
    for (;;)
      pause();
  }
  puts("private fixture completed");
  return 0;
}
