#include "window.h"
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_ask.H>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
namespace nc {
namespace {
enum Purpose { Sample = 1, Load, Write, Mutation, Display, RestoreJob };
double seconds() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
class MainWindow : public Fl_Double_Window {
  App &app_;

public:
  MainWindow(App &a)
      : Fl_Double_Window(1000, 670, T("Network Controller")), app_(a) {}
  void resize(int x, int y, int w, int h) override {
    Fl_Double_Window::resize(x, y, w, h);
    app_.layout();
  }
};
void item(Fl_Menu_Button *m, const char *label, Action a, int shortcut = 0) {
  m->add(
      label, shortcut,
      [](Fl_Widget *w, void *p) {
        static_cast<App *>(w->window()->user_data())
            ->action(static_cast<Action>(reinterpret_cast<intptr_t>(p)));
      },
      reinterpret_cast<void *>(static_cast<intptr_t>(a)));
}
Fl_Button *button(const char *label, Action a) {
  auto *b = new Fl_Button(0, 0, 100, 30, label);
  b->callback(
      [](Fl_Widget *w, void *p) {
        static_cast<App *>(w->window()->user_data())
            ->action(static_cast<Action>(reinterpret_cast<intptr_t>(p)));
      },
      reinterpret_cast<void *>(static_cast<intptr_t>(a)));
  return b;
}
void bytes(double value, char *b, size_t cap, bool rate = false) {
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  unsigned unit = 0;
  while (value >= 1024 && unit < 4) {
    value /= 1024;
    ++unit;
  }
  snprintf(b, cap, "%.1f %s%s", value, units[unit], rate ? "/s" : "");
}
} // namespace
App::App(Session &s) : session_(s) {
  window_ = new MainWindow(*this);
  window_->xclass("reliefos-network-controller");
  window_->user_data(this);
  window_->callback(
      [](Fl_Widget *, void *p) { static_cast<App *>(p)->action(Exit); }, this);
  auto *file = new Fl_Menu_Button(10, 8, 80, 28, T("File"));
  item(file, T("Diagnostic report"), Report);
  item(file, T("Copy output"), Copy, FL_CTRL + FL_SHIFT + 'c');
  item(file, T("Export output…"), Export, FL_CTRL + FL_SHIFT + 's');
  item(file, T("Close"), Exit, FL_ALT + FL_F + 4);
  auto *network = new Fl_Menu_Button(98, 8, 96, 28, T("Network"));
  item(network, T("Refresh"), Refresh, FL_F + 5);
  item(network, T("Save and apply"), SaveApply, FL_CTRL + 's');
  item(network, T("Renew DHCP"), Renew);
  item(network, T("Enable Ethernet"), Enable);
  item(network, T("Disable Ethernet"), Disable);
  item(network, T("Reload configuration"), Reload);
  item(network, T("Restore previous configuration"), Restore);
  auto *tools = new Fl_Menu_Button(202, 8, 80, 28, T("Tools"));
  item(tools, T("Routing table"), Routes);

  item(tools, T("TCP connections"), Connections);
  item(tools, T("DHCP lease"), Lease);
  item(tools, T("DNS resolver"), Resolver);
  item(tools, T("DHCP log"), Log);
  item(tools, T("Diagnostic report"), Report);
  auto *help = new Fl_Menu_Button(290, 8, 80, 28, T("Help"));
  item(help, T("Usage and limitations"), Help);
  item(help, T("About"), About);
  menus_[0] = file;
  menus_[1] = network;
  menus_[2] = tools;
  menus_[3] = help;
  refresh_ = button(T("Refresh"), Refresh);
  apply_ = button(T("Save and apply"), SaveApply);
  renew_ = button(T("Renew DHCP"), Renew);
  cancel_ = button(T("Cancel operation"), Cancel);
  automatic_ = new Fl_Check_Button(0, 0, 150, 30, T("Auto refresh"));
  automatic_->value(1);
  adapter_label_ = new Fl_Box(0, 0, 100, 22, T("Network adapters"));
  adapter_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  adapters_ = new Fl_Hold_Browser(0, 0, 100, 100);
  adapters_->textsize(15);
  adapters_->callback(dispatch, this);
  tabs_ = new Fl_Tabs(0, 0, 100, 100);
  pages_[0] = new Fl_Group(0, 0, 100, 100, T("Overview"));
  const char *labels[] = {
      T("State"),           T("IPv4 address"), T("Subnet mask"),
      T("Default gateway"), T("MAC address"),  T("MTU"),
      T("DNS servers"),     T("Receive rate"), T("Send rate"),
      T("Received total"),  T("Sent total"),   T("Packets / errors")};
  for (unsigned i = 0; i < 12; ++i) {
    information_[i] = new Fl_Output(0, 0, 100, 28, labels[i]);
    information_[i]->align(FL_ALIGN_TOP_LEFT);
  }
  overview_note_ =
      new Fl_Box(0, 0, 100, 55,
                 T("Live interface data. Configuration is managed by OpenRC; "
                   "diagnostics report actual results."));
  overview_note_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  pages_[0]->end();
  pages_[1] = new Fl_Group(0, 0, 100, 100, T("Configuration"));
  mode_ = new Fl_Choice(0, 0, 100, 30, T("Address mode"));
  mode_->add(T("Automatic (DHCP)"));
  mode_->add(T("Static IPv4"));
  mode_->add(T("Disabled"));
  address_ = new Fl_Input(0, 0, 100, 30, T("IPv4 address"));
  prefix_ = new Fl_Input(0, 0, 100, 30, T("Prefix length (0–32)"));
  gateway_ = new Fl_Input(0, 0, 100, 30, T("Gateway (optional)"));
  dns_mode_ = new Fl_Choice(0, 0, 100, 30, T("DNS policy"));
  dns_mode_->add(T("From DHCP"));
  dns_mode_->add(T("Custom servers"));
  dns_mode_->add(T("Cloudflare (1.1.1.1)"));
  servers_ = new Fl_Input(0, 0, 100, 30, T("DNS servers"));
  servers_->tooltip(T("Up to three IPv4 addresses separated by spaces."));
  Fl_Widget *fields[] = {mode_,    address_,  prefix_,
                         gateway_, dns_mode_, servers_};
  for (auto f : fields)
    f->callback(edited, this);
  address_->maximum_size(15);
  prefix_->maximum_size(2);
  gateway_->maximum_size(15);
  servers_->maximum_size(47);
  config_note_ = new Fl_Box(0, 0, 100, 70);
  config_note_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  save_ = button(T("Save only"), Save);
  restore_ = button(T("Restore previous…"), Restore);
  reload_ = button(T("Reload"), Reload);
  pages_[1]->end();
  pages_[1]->hide();
  pages_[2] = new Fl_Group(0, 0, 100, 100, T("Diagnostics"));
  host_ = new Fl_Input(0, 0, 100, 30, T("Host or IPv4"));
  host_->value("example.com");
  host_->maximum_size(253);
  dns_server_ = new Fl_Input(0, 0, 100, 30, T("DNS server (optional)"));
  dns_server_->maximum_size(15);
  port_ = new Fl_Input(0, 0, 100, 30, T("TCP port"));
  port_->value("443");
  port_->maximum_size(5);
  ping_ = button(T("Ping"), Ping);
  dns_button_ = button(T("DNS lookup"), DnsLookup);
  tcp_ = button(T("Test TCP port"), Tcp);
  subnet_label_ = new Fl_Box(0, 0, 100, 24, T("IPv4 subnet calculator"));
  subnet_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  subnet_ip_ = new Fl_Input(0, 0, 100, 30, T("IPv4 address"));
  subnet_ip_->value("192.168.1.100");
  subnet_ip_->maximum_size(15);
  subnet_prefix_ = new Fl_Input(0, 0, 100, 30, T("Prefix length"));
  subnet_prefix_->value("24");
  subnet_prefix_->maximum_size(2);
  calculate_ = button(T("Calculate"), Calculate);
  subnet_output_ = new Fl_Text_Display(0, 0, 100, 100);
  subnet_output_->buffer(subnet_buffer_);
  subnet_output_->textfont(FL_COURIER);
  subnet_output_->textsize(15);
  pages_[2]->end();
  pages_[2]->hide();
  pages_[3] = new Fl_Group(0, 0, 100, 100, T("Output"));
  output_ = new Fl_Text_Display(0, 0, 100, 100);
  output_->buffer(output_buffer_);
  output_->textfont(FL_COURIER);
  output_->textsize(15);
  output_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
  export_ = button(T("Export output…"), Export);
  clear_ = button(T("Clear output"), Clear);
  pages_[3]->end();
  pages_[3]->hide();
  tabs_->end();
  footer_ = new Fl_Box(0, 0, 100, 26);
  footer_->box(FL_THIN_DOWN_BOX);
  footer_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  window_->end();
  window_->size_range(920, 630);
  window_->resizable(tabs_);
  reliefos_fltk::style(window_);
  layout();
  populate(Config{});
  controls();
  Fl::add_timeout(.025, timer, this);
  enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
  enqueue(Op::ReadConfig, Load, T("Read configuration"));
}
App::~App() {
  closing_ = true;
  Fl::remove_timeout(timer, this);
  session_.stop();
  delete window_;
}
void App::show() { window_->show(); }
void App::layout() {
  if (!footer_)
    return;
  int w = window_->w(), h = window_->h();
  const int mx[] = {10, 98, 202, 290}, mw[] = {80, 96, 80, 80};
  for (unsigned n = 0; n < 4; ++n)
    menus_[n]->resize(mx[n], 8, mw[n], 28);
  refresh_->resize(10, 44, 102, 30);
  apply_->resize(122, 44, 142, 30);
  renew_->resize(274, 44, 124, 30);
  cancel_->resize(408, 44, 146, 30);
  automatic_->resize(w - 166, 44, 156, 30);
  adapter_label_->resize(10, 88, 178, 22);
  adapters_->resize(10, 116, 178, h - 158);
  int x = 200, y = 88, cw = w - x - 10, ch = h - y - 42;
  tabs_->resize(x, y, cw, ch);
  for (auto p : pages_)
    p->resize(x, y + 30, cw, ch - 30);
  int half = (cw - 54) / 2;
  for (unsigned i = 0; i < 12; ++i)
    information_[i]->resize(x + 18 + (i % 2) * (half + 18),
                            y + 65 + (i / 2) * 63, half, 29);
  bool counters = false;
  for (size_t n = 0; n < interfaces_.size(); ++n)
    if (interfaces_[n].name == selected_)
      counters = interfaces_[n].counters;
  overview_note_->resize(x + 16, y + (counters ? 448 : 330), cw - 32,
                         counters ? ch - 458 : 80);
  Fl_Widget *fields[] = {mode_,    address_,  prefix_,
                         gateway_, dns_mode_, servers_};
  for (unsigned i = 0; i < 6; ++i)
    fields[i]->resize(x + 182, y + 60 + i * 52, cw - 204, 30);
  config_note_->resize(x + 18, y + 378, cw - 36, ch - 440);
  save_->resize(x + 18, y + ch - 46, 118, 30);
  restore_->resize(x + 148, y + ch - 46, 164, 30);
  reload_->resize(x + 324, y + ch - 46, 112, 30);
  host_->resize(x + 190, y + 60, cw - 214, 30);
  dns_server_->resize(x + 190, y + 110, cw - 214, 30);
  port_->resize(x + 190, y + 160, 100, 30);
  ping_->resize(x + 18, y + 210, 100, 30);
  dns_button_->resize(x + 130, y + 210, 130, 30);
  tcp_->resize(x + 272, y + 210, 154, 30);
  subnet_label_->resize(x + 18, y + 268, cw - 36, 24);
  subnet_ip_->resize(x + 150, y + 310, 190, 30);
  subnet_prefix_->resize(x + 464, y + 310, 68, 30);
  calculate_->resize(x + 544, y + 310, 118, 30);
  subnet_output_->resize(x + 18, y + 358, cw - 36, ch - 378);
  output_->resize(x + 12, y + 42, cw - 24, ch - 100);
  export_->resize(x + 12, y + ch - 46, 162, 30);
  clear_->resize(x + 186, y + ch - 46, 130, 30);
  footer_->resize(10, h - 31, w - 20, 23);
  window_->redraw();
}
void App::status(const char *s) { footer_->copy_label(s); }
void App::log(const char *s) {
  if (output_buffer_.length() > 512 * 1024)
    output_buffer_.remove(0, output_buffer_.length() - 256 * 1024);
  output_buffer_.append(s);
  output_->insert_position(output_buffer_.length());
  output_->show_insert_position();
}
bool App::enqueue(const Request &r, unsigned purpose, const char *title) {
  if (queue_.size() >= 32 || closing_) {
    status(T("Too many queued operations."));
    return false;
  }
  Job j;
  j.request = r;
  j.purpose = purpose;
  if (!j.title.set(title) || !queue_.push(j))
    return false;
  next();
  return true;
}
void App::enqueue(Op op, unsigned purpose, const char *title, const char *a) {
  Request r;
  r.op = op;
  r.a.set(a);
  enqueue(r, purpose, title);
}
void App::next() {
  if (busy_ || queue_.empty() || !session_.ready())
    return;
  current_ = queue_[0];
  queue_.erase(0);
  response_.clear();
  if (!session_.send(current_.request, ++id_)) {
    queue_.clear();
    status(T("Cannot send the network operation."));
    return;
  }
  busy_ = true;
  status(current_.title.c_str());
  if (current_.purpose != Sample && current_.purpose != Load) {
    log("\n");
    log(current_.title.c_str());
    log("\n----------------------------------------\n");
  }
  controls();
}
void App::finish(int code) {
  unsigned purpose = current_.purpose;
  if (!code && purpose == Sample) {
    Array<Interface> next;
    Text gw, dns;
    if (parse_snapshot(response_, next, gw, dns)) {
      double now = seconds();
      rx_rate_ = tx_rate_ = 0;
      for (size_t a = 0; a < next.size(); ++a)
        if (next[a].name == selected_)
          for (size_t b = 0; b < interfaces_.size(); ++b)
            if (interfaces_[b].name == selected_ && sample_time_ > 0 &&
                now > sample_time_) {
              if (next[a].rx >= interfaces_[b].rx)
                rx_rate_ =
                    (next[a].rx - interfaces_[b].rx) / (now - sample_time_);
              if (next[a].tx >= interfaces_[b].tx)
                tx_rate_ =
                    (next[a].tx - interfaces_[b].tx) / (now - sample_time_);
            }
      sample_time_ = now;
      interfaces_.swap(next);
      default_gateway_ = gw;
      dns_ = dns;
      adapters_->clear();
      int selected = 0;
      for (size_t i = 0; i < interfaces_.size(); ++i) {
        char b[128];
        snprintf(b, sizeof(b), "%s  %s", interfaces_[i].name.c_str(),
                 interfaces_[i].flags & 1 ? T("Enabled") : T("Disabled"));
        adapters_->add(b);
        if (interfaces_[i].name == selected_)
          selected = i + 1;
      }
      if (!selected)
        for (size_t i = 0; i < interfaces_.size(); ++i)
          if (interfaces_[i].name == "eth0")
            selected = i + 1;
      if (!selected && !interfaces_.empty())
        selected = 1;
      adapters_->value(selected);
      if (selected)
        selected_ = interfaces_[selected - 1].name;
      overview();
    } else {
      code = 1;
      log(T("Malformed interface response.\n"));
    }
  } else if (!code && purpose == Load) {
    Config c;
    Text error;
    if (!decode_profile(response_, original_.exists, original_.bytes, c))
      code = 1;
    else {
      populate(c);
      loaded_ = true;
      dirty_ = false;
      Config original_config;
      if (original_.exists &&
          !parse_config(original_.bytes, original_config, error)) {
        dirty_ = true;
        code = 1;
        log(T(error.c_str()));
      }
    }
  } else if (!code && purpose == Write) {
    original_.exists = true;
    original_.bytes = current_.request.a;
    dirty_ = false;
  }
  if (purpose != Sample && purpose != Load) {
    char b[128];
    snprintf(b, sizeof(b), "\n%s (%d)\n",
             code ? T("Operation failed") : T("Completed"), code);
    log(b);
  }
  busy_ = false;
  response_.clear();
  last_refresh_ = seconds();
  if (code) {
    queue_.clear();
    status(T("Operation failed. See Output for details."));
    tabs_->value(pages_[3]);
    if (purpose == Mutation)
      fl_alert("%s",
               T("Applying the network failed. Saved settings remain on disk. "
                 "Check the output and restore or correct the configuration."));
  } else if (queue_.empty())
    status(dirty_ ? T("Unsaved configuration changes") : T("Ready"));
  controls();
  next();
}
void App::pump() {
  session_.pump();
  for (size_t i = 0; i < session_.events.size(); ++i) {
    const auto &p = session_.events[i];
    if (!busy_ || p.id != id_)
      continue;
    if (p.kind == Kind::Output) {
      if (current_.purpose == Sample || current_.purpose == Load) {
        if (!append(response_, p.body.c_str(), p.body.size())) {
          queue_.clear();
          session_.stop();
          status(T("Network response is too large."));
        }
      } else
        log(p.body.c_str());
    } else if (p.kind == Kind::Done)
      finish(atoi(p.body.c_str()));
  }
  session_.events.clear();
  if (!session_.alive()) {
    queue_.clear();
    busy_ = false;
    status(T("Network worker disconnected. Reopen the application to "
             "authenticate."));
    controls();
  } else if (automatic_->value() && !busy_ && queue_.empty() &&
             seconds() - last_refresh_ > 2)
    enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
}
void App::timer(void *p) {
  auto *a = static_cast<App *>(p);
  a->pump();
  if (!a->closing_)
    Fl::repeat_timeout(.025, timer, p);
}
void App::dispatch(Fl_Widget *w, void *p) {
  auto *a = static_cast<App *>(p);
  if (w == a->adapters_) {
    int i = a->adapters_->value() - 1;
    if (i >= 0 && static_cast<size_t>(i) < a->interfaces_.size()) {
      a->selected_ = a->interfaces_[i].name;
      a->rx_rate_ = a->tx_rate_ = 0;
      a->overview();
      a->controls();
    }
  }
}
void App::edited(Fl_Widget *widget, void *p) {
  auto *a = static_cast<App *>(p);
  if (a->setting_)
    return;
  if (widget == a->mode_ && a->mode_->value() == 1) {
    for (size_t n = 0; n < a->interfaces_.size(); ++n)
      if (a->interfaces_[n].name == "eth0") {
        if (!*a->address_->value() && !(a->interfaces_[n].address == "0.0.0.0"))
          a->address_->value(a->interfaces_[n].address.c_str());
        if (!*a->gateway_->value())
          a->gateway_->value(a->default_gateway_.c_str());
      }
    if (a->dns_mode_->value() == 0) {
      a->dns_mode_->value(1);
      if (!*a->servers_->value()) {
        char servers[64];
        snprintf(servers, sizeof(servers), "%s", a->dns_.c_str());
        for (char *q = servers; *q; ++q)
          if (*q == ',')
            *q = ' ';
        a->servers_->value(servers);
      }
    }
  }
  a->dirty_ = true;
  a->controls();
  a->status(T("Unsaved configuration changes"));
}
void App::overview() {
  const Interface *i = nullptr;
  for (size_t n = 0; n < interfaces_.size(); ++n)
    if (interfaces_[n].name == selected_)
      i = &interfaces_[n];
  for (auto f : information_)
    f->value("");
  if (!i)
    return;
  information_[0]->value(
      i->flags & 1
          ? (i->flags & 64 ? T("Connected") : T("Up, carrier not reported"))
          : T("Disabled"));
  information_[1]->value(i->address.c_str());
  information_[2]->value(i->mask.c_str());
  information_[3]->value(i->name == "eth0" ? default_gateway_.c_str() : "-");
  information_[4]->value(i->mac.c_str());
  char b[160];
  snprintf(b, sizeof(b), "%u", i->mtu);
  information_[5]->value(b);
  information_[6]->value(dns_.c_str());
  bytes(rx_rate_, b, sizeof(b), true);
  information_[7]->value(b);
  bytes(tx_rate_, b, sizeof(b), true);
  information_[8]->value(b);
  bytes(i->rx, b, sizeof(b));
  information_[9]->value(b);
  bytes(i->tx, b, sizeof(b));
  information_[10]->value(b);
  snprintf(b, sizeof(b), "RX %llu / %llu    TX %llu / %llu",
           static_cast<unsigned long long>(i->rx_packets),
           static_cast<unsigned long long>(i->rx_errors),
           static_cast<unsigned long long>(i->tx_packets),
           static_cast<unsigned long long>(i->tx_errors));
  information_[11]->value(b);
  for (unsigned n = 7; n < 12; ++n) {
    if (i->counters)
      information_[n]->show();
    else
      information_[n]->hide();
  }
  overview_note_->copy_label(
      i->counters
          ? T("Live interface data. Configuration is managed by OpenRC; "
              "diagnostics report actual results.")
          : T("Adapter counters are not exported by this kernel. Address, "
              "gateway, DNS and connection diagnostics remain available."));
  if (!i->counters)
    overview_note_->resize(tabs_->x() + 16, tabs_->y() + 330, tabs_->w() - 32,
                           80);
}
void App::populate(const Config &c) {
  setting_ = true;
  config_ = c;
  mode_->value(static_cast<int>(c.mode));
  dns_mode_->value(static_cast<int>(c.dns));
  char ip[16], b[64] = {};
  format_ip(c.address, ip);
  address_->value(c.address ? ip : "");
  format_ip(c.gateway, ip);
  gateway_->value(c.gateway ? ip : "");
  snprintf(b, sizeof(b), "%u", c.prefix);
  prefix_->value(b);
  b[0] = 0;
  for (unsigned n = 0; n < c.server_count; ++n) {
    format_ip(c.servers[n], ip);
    if (n)
      strcat(b, " ");
    strcat(b, ip);
  }
  servers_->value(b);
  setting_ = false;
}
void App::controls() {
  bool live = session_.ready() && session_.alive(), idle = live && !busy_,
       ethernet = selected_ == "eth0",
       editable = live && loaded_ && ethernet &&
                  (!busy_ || current_.purpose == Sample);
  Fl_Widget *general[] = {refresh_, renew_, reload_, ping_, dns_button_, tcp_};
  for (auto w : general)
    idle ? w->activate() : w->deactivate();
  Fl_Widget *mutations[] = {apply_, save_, restore_};
  for (auto w : mutations)
    (idle && loaded_ && ethernet) ? w->activate() : w->deactivate();
  busy_ ? cancel_->activate() : cancel_->deactivate();
  editable ? mode_->activate() : mode_->deactivate();
  editable ? dns_mode_->activate() : dns_mode_->deactivate();
  Fl_Widget *static_fields[] = {address_, prefix_, gateway_};
  for (auto w : static_fields)
    (editable && mode_->value() == 1) ? w->activate() : w->deactivate();
  (editable && dns_mode_->value() == 1) ? servers_->activate()
                                        : servers_->deactivate();
  config_note_->copy_label(
      !ethernet
          ? T("Persistent configuration is available for eth0. Loopback and "
              "other interfaces are shown for inspection.")
          : T("Settings: /etc/reliefos/network.conf\nSaving keeps a backup and "
              "detects external changes. Applying may briefly disconnect the "
              "network. Static mode requires explicit DNS."));
}
bool App::form(Config &c, Text &e) {
  c = Config{};
  c.mode = static_cast<Mode>(mode_->value());
  c.dns = static_cast<Dns>(dns_mode_->value());
  if (c.mode == Mode::Static) {
    if (*address_->value() && !ipv4(address_->value(), c.address)) {
      e.set("Invalid IPv4 address.");
      return false;
    }
    if (*gateway_->value() && !ipv4(gateway_->value(), c.gateway)) {
      e.set("Invalid IPv4 gateway.");
      return false;
    }
    const char *s = prefix_->value();
    if (!*s || strspn(s, "0123456789") != strlen(s) || strlen(s) > 2 ||
        atoi(s) > 32) {
      e.set("Invalid IPv4 prefix.");
      return false;
    }
    c.prefix = atoi(s);
  }
  if (c.dns == Dns::Custom) {
    char b[64];
    snprintf(b, sizeof(b), "%s", servers_->value());
    char *save = nullptr;
    for (char *p = strtok_r(b, " \t", &save); p;
         p = strtok_r(nullptr, " \t", &save)) {
      if (c.server_count == 3 || !ipv4(p, c.servers[c.server_count])) {
        e.set("Enter one to three IPv4 DNS servers.");
        return false;
      }
      ++c.server_count;
    }
  }
  return valid_config(c, e);
}
void App::save(bool apply) {
  if (busy_ || !loaded_ || !(selected_ == "eth0"))
    return;
  Config c;
  Text e, canonical;
  if (!form(c, e) || !serialize_config(c, canonical, e)) {
    fl_alert("%s", T(e.c_str()));
    tabs_->value(pages_[1]);
    return;
  }
  if (apply) {
    char ip[16], gw[16], message[2048];
    format_ip(c.address, ip);
    format_ip(c.gateway, gw);
    snprintf(message, sizeof(message), "%s\n\n%s: %s\n%s: %s/%u\n%s: %s\n\n%s",
             T("Save and apply this Ethernet configuration?"),
             T("Address mode"),
             c.mode == Mode::Dhcp     ? T("Automatic (DHCP)")
             : c.mode == Mode::Static ? T("Static IPv4")
                                      : T("Disabled"),
             T("IPv4 address"), ip, c.prefix, T("Default gateway"), gw,
             T("The current network connection may be interrupted."));
    if (fl_choice("%s", T("Cancel"), T("Apply"), nullptr, message) != 1)
      return;
  }
  Request r;
  r.op = Op::WriteConfig;
  r.a = canonical;
  r.b = original_.bytes;
  r.c.set(original_.exists ? "1" : "0");
  enqueue(r, Write, T("Save network configuration"));
  if (apply)
    enqueue(Op::Apply, Mutation, T("Apply Ethernet configuration"));
  enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
}
void App::restore() {
  if (busy_ || !loaded_ || !(selected_ == "eth0"))
    return;
  if (fl_choice("%s", T("Cancel"), T("Restore and apply"), nullptr,
                T("Restore the previous profile and apply it? The current "
                  "network connection may be interrupted.")) != 1)
    return;
  Request r;
  r.op = Op::RestoreConfig;
  r.b = original_.bytes;
  r.c.set(original_.exists ? "1" : "0");
  enqueue(r, RestoreJob, T("Restore previous configuration"));
  enqueue(Op::ReadConfig, Load, T("Read configuration"));
  enqueue(Op::Apply, Mutation, T("Apply Ethernet configuration"));
  enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
}
void App::export_output() {
  const char *path = fl_file_chooser(T("Export network diagnostics"), "*.txt",
                                     "network-diagnostics.txt");
  if (!path)
    return;
  if (access(path, F_OK) == 0 &&
      fl_choice("%s", T("Cancel"), T("Replace"), nullptr,
                T("Replace the existing file?")) != 1)
    return;
  FILE *f = fopen(path, "w");
  if (!f) {
    fl_alert("%s", strerror(errno));
    return;
  }
  char *text = output_buffer_.text();
  size_t n = strlen(text);
  bool ok = fwrite(text, 1, n, f) == n;
  if (fclose(f))
    ok = false;
  free(text);
  if (!ok)
    fl_alert("%s", T("Cannot export the diagnostic output."));
  else
    status(T("Output exported."));
}
void App::action(Action a) {
  if (a == Exit) {
    if (dirty_ &&
        fl_choice("%s", T("Cancel"), T("Discard"), nullptr,
                  T("Discard unsaved network settings and close?")) != 1)
      return;
    if (busy_ &&
        fl_choice("%s", T("Cancel"), T("Close"), nullptr,
                  T("Cancel the running operation and close? Configuration "
                    "changes already saved remain on disk.")) != 1)
      return;
    closing_ = true;
    session_.stop();
    window_->hide();
    return;
  }
  if (a == Calculate) {
    uint32_t ip;
    Subnet s{};
    const char *p = subnet_prefix_->value();
    if (!ipv4(subnet_ip_->value(), ip) || !*p ||
        strspn(p, "0123456789") != strlen(p) || strlen(p) > 2 ||
        !subnet(ip, atoi(p), s)) {
      fl_alert("%s", T("Enter an IPv4 address and a prefix from 0 to 32."));
      return;
    }
    char mask[16], net[16], broadcast[16], first[16], last[16], b[1024];
    format_ip(s.mask, mask);
    format_ip(s.network, net);
    format_ip(s.broadcast, broadcast);
    format_ip(s.first, first);
    format_ip(s.last, last);
    snprintf(b, sizeof(b), "%s: %s/%s\n%s: %s\n%s: %s\n%s: %s – %s\n%s: %llu",
             T("Network"), net, p, T("Subnet mask"), mask, T("Broadcast"),
             broadcast, T("Host range"), first, last, T("Usable addresses"),
             static_cast<unsigned long long>(s.hosts));
    subnet_buffer_.text(b);
    return;
  }
  if (a == Export) {
    export_output();
    return;
  }
  if (a == Clear) {
    output_buffer_.text("");
    return;
  }
  if (a == Copy) {
    char *s = output_buffer_.text();
    Fl::copy(s, strlen(s), 1);
    free(s);
    return;
  }
  if (a == About) {
    fl_message("%s", T("Network Controller\nIPv4 Ethernet configuration and "
                       "network diagnostics for ReliefOS.\nC++ / FLTK"));
    return;
  }
  if (a == Help) {
    fl_message("%s",
               T("Select an adapter to inspect its live state. Configure eth0 "
                 "with DHCP, static IPv4 or disabled mode. Save and apply uses "
                 "the existing OpenRC network service.\n\nStatic mode requires "
                 "custom or Cloudflare DNS. Use spaces between custom DNS "
                 "addresses. Saving detects external changes and keeps the "
                 "previous configuration.\n\nDiagnostics use real network "
                 "operations. TCP success confirms a connection, not TLS or "
                 "application health. Wi-Fi, IPv6, VPN and changing the "
                 "kernel's fixed MTU are not supported."));
    return;
  }
  if (a == Cancel) {
    Request r;
    r.op = Op::Cancel;
    queue_.clear();
    session_.send(r, 0);
    status(T("Cancellation requested; waiting for the running command."));
    return;
  }
  if (busy_) {
    status(T("Wait for the current operation or cancel it."));
    return;
  }
  if (a == Refresh) {
    enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
    return;
  }
  if (a == Save || a == SaveApply) {
    save(a == SaveApply);
    return;
  }
  if (a == Restore) {
    restore();
    return;
  }
  if (a == Reload) {
    if (dirty_ &&
        fl_choice("%s", T("Cancel"), T("Reload"), nullptr,
                  T("Discard unsaved edits and reload the network profile?")) !=
            1)
      return;
    enqueue(Op::ReadConfig, Load, T("Read configuration"));
    return;
  }
  if (a == Enable || a == Disable) {
    if (!(selected_ == "eth0") || !loaded_)
      return;
    mode_->value(a == Enable ? 0 : 2);
    dirty_ = true;
    controls();
    tabs_->value(pages_[1]);
    save(true);
    return;
  }
  if (a == Renew) {
    enqueue(Op::Renew, Mutation, T("Renew DHCP"));
    enqueue(Op::Snapshot, Sample, T("Refresh interfaces"));
    return;
  }
  if (a == Ping || a == DnsLookup || a == Tcp) {
    Request r;
    r.op = a == Ping ? Op::Ping : a == DnsLookup ? Op::DnsQuery : Op::Tcp;
    r.a.set(host_->value());
    if (a == DnsLookup)
      r.b.set(dns_server_->value());
    if (a == Tcp)
      r.b.set(port_->value());
    Text e;
    if (!validate(r, e)) {
      fl_alert(
          "%s",
          T("Enter a valid host, IPv4 DNS server and TCP port (1–65535)."));
      return;
    }
    tabs_->value(pages_[3]);
    enqueue(r, Display,
            a == Ping        ? T("Ping")
            : a == DnsLookup ? T("DNS lookup")
                             : T("TCP port test"));
    return;
  }
  const char *view = a == Routes        ? "routes"
                     : a == Arp         ? "arp"
                     : a == Connections ? "connections"
                     : a == Lease       ? "lease"
                     : a == Resolver    ? "resolver"
                     : a == Log         ? "log"
                                        : "report";
  tabs_->value(pages_[3]);
  enqueue(Op::View, Display,
          a == Report ? T("Diagnostic report") : T("Network information"),
          view);
}
} // namespace nc
