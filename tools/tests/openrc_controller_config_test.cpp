#include "config.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static int tests;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++tests;                                                                   \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main(int argc, char **argv) {
  using namespace oc;
  if (argc != 2)
    return 2;
  CHECK(chdir(argv[1]) == 0);
  CHECK(mkdir("conf.d", 0700) == 0);
  FILE *f = fopen("conf.d/probe", "w");
  CHECK(f);
  fputs("# comment\nvalue=old\n", f);
  CHECK(fclose(f) == 0);
  ConfigStore store(argv[1]);
  Snapshot old, now;
  Text error, edit;
  CHECK(store.read("probe", old, error));
  CHECK(old.exists);
  CHECK(old.bytes == "# comment\nvalue=old\n");
  CHECK(edit.set("# comment\nvalue=new\n"));
  CHECK(store.save("probe", old, edit, error));
  struct stat lock_info;
  CHECK(lstat("conf.d/.openrc-controller.lock", &lock_info) == 0);
  CHECK(S_ISREG(lock_info.st_mode) && !(lock_info.st_mode & 0022));
  CHECK(store.read("probe", now, error));
  CHECK(now.bytes == edit);
  Snapshot bak;
  CHECK(store.read("probe", bak, error, true));
  CHECK(bak.bytes == old.bytes);
  CHECK(!store.save(
      "probe", old, edit,
      error)); // external or second editor changes are not overwritten
  CHECK(store.restore("probe", now, error));
  CHECK(store.read("probe", now, error));
  CHECK(now.bytes == old.bytes);
  CHECK(!store.read("../shadow", now, error));
  CHECK(symlink("probe", "conf.d/link") == 0);
  CHECK(!store.read("link", now, error));
  CHECK(!store.save("link", Snapshot{}, edit, error));
  CHECK(store.read("new", now, error));
  CHECK(!now.exists);
  CHECK(store.save("new", now, edit, error));
  CHECK(store.read("new", now, error));
  CHECK(now.exists && now.bytes == edit);
  CHECK(chmod("conf.d/new", 0666) == 0);
  CHECK(!store.read("new", now, error));
  CHECK(store.read("@global", now, error));
  CHECK(!now.exists);
  CHECK(store.save("@global", now, edit, error));
  CHECK(access("rc.conf", F_OK) == 0);
  f = fopen("conf.d/private", "w");
  CHECK(f);
  CHECK(fputs("private fixture\n", f) >= 0);
  CHECK(fclose(f) == 0);
  CHECK(chmod("conf.d/private", 0600) == 0);
  CHECK(store.read("private", old, error));
  CHECK(store.save("private", old, edit, error));
  CHECK(unlink("conf.d/private") == 0);
  CHECK(store.read("private", now, error) && !now.exists);
  CHECK(store.restore("private", now, error));
  struct stat restored;
  CHECK(stat("conf.d/private", &restored) == 0);
  CHECK((restored.st_mode & 0777) == 0600);
  Snapshot safe;
  CHECK(store.read("probe", safe, error));
  int lock = open("conf.d/.openrc-controller.lock", O_RDWR);
  CHECK(lock >= 0);
  CHECK(flock(lock, LOCK_EX | LOCK_NB) == 0);
  CHECK(!store.save("probe", safe, edit, error));
  CHECK(flock(lock, LOCK_UN) == 0);
  CHECK(close(lock) == 0);
  CHECK(unlink("conf.d/.openrc-controller.lock") == 0);
  CHECK(symlink("probe", "conf.d/.openrc-controller.lock") == 0);
  CHECK(!store.save("probe", safe, edit, error));
  puts("OpenRC Controller configuration tests passed");
  printf("%d checks\n", tests);
  return 0;
}
