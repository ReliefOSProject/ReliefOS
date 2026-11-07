extern "C" {
#include "model.h"
}
#include "../fltk_png_app.h"
int main(int argc, char **argv)
{
    return reliefos_fltk::png_main<leonmmcoset_fit>(argc, argv, "leonmmcoset", leonmmcoset_fit_rect);
}
