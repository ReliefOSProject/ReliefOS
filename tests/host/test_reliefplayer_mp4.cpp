#include "../../userland/apps/reliefplayer/decoder.h"
#include "../../userland/apps/reliefplayer/media.h"
#include "../../userland/apps/reliefplayer/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
using namespace rp;
static bool cancel(void *, uint64_t, uint64_t) { return false; }
static unsigned checksum(const VideoShared &s) {
  unsigned value = 0;
  for (size_t n = 0; n < size_t(s.width) * s.height * 3; ++n)
    value = value * 33 + s.pixels[n];
  return value;
}
static void wait_state(Session &s, State desired) {
  for (unsigned n = 0; n < 600; ++n) {
    s.poll();
    if (s.status().state == desired)
      return;
    assert(s.status().state != State::Failed);
    usleep(10000);
  }
  fprintf(stderr, "wanted state=%u actual=%u\n", unsigned(desired),
          unsigned(s.status().state));
  assert(false);
}
int main(int argc, char **argv) {
  assert(argc == 8);
  Error error;
  Decoder audio;
  assert(audio.open(argv[1], error));
  assert(audio.info().kind == Kind::Mp4 && audio.info().has_audio);
  assert(audio.info().width == 320 && audio.info().height == 180);
  assert(audio.info().rate == 48000 && audio.info().channels == 2);
  assert(audio.info().frames >= 95000 && audio.info().frames <= 100000);
  int16_t samples[4096 * 2];
  uint64_t frames = 0, energy = 0;
  int n;
  while ((n = audio.read(samples, 4096, error)) > 0) {
    for (int i = 0; i < n * 2; ++i)
      energy += abs(int(samples[i]));
    frames += unsigned(n);
  }
  assert(n == 0 && error == Error::Ok && frames == audio.info().frames &&
         energy > 1000000);
  assert(audio.seek(48000, error));
  assert(audio.read(samples, 4096, error) == 4096 && audio.position() == 52096);
  assert(audio.seek(0, error) && audio.read(samples, 4096, error) == 4096);
  // The AAC track starts 300 ms after the video. Preserve the initial silence.
  energy = 0;
  for (int i = 0; i < 8192; ++i)
    energy += abs(int(samples[i]));
  assert(energy == 0);
  VideoDecoder video;
  assert(video.open(argv[1], error, nullptr, nullptr));
  auto *image = static_cast<VideoShared *>(calloc(1, sizeof(VideoShared)));
  assert(image);
  assert(video.pump(0, image, error));
  assert(image->serial && image->width && image->width <= VideoWidth &&
         image->height <= VideoHeight);
  unsigned first = checksum(*image);
  assert(video.seek(1000, error));
  for (int i = 0; i < 10; ++i)
    assert(video.pump(1000, image, error));
  assert(checksum(*image) != first);
  assert(video.seek(0, error) && video.pump(0, image, error));
  assert(checksum(*image) == first);
  unsigned char rotated_corner[3];
  memcpy(rotated_corner, image->pixels + (image->width - 1) * 3, 3);
  for (int file = 5; file < 8; ++file) {
    VideoDecoder other;
    assert(other.open(argv[file], error, nullptr, nullptr));
    unsigned serial = image->serial;
    assert(other.pump(0, image, error) && image->serial != serial);
    fprintf(stderr, "MP4 fixture=%d snapshot=%ux%u\n", file, image->width,
            image->height);
    assert(image->width == (file == 5 ? 180u : 320u));
    assert(image->height == (file == 5 ? 320u : 180u));
    if (file == 5)
      assert(!memcmp(image->pixels, rotated_corner, 3));
  }
  free(image);
  Decoder silent;
  assert(silent.open(argv[2], error) && !silent.info().has_audio &&
         silent.info().rate == 1000);
  Decoder invalid;
  assert(!invalid.open(argv[3], error) && error == Error::Invalid);
  assert(!invalid.open(argv[4], error) && error == Error::Unsupported);
  assert(!invalid.open(argv[1], error, cancel) && error == Error::Cancelled);
  Session session;
  assert(session.open(argv[2], 70));
  wait_state(session, State::Playing);
  for (unsigned tries = 0; !session.video_serial() && tries < 200; ++tries) {
    session.poll();
    usleep(10000);
  }
  assert(session.video_serial() && session.pixels());
  assert(session.command(Op::Play, 0));
  wait_state(session, State::Paused);
  assert(session.command(Op::Seek, 1000));
  for (unsigned tries = 0; session.status().position != 1000 && tries < 200;
       ++tries) {
    session.poll();
    usleep(10000);
  }
  assert(session.status().position == 1000);
  assert(session.command(Op::Seek, session.status().info.frames));
  wait_state(session, State::Ended);
  assert(session.status().position == session.status().info.frames);
  assert(session.command(Op::Seek, 1000));
  wait_state(session, State::Paused);
  assert(session.command(Op::Play, 1));
  wait_state(session, State::Playing);
  wait_state(session, State::Ended);
  assert(session.status().position == session.status().info.frames);
  assert(session.command(Op::Play, 1));
  wait_state(session, State::Playing);
  assert(session.command(Op::Stop));
  wait_state(session, State::Stopped);
  assert(session.status().position == 0);
  session.shutdown();
  for (unsigned tries = 0; session.active() && tries < 400; ++tries) {
    session.poll();
    usleep(10000);
  }
  assert(!session.active());
  assert(session.open(argv[4], 70));
  for (unsigned tries = 0;
       session.status().state != State::Failed && tries < 400; ++tries) {
    session.poll();
    usleep(10000);
  }
  assert(session.status().state == State::Failed && !session.width() &&
         !session.height());
  puts("ReliefPlayer MP4: H.264/HEVC/MPEG-4, AAC, rotation, delayed track, "
       "B-frames, seek, silent video, controls and invalid media passed");
}
