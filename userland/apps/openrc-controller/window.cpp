#include "window.h"
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <errno.h>
#include <stdio.h>
#include <time.h>
namespace oc {
namespace {
enum Purpose {
  Inventory = 1,
  State,
  View,
  Dependency,
  Edit,
  Save,
  Mutation,
  Diagnostic
};
double now() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
Request request(Op op, const char *a = "", const char *b = "",
                const char *c = "") {
  Request r;
  r.op = op;
  r.a.set(a);
  r.b.set(b);
  r.c.set(c);
  return r;
}
void menu_item(Fl_Menu_Button *m, const char *label, Action a,
               int shortcut = 0) {
  m->add(
      label, shortcut,
      [](Fl_Widget *w, void *p) {
        static_cast<App *>(w->window()->user_data())
            ->action(static_cast<Action>(reinterpret_cast<intptr_t>(p)));
      },
      reinterpret_cast<void *>(static_cast<intptr_t>(a)));
}
void text_display(Fl_Text_Display *d, Fl_Text_Buffer &b) {
  d->buffer(b);
  d->textfont(FL_COURIER);
  d->textsize(15);
  d->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
}
} // namespace
ServiceTable::ServiceTable(int x, int y, int w, int h, App &app)
    : Fl_Table_Row(x, y, w, h), app_(app) {
  cols(4);
  rows(0);
  col_header(1);
  col_header_height(30);
  row_header(0);
  row_height_all(30);
  col_width(0, 260);
  col_width(1, 130);
  col_width(2, 240);
  col_width(3, 180);
  col_resize(1);
  type(SELECT_MULTI);
  end();
}
void ServiceTable::draw_cell(TableContext context, int row, int col, int x,
                             int y, int w, int h) {
  if (context == CONTEXT_STARTPAGE) {
    fl_font(FL_HELVETICA, 15);
    return;
  }
  if (context != CONTEXT_CELL && context != CONTEXT_COL_HEADER)
    return;
  fl_push_clip(x, y, w, h);
  if (context == CONTEXT_COL_HEADER) {
    const char *headers[] = {T("Service"), T("State"), T("Runlevels"),
                             T("Uptime")};
    fl_draw_box(FL_THIN_UP_BOX, x, y, w, h, FL_BACKGROUND_COLOR);
    fl_color(FL_FOREGROUND_COLOR);
    fl_draw(headers[col], x + 10, y, w - 16, h, FL_ALIGN_LEFT);
  } else if (row >= 0 && static_cast<size_t>(row) < app_.visible.size()) {
    const Service &s = app_.catalog.services[app_.visible[row]];
    bool selected = row_selected(row);
    fl_color(selected ? selection_color() : FL_BACKGROUND2_COLOR);
    fl_rectf(x, y, w, h);
    fl_color(selected ? fl_contrast(FL_FOREGROUND_COLOR, selection_color())
                      : FL_FOREGROUND_COLOR);
    const char *value = col == 0   ? s.name.c_str()
                        : col == 1 ? app_.state_name(s.state.c_str())
                        : col == 2 ? s.levels.c_str()
                                   : s.uptime.c_str();
    fl_draw(value, x + 10, y, w - 16, h, FL_ALIGN_LEFT);
    fl_color(FL_LIGHT2);
    fl_line(x, y + h - 1, x + w, y + h - 1);
  }
  fl_pop_clip();
}
const char *App::state_name(const char *s) const {
  if (!strcmp(s, "started"))
    return T("Running");
  if (!strcmp(s, "stopped"))
    return T("Stopped");
  if (!strcmp(s, "starting"))
    return T("Starting");
  if (!strcmp(s, "stopping"))
    return T("Stopping");
  if (!strcmp(s, "crashed"))
    return T("Crashed");
  if (!strcmp(s, "failed"))
    return T("Failed");
  if (!strcmp(s, "inactive"))
    return T("Inactive");
  if (!strcmp(s, "scheduled"))
    return T("Scheduled");
  if (!strcmp(s, "unsupervised"))
    return T("Unsupervised");
  return T("Unknown");
}
App::App(Session &session) : session_(session) {
  window_ = new Fl_Double_Window(1180, 790, "OpenRC Controller");
  window_->user_data(this);
  window_->callback(
      [](Fl_Widget *, void *p) { static_cast<App *>(p)->action(Exit); }, this);
  auto *file = new Fl_Menu_Button(8, 8, 84, 30, T("File"));
  file->callback(dispatch);
  menu_item(file, T("Export output…"), Export, FL_CTRL + FL_SHIFT + 's');
  menu_item(file, T("Exit"), Exit, FL_ALT + FL_F + 4);
  auto *service = new Fl_Menu_Button(100, 8, 94, 30, T("Service"));
  service->callback(dispatch);
  menu_item(service, T("Start"), Start);
  menu_item(service, T("Stop"), Stop);
  menu_item(service, T("Restart"), Restart);
  menu_item(service, T("Reload"), Reload);
  menu_item(service, T("Service status"), ServiceStatus);
  menu_item(service, T("Advanced service action…"), Advanced);
  menu_item(service, T("Description and commands"), Description);
  menu_item(service, T("Read service script"), Script);
  menu_item(service, T("Dependencies…"), Dependencies);
  menu_item(service, T("Edit service configuration"), ServiceConfig);
  auto *levels = new Fl_Menu_Button(202, 8, 110, 30, T("Runlevels"));
  levels->callback(dispatch);
  menu_item(levels, T("Add services to runlevel…"), Enable);
  menu_item(levels, T("Remove services from runlevel…"), Disable);
  menu_item(levels, T("Create runlevel…"), CreateLevel);
  menu_item(levels, T("Delete empty runlevel…"), DeleteLevel);
  menu_item(levels, T("Stack runlevel…"), StackAdd);
  menu_item(levels, T("Unstack runlevel…"), StackDel);
  menu_item(levels, T("Switch runlevel…"), SwitchLevel);
  auto *tools = new Fl_Menu_Button(320, 8, 90, 30, T("Tools"));
  tools->callback(dispatch);
  menu_item(tools, T("All runlevel states"), AllStatus);
  menu_item(tools, T("Crashed services"), Crashed);
  menu_item(tools, T("Manually started services"), Manual);
  menu_item(tools, T("Supervised services"), Supervised);
  menu_item(tools, T("Unassigned services"), Unused);
  menu_item(tools, T("Rebuild dependency cache"), Rebuild);
  menu_item(tools, T("Edit global rc.conf"), GlobalConfig);
  menu_item(tools, T("OpenRC log"), RcLog);
  menu_item(tools, T("System messages"), MessagesLog);
  menu_item(tools, T("System log"), Syslog);
  menu_item(tools, T("Boot log"), BootLog);
  menu_item(tools, T("Diagnostic report"), Report);
  auto *help = new Fl_Menu_Button(418, 8, 88, 30, T("Help"));
  help->callback(dispatch);
  menu_item(help, T("Usage and precautions"), Help, FL_F + 1);
  menu_item(help, T("About"), About);
  automatic_ =
      new Fl_Check_Button(950, 8, 220, 30, T("Refresh every 10 seconds"));
  automatic_->value(1);
  const Action actions[] = {Refresh, Start,   Stop,     Restart, Reload,
                            Enable,  Disable, Advanced, Cancel};
  const char *labels[] = {T("Refresh"),  T("Start"),     T("Stop"),
                          T("Restart"),  T("Reload"),    T("Enable…"),
                          T("Disable…"), T("Advanced…"), T("Cancel operation")};
  for (unsigned i = 0; i < 9; ++i) {
    auto *b = new Fl_Button(8 + i * 112, 48, i == 8 ? 156 : 102, 32, labels[i]);
    b->callback(dispatch,
                reinterpret_cast<void *>(static_cast<intptr_t>(actions[i])));
  }
  Fl_Tile *tile = new Fl_Tile(8, 94, 1164, 658);
  auto *left = new Fl_Group(8, 94, 180, 658);
  sidebar_ = new Fl_Hold_Browser(8, 94, 180, 658);
  sidebar_->textsize(15);
  sidebar_->callback(changed, this);
  left->end();
  left->resizable(sidebar_);
  auto *right = new Fl_Group(198, 94, 974, 658);
  search_ = new Fl_Input(262, 94, 420, 32, T("Search"));
  search_->when(FL_WHEN_CHANGED);
  search_->callback(changed, this);
  summary_ = new Fl_Box(692, 94, 480, 32);
  summary_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  table = new ServiceTable(198, 136, 974, 274, *this);
  table->callback(table_event, this);
  table->when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
  tabs_ = new Fl_Tabs(198, 422, 974, 330);
  overview_ = new Fl_Group(198, 450, 974, 302, T("Overview"));
  auto *describe =
      new Fl_Button(212, 462, 166, 30, T("Description / commands"));
  describe->callback(dispatch, reinterpret_cast<void *>(Description));
  auto *script = new Fl_Button(390, 462, 148, 30, T("Read service script"));
  script->callback(dispatch, reinterpret_cast<void *>(Script));
  auto *configure = new Fl_Button(550, 462, 162, 30, T("Edit configuration"));
  configure->callback(dispatch, reinterpret_cast<void *>(ServiceConfig));
  details_ = new Fl_Text_Display(212, 504, 946, 234);
  text_display(details_, detail_buffer_);
  overview_->resizable(details_);
  overview_->end();
  dependency_ = new Fl_Group(198, 450, 974, 302, T("Dependencies"));
  auto *depend = new Fl_Button(212, 462, 180, 30, T("Query dependencies…"));
  depend->callback(dispatch, reinterpret_cast<void *>(Dependencies));
  depend_display_ = new Fl_Text_Display(212, 504, 946, 234);
  text_display(depend_display_, dependency_buffer_);
  dependency_->resizable(depend_display_);
  dependency_->end();
  configuration_ = new Fl_Group(198, 450, 974, 302, T("Configuration"));
  config_path_ =
      new Fl_Box(212, 462, 480, 30,
                 T("Choose a service configuration or global rc.conf."));
  config_path_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  auto *save = new Fl_Button(704, 462, 116, 30, T("Save"));
  save->callback(dispatch, reinterpret_cast<void *>(SaveConfig));
  save->shortcut(FL_CTRL + 's');
  auto *reload = new Fl_Button(832, 462, 116, 30, T("Reload"));
  reload->callback(dispatch, reinterpret_cast<void *>(ReloadConfig));
  auto *restore = new Fl_Button(960, 462, 198, 30, T("Restore backup"));
  restore->callback(dispatch, reinterpret_cast<void *>(RestoreConfig));
  editor_ = new Fl_Text_Editor(212, 504, 946, 234);
  editor_->buffer(config_buffer_);
  editor_->textfont(FL_COURIER);
  editor_->textsize(15);
  config_buffer_.add_modify_callback(config_changed, this);
  configuration_->resizable(editor_);
  configuration_->end();
  output_ = new Fl_Group(198, 450, 974, 302, T("Output"));
  auto *export_button = new Fl_Button(212, 462, 156, 30, T("Export output…"));
  export_button->callback(dispatch, reinterpret_cast<void *>(Export));
  auto *clear = new Fl_Button(380, 462, 110, 30, T("Clear"));
  clear->callback(dispatch, reinterpret_cast<void *>(ClearOutput));
  output_display_ = new Fl_Text_Display(212, 504, 946, 234);
  text_display(output_display_, output_buffer_);
  output_->resizable(output_display_);
  output_->end();
  tabs_->end();
  right->resizable(table);
  right->end();
  tile->end();
  tile->resizable(right);
  footer_ = new Fl_Box(8, 760, 1164, 24, T("Connecting to OpenRC…"));
  footer_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  window_->end();
  window_->resizable(tile);
  window_->size_range(1080, 720);
  reliefos_fltk::style(window_);
  tabs_->value(overview_);
  refresh();
}
App::~App() {
  Fl::remove_timeout(tick, this);
  delete window_;
}
void App::show() {
  window_->show();
  Fl::add_timeout(.03, tick, this);
}
void App::dispatch(Fl_Widget *w, void *p) {
  auto *app = static_cast<App *>(w->window()->user_data());
  // Menu item user data is delivered by the native FLTK menu callback.
  app->action(static_cast<Action>(reinterpret_cast<intptr_t>(p)));
}
void App::changed(Fl_Widget *w, void *p) {
  auto *app = static_cast<App *>(p);
  if (w == app->sidebar_) {
    int row = app->sidebar_->value();
    if (row < 1)
      return;
    if (row <= 6) {
      app->filter_ = row - 1;
      app->level_.clear();
    } else if (static_cast<size_t>(row - 7) < app->catalog.levels.size()) {
      app->filter_ = 0;
      app->level_ = app->catalog.levels[row - 7];
    }
  }
  app->filter();
}
void App::table_event(Fl_Widget *, void *p) {
  auto *app = static_cast<App *>(p);
  if (app->table->callback_context() == Fl_Table::CONTEXT_COL_HEADER)
    app->sort(app->table->callback_col());
  else
    app->overview();
}
void App::config_changed(int, int inserted, int deleted, int, const char *,
                         void *p) {
  auto *app = static_cast<App *>(p);
  if (!app->loading_config_ && (inserted || deleted))
    app->config_dirty_ = true;
}
void App::selection(Array<Text> &names) {
  for (size_t i = 0; i < visible.size(); ++i)
    if (table->row_selected(i))
      names.push(catalog.services[visible[i]].name);
}
void App::filter() {
  Array<Text> selected;
  selection(selected);
  visible.clear();
  for (size_t i = 0; i < catalog.services.size(); ++i)
    if (matches(catalog.services[i], search_->value(), filter_, level_.c_str()))
      visible.push(i);
  for (size_t i = 1; i < visible.size(); ++i) {
    size_t key = visible[i], j = i;
    auto field = [this](size_t n) -> const char * {
      const auto &s = catalog.services[n];
      return sort_ == 0   ? s.name.c_str()
             : sort_ == 1 ? s.state.c_str()
             : sort_ == 2 ? s.levels.c_str()
                          : s.uptime.c_str();
    };
    while (j) {
      int c = strcmp(field(visible[j - 1]), field(key));
      if (c == 0)
        c = strcmp(catalog.services[visible[j - 1]].name.c_str(),
                   catalog.services[key].name.c_str());
      if (descending_ ? c >= 0 : c <= 0)
        break;
      visible[j] = visible[j - 1];
      --j;
    }
    visible[j] = key;
  }
  table->rows(visible.size());
  table->select_all_rows(0);
  for (size_t i = 0; i < visible.size(); ++i)
    for (size_t j = 0; j < selected.size(); ++j)
      if (catalog.services[visible[i]].name == selected[j])
        table->select_row(i, 1);
  char info[192];
  snprintf(info, sizeof(info), T("%zu / %zu services · current: %s"),
           visible.size(), catalog.services.size(),
           catalog.current.empty() ? "?" : catalog.current.c_str());
  summary_->copy_label(info);
  table->redraw();
  overview();
}
void App::sort(unsigned column) {
  if (sort_ == column)
    descending_ = !descending_;
  else {
    sort_ = column;
    descending_ = false;
  }
  filter();
}
void App::overview() {
  Array<Text> selected;
  selection(selected);
  if (selected.empty()) {
    detail_buffer_.text(T(
        "Select services to inspect them. Ctrl/Shift selects multiple rows."));
    return;
  }
  Text text;
  for (size_t n = 0; n < selected.size(); ++n)
    for (size_t i = 0; i < catalog.services.size(); ++i)
      if (catalog.services[i].name == selected[n]) {
        const Service &s = catalog.services[i];
        char line[1024];
        snprintf(line, sizeof(line), "%s\n%s: %s\n%s: %s\n%s: %s\n\n",
                 s.name.c_str(), T("State"), state_name(s.state.c_str()),
                 T("Runlevels"),
                 s.levels.empty() ? T("Unassigned") : s.levels.c_str(),
                 T("Uptime"), s.uptime.empty() ? "—" : s.uptime.c_str());
        if (!append(text, line) || text.size() > 128 * 1024)
          break;
      }
  detail_buffer_.text(text.c_str());
}
void App::log(const char *s) {
  output_buffer_.append(s);
  if (output_buffer_.length() > 1024 * 1024) {
    int cut = output_buffer_.length() - 768 * 1024;
    cut = output_buffer_.utf8_align(cut);
    output_buffer_.remove(0, cut);
  }
  output_display_->insert_position(output_buffer_.length());
  output_display_->show_insert_position();
}
bool App::idle() {
  if (!session_.alive() || !session_.ready()) {
    fl_alert("%s",
             T("The privileged session has ended. Reopen the application."));
    return false;
  }
  if (busy_ || !queue_.empty()) {
    fl_alert("%s", T("Wait for the current operation or cancel it first."));
    return false;
  }
  return true;
}
bool App::enqueue(const Request &r, unsigned purpose, const char *title) {
  Text error;
  if (!validate(r, error)) {
    fl_alert("%s", T("Invalid service, runlevel, or operation."));
    return false;
  }
  Job job;
  job.request = r;
  job.purpose = purpose;
  job.title.set(title);
  if (!queue_.push(job)) {
    fl_alert("%s", T("Not enough memory."));
    return false;
  }
  return true;
}
void App::next() {
  if (busy_ || queue_.empty() || !session_.ready() || !session_.alive())
    return;
  current_ = queue_[0];
  queue_.erase(0);
  response_.clear();
  busy_ = true;
  ++request_id_;
  if (current_.purpose != Inventory && current_.purpose != State) {
    log("\n");
    log(current_.title.c_str());
    log("\n");
    if (current_.purpose == Mutation || current_.purpose == Diagnostic)
      tabs_->value(output_);
  }
  footer_->copy_label(current_.title.c_str());
  if (!session_.send(current_.request, request_id_)) {
    busy_ = false;
    queue_.clear();
    refreshing_ = false;
    fl_alert("%s", T("Cannot send request to the privileged worker."));
  }
}
void App::refresh() {
  if (busy_ || !queue_.empty() || !session_.alive())
    return;
  remembered_.clear();
  selection(remembered_);
  refreshing_ = true;
  last_refresh_ = now();
  enqueue(request(Op::Catalog), Inventory,
          T("Reading services and runlevels…"));
  enqueue(request(Op::State), State, T("Reading service states…"));
  next();
}
void App::finish(int code) {
  busy_ = false;
  if (code == 1 && current_.request.op == Op::State &&
      current_.request.a == "crashed") {
    log(T("No crashed services were reported."));
    log("\n");
    code = 0;
  }
  if (code) {
    queue_.clear();
    refreshing_ = false;
    char message[128];
    snprintf(
        message, sizeof(message),
        T("Operation failed (exit %d). Remaining operations were stopped."),
        code);
    footer_->copy_label(message);
    log(message);
    log("\n");
    if (!response_.empty() && current_.purpose != Mutation &&
        current_.purpose != Diagnostic)
      log(response_.c_str());
    tabs_->value(output_);
    return;
  }
  if (current_.purpose == Inventory) {
    if (!refresh_model_.accept_catalog(response_.c_str())) {
      queue_.clear();
      refreshing_ = false;
      footer_->copy_label(T("Cannot parse the OpenRC service catalog."));
      return;
    }
  } else if (current_.purpose == State) {
    remembered_.clear();
    selection(remembered_);
    if (!refresh_model_.accept_state(response_.c_str(), catalog)) {
      queue_.clear();
      footer_->copy_label(T("Cannot parse OpenRC status."));
      refreshing_ = false;
      return;
    }
    visible.clear();
    table->rows(0);
    sidebar_->clear();
    const char *labels[] = {T("All services"), T("Running"),
                            T("Stopped"),      T("Failed / crashed"),
                            T("Unassigned"),   T("In transition")};
    for (const char *label : labels)
      sidebar_->add(label);
    for (size_t i = 0; i < catalog.levels.size(); ++i) {
      Text label;
      label.set("  ");
      append(label, catalog.levels[i].c_str());
      sidebar_->add(label.c_str());
    }
    sidebar_->value(filter_ + 1);
    if (!level_.empty()) {
      bool found = false;
      for (size_t i = 0; i < catalog.levels.size(); ++i)
        if (catalog.levels[i] == level_) {
          sidebar_->value(i + 7);
          found = true;
        }
      if (!found)
        level_.clear();
    }
    refreshing_ = false;
    filter();
    for (size_t i = 0; i < visible.size(); ++i)
      for (size_t j = 0; j < remembered_.size(); ++j)
        if (catalog.services[visible[i]].name == remembered_[j])
          table->select_row(i, 1);
    overview();
    last_refresh_ = now();
    footer_->copy_label(T("Ready · administrator session active"));
  } else if (current_.purpose == Edit) {
    if (response_.empty() ||
        (response_.c_str()[0] != '0' && response_.c_str()[0] != '1')) {
      footer_->copy_label(T("Invalid configuration response."));
      return;
    }
    config_target_ = current_.request.a;
    config_original_.exists = response_.c_str()[0] == '1';
    config_original_.bytes.set(response_.c_str() + 1, response_.size() - 1);
    loading_config_ = true;
    config_buffer_.text(config_original_.bytes.c_str());
    loading_config_ = false;
    config_dirty_ = false;
    char path[256];
    snprintf(path, sizeof(path), "%s%s",
             config_target_ == "@global" ? "/etc/" : "/etc/conf.d/",
             config_target_ == "@global" ? "rc.conf" : config_target_.c_str());
    config_path_->copy_label(path);
    tabs_->value(configuration_);
  } else if (current_.purpose == View) {
    detail_buffer_.text(response_.c_str());
    tabs_->value(overview_);
  } else if (current_.purpose == Dependency) {
    dependency_buffer_.text(response_.c_str());
    tabs_->value(dependency_);
  } else if (current_.purpose == Save) {
    config_dirty_ = false;
    enqueue(request(Op::ReadConfig, config_target_.c_str()), Edit,
            T("Reloading saved configuration…"));
  } else if (current_.purpose == Mutation && queue_.empty()) {
    refresh();
    return;
  }
  if (!refreshing_ && queue_.empty())
    footer_->copy_label(T("Operation completed"));
  next();
}
void App::tick(void *p) {
  auto *app = static_cast<App *>(p);
  app->session_.pump();
  for (size_t i = 0; i < app->session_.events.size(); ++i) {
    const Packet &event = app->session_.events[i];
    if (!app->busy_ || event.id != app->request_id_)
      continue;
    if (event.kind == Kind::Output) {
      if (app->response_.size() + event.body.size() <= MaxPayload)
        append(app->response_, event.body.c_str(), event.body.size());
      if (app->current_.purpose == Mutation ||
          app->current_.purpose == Diagnostic)
        app->log(event.body.c_str());
    } else if (event.kind == Kind::Done)
      app->finish(atoi(event.body.c_str()));
  }
  app->session_.events.clear();
  if (!app->session_.alive()) {
    app->busy_ = false;
    app->queue_.clear();
    app->footer_->copy_label(
        T("Administrator session ended. Close and reopen the application."));
  } else {
    app->next();
    if (app->automatic_->value() && !app->busy_ && app->queue_.empty() &&
        now() - app->last_refresh_ >= 10)
      app->refresh();
  }
  if (app->window_->shown())
    Fl::repeat_timeout(.03, tick, p);
}
bool App::confirm(const char *message, const Array<Text> &names) {
  Text prompt;
  prompt.set(message);
  append(prompt, "\n\n");
  for (size_t i = 0; i < names.size() && i < 16; ++i) {
    append(prompt, names[i].c_str());
    append(prompt, "\n");
  }
  if (names.size() > 16)
    append(prompt, T("More selected services…"));
  return fl_choice("%s", T("Cancel"), T("Continue"), nullptr, prompt.c_str()) ==
         1;
}
void App::run_services(Action action) {
  if (!idle())
    return;
  Array<Text> names;
  selection(names);
  if (names.empty()) {
    fl_alert("%s", T("Select at least one service."));
    return;
  }
  if (names.size() > 512) {
    fl_alert("%s", T("Select at most 512 services per operation."));
    return;
  }
  const char *verb = action == Start         ? "start"
                     : action == Stop        ? "stop"
                     : action == Restart     ? "restart"
                     : action == Reload      ? "reload"
                     : action == Description ? "describe"
                                             : "status";
  Text level, extra, custom;
  if (action == Enable || action == Disable) {
    const char *value = fl_input(T("Runlevel name (use Tools / all runlevel "
                                   "states to inspect membership):"),
                                 level_.empty() ? "default" : level_.c_str());
    if (!value)
      return;
    level.set(value);
    if (!identifier(value)) {
      fl_alert("%s", T("Invalid runlevel name."));
      return;
    }
    bool exists = false;
    for (size_t i = 0; i < catalog.levels.size(); ++i)
      if (catalog.levels[i] == level)
        exists = true;
    if (!exists) {
      fl_alert("%s", T("Create this runlevel before adding services."));
      return;
    }
    if (!confirm(action == Enable
                     ? T("Add the selected services to this runlevel? This "
                         "changes startup configuration.")
                     : T("Remove the selected services from this runlevel? "
                         "This does not stop them immediately."),
                 names))
      return;
  } else if (action == Stop || action == Restart) {
    if (!confirm(T("This operation may interrupt networking, audio, or "
                   "services required by other programs. Continue?"),
                 names))
      return;
  } else if (action == Advanced) {
    Fl_Double_Window dialog(520, 244, T("Advanced service action"));
    Fl_Input cmd(150, 28, 342, 32, T("Command"));
    cmd.value("restart");
    Fl_Choice mode(150, 78, 342, 32, T("Condition / mode"));
    const char *modes[] = {T("Normal"),           T("Dry run"),
                           T("Only if started"),  T("Only if stopped"),
                           T("Only if inactive"), T("Only if not started"),
                           T("Only if crashed"),  T("Ignore dependencies"),
                           T("Debug output")};
    for (const char *m : modes)
      mode.add(m);
    mode.value(0);
    Fl_Box note(20, 126, 480, 46,
                T("Use an OpenRC command such as start, stop, reload, zap, or "
                  "a command advertised by the service."));
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    bool accepted = false;
    Fl_Button cancel(276, 192, 100, 32, T("Cancel"));
    cancel.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    Fl_Return_Button run(392, 192, 100, 32, T("Run"));
    run.callback(
        [](Fl_Widget *w, void *p) {
          *static_cast<bool *>(p) = true;
          w->window()->hide();
        },
        &accepted);
    dialog.end();
    dialog.set_modal();
    reliefos_fltk::style(&dialog);
    dialog.show();
    while (dialog.shown())
      Fl::wait();
    if (!accepted)
      return;
    custom.set(cmd.value());
    if (!identifier(custom.c_str())) {
      fl_alert("%s", T("Invalid service command."));
      return;
    }
    const char *options[] = {"",          "dry-run",    "ifstarted",
                             "ifstopped", "ifinactive", "ifnotstarted",
                             "ifcrashed", "nodeps",     "debug"};
    extra.set(options[mode.value()]);
    verb = custom.c_str();
    if (!confirm(
            T("Run this advanced command for the selected services? zap only "
              "resets OpenRC state; it does not stop a running process."),
            names))
      return;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    Request r =
        request(action == Enable    ? Op::Add
                : action == Disable ? Op::Remove
                                    : Op::Service,
                names[i].c_str(),
                action == Enable || action == Disable ? level.c_str() : verb,
                extra.c_str());
    Text title;
    title.set(action == Enable    ? "rc-update add "
              : action == Disable ? "rc-update del "
                                  : "rc-service ");
    append(title, names[i].c_str());
    append(title, " ");
    append(title, r.b.c_str());
    unsigned purpose = action == Description     ? View
                       : action == ServiceStatus ? Diagnostic
                                                 : Mutation;
    if (!enqueue(r, purpose, title.c_str())) {
      queue_.clear();
      return;
    }
  }
  next();
}
void App::runlevels(Action action) {
  if (!idle())
    return;
  const char *value =
      fl_input(T("Runlevel name:"), level_.empty() ? "" : level_.c_str());
  if (!value)
    return;
  Text target;
  target.set(value);
  if (!identifier(target.c_str())) {
    fl_alert("%s", T("Invalid runlevel name."));
    return;
  }
  Op op = action == CreateLevel   ? Op::CreateLevel
          : action == DeleteLevel ? Op::DeleteLevel
          : action == StackAdd    ? Op::StackAdd
          : action == StackDel    ? Op::StackDel
                                  : Op::SwitchLevel;
  Text other;
  if (action == StackAdd || action == StackDel) {
    value = fl_input(T("Parent runlevel (the selected runlevel is stacked into "
                       "this level):"),
                     "default");
    if (!value)
      return;
    other.set(value);
  } else if (action == SwitchLevel) {
    int choice =
        fl_choice("%s", T("Cancel"), T("Start only"), T("Switch fully"),
                  T("Changing runlevel may stop services and disconnect the "
                    "desktop. Start only keeps existing services running."));
    if (choice == 0)
      return;
    if (choice == 1)
      other.set("no-stop");
  } else if (action == DeleteLevel) {
    Array<Text> names;
    names.push(target);
    if (!confirm(
            T("Delete this empty, inactive, unreferenced custom runlevel?"),
            names))
      return;
  }
  Request r = request(op, target.c_str(), other.c_str());
  if (enqueue(r, Mutation, T("Updating runlevel…")))
    next();
}
void App::edit(const char *target) {
  if (!idle())
    return;
  if (config_dirty_ &&
      fl_choice("%s", T("Cancel"), T("Discard changes"), nullptr,
                T("Configuration has unsaved changes.")) != 1)
    return;
  if (enqueue(request(Op::ReadConfig, target), Edit,
              T("Reading configuration…")))
    next();
}
void App::export_output() {
  const char *path =
      fl_file_chooser(T("Export output"), "*.txt", "openrc-report.txt");
  if (!path)
    return;
  FILE *file = fopen(path, "wx");
  if (!file && errno == EEXIST) {
    if (fl_choice("%s", T("Cancel"), T("Replace"), nullptr,
                  T("This file already exists. Replace it?")) != 1)
      return;
    file = fopen(path, "w");
  }
  if (!file) {
    fl_alert("%s", T("Cannot create the output file."));
    return;
  }
  char *text = output_buffer_.text();
  size_t length = strlen(text), at = 0;
  while (at < length) {
    size_t n = fwrite(text + at, 1, length - at, file);
    if (!n)
      break;
    at += n;
  }
  bool ok = at == length;
  free(text);
  if (fclose(file) != 0)
    ok = false;
  if (!ok)
    fl_alert("%s", T("Could not completely write the output file."));
  else
    footer_->copy_label(T("Output exported"));
}
void App::action(Action a) {
  if (a == Exit) {
    if (config_dirty_ &&
        fl_choice("%s", T("Cancel"), T("Discard changes"), nullptr,
                  T("Configuration has unsaved changes.")) != 1)
      return;
    if (busy_ && fl_choice("%s", T("Cancel"), T("Stop and close"), nullptr,
                           T("An operation is running. Closing cancels it; "
                             "check service state before retrying.")) != 1)
      return;
    session_.stop();
    window_->hide();
    closing_ = true;
    return;
  }
  if (a == Refresh) {
    if (idle())
      refresh();
    return;
  }
  if (a == Cancel) {
    queue_.clear();
    if (busy_) {
      Request r;
      r.op = Op::Cancel;
      session_.send(r, 0);
      footer_->copy_label(T("Canceling operation…"));
    }
    return;
  }
  if (a == Export) {
    export_output();
    return;
  }
  if (a == ClearOutput) {
    output_buffer_.text("");
    return;
  }
  if (a == Help) {
    fl_message(
        "%s",
        T("Select services and use the toolbar or Service menu. Ctrl/Shift "
          "selects multiple services; column headers sort.\n\nEnabling changes "
          "runlevel membership; starting changes the current running state. "
          "Use the Configuration tab to edit conf.d or global rc.conf; saving "
          "makes a backup and detects external changes.\n\nAdvanced actions "
          "support conditions, dry run, zap and custom service commands. "
          "Canceling may leave OpenRC in a transitional state; inspect status "
          "before using zap.\n\nThe application never invokes shutdown, "
          "reboot, boot or sysinit runlevels. Logs are available when the "
          "corresponding system logger is configured."));
    return;
  }
  if (a == About) {
    fl_message("%s", T("OpenRC Controller\nC++ / FLTK service and runlevel "
                       "manager for ReliefOS.\nUses the system OpenRC commands "
                       "and native FLTK gtk+ controls."));
    return;
  }
  if (a >= Start && a <= Advanced) {
    run_services(a);
    return;
  }
  if (a == ServiceStatus || a == Description) {
    run_services(a);
    return;
  }
  if (a >= CreateLevel && a <= SwitchLevel) {
    runlevels(a);
    return;
  }
  if (a == ServiceConfig || a == Script || a == Dependencies) {
    if (!idle())
      return;
    Array<Text> names;
    selection(names);
    if (names.size() != 1) {
      fl_alert("%s", T("Select exactly one service."));
      return;
    }
    if (a == ServiceConfig) {
      edit(names[0].c_str());
      return;
    }
    if (a == Script) {
      enqueue(request(Op::ReadScript, names[0].c_str()), View,
              T("Reading service script…"));
      next();
      return;
    }
    const char *type =
        fl_input(T("Dependency type: ineed, iuse, iwant, iafter, ibefore; "
                   "reverse: needsme, usesme, wantsme, beforeme, afterme"),
                 "ineed");
    if (type && enqueue(request(Op::Depend, names[0].c_str(), type), Dependency,
                        T("Querying dependency tree…")))
      next();
    return;
  }
  if (a == GlobalConfig) {
    edit("@global");
    return;
  }
  if (a == ReloadConfig) {
    if (!config_target_.empty())
      edit(config_target_.c_str());
    return;
  }
  if (a == SaveConfig || a == RestoreConfig) {
    if (!idle())
      return;
    if (config_target_.empty()) {
      fl_alert("%s", T("Load a configuration first."));
      return;
    }
    if (a == RestoreConfig &&
        fl_choice("%s", T("Cancel"), T("Restore"), nullptr,
                  T("Replace the current configuration with its last backup? "
                    "Unsaved editor changes will be discarded.")) != 1)
      return;
    Request r = request(a == SaveConfig ? Op::WriteConfig : Op::RestoreConfig,
                        config_target_.c_str());
    r.b = config_original_.bytes;
    r.d.set(config_original_.exists ? "present" : "missing");
    if (a == SaveConfig) {
      char *s = config_buffer_.text();
      r.c.set(s);
      free(s);
    }
    if (enqueue(r, Save, T("Saving configuration…")))
      next();
    return;
  }
  if (!idle())
    return;
  Request r;
  const char *title = T("OpenRC diagnostics");
  if (a >= AllStatus && a <= Unused) {
    const char *modes[] = {"all", "crashed", "manual", "supervised", "unused"};
    r = request(Op::State, modes[a - AllStatus]);
  } else if (a == Rebuild)
    r = request(Op::Rebuild);
  else if (a >= RcLog && a <= BootLog) {
    const char *logs[] = {"rc.log", "messages", "syslog", "boot.log"};
    r = request(Op::Logs, logs[a - RcLog]);
  } else if (a == Report)
    r = request(Op::Report);
  else
    return;
  if (enqueue(r, Diagnostic, title))
    next();
}
} // namespace oc
