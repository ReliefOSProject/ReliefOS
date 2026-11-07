/* Authentication transport fixture, confined to the host test chroot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
int main(void) {
  struct termios t;
  if (tcgetattr(0, &t))
    return 1;
  if (getenv("OC_TEST_PASSWORDLESS")) {
    cfmakeraw(&t);
    if (tcsetattr(0, TCSANOW, &t))
      return 1;
    usleep(200000);
  } else {
    t.c_lflag &= ~ECHO;
    if (tcsetattr(0, TCSANOW, &t))
      return 1;
    if (write(0, "Password:", 9) != 9)
      return 1;
    char password[64];
    size_t size = 0;
    while (size < sizeof(password) - 1) {
      char c;
      if (read(0, &c, 1) != 1)
        return 1;
      if (c == '\n')
        break;
      password[size++] = c;
    }
    password[size] = 0;
    if (strcmp(password, "fixture"))
      return 1;
    memset(password, 0, sizeof(password));
  }
  execl("/usr/lib/reliefos/apps/openrc-controller/openrc-controller.elf",
        "openrc-controller.elf", "--worker", (char *)NULL);
  return 1;
}
