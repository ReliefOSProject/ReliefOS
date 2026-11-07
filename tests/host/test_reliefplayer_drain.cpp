// A deliberately slow OSS sink exercises the real worker and IPC, including
// arbitrary short byte writes, without requiring a host audio device.
#include "../../userland/apps/reliefplayer/session.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct Shared {
  volatile unsigned sync_calls, starts, last_bytes;
  volatile unsigned allow_end;
  unsigned char audio[65536];
};
static Shared *shared;
static int audio_fd = -1;
static unsigned accepted = 0, queued = 0;
static uint64_t clock_ms = 0;
static bool started = false;
extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_close(int);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __wrap_open(const char *path, int flags, ...) {
  if (strcmp(path, "/dev/dsp"))
    return __real_open(path, flags, 0600);
  audio_fd = __real_open("/dev/null", flags, 0600);
  accepted = queued = 0;
  started = false;
  clock_ms = rp::monotonic_ms();
  return audio_fd;
}
extern "C" int __wrap_close(int fd) {
  if (fd == audio_fd) {
    shared->last_bytes = accepted;
    audio_fd = -1;
  }
  return __real_close(fd);
}
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t bytes) {
  if (fd != audio_fd)
    return __real_write(fd, data, bytes);
  if (bytes > 733)
    bytes = 733;
  assert(accepted + bytes <= sizeof(shared->audio));
  memcpy(shared->audio + accepted, data, bytes);
  accepted += bytes;
  queued += bytes;
  return ssize_t(bytes);
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  assert(fd == audio_fd);
  va_list ap;
  va_start(ap, request);
  auto *value = va_arg(ap, int *);
  va_end(ap);
  switch (request) {
  case SNDCTL_DSP_SETFMT:
    *value = AFMT_S16_LE;
    return 0;
  case SNDCTL_DSP_CHANNELS:
    *value = 2;
    return 0;
  case SNDCTL_DSP_SPEED:
    *value = 8000;
    return 0;
  case SNDCTL_DSP_GETBLKSIZE:
    *value = 4096;
    return 0;
  case SNDCTL_DSP_SETFRAGMENT:
    return 0;
  case SNDCTL_DSP_RESET:
    queued = 0;
    return 0;
  case SNDCTL_DSP_SETTRIGGER:
    started = true;
    clock_ms = rp::monotonic_ms();
    shared->starts = shared->starts + 1;
    return 0;
  case SNDCTL_DSP_GETODELAY: {
    uint64_t now = rp::monotonic_ms(), elapsed = now - clock_ms;
    if (started) {
      unsigned consumed = unsigned(elapsed * 32);
      queued = consumed < queued ? queued - consumed : 0;
      if (!shared->allow_end && queued < 128)
        queued = 128;
    }
    clock_ms = now;
    *value = int(queued);
    return 0;
  }
  case SNDCTL_DSP_SYNC:
    shared->sync_calls = shared->sync_calls + 1;
    usleep(2000000);
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}
static void state(rp::Session &s, rp::State wanted, unsigned timeout = 2000) {
  uint64_t deadline = rp::monotonic_ms() + timeout;
  while (s.status().state != wanted && rp::monotonic_ms() < deadline) {
    s.poll();
    usleep(1000);
  }
  assert(s.status().state == wanted);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  shared = static_cast<Shared *>(mmap(nullptr, sizeof(Shared),
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  assert(shared != MAP_FAILED);
  memset(shared, 0, sizeof(*shared));
  unsigned char header[44] = {'R', 'I', 'F', 'F'};
  auto le = [&](unsigned at, unsigned value, unsigned bytes) {
    while (bytes--) {
      header[at++] = value;
      value >>= 8;
    }
  };
  memcpy(header + 8, "WAVEfmt ", 8);
  le(4, 36 + 25600, 4);
  le(16, 16, 4);
  le(20, 1, 2);
  le(22, 2, 2);
  le(24, 8000, 4);
  le(28, 32000, 4);
  le(32, 4, 2);
  le(34, 16, 2);
  memcpy(header + 36, "data", 4);
  le(40, 25600, 4);
  FILE *file = fopen(argv[1], "wb");
  assert(file);
  assert(fwrite(header, 1, 44, file) == 44);
  int16_t pair[] = {10000, -10000};
  for (int n = 0; n < 6400; ++n)
    assert(fwrite(pair, 1, 4, file) == 4);
  assert(!fclose(file));
  rp::Session player;
  assert(player.open(argv[1], 70));
  uint64_t deadline = rp::monotonic_ms() + 2000;
  while (!shared->starts && !shared->sync_calls &&
         rp::monotonic_ms() < deadline) {
    player.poll();
    usleep(1000);
  }
  assert(shared->starts || shared->sync_calls);
  assert(player.command(rp::Op::Play, 0));
  state(player, rp::State::Paused, 350);
  assert(!shared->sync_calls);
  assert(player.command(rp::Op::Seek, 6400));
  state(player, rp::State::Ended);
  assert(player.status().position == 6400);
  assert(player.command(rp::Op::Play, 1));
  state(player, rp::State::Playing);
  deadline = rp::monotonic_ms() + 2000;
  while (player.status().position != 6400 && rp::monotonic_ms() < deadline) {
    player.poll();
    usleep(1000);
  }
  assert(player.status().state == rp::State::Playing &&
         player.status().position == 6400);
  assert(player.command(rp::Op::Volume, 30));
  state(player, rp::State::Ended, 350);
  assert(player.command(rp::Op::Volume, 70));
  shared->allow_end = 1;
  assert(player.command(rp::Op::Play, 1));
  state(player, rp::State::Playing);
  state(player, rp::State::Ended, 3000);
  assert(player.status().position == 6400);
  assert(shared->last_bytes ==
         28672); // every source byte plus one period's padding
  for (unsigned n = 0; n < 25600; n += 4) {
    int16_t values[2];
    memcpy(values, shared->audio + n, 4);
    assert(values[0] == 7000 && values[1] == -7000);
  }
  for (unsigned n = 25600; n < 28672; ++n)
    assert(!shared->audio[n]);
  player.shutdown();
  deadline = rp::monotonic_ms() + 350;
  while (player.active() && rp::monotonic_ms() < deadline) {
    player.poll();
    usleep(1000);
  }
  assert(!player.active() && !shared->sync_calls);
  munmap(shared, sizeof(*shared));
  puts("ReliefPlayer: 8 kHz drain remains cancellable; short writes, "
       "seek-to-end and replay passed");
}
