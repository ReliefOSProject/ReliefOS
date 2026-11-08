#include "media.h"
#include "../relieftodo/storage.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace rp {
namespace {
constexpr AVRational Milliseconds{1, 1000}, Samples{1, 48000};
Error media_error(int code) {
  if (code == AVERROR_EXIT)
    return Error::Cancelled;
  if (code == AVERROR(ENOMEM))
    return Error::Memory;
  if (code == AVERROR_DECODER_NOT_FOUND)
    return Error::Unsupported;
  if (code == AVERROR(EIO) || code == AVERROR(EACCES))
    return Error::Io;
  return Error::Invalid;
}
bool failure(const char *stage, int code, Error &error) {
  char text[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(code, text, sizeof(text));
  fprintf(stderr, "[reliefplayer.elf] media stage=%s code=%d detail=%s\n",
          stage, code, text);
  error = media_error(code);
  return false;
}
// Custom AVIO confines the demuxer to one already-open regular file. No URL,
// network protocol, external data reference or shell command is evaluated.
struct Input {
  int fd = -1, stream = -1;
  int64_t size = 0, origin = 0;
  AVFormatContext *format = nullptr;
  AVIOContext *io = nullptr;
  AVCodecContext *codec = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  Progress progress = nullptr;
  void *opaque = nullptr;
  bool flushed = false, ready = false;
  ~Input() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    if (io)
      av_freep(&io->buffer);
    avio_context_free(&io);
    if (fd >= 0)
      ::close(fd);
  }
  static int interrupt(void *p) {
    auto &s = *static_cast<Input *>(p);
    return s.progress && !s.progress(s.opaque, s.ready ? 1 : 0, 1);
  }
  static int read(void *p, uint8_t *bytes, int count) {
    auto &s = *static_cast<Input *>(p);
    if (interrupt(p))
      return AVERROR_EXIT;
    ssize_t n;
    do {
      n = ::read(s.fd, bytes, count);
    } while (n < 0 && errno == EINTR);
    return n > 0 ? int(n) : n == 0 ? AVERROR_EOF : AVERROR(errno);
  }
  static int64_t seek(void *p, int64_t offset, int whence) {
    auto &s = *static_cast<Input *>(p);
    if (whence & AVSEEK_SIZE)
      return s.size;
    off_t result = lseek(s.fd, offset, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
  }
  bool open(const char *path, AVMediaType type, Error &error, Progress cb,
            void *ctx) {
    progress = cb;
    opaque = ctx;
    fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) < 0) {
      error = Error::Io;
      return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 12) {
      error = Error::Invalid;
      return false;
    }
    size = st.st_size;
    format = avformat_alloc_context();
    auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
    if (!format || !buffer) {
      av_free(buffer);
      error = Error::Memory;
      return false;
    }
    io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
    if (!io) {
      av_free(buffer);
      error = Error::Memory;
      return false;
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback = {interrupt, this};
    format->probesize = 4 * 1024 * 1024;
    format->max_analyze_duration = 5 * AV_TIME_BASE;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "enable_drefs", "0", 0);
    av_dict_set(&options, "use_absolute_path", "0", 0);
    int n = avformat_open_input(&format, nullptr, av_find_input_format("mov"),
                                &options);
    av_dict_free(&options);
    if (n < 0)
      return failure("open MP4", n, error);
    n = avformat_find_stream_info(format, nullptr);
    if (n < 0)
      return failure("read MP4 streams", n, error);
    origin = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    const AVCodec *decoder = nullptr;
    stream = av_find_best_stream(format, type, -1, -1, &decoder, 0);
    ready = true;
    if (stream == AVERROR_STREAM_NOT_FOUND)
      return true;
    if (stream < 0)
      return failure("find codec", stream, error);
    auto *parameters = format->streams[stream]->codecpar;
    // Keep host validation and target builds on the same supported codec set.
    bool supported = type == AVMEDIA_TYPE_VIDEO
                         ? parameters->codec_id == AV_CODEC_ID_H264 ||
                               parameters->codec_id == AV_CODEC_ID_HEVC ||
                               parameters->codec_id == AV_CODEC_ID_MPEG4
                         : parameters->codec_id == AV_CODEC_ID_AAC ||
                               parameters->codec_id == AV_CODEC_ID_MP3;
    if (!supported) {
      error = Error::Unsupported;
      return false;
    }
    codec = avcodec_alloc_context3(decoder);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!codec || !packet || !frame) {
      error = Error::Memory;
      return false;
    }
    n = avcodec_parameters_to_context(codec, parameters);
    if (n < 0)
      return failure("codec parameters", n, error);
    codec->thread_count = 1;
    codec->max_pixels = 4096 * 2160;
    codec->pkt_timebase = format->streams[stream]->time_base;
    n = avcodec_open2(codec, decoder, nullptr);
    return n >= 0 || failure("open codec", n, error);
  }
  int next(Error &error) {
    for (unsigned tries = 0; tries < 4096; ++tries) {
      if (interrupt(this)) {
        error = Error::Cancelled;
        return -1;
      }
      av_frame_unref(frame);
      int n = avcodec_receive_frame(codec, frame);
      if (n >= 0)
        return 1;
      if (n == AVERROR_EOF)
        return 0;
      if (n != AVERROR(EAGAIN)) {
        failure("decode", n, error);
        return -1;
      }
      if (flushed) {
        error = Error::Invalid;
        return -1;
      }
      do {
        av_packet_unref(packet);
        n = av_read_frame(format, packet);
      } while (n >= 0 && packet->stream_index != stream && ++tries < 4096);
      if (tries >= 4096)
        break;
      if (n < 0 && n != AVERROR_EOF) {
        failure("read packet", n, error);
        return -1;
      }
      if (n == AVERROR_EOF)
        flushed = true;
      n = avcodec_send_packet(codec, flushed ? nullptr : packet);
      av_packet_unref(packet);
      if (n < 0) {
        failure("submit packet", n, error);
        return -1;
      }
    }
    error = Error::Invalid;
    return -1;
  }
  int64_t timestamp(AVRational units, int64_t fallback) const {
    if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
      return fallback;
    return av_rescale_q(frame->best_effort_timestamp,
                        format->streams[stream]->time_base, units) -
           av_rescale_q(origin, AV_TIME_BASE_Q, units);
  }
  bool reposition(uint64_t ms, Error &error) {
    int64_t at = int64_t(ms) * 1000 + origin;
    int n = av_seek_frame(format, -1, at, AVSEEK_FLAG_BACKWARD);
    if (n < 0)
      return failure("seek", n, error);
    avcodec_flush_buffers(codec);
    av_packet_unref(packet);
    av_frame_unref(frame);
    flushed = false;
    return true;
  }
};
template <class T> T *allocate(Error &error) {
  void *memory = malloc(sizeof(T));
  if (!memory) {
    error = Error::Memory;
    return nullptr;
  }
  return new (memory, rt::Placement{}) T;
}
template <class T> void release(T *&p) {
  if (p) {
    p->~T();
    free(p);
    p = nullptr;
  }
}
} // namespace
struct MediaAudio::Impl {
  Input input;
  SwrContext *resample = nullptr;
  rt::Array<int16_t> pcm;
  uint64_t position = 0, total = 0;
  int64_t start = 0, next = 0;
  unsigned count = 0;
  bool eof = false;
  ~Impl() { swr_free(&resample); }
};
MediaAudio::~MediaAudio() { release(p_); }
bool MediaAudio::open(const char *path, Info &info, Error &error, Progress cb,
                      void *ctx) {
  release(p_);
  p_ = allocate<Impl>(error);
  if (!p_ || !p_->input.open(path, AVMEDIA_TYPE_AUDIO, error, cb, ctx))
    return false;
  auto &in = p_->input;
  int video =
      av_find_best_stream(in.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    error = Error::Unsupported;
    return false;
  }
  auto *v = in.format->streams[video]->codecpar;
  if (v->width <= 0 || v->height <= 0 || v->width > 4096 || v->height > 2160) {
    error = Error::TooLarge;
    return false;
  }
  info.kind = Kind::Mp4;
  info.width = v->width;
  info.height = v->height;
  info.has_audio = in.stream >= 0;
  info.rate = info.has_audio ? 48000 : 1000;
  info.channels = info.has_audio ? 2 : 0;
  info.bits = info.has_audio ? 16 : 0;
  if (in.format->duration <= 0 ||
      in.format->duration > int64_t(7 * 86400) * AV_TIME_BASE) {
    error = Error::Invalid;
    return false;
  }
  info.frames =
      av_rescale_q(in.format->duration, AV_TIME_BASE_Q, {1, int(info.rate)});
  p_->total = info.frames;
  if (!info.has_audio)
    return true;
  auto *c = in.codec;
  if (c->sample_rate < 8000 || c->sample_rate > 192000 ||
      c->ch_layout.nb_channels < 1 || c->ch_layout.nb_channels > 8) {
    error = Error::Unsupported;
    return false;
  }
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  int n = swr_alloc_set_opts2(&p_->resample, &stereo, AV_SAMPLE_FMT_S16, 48000,
                              &c->ch_layout, c->sample_fmt, c->sample_rate, 0,
                              nullptr);
  if (n >= 0)
    n = swr_init(p_->resample);
  return n >= 0 || failure("audio conversion", n, error);
}
uint64_t MediaAudio::position() const { return p_ ? p_->position : 0; }
bool MediaAudio::seek(uint64_t frame, Error &error) {
  if (!p_) {
    error = Error::Invalid;
    return false;
  }
  p_->position = frame > p_->total ? p_->total : frame;
  p_->count = 0;
  p_->eof = p_->position == p_->total;
  p_->next = int64_t(p_->position);
  if (!p_->input.codec || p_->eof)
    return true;
  if (!p_->input.reposition(p_->position * 1000 / 48000, error))
    return false;
  swr_close(p_->resample);
  int n = swr_init(p_->resample);
  return n >= 0 || failure("reset audio conversion", n, error);
}
int MediaAudio::read(int16_t *out, unsigned frames, Error &error) {
  if (!p_ || !p_->resample) {
    error = Error::Invalid;
    return -1;
  }
  if (frames > p_->total - p_->position)
    frames = unsigned(p_->total - p_->position);
  unsigned written = 0;
  while (written < frames) {
    if (p_->count && int64_t(p_->position) >= p_->start + p_->count)
      p_->count = 0;
    if (!p_->count && !p_->eof) {
      int n = p_->input.next(error);
      if (n < 0)
        return -1;
      auto *f = p_->input.frame;
      auto *c = p_->input.codec;
      if (n &&
          (f->sample_rate != c->sample_rate || f->format != c->sample_fmt ||
           av_channel_layout_compare(&f->ch_layout, &c->ch_layout))) {
        error = Error::Unsupported;
        return -1;
      }
      int capacity = swr_get_out_samples(p_->resample, n ? f->nb_samples : 0);
      if (capacity < 0 || capacity > 1048576) {
        error = Error::TooLarge;
        return -1;
      }
      if (!p_->pcm.resize(size_t(capacity) * 2 + 2)) {
        error = Error::Memory;
        return -1;
      }
      auto *dest = reinterpret_cast<uint8_t *>(p_->pcm.data());
      int64_t delay = swr_get_delay(p_->resample, 48000);
      p_->start = n ? p_->input.timestamp(Samples, p_->next) - delay : p_->next;
      int samples = swr_convert(
          p_->resample, &dest, capacity,
          n ? const_cast<const uint8_t **>(f->extended_data) : nullptr,
          n ? f->nb_samples : 0);
      if (samples < 0) {
        failure("resample", samples, error);
        return -1;
      }
      p_->count = unsigned(samples);
      p_->next = p_->start + samples;
      if (!n)
        p_->eof = true;
      if (!p_->count && !p_->eof)
        continue;
    }
    unsigned n = frames - written;
    if (!p_->count || int64_t(p_->position) < p_->start) {
      if (p_->count && uint64_t(p_->start) - p_->position < n)
        n = unsigned(p_->start - p_->position);
      memset(out + written * 2, 0, n * 4);
    } else {
      unsigned offset = unsigned(int64_t(p_->position) - p_->start);
      if (offset >= p_->count) {
        p_->count = 0;
        continue;
      }
      if (n > p_->count - offset)
        n = p_->count - offset;
      memcpy(out + written * 2, p_->pcm.data() + offset * 2, n * 4);
    }
    p_->position += n;
    written += n;
  }
  return int(written);
}
struct VideoDecoder::Impl {
  Input input;
  SwsContext *scale = nullptr;
  rt::Array<unsigned char> rgb;
  bool pending = false, eof = false;
  int rotation = 0;
  rt::Array<unsigned char> rotated;
  int64_t at = 0, next = 0;
  ~Impl() { sws_freeContext(scale); }
};
VideoDecoder::~VideoDecoder() { release(p_); }
bool VideoDecoder::open(const char *path, Error &error, Progress cb,
                        void *ctx) {
  release(p_);
  p_ = allocate<Impl>(error);
  if (!p_ || !p_->input.open(path, AVMEDIA_TYPE_VIDEO, error, cb, ctx))
    return false;
  if (p_->input.stream < 0) {
    error = Error::Unsupported;
    return false;
  }
  auto *parameters = p_->input.format->streams[p_->input.stream]->codecpar;
  const auto *matrix = av_packet_side_data_get(parameters->coded_side_data,
                                               parameters->nb_coded_side_data,
                                               AV_PKT_DATA_DISPLAYMATRIX);
  if (matrix && matrix->size >= 9 * sizeof(int32_t)) {
    double angle = av_display_rotation_get(
        reinterpret_cast<const int32_t *>(matrix->data));
    if (isfinite(angle)) {
      int turn = int(lround(angle / 90));
      if (fabs(angle - turn * 90) < 1)
        p_->rotation = (turn % 4 + 4) % 4;
    }
  }
  return true;
}
bool VideoDecoder::seek(uint64_t ms, Error &error) {
  if (!p_ || !p_->input.reposition(ms, error))
    return false;
  p_->pending = p_->eof = false;
  p_->next = int64_t(ms);
  return true;
}
bool VideoDecoder::pump(uint64_t ms, VideoShared *shared, Error &error) {
  if (!p_ || !shared) {
    error = Error::Memory;
    return false;
  }
  auto &s = *p_;
  // A bounded batch permits control commands even when decoding a long GOP.
  for (unsigned frames = 0; frames < 12 && !s.eof; ++frames) {
    if (!s.pending) {
      int n = s.input.next(error);
      if (n < 0)
        return false;
      if (!n) {
        s.eof = true;
        break;
      }
      s.at = s.input.timestamp(Milliseconds, s.next);
      AVRational fps = av_guess_frame_rate(
          s.input.format, s.input.format->streams[s.input.stream],
          s.input.frame);
      int64_t step = fps.num > 0 && fps.den > 0
                         ? av_rescale_q(1, av_inv_q(fps), Milliseconds)
                         : 40;
      s.next = s.at + (step > 0 ? step : 1);
      s.pending = true;
    }
    if (s.at > int64_t(ms))
      break;
    auto *f = s.input.frame;
    if (f->width <= 0 || f->height <= 0 || f->width > 4096 ||
        f->height > 2160) {
      error = Error::TooLarge;
      return false;
    }
    // Apply pixel aspect ratio before fitting the RGB snapshot into its limit.
    AVRational aspect = av_guess_sample_aspect_ratio(
        s.input.format, s.input.format->streams[s.input.stream], f);
    double ratio = double(f->width) / f->height;
    if (aspect.num > 0 && aspect.den > 0)
      ratio *= av_q2d(aspect);
    if (ratio < .05 || ratio > 20) {
      error = Error::Unsupported;
      return false;
    }
    unsigned w = unsigned(f->height * ratio);
    if (w > VideoWidth)
      w = VideoWidth;
    unsigned h = unsigned(w / ratio);
    if (h > VideoHeight) {
      h = VideoHeight;
      w = unsigned(h * ratio);
    }
    if (!w)
      w = 1;
    if (!h)
      h = 1;
    if (!s.rgb.resize(size_t(w) * h * 3)) {
      error = Error::Memory;
      return false;
    }
    s.scale = sws_getCachedContext(
        s.scale, f->width, f->height, AVPixelFormat(f->format), int(w), int(h),
        AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!s.scale) {
      error = Error::Memory;
      return false;
    }
    int colorspace =
        f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int *coefficients = sws_getCoefficients(colorspace);
    sws_setColorspaceDetails(s.scale, coefficients,
                             f->color_range == AVCOL_RANGE_JPEG, coefficients,
                             1, 0, 1 << 16, 1 << 16);
    uint8_t *dest[4] = {s.rgb.data(), nullptr, nullptr, nullptr};
    int strides[4] = {int(w * 3), 0, 0, 0};
    int n =
        sws_scale(s.scale, f->data, f->linesize, 0, f->height, dest, strides);
    if (n != int(h)) {
      error = Error::Playback;
      return false;
    }
    unsigned output_w = (s.rotation & 1) ? h : w;
    unsigned output_h = (s.rotation & 1) ? w : h;
    const unsigned char *pixels = s.rgb.data();
    if (s.rotation) {
      if (!s.rotated.resize(size_t(w) * h * 3)) {
        error = Error::Memory;
        return false;
      }
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
          unsigned dx = s.rotation == 1   ? y
                        : s.rotation == 2 ? w - x - 1
                                          : h - y - 1;
          unsigned dy = s.rotation == 1   ? w - x - 1
                        : s.rotation == 2 ? h - y - 1
                                          : x;
          memcpy(s.rotated.data() + (dy * output_w + dx) * 3,
                 s.rgb.data() + (y * w + x) * 3, 3);
        }
      pixels = s.rotated.data();
    }
    // Portrait frames fit the same maximum byte count; validate both sides at
    // the reader using the larger dimension rather than assuming landscape.
    if (!__atomic_exchange_n(&shared->lock, 1u, __ATOMIC_ACQUIRE)) {
      memcpy(shared->pixels, pixels, size_t(output_w) * output_h * 3);
      shared->width = output_w;
      shared->height = output_h;
      ++shared->serial;
      __atomic_store_n(&shared->lock, 0u, __ATOMIC_RELEASE);
    } else
      return true; // Retry this frame if a paused seek races a GUI copy.
    s.pending = false;
  }
  return true;
}
} // namespace rp
