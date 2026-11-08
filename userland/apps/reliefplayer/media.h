#ifndef RELIEFPLAYER_MEDIA_H
#define RELIEFPLAYER_MEDIA_H
#include "decoder.h"
namespace rp {
// RGB snapshots are bounded independently of the source resolution. The worker
// and GUI only try the lock; neither waits for the other process.
constexpr unsigned VideoWidth = 1280, VideoHeight = 720;
constexpr unsigned VideoBytes = VideoWidth * VideoHeight * 3;
struct VideoShared {
  uint32_t lock, serial, width, height;
  unsigned char pixels[VideoBytes];
};
class MediaAudio {
  struct Impl;
  Impl *p_ = nullptr;

public:
  MediaAudio() = default;
  MediaAudio(const MediaAudio &) = delete;
  MediaAudio &operator=(const MediaAudio &) = delete;
  ~MediaAudio();
  bool open(const char *, Info &, Error &, Progress, void *);
  bool seek(uint64_t, Error &);
  int read(int16_t *, unsigned, Error &);
  uint64_t position() const;
};
class VideoDecoder {
  struct Impl;
  Impl *p_ = nullptr;

public:
  VideoDecoder() = default;
  VideoDecoder(const VideoDecoder &) = delete;
  VideoDecoder &operator=(const VideoDecoder &) = delete;
  ~VideoDecoder();
  bool open(const char *, Error &, Progress, void *);
  bool seek(uint64_t milliseconds, Error &);
  bool pump(uint64_t milliseconds, VideoShared *, Error &);
};
} // namespace rp
#endif
