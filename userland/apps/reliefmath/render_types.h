#ifndef RELIEFMATH_RENDER_TYPES_H
#define RELIEFMATH_RENDER_TYPES_H
#include "model.h"
namespace rm {
enum Artifact : uint8_t { WantRaster=1, WantSvg=2, WantPng=4 };
struct Snapshot {
    uint64_t request_id=0, session_id=0;
    Formula formula;
    double scale=1.0;
    uint32_t dpi=96;
    uint8_t artifacts=WantRaster;
};
struct Raster { uint32_t width=0,height=0,stride=0; Bytes rgba; };
struct Rendered {
    Snapshot snapshot;
    FormulaId formula_id=0;
    uint64_t request_id=0,generation=0;
    Bytes svg,png;
    Raster raster;
    double baseline=0;
};
bool same_snapshot(const Snapshot &,const Snapshot &);
}
#endif
