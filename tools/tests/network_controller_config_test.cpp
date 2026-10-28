#include "config.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace nc;
static Text text(const char *s) {
  Text t;
  assert(t.set(s));
  return t;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  ConfigStore store(argv[1]);
  Snapshot original, current;
  Text error;
  assert(store.read(original, error) && !original.exists);
  Config c;
  Text bytes;
  assert(serialize_config(c, bytes, error));
  assert(store.save(original, bytes, error));
  assert(store.read(current, error) && current.exists &&
         current.bytes == bytes);
  assert(!store.save(
      original, bytes,
      error)); // Another save cannot silently overwrite a changed file.
  assert(store.restore(current, error));
  assert(store.read(current, error) && !current.exists);
  assert(store.save(current, bytes, error));
  assert(store.read(current, error));
  c.dns = Dns::Cloudflare;
  Text updated;
  assert(serialize_config(c, updated, error));
  assert(store.save(current, updated, error));
  assert(store.read(current, error) && current.bytes == updated);
  assert(store.restore(current, error));
  assert(store.read(current, error) && current.bytes == bytes);
  assert(!store.save(current, text("mode=dhcp\n"), error));
  char path[1024], outside[1024];
  snprintf(path, sizeof(path), "%s/network.conf", argv[1]);
  snprintf(outside, sizeof(outside), "%s/outside", argv[1]);
  int fd = open(outside, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && write(fd, "sentinel", 8) == 8);
  close(fd);
  assert(unlink(path) == 0 && symlink(outside, path) == 0);
  assert(!store.read(current, error));
  assert(!store.save(original, bytes, error));
  fd = open(outside, O_RDONLY);
  char sentinel[9] = {};
  assert(fd >= 0 && read(fd, sentinel, 8) == 8);
  close(fd);
  assert(!strcmp(sentinel, "sentinel"));
  assert(unlink(path) == 0);
  assert(store.read(current, error));
  assert(store.save(current, bytes, error));
  assert(store.read(current, error));
  snprintf(path, sizeof(path), "%s/network.conf.bak", argv[1]);
  assert(unlink(path) == 0 && symlink(outside, path) == 0);
  assert(!store.save(current, updated, error));
  assert(!store.restore(current, error));
  puts("PASS Network Controller configuration: save, absence restore, backup, "
       "conflict and symlink isolation");
}
