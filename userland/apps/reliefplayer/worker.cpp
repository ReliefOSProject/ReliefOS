#include "worker.h"
#include "../relieftodo/storage.h"
#include "pcm.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
namespace rp {
uint64_t monotonic_ms() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
const char *state_name(State state) {
  switch (state) {
  case State::Idle:
    return "idle";
  case State::Loading:
    return "loading";
  case State::Playing:
    return "playing";
  case State::Paused:
    return "paused";
  case State::Stopped:
    return "stopped";
  case State::Ended:
    return "ended";
  case State::Failed:
    return "failed";
  }
  return "failed";
}
namespace {
struct Control {
  int fd = -1;
  bool quit = false, playing = true, reset = false, seeking = false,
       stopped = false;
  bool play_requested = false;
  uint64_t target = 0;
  unsigned volume = 70, used = 0;
  Command incoming;
  void read_commands() {
    for (unsigned tries = 0; tries < 64; ++tries) {
      ssize_t n = read(fd, reinterpret_cast<char *>(&incoming) + used,
                       sizeof(incoming) - used);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
      if (n <= 0) {
        quit = true;
        return;
      }
      used += unsigned(n);
      if (used < sizeof(incoming))
        continue;
      used = 0;
      if (incoming.magic != Protocol) {
        quit = true;
        return;
      }
      switch (incoming.op) {
      case Op::Quit:
        quit = true;
        return;
      case Op::Volume:
        if (playing &&
            volume != (incoming.value > 100 ? 100 : unsigned(incoming.value)))
          reset = true;
        volume = incoming.value > 100 ? 100 : unsigned(incoming.value);
        fprintf(stderr, "[reliefplayer.elf] volume=%u\n", volume);
        fflush(stderr);
        break;
      case Op::Play:
        playing = incoming.value != 0;
        play_requested = playing;
        reset = true;
        stopped = false;
        break;
      case Op::Stop:
        play_requested = false;
        playing = false;
        target = 0;
        seeking = true;
        reset = true;
        stopped = true;
        break;
      case Op::Seek:
        play_requested = false;
        target = incoming.value;
        seeking = true;
        reset = true;
        stopped = false;
        fprintf(stderr, "[reliefplayer.elf] seek=%llu\n",
                (unsigned long long)target);
        fflush(stderr);
        break;
      }
    }
  }
};
struct Reporter {
  int fd;
  PlaybackStatus status;
  uint64_t last = 0;
  State logged = State::Idle;
  bool send(bool force = false) {
    uint64_t now = monotonic_ms();
    if (!force && now - last < 100)
      return true;
    if (status.state != logged) {
      fprintf(stderr,
              "[reliefplayer.elf] state=%s position=%llu total=%llu rate=%u "
              "channels=%u written_bytes=%llu error=%u errno=%d\n",
              state_name(status.state), (unsigned long long)status.position,
              (unsigned long long)status.info.frames, status.info.rate,
              status.info.channels, (unsigned long long)status.written_bytes,
              unsigned(status.error), status.system_error);
      fflush(stderr);
      logged = status.state;
    }
    ssize_t n = write(fd, &status, sizeof(status));
    if (n == ssize_t(sizeof(status))) {
      last = now;
      return true;
    }
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }
};
struct Loading {
  Control *control;
  Reporter *reporter;
};
bool loading(void *opaque, uint64_t at, uint64_t total) {
  auto &p = *static_cast<Loading *>(opaque);
  p.control->read_commands();
  p.reporter->status.progress = total ? unsigned(at * 100 / total) : 0;
  return !p.control->quit && p.reporter->send();
}
class Audio {
  int fd_ = -1;
  PcmStream *stream_ = nullptr;
  uint64_t base_ = 0, total_ = 0, written_ = 0, heard_ = 0, last_activity_ = 0;
  uint32_t source_rate_ = 0, rate_ = 0, channels_ = 0;
  unsigned pending_ = 0, offset_ = 0;
  bool draining_ = false, drain_started_ = false;
  unsigned fragment_bytes_ = 8192;
  int16_t buffer_[2048 * 16];
  bool configure(unsigned long request, int *value, const char *stage) {
    if (ioctl(fd_, request, value) >= 0)
      return true;
    int saved = errno;
    fprintf(stderr, "[reliefplayer.elf] device stage=%s errno=%d\n", stage,
            saved);
    fflush(stderr);
    errno = saved;
    return false;
  }

public:
  ~Audio() { close(); }
  void close() {
    if (fd_ >= 0) {
      ioctl(fd_, SNDCTL_DSP_RESET, 0);
      ::close(fd_);
      fd_ = -1;
    }
    if (stream_) {
      stream_->~PcmStream();
      free(stream_);
      stream_ = nullptr;
    }
    pending_ = offset_ = 0;
    draining_ = drain_started_ = false;
  }
  bool active() const { return fd_ >= 0; }
  uint64_t written() const { return written_; }
  uint32_t rate() const { return rate_; }
  uint32_t channels() const { return channels_; }
  uint64_t position() {
    if (fd_ < 0)
      return heard_;
    int delay = 0;
    if (ioctl(fd_, SNDCTL_DSP_GETODELAY, &delay) < 0 || delay < 0)
      return heard_;
    uint64_t queued = uint64_t(delay),
             played = written_ > queued ? written_ - queued : 0;
    uint64_t frame = base_ + (played / (channels_ * 2)) * source_rate_ / rate_;
    if (frame > total_)
      frame = total_;
    if (frame > heard_) {
      heard_ = frame;
      last_activity_ = monotonic_ms();
    }
    return heard_;
  }
  bool open(Decoder &decoder, uint64_t target, Error &error) {
    close();
    base_ = heard_ = target;
    written_ = 0;
    total_ = decoder.info().frames;
    source_rate_ = decoder.info().rate;
    fd_ = ::open("/dev/dsp", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      fprintf(stderr, "[reliefplayer.elf] device stage=open errno=%d\n", errno);
      fflush(stderr);
      error = errno == EBUSY ? Error::Busy : Error::Device;
      return false;
    }
    int format = AFMT_S16_LE, channels = 2, rate = 48000,
        fragment = (64 << 16) | 13;
    if (!configure(SNDCTL_DSP_SETFRAGMENT, &fragment, "fragment") ||
        !configure(SNDCTL_DSP_SETFMT, &format, "format") ||
        format != AFMT_S16_LE ||
        !configure(SNDCTL_DSP_CHANNELS, &channels, "channels") ||
        channels < 2 || channels > 16 ||
        !configure(SNDCTL_DSP_SPEED, &rate, "rate") || rate < 8000 ||
        rate > 192000) {
      int saved = errno;
      close();
      errno = saved;
      error = Error::Device;
      return false;
    }
    int block = 0;
    if (!configure(SNDCTL_DSP_GETBLKSIZE, &block, "block") || block <= 0 ||
        unsigned(block) > sizeof(buffer_) || block % (channels * 2)) {
      int saved = errno;
      close();
      errno = saved;
      error = Error::Device;
      return false;
    }
    fragment_bytes_ = unsigned(block);
    rate_ = unsigned(rate);
    channels_ = unsigned(channels);
    void *memory = malloc(sizeof(PcmStream));
    if (!memory) {
      close();
      error = Error::Memory;
      return false;
    }
    stream_ =
        new (memory, rt::Placement{}) PcmStream(decoder, rate_, channels_);
    if (!stream_->seek(target, error)) {
      close();
      return false;
    }
    last_activity_ = monotonic_ms();
    fprintf(stderr, "[reliefplayer.elf] output rate=%u channels=%u\n", rate_,
            channels_);
    fflush(stderr);
    return true;
  }
  // One bounded render/write operation; commands are checked between calls.
  int pump(unsigned volume, Error &error) {
    if (monotonic_ms() - last_activity_ > 5000) {
      error = Error::Playback;
      errno = ETIMEDOUT;
      return -1;
    }
    if (!pending_ && !draining_) {
      int n = stream_->render(buffer_, 2048, volume, error);
      if (n < 0)
        return -1;
      pending_ = unsigned(n) * channels_ * 2;
      offset_ = 0;
      draining_ = !n;
      if (draining_) {
        // Complete the last hardware period with silence using the same
        // nonblocking short-write path. Never enter the blocking OSS SYNC.
        pending_ = unsigned((fragment_bytes_ - written_ % fragment_bytes_) %
                            fragment_bytes_);
        memset(buffer_, 0, pending_);
      }
    }
    if (pending_) {
      ssize_t n = write(fd_, reinterpret_cast<const char *>(buffer_) + offset_,
                        pending_ - offset_);
      if (n > 0 && size_t(n) <= pending_ - offset_) {
        offset_ += unsigned(n);
        written_ += unsigned(n);
        last_activity_ = monotonic_ms();
        if (offset_ == pending_)
          pending_ = offset_ = 0;
        return 2; // More work can be queued without an unconditional sleep.
      }
      if (!n || (n < 0 &&
                 (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)))
        return 0;
      error = Error::Playback;
      return -1;
    }
    if (draining_) {
      if (!drain_started_) {
        int trigger = PCM_ENABLE_OUTPUT;
        if (ioctl(fd_, SNDCTL_DSP_SETTRIGGER, &trigger) < 0 &&
            errno != ENOTTY) {
          error = Error::Playback;
          return -1;
        }
        drain_started_ = true;
      }
      int delay = 0;
      if (ioctl(fd_, SNDCTL_DSP_GETODELAY, &delay) < 0 || delay < 0) {
        error = Error::Playback;
        return -1;
      }
      return delay ? 0 : 1;
    }
    return 0;
  }
};
} // namespace
int worker(const char *path, int commands, int statuses, unsigned volume) {
  signal(SIGPIPE, SIG_IGN);
  Control control;
  control.fd = commands;
  control.volume = volume;
  Reporter report{statuses, {}};
  report.status.state = State::Loading;
  report.send(true);
  Loading progress{&control, &report};
  Decoder decoder;
  Error error = Error::Ok;
  if (!decoder.open(path, error, loading, &progress)) {
    if (error == Error::Cancelled || control.quit)
      return 0;
    report.status.state = State::Failed;
    report.status.error = error;
    report.status.system_error = error == Error::Io ? errno : 0;
    report.send(true);
    return 1;
  }
  report.status.info = decoder.info();
  report.status.progress = 100;
  Audio audio;
  uint64_t target = 0;
  State state = State::Stopped;
  unsigned recoveries = 0;
  while (!control.quit) {
    bool progressed = false;
    control.read_commands();
    if (control.quit)
      break;
    if (control.reset) {
      if (control.seeking)
        target = control.target > decoder.info().frames ? decoder.info().frames
                                                        : control.target;
      else if (audio.active())
        target = audio.position();
      audio.close();
      if (target == decoder.info().frames && !control.play_requested) {
        state = State::Ended;
        control.playing = false;
      } else
        state = control.stopped ? State::Stopped : State::Paused;
      control.reset = control.seeking = false;
      control.play_requested = false;
    }
    if (control.playing && !audio.active()) {
      if (target >= decoder.info().frames)
        target = 0;
      errno = 0;
      if (!audio.open(decoder, target, error)) {
        report.status.error = error;
        report.status.system_error = errno;
        state = State::Failed;
        control.playing = false;
      } else {
        state = State::Playing;
        report.status.error = Error::Ok;
        report.status.system_error = 0;
      }
    }
    if (control.playing && audio.active()) {
      errno = 0;
      int n = audio.pump(control.volume, error);
      int pump_error = errno;
      target = audio.position();
      report.status.written_bytes = audio.written();
      report.status.output_rate = audio.rate();
      report.status.output_channels = audio.channels();
      if (n == 2)
        progressed = true;
      else if (n == 1) {
        target = decoder.info().frames;
        audio.close();
        state = State::Ended;
        control.playing = false;
      } else if (n < 0) {
        int saved = pump_error;
        if ((saved == EPIPE || saved == ESTRPIPE) && recoveries++ < 8) {
          audio.close();
          report.status.recoveries = recoveries;
          fprintf(
              stderr,
              "[reliefplayer.elf] recover count=%u position=%llu errno=%d\n",
              recoveries, (unsigned long long)target, saved);
        } else {
          audio.close();
          state = State::Failed;
          control.playing = false;
          report.status.error = error;
          report.status.system_error = saved;
        }
      }
    }
    bool changed = report.status.state != state;
    report.status.state = state;
    report.status.position = target;
    if (!report.send(changed))
      break;
    pollfd descriptor{commands, POLLIN, 0};
    if (!progressed)
      poll(&descriptor, 1, control.playing ? 5 : 50);
  }
  audio.close();
  fprintf(stderr, "[reliefplayer.elf] worker exit recoveries=%u\n", recoveries);
  fflush(stderr);
  return 0;
}
} // namespace rp
