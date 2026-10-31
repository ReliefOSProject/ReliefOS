#ifndef RELIEFPLAYER_PCM_H
#define RELIEFPLAYER_PCM_H
#include "decoder.h"
namespace rp {
class PcmStream {
  Decoder &decoder_;
  uint32_t rate_, channels_;
  uint64_t cursor_ = 0, base_ = 0;
  unsigned count_ = 0;
  int16_t buffer_[8192]{};
  int16_t previous_[2]{};
  bool previous_valid_ = false;
  bool sample(uint64_t index, int16_t *pair, Error &error);

public:
  PcmStream(Decoder &decoder, uint32_t rate, uint32_t channels)
      : decoder_(decoder), rate_(rate), channels_(channels) {}
  bool seek(uint64_t frame, Error &error);
  int render(int16_t *output, unsigned frames, unsigned volume, Error &error);
  uint64_t position() const;
};
} // namespace rp
#endif
