#include "../fltk_support.h"
#include "../fltk_pixels.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Output.H>
#include <generated/build_info.h>
#include <stdio.h>
#include <sys/utsname.h>
extern "C" {
#include <reliefos/png.h>
#include <reliefos/system.h>
}

class Logo : public Fl_Widget {
    uint32_t *pixels_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    int draw_width_ = 0, draw_height_ = 0;
public:
    Logo(int x, int y) : Fl_Widget(x, y, 180, 180)
    {
        if (reliefos_png_decode_file(RELIEFOS_PATH_LOGO_PNG, &pixels_, &width_, &height_) == 0 && width_ && height_) {
            draw_width_ = 180;
            draw_height_ = (uint64_t)height_ * 180 / width_;
            if (draw_height_ > 180) { draw_height_ = 180; draw_width_ = (uint64_t)width_ * 180 / height_; }
        }
    }
    ~Logo() override { reliefos_png_free(pixels_); }
    void draw() override
    {
        fl_color(FL_WHITE); fl_rectf(x(), y(), w(), h());
        if (pixels_) reliefos_fltk::draw_pixels(pixels_, width_, height_, x() + (w()-draw_width_)/2,
            y() + (h()-draw_height_)/2, draw_width_, draw_height_);
        else { fl_font(FL_HELVETICA, 15); fl_color(FL_FOREGROUND_COLOR); fl_draw(T("Logo unavailable"), x(), y(), w(), h(), FL_ALIGN_CENTER); }
    }
};

int main()
{
    reliefos_fltk::init("osver");
    struct reliefos_system_info info{};
    const char *status = T("System version information");
    if (reliefos_system_info(&info) < 0) {
        snprintf(info.kernel_name, sizeof(info.kernel_name), "unknown");
        snprintf(info.kernel_version, sizeof(info.kernel_version), "unknown");
        status = T("Could not read system version information");
    }
    if (!info.build_time[0]) {
        utsname uts{};
        if (uname(&uts) == 0) snprintf(info.build_time, sizeof(info.build_time), "%.*s", (int)sizeof(info.build_time)-1, uts.version);
        if (!info.build_time[0]) snprintf(info.build_time, sizeof(info.build_time), "%s", T("Unavailable"));
    }
    if (!info.copyright[0]) snprintf(info.copyright, sizeof(info.copyright), "%s", RELIEFOS_COPYRIGHT);
    info.kernel_name[sizeof(info.kernel_name)-1] = info.kernel_version[sizeof(info.kernel_version)-1] = 0;
    info.build_time[sizeof(info.build_time)-1] = info.copyright[sizeof(info.copyright)-1] = 0;
    Fl_Double_Window window(790, 390, T("About ReliefOS"));
    new Fl_Box(20, 12, 750, 32, "ReliefOS");
    new Fl_Box(20, 46, 750, 28, T("About this operating system"));
    new Logo(22, 98);
    const char *names[] = {T("Kernel"), T("Kernel version"), T("Build time"), T("Copyright")};
    const char *values[] = {info.kernel_name, info.kernel_version, info.build_time, info.copyright};
    for (int i = 0; i < 4; ++i) {
        Fl_Box *name = new Fl_Box(220, 96 + i * 48, 140, 36, names[i]);
        name->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        Fl_Output *value = new Fl_Output(364, 96 + i * 48, 406, 36);
        value->value(values[i]); value->textsize(14);
    }
    new Fl_Box(220, 292, 550, 28, T("Build and runtime components"));
    Fl_Box *status_label = new Fl_Box(20, 340, 630, 32, status);
    status_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Button *close = new Fl_Button(664, 340, 106, 32, T("Close"));
    close->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
    window.end(); reliefos_fltk::style(&window); window.show();
    printf("[osver.elf] FLTK system information ready kernel=%s version=%s build=%s\n", info.kernel_name, info.kernel_version, info.build_time);
    return Fl::run();
}
