#ifndef NETWORK_CONTROLLER_WINDOW_H
#define NETWORK_CONTROLLER_WINDOW_H
#include "../fltk_support.h"
#include "config.h"
#include "session.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Display.H>
namespace nc {
enum Action {
  Refresh = 1,
  Save,
  SaveApply,
  Restore,
  Renew,
  Disable,
  Enable,
  Ping,
  DnsLookup,
  Tcp,
  Calculate,
  Routes,
  Arp,
  Connections,
  Lease,
  Resolver,
  Log,
  Report,
  Export,
  Copy,
  Clear,
  Cancel,
  Help,
  About,
  Exit,
  Reload
};
struct Job {
  Request request;
  unsigned purpose = 0;
  Text title;
};
class App {
  Session &session_;
  Fl_Double_Window *window_;
  Fl_Hold_Browser *adapters_;
  Fl_Tabs *tabs_;
  Fl_Group *pages_[4];
  Fl_Check_Button *automatic_;
  Fl_Box *footer_ = nullptr, *adapter_label_, *overview_note_, *config_note_,
         *subnet_label_;
  Fl_Button *refresh_, *apply_, *renew_, *cancel_, *save_, *restore_, *reload_,
      *ping_, *dns_button_, *tcp_, *calculate_, *export_, *clear_;
  Fl_Widget *menus_[4];
  Fl_Output *information_[12];
  Fl_Choice *mode_, *dns_mode_;
  Fl_Input *address_, *prefix_, *gateway_, *servers_, *host_, *dns_server_,
      *port_, *subnet_ip_, *subnet_prefix_;
  Fl_Text_Buffer output_buffer_, subnet_buffer_;
  Fl_Text_Display *output_, *subnet_output_;
  Array<Interface> interfaces_;
  Array<Job> queue_;
  Job current_;
  Snapshot original_;
  Config config_;
  Text response_, default_gateway_, dns_, selected_;
  bool busy_ = false, dirty_ = false, loaded_ = false, setting_ = false,
       closing_ = false;
  unsigned id_ = 0;
  double sample_time_ = 0, last_refresh_ = 0, rx_rate_ = 0, tx_rate_ = 0;
  void enqueue(Op, unsigned, const char *, const char *a = "");
  bool enqueue(const Request &, unsigned, const char *);
  void next();
  void finish(int);
  void pump();
  void log(const char *);
  void status(const char *);
  void overview();
  void controls();
  void populate(const Config &);
  bool form(Config &, Text &);
  void save(bool apply);
  void restore();
  void export_output();
  static void timer(void *);
  static void dispatch(Fl_Widget *, void *);
  static void edited(Fl_Widget *, void *);

public:
  explicit App(Session &);
  ~App();
  void show();
  void action(Action);
  void layout();
};
bool authenticate(Session &);
} // namespace nc
#endif
