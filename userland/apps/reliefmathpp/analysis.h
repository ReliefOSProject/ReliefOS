#ifndef RELIEFMATHPP_ANALYSIS_H
#define RELIEFMATHPP_ANALYSIS_H
#include "expression.h"
namespace rmp {
struct Result {
  bool ok = false;
  double value = 0, error = 0;
  const char *message = "定义域错误或未收敛";
};
Result derivative(const Expression &, Variables, double x, int order = 1);
Result integrate(const Expression &, Variables, double a, double b,
                 bool length = false);
Result root(const Expression &, Variables, double a, double b);
Result partial(const Expression &, Variables, double x, double y, int axis,
               int order = 1);
Result integrate2d(const Expression &, Variables, double xmin, double xmax,
                   double ymin, double ymax, bool surface_area = false);
Result extremum(const Expression &, Variables, double a, double b,
                bool maximum);
} // namespace rmp
#endif
