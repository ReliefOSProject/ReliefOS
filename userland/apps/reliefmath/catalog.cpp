#include "catalog.h"

#include <stddef.h>
#include <string.h>

namespace rm {
namespace {

#define ENTRY(tool_value, stable_key, english_id, group_name, command_text, \
              insertion_text, parameter_rule, example_text)                  \
    {tool_value, stable_key, english_id, group_name, command_text,            \
     insertion_text, parameter_rule, example_text}

const ToolEntry kEntries[] = {
    ENTRY(Tool::Alpha, "greek.alpha", "Alpha", "Greek letters", "\\alpha", "\\alpha", "none", "\\alpha + x"),
    ENTRY(Tool::Beta, "greek.beta", "Beta", "Greek letters", "\\beta", "\\beta", "none", "\\beta + x"),
    ENTRY(Tool::Gamma, "greek.gamma", "Gamma", "Greek letters", "\\gamma", "\\gamma", "none", "\\gamma + x"),
    ENTRY(Tool::Delta, "greek.delta", "Delta", "Greek letters", "\\delta", "\\delta", "none", "\\delta + x"),
    ENTRY(Tool::Epsilon, "greek.epsilon", "Epsilon", "Greek letters", "\\epsilon", "\\epsilon", "none", "\\epsilon + x"),
    ENTRY(Tool::Zeta, "greek.zeta", "Zeta", "Greek letters", "\\zeta", "\\zeta", "none", "\\zeta + x"),
    ENTRY(Tool::Eta, "greek.eta", "Eta", "Greek letters", "\\eta", "\\eta", "none", "\\eta + x"),
    ENTRY(Tool::Theta, "greek.theta", "Theta", "Greek letters", "\\theta", "\\theta", "none", "\\theta + x"),
    ENTRY(Tool::Iota, "greek.iota", "Iota", "Greek letters", "\\iota", "\\iota", "none", "\\iota + x"),
    ENTRY(Tool::Kappa, "greek.kappa", "Kappa", "Greek letters", "\\kappa", "\\kappa", "none", "\\kappa + x"),
    ENTRY(Tool::Lambda, "greek.lambda", "Lambda", "Greek letters", "\\lambda", "\\lambda", "none", "\\lambda + x"),
    ENTRY(Tool::Mu, "greek.mu", "Mu", "Greek letters", "\\mu", "\\mu", "none", "\\mu + x"),
    ENTRY(Tool::Nu, "greek.nu", "Nu", "Greek letters", "\\nu", "\\nu", "none", "\\nu + x"),
    ENTRY(Tool::Xi, "greek.xi", "Xi", "Greek letters", "\\xi", "\\xi", "none", "\\xi + x"),
    ENTRY(Tool::Omicron, "greek.omicron", "Omicron", "Greek letters", "ο", "ο", "none", "ο + x"),
    ENTRY(Tool::Pi, "greek.pi", "Pi", "Greek letters", "\\pi", "\\pi", "none", "\\pi + x"),
    ENTRY(Tool::Rho, "greek.rho", "Rho", "Greek letters", "\\rho", "\\rho", "none", "\\rho + x"),
    ENTRY(Tool::Sigma, "greek.sigma", "Sigma", "Greek letters", "\\sigma", "\\sigma", "none", "\\sigma + x"),
    ENTRY(Tool::Tau, "greek.tau", "Tau", "Greek letters", "\\tau", "\\tau", "none", "\\tau + x"),
    ENTRY(Tool::Upsilon, "greek.upsilon", "Upsilon", "Greek letters", "\\upsilon", "\\upsilon", "none", "\\upsilon + x"),
    ENTRY(Tool::Phi, "greek.phi", "Phi", "Greek letters", "\\phi", "\\phi", "none", "\\phi + x"),
    ENTRY(Tool::Chi, "greek.chi", "Chi", "Greek letters", "\\chi", "\\chi", "none", "\\chi + x"),
    ENTRY(Tool::Psi, "greek.psi", "Psi", "Greek letters", "\\psi", "\\psi", "none", "\\psi + x"),
    ENTRY(Tool::Omega, "greek.omega", "Omega", "Greek letters", "\\omega", "\\omega", "none", "\\omega + x"),
    ENTRY(Tool::CapitalGamma, "greek.capital-gamma", "Capital gamma", "Greek letters", "\\Gamma", "\\Gamma", "none", "\\Gamma + x"),
    ENTRY(Tool::CapitalDelta, "greek.capital-delta", "Capital delta", "Greek letters", "\\Delta", "\\Delta", "none", "\\Delta + x"),
    ENTRY(Tool::CapitalTheta, "greek.capital-theta", "Capital theta", "Greek letters", "\\Theta", "\\Theta", "none", "\\Theta + x"),
    ENTRY(Tool::CapitalLambda, "greek.capital-lambda", "Capital lambda", "Greek letters", "\\Lambda", "\\Lambda", "none", "\\Lambda + x"),
    ENTRY(Tool::CapitalXi, "greek.capital-xi", "Capital xi", "Greek letters", "\\Xi", "\\Xi", "none", "\\Xi + x"),
    ENTRY(Tool::CapitalPi, "greek.capital-pi", "Capital pi", "Greek letters", "\\Pi", "\\Pi", "none", "\\Pi + x"),
    ENTRY(Tool::CapitalSigma, "greek.capital-sigma", "Capital sigma", "Greek letters", "\\Sigma", "\\Sigma", "none", "\\Sigma + x"),
    ENTRY(Tool::CapitalUpsilon, "greek.capital-upsilon", "Capital upsilon", "Greek letters", "\\Upsilon", "\\Upsilon", "none", "\\Upsilon + x"),
    ENTRY(Tool::CapitalPhi, "greek.capital-phi", "Capital phi", "Greek letters", "\\Phi", "\\Phi", "none", "\\Phi + x"),
    ENTRY(Tool::CapitalPsi, "greek.capital-psi", "Capital psi", "Greek letters", "\\Psi", "\\Psi", "none", "\\Psi + x"),
    ENTRY(Tool::CapitalOmega, "greek.capital-omega", "Capital omega", "Greek letters", "\\Omega", "\\Omega", "none", "\\Omega + x"),
    ENTRY(Tool::EpsilonVariant, "greek.varepsilon", "Variant epsilon", "Greek letters", "\\varepsilon", "\\varepsilon", "none", "\\varepsilon + x"),
    ENTRY(Tool::ThetaVariant, "greek.vartheta", "Variant theta", "Greek letters", "\\vartheta", "\\vartheta", "none", "\\vartheta + x"),
    ENTRY(Tool::PiVariant, "greek.varpi", "Variant pi", "Greek letters", "\\varpi", "\\varpi", "none", "\\varpi + x"),
    ENTRY(Tool::RhoVariant, "greek.varrho", "Variant rho", "Greek letters", "\\varrho", "\\varrho", "none", "\\varrho + x"),
    ENTRY(Tool::SigmaVariant, "greek.varsigma", "Final sigma", "Greek letters", "\\varsigma", "\\varsigma", "none", "\\varsigma + x"),
    ENTRY(Tool::PhiVariant, "greek.varphi", "Variant phi", "Greek letters", "\\varphi", "\\varphi", "none", "\\varphi + x"),

    ENTRY(Tool::Plus, "operator.plus", "Plus", "Binary operators", "+", "+", "none", "a + b"),
    ENTRY(Tool::Minus, "operator.minus", "Minus", "Binary operators", "-", "-", "none", "a - b"),
    ENTRY(Tool::Equals, "relation.equals", "Equals", "Relations", "=", "=", "none", "a = b"),
    ENTRY(Tool::PlusMinus, "operator.plus-minus", "Plus or minus", "Binary operators", "\\pm", "\\pm", "none", "a \\pm b"),
    ENTRY(Tool::MinusPlus, "operator.minus-plus", "Minus or plus", "Binary operators", "\\mp", "\\mp", "none", "a \\mp b"),
    ENTRY(Tool::Times, "operator.times", "Multiplication", "Binary operators", "\\times", "\\times", "none", "a \\times b"),
    ENTRY(Tool::Dot, "operator.dot", "Centered dot", "Binary operators", "\\cdot", "\\cdot", "none", "a \\cdot b"),
    ENTRY(Tool::Divide, "operator.divide", "Division", "Binary operators", "\\div", "\\div", "none", "a \\div b"),
    ENTRY(Tool::Star, "operator.star", "Star product", "Binary operators", "\\star", "\\star", "none", "a \\star b"),

    ENTRY(Tool::Less, "relation.less", "Less than", "Relations", "<", "<", "none", "a < b"),
    ENTRY(Tool::Greater, "relation.greater", "Greater than", "Relations", ">", ">", "none", "a > b"),
    ENTRY(Tool::LessEqual, "relation.less-equal", "Less than or equal", "Relations", "\\le", "\\le", "none", "x \\le y"),
    ENTRY(Tool::GreaterEqual, "relation.greater-equal", "Greater than or equal", "Relations", "\\ge", "\\ge", "none", "x \\ge y"),
    ENTRY(Tool::NotEqual, "relation.not-equal", "Not equal", "Relations", "\\ne", "\\ne", "none", "a \\ne b"),
    ENTRY(Tool::Approx, "relation.approx", "Approximately equal", "Relations", "\\approx", "\\approx", "none", "a \\approx b"),
    ENTRY(Tool::Equivalent, "relation.equivalent", "Equivalent", "Relations", "\\equiv", "\\equiv", "none", "a \\equiv b"),
    ENTRY(Tool::Similar, "relation.similar", "Similar", "Relations", "\\sim", "\\sim", "none", "a \\sim b"),
    ENTRY(Tool::Proportional, "relation.proportional", "Proportional to", "Relations", "\\propto", "\\propto", "none", "a \\propto b"),
    ENTRY(Tool::MuchLess, "relation.much-less", "Much less than", "Relations", "\\ll", "\\ll", "none", "a \\ll b"),
    ENTRY(Tool::MuchGreater, "relation.much-greater", "Much greater than", "Relations", "\\gg", "\\gg", "none", "a \\gg b"),
    ENTRY(Tool::Perpendicular, "relation.perpendicular", "Perpendicular", "Relations", "\\perp", "\\perp", "none", "a \\perp b"),
    ENTRY(Tool::Parallel, "relation.parallel", "Parallel", "Relations", "\\parallel", "\\parallel", "none", "a \\parallel b"),

    ENTRY(Tool::To, "arrow.to", "Maps to the right", "Arrows", "\\to", "\\to", "none", "x \\to y"),
    ENTRY(Tool::LeftArrow, "arrow.left", "Left arrow", "Arrows", "\\leftarrow", "\\leftarrow", "none", "x \\leftarrow y"),
    ENTRY(Tool::RightArrow, "arrow.right", "Right arrow", "Arrows", "\\rightarrow", "\\rightarrow", "none", "a \\rightarrow b"),
    ENTRY(Tool::LeftRightArrow, "arrow.left-right", "Left and right arrow", "Arrows", "\\leftrightarrow", "\\leftrightarrow", "none", "a \\leftrightarrow b"),
    ENTRY(Tool::RightDoubleArrow, "arrow.right-double", "Right double arrow", "Arrows", "\\Rightarrow", "\\Rightarrow", "none", "a \\Rightarrow b"),
    ENTRY(Tool::LeftDoubleArrow, "arrow.left-double", "Left double arrow", "Arrows", "\\Leftarrow", "\\Leftarrow", "none", "a \\Leftarrow b"),
    ENTRY(Tool::Mapsto, "arrow.mapsto", "Maps to", "Arrows", "\\mapsto", "\\mapsto", "none", "x \\mapsto f(x)"),

    ENTRY(Tool::In, "set.member", "Element of", "Sets", "\\in", "\\in", "none", "x \\in A"),
    ENTRY(Tool::NotIn, "set.not-member", "Not an element of", "Sets", "\\notin", "\\notin", "none", "x \\notin A"),
    ENTRY(Tool::Subset, "set.subset", "Subset", "Sets", "\\subset", "\\subset", "none", "A \\subset B"),
    ENTRY(Tool::SubsetEqual, "set.subset-equal", "Subset or equal", "Sets", "\\subseteq", "\\subseteq", "none", "A \\subseteq B"),
    ENTRY(Tool::Union, "set.union", "Union", "Sets", "\\cup", "\\cup", "none", "A \\cup B"),
    ENTRY(Tool::Intersection, "set.intersection", "Intersection", "Sets", "\\cap", "\\cap", "none", "A \\cap B"),
    ENTRY(Tool::EmptySet, "set.empty", "Empty set", "Sets", "\\emptyset", "\\emptyset", "none", "x \\in \\emptyset"),
    ENTRY(Tool::ForAll, "set.forall", "For all", "Sets", "\\forall", "\\forall", "none", "\\forall x \\in A"),
    ENTRY(Tool::Exists, "set.exists", "There exists", "Sets", "\\exists", "\\exists", "none", "\\exists x \\in A"),
    ENTRY(Tool::Infinity, "symbol.infinity", "Infinity", "Sets", "\\infty", "\\infty", "none", "x \\to \\infty"),

    ENTRY(Tool::Parentheses, "bracket.parentheses", "Parentheses", "Brackets", "\\left( ... \\right)", "\\left( x \\right)", "content: math source", "\\left( x + 1 \\right)"),
    ENTRY(Tool::SquareBrackets, "bracket.square", "Square brackets", "Brackets", "\\left[ ... \\right]", "\\left[ x \\right]", "content: math source", "\\left[ x + 1 \\right]"),
    ENTRY(Tool::CurlyBrackets, "bracket.curly", "Curly brackets", "Brackets", "\\left\\{ ... \\right\\}", "\\left\\{ x \\right\\}", "content: math source", "\\left\\{ x + 1 \\right\\}"),
    ENTRY(Tool::AngleBrackets, "bracket.angle", "Angle brackets", "Brackets", "\\left\\langle ... \\right\\rangle", "\\left\\langle x \\right\\rangle", "content: math source", "\\left\\langle x, y \\right\\rangle"),
    ENTRY(Tool::FloorBrackets, "bracket.floor", "Floor brackets", "Brackets", "\\left\\lfloor ... \\right\\rfloor", "\\left\\lfloor x \\right\\rfloor", "content: math source", "\\left\\lfloor x \\right\\rfloor"),
    ENTRY(Tool::CeilingBrackets, "bracket.ceiling", "Ceiling brackets", "Brackets", "\\left\\lceil ... \\right\\rceil", "\\left\\lceil x \\right\\rceil", "content: math source", "\\left\\lceil x \\right\\rceil"),

    ENTRY(Tool::ThinSpace, "spacing.thin", "Thin space", "Spacing", "\\,", "\\,", "none", "a\\,b"),
    ENTRY(Tool::MediumSpace, "spacing.medium", "Medium space", "Spacing", "\\:", "\\:", "none", "a\\:b"),
    ENTRY(Tool::ThickSpace, "spacing.thick", "Thick space", "Spacing", "\\;", "\\;", "none", "a\\;b"),
    ENTRY(Tool::QuadSpace, "spacing.quad", "Quad space", "Spacing", "\\quad", "\\quad", "none", "a\\quad b"),
    ENTRY(Tool::QquadSpace, "spacing.double-quad", "Double quad space", "Spacing", "\\qquad", "\\qquad", "none", "a\\qquad b"),
    ENTRY(Tool::NegativeSpace, "spacing.negative", "Negative thin space", "Spacing", "\\!", "\\!", "none", "a\\!b"),

    ENTRY(Tool::Fraction, "structure.fraction", "Fraction", "Fractions", "\\frac", "\\frac{a}{b}", "numerator and denominator: math source", "\\frac{a}{b}"),
    ENTRY(Tool::DisplayFraction, "structure.display-fraction", "Display fraction", "Fractions", "\\displaystyle\\frac", "\\displaystyle\\frac{a}{b}", "numerator and denominator: display-style math", "\\displaystyle\\frac{a}{b}"),
    ENTRY(Tool::Root, "structure.root", "Square root", "Roots", "\\sqrt", "\\sqrt{x}", "radicand: math source", "\\sqrt{x}"),
    ENTRY(Tool::IndexedRoot, "structure.indexed-root", "Indexed root", "Roots", "\\sqrt[n]", "\\sqrt[n]{x}", "index and radicand: math source", "\\sqrt[3]{x}"),
    ENTRY(Tool::Superscript, "structure.superscript", "Superscript", "Scripts", "^", "a^{b}", "base and exponent: math source", "x^{2}"),
    ENTRY(Tool::Subscript, "structure.subscript", "Subscript", "Scripts", "_", "a_{i}", "base and subscript: math source", "x_{i}"),
    ENTRY(Tool::AutoParentheses, "structure.auto-parentheses", "Automatic parentheses", "Delimiters", "\\left( ... \\right)", "\\left( x \\right)", "content: math source", "\\left( x + 1 \\right)"),
    ENTRY(Tool::AutoSquareBrackets, "structure.auto-square-brackets", "Automatic square brackets", "Delimiters", "\\left[ ... \\right]", "\\left[ x \\right]", "content: math source", "\\left[ x + 1 \\right]"),
    ENTRY(Tool::AutoCurlyBrackets, "structure.auto-curly-brackets", "Automatic curly brackets", "Delimiters", "\\left\\{ ... \\right\\}", "\\left\\{ x \\right\\}", "content: math source", "\\left\\{ x + 1 \\right\\}"),
    ENTRY(Tool::AutoAbsolute, "structure.auto-absolute", "Automatic absolute value", "Delimiters", "\\left| ... \\right|", "\\left| x \\right|", "content: math source", "\\left| x + 1 \\right|"),
    ENTRY(Tool::Sum, "structure.sum", "Summation", "Big operators", "\\sum", "\\sum_{i=1}^{n} x", "lower bound, upper bound, and term: math source", "\\sum_{i=1}^{n} i"),
    ENTRY(Tool::Product, "structure.product", "Product", "Big operators", "\\prod", "\\prod_{i=1}^{n} x", "lower bound, upper bound, and term: math source", "\\prod_{i=1}^{n} i"),
    ENTRY(Tool::Integral, "structure.integral", "Integral", "Big operators", "\\int", "\\int_{a}^{b} f(x) \\, dx", "bounds and integrand: math source", "\\int_{0}^{1} x \\, dx"),
    ENTRY(Tool::DoubleIntegral, "structure.double-integral", "Double integral", "Big operators", "\\iint", "\\iint_{a}^{b} f(x,y) \\, dx \\, dy", "bounds and integrand: math source", "\\iint_D f(x,y) \\, dx \\, dy"),
    ENTRY(Tool::TripleIntegral, "structure.triple-integral", "Triple integral", "Big operators", "\\iiint", "\\iiint_{a}^{b} f(x,y,z) \\, dx \\, dy \\, dz", "bounds and integrand: math source", "\\iiint_V f(x,y,z) \\, dx \\, dy \\, dz"),
    ENTRY(Tool::ClosedIntegral, "structure.closed-integral", "Closed integral", "Big operators", "\\oint", "\\oint_C f(x) \\, dx", "path and integrand: math source", "\\oint_C f"),
    ENTRY(Tool::Limit, "structure.limit", "Limit", "Big operators", "\\lim", "\\lim_{x \\to 0} f(x)", "variable, target, and expression: math source", "\\lim_{x\\to 0} f(x)"),
    ENTRY(Tool::Sin, "function.sin", "Sine", "Functions", "\\sin", "\\sin{x}", "argument: math source", "\\sin x"),
    ENTRY(Tool::Cos, "function.cos", "Cosine", "Functions", "\\cos", "\\cos{x}", "argument: math source", "\\cos x"),
    ENTRY(Tool::Tan, "function.tan", "Tangent", "Functions", "\\tan", "\\tan{x}", "argument: math source", "\\tan x"),
    ENTRY(Tool::Log, "function.log", "Logarithm", "Functions", "\\log", "\\log{x}", "argument: math source", "\\log x"),
    ENTRY(Tool::Ln, "function.ln", "Natural logarithm", "Functions", "\\ln", "\\ln{x}", "argument: math source", "\\ln x"),
    ENTRY(Tool::Exp, "function.exp", "Exponential", "Functions", "\\exp", "\\exp{x}", "argument: math source", "\\exp x"),
    ENTRY(Tool::Min, "function.min", "Minimum", "Functions", "\\min", "\\min{x}", "argument: math source", "\\min(x,y)"),
    ENTRY(Tool::Max, "function.max", "Maximum", "Functions", "\\max", "\\max{x}", "argument: math source", "\\max(x,y)"),
    ENTRY(Tool::Matrix, "structure.matrix", "Matrix", "Matrices", "matrix", "\\begin{matrix}a & b \\\\ c & d\\end{matrix}", "rows and columns: 1..16", "\\begin{matrix}a & b \\\\ c & d\\end{matrix}"),
    ENTRY(Tool::ParenthesizedMatrix, "structure.parenthesized-matrix", "Parenthesized matrix", "Matrices", "pmatrix", "\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}", "rows and columns: 1..16", "\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}"),
    ENTRY(Tool::BracketedMatrix, "structure.bracketed-matrix", "Bracketed matrix", "Matrices", "bmatrix", "\\begin{bmatrix}a & b \\\\ c & d\\end{bmatrix}", "rows and columns: 1..16", "\\begin{bmatrix}a & b \\\\ c & d\\end{bmatrix}"),
    ENTRY(Tool::Cases, "structure.cases", "Piecewise cases", "Matrices", "cases", "\\begin{cases}a & b \\\\ c & d\\end{cases}", "rows and columns: 1..16", "\\begin{cases}a & b \\\\ c & d\\end{cases}"),
    ENTRY(Tool::Aligned, "structure.aligned", "Aligned equations", "Matrices", "aligned", "\\begin{aligned}a &= b \\\\ c &= d\\end{aligned}", "rows and columns: 1..16", "\\begin{aligned}a &= b \\\\ c &= d\\end{aligned}"),
    ENTRY(Tool::Vector, "structure.vector", "Vector accent", "Accents", "\\vec", "\\vec{x}", "base: math source", "\\vec{x}"),
    ENTRY(Tool::Hat, "structure.hat", "Hat accent", "Accents", "\\hat", "\\hat{x}", "base: math source", "\\hat{x}"),
    ENTRY(Tool::Overline, "structure.overline", "Overline accent", "Accents", "\\overline", "\\overline{x}", "base: math source", "\\overline{x}"),
    ENTRY(Tool::AbsoluteValue, "structure.absolute-value", "Absolute value", "Delimiters", "|...|", "\\left| x \\right|", "content: math source", "\\left|x\\right|"),
    ENTRY(Tool::Norm, "structure.norm", "Norm", "Delimiters", "\\|...\\|", "\\left\\| x \\right\\|", "content: math source", "\\left\\|x\\right\\|"),
    ENTRY(Tool::Text, "structure.text", "Text in math", "Text and fonts", "\\text", "\\text{a}", "text content; selected UTF-8 text is preserved", "\\text{中文}"),
    ENTRY(Tool::Roman, "structure.roman", "Roman math font", "Text and fonts", "\\mathrm", "\\mathrm{a}", "content: math source", "\\mathrm{d}x"),
    ENTRY(Tool::Bold, "structure.bold", "Bold math font", "Text and fonts", "\\mathbf", "\\mathbf{a}", "content: math source", "\\mathbf{x}"),
    ENTRY(Tool::Italic, "structure.italic", "Italic math font", "Text and fonts", "\\mathit", "\\mathit{a}", "content: math source", "\\mathit{x}")
};

#undef ENTRY

char lower_ascii(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

bool contains_ascii_case_insensitive(const char *text, const char *query) {
    if (query == 0 || query[0] == '\0') {
        return true;
    }
    if (text == 0) {
        return false;
    }
    const size_t query_length = strlen(query);
    for (size_t start = 0; text[start] != '\0'; ++start) {
        size_t offset = 0;
        while (offset < query_length && text[start + offset] != '\0' &&
               lower_ascii(text[start + offset]) == lower_ascii(query[offset])) {
            ++offset;
        }
        if (offset == query_length) {
            return true;
        }
    }
    return false;
}

bool starts_with(const char *text, const char *prefix) {
    if (text == 0 || prefix == 0) {
        return false;
    }
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

} // namespace

const ToolEntry *catalog_entries(size_t *out_count) {
    if (out_count != 0) {
        *out_count = sizeof(kEntries) / sizeof(kEntries[0]);
    }
    return kEntries;
}

const ToolEntry *catalog_find(const char *key) {
    if (key == 0) {
        return 0;
    }
    const size_t count = sizeof(kEntries) / sizeof(kEntries[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(kEntries[i].key, key) == 0) {
            return &kEntries[i];
        }
    }
    return 0;
}

size_t catalog_search(const char *query, const ToolEntry **out_matches,
                      size_t capacity) {
    if (query == 0 || out_matches == 0 || capacity == 0) {
        return 0;
    }
    const size_t count = sizeof(kEntries) / sizeof(kEntries[0]);
    size_t written = 0;
    for (size_t i = 0; i < count && written < capacity; ++i) {
        const ToolEntry &entry = kEntries[i];
        if (contains_ascii_case_insensitive(entry.key, query) ||
            contains_ascii_case_insensitive(entry.msgid, query) ||
            contains_ascii_case_insensitive(entry.category, query) ||
            contains_ascii_case_insensitive(entry.command, query) ||
            contains_ascii_case_insensitive(entry.insert, query) ||
            contains_ascii_case_insensitive(entry.parameters, query) ||
            contains_ascii_case_insensitive(entry.help_example, query)) {
            if (out_matches != 0) {
                out_matches[written] = &entry;
            }
            ++written;
        }
    }
    return written;
}

size_t complete_command(const char *prefix, const ToolEntry **out_matches,
                        size_t capacity) {
    if (prefix == 0 || prefix[0] != '\\' || out_matches == 0 || capacity == 0) {
        return 0;
    }
    const size_t count = sizeof(kEntries) / sizeof(kEntries[0]);
    size_t written = 0;
    for (size_t i = 0; i < count && written < capacity; ++i) {
        if (starts_with(kEntries[i].command, prefix)) {
            if (out_matches != 0) {
                out_matches[written] = &kEntries[i];
            }
            ++written;
        }
    }
    return written;
}

} // namespace rm
