#ifndef RELIEFPLAYER_DECODER_H
#define RELIEFPLAYER_DECODER_H
#include <stddef.h>
#include <stdint.h>
namespace rp {
enum class Error : uint32_t {
  Ok,
  Cancelled,
  Io,
  Invalid,
  Unsupported,
  TooLarge,
  Memory,
  Device,
  Busy,
  Playback,
  Worker
};
enum class Kind : uint32_t { Wav, Mp3 };
struct Info {
  Kind kind = Kind::Wav;
  uint32_t rate = 0, channels = 0, bits = 0;
  uint64_t frames = 0;
};
using Progress = bool (*)(void *, uint64_t, uint64_t);
const char *error_text(Error error);
class Decoder {
  struct Impl;
  Impl *p_ = nullptr;

public:
  Decoder() = default;
  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;
  ~Decoder();
  bool open(const char *path, Error &error, Progress progress = nullptr,
            void *opaque = nullptr);
  void close();
  const Info &info() const;
  uint64_t position() const;
  bool seek(uint64_t frame, Error &error);
  // Stereo interleaved S16; returns frames, 0 at EOF, or -1 with error.
  int read(int16_t *stereo, unsigned frames, Error &error);
};
} // namespace rp
#endif
