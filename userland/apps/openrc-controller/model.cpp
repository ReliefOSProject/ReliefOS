#include "model.h"
#include <ctype.h>
namespace oc {
bool RefreshModel::accept_catalog(const char *text) {
  ready_ = false;
  if (!parse_catalog(text, pending_))
    return false;
  ready_ = true;
  return true;
}
bool RefreshModel::accept_state(const char *text, Catalog &visible) {
  if (!ready_ || !parse_status(text, pending_))
    return false;
  visible = static_cast<Catalog &&>(pending_);
  ready_ = false;
  return true;
}
bool append(Text &to, const char *data, size_t size) {
  if (size > 4 * MaxPayload || to.size() > 4 * MaxPayload - size)
    return false;
  char *p = static_cast<char *>(malloc(to.size() + size + 1));
  if (!p)
    return false;
  memcpy(p, to.c_str(), to.size());
  memcpy(p + to.size(), data, size);
  bool ok = to.set(p, to.size() + size);
  free(p);
  return ok;
}
bool identifier(const char *s) {
  if (!s || !isalnum(static_cast<unsigned char>(*s)) || strlen(s) > 128)
    return false;
  for (; *s; ++s)
    if (!isalnum(static_cast<unsigned char>(*s)) && !strchr("_.@+-", *s))
      return false;
  return true;
}
bool protected_level(const char *s) {
  return !strcmp(s, "sysinit") || !strcmp(s, "boot") || !strcmp(s, "single") ||
         !strcmp(s, "shutdown") || !strcmp(s, "reboot") ||
         !strcmp(s, "default");
}
static bool one_of(const Text &s, const char *choices) {
  return has_word(choices, s.c_str());
}
bool validate(const Request &r, Text &error) {
  const Text *fields[] = {&r.a, &r.b, &r.c, &r.d};
  for (const Text *field : fields)
    if (memchr(field->c_str(), 0, field->size())) {
      error.set("NUL bytes are not allowed in a request.");
      return false;
    }
  bool ok = false;
  const bool name = identifier(r.a.c_str()), level = identifier(r.b.c_str());
  switch (r.op) {
  case Op::Catalog:
  case Op::Rebuild:
  case Op::Report:
  case Op::Cancel:
  case Op::Quit:
    ok = r.a.empty() && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::State:
    ok = r.a.empty() ||
         one_of(r.a, "all crashed manual supervised unused runlevel");
    ok = ok && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::Service:
    ok = name && identifier(r.b.c_str()) &&
         (r.c.empty() || one_of(r.c, "dry-run ifstarted ifstopped ifinactive "
                                     "ifnotstarted ifcrashed nodeps debug")) &&
         r.d.empty();
    break;
  case Op::Add:
  case Op::Remove:
  case Op::StackAdd:
  case Op::StackDel:
    ok = name && level && r.c.empty() && r.d.empty();
    if (r.op == Op::StackAdd)
      ok = ok && r.a != r.b && !protected_level(r.a.c_str()) &&
           (!protected_level(r.b.c_str()) || r.b == "default");
    break;
  case Op::Depend:
    ok = name &&
         one_of(r.b, "ineed iuse iwant iafter ibefore needsme usesme wantsme "
                     "beforeme afterme") &&
         r.c.empty() && r.d.empty();
    break;
  case Op::ReadConfig:
    ok =
        (name || r.a == "@global") && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::WriteConfig:
  case Op::RestoreConfig:
    ok = (name || r.a == "@global") && r.b.size() <= 256 * 1024 &&
         r.c.size() <= 256 * 1024 && one_of(r.d, "present missing") &&
         !memchr(r.b.c_str(), 0, r.b.size()) &&
         !memchr(r.c.c_str(), 0, r.c.size());
    break;
  case Op::ReadScript:
    ok = name && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::Logs:
    ok = one_of(r.a, "rc.log messages syslog boot.log") && r.b.empty() &&
         r.c.empty() && r.d.empty();
    break;
  case Op::CreateLevel:
  case Op::DeleteLevel:
    ok = name && !protected_level(r.a.c_str()) && r.b.empty() && r.c.empty() &&
         r.d.empty();
    break;
  case Op::SwitchLevel:
    ok = name && (!protected_level(r.a.c_str()) || r.a == "default") &&
         (r.b.empty() || r.b == "no-stop") && r.c.empty() && r.d.empty();
    break;
  }
  if (!ok)
    error.set("Invalid or unsupported OpenRC request.");
  return ok;
}
static bool arg(Array<Text> &v, const char *s) {
  Text t;
  return t.set(s) && v.push(t);
}
bool command(const Request &r, Array<Text> &v, Text &error) {
  v.clear();
  if (!validate(r, error))
    return false;
  switch (r.op) {
  case Op::State:
    if (!arg(v, "/bin/rc-status") || !arg(v, "--nocolor"))
      return false;
    if (!(r.a == "crashed") && !(r.a == "runlevel"))
      if (!arg(v, "--format") || !arg(v, "ini"))
        return false;
    return arg(v, r.a == "all"          ? "--all"
                  : r.a == "manual"     ? "--manual"
                  : r.a == "supervised" ? "--supervised"
                  : r.a == "unused"     ? "--unused"
                  : r.a == "crashed"    ? "--crashed"
                  : r.a == "runlevel"   ? "--runlevel"
                                        : "--servicelist");
  case Op::Service:
    if (!arg(v, "/sbin/rc-service"))
      return false;
    if (!r.c.empty()) {
      Text flag;
      if (!flag.set("--") || !append(flag, r.c.c_str()) ||
          !arg(v, flag.c_str()))
        return false;
    }
    return arg(v, r.a.c_str()) && arg(v, r.b.c_str());
  case Op::Add:
  case Op::Remove:
  case Op::StackAdd:
  case Op::StackDel:
    if (!arg(v, "/sbin/rc-update"))
      return false;
    if ((r.op == Op::StackAdd || r.op == Op::StackDel) && !arg(v, "--stack"))
      return false;
    return arg(v, (r.op == Op::Add || r.op == Op::StackAdd) ? "add" : "del") &&
           arg(v, r.a.c_str()) && arg(v, r.b.c_str());
  case Op::Depend:
    return arg(v, "/usr/libexec/rc/bin/rc-depend") && arg(v, "--type") &&
           arg(v, r.b.c_str()) && arg(v, r.a.c_str());
  case Op::Rebuild:
    return arg(v, "/sbin/rc-update") && arg(v, "--update");
  case Op::SwitchLevel:
    if (!arg(v, "/sbin/openrc"))
      return false;
    if (r.b == "no-stop" && !arg(v, "--no-stop"))
      return false;
    return arg(v, r.a.c_str());
  default:
    error.set("This request does not execute a command.");
    return false;
  }
}
static bool u32(Text &b, unsigned v) {
  uint32_t n = v;
  return append(b, reinterpret_cast<const char *>(&n), 4);
}
static bool field(Text &b, const Text &s) {
  return u32(b, s.size()) && append(b, s.c_str(), s.size());
}
bool encode_request(const Request &r, Text &b) {
  b.clear();
  return u32(b, static_cast<unsigned>(r.op)) && field(b, r.a) &&
         field(b, r.b) && field(b, r.c) && field(b, r.d) &&
         b.size() <= MaxPayload;
}
static bool read32(const char *&p, size_t &left, unsigned &v) {
  if (left < 4)
    return false;
  uint32_t n;
  memcpy(&n, p, 4);
  v = n;
  p += 4;
  left -= 4;
  return true;
}
bool decode_request(const Text &b, Request &r) {
  const char *p = b.c_str();
  size_t n = b.size();
  unsigned op, len;
  Request next;
  if (!read32(p, n, op) || op < static_cast<unsigned>(Op::Catalog) ||
      op > static_cast<unsigned>(Op::Quit))
    return false;
  next.op = static_cast<Op>(op);
  Text *fields[] = {&next.a, &next.b, &next.c, &next.d};
  for (auto *f : fields) {
    if (!read32(p, n, len) || len > n || !f->set(p, len))
      return false;
    p += len;
    n -= len;
  }
  Text error;
  if (n || !validate(next, error))
    return false;
  r = static_cast<Request &&>(next);
  return true;
}
bool encode_packet(const Packet &p, Text &b) {
  b.clear();
  return p.body.size() <= MaxPayload && u32(b, 0x4f524343) &&
         u32(b, p.body.size()) && u32(b, static_cast<unsigned>(p.kind)) &&
         u32(b, p.id) && append(b, p.body.c_str(), p.body.size());
}
int take_packet(Text &b, Packet &result) {
  if (b.size() < 16)
    return 0;
  const char *p = b.c_str();
  size_t left = b.size();
  unsigned magic, len, kind, id;
  if (!read32(p, left, magic) || !read32(p, left, len) ||
      !read32(p, left, kind) || !read32(p, left, id))
    return -1;
  if (magic != 0x4f524343 || len > MaxPayload || kind < 1 || kind > 4)
    return -1;
  if (left < len)
    return 0;
  Packet next;
  next.kind = static_cast<Kind>(kind);
  next.id = id;
  Text rest;
  if (!next.body.set(p, len) || !rest.set(p + len, left - len))
    return -1;
  result = static_cast<Packet &&>(next);
  b = rest;
  return 1;
}
bool has_word(const char *s, const char *word) {
  size_t n = strlen(word);
  if (!n)
    return false;
  while (*s) {
    while (*s == ' ' || *s == ',' || *s == '\t')
      ++s;
    const char *p = s;
    while (*s && *s != ' ' && *s != ',' && *s != '\t')
      ++s;
    if (static_cast<size_t>(s - p) == n && !memcmp(p, word, n))
      return true;
  }
  return false;
}
static Service *find(Catalog &c, const Text &name) {
  for (size_t i = 0; i < c.services.size(); ++i)
    if (c.services[i].name == name)
      return &c.services[i];
  Service s;
  s.name = name;
  s.state.set("unknown");
  if (c.services.size() >= 16384 || !c.services.push(s))
    return nullptr;
  return &c.services[c.services.size() - 1];
}
bool parse_catalog(const char *s, Catalog &c) {
  Catalog next;
  while (*s) {
    const char *end = strchr(s, '\n');
    if (!end)
      end = s + strlen(s);
    if (end - s < 2 || s[1] != '\t')
      return false;
    Text name;
    const char *tab =
        static_cast<const char *>(memchr(s + 2, '\t', end - s - 2));
    if (!name.set(s + 2, (tab ? tab : end) - (s + 2)))
      return false;
    if (!identifier(name.c_str()))
      return false;
    if (s[0] == 'C')
      next.current = name;
    else if (s[0] == 'L') {
      if (!next.levels.push(name))
        return false;
    } else if (s[0] == 'S') {
      if (!tab)
        return false;
      Service *service = find(next, name);
      if (!service || !service->levels.set(tab + 1, end - tab - 1))
        return false;
    } else
      return false;
    s = *end ? end + 1 : end;
  }
  c = static_cast<Catalog &&>(next);
  return true;
}
static bool trim(Text &t, const char *p, size_t n) {
  while (n && isspace(static_cast<unsigned char>(*p))) {
    ++p;
    --n;
  }
  while (n && isspace(static_cast<unsigned char>(p[n - 1])))
    --n;
  return t.set(p, n);
}
bool parse_status(const char *s, Catalog &c) {
  while (*s) {
    const char *end = strchr(s, '\n');
    if (!end)
      end = s + strlen(s);
    const char *eq = static_cast<const char *>(memchr(s, '=', end - s));
    if (eq) {
      Text name, status;
      if (!trim(name, s, eq - s) || !trim(status, eq + 1, end - eq - 1))
        return false;
      if (identifier(name.c_str())) {
        Service *item = find(c, name);
        if (!item)
          return false;
        const char *space = strchr(status.c_str(), ' ');
        if (!item->state.set(status.c_str(),
                             space ? static_cast<size_t>(space - status.c_str())
                                   : status.size()))
          return false;
        item->uptime.clear();
        if (space && !trim(item->uptime, space + 1, strlen(space + 1)))
          return false;
      }
    }
    s = *end ? end + 1 : end;
  }
  return true;
}
static bool contains(const char *s, const char *word) {
  if (!*word)
    return true;
  for (; *s; ++s) {
    const char *p = s, *q = word;
    while (*p && *q &&
           tolower(static_cast<unsigned char>(*p)) ==
               tolower(static_cast<unsigned char>(*q))) {
      ++p;
      ++q;
    }
    if (!*q)
      return true;
  }
  return false;
}
bool matches(const Service &s, const char *search, unsigned filter,
             const char *level) {
  if (!contains(s.name.c_str(), search) && !contains(s.levels.c_str(), search))
    return false;
  if (*level && !has_word(s.levels.c_str(), level))
    return false;
  if (filter == 1)
    return s.state == "started";
  if (filter == 2)
    return s.state == "stopped";
  if (filter == 3)
    return s.state == "crashed" || s.state == "failed" ||
           s.state == "unsupervised";
  if (filter == 4)
    return s.levels.empty();
  if (filter == 5)
    return s.state == "starting" || s.state == "stopping" ||
           s.state == "scheduled" || s.state == "inactive";
  return true;
}
} // namespace oc
