#ifndef RELIEFMATHPP_EXPRESSION_H
#define RELIEFMATHPP_EXPRESSION_H
#include "storage.h"
namespace rmp {
struct Variables {
  double x = 0, y = 0, z = 0, t = 0, u = 0, v = 0, a = 1, b = 1, c = 1, d = 1;
  double get(int i) const;
};
class Parser;
class Expression {
  friend class Parser;
  struct Instruction {
    int op = 0, index = 0;
    double value = 0;
  };
  Instruction code_[512];
  int count_ = 0;
  unsigned variables_ = 0;

public:
  bool compile(const char *, Text &error, unsigned allowed = 1023);
  double eval(const Variables &) const;
  unsigned variables() const { return variables_; }
};
} // namespace rmp
#endif
