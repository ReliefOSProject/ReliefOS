#include "fltk_support.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char paste_text[] = "中文输入框粘贴测试";
static Fl_Input *text_input;
static Fl_Secret_Input *secret_input;
static Fl_Hold_Browser *browser;
static int input_pass_reported;
static int secret_pass_reported;
static int browser_pass_reported;

static int window_info(unsigned long pid, bool watch = false)
{
    fl_open_display();
    Window root = DefaultRootWindow(fl_display);
    Atom clients = XInternAtom(fl_display, "_NET_CLIENT_LIST", False);
    Atom process_id = XInternAtom(fl_display, "_NET_WM_PID", False);
    for (int retry = 0; retry < 100; ++retry) {
        Atom actual_type;
        int format;
        unsigned long count, remaining;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(fl_display, root, clients, 0, 4096, False,
                               XA_WINDOW, &actual_type, &format, &count,
                               &remaining, &data) == Success && data) {
            Window *windows = reinterpret_cast<Window *>(data);
            for (unsigned long index = 0; format == 32 && index < count; ++index) {
                unsigned char *owner = nullptr;
                unsigned long owner_count;
                int owner_format;
                if (XGetWindowProperty(fl_display, windows[index], process_id,
                                       0, 1, False, XA_CARDINAL, &actual_type,
                                       &owner_format, &owner_count, &remaining,
                                       &owner) != Success || !owner) continue;
                bool matches = owner_format == 32 && owner_count == 1 &&
                               *reinterpret_cast<unsigned long *>(owner) == pid;
                XFree(owner);
                if (!matches) continue;
                Window child, geometry_root;
                int x, y, ignored_x, ignored_y;
                unsigned width, height, border, depth;
                if (XGetGeometry(fl_display, windows[index], &geometry_root,
                                  &ignored_x, &ignored_y, &width, &height,
                                  &border, &depth) &&
                    XTranslateCoordinates(fl_display, windows[index], root,
                                           0, 0, &x, &y, &child)) {
                    printf("FLTK_APP_GEOMETRY pid=%lu x=%d y=%d w=%u h=%u\n",
                           pid, x, y, width, height);
                    Window target = windows[index];
                    XFree(data);
                    fflush(stdout);
                    if (!watch) return 0;
                    XSelectInput(fl_display, target, StructureNotifyMask);
                    XFlush(fl_display);
                    for (;;) {
                        XEvent event;
                        XNextEvent(fl_display, &event);
                        if (event.type == DestroyNotify) return 0;
                        if (event.type != ConfigureNotify) continue;
                        if (XTranslateCoordinates(fl_display, target, root,
                                                   0, 0, &x, &y, &child)) {
                            printf("FLTK_APP_GEOMETRY pid=%lu x=%d y=%d w=%d h=%d\n",
                                   pid, x, y, event.xconfigure.width, event.xconfigure.height);
                            fflush(stdout);
                        }
                    }
                }
            }
            XFree(data);
        }
        usleep(100000);
    }
    fprintf(stderr, "FLTK_APP_GEOMETRY_FAIL pid=%lu\n", pid);
    return 1;
}

static void input_changed(Fl_Widget *widget, void *)
{
    Fl_Input *input = static_cast<Fl_Input *>(widget);
    if (strcmp(input->value(), paste_text) == 0 && !input_pass_reported) {
        input_pass_reported = 1;
        puts("FLTK_PROBE_PASS input_utf8_paste");
        fflush(stdout);
    }
}

static void secret_changed(Fl_Widget *widget, void *)
{
    Fl_Secret_Input *input = static_cast<Fl_Secret_Input *>(widget);
    if (strcmp(input->value(), paste_text) == 0 && !secret_pass_reported) {
        secret_pass_reported = 1;
        puts("FLTK_PROBE_PASS secret_utf8_paste");
        fflush(stdout);
    }
}

static void browser_changed(Fl_Widget *, void *)
{
    int selected = browser->value();
    const char *label = selected > 0 ? browser->text(selected) : nullptr;
    if (label && strncmp(label, "桌面验收目录", strlen("桌面验收目录")) == 0 &&
        !browser_pass_reported) {
        browser_pass_reported = 1;
        puts("FLTK_PROBE_PASS browser_utf8_selection");
        fflush(stdout);
    }
}

static bool copy_input_checks()
{
    char small[8] = "stable";
    if (reliefos_fltk::copy_input(small, sizeof(small), paste_text)) return false;
    if (strcmp(small, "stable") != 0) return false;

    char complete[sizeof(paste_text)];
    if (!reliefos_fltk::copy_input(complete, sizeof(complete), paste_text)) return false;
    return strcmp(complete, paste_text) == 0;
}

static void ready(void *data)
{
    Fl_Window *window = static_cast<Fl_Window *>(data);
    Fl::flush();
    printf("FLTK_PROBE_GEOMETRY x=%d y=%d w=%d h=%d\n",
           window->x(), window->y(), window->w(), window->h());
    puts("FLTK_PROBE_READY");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--window-info") == 0)
        return window_info(strtoul(argv[2], nullptr, 10));
    if (argc == 3 && strcmp(argv[1], "--watch-window") == 0)
        return window_info(strtoul(argv[2], nullptr, 10), true);
    reliefos_fltk::init();
    if (!copy_input_checks()) {
        puts("FLTK_PROBE_FAIL copy_input_capacity");
        return 1;
    }
    puts("FLTK_PROBE_PASS copy_input_accept");
    puts("FLTK_PROBE_PASS copy_input_reject_unchanged");

    Fl_Double_Window window(850, 520, "FLTK 中文输入验收");
    Fl_Box *heading = new Fl_Box(24, 16, 802, 38, "中文输入框与剪贴板验收");
    heading->labelfont(FL_HELVETICA_BOLD);
    heading->labelsize(24);

    Fl_Box *input_label = new Fl_Box(28, 82, 214, 34, "普通输入框：Ctrl+V");
    input_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    text_input = new Fl_Input(250, 82, 570, 34);
    text_input->when(FL_WHEN_CHANGED);
    text_input->callback(input_changed);

    Fl_Box *secret_label = new Fl_Box(28, 132, 214, 34, "密码输入框：Ctrl+V");
    secret_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    secret_input = new Fl_Secret_Input(250, 132, 570, 34);
    secret_input->when(FL_WHEN_CHANGED);
    secret_input->callback(secret_changed);

    Fl_Box *browser_label = new Fl_Box(28, 194, 792, 30, "中文浏览列表：点击第一行进行选择");
    browser_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    browser = new Fl_Hold_Browser(28, 228, 792, 250);
    browser->format_char(0);
    browser->add("桌面验收目录    文件夹");
    browser->add("中文样本.txt    UTF-8 文本");
    browser->add("图片标志.png    PNG 图像");
    browser->add("扫雷矿石.bmp    BMP 图像");
    browser->when(FL_WHEN_CHANGED);
    browser->callback(browser_changed);

    window.end();
    window.position((Fl::w() - window.w()) / 2, (Fl::h() - window.h()) / 2);
    reliefos_fltk::style(&window);
    window.show();
    Fl::copy(paste_text, static_cast<int>(sizeof(paste_text) - 1), 1);
    text_input->take_focus();
    Fl::add_timeout(.2, ready, &window);
    return Fl::run();
}
