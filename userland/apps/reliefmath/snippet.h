#ifndef RELIEFMATH_SNIPPET_H
#define RELIEFMATH_SNIPPET_H

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "storage.h"

namespace rm {

struct Range {
    size_t begin;
    size_t end;
};

enum class Tool : uint16_t {
    Alpha, Beta, Gamma, Delta, Epsilon, Zeta, Eta, Theta, Iota, Kappa,
    Lambda, Mu, Nu, Xi, Omicron, Pi, Rho, Sigma, Tau, Upsilon, Phi, Chi,
    Psi, Omega, EpsilonVariant, ThetaVariant, PiVariant, RhoVariant,
    SigmaVariant, PhiVariant, CapitalGamma, CapitalDelta, CapitalTheta,
    CapitalLambda, CapitalXi, CapitalPi, CapitalSigma, CapitalUpsilon,
    CapitalPhi, CapitalPsi, CapitalOmega,
    Plus, Minus, Equals, PlusMinus, MinusPlus, Times, Dot, Divide, Star,
    Less, Greater, LessEqual, GreaterEqual, NotEqual, Approx, Equivalent,
    Similar, Proportional, MuchLess, MuchGreater, Perpendicular, Parallel,
    To, LeftArrow, RightArrow, LeftRightArrow, RightDoubleArrow,
    LeftDoubleArrow, Mapsto,
    In, NotIn, Subset, SubsetEqual, Union, Intersection, EmptySet, ForAll,
    Exists, Infinity,
    Parentheses, SquareBrackets, CurlyBrackets, AngleBrackets, FloorBrackets,
    CeilingBrackets,
    ThinSpace, MediumSpace, ThickSpace, QuadSpace, QquadSpace, NegativeSpace,
    Fraction, DisplayFraction, Root, IndexedRoot, Superscript, Subscript,
    AutoParentheses, AutoSquareBrackets, AutoCurlyBrackets, AutoAbsolute,
    Sum, Product, Integral, DoubleIntegral, TripleIntegral, ClosedIntegral,
    Limit, Sin, Cos, Tan, Log, Ln, Exp, Min, Max, Matrix,
    ParenthesizedMatrix, BracketedMatrix, Cases, Aligned, Vector, Hat,
    Overline, AbsoluteValue, Norm, Text, Roman, Bold, Italic
};

struct ToolArgs {
    uint16_t rows;
    uint16_t columns;
    bool numbered_root;
    Text selected;
};

struct Snippet {
    Text source;
    Array<Range> stops;
};

/**
 * @brief Builds editable mathematical source and byte-based Tab stops.
 * Parameterless symbols may have no stops; parameterized structures always
 * have nonempty UTF-8 ranges. Each append enforces kMaxSourceBytes before
 * growing the source buffer. Failure clears both destination fields.
 * @param tool A built-in symbol or structure tool.
 * @param args Optional dimensions, root index mode, and selected source text.
 * @param out_snippet Caller-owned destination for generated source and stops.
 * @param error Caller-owned error state updated when generation fails.
 * @return True when valid source and all stop ranges were produced.
 */
bool build_snippet(Tool tool, const ToolArgs &args, Snippet &out_snippet,
                   Error &error);

} // namespace rm

#endif
