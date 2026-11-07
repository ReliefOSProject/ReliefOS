#ifndef RELIEFDIAGRAM_MODEL_H
#define RELIEFDIAGRAM_MODEL_H
#include "storage.h"
namespace rd {
enum class Shape {
  Rectangle,
  Rounded,
  Diamond,
  Ellipse,
  Parallelogram,
  Document,
  Database,
  Note,
  Count
};
enum class Route { Straight, Orthogonal, Curve, Count };
enum class Layout { Mind, Tree, Flow };
struct Point {
  double x = 0, y = 0;
};
struct Rect {
  double x = 0, y = 0, w = 0, h = 0;
};
struct Style {
  uint32_t fill = 0xe8f0fb, line = 0x527bb2, text = 0x202d40;
  int font = 16;
  bool bold = false;
};
struct Node {
  uint64_t id = 0, parent = 0;
  Shape shape = Shape::Rounded;
  double x = 0, y = 0, w = 160, h = 64;
  Style style;
  bool collapsed = false;
  Text text, note;
};
struct Edge {
  uint64_t id = 0, from = 0, to = 0;
  Route route = Route::Orthogonal;
  unsigned arrows = 1, width = 2;
  uint32_t color = 0x65768c;
  Text text;
};
struct View {
  double cx = 0, cy = 0, zoom = 1;
  bool grid = true, snap = true;
};
struct Document {
  Text title;
  Array<Node> nodes;
  Array<Edge> edges;
  uint64_t next_id = 1;
  View view;
  bool clone_from(const Document &);
};
// One bounded parent index per bulk operation: no repeated ancestor ID scans.
class TreeIndex {
  const Document &document_;
  int parents_[MaxNodes]{};
  bool shown_[MaxNodes]{}, valid_ = false;

public:
  explicit TreeIndex(const Document &);
  int index(uint64_t) const;
  bool valid() const { return valid_; }
  bool shown(size_t i) const {
    return valid_ && i < document_.nodes.size() && shown_[i];
  }
  bool descendant(size_t i, uint64_t ancestor) const;
};
const char *shape_name(Shape);
Node *node(Document &, uint64_t);
const Node *node(const Document &, uint64_t);
Edge *edge(Document &, uint64_t);
bool utf8(const char *, size_t);
bool validate(const Document &, Text &);
bool visible(const Document &, const Node &);
bool descendant(const Document &, uint64_t id, uint64_t ancestor);
bool add_node(Document &, Shape, const char *, double x, double y,
              uint64_t parent, uint64_t &id, Text &);
bool add_edge(Document &, uint64_t from, uint64_t to, Route, uint64_t &id,
              Text &);
void erase(Document &, const Array<uint64_t> &, bool subtree = true);
bool duplicate(Document &, const Array<uint64_t> &, Array<uint64_t> &result,
               Text &);
bool arrange(Document &, Layout, Text &);
bool example(Document &, int, Text &);
class History {
  Array<Document> past_, future_;
  bool transfer(Array<Document> &, Array<Document> &, Document &);

public:
  bool checkpoint(const Document &);
  bool undo(Document &);
  bool redo(Document &);
  void clear() {
    past_.clear();
    future_.clear();
  }
};
} // namespace rd
#endif
