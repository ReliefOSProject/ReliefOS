#include "render_types.h"
namespace rm {
bool same_snapshot(const Snapshot &a,const Snapshot &b) {
    return a.session_id==b.session_id && a.formula.id==b.formula.id &&
        a.formula.generation==b.formula.generation && a.formula.source==b.formula.source &&
        a.formula.style.rgba==b.formula.style.rgba && a.formula.style.point_size==b.formula.style.point_size &&
        a.formula.style.padding==b.formula.style.padding && a.formula.style.display==b.formula.style.display &&
        a.scale==b.scale && a.dpi==b.dpi;
}
}
