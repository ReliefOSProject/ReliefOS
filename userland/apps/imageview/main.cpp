#include "../fltk_support.h"
#include "../fltk_pixels.h"
#include "../fltk_files.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Toggle_Button.H>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
extern "C" {
#include "model.h"
#include <reliefos/png.h>
}

static uint32_t *pixels;
static unsigned image_width, image_height, sibling_count, sibling_index;
static imageview_zoom zoom = IMAGEVIEW_ZOOM_FIT;
static char path[PATH_MAX], directory[PATH_MAX], siblings[64][NAME_MAX+1];
static Fl_Output *path_field;
static Fl_Box *details, *status;
static Fl_Button *previous_button, *next_button;
static Fl_Toggle_Button *zoom_buttons[3];
static Fl_Widget *canvas;

static void refresh()
{
    char text[192];
    imageview_format_detail(text, sizeof(text), image_width, image_height, zoom, sibling_index, sibling_count);
    details->copy_label(text); path_field->value(path);
    if (sibling_count > 1) { previous_button->activate(); next_button->activate(); }
    else { previous_button->deactivate(); next_button->deactivate(); }
    for (int i = 0; i < 3; ++i) zoom_buttons[i]->value(i == zoom);
    canvas->redraw();
}

static void find_siblings()
{
    reliefos_fltk::copy_input(directory, sizeof(directory), path);
    char *slash = strrchr(directory, '/');
    const char *base = strrchr(path, '/'); base = base ? base+1 : path;
    if (!slash) snprintf(directory, sizeof(directory), ".");
    else if (slash == directory) directory[1] = 0;
    else *slash = 0;
    sibling_count = sibling_index = 0;
    DIR *dir = opendir(directory);
    if (!dir) return;
    dirent *entry;
    while (sibling_count < 64 && (entry = readdir(dir))) {
        if (!imageview_is_supported_path(entry->d_name)) continue;
        char child[PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s%s%s", directory, !strcmp(directory,"/") ? "" : "/", entry->d_name);
        struct stat st{};
        if (length < 0 || length >= static_cast<int>(sizeof(child)) || stat(child, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        if (!reliefos_fltk::copy_input(siblings[sibling_count], sizeof(siblings[0]), entry->d_name)) continue;
        if (!strcmp(entry->d_name, base)) sibling_index = sibling_count;
        ++sibling_count;
    }
    closedir(dir);
}

static bool load(const char *selected)
{
    char next_path[PATH_MAX];
    if (!selected || !imageview_is_supported_path(selected)) {
        status->copy_label(T("Unsupported image format. Use BMP, DIB, or PNG.")); return false;
    }
    if (!reliefos_fltk::copy_input(next_path, sizeof(next_path), selected)) {
        status->copy_label(T("Input is too long")); return false;
    }
    uint32_t *decoded = nullptr, width = 0, height = 0;
    int result;
    if (reliefos_fltk::suffix(selected, ".png")) result = reliefos_png_decode_file(selected, &decoded, &width, &height);
    else {
        unsigned char *bytes = nullptr; unsigned length = 0;
        result = reliefos_fltk::read_file(selected, 8*1024*1024, &bytes, &length);
        if (result == 0) result = imageview_bmp_decode(bytes, length, &decoded, &width, &height);
        free(bytes);
    }
    if (result < 0 || !decoded || !width || !height) {
        free(decoded); status->copy_label(T("Could not open image")); return false;
    }
    free(pixels); pixels = decoded; image_width = width; image_height = height;
    reliefos_fltk::copy_input(path, sizeof(path), next_path); find_siblings();
    status->copy_label(T("Image loaded")); refresh(); return true;
}

static void open_image(Fl_Widget *, void *)
{
    char selected[PATH_MAX];
    if (reliefos_fltk::choose_file(T("Open image"), path[0] ? path : ".", selected, sizeof(selected))) load(selected);
}
static void navigate(Fl_Widget *, void *delta)
{
    if (sibling_count < 2) return;
    unsigned next = imageview_next_index(sibling_index, sibling_count, static_cast<int>(reinterpret_cast<intptr_t>(delta)));
    char selected[PATH_MAX];
    int count = snprintf(selected, sizeof(selected), "%s%s%s", directory, !strcmp(directory,"/") ? "" : "/", siblings[next]);
    if (count > 0 && count < static_cast<int>(sizeof(selected))) load(selected);
}

class Canvas : public Fl_Widget {
public:
    Canvas() : Fl_Widget(12, 100, 876, 458) {}
    void draw() override
    {
        fl_push_clip(x(), y(), w(), h()); fl_color(FL_WHITE); fl_rectf(x(), y(), w(), h());
        unsigned width = 0, height = 0;
        imageview_zoom_dims(image_width, image_height, zoom, w()>12 ? w()-12 : 1, h()>12 ? h()-12 : 1, &width, &height);
        reliefos_fltk::draw_pixels(pixels, image_width, image_height,
            x()+(w()>static_cast<int>(width) ? (w()-width)/2 : 0),
            y()+(h()>static_cast<int>(height) ? (h()-height)/2 : 0), width, height);
        fl_pop_clip();
    }
};
class Viewer : public Fl_Double_Window {
public:
    Viewer() : Fl_Double_Window(900, 620, T("Image Viewer")) {}
    int handle(int event) override
    {
        if (event == FL_SHORTCUT || event == FL_KEYDOWN) {
            if (Fl::event_key() == FL_Left) { navigate(nullptr, reinterpret_cast<void *>(-1)); return 1; }
            if (Fl::event_key() == FL_Right) { navigate(nullptr, reinterpret_cast<void *>(1)); return 1; }
        }
        return Fl_Double_Window::handle(event);
    }
};

int main(int argc, char **argv)
{
    reliefos_fltk::init("imageview");
    Viewer window;
    Fl_Button *open = new Fl_Button(12, 12, 100, 34, T("Open")); open->callback(open_image); open->shortcut(FL_CTRL+'o');
    previous_button = new Fl_Button(120, 12, 120, 34, T("Previous")); previous_button->callback(navigate, reinterpret_cast<void *>(-1));
    next_button = new Fl_Button(248, 12, 120, 34, T("Next Image")); next_button->callback(navigate, reinterpret_cast<void *>(1));
    const char *labels[] = {T("Fit"), "1x", "2x"};
    for (int i = 0; i < 3; ++i) {
        zoom_buttons[i] = new Fl_Toggle_Button(388+i*64, 12, 58, 34, labels[i]);
        zoom_buttons[i]->callback([](Fl_Widget *, void *index) { zoom = static_cast<imageview_zoom>(reinterpret_cast<intptr_t>(index)); refresh(); }, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    path_field = new Fl_Output(12, 56, 876, 32); path_field->textsize(15);
    canvas = new Canvas;
    details = new Fl_Box(12, 566, 876, 22);
    status = new Fl_Box(12, 590, 876, 24, T("Ready")); status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    window.end(); window.resizable(canvas); window.size_range(600, 360); reliefos_fltk::style(&window);
    refresh(); if (argc > 1) load(argv[1]); window.show();
    puts("[imageview.elf] FLTK image viewer ready");
    int result = Fl::run(); free(pixels); return result;
}
