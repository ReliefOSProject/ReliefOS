#include "analysis.h"
#include <float.h>
#include <math.h>
namespace rmp {
namespace {
double at(const Expression &e, Variables v, double x) {
  v.x = x;
  return e.eval(v);
}
struct Integrator {
  const Expression &e;
  Variables vars;
  bool length;
  int budget = 50000;
  bool ok = true;
  double error = 0;
  double f(double x) {
    if (--budget < 0) {
      ok = false;
      return NAN;
    }
    Result d;
    if (length) {
      d = derivative(e, vars, x);
      if (!d.ok) {
        ok = false;
        return NAN;
      }
    }
    double y = length ? d.value : at(e, vars, x);
    if (length)
      y = hypot(1, y);
    if (!isfinite(y))
      ok = false;
    return y;
  }
  double sum(double a, double b, double fa, double fm, double fb, double old,
             double tol, int depth) {
    double m = a + (b - a) / 2, left = f(a + (m - a) / 2),
           right = f(m + (b - m) / 2);
    if (!ok)
      return NAN;
    double s1 = (m - a) * (fa + 4 * left + fm) / 6,
           s2 = (b - m) * (fm + 4 * right + fb) / 6;
    double delta = s1 + s2 - old;
    if (fabs(delta) <= 15 * tol) {
      error += fabs(delta) / 15;
      return s1 + s2 + delta / 15;
    }
    if (!depth) {
      ok = false;
      return NAN;
    }
    return sum(a, m, fa, left, fm, s1, tol / 2, depth - 1) +
           sum(m, b, fm, right, fb, s2, tol / 2, depth - 1);
  }
};
} // namespace
namespace {
Result finite_difference(const Expression &e, Variables v, double position,
                         int axis, int order) {
  Result result;
  if (!isfinite(position) || fabs(position) > 1e12 ||
      (order != 1 && order != 2))
    return result;
  auto sample = [&](double value) {
    Variables at = v;
    if (axis == 0)
      at.x = value;
    else
      at.y = value;
    return e.eval(at);
  };
  double center = sample(position);
  if (!isfinite(center))
    return result;
  if (!(e.variables() & (1U << axis))) {
    result.ok = true;
    return result;
  }
  double scale = position == 0 ? 1 : fmin(10, fabs(position));
  double ulp = fabs(nextafter(position, INFINITY) - position);
  double h =
      fmax((order == 1 ? cbrt(DBL_EPSILON) : pow(DBL_EPSILON, .25)) * scale,
           32 * ulp);
  double previous = 0;
  bool have_previous = false;
  for (int n = 0; n < 32 && h >= ulp; ++n, h *= .5) {
    double plus = position + h, minus = position - h, hp = plus - position,
           hm = position - minus;
    if (hp <= 0 || hm <= 0)
      break;
    double hi = sample(plus), lo = sample(minus);
    if (!isfinite(hi) || !isfinite(lo)) {
      have_previous = false;
      continue;
    }
    double right = (hi - center) / hp, left = (center - lo) / hm;
    double value =
        order == 1 ? (hi - lo) / (hp + hm) : 2 * (right - left) / (hp + hm);
    double roundoff =
        order == 1 ? 4 * DBL_EPSILON * (fabs(hi) + fabs(lo)) / (hp + hm)
                   : 8 * DBL_EPSILON *
                         (fabs(hi) + 2 * fabs(center) + fabs(lo)) / (hp * hm);
    if (have_previous) {
      double richardson = value + (value - previous) / 3;
      double error = fabs(value - previous) / 3 + roundoff;
      // Agreement of left/right slopes rejects corners such as abs(x) at zero.
      if (order == 1)
        error += fabs(right - left) / 2;
      if (isfinite(richardson) && isfinite(error) &&
          error <= 1e-5 * fmax(1, fabs(richardson))) {
        result.ok = true;
        result.value = richardson;
        result.error = error;
        return result;
      }
    }
    previous = value;
    have_previous = isfinite(value);
  }
  return result;
}
} // namespace
Result derivative(const Expression &e, Variables v, double x, int order) {
  return finite_difference(e, v, x, 0, order);
}
Result integrate(const Expression &e, Variables v, double a, double b,
                 bool length) {
  Result r;
  if (!isfinite(a) || !isfinite(b) || fabs(a) > 1e12 || fabs(b) > 1e12)
    return r;
  if (a == b) {
    r.ok = true;
    return r;
  }
  double sign = a > b ? -1 : 1;
  if (a > b) {
    double t = a;
    a = b;
    b = t;
  }
  Integrator i{e, v, length};
  double fa = i.f(a), fb = i.f(b), fm = i.f(a + (b - a) / 2);
  if (!i.ok)
    return r;
  // Split first, preventing oscillatory functions from aliasing one Simpson
  // panel.
  double value = 0;
  for (int n = 0; n < 32 && i.ok; ++n) {
    double lo = a + (b - a) * n / 32, hi = a + (b - a) * (n + 1) / 32;
    fa = i.f(lo);
    fb = i.f(hi);
    fm = i.f(lo + (hi - lo) / 2);
    double s = (hi - lo) * (fa + 4 * fm + fb) / 6;
    value += i.sum(lo, hi, fa, fm, fb, s, 1e-9 / 32, 16);
  }
  r.ok = i.ok && isfinite(value);
  r.value = r.ok ? value * (length ? 1 : sign) : NAN;
  r.error = i.error;
  return r;
}
Result root(const Expression &e, Variables v, double a, double b) {
  Result r;
  if (!isfinite(a) || !isfinite(b) || a >= b || fabs(a) > 1e12 ||
      fabs(b) > 1e12)
    return r;
  double fa = at(e, v, a), fb = at(e, v, b);
  if (!isfinite(fa) || !isfinite(fb))
    return r;
  if (fa == 0) {
    r.ok = true;
    r.value = a;
    return r;
  }
  if (fb == 0) {
    r.ok = true;
    r.value = b;
    return r;
  }
  if (signbit(fa) == signbit(fb)) {
    r.message = "端点未夹住零点；请缩小到连续且异号的区间";
    return r;
  }
  double residual_scale = fmax(fabs(fa), fabs(fb));
  for (int n = 0; n < 100; ++n) {
    double m = a + (b - a) / 2, fm = at(e, v, m);
    if (!isfinite(fm))
      return r;
    if (fm == 0 || (fabs(fm) <= residual_scale * 1e-12 &&
                    b - a <= 1e-8 * fmax(1, fabs(m)))) {
      r.ok = true;
      r.value = m;
      r.error = fabs(fm);
      return r;
    }
    if (m == a || m == b)
      break;
    if (signbit(fa) == signbit(fm)) {
      a = m;
      fa = fm;
    } else {
      b = m;
      fb = fm;
    }
  }
  return r; // A narrow bracket is insufficient: poles must not count as roots.
}
Result extremum(const Expression &e, Variables v, double a, double b,
                bool maximum) {
  Result r;
  if (!isfinite(a) || !isfinite(b) || a >= b || fabs(a) > 1e12 ||
      fabs(b) > 1e12)
    return r;
  double best = NAN, bestx = a;
  int index = 0;
  for (int n = 0; n <= 512; ++n) {
    double x = a + (b - a) * n / 512, y = at(e, v, x);
    if (!isfinite(y))
      return r;
    if (!isfinite(best) || (maximum ? y > best : y < best)) {
      best = y;
      bestx = x;
      index = n;
    }
  }
  if (index == 0 || index == 512) {
    r.ok = true;
    r.value = bestx;
    return r;
  }
  double lo = a + (b - a) * (index - 1) / 512,
         hi = a + (b - a) * (index + 1) / 512;
  for (int n = 0; n < 80; ++n) {
    double x = lo + (hi - lo) / 3, y = hi - (hi - lo) / 3, fx = at(e, v, x),
           fy = at(e, v, y);
    if (!isfinite(fx) || !isfinite(fy))
      return r;
    if (maximum ? fx > fy : fx < fy)
      hi = y;
    else
      lo = x;
  }
  r.ok = true;
  r.value = (lo + hi) / 2;
  r.error = hi - lo;
  return r;
}
Result partial(const Expression &e, Variables v, double x, double y, int axis,
               int order) {
  Result failed;
  if (!isfinite(x) || !isfinite(y) || fabs(x) > 1e12 || fabs(y) > 1e12 ||
      (axis != 0 && axis != 1))
    return failed;
  v.x = x;
  v.y = y;
  return finite_difference(e, v, axis == 0 ? x : y, axis, order);
}
Result integrate2d(const Expression &e, Variables v, double a, double b,
                   double c, double d, bool area) {
  Result r;
  if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) || a >= b ||
      c >= d || fabs(a) > 1e6 || fabs(b) > 1e6 || fabs(c) > 1e6 ||
      fabs(d) > 1e6)
    return r;
  auto sample = [&](int n, double &out) {
    double sum = 0;
    for (int row = 0; row <= n; ++row)
      for (int col = 0; col <= n; ++col) {
        v.x = a + (b - a) * col / n;
        v.y = c + (d - c) * row / n;
        double value = e.eval(v);
        if (!isfinite(value))
          return false;
        if (area) {
          Result dx = partial(e, v, v.x, v.y, 0),
                 dy = partial(e, v, v.x, v.y, 1);
          if (!dx.ok || !dy.ok)
            return false;
          value = sqrt(1 + dx.value * dx.value + dy.value * dy.value);
        }
        int wx = col == 0 || col == n ? 1
                 : col % 2            ? 4
                                      : 2,
            wy = row == 0 || row == n ? 1
                 : row % 2            ? 4
                                      : 2;
        sum += value * wx * wy;
      }
    out = sum * (b - a) * (d - c) / (9 * n * n);
    return isfinite(out) != 0;
  };
  double coarse = 0, fine = 0;
  if (!sample(24, coarse) || !sample(48, fine))
    return r;
  r.error = fabs(fine - coarse) / 15;
  r.ok = r.error < 1e-4 * fmax(1, fabs(fine));
  r.value = fine;
  return r;
}
} // namespace rmp
