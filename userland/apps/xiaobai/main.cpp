extern "C" {
#include "model.h"
}
#include "../fltk_png_app.h"
int main(int argc, char **argv)
{
    return reliefos_fltk::png_main<xiaobai_fit>(argc, argv, "xiaobai", xiaobai_fit_rect);
}
