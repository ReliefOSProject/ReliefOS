#include "analysis.h"
#include "window.h"
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Text_Display.H>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
namespace rmp {
namespace {
Fl_Box *caption(int x, int y, int w, const char *s) {
  auto *b = new Fl_Box(x, y, w, 22, s);
  b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  b->labelsize(14);
  return b;
}
bool number(const char *s, double &out) {
  char *end;
  out = strtod(s, &end);
  while (*end == ' ')
    ++end;
  return end != s && !*end && isfinite(out) && fabs(out) <= 1e12;
}
bool coordinates(const char *s, Vec3 &out) {
  double *v[] = {&out.x, &out.y, &out.z};
  for (int i = 0; i < 3; ++i) {
    char *end;
    *v[i] = strtod(s, &end);
    if (end == s || !isfinite(*v[i]) || fabs(*v[i]) > 1e8)
      return false;
    s = end;
    while (*s == ' ')
      ++s;
    if (i < 2) {
      if (*s++ != ',')
        return false;
    }
  }
  return !*s;
}
void close_modal(Fl_Widget *w, void *) { w->window()->hide(); }
void run_modal(Fl_Window &dialog) {
  reliefos_fltk::style(&dialog);
  dialog.set_modal();
  dialog.show();
  while (dialog.shown())
    Fl::wait(.03);
}
struct CalcRequest {
  Expression expression;
  Variables vars;
  double x = 0, y = 0, a = 0, b = 1, c = 0, d = 1;
  int mode = 0;
};
struct CalcReply {
  bool ok = false;
  double value = 0, error = 0, y = 0, gx = 0, gy = 0;
  int mode = 0;
};
CalcReply calculate(const CalcRequest &q) {
  CalcReply reply;
  Result result;
  reply.mode = q.mode;
  Variables v = q.vars;
  v.x = q.x;
  const Expression &e = q.expression;
  switch (q.mode) {
  case 0:
    result.value = e.eval(v);
    result.ok = isfinite(result.value);
    break;
  case 1:
  case 2:
    result = derivative(e, v, q.x, q.mode);
    break;
  case 3:
    result = integrate(e, v, q.a, q.b);
    break;
  case 4:
  case 5:
    result = root(e, v, q.a, q.b);
    break;
  case 6:
  case 7:
    result = extremum(e, v, q.a, q.b, q.mode == 7);
    break;
  case 8:
    result = integrate(e, v, q.a, q.b, true);
    break;
  }
  if (q.mode >= 10) {
    v.x = q.x;
    v.y = q.y;
    if (q.mode == 10) {
      result.value = e.eval(v);
      result.ok = isfinite(result.value);
    } else if (q.mode == 11) {
      Result dx = partial(e, v, q.x, q.y, 0), dy = partial(e, v, q.x, q.y, 1);
      result.ok = dx.ok && dy.ok;
      result.value = e.eval(v);
      result.error = fmax(dx.error, dy.error);
      reply.gx = dx.value;
      reply.gy = dy.value;
    } else if (q.mode == 12) {
      Result dx = partial(e, v, q.x, q.y, 0, 2),
             dy = partial(e, v, q.x, q.y, 1, 2);
      result.ok = dx.ok && dy.ok;
      result.value = dx.value + dy.value;
      result.error = dx.error + dy.error;
    } else
      result = integrate2d(e, v, q.a, q.b, q.c, q.d, q.mode == 14);
  }
  if (result.ok) {
    v.x = result.value;
    reply.y = e.eval(v);
  }
  reply.ok = result.ok;
  reply.value = result.value;
  reply.error = result.error;
  return reply;
}
// Only a numeric POD crosses this private pipe; the worker never touches Xlib.
class CalcJob {
  pid_t pid_ = -1;
  int fd_ = -1;
  size_t received_ = 0;
  char data_[sizeof(CalcReply)]{};
  double started_ = 0;

public:
  ~CalcJob() { cancel(); }
  bool start(const CalcRequest &q) {
    cancel();
    int fds[2];
    if (pipe(fds) < 0)
      return false;
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
      close(fds[0]);
      close(fds[1]);
      return false;
    }
    pid_ = fork();
    if (pid_ == 0) {
      close(fds[0]);
      CalcReply reply = calculate(q);
      const char *p = reinterpret_cast<const char *>(&reply);
      size_t at = 0;
      while (at < sizeof(reply)) {
        ssize_t n = write(fds[1], p + at, sizeof(reply) - at);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          break;
        at += size_t(n);
      }
      close(fds[1]);
      _exit(at == sizeof(reply) ? 0 : 1);
    }
    close(fds[1]);
    if (pid_ < 0) {
      close(fds[0]);
      return false;
    }
    fd_ = fds[0];
    received_ = 0;
    started_ = clock_now();
    return true;
  }
  static double clock_now() {
    struct timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
  }
  bool running() const { return pid_ > 0; }
  void cancel() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }
  }
  bool poll(CalcReply &reply) {
    if (pid_ <= 0)
      return false;
    if (clock_now() - started_ > 20) {
      cancel();
      reply = CalcReply{};
      return true;
    }
    for (;;) {
      ssize_t n = read(fd_, data_ + received_, sizeof(data_) - received_);
      if (n > 0) {
        received_ += size_t(n);
        if (received_ == sizeof(data_))
          break;
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return false;
      break;
    }
    int status;
    pid_t done = waitpid(pid_, &status, WNOHANG);
    if (done == 0)
      return false;
    if (done == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        received_ == sizeof(data_))
      memcpy(&reply, data_, sizeof(reply));
    else
      reply = CalcReply{};
    close(fd_);
    fd_ = -1;
    pid_ = -1;
    return true;
  }
};
} // namespace
void MathWindow::calculate_dialog() {
  Fl_Double_Window dialog(740, 522, "函数与数值计算");
  caption(18, 12, 700,
          "数值近似计算 · 使用当前 a、b、c、d 参数 · 区间内应满足连续性要求");
  caption(18, 47, 90, "计算工具");
  Fl_Choice mode(116, 45, 600, 29);
  mode.add("函数值 f(x)|一阶导数 f'(x)|二阶导数 "
           "f''(x)|定积分|区间零点|两个函数交点|区间最小值（采样搜索）|"
           "区间最大值（采样搜索）|函数弧长");
  mode.value(0);
  caption(18, 87, 90, "f(x)");
  Fl_Input source(116, 85, 600, 29);
  source.maximum_size(1024);
  Object *o = current();
  source.value(o && o->kind == Kind::Function2 ? o->formula[0].c_str()
                                               : "sin(x)");
  caption(18, 127, 90, "g(x)");
  Fl_Input second(116, 125, 600, 29);
  second.value("cos(x)");
  caption(18, 165, 95, "点 x");
  Fl_Input point(116, 163, 136, 29);
  point.value("1");
  caption(274, 165, 40, "下限");
  Fl_Input lo(320, 163, 155, 29);
  lo.value("0");
  caption(493, 165, 40, "上限");
  Fl_Input hi(540, 163, 176, 29);
  hi.value("3.141592653589793");
  Fl_Button compute(116, 207, 144, 31, "开始计算");
  Fl_Button cancel(274, 207, 120, 31, "取消计算");
  Fl_Button tangent(408, 207, 150, 31, "添加此点切线");
  Fl_Multiline_Output output(18, 254, 698, 196);
  output.textsize(15);
  output.wrap(1);
  output.value("选择工具并输入表达式。求根需要端点异号；交点计算 "
               "f(x)-g(x)="
               "0。\n积分与求导可能在间断点、振荡过快或浮点误差过大时失败。\n极"
               "值为数值采样搜索结果，不能证明已找到所有局部极值。");
  Fl_Button close(596, 472, 120, 31, "关闭");
  close.callback(close_modal);
  dialog.end();
  CalcJob job;
  struct Context {
    MathWindow *owner;
    Fl_Choice *mode;
    Fl_Input *source, *second, *point, *lo, *hi;
    Fl_Multiline_Output *output;
    CalcJob *job;
  } c{this, &mode, &source, &second, &point, &lo, &hi, &output, &job};
  compute.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        CalcRequest q;
        Text err;
        char combined[2200];
        const char *formula = c->source->value();
        q.mode = c->mode->value();
        if (q.mode == 5) {
          snprintf(combined, sizeof(combined), "(%s)-(%s)", formula,
                   c->second->value());
          formula = combined;
        }
        if (!q.expression.compile(formula, err, 961) ||
            !number(c->point->value(), q.x) || !number(c->lo->value(), q.a) ||
            !number(c->hi->value(), q.b)) {
          c->output->value(err.empty() ? "请输入有限数值，坐标绝对值不超过 1e12"
                                       : err.c_str());
          return;
        }
        q.vars = c->owner->document.vars;
        if (!c->job->start(q)) {
          c->output->value("不能启动数值计算进程");
          return;
        }
        c->output->value("计算中… 可继续操作窗口或取消计算。");
      },
      &c);
  cancel.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        c->job->cancel();
        c->output->value("计算已取消");
      },
      &c);
  tangent.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        Expression e;
        Text err;
        double x;
        if (!number(c->point->value(), x) ||
            !e.compile(c->source->value(), err, 961)) {
          c->output->value("切线表达式或坐标无效");
          return;
        }
        Variables v = c->owner->document.vars;
        v.x = x;
        double y = e.eval(v);
        Result slope = derivative(e, v, x);
        if (!slope.ok || !isfinite(y)) {
          c->output->value("此点不能取得有限的导数");
          return;
        }
        if (!c->owner->checkpoint())
          return;
        Object line = make_object(Kind::Function2);
        char b[256];
        snprintf(b, sizeof(b), "(%.17g)*(x-(%.17g))+(%.17g)", slope.value, x,
                 y);
        line.formula[0].set(b);
        line.name.set("数值切线");
        line.color = 1;
        if (append(c->owner->document, line, c->owner->active_)) {
          c->owner->changed();
          c->owner->refresh();
          c->output->value("切线已加入文档；切线保存计算时的参数值。");
        }
      },
      &c);
  reliefos_fltk::style(&dialog);
  dialog.set_modal();
  dialog.show();
  while (dialog.shown()) {
    Fl::wait(.03);
    CalcReply r;
    if (job.poll(r)) {
      char b[700];
      if (r.ok) {
        int m = r.mode;
        if (m == 4 || m == 5 || m == 6 || m == 7)
          snprintf(b, sizeof(b),
                   "x ≈ %.14g\nf(x) / 差值 ≈ %.14g\n残差或区间宽度 ≈ "
                   "%.4g\n结果为数值近似。",
                   r.value, r.y, r.error);
        else
          snprintf(b, sizeof(b),
                   "结果 ≈ %.14g\n估计差异 / 误差 ≈ %.4g\n结果为数值近似。",
                   r.value, r.error);
        printf("[reliefmathpp.elf] calculation mode=%d value=%.14g\n", m,
               r.value);
        fflush(stdout);
      } else
        snprintf(b, sizeof(b),
                 "未得到可靠结果。\n请检查定义域、连续性、区间和端点符号；可缩"
                 "小区间重试。\n区间宽度很小并不能证明间断点是零点。");
      output.value(b);
    }
  }
}
void MathWindow::surface_dialog() {
  Fl_Double_Window dialog(760, 560, "三维函数计算");
  caption(18, 12, 720,
          "对显函数 z=f(x,y) 计算；使用当前 a/b/c/d 参数；结果为数值近似");
  caption(18, 48, 90, "工具");
  Fl_Choice mode(114, 46, 620, 29);
  mode.add("函数值 z(x,y)|梯度与法向量|拉普拉斯算子|矩形区域二重积分|曲面面积");
  mode.value(0);
  caption(18, 88, 90, "z(x,y)");
  Fl_Input formula(114, 86, 620, 29);
  Object *o = current();
  formula.value(o && o->kind == Kind::Function3 ? o->formula[0].c_str()
                                                : "x^2+y^2");
  formula.maximum_size(1024);
  caption(18, 128, 32, "x");
  Fl_Input x(64, 126, 290, 29);
  x.value("1");
  caption(382, 128, 32, "y");
  Fl_Input y(428, 126, 306, 29);
  y.value("2");
  caption(18, 171, 90, "X 区间");
  Fl_Input a(114, 169, 112, 29);
  a.value("0");
  Fl_Input b(242, 169, 112, 29);
  b.value("1");
  caption(382, 171, 90, "Y 区间");
  Fl_Input c(478, 169, 112, 29);
  c.value("0");
  Fl_Input d(606, 169, 128, 29);
  d.value("1");
  Fl_Button compute(114, 214, 138, 31, "开始计算");
  Fl_Button cancel(268, 214, 118, 31, "取消计算");
  Fl_Button tangent(402, 214, 174, 31, "添加此点切平面");
  Fl_Multiline_Output output(18, 263, 716, 223);
  output.textsize(15);
  output.wrap(1);
  output.value(
      "梯度为 (∂z/∂x, ∂z/∂y)，法向量为 (-∂z/∂x, -∂z/∂y, "
      "1)"
      "。\n二重积分为带符号的体积；曲面面积只计算指定矩形区域。\n积分采用两级 "
      "Simpson 网格估计误差，不适用于奇点或未解析的高频结构。");
  Fl_Button close(614, 510, 120, 31, "关闭");
  close.callback(close_modal);
  dialog.end();
  CalcJob job;
  struct Context {
    MathWindow *owner;
    Fl_Choice *mode;
    Fl_Input *formula, *x, *y, *a, *b, *c, *d;
    Fl_Multiline_Output *output;
    CalcJob *job;
  } ctx{this, &mode, &formula, &x, &y, &a, &b, &c, &d, &output, &job};
  compute.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        CalcRequest q;
        Text error;
        q.mode = 10 + c->mode->value();
        q.vars = c->owner->document.vars;
        if (!q.expression.compile(c->formula->value(), error, 963) ||
            !number(c->x->value(), q.x) || !number(c->y->value(), q.y) ||
            !number(c->a->value(), q.a) || !number(c->b->value(), q.b) ||
            !number(c->c->value(), q.c) || !number(c->d->value(), q.d)) {
          c->output->value(error.empty() ? "坐标或区间无效" : error.c_str());
          return;
        }
        if (c->job->start(q))
          c->output->value("计算中… 可取消。");
        else
          c->output->value("不能启动计算进程");
      },
      &ctx);
  cancel.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        c->job->cancel();
        c->output->value("计算已取消");
      },
      &ctx);
  tangent.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        double x, y;
        Expression e;
        Text err;
        if (!number(c->x->value(), x) || !number(c->y->value(), y) ||
            !e.compile(c->formula->value(), err, 963)) {
          c->output->value("表达式或坐标无效");
          return;
        }
        Variables v = c->owner->document.vars;
        v.x = x;
        v.y = y;
        double z = e.eval(v);
        Result dx = partial(e, v, x, y, 0), dy = partial(e, v, x, y, 1);
        if (!dx.ok || !dy.ok || !isfinite(z)) {
          c->output->value("此点不能取得有限梯度");
          return;
        }
        Object o = make_object(Kind::Plane3);
        o.name.set("数值切平面");
        o.p[0] = -dx.value;
        o.p[1] = -dy.value;
        o.p[2] = 1;
        o.p[3] = z - dx.value * x - dy.value * y;
        o.color = 1;
        if (!valid_object(o, err)) {
          c->output->value(err.c_str());
          return;
        }
        if (!c->owner->checkpoint())
          return;
        if (append(c->owner->document, o, c->owner->active_)) {
          c->owner->changed();
          c->owner->refresh();
          c->output->value("切平面已添加；保存计算时的参数值。");
        } else
          c->output->value("不能添加切平面");
      },
      &ctx);
  reliefos_fltk::style(&dialog);
  dialog.set_modal();
  dialog.show();
  while (dialog.shown()) {
    Fl::wait(.03);
    CalcReply r;
    if (job.poll(r)) {
      char result[700];
      if (r.ok) {
        if (r.mode == 11)
          snprintf(result, sizeof(result),
                   "z ≈ %.14g\n梯度 ≈ (%.12g, %.12g)\n法向量 ≈ (%.12g, %.12g, "
                   "1)\n估计差异 ≈ %.5g",
                   r.value, r.gx, r.gy, -r.gx, -r.gy, r.error);
        else
          snprintf(result, sizeof(result),
                   "结果 ≈ %.14g\n估计差异 / 误差 ≈ %.5g", r.value, r.error);
        printf("[reliefmathpp.elf] calculation mode=%d value=%.14g\n", r.mode,
               r.value);
        fflush(stdout);
      } else
        snprintf(result, sizeof(result),
                 "未取得可靠结果，请检查定义域、连续性和区间。\n积分区间下限必"
                 "须小于上限，绝对值不超过 1e6。");
      output.value(result);
    }
  }
}
void MathWindow::table_dialog() {
  Fl_Double_Window dialog(720, 550, "函数值表与 CSV");
  caption(18, 12, 680, "采样二维显函数，最多 2000 行；未定义值显示 undefined");
  caption(18, 48, 80, "f(x)");
  Fl_Input formula(108, 46, 588, 29);
  Object *o = current();
  formula.value(o && o->kind == Kind::Function2 ? o->formula[0].c_str()
                                                : "sin(x)");
  caption(18, 88, 60, "下限");
  Fl_Input lo(78, 86, 140, 28);
  lo.value("-5");
  caption(234, 88, 60, "上限");
  Fl_Input hi(294, 86, 140, 28);
  hi.value("5");
  caption(450, 88, 60, "行数");
  Fl_Input count(510, 86, 186, 28);
  count.value("101");
  Fl_Text_Buffer buffer;
  Fl_Text_Display display(18, 130, 678, 343);
  display.buffer(&buffer);
  display.textfont(FL_COURIER);
  display.textsize(14);
  Fl_Button generate(18, 495, 128, 31, "生成值表");
  Fl_Button export_csv(160, 495, 128, 31, "导出 CSV");
  Fl_Button close(576, 495, 120, 31, "关闭");
  close.callback(close_modal);
  dialog.end();
  Bytes csv;
  struct Context {
    MathWindow *owner;
    Fl_Input *formula, *lo, *hi, *count;
    Fl_Text_Buffer *buffer;
    Bytes *csv;
  } c{this, &formula, &lo, &hi, &count, &buffer, &csv};
  generate.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        c->csv->clear();
        double a, b, count;
        Expression e;
        Text err;
        if (!number(c->lo->value(), a) || !number(c->hi->value(), b) ||
            a >= b || !number(c->count->value(), count) || count < 2 ||
            count > 2000 || floor(count) != count ||
            !e.compile(c->formula->value(), err, 961)) {
          c->buffer->text(err.empty() ? "区间或行数无效；需要 2–2000 行"
                                      : err.c_str());
          return;
        }
        Bytes out;
        const char *header = "x,f(x)\r\n";
        out.resize(strlen(header));
        memcpy(out.data(), header, out.size());
        Variables v = c->owner->document.vars;
        for (int n = 0; n < int(count); ++n) {
          v.x = a + (b - a) * n / (count - 1);
          double y = e.eval(v);
          char line[128];
          int bytes =
              isfinite(y)
                  ? snprintf(line, sizeof(line), "%.17g,%.17g\r\n", v.x, y)
                  : snprintf(line, sizeof(line), "%.17g,undefined\r\n", v.x);
          size_t pos = out.size();
          if (bytes <= 0 || !out.resize(pos + size_t(bytes))) {
            c->buffer->text("内存不足");
            return;
          }
          memcpy(out.data() + pos, line, size_t(bytes));
        }
        c->csv->swap(out);
        Text text;
        if (text.set(c->csv->data(), c->csv->size()))
          c->buffer->text(text.c_str());
      },
      &c);
  export_csv.callback(
      [](Fl_Widget *w, void *p) {
        auto *c = static_cast<Context *>(p);
        if (c->csv->empty()) {
          fl_message("请先生成值表");
          return;
        }
        const char *path =
            fl_file_chooser("导出函数值表", "*.csv", "values.csv", 0);
        if (!path)
          return;
        Bytes copy;
        if (!copy.clone_from(*c->csv)) {
          fl_alert("内存不足");
          return;
        }
        c->owner->pending_bytes_.swap(copy);
        c->owner->start_write(path, true);
        w->window()->hide();
      },
      &c);
  run_modal(dialog);
  display.buffer(nullptr);
}
void MathWindow::vectors_dialog() {
  Fl_Double_Window dialog(660, 420, "向量与两点计算");
  caption(18, 15, 620, "输入三维分量；二维计算将 Z 设为 0");
  caption(18, 55, 90, "A / 向量 u");
  Fl_Input a(120, 53, 516, 30);
  a.value("1, 0, 0");
  caption(18, 99, 90, "B / 向量 v");
  Fl_Input b(120, 97, 516, 30);
  b.value("0, 1, 0");
  Fl_Multiline_Output result(18, 147, 618, 207);
  result.textsize(15);
  Fl_Button compute(18, 374, 120, 30, "计算");
  Fl_Button close(516, 374, 120, 30, "关闭");
  close.callback(close_modal);
  dialog.end();
  struct Context {
    Fl_Input *a, *b;
    Fl_Multiline_Output *out;
  } c{&a, &b, &result};
  compute.callback(
      [](Fl_Widget *, void *p) {
        auto *c = static_cast<Context *>(p);
        Vec3 a, b;
        if (!coordinates(c->a->value(), a) || !coordinates(c->b->value(), b)) {
          c->out->value("请用英文逗号输入三个有限坐标");
          return;
        }
        Vec3 mid = scale(rmp::add(a, b), .5), cross = cross_product(a, b);
        double n = norm(a) * norm(b),
               angle = n ? acos(fmax(-1, fmin(1, dot(a, b) / n))) : NAN;
        char text[1000];
        snprintf(text, sizeof(text),
                 "两点距离 ≈ %.12g\n中点 = (%.10g, %.10g, %.10g)\n点积 u·v = "
                 "%.12g\n叉积 u×v = (%.10g, %.10g, %.10g)\n|u| ≈ %.10g，|v| ≈ "
                 "%.10g\n夹角 ≈ %.10g 弧度 / %.10g "
                 "度（零向量无定义）\n平行四边形面积 ≈ %.12g",
                 norm(subtract(a, b)), mid.x, mid.y, mid.z, dot(a, b), cross.x,
                 cross.y, cross.z, norm(a), norm(b), angle,
                 angle * 180 / acos(-1), norm(cross));
        c->out->value(text);
      },
      &c);
  run_modal(dialog);
}
void MathWindow::help_dialog() {
  Fl_Double_Window dialog(790, 602, "ReliefMath++ 使用说明");
  Fl_Text_Buffer buffer;
  Fl_Text_Display display(16, 16, 758, 525);
  display.buffer(&buffer);
  display.textsize(15);
  display.wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
  buffer.text(
      "ReliefMath++ · 二维 / "
      "三维几何、函数与数值计算\n\n选择工具创建对象，在左侧编辑名称、公式、坐标"
      "和范围，按“应用属性”或 "
      "Ctrl+Enter。选中对象可复制、隐藏、删除、撤销和重做。\n\n二维工具：y=f(x)"
      "、参数曲线 x(t)/y(t)、极坐标 r(t)、隐式 "
      "F(x,y)="
      "0，以及点、线段、向量、圆、椭圆、矩形、三角形。隐式公式请输入左端减右端"
      "，例如 x^2+y^2-9。\n三维工具：z=f(x,y)、参数曲面 "
      "x(u,v)/y(u,v)/z(u,v)、空间曲线 "
      "x(t)/y(t)/"
      "z(t)"
      "，以及点、线段、向量、三角形、平面、球、长方体、圆柱和圆锥。\n\n画布：二"
      "维拖动平移，三维拖动旋转，滚轮缩放，Home "
      "重置。“计算与工具”提供拾点构造，圆依次选择圆心与圆周一点。参数 a/b/c/d "
      "可用滑块或右侧输入框（Enter）修改。视图、参数与对象一起保存。\n\n表达式"
      "语法\n+ - * / % ^，比较 < <= > >= == !=，逻辑 && "
      "||。乘方右结合；-2^2=-4，2^3^2=512。乘法必须写 *，例如 "
      "2*x、a*sin(x)，不支持隐式乘法和 LaTeX "
      "输入。\n常量：pi、tau、e；变量按对象类型使用 x/y、t、u/v，及 "
      "a/b/c/d。所有三角函数使用弧度。\n函数：sin cos tan asin acos atan sinh "
      "cosh tanh exp ln log log10 log2 sqrt abs floor ceil round "
      "sign；min(a,b)、max(a,b)、pow(a,b)、atan2(y,x)、hypot(a,b)、if(条件,"
      "真值,假值)。\n例子：if(x<0,-x,x)、sin(x)*exp(-x^2/"
      "10)"
      "。\n\n数值计算\n支持函数值、一二阶导数、定积分、区间夹根、两个显函数交点"
      "、采样搜索极值、弧长和切线；向量工具计算点积、叉积、夹角、两点距离和中点"
      "。几何度量显示周长/面积/"
      "体积，椭圆周长为近似值。\n这些是浮点近似，不是符号计算或形式证明。求根需"
      "要连续且端点异号，不能保证找到偶重根或所有零点。极值工具不是全局最优证明"
      "。高频振荡、奇点、细小结构可能超出采样精度；可调整范围和采样。三维使用正"
      "交软件投影和面片深度排序，交叠曲面存在绘制限制。\n\n文件与导出\n."
      "rmathpp 保存全部对象与视图，支持版本、CRC "
      "和范围校验；保存使用工作进程、原子替换与冲突检测。支持当前视图 SVG、PNG "
      "和函数值 CSV 导出。每文档最多 64 个对象，公式 1024 字节，文件 16 "
      "MiB。坐标绝对值不超过 1e8，范围不超过 "
      "1e6。多对象场景会降低有效网格精度。\n\n此应用与 ReliefMath "
      "公式排版编辑器是两个独立程序。");
  Fl_Button close(654, 557, 120, 30, "关闭");
  close.callback(close_modal);
  dialog.end();
  run_modal(dialog);
  display.buffer(nullptr);
}
} // namespace rmp
