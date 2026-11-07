#include "../../userland/apps/reliefplayer/decoder.h"
#include "../../userland/apps/reliefplayer/pcm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char bytes[512];
static void u16(size_t at, unsigned n) {
  bytes[at] = n;
  bytes[at + 1] = n >> 8;
}
static void u32(size_t at, unsigned n) {
  u16(at, n);
  u16(at + 2, n >> 16);
}
static void store(const char *path, size_t n) {
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(bytes, 1, n, f) == n);
  assert(!fclose(f));
}
static size_t wave(const char *path, unsigned bits = 16, unsigned channels = 1,
                   unsigned format = 1) {
  memset(bytes, 0, sizeof(bytes));
  memcpy(bytes, "RIFF", 4);
  memcpy(bytes + 8, "WAVE", 4);
  memcpy(bytes + 12, "JUNK", 4);
  u32(16, 3);
  memcpy(bytes + 20, "abc", 3);
  memcpy(bytes + 24, "fmt ", 4);
  u32(28, 16);
  u16(32, format);
  u16(34, channels);
  u32(36, 8000);
  u32(40, 8000 * channels * bits / 8);
  u16(44, channels * bits / 8);
  u16(46, bits);
  memcpy(bytes + 48, "data", 4);
  size_t count = 4 * channels * bits / 8;
  u32(52, count);
  u32(4, 48 + count);
  for (unsigned n = 0; n < 4 * channels; ++n) {
    size_t at = 56 + n * bits / 8;
    if (format == 3) {
      float v = n % 2 ? -.5f : .5f;
      memcpy(bytes + at, &v, 4);
    } else if (bits == 8)
      bytes[at] = n % 2 ? 64 : 192;
    else if (bits == 16)
      u16(at, n % 2 ? (unsigned)-16384 : 16384);
    else if (bits == 24) {
      bytes[at + 2] = n % 2 ? 0xc0 : 0x40;
    } else
      u32(at, n % 2 ? 0xc0000000 : 0x40000000);
  }
  store(path, 56 + count);
  return 56 + count;
}
static bool cancel(void *, uint64_t, uint64_t) { return false; }

int main(int argc, char **argv) {
  assert(argc == 3);
  char path[4096];
  snprintf(path, sizeof(path), "%s/audio.wav", argv[1]);
  rp::Error error = rp::Error::Ok;
  rp::Decoder d;
  const unsigned widths[] = {8, 16, 24, 32};
  for (unsigned bits : widths) {
    wave(path, bits);
    assert(d.open(path, error));
    assert(d.info().rate == 8000 && d.info().channels == 1 &&
           d.info().frames == 4);
    int16_t pcm[32];
    assert(d.read(pcm, 8, error) == 4);
    assert(pcm[0] == 16384 && pcm[1] == 16384 && pcm[2] == -16384 &&
           pcm[3] == -16384);
    assert(d.read(pcm, 8, error) == 0);
    assert(d.seek(1, error));
    assert(d.read(pcm, 1, error) == 1);
    assert(pcm[0] == -16384 && pcm[1] == -16384);
    assert(d.seek(999, error));
    assert(d.position() == 4 && d.read(pcm, 1, error) == 0);
  }
  wave(path, 32, 2, 3);
  assert(d.open(path, error));
  int16_t pcm[1024];
  assert(d.read(pcm, 8, error) == 4 && pcm[0] == 16384 && pcm[1] == -16384);
  float nan = NAN;
  memcpy(bytes + 56, &nan, 4);
  store(path, 88);
  assert(d.open(path, error));
  assert(d.read(pcm, 1, error) == 1 && pcm[0] == 0);
  wave(path);
  assert(d.open(path, error));
  rp::PcmStream s(d, 48000, 6);
  assert(s.seek(0, error));
  assert(s.render(pcm, 100, 50, error) == 24);
  assert(pcm[0] == 8192 && pcm[1] == 8192);
  for (int n = 0; n < 24; ++n)
    for (int c = 2; c < 6; ++c)
      assert(pcm[n * 6 + c] == 0);
  assert(s.render(pcm, 100, 50, error) == 0 && s.position() == 4);
  assert(s.seek(2, error));
  assert(s.render(pcm, 100, 0, error) == 12);
  for (int n = 0; n < 72; ++n)
    assert(pcm[n] == 0);
  assert(s.seek(0, error));
  assert(s.render(pcm, 2, 100, error) == 2);
  assert(pcm[0] == 16384 && pcm[6] > 10000 &&
         pcm[6] < 12000); // linear interpolation
  // A right interpolation sample may refill the cache before the left sample
  // advances.
  wave(path);
  u32(4, 48 + 10000);
  u32(52, 10000);
  FILE *long_file = fopen(path, "wb");
  assert(long_file);
  assert(fwrite(bytes, 1, 56, long_file) == 56);
  const unsigned char pair[] = {0, 0x40};
  for (unsigned i = 0; i < 5000; ++i)
    assert(fwrite(pair, 1, 2, long_file) == 2);
  assert(!fclose(long_file));
  assert(d.open(path, error));
  rp::PcmStream boundary(d, 48000, 2);
  assert(boundary.seek(0, error));
  uint64_t rendered = 0;
  int produced;
  while ((produced = boundary.render(pcm, 512, 100, error)) > 0) {
    for (int i = 0; i < produced * 2; ++i)
      assert(pcm[i] == 16384);
    rendered += produced;
  }
  assert(produced == 0 && rendered == 30000);
  wave(path);
  store(path, 57);
  assert(!d.open(path, error) && error == rp::Error::Invalid);
  wave(path);
  u16(44, 3);
  store(path, 64);
  assert(!d.open(path, error));
  wave(path, 16, 3);
  assert(!d.open(path, error) && error == rp::Error::Unsupported);
  wave(path, 16, 1, 7);
  assert(!d.open(path, error) && error == rp::Error::Unsupported);
  wave(path);
  u32(16, 0xfffffffe);
  store(path, 64);
  assert(!d.open(path, error));
  wave(path);
  u32(52, 7);
  store(path, 64);
  assert(!d.open(path, error));
  wave(path);
  assert(!d.open(path, error, cancel) && error == rp::Error::Cancelled);
  memset(bytes, 0, sizeof(bytes));
  store(path, 512);
  assert(!d.open(path, error));
  assert(!d.open(argv[1],
                 error)); // directories must not enter the streaming decoder
  assert(d.open(argv[2], error));
  assert(d.info().kind == rp::Kind::Mp3 && d.info().frames > 0);
  uint64_t count = 0;
  int peak = 0, n;
  int16_t expected[2] = {};
  uint64_t target = d.info().frames / 2;
  while ((n = d.read(pcm, 512, error)) > 0) {
    if (count <= target && count + (unsigned)n > target)
      memcpy(expected, pcm + 2 * (target - count), 4);
    for (int i = 0; i < n * 2; ++i)
      if (abs(pcm[i]) > peak)
        peak = abs(pcm[i]);
    count += n;
  }
  assert(n == 0 && count == d.info().frames && peak > 50);
  assert(d.seek(target, error));
  assert(d.read(pcm, 1, error) == 1);
  assert(abs(pcm[0] - expected[0]) < 8 && abs(pcm[1] - expected[1]) < 8);
  assert(d.seek(0, error));
  assert(d.read(pcm, 4, error) == 4);
  uint64_t original_frames = d.info().frames;
  FILE *source = fopen(argv[2], "rb"), *tagged = fopen(path, "wb");
  assert(source && tagged);
  size_t copied;
  while ((copied = fread(bytes, 1, sizeof(bytes), source)))
    assert(fwrite(bytes, 1, copied, tagged) == copied);
  assert(!fclose(source));
  memset(bytes, 0, 128);
  memcpy(bytes, "TAG", 3);
  assert(fwrite(bytes, 1, 128, tagged) == 128 && !fclose(tagged));
  assert(d.open(path, error) && d.info().frames == original_frames);
  for (unsigned variant = 0; variant < 3; ++variant) {
    source = fopen(argv[2], "rb");
    tagged = fopen(path, "wb");
    assert(source && tagged);
    while ((copied = fread(bytes, 1, sizeof(bytes), source)))
      assert(fwrite(bytes, 1, copied, tagged) == copied);
    assert(!fclose(source));
    memset(bytes, 0, 32);
    memcpy(bytes, "APETAGEX", 8);
    u32(8, 2000);
    u32(12, 32);
    if (variant) {
      u32(20, 0xa0000000);
      assert(fwrite(bytes, 1, 32, tagged) == 32);
      u32(20, 0x80000000);
    }
    assert(fwrite(bytes, 1, 32, tagged) == 32);
    if (variant == 2) {
      memset(bytes, 0, 128);
      memcpy(bytes, "TAG", 3);
      assert(fwrite(bytes, 1, 128, tagged) == 128);
    }
    assert(!fclose(tagged));
    assert(d.open(path, error) && d.info().frames == original_frames);
  }
  puts("ReliefPlayer: WAV formats/bounds, MP3 decode/seek, resampling and "
       "volume passed");
}
