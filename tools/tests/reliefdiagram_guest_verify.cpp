#include "../../userland/apps/reliefdiagram/document.h"
#include "../../userland/apps/reliefdiagram/scene.h"
#include <assert.h>
#include <stdio.h>
using namespace rd;
static Document read(const char *path) {
  FILE *f = fopen(path, "rb");
  assert(f);
  assert(!fseek(f, 0, SEEK_END));
  long len = ftell(f);
  assert(len > 0 && len < long(MaxFileBytes));
  rewind(f);
  Bytes b;
  assert(b.resize(size_t(len)));
  assert(fread(b.data(), 1, b.size(), f) == b.size());
  assert(!fclose(f));
  Document d;
  Text e;
  assert(decode(b.data(), b.size(), d, e));
  return d;
}
static bool text(const Document &d, const char *s) {
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (!strcmp(d.nodes[i].text.c_str(), s))
      return true;
  return false;
}
int main(int argc, char **argv) {
  assert(argc == 3);
  char path[4096];
  Text e;
  if (!strcmp(argv[1], "prepare")) {
    for (int kind = 0; kind < 3; ++kind) {
      Document d;
      assert(example(d, kind, e));
      Frame frame;
      assert(build_frame(d, frame, e));
      snprintf(path, sizeof(path), "%s/template-%d.json", argv[2], kind);
      FILE *json = fopen(path, "w");
      assert(json);
      fprintf(json, "{\"bounds\":[%.3f,%.3f,%.3f,%.3f],\"nodes\":[",
              frame.bounds.x, frame.bounds.y, frame.bounds.w, frame.bounds.h);
      for (size_t i = 0; i < d.nodes.size(); ++i) {
        const Node &n = d.nodes[i];
        fprintf(json, "%s[%.3f,%.3f,%.3f,%.3f]", i ? "," : "", n.x, n.y, n.w,
                n.h);
      }
      fputs("]}\n", json);
      assert(!fclose(json));
    }
    snprintf(path, sizeof(path), "%s/invalid.rdiagram", argv[2]);
    FILE *bad = fopen(path, "wb");
    assert(bad);
    fputs("RDGM broken document", bad);
    assert(!fclose(bad));
    return 0;
  }
  snprintf(path, sizeof(path), "%s/acceptance.rdiagram", argv[2]);
  Document mind = read(path);
  assert(mind.nodes.size() == 14 && mind.edges.empty());
  assert(text(mind, "Diagram review"));
  assert(text(mind, "Timeline"));
  assert(!mind.nodes[0].collapsed);
  snprintf(path, sizeof(path), "%s/flow.rdiagram", argv[2]);
  Document flow = read(path);
  assert(flow.nodes.size() == 7 && flow.edges.size() == 8);
  assert(!strcmp(flow.edges[7].text.c_str(), "summary"));
  assert(flow.nodes[3].x != 300 || flow.nodes[3].y != 210);
  assert(flow.nodes[2].w != 180 || flow.nodes[2].h != 100);
  snprintf(path, sizeof(path), "%s/logic.rdiagram", argv[2]);
  Document logic = read(path);
  assert(logic.nodes.size() == 5 && logic.edges.size() == 5);
  puts("ReliefDiagram guest documents independently verified: edited mind map, "
       "moved flow node, labelled connector and logic diagram");
}
