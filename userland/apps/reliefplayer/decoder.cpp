#include "decoder.h"
#include "../relieftodo/storage.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>
namespace rp {
namespace {
constexpr uint64_t MaxBytes = uint64_t(2) * 1024 * 1024 * 1024;
constexpr unsigned MaxFrames = 1000000, BufferBytes = 65536;
uint16_t le16(const unsigned char *p) { return p[0] | (unsigned(p[1]) << 8); }
uint32_t le32(const unsigned char *p) {
  return le16(p) | (uint32_t(le16(p + 2)) << 16);
}
bool read_at(int fd, void *data, size_t size, uint64_t offset) {
  auto *p = static_cast<unsigned char *>(data);
  while (size) {
    ssize_t n = pread(fd, p, size, off_t(offset));
    if (n < 0 && errno == EINTR)
      continue;
    if (!n)
      errno = EIO;
    if (n <= 0)
      return false;
    p += n;
    offset += n;
    size -= n;
  }
  return true;
}
struct Frame {
  uint64_t offset = 0, start = 0;
  uint32_t bytes = 0, samples = 0;
};
} // namespace
struct Decoder::Impl {
  int fd = -1;
  Info info;
  uint64_t size = 0, data = 0, position = 0;
  unsigned align = 0, format = 0;
  rt::Array<Frame> index;
  mp3dec_t mp3{};
  size_t next = 0;
  unsigned pcm_count = 0, pcm_at = 0, failures = 0;
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]{};
  unsigned char input[BufferBytes];
  ~Impl() {
    if (fd >= 0)
      ::close(fd);
  }
  bool wav(Error &error, Progress progress, void *opaque);
  bool scan(Error &error, Progress progress, void *opaque);
  int frame(Error &error);
};
const char *error_text(Error error) {
  switch (error) {
  case Error::Ok:
    return "Ready";
  case Error::Cancelled:
    return "Cancelled";
  case Error::Io:
    return "Could not read the audio file";
  case Error::Invalid:
    return "Invalid or truncated audio file";
  case Error::Unsupported:
    return "Unsupported audio format";
  case Error::TooLarge:
    return "Audio file exceeds the supported limit";
  case Error::Memory:
    return "Not enough memory";
  case Error::Device:
    return "Audio device is unavailable";
  case Error::Busy:
    return "Audio device is busy";
  case Error::Playback:
    return "Audio playback failed";
  case Error::Worker:
    return "Audio process stopped unexpectedly";
  }
  return "Audio playback failed";
}
bool Decoder::Impl::wav(Error &error, Progress progress, void *opaque) {
  uint64_t end = uint64_t(le32(input + 4)) + 8, at = 12;
  if (end > size || end < 12)
    return false;
  bool have_format = false, have_data = false;
  uint64_t bytes = 0;
  for (unsigned chunks = 0; at < end && chunks < 65536; ++chunks) {
    if (progress && !progress(opaque, at, size)) {
      error = Error::Cancelled;
      return false;
    }
    unsigned char head[8], fmt[40];
    if (end - at < 8 || !read_at(fd, head, 8, at))
      return false;
    uint64_t length = le32(head + 4), body = at + 8;
    if (length > end - body)
      return false;
    if (!memcmp(head, "fmt ", 4)) {
      if (have_format || length < 16 ||
          !read_at(fd, fmt, length < 40 ? size_t(length) : 40, body))
        return false;
      format = le16(fmt);
      info.channels = le16(fmt + 2);
      info.rate = le32(fmt + 4);
      align = le16(fmt + 12);
      info.bits = le16(fmt + 14);
      if (format == 0xfffe) {
        static const unsigned char guid_tail[14] = {
            0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (length < 40 || le16(fmt + 16) < 22 || le16(fmt + 18) != info.bits ||
            memcmp(fmt + 26, guid_tail, 14))
          return false;
        format = le16(fmt + 24);
      }
      if ((format != 1 && format != 3) ||
          (info.channels != 1 && info.channels != 2) || info.rate < 8000 ||
          info.rate > 192000 ||
          (format == 3 ? info.bits != 32
                       : (info.bits != 8 && info.bits != 16 &&
                          info.bits != 24 && info.bits != 32))) {
        error = Error::Unsupported;
        return false;
      }
      if (align != info.channels * (info.bits / 8) ||
          uint64_t(le32(fmt + 8)) != uint64_t(info.rate) * align)
        return false;
      have_format = true;
    } else if (!memcmp(head, "data", 4) && !have_data) {
      data = body;
      bytes = length;
      have_data = true;
    }
    uint64_t padded = length + (length & 1);
    if (padded > end - body)
      return false;
    at = body + padded;
  }
  if (at != end || !have_format || !have_data || !bytes || bytes % align)
    return false;
  info.kind = Kind::Wav;
  info.frames = bytes / align;
  return true;
}
bool Decoder::Impl::scan(Error &error, Progress progress, void *opaque) {
  // ID3v1 is outside the MPEG bitstream. minimp3 needs the real audio end to
  // recognize a final frame without a following header.
  unsigned char tag[3];
  if (size >= 128 && read_at(fd, tag, sizeof(tag), size - 128) &&
      !memcmp(tag, "TAG", 3))
    size -= 128;
  // APEv2's size includes its footer and excludes its optional header.
  // Layout reference: FFmpeg libavformat/apetag.c (ff_ape_parse_tag).
  unsigned char footer[32];
  if (size >= 32 && read_at(fd, footer, 32, size - 32) &&
      !memcmp(footer, "APETAGEX", 8)) {
    uint64_t bytes = le32(footer + 12);
    uint32_t flags = le32(footer + 20), version = le32(footer + 8);
    if ((version != 1000 && version != 2000) || bytes < 32 || bytes > size ||
        (flags & 0x60000000u))
      return false;
    uint64_t start = size - bytes;
    if (flags & 0x80000000u) {
      unsigned char header[32];
      if (start < 32 || !read_at(fd, header, 32, start - 32) ||
          memcmp(header, "APETAGEX", 8) || le32(header + 8) != version ||
          le32(header + 12) != bytes || !(le32(header + 20) & 0x20000000u))
        return false;
      start -= 32;
    }
    size = start;
  }
  uint64_t at = 0, base = 0;
  size_t length = 0;
  // Skip a bounded, synchsafe ID3v2 tag without mistaking album artwork for
  // audio.
  if (size >= 10 && !memcmp(input, "ID3", 3)) {
    if ((input[6] | input[7] | input[8] | input[9]) & 0x80)
      return false;
    at = 10 + (uint64_t(input[6]) << 21) + (uint64_t(input[7]) << 14) +
         (uint64_t(input[8]) << 7) + input[9];
    if (input[5] & 0x10)
      at += 10;
    if (at >= size)
      return false;
  }
  mp3dec_t scanner{};
  mp3dec_init(&scanner);
  while (at < size) {
    if (progress && !progress(opaque, at, size)) {
      error = Error::Cancelled;
      return false;
    }
    if (at < base || at >= base + length ||
        (base + length - at < 8192 && base + length < size)) {
      base = at;
      length = size - at < BufferBytes ? size_t(size - at) : BufferBytes;
      if (!read_at(fd, input, length, at)) {
        error = Error::Io;
        return false;
      }
    }
    size_t available = length - size_t(at - base);
    mp3dec_frame_info_t f{};
    int samples = mp3dec_decode_frame(&scanner, input + (at - base),
                                      int(available), nullptr, &f);
    if (samples > 0 && f.frame_bytes > 0 && f.frame_offset >= 0 &&
        f.frame_bytes > f.frame_offset) {
      if (f.layer != 3 || f.hz < 8000 || f.hz > 48000 ||
          (f.channels != 1 && f.channels != 2)) {
        error = Error::Unsupported;
        return false;
      }
      if (!index.empty() && info.rate != unsigned(f.hz)) {
        error = Error::Unsupported;
        return false;
      }
      if (index.size() >= MaxFrames) {
        error = Error::TooLarge;
        return false;
      }
      Frame entry{at + unsigned(f.frame_offset), info.frames,
                  unsigned(f.frame_bytes - f.frame_offset), unsigned(samples)};
      if (entry.bytes > 4096 || !index.push(entry)) {
        error = Error::Memory;
        return false;
      }
      info.rate = f.hz;
      info.channels = f.channels;
      info.frames += samples;
    }
    size_t consumed = f.frame_bytes > 0 ? size_t(f.frame_bytes) : 0;
    if (consumed > available)
      return false;
    if (!consumed)
      consumed = available > 4096 ? available - 4096 : available;
    at += consumed;
  }
  info.kind = Kind::Mp3;
  info.bits = 16;
  mp3dec_init(&mp3);
  return !index.empty();
}
Decoder::~Decoder() { close(); }
void Decoder::close() {
  if (p_) {
    p_->~Impl();
    free(p_);
    p_ = nullptr;
  }
}
bool Decoder::open(const char *path, Error &error, Progress progress,
                   void *opaque) {
  close();
  error = Error::Invalid;
  void *memory = malloc(sizeof(Impl));
  if (!memory) {
    error = Error::Memory;
    return false;
  }
  p_ = new (memory, rt::Placement{}) Impl;
  p_->fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  struct stat st{};
  if (p_->fd < 0 || fstat(p_->fd, &st) < 0) {
    error = Error::Io;
    close();
    return false;
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 12) {
    close();
    return false;
  }
  if (uint64_t(st.st_size) > MaxBytes) {
    error = Error::TooLarge;
    close();
    return false;
  }
  p_->size = st.st_size;
  if (progress && !progress(opaque, 0, p_->size)) {
    error = Error::Cancelled;
    close();
    return false;
  }
  if (!read_at(p_->fd, p_->input, 12, 0)) {
    error = Error::Io;
    close();
    return false;
  }
  bool wav = !memcmp(p_->input, "RIFF", 4) && !memcmp(p_->input + 8, "WAVE", 4);
  if (!(wav ? p_->wav(error, progress, opaque)
            : p_->scan(error, progress, opaque))) {
    close();
    return false;
  }
  error = Error::Ok;
  return true;
}
const Info &Decoder::info() const {
  static const Info empty;
  return p_ ? p_->info : empty;
}
uint64_t Decoder::position() const { return p_ ? p_->position : 0; }
int Decoder::Impl::frame(Error &error) {
  if (next >= index.size())
    return 0;
  const Frame &entry = index[next++];
  size_t available = size - entry.offset < entry.bytes + 4096
                         ? size_t(size - entry.offset)
                         : entry.bytes + 4096;
  if (!read_at(fd, input, available, entry.offset)) {
    error = Error::Io;
    return -1;
  }
  mp3dec_frame_info_t f{};
  int samples = mp3dec_decode_frame(&mp3, input, int(available), pcm, &f);
  if (samples != int(entry.samples) || f.frame_offset ||
      f.frame_bytes != int(entry.bytes)) {
    // Reservoir priming and isolated damaged frames have silent, bounded
    // duration.
    if (++failures > 20) {
      error = Error::Invalid;
      return -1;
    }
    memset(pcm, 0, entry.samples * 2 * sizeof(int16_t));
  } else {
    failures = 0;
    if (f.channels == 1)
      for (int n = samples - 1; n >= 0; --n)
        pcm[2 * n] = pcm[2 * n + 1] = pcm[n];
  }
  pcm_at = 0;
  pcm_count = entry.samples;
  return int(pcm_count);
}
bool Decoder::seek(uint64_t target, Error &error) {
  error = Error::Ok;
  if (!p_) {
    error = Error::Invalid;
    return false;
  }
  if (target > p_->info.frames)
    target = p_->info.frames;
  p_->pcm_at = p_->pcm_count = p_->failures = 0;
  if (p_->info.kind == Kind::Wav) {
    p_->position = target;
    return true;
  }
  size_t low = 0, high = p_->index.size();
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (p_->index[middle].start <= target)
      low = middle + 1;
    else
      high = middle;
  }
  size_t selected = low ? low - 1 : 0;
  p_->next = selected > 20 ? selected - 20 : 0;
  p_->position = p_->index[p_->next].start;
  mp3dec_init(&p_->mp3);
  while (p_->position < target) {
    int n = p_->frame(error);
    if (n <= 0)
      return false;
    uint64_t remaining = target - p_->position;
    unsigned consume =
        remaining < p_->pcm_count ? unsigned(remaining) : p_->pcm_count;
    p_->pcm_at = consume;
    p_->position += consume;
  }
  return true;
}
int Decoder::read(int16_t *stereo, unsigned frames, Error &error) {
  error = Error::Ok;
  if (!p_ || (!stereo && frames)) {
    error = Error::Invalid;
    return -1;
  }
  if (!frames || p_->position >= p_->info.frames)
    return 0;
  if (frames > 4096)
    frames = 4096;
  if (p_->info.kind == Kind::Mp3) {
    unsigned count = 0;
    while (count < frames && p_->position < p_->info.frames) {
      if (p_->pcm_at == p_->pcm_count && p_->frame(error) <= 0)
        return -1;
      unsigned n = p_->pcm_count - p_->pcm_at;
      if (n > frames - count)
        n = frames - count;
      memcpy(stereo + count * 2, p_->pcm + p_->pcm_at * 2, n * 4);
      p_->pcm_at += n;
      p_->position += n;
      count += n;
    }
    return int(count);
  }
  if (frames > p_->info.frames - p_->position)
    frames = unsigned(p_->info.frames - p_->position);
  if (!read_at(p_->fd, p_->input, frames * p_->align,
               p_->data + p_->position * p_->align)) {
    error = Error::Io;
    return -1;
  }
  for (unsigned n = 0; n < frames; ++n) {
    for (unsigned channel = 0; channel < p_->info.channels; ++channel) {
      const auto *at =
          p_->input + n * p_->align + channel * (p_->info.bits / 8);
      int32_t value = 0;
      if (p_->format == 3) {
        float f;
        memcpy(&f, at, 4);
        if (!isfinite(f))
          value = 0;
        else if (f >= 1)
          value = 32767;
        else if (f <= -1)
          value = -32768;
        else
          value = int32_t(f * 32768.f);
      } else if (p_->info.bits == 8)
        value = (int(at[0]) - 128) * 256;
      else if (p_->info.bits == 16)
        value = int16_t(le16(at));
      else if (p_->info.bits == 24)
        value = int16_t(le16(at + 1));
      else
        value = int16_t(le16(at + 2));
      stereo[n * 2 + channel] = int16_t(value);
    }
    if (p_->info.channels == 1)
      stereo[n * 2 + 1] = stereo[n * 2];
  }
  p_->position += frames;
  return int(frames);
}
} // namespace rp
