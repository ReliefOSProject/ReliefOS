#include "../fltk_support.h"
#include "../fltk_pixels.h"
#include "../fltk_files.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>
#include <limits.h>
#include <stdio.h>
extern "C" {
#include "model.h"
#include <reliefos/png.h>
#include <png.h>
}

static uint32_t *pixels;
static unsigned image_width, image_height, brush_color, brush_size = 4;
static paint_tool tool = PAINT_TOOL_BRUSH;
static bool dirty;
static char current_path[PATH_MAX];
static Fl_Box *status;
static Fl_Widget *canvas;
static Fl_Scroll *scroll;
static Fl_Double_Window *window;
static Fl_Button *colors[6];
static const unsigned palette[] = {0, 0xff0000, 0x0080ff, 0x00aa00, 0xffff00, 0xffffff};

static void title()
{
    char text[PATH_MAX + 64];
    snprintf(text, sizeof(text), "%s%s — %s", dirty ? "* " : "", current_path[0] ? current_path : T("Untitled"), T("Paint"));
    window->copy_label(text);
}
static void refresh()
{
    canvas->size(image_width, image_height); canvas->redraw(); scroll->redraw(); title();
}
static bool new_canvas()
{
    uint32_t *next = static_cast<uint32_t *>(malloc(800*520*sizeof(uint32_t)));
    if (!next) { status->copy_label(T("Could not create canvas")); return false; }
    for (unsigned i = 0; i < 800*520; ++i) next[i] = 0xffffff;
    free(pixels); pixels = next; image_width = 800; image_height = 520;
    current_path[0] = 0; dirty = false; scroll->scroll_to(0,0); refresh(); return true;
}
static bool discard()
{
    if (!dirty) return true;
    fl_message_title(T("Discard changes?"));
    return fl_choice("%s", T("Keep"), T("Discard"), nullptr, T("The current drawing has not been saved.")) == 1;
}
static bool load(const char *path)
{
    char next_path[PATH_MAX];
    if (!reliefos_fltk::copy_input(next_path, sizeof(next_path), path)) {
        status->copy_label(T("Input is too long")); return false;
    }
    uint32_t *next = nullptr, width = 0, height = 0;
    int result;
    if (reliefos_fltk::suffix(path, ".png")) result = reliefos_png_decode_file(path, &next, &width, &height);
    else {
        unsigned char *bytes = nullptr; unsigned length = 0;
        result = reliefos_fltk::read_file(path, RELIEFOS_PNG_MAX_FILE_BYTES, &bytes, &length);
        if (result == 0) result = paint_bmp_decode(bytes, length, &next, &width, &height);
        free(bytes);
    }
    if (result < 0 || !next || !width || !height) {
        free(next); status->copy_label(T("Could not open image")); return false;
    }
    free(pixels); pixels = next; image_width = width; image_height = height;
    reliefos_fltk::copy_input(current_path, sizeof(current_path), next_path);
    dirty = false; scroll->scroll_to(0,0); refresh(); status->copy_label(T("Image opened")); return true;
}
static bool save(const char *path)
{
    char selected[PATH_MAX];
    if (!pixels || !reliefos_fltk::copy_input(selected, sizeof(selected), path)) return false;
    int result = -1;
    if (reliefos_fltk::suffix(path, ".png")) {
        unsigned char *rgb = static_cast<unsigned char *>(malloc((uint64_t)image_width*image_height*3));
        if (rgb) {
            for (uint64_t i = 0; i < (uint64_t)image_width*image_height; ++i) {
                rgb[i*3] = pixels[i] >> 16; rgb[i*3+1] = pixels[i] >> 8; rgb[i*3+2] = pixels[i];
            }
            png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = image_width;
            image.height = image_height; image.format = PNG_FORMAT_RGB;
            result = png_image_write_to_file(&image, path, 0, rgb, 0, nullptr) ? 0 : -1;
            png_image_free(&image); free(rgb);
        }
    } else {
        unsigned char *bytes = nullptr; unsigned length = 0;
        if (paint_bmp_encode(pixels, image_width, image_height, &bytes, &length) == 0)
            result = reliefos_fltk::write_file(path, bytes, length);
        free(bytes);
    }
    if (result < 0) { status->copy_label(T("Could not save image")); return false; }
    reliefos_fltk::copy_input(current_path, sizeof(current_path), selected);
    dirty = false; title(); status->copy_label(T("Image saved")); return true;
}
static bool save_as()
{
    char path[PATH_MAX];
    if (!reliefos_fltk::choose_file(T("Save image"), current_path[0] ? current_path : "untitled.bmp", path, sizeof(path), true)) return false;
    struct stat st{};
    if (stat(path, &st) == 0 && fl_choice("%s", T("Cancel"), T("Save"), nullptr, T("Replace existing file?")) != 1) return false;
    return save(path);
}
static bool save_current() { return current_path[0] ? save(current_path) : save_as(); }
static void close(Fl_Widget *, void *)
{
    if (dirty) {
        fl_message_title(T("Save changes?"));
        int choice = fl_choice("%s", T("Cancel"), T("Save"), T("Discard"), T("Save the current drawing before closing?"));
        if (choice == 0 || (choice == 1 && !save_current())) return;
    }
    window->hide();
}

class Canvas : public Fl_Widget {
    bool drawing_ = false;
    unsigned last_x_ = 0, last_y_ = 0;
public:
    Canvas(int x, int y) : Fl_Widget(x, y, 800, 520) {}
    void draw() override
    {
        fl_push_clip(x(), y(), w(), h());
        reliefos_fltk::draw_pixels(pixels, image_width, image_height, x(), y(), image_width, image_height);
        fl_pop_clip();
    }
    int handle(int event) override
    {
        if (event == FL_RELEASE) { drawing_ = false; return 1; }
        if (event == FL_PUSH) {
            if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
            drawing_ = true;
        } else if (event != FL_DRAG || !drawing_) return Fl_Widget::handle(event);
        int px = Fl::event_x()-x(), py = Fl::event_y()-y();
        if (px < 0) px = 0; if (py < 0) py = 0;
        if (px >= static_cast<int>(image_width)) px = image_width-1;
        if (py >= static_cast<int>(image_height)) py = image_height-1;
        if (event == FL_PUSH) paint_draw_point(pixels, image_width, image_height, px, py, brush_color, brush_size, tool);
        else paint_draw_line(pixels, image_width, image_height, last_x_, last_y_, px, py, brush_color, brush_size, tool);
        last_x_ = px; last_y_ = py; dirty = true; redraw(); title(); return 1;
    }
};

int main(int argc, char **argv)
{
    reliefos_fltk::init("paint");
    Fl_Double_Window root(940, 700, T("Paint")); window = &root; root.callback(close);
    const char *labels[] = {T("New"), T("Open"), T("Save"), T("Save as")};
    for (int i = 0; i < 4; ++i) {
        Fl_Button *button = new Fl_Button(12+i*122, 12, 114, 34, labels[i]);
        button->shortcut(FL_CTRL + "nosa"[i]);
        button->callback([](Fl_Widget *, void *action) {
            switch (reinterpret_cast<intptr_t>(action)) {
            case 0: if (discard()) new_canvas(); break;
            case 1: {
                if (!discard()) break;
                char path[PATH_MAX];
                if (reliefos_fltk::choose_file(T("Open image"), current_path[0] ? current_path : ".", path, sizeof(path))) load(path);
                break;
            }
            case 2: save_current(); break;
            case 3: save_as(); break;
            }
        }, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    Fl_Choice *tools = new Fl_Choice(12, 56, 168, 32);
    tools->add(T("Pencil")); tools->add(T("Brush")); tools->add(T("Eraser")); tools->value(1); tools->textsize(15);
    tools->callback([](Fl_Widget *widget, void *) { tool = static_cast<paint_tool>(static_cast<Fl_Choice *>(widget)->value()); });
    Fl_Choice *sizes = new Fl_Choice(194, 56, 124, 32);
    sizes->add("2 px"); sizes->add("4 px"); sizes->add("6 px"); sizes->add("14 px"); sizes->value(1);
    sizes->callback([](Fl_Widget *widget, void *) { static const unsigned values[] = {2,4,6,14}; brush_size = values[static_cast<Fl_Choice *>(widget)->value()]; });
    for (int i = 0; i < 6; ++i) {
        colors[i] = new Fl_Button(336+i*40, 56, 32, 32);
        unsigned rgb = palette[i]; colors[i]->color(fl_rgb_color(rgb >> 16, rgb >> 8, rgb));
        colors[i]->box(i == 0 ? FL_DOWN_BOX : FL_UP_BOX);
        colors[i]->callback([](Fl_Widget *, void *index) {
            int selected = reinterpret_cast<intptr_t>(index); brush_color = palette[selected];
            for (int i = 0; i < 6; ++i) { colors[i]->box(i == selected ? FL_DOWN_BOX : FL_UP_BOX); colors[i]->redraw(); }
        }, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    scroll = new Fl_Scroll(12, 102, 916, 548); scroll->box(FL_DOWN_FRAME);
    canvas = new Canvas(14, 104); scroll->end();
    status = new Fl_Box(12, 660, 916, 28, T("Ready")); status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    root.end(); root.resizable(scroll); root.size_range(600, 400); reliefos_fltk::style(&root);
    if (!new_canvas()) return 1;
    if (argc > 1) load(argv[1]);
    root.show(); puts("[paint.elf] FLTK paint ready");
    int result = Fl::run(); free(pixels); return result;
}
