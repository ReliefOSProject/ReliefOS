#include "../../userland/apps/reliefcapture/png_io.h"
#include <png.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace rc;
int main(int argc, char **argv) {
  assert(argc == 2 && chdir(argv[1]) == 0);
  Image im; assert(im.create(3, 2));
  im.pixels[0] = 0xff0000; im.pixels[1] = 0x00ff00; im.pixels[2] = 0x0000ff;
  im.pixels[3] = 0x123456; im.pixels[4] = 0xffffff; im.pixels[5] = 0;
  Destination dest; Text error;
  assert(probe_destination("image.png", dest, error) && !dest.exists);
  assert(save_png("image.png", im, dest, error));
  struct stat st{}; assert(stat("image.png", &st) == 0 && (st.st_mode & 0777) == 0600);
  png_image png{}; png.version = PNG_IMAGE_VERSION;
  assert(png_image_begin_read_from_file(&png, "image.png"));
  png.format = PNG_FORMAT_RGB; unsigned char rgb[18];
  assert(png_image_finish_read(&png, nullptr, rgb, 0, nullptr));
  assert(png.width == 3 && png.height == 2);
  assert(rgb[0] == 255 && rgb[4] == 255 && rgb[8] == 255 && rgb[9] == 0x12 && rgb[10] == 0x34);
  png_image_free(&png);
  assert(!save_png("image.png", im, dest, error));
  assert(probe_destination("image.png", dest, error) && dest.exists);
  int fd = open("image.png", O_WRONLY | O_APPEND); assert(fd >= 0);
  assert(write(fd, "changed", 7) == 7 && close(fd) == 0);
  assert(!save_png("image.png", im, dest, error));
  assert(symlink("image.png", "alias.png") == 0);
  assert(!probe_destination("alias.png", dest, error));
  assert(mkdir("folder.png", 0700) == 0);
  assert(!probe_destination("folder.png", dest, error));
  assert(probe_destination("replace.png", dest, error));
  assert(symlink("image.png", "replace.png") == 0);
  assert(!save_png("replace.png", im, dest, error));
  assert(probe_destination("image.png", dest, error) && save_png("image.png", im, dest, error));
  assert(!probe_destination(".", dest, error));
  puts("ReliefCapture: PNG pixels, private permissions, atomic replacement, conflict and symlink protection passed");
}
