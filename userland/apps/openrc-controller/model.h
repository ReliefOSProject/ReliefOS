#ifndef OPENRC_CONTROLLER_MODEL_H
#define OPENRC_CONTROLLER_MODEL_H
#include "storage.h"
namespace oc {
constexpr size_t MaxPayload = 1024 * 1024;
constexpr const char *WorkerPath =
    "/usr/lib/reliefos/apps/openrc-controller/openrc-controller.elf";
enum class Op : unsigned {
  Catalog = 1,
  State,
  Service,
  Add,
  Remove,
  Depend,
  ReadConfig,
  WriteConfig,
  RestoreConfig,
  ReadScript,
  Logs,
  Rebuild,
  Report,
  CreateLevel,
  DeleteLevel,
  StackAdd,
  StackDel,
  SwitchLevel,
  Cancel,
  Quit
};
enum class Kind : unsigned { Request = 1, Output, Done, Ready };
struct Request {
  Op op = Op::Catalog;
  Text a, b, c, d;
};
struct Packet {
  Kind kind = Kind::Request;
  unsigned id = 0;
  Text body;
};
struct Service {
  Text name, state, uptime, levels;
};
struct Catalog {
  Array<Service> services;
  Array<Text> levels;
  Text current;
};
bool append(Text &, const char *, size_t);
inline bool append(Text &t, const char *s) { return append(t, s, strlen(s)); }
bool identifier(const char *);
bool protected_level(const char *);
bool validate(const Request &, Text &error);
bool command(const Request &, Array<Text> &args, Text &error);
bool encode_request(const Request &, Text &);
bool decode_request(const Text &, Request &);
bool encode_packet(const Packet &, Text &);
// 1: complete, 0: incomplete, -1: malformed. Consumes only a complete frame.
int take_packet(Text &buffer, Packet &);
bool parse_catalog(const char *, Catalog &);
bool parse_status(const char *, Catalog &);
// Keep the visible catalog intact until both asynchronous queries succeed.
class RefreshModel {
  Catalog pending_;
  bool ready_ = false;

public:
  bool accept_catalog(const char *);
  bool accept_state(const char *, Catalog &visible);
};
bool has_word(const char *, const char *);
bool matches(const Service &, const char *search, unsigned filter,
             const char *level);
} // namespace oc
#endif
