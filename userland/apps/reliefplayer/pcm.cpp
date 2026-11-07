#include "pcm.h"
#include <string.h>
namespace rp {
bool PcmStream::seek(uint64_t frame, Error &error) {
  if (!rate_ || channels_ < 2 || channels_ > 16) {
    error = Error::Unsupported;
    return false;
  }
  if (!decoder_.seek(frame, error))
    return false;
  base_ = decoder_.position();
  count_ = 0;
  previous_valid_ = false;
  cursor_ = base_ * rate_;
  return true;
}
uint64_t PcmStream::position() const {
  uint64_t frame = rate_ ? cursor_ / rate_ : 0;
  return frame > decoder_.info().frames ? decoder_.info().frames : frame;
}
bool PcmStream::sample(uint64_t index, int16_t *pair, Error &error) {
  if (index >= decoder_.info().frames)
    index = decoder_.info().frames - 1;
  if (previous_valid_ && index + 1 == base_) {
    memcpy(pair, previous_, 4);
    return true;
  }
  if (!count_ || index >= base_ + count_) {
    previous_valid_ = count_ != 0;
    if (count_)
      memcpy(previous_, buffer_ + (count_ - 1) * 2, 4);
    base_ = decoder_.position();
    int n = decoder_.read(buffer_, 4096, error);
    if (n <= 0)
      return false;
    count_ = unsigned(n);
  }
  if (index < base_ || index >= base_ + count_) {
    error = Error::Invalid;
    return false;
  }
  memcpy(pair, buffer_ + (index - base_) * 2, 4);
  return true;
}
int PcmStream::render(int16_t *output, unsigned frames, unsigned volume,
                      Error &error) {
  error = Error::Ok;
  if (!output || !rate_ || channels_ < 2 || channels_ > 16) {
    error = Error::Invalid;
    return -1;
  }
  if (volume > 100)
    volume = 100;
  uint64_t end = decoder_.info().frames * rate_;
  unsigned n = 0;
  for (; n < frames && cursor_ < end; ++n) {
    uint64_t index = cursor_ / rate_, fraction = cursor_ % rate_;
    int16_t left[2], right[2];
    if (!sample(index, left, error) || !sample(index + 1, right, error))
      return -1;
    for (unsigned channel = 0; channel < channels_; ++channel) {
      int64_t value =
          channel < 2
              ? left[channel] + int64_t(right[channel] - left[channel]) *
                                    int64_t(fraction) / rate_
              : 0;
      output[n * channels_ + channel] = int16_t(value * volume / 100);
    }
    cursor_ += decoder_.info().rate;
  }
  return int(n);
}
} // namespace rp
