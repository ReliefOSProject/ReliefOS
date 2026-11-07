#include "../../userland/apps/reliefmathpp/analysis.h"
#include "../../userland/apps/reliefmathpp/document.h"
#include "../../userland/apps/reliefmathpp/expression.h"
#include "../../userland/apps/reliefmathpp/mesh.h"
#include "../../userland/apps/reliefmathpp/model.h"
#include "../../userland/apps/reliefmathpp/scene.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
using namespace rmp;
static void near(double a, double b, double eps = 1e-6) {
  assert(isfinite(a));
  assert(fabs(a - b) < eps);
}
int main() {
  Expression e;
  Text error;
  Variables v;
  assert(e.compile("-2^2", error));
  near(e.eval(v), -4);
  assert(e.compile("2^3^2", error));
  near(e.eval(v), 512);
  assert(e.compile("sin(pi/2)+max(2,3)+if(x>0,1,-1)", error));
  v.x = 1;
  near(e.eval(v), 5);
  assert(!e.compile("sin(", error));
  assert(!e.compile("system(1)", error));
  assert(!e.compile("2x", error));
  assert(!e.compile("1e999", error));
  assert(e.compile("x^2", error));
  v.x = 3;
  near(e.eval(v), 9);
  near(derivative(e, v, 3).value, 6);
  near(derivative(e, v, 3, 2).value, 2, 1e-4);
  auto result = integrate(e, v, 0, 3);
  assert(result.ok);
  near(result.value, 9);
  assert(e.compile("sin(x)", error));
  result = root(e, v, 3, 4);
  assert(result.ok);
  near(result.value, acos(-1));
  assert(e.compile("1/x", error));
  assert(!root(e, v, -1, 1).ok);
  assert(!integrate(e, v, -1, 1).ok);
  assert(!e.compile("x?", error));
  assert(e.compile("sqrt(-1)", error));
  assert(!isfinite(e.eval(v)));
  Document doc;
  assert(example(doc, 0));
  Text why;
  assert(validate(doc, why));
  Object circle = make_object(Kind::Circle2);
  circle.p[2] = 2;
  near(measure(circle).area, 4 * acos(-1));
  Object sphere = make_object(Kind::Sphere3);
  sphere.p[3] = 3;
  near(measure(sphere).volume, 36 * acos(-1));
  Vec3 cross = cross_product({1, 0, 0}, {0, 1, 0});
  near(cross.z, 1);
  Sampler sampler;
  Mesh mesh;
  Object pole = make_object(Kind::Function2);
  pole.formula[0].set("1/x");
  assert(sampler.begin(pole, Variables{}, mesh));
  while (!sampler.step(mesh, 8)) {
  };
  for (size_t n = 0; n < mesh.lines.size(); ++n)
    assert(!(mesh.lines[n].a.x < 0 && mesh.lines[n].b.x > 0));
  for (int n = 0; n < int(Kind::Count); ++n) {
    Object o = make_object(Kind(n));
    Mesh m;
    Sampler job;
    assert(job.begin(o, Variables{}, m));
    int slices = 0;
    while (!job.step(m, 8))
      assert(++slices < 10000);
    assert(m.error.empty());
    assert(m.lines.size() + m.faces.size() + m.points.size() > 0);
  }
  Bytes bytes;
  assert(encode(doc, bytes, why));
  Document decoded;
  assert(decode(bytes.data(), bytes.size(), decoded, why));
  assert(decoded.objects.size() == doc.objects.size());
  size_t kept = decoded.objects.size();
  bytes[bytes.size() - 1] ^= 1;
  assert(!decode(bytes.data(), bytes.size(), decoded, why));
  assert(decoded.objects.size() == kept);
  assert(!decode(bytes.data(), 7, decoded, why));
  doc.objects[0].p[0] = NAN;
  assert(!encode(doc, bytes, why));
  pole.formula[0].set("1/(x-0.123)");
  assert(sampler.begin(pole, Variables{}, mesh));
  while (!sampler.step(mesh, 8)) {
  };
  for (size_t n = 0; n < mesh.lines.size(); ++n)
    assert(!(mesh.lines[n].a.x < 0.123 && mesh.lines[n].b.x > 0.123));
  Array<Mesh> meshes;
  assert(example(doc, 1));
  assert(meshes.resize(doc.objects.size()));
  for (size_t n = 0; n < doc.objects.size(); ++n) {
    Sampler j;
    assert(j.begin(doc.objects[n], doc.vars, meshes[n]));
    while (!j.step(meshes[n], 8)) {
    };
  }
  Frame f;
  assert(frame(doc, meshes, 1, 800, 500, f));
  assert(f.items.size() > 1000);
  assert(svg(f, bytes, why));
  Text xml;
  assert(xml.set(bytes.data(), bytes.size()));
  assert(strstr(xml.c_str(), "<svg"));
  doc.view.tx = 10000;
  doc.view.ty = -20000;
  doc.view.tz = 30000;
  auto projected = project({10000, -20000, 30000}, doc.view, 800, 500);
  near(projected.x, 400);
  near(projected.y, 265);
  History history;
  assert(history.checkpoint(doc));
  doc.vars.a = 3;
  assert(history.undo(doc));
  near(doc.vars.a, 1);
  assert(history.redo(doc));
  near(doc.vars.a, 3);
  Object bad = make_object(Kind::Sphere3);
  bad.p[3] = -1;
  assert(!valid_object(bad, why));
  bad = make_object(Kind::Plane3);
  bad.p[2] = 0;
  assert(!valid_object(bad, why));
  bad = make_object(Kind::Function2);
  bad.formula[0].set("y");
  assert(!valid_object(bad, why));
  assert(e.compile("if(x<0,sqrt(-x),sqrt(x))", why));
  v.x = -4;
  near(e.eval(v), 2);
  assert(e.compile("1/(x-0.123)", why));
  assert(!root(e, v, 0, 1).ok);
  assert(!integrate(e, v, 0, 1).ok);
  assert(e.compile("x^2+y^2", why));
  auto dx = partial(e, Variables{}, 2, 3, 0);
  auto dy = partial(e, Variables{}, 2, 3, 1);
  assert(dx.ok && dy.ok);
  near(dx.value, 4);
  near(dy.value, 6);
  assert(e.compile("x+y", why));
  auto area = integrate2d(e, Variables{}, 0, 1, 0, 1);
  assert(area.ok);
  near(area.value, 1);
  assert(e.compile("0", why));
  area = integrate2d(e, Variables{}, 0, 2, 0, 3, true);
  assert(area.ok);
  near(area.value, 6);
  assert(e.compile("sin(x)", why));
  auto second_large = derivative(e, Variables{}, 1e8, 2);
  assert(!second_large.ok || fabs(second_large.value + sin(1e8)) < 1e-4);
  assert(e.compile("ln(x)", why));
  auto tiny = derivative(e, Variables{}, 1e-8);
  assert(tiny.ok);
  near(tiny.value / 1e8, 1, 1e-5);
  assert(e.compile("1e-12*(x^2+1)", why));
  assert(!root(e, Variables{}, 0, 1).ok);
  assert(e.compile("abs(x)", why));
  assert(!derivative(e, Variables{}, 0).ok);
  puts("ok - ReliefMath++ expressions, analysis, geometry, mesh and document "
       "transactions");
}
