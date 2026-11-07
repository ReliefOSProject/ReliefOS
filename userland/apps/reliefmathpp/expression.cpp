#include "expression.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
namespace rmp {
namespace {
enum Op {
  Constant,
  Variable,
  Neg,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Pow,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  And,
  Or,
  Sin,
  Cos,
  Tan,
  Asin,
  Acos,
  Atan,
  Sinh,
  Cosh,
  Tanh,
  Exp,
  Log,
  Log10,
  Log2,
  Sqrt,
  Abs,
  Floor,
  Ceil,
  Round,
  Sign,
  Min,
  Max,
  Atan2,
  Hypot,
  If
};
struct Function {
  const char *name;
  int arity, op;
};
const Function functions[] = {
    {"sin", 1, Sin},     {"cos", 1, Cos},     {"tan", 1, Tan},
    {"asin", 1, Asin},   {"acos", 1, Acos},   {"atan", 1, Atan},
    {"sinh", 1, Sinh},   {"cosh", 1, Cosh},   {"tanh", 1, Tanh},
    {"exp", 1, Exp},     {"ln", 1, Log},      {"log", 1, Log},
    {"log10", 1, Log10}, {"log2", 1, Log2},   {"sqrt", 1, Sqrt},
    {"abs", 1, Abs},     {"floor", 1, Floor}, {"ceil", 1, Ceil},
    {"round", 1, Round}, {"sign", 1, Sign},   {"min", 2, Min},
    {"max", 2, Max},     {"pow", 2, Pow},     {"atan2", 2, Atan2},
    {"hypot", 2, Hypot}, {"if", 3, If}};
} // namespace
double Variables::get(int i) const {
  switch (i) {
  case 0:
    return x;
  case 1:
    return y;
  case 2:
    return z;
  case 3:
    return t;
  case 4:
    return u;
  case 5:
    return v;
  case 6:
    return a;
  case 7:
    return b;
  case 8:
    return c;
  default:
    return d;
  }
}
class Parser {
  Expression &out_;
  const char *start_, *p_;
  Text &error_;
  unsigned allowed_;
  int depth_ = 0;
  bool fail(const char *s) {
    if (error_.empty()) {
      char b[256];
      snprintf(b, sizeof(b), "%s（位置 %zu）", s, size_t(p_ - start_ + 1));
      error_.set(b);
    }
    return false;
  }
  void spaces() {
    while (*p_ && isspace(static_cast<unsigned char>(*p_)))
      ++p_;
  }
  bool emit(int op, int index = 0, double value = 0) {
    if (out_.count_ == 512)
      return fail("公式过于复杂");
    out_.code_[out_.count_++] = {op, index, value};
    return true;
  }
  bool take(char ch) {
    spaces();
    if (*p_ != ch)
      return false;
    ++p_;
    return true;
  }
  bool atom() {
    spaces();
    if (*p_ == '+' || *p_ == '-') {
      char sign = *p_++;
      return expr(7) && (sign == '+' || emit(Neg));
    }
    if (take('('))
      return expr(0) && (take(')') || fail("缺少右括号"));
    if (isdigit(static_cast<unsigned char>(*p_)) || *p_ == '.') {
      char *end;
      double n = strtod(p_, &end);
      if (end == p_ || !isfinite(n))
        return fail("无效或超范围的数值");
      p_ = end;
      return emit(Constant, 0, n);
    }
    if (!isalpha(static_cast<unsigned char>(*p_)))
      return fail("需要数值、变量或函数");
    const char *at = p_;
    while (isalnum(static_cast<unsigned char>(*p_)) || *p_ == '_')
      ++p_;
    size_t n = size_t(p_ - at);
    char name[32];
    if (n >= sizeof(name))
      return fail("名称过长");
    memcpy(name, at, n);
    name[n] = 0;
    if (!strcmp(name, "pi"))
      return emit(Constant, 0, acos(-1));
    if (!strcmp(name, "tau"))
      return emit(Constant, 0, 2 * acos(-1));
    if (!strcmp(name, "e"))
      return emit(Constant, 0, exp(1));
    const char *vars = "xyztuvabcd";
    const char *v = n == 1 ? strchr(vars, name[0]) : nullptr;
    if (v) {
      int index = int(v - vars);
      if (!(allowed_ & (1U << index)))
        return fail("此对象不支持该变量");
      out_.variables_ |= 1U << index;
      return emit(Variable, index);
    }
    const Function *f = nullptr;
    for (const auto &entry : functions)
      if (!strcmp(entry.name, name)) {
        f = &entry;
        break;
      }
    if (!f)
      return fail("未知函数或变量");
    if (!take('('))
      return fail("函数后需要括号");
    for (int i = 0; i < f->arity; ++i) {
      if (i && !take(','))
        return fail("函数参数需要逗号");
      if (!expr(0))
        return false;
    }
    return (take(')') || fail("参数数量不正确或缺少右括号")) && emit(f->op);
  }
  bool expr(int minimum) {
    if (++depth_ > 48) {
      --depth_;
      return fail("公式嵌套过深");
    }
    if (!atom()) {
      --depth_;
      return false;
    }
    for (;;) {
      spaces();
      int op = -1, precedence = 0, len = 1;
      if (!strncmp(p_, "||", 2)) {
        op = Or;
        precedence = 1;
        len = 2;
      } else if (!strncmp(p_, "&&", 2)) {
        op = And;
        precedence = 2;
        len = 2;
      } else if (!strncmp(p_, "==", 2)) {
        op = Eq;
        precedence = 3;
        len = 2;
      } else if (!strncmp(p_, "!=", 2)) {
        op = Ne;
        precedence = 3;
        len = 2;
      } else if (*p_ == '<' || *p_ == '>') {
        op = *p_ == '<' ? Lt : Gt;
        precedence = 4;
        if (p_[1] == '=') {
          ++op;
          len = 2;
        }
      } else if (*p_ == '+' || *p_ == '-') {
        op = *p_ == '+' ? Add : Sub;
        precedence = 5;
      } else if (*p_ == '*' || *p_ == '/' || *p_ == '%') {
        op = *p_ == '*' ? Mul : *p_ == '/' ? Div : Mod;
        precedence = 6;
      } else if (*p_ == '^') {
        op = Pow;
        precedence = 8;
      }
      if (op < 0 || precedence < minimum)
        break;
      p_ += len;
      if (!expr(op == Pow ? precedence : precedence + 1) || !emit(op)) {
        --depth_;
        return false;
      }
    }
    --depth_;
    return true;
  }

public:
  Parser(Expression &e, const char *s, Text &err, unsigned allowed)
      : out_(e), start_(s), p_(s), error_(err), allowed_(allowed) {}
  bool parse() {
    if (!expr(0))
      return false;
    spaces();
    return !*p_ || fail("多余字符；乘法请显式使用 *");
  }
};
bool Expression::compile(const char *s, Text &error, unsigned allowed) {
  count_ = 0;
  variables_ = 0;
  error.clear();
  if (!s || !*s || strlen(s) > 1024) {
    error.set("公式为空或超过 1024 字节");
    return false;
  }
  if (!Parser(*this, s, error, allowed).parse()) {
    count_ = 0;
    return false;
  }
  return true;
}
double Expression::eval(const Variables &v) const {
  double stack[512];
  int n = 0;
  for (int i = 0; i < count_; ++i) {
    const auto &in = code_[i];
    int op = in.op;
    if (op == Constant || op == Variable) {
      stack[n++] = op == Constant ? in.value : v.get(in.index);
      continue;
    }
    if (op == If) {
      if (n < 3)
        return NAN;
      double no = stack[--n], yes = stack[--n], test = stack[n - 1];
      stack[n - 1] = isfinite(test) ? (test != 0 ? yes : no) : NAN;
      continue;
    }
    if ((op >= Sin && op <= Sign) || op == Neg) {
      if (!n)
        return NAN;
      double a = stack[n - 1], r = NAN;
      switch (op) {
      case Neg:
        r = -a;
        break;
      case Sin:
        r = sin(a);
        break;
      case Cos:
        r = cos(a);
        break;
      case Tan:
        r = tan(a);
        break;
      case Asin:
        r = asin(a);
        break;
      case Acos:
        r = acos(a);
        break;
      case Atan:
        r = atan(a);
        break;
      case Sinh:
        r = sinh(a);
        break;
      case Cosh:
        r = cosh(a);
        break;
      case Tanh:
        r = tanh(a);
        break;
      case Exp:
        r = exp(a);
        break;
      case Log:
        r = log(a);
        break;
      case Log10:
        r = log10(a);
        break;
      case Log2:
        r = log2(a);
        break;
      case Sqrt:
        r = sqrt(a);
        break;
      case Abs:
        r = fabs(a);
        break;
      case Floor:
        r = floor(a);
        break;
      case Ceil:
        r = ceil(a);
        break;
      case Round:
        r = round(a);
        break;
      case Sign:
        r = isfinite(a) ? (a > 0 ? 1 : a < 0 ? -1 : 0) : NAN;
        break;
      }
      stack[n - 1] = r;
      continue;
    }
    if (n < 2)
      return NAN;
    double b = stack[--n], a = stack[n - 1], r = NAN;
    if (isfinite(a) && isfinite(b))
      switch (op) {
      case Add:
        r = a + b;
        break;
      case Sub:
        r = a - b;
        break;
      case Mul:
        r = a * b;
        break;
      case Div:
        r = b ? a / b : NAN;
        break;
      case Mod:
        r = b ? fmod(a, b) : NAN;
        break;
      case Pow:
        r = pow(a, b);
        break;
      case Lt:
        r = a < b;
        break;
      case Le:
        r = a <= b;
        break;
      case Gt:
        r = a > b;
        break;
      case Ge:
        r = a >= b;
        break;
      case Eq:
        r = a == b;
        break;
      case Ne:
        r = a != b;
        break;
      case And:
        r = a != 0 && b != 0;
        break;
      case Or:
        r = a != 0 || b != 0;
        break;
      case Min:
        r = fmin(a, b);
        break;
      case Max:
        r = fmax(a, b);
        break;
      case Atan2:
        r = atan2(a, b);
        break;
      case Hypot:
        r = hypot(a, b);
        break;
      }
    stack[n - 1] = r;
  }
  return n == 1 && isfinite(stack[0]) ? stack[0] : NAN;
}
} // namespace rmp
