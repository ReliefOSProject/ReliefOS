#ifndef OPENRC_CONTROLLER_WINDOW_H
#define OPENRC_CONTROLLER_WINDOW_H
#include "../fltk_support.h"
#include "config.h"
#include "session.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
namespace oc {
class App;
class ServiceTable : public Fl_Table_Row {
  App &app_;
  void draw_cell(TableContext, int, int, int, int, int, int) override;

public:
  ServiceTable(int, int, int, int, App &);
};
enum Action {
  Refresh = 1,
  Start,
  Stop,
  Restart,
  Reload,
  ServiceStatus,
  Enable,
  Disable,
  Advanced,
  Description,
  Script,
  Dependencies,
  ServiceConfig,
  GlobalConfig,
  SaveConfig,
  RestoreConfig,
  ReloadConfig,
  CreateLevel,
  DeleteLevel,
  StackAdd,
  StackDel,
  SwitchLevel,
  AllStatus,
  Crashed,
  Manual,
  Supervised,
  Unused,
  Rebuild,
  RcLog,
  MessagesLog,
  Syslog,
  BootLog,
  Report,
  Export,
  ClearOutput,
  Cancel,
  SelectAll,
  Help,
  About,
  Exit
};
struct Job {
  Request request;
  unsigned purpose = 0;
  Text title;
};
class App {
  Session &session_;
  RefreshModel refresh_model_;
  Fl_Double_Window *window_ = nullptr;
  Fl_Hold_Browser *sidebar_ = nullptr;
  Fl_Input *search_ = nullptr;
  Fl_Check_Button *automatic_ = nullptr;
  Fl_Tabs *tabs_ = nullptr;
  Fl_Group *overview_ = nullptr, *dependency_ = nullptr,
           *configuration_ = nullptr, *output_ = nullptr;
  Fl_Text_Buffer detail_buffer_, dependency_buffer_, config_buffer_,
      output_buffer_;
  Fl_Text_Display *details_ = nullptr, *depend_display_ = nullptr,
                  *output_display_ = nullptr;
  Fl_Text_Editor *editor_ = nullptr;
  Fl_Box *summary_ = nullptr, *footer_ = nullptr, *config_path_ = nullptr;
  Array<Job> queue_;
  Job current_;
  bool busy_ = false, refreshing_ = false, loading_config_ = false,
       config_dirty_ = false, closing_ = false;
  unsigned request_id_ = 0, filter_ = 0, sort_ = 0;
  bool descending_ = false;
  Text response_, level_, config_target_;
  Snapshot config_original_;
  double last_refresh_ = 0;
  Array<Text> remembered_;
  void next();
  void finish(int);
  void refresh();
  bool enqueue(const Request &, unsigned purpose, const char *title);
  void log(const char *);
  void overview();
  void edit(const char *);
  void export_output();
  bool idle();
  bool confirm(const char *, const Array<Text> &);
  void selection(Array<Text> &);
  void run_services(Action);
  void runlevels(Action);
  static void tick(void *);
  static void dispatch(Fl_Widget *, void *);
  static void changed(Fl_Widget *, void *);
  static void table_event(Fl_Widget *, void *);
  static void config_changed(int, int, int, int, const char *, void *);

public:
  Catalog catalog;
  Array<size_t> visible;
  ServiceTable *table = nullptr;
  explicit App(Session &);
  ~App();
  void show();
  void action(Action);
  void filter();
  void sort(unsigned);
  const char *state_name(const char *) const;
};
bool authenticate(Session &);
} // namespace oc
#endif
