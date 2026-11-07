#ifndef RELIEFPLAYER_PROTOCOL_H
#define RELIEFPLAYER_PROTOCOL_H
#include "decoder.h"
namespace rp {
constexpr uint32_t Protocol = 0x31504c52;
enum class Op : uint32_t { Play, Stop, Seek, Volume, Quit };
enum class State : uint32_t {
  Idle,
  Loading,
  Playing,
  Paused,
  Stopped,
  Ended,
  Failed
};
struct Command {
  uint32_t magic = Protocol;
  Op op = Op::Quit;
  uint64_t value = 0;
};
struct PlaybackStatus {
  uint32_t magic = Protocol;
  State state = State::Idle;
  Error error = Error::Ok;
  int32_t system_error = 0;
  Info info;
  uint64_t position = 0, written_bytes = 0;
  uint32_t progress = 0, output_rate = 0, output_channels = 0, recoveries = 0;
};
static_assert(sizeof(Command) <= 512 && sizeof(PlaybackStatus) <= 512,
              "atomic pipe packets");
uint64_t monotonic_ms();
const char *state_name(State state);
} // namespace rp
#endif
