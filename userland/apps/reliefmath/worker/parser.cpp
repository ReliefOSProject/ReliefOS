#include "parser.h"

#include "../limits.h"
#include "../storage.h"

#include <math.h>
#include <string.h>

namespace rm {
namespace {

struct Symbol {
    const char *name;
    uint32_t codepoint;
    uint8_t atom_class;
};

const Symbol kSymbols[] = {
    {"alpha",0x03b1,0},{"beta",0x03b2,0},{"gamma",0x03b3,0},
    {"delta",0x03b4,0},{"epsilon",0x03b5,0},{"zeta",0x03b6,0},
    {"eta",0x03b7,0},{"theta",0x03b8,0},{"iota",0x03b9,0},
    {"kappa",0x03ba,0},{"lambda",0x03bb,0},{"mu",0x03bc,0},
    {"nu",0x03bd,0},{"xi",0x03be,0},{"omicron",0x03bf,0},
    {"pi",0x03c0,0},{"rho",0x03c1,0},{"sigma",0x03c3,0},
    {"tau",0x03c4,0},{"upsilon",0x03c5,0},{"phi",0x03c6,0},
    {"chi",0x03c7,0},{"psi",0x03c8,0},{"omega",0x03c9,0},
    {"varepsilon",0x03f5,0},{"vartheta",0x03d1,0},
    {"varpi",0x03d6,0},{"varrho",0x03f1,0},{"varsigma",0x03c2,0},
    {"varphi",0x03d5,0},
    {"Gamma",0x0393,0},{"Delta",0x0394,0},{"Theta",0x0398,0},
    {"Lambda",0x039b,0},{"Xi",0x039e,0},{"Pi",0x03a0,0},
    {"Sigma",0x03a3,0},{"Upsilon",0x03a5,0},{"Phi",0x03a6,0},
    {"Psi",0x03a8,0},{"Omega",0x03a9,0},
    {"pm",0x00b1,3},{"mp",0x2213,3},{"times",0x00d7,3},
    {"cdot",0x22c5,3},{"div",0x00f7,3},{"star",0x22c6,3},
    {"le",0x2264,4},{"leq",0x2264,4},{"ge",0x2265,4},
    {"geq",0x2265,4},{"ne",0x2260,4},{"neq",0x2260,4},
    {"approx",0x2248,4},{"equiv",0x2261,4},{"sim",0x223c,4},
    {"propto",0x221d,4},{"ll",0x226a,4},{"gg",0x226b,4},
    {"perp",0x27c2,4},{"parallel",0x2225,4},
    {"to",0x2192,4},{"rightarrow",0x2192,4},{"leftarrow",0x2190,4},
    {"leftrightarrow",0x2194,4},{"Rightarrow",0x21d2,4},
    {"Leftarrow",0x21d0,4},{"mapsto",0x21a6,4},
    {"in",0x2208,4},{"notin",0x2209,4},{"subset",0x2282,4},
    {"subseteq",0x2286,4},{"cup",0x222a,3},{"cap",0x2229,3},
    {"emptyset",0x2205,0},{"varnothing",0x2205,0},
    {"forall",0x2200,0},{"exists",0x2203,0},{"infty",0x221e,0},
    {"partial",0x2202,0},{"nabla",0x2207,0},{"ldots",0x2026,7},
    {"cdots",0x22ef,7},{"vdots",0x22ee,7},{"ddots",0x22f1,7},
    {"prime",0x2032,0},{"langle",0x27e8,5},{"rangle",0x27e9,6},
    {"lfloor",0x230a,5},{"rfloor",0x230b,6},
    {"lceil",0x2308,5},{"rceil",0x2309,6},
    {"Vert",0x2016,0},{"|",0x2016,0},
};

const char *const kFunctions[] = {
    "sin", "cos", "tan", "log", "ln", "exp", "min", "max"
};

bool ascii_letter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool slice_equals(const char *source, size_t length, const char *word) {
    const size_t word_length = strlen(word);
    return length == word_length && memcmp(source, word, length) == 0;
}

bool pair_delimiters(uint32_t left, uint32_t right) {
    if (!left || !right) return true;
    switch (left) {
    case '(': return right == ')';
    case '[': return right == ']';
    case '{': return right == '}';
    case '<': return right == '>';
    case 0x27e8: return right == 0x27e9;
    case 0x230a: return right == 0x230b;
    case 0x2308: return right == 0x2309;
    case '|': return right == '|';
    case 0x2016: return right == 0x2016;
    default: return left == right;
    }
}

} // namespace

LatexParser::LatexParser(const char *source, size_t length, double size, bool display,
                         FontBook &fonts, Error &error)
    : source_(source), length_(length), size_(size), display_(display),
      fonts_(fonts), error_(error) {}

bool LatexParser::fail(Code code, const char *message, size_t begin, size_t end) {
    if (begin > length_) begin = length_;
    if (end > length_) end = length_;
    if (end < begin) end = begin;
    error_.set(code, message, begin, end);
    return false;
}

bool LatexParser::fail_here(Code code, const char *message) {
    const size_t end = position_ < length_ ? position_ + 1 : position_;
    return fail(code, message, position_, end);
}

bool LatexParser::layout_result(bool okay, const Command &command) {
    if (okay) return true;
    if (error_.code == Code::None)
        error_.set(Code::NoMemory, "Cannot construct formula layout");
    error_.begin = command.begin;
    error_.end = command.end;
    return false;
}

bool LatexParser::command_is(const Command &command, const char *name) const {
    return command.word &&
           slice_equals(source_ + command.name_begin, command.name_length, name);
}

bool LatexParser::read_command(Command &command) {
    if (position_ >= length_ || source_[position_] != '\\')
        return fail_here(Code::Parse, "Expected a control sequence");
    command.begin = position_++;
    command.name_begin = position_;
    command.word = false;
    if (position_ >= length_)
        return fail(Code::Parse, "Incomplete control sequence", command.begin, position_);
    if (ascii_letter(source_[position_])) {
        command.word = true;
        while (position_ < length_ && ascii_letter(source_[position_])) ++position_;
    } else {
        size_t bytes = 0;
        uint32_t ignored = 0;
        if (!decode_codepoint(position_, ignored, bytes)) return false;
        position_ += bytes;
    }
    command.name_length = position_ - command.name_begin;
    command.end = position_;
    return true;
}

bool LatexParser::at_control(const char *name, size_t name_length, size_t *end) const {
    if (position_ >= length_ || source_[position_] != '\\') return false;
    size_t cursor = position_ + 1;
    if (cursor >= length_ || !ascii_letter(source_[cursor])) return false;
    const size_t start = cursor;
    while (cursor < length_ && ascii_letter(source_[cursor])) ++cursor;
    if (cursor - start != name_length ||
        memcmp(source_ + start, name, name_length) != 0) return false;
    if (cursor < length_ && ascii_letter(source_[cursor])) return false;
    if (end) *end = cursor;
    return true;
}

bool LatexParser::at_environment_end(const char *environment, size_t environment_length,
                                    size_t *end) const {
    size_t cursor = 0;
    if (!at_control("end", 3, &cursor) || cursor >= length_ ||
        source_[cursor] != '{') return false;
    ++cursor;
    if (environment_length > length_ - cursor ||
        memcmp(source_ + cursor, environment, environment_length) != 0) return false;
    cursor += environment_length;
    if (cursor >= length_ || source_[cursor] != '}') return false;
    if (end) *end = cursor + 1;
    return true;
}

bool LatexParser::at_row_break(size_t *end) const {
    if (position_ + 1 >= length_ || source_[position_] != '\\' ||
        source_[position_ + 1] != '\\') return false;
    if (end) *end = position_ + 2;
    return true;
}

bool LatexParser::skip_ignored() {
    for (;;) {
        while (position_ < length_) {
            const char value = source_[position_];
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n')
                ++position_;
            else
                break;
        }
        if (position_ >= length_ || source_[position_] != '%') return true;
        while (position_ < length_ && source_[position_] != '\n') ++position_;
    }
}

bool LatexParser::decode_codepoint(size_t at, uint32_t &codepoint,
                                   size_t &byte_count) const {
    if (at >= length_) return false;
    const uint8_t first = static_cast<uint8_t>(source_[at]);
    if (first <= 0x7f) {
        codepoint = first;
        byte_count = 1;
    } else if (first <= 0xdf) {
        codepoint = ((first & 0x1f) << 6) |
                    (static_cast<uint8_t>(source_[at + 1]) & 0x3f);
        byte_count = 2;
    } else if (first <= 0xef) {
        codepoint = ((first & 0x0f) << 12) |
                    ((static_cast<uint8_t>(source_[at + 1]) & 0x3f) << 6) |
                    (static_cast<uint8_t>(source_[at + 2]) & 0x3f);
        byte_count = 3;
    } else {
        codepoint = ((first & 0x07) << 18) |
                    ((static_cast<uint8_t>(source_[at + 1]) & 0x3f) << 12) |
                    ((static_cast<uint8_t>(source_[at + 2]) & 0x3f) << 6) |
                    (static_cast<uint8_t>(source_[at + 3]) & 0x3f);
        byte_count = 4;
    }
    return true;
}

bool LatexParser::enter(size_t at) {
    if (depth_ >= 128)
        return fail(Code::Limit, "Formula nesting exceeds 128 levels", at, at + 1);
    ++depth_;
    return true;
}

void LatexParser::leave() {
    if (depth_) --depth_;
}

double LatexParser::inter_atom_space(AtomClass left, AtomClass right, double size) const {
    if (left == AtomClass::Open || right == AtomClass::Close ||
        right == AtomClass::Punctuation) return 0;
    if (left == AtomClass::Relation || right == AtomClass::Relation)
        return size * 0.24;
    if (left == AtomClass::Binary || right == AtomClass::Binary)
        return size * 0.19;
    if (left == AtomClass::Operator || right == AtomClass::Operator ||
        left == AtomClass::LargeOperator || right == AtomClass::LargeOperator)
        return size * 0.13;
    if (left == AtomClass::Punctuation) return size * 0.12;
    return 0;
}

bool LatexParser::make_glyph(Box &out, uint32_t codepoint, Face face,
                             size_t begin, size_t end) {
    if (!box_make_glyph(fonts_, codepoint, size_, face, out, error_)) {
        if (error_.code == Code::Font)
            error_.set(Code::Font, "A required formula glyph is unavailable", begin, end);
        return false;
    }
    return true;
}

bool LatexParser::make_word(Box &out, const char *word, Face face, double size,
                            AtomClass, size_t begin, size_t end) {
    Box candidate;
    double cursor = 0;
    for (size_t i = 0; word[i]; ++i) {
        Box letter;
        if (!box_make_glyph(fonts_, static_cast<uint8_t>(word[i]), size, face,
                            letter, error_)) {
            if (error_.code == Code::Font)
                error_.set(Code::Font, "A required function glyph is unavailable", begin, end);
            return false;
        }
        if (!box_place(candidate, letter, cursor, 0, error_)) return false;
        cursor += letter.width;
        if (!isfinite(cursor))
            return fail(error_.code == Code::None ? Code::Limit : error_.code,
                        "Function width overflow", begin, end);
    }
    candidate.width = cursor;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool LatexParser::parse(Box &out) {
    Box candidate;
    if (!parse_source(candidate)) return false;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool LatexParser::parse_source(Box &out) {
    Box candidate;
    if (!parse_expression(candidate, display_, Face::Math, Stop::End, nullptr, 0))
        return false;
    if (position_ != length_)
        return fail_here(Code::Parse, "Unexpected input after formula");
    out = static_cast<Box &&>(candidate);
    return true;
}

bool LatexParser::parse_expression(Box &out, bool display, Face face, Stop stop,
                                   const char *environment, size_t environment_length) {
    Box candidate;
    double cursor = 0;
    AtomClass previous = AtomClass::Ordinary;
    bool have_previous = false;
    for (;;) {
        if (!skip_ignored()) return false;
        if (position_ == length_) {
            if (stop == Stop::End) {
                out = static_cast<Box &&>(candidate);
                return true;
            }
            if (stop == Stop::Group || stop == Stop::Bracket ||
                stop == Stop::GridCell || stop == Stop::Right) {
                out = static_cast<Box &&>(candidate);
                return true;
            }
            return fail(Code::Parse, "Unclosed group or layout construct",
                        position_, position_);
        }
        if (stop == Stop::Group && source_[position_] == '}') {
            out = static_cast<Box &&>(candidate);
            return true;
        }
        if (stop == Stop::Bracket && source_[position_] == ']') {
            out = static_cast<Box &&>(candidate);
            return true;
        }
        if (stop == Stop::GridCell &&
            (source_[position_] == '&' || at_row_break(nullptr) ||
             at_environment_end(environment, environment_length, nullptr))) {
            out = static_cast<Box &&>(candidate);
            return true;
        }
        if (stop == Stop::Right && at_control("right", 5, nullptr)) {
            out = static_cast<Box &&>(candidate);
            return true;
        }
        const size_t token_begin = position_;
        Box atom;
        AtomClass atom_class = AtomClass::Ordinary;
        bool is_spacing = false;
        double explicit_spacing = 0;
        bool atom_display = display;
        if (!parse_atom(atom, atom_class, is_spacing, explicit_spacing, display,
                        face, atom_display)) return false;
        if (is_spacing) {
            cursor += explicit_spacing;
            if (!isfinite(cursor))
                return fail(Code::Limit, "Spacing overflow", token_begin, position_);
            if (cursor > candidate.width) candidate.width = cursor;
            continue;
        }

        bool has_subscript = false;
        bool has_superscript = false;
        Box subscript;
        Box superscript;
        for (;;) {
            if (!skip_ignored()) return false;
            if (position_ >= length_ ||
                (source_[position_] != '_' && source_[position_] != '^')) break;
            const size_t marker = position_;
            const char marker_character = source_[position_++];
            bool &present = marker_character == '_' ? has_subscript : has_superscript;
            if (present)
                return fail(Code::Parse, "Duplicate script on one math atom",
                            marker, marker + 1);
            const double saved_size = size_;
            size_ *= 0.70;
            Box argument;
            const bool parsed = parse_script_argument(argument, atom_display, face);
            size_ = saved_size;
            if (!parsed) return false;
            if (marker_character == '_')
                subscript = static_cast<Box &&>(argument);
            else
                superscript = static_cast<Box &&>(argument);
            present = true;
        }
        if (has_subscript || has_superscript) {
            Box scripted;
            const bool vertical_limits = atom_class == AtomClass::LargeOperator &&
                                         atom_display;
            if (!box_scripts(atom, has_subscript ? &subscript : nullptr,
                             has_superscript ? &superscript : nullptr,
                             size_, vertical_limits, scripted, error_)) return false;
            atom = static_cast<Box &&>(scripted);
        }

        if (!have_previous && (atom_class == AtomClass::Binary ||
                               atom_class == AtomClass::Relation))
            atom_class = AtomClass::Ordinary;
        const double spacing = have_previous
                                   ? inter_atom_space(previous, atom_class, size_) : 0;
        cursor += spacing;
        if (!isfinite(cursor) || !box_place(candidate, atom, cursor, 0, error_))
            return error_.code == Code::None
                       ? fail(Code::Limit, "Formula width overflow", token_begin, position_)
                       : false;
        cursor += atom.width;
        if (!isfinite(cursor))
            return fail(Code::Limit, "Formula width overflow", token_begin, position_);
        if (cursor > candidate.width) candidate.width = cursor;
        previous = atom_class;
        have_previous = true;
    }
}

bool LatexParser::parse_group(Box &out, bool display, Face face) {
    if (position_ >= length_ || source_[position_] != '{')
        return fail_here(Code::Parse, "Expected a braced argument");
    const size_t opening = position_;
    if (!enter(opening)) return false;
    ++position_;
    Box candidate;
    const bool parsed = parse_expression(candidate, display, face, Stop::Group, nullptr, 0);
    if (!parsed) {
        leave();
        return false;
    }
    if (position_ >= length_ || source_[position_] != '}') {
        leave();
        return fail(Code::Parse, "Unclosed group", opening, opening + 1);
    }
    ++position_;
    leave();
    out = static_cast<Box &&>(candidate);
    return true;
}

bool LatexParser::parse_script_argument(Box &out, bool display, Face face) {
    if (position_ >= length_)
        return fail(Code::Parse, "Missing script argument", position_, position_);
    if (source_[position_] == '{') return parse_group(out, display, face);
    const size_t argument_begin = position_;
    if (!enter(argument_begin)) return false;
    AtomClass ignored_class = AtomClass::Ordinary;
    bool spacing = false;
    double spacing_width = 0;
    bool ignored_display = display;
    const bool parsed = parse_atom(out, ignored_class, spacing, spacing_width,
                                   display, face, ignored_display);
    leave();
    if (!parsed) return false;
    if (spacing) return fail_here(Code::Parse, "A script cannot be a spacing command");
    return true;
}

bool LatexParser::parse_atom(Box &out, AtomClass &atom_class, bool &is_spacing,
                             double &spacing, bool display, Face face,
                             bool &effective_display) {
    is_spacing = false;
    spacing = 0;
    effective_display = display;
    if (position_ >= length_)
        return fail(Code::Parse, "Expected a math atom", position_, position_);

    const size_t begin = position_;
    const char value = source_[position_];
    if (value == '{') {
        if (!parse_group(out, display, face)) return false;
        atom_class = AtomClass::Ordinary;
        return true;
    }
    if (value == '}')
        return fail_here(Code::Parse, "Unexpected closing brace");
    if (value == '&')
        return fail_here(Code::Parse, "Alignment marker outside an alignment environment");
    if (value == '_' || value == '^')
        return fail_here(Code::Parse, "A script marker has no base atom");
    if (value == '[') {
        ++position_;
        if (!make_glyph(out, '[', face, begin, position_)) return false;
        atom_class = AtomClass::Open;
        return true;
    }
    if (value == ']') {
        ++position_;
        if (!make_glyph(out, ']', face, begin, position_)) return false;
        atom_class = AtomClass::Close;
        return true;
    }
    if (value == '\\') {
        Command command{};
        if (!read_command(command)) return false;
        if (command_is(command, "displaystyle") || command_is(command, "textstyle")) {
            if (!enter(command.begin)) return false;
            const bool next_display = command_is(command, "displaystyle");
            const bool result = parse_atom(out, atom_class, is_spacing, spacing,
                                           next_display, face, effective_display);
            leave();
            return result;
        }
        return parse_command(out, atom_class, is_spacing, spacing, display, face, command);
    }

    uint32_t codepoint = 0;
    size_t bytes = 0;
    if (!decode_codepoint(position_, codepoint, bytes))
        return fail_here(Code::InvalidUtf8, "Invalid UTF-8 in formula");
    position_ += bytes;
    if (codepoint == '~') {
        is_spacing = true;
        spacing = size_ * 0.333;
        return true;
    }
    if (codepoint == '%' || codepoint == '#' || codepoint == '$')
        return fail(Code::Unsupported, "Reserved TeX character must be escaped",
                    begin, position_);
    if (codepoint == '+' || codepoint == '-') atom_class = AtomClass::Binary;
    else if (codepoint == '=' || codepoint == '<' || codepoint == '>')
        atom_class = AtomClass::Relation;
    else if (codepoint == '(') atom_class = AtomClass::Open;
    else if (codepoint == ')') atom_class = AtomClass::Close;
    else if (codepoint == ',' || codepoint == ';') atom_class = AtomClass::Punctuation;
    else atom_class = AtomClass::Ordinary;
    return make_glyph(out, codepoint, face, begin, position_);
}

bool LatexParser::parse_command(Box &out, AtomClass &atom_class, bool &is_spacing,
                                double &spacing, bool display, Face face,
                                const Command &command) {
    is_spacing = false;
    spacing = 0;
    if (!command.word) {
        const char symbol = source_[command.name_begin];
        uint32_t codepoint = static_cast<uint8_t>(symbol);
        if (symbol == ',' || symbol == ':' || symbol == ';' || symbol == '!' ||
            symbol == ' ') {
            is_spacing = true;
            if (symbol == ',') spacing = size_ * 0.166;
            else if (symbol == ':') spacing = size_ * 0.222;
            else if (symbol == ';') spacing = size_ * 0.277;
            else if (symbol == '!') spacing = -size_ * 0.166;
            else spacing = size_ * 0.333;
            return true;
        }
        if (symbol == '\\') return fail(Code::Parse, "Row break outside a matrix or alignment",
                                        command.begin, command.end);
        if (symbol == '%') codepoint = '%';
        else if (symbol == '#') codepoint = '#';
        else if (symbol == '$') codepoint = '$';
        else if (symbol == '&') codepoint = '&';
        else if (symbol == '_') codepoint = '_';
        else if (symbol == '{') codepoint = '{';
        else if (symbol == '}') codepoint = '}';
        else if (symbol == '|') codepoint = 0x2016;
        else if (symbol == '\\') codepoint = '\\';
        else return fail(Code::Unsupported, "Unsupported control symbol",
                         command.begin, command.end);

        if (symbol == '{') atom_class = AtomClass::Open;
        else if (symbol == '}') atom_class = AtomClass::Close;
        else if (symbol == '&') atom_class = AtomClass::Relation;
        else atom_class = AtomClass::Ordinary;
        return make_glyph(out, codepoint, face, command.begin, command.end);
    }

    if (command_is(command, "frac") || command_is(command, "dfrac") ||
        command_is(command, "tfrac")) {
        const bool display_fraction = command_is(command, "dfrac") ||
                                      (command_is(command, "frac") && display);
        const double fraction_size = command_is(command, "tfrac")
                                         ? size_ * 0.76 : size_;
        const double child_size = fraction_size * (display_fraction ? 0.72 : 0.60);
        const double saved_size = size_;
        size_ = child_size;
        Box numerator;
        bool parsed = parse_group(numerator, false, face);
        Box denominator;
        if (parsed) parsed = parse_group(denominator, false, face);
        size_ = saved_size;
        if (!parsed) return false;
        if (!layout_result(box_fraction(numerator, denominator, fraction_size,
                                         out, error_), command))
            return false;
        atom_class = AtomClass::Inner;
        return true;
    }

    if (command_is(command, "sqrt")) {
        Box index;
        bool has_index = false;
        if (position_ < length_ && source_[position_] == '[') {
            const size_t opening = position_;
            if (!enter(opening)) return false;
            ++position_;
            const double saved_size = size_;
            size_ *= 0.48;
            const bool parsed = parse_expression(index, false, face, Stop::Bracket,
                                                 nullptr, 0);
            size_ = saved_size;
            if (!parsed) {
                leave();
                return false;
            }
            if (position_ >= length_ || source_[position_] != ']') {
                leave();
                return fail(Code::Parse, "Unclosed root index", opening, opening + 1);
            }
            ++position_;
            leave();
            has_index = true;
        }
        Box radicand;
        if (!parse_script_argument(radicand, display, face)) return false;
        if (!layout_result(box_radical(fonts_, radicand, has_index ? &index : nullptr,
                                        size_, out, error_), command)) return false;
        atom_class = AtomClass::Inner;
        return true;
    }

    if (command_is(command, "text")) {
        if (!parse_text_group(out, size_)) return false;
        atom_class = AtomClass::Inner;
        return true;
    }

    if (command_is(command, "mathrm") || command_is(command, "mathbf") ||
        command_is(command, "mathit") || command_is(command, "mathbb") ||
        command_is(command, "mathcal")) {
        Face selected = Face::Text;
        if (command_is(command, "mathbf")) selected = Face::Bold;
        else if (command_is(command, "mathit")) selected = Face::Italic;
        else if (command_is(command, "mathbb")) selected = Face::Blackboard;
        else if (command_is(command, "mathcal")) selected = Face::Calligraphic;
        if (!parse_group(out, display, selected)) return false;
        atom_class = AtomClass::Inner;
        return true;
    }

    if (command_is(command, "vec") || command_is(command, "hat") ||
        command_is(command, "widehat") || command_is(command, "tilde") ||
        command_is(command, "widetilde") || command_is(command, "bar") ||
        command_is(command, "overline") || command_is(command, "underline")) {
        const double saved_size = size_;
        size_ *= 0.92;
        Box base;
        const bool parsed = parse_script_argument(base, display, face);
        size_ = saved_size;
        if (!parsed) return false;
        const bool line = command_is(command, "bar") ||
                          command_is(command, "overline") ||
                          command_is(command, "underline");
        const bool under = command_is(command, "underline");
        uint32_t mark = '^';
        if (command_is(command, "vec")) mark = 0x2192;
        else if (command_is(command, "tilde") || command_is(command, "widetilde"))
            mark = 0x02dc;
        if (!layout_result(box_accent(fonts_, base, mark, line, under,
                                       size_, out, error_), command))
            return false;
        atom_class = AtomClass::Ordinary;
        return true;
    }

    if (command_is(command, "left")) {
        uint32_t left = 0;
        size_t delimiter_begin = position_;
        if (position_ >= length_)
            return fail(Code::Parse, "Missing left delimiter",
                        command.begin, command.end);
        if (!parse_delimiter(left, delimiter_begin)) return false;
        if (!enter(command.begin)) return false;
        Box body;
        if (!parse_expression(body, display, face, Stop::Right, nullptr, 0)) {
            leave();
            return false;
        }
        leave();
        if (position_ >= length_)
            return fail(Code::Parse, "Missing a matching right delimiter",
                        command.begin, command.end);
        Command right_command{};
        if (!read_command(right_command)) return false;
        if (!command_is(right_command, "right"))
            return fail(Code::Parse, "Expected a matching right delimiter",
                        right_command.begin, right_command.end);
        uint32_t right = 0;
        if (position_ >= length_)
            return fail(Code::Parse, "Missing right delimiter",
                        right_command.begin, right_command.end);
        size_t right_begin = position_;
        if (!parse_delimiter(right, right_begin)) return false;
        if (!pair_delimiters(left, right))
            return fail(Code::Parse, "Mismatched scalable delimiters",
                        command.begin, right_begin);
        if (!wrap_delimited(body, left, right, out, command, right_command))
            return false;
        atom_class = AtomClass::Inner;
        return true;
    }
    if (command_is(command, "right"))
        return fail(Code::Parse, "Right delimiter has no matching left delimiter",
                    command.begin, command.end);

    if (command_is(command, "begin")) {
        if (!parse_environment(out, command, display, face)) return false;
        atom_class = AtomClass::Inner;
        return true;
    }
    if (command_is(command, "end"))
        return fail(Code::Parse, "Unexpected environment terminator",
                    command.begin, command.end);

    if (command_is(command, "sum") || command_is(command, "prod") ||
        command_is(command, "int") || command_is(command, "iint") ||
        command_is(command, "iiint") || command_is(command, "oint")) {
        uint32_t codepoint = 0;
        if (command_is(command, "sum")) codepoint = 0x2211;
        else if (command_is(command, "prod")) codepoint = 0x220f;
        else if (command_is(command, "int")) codepoint = 0x222b;
        else if (command_is(command, "iint")) codepoint = 0x222c;
        else if (command_is(command, "iiint")) codepoint = 0x222d;
        else codepoint = 0x222e;
        const double operator_size = size_ * (display ? 1.18 : 1.0);
        if (!box_make_glyph(fonts_, codepoint, operator_size, Face::Math, out, error_)) {
            if (error_.code == Code::Font)
                error_.set(Code::Font, "A required large operator is unavailable",
                           command.begin, command.end);
            return false;
        }
        atom_class = AtomClass::LargeOperator;
        return true;
    }

    if (command_is(command, "lim")) {
        if (!make_word(out, "lim", Face::Text, size_, AtomClass::LargeOperator,
                       command.begin, command.end)) return false;
        atom_class = AtomClass::LargeOperator;
        return true;
    }

    for (size_t i = 0; i < sizeof(kFunctions) / sizeof(kFunctions[0]); ++i) {
        if (command_is(command, kFunctions[i])) {
            if (!make_word(out, kFunctions[i], Face::Text, size_, AtomClass::Operator,
                           command.begin, command.end)) return false;
            atom_class = AtomClass::Operator;
            return true;
        }
    }

    if (command_is(command, "quad") || command_is(command, "qquad")) {
        is_spacing = true;
        spacing = size_ * (command_is(command, "qquad") ? 2.0 : 1.0);
        return true;
    }

    for (size_t i = 0; i < sizeof(kSymbols) / sizeof(kSymbols[0]); ++i) {
        if (!command_is(command, kSymbols[i].name)) continue;
        atom_class = static_cast<AtomClass>(kSymbols[i].atom_class);
        if (atom_class == AtomClass::Open || atom_class == AtomClass::Close) {
            if (!make_glyph(out, kSymbols[i].codepoint, face,
                            command.begin, command.end)) return false;
            return true;
        }
        return make_glyph(out, kSymbols[i].codepoint, face,
                          command.begin, command.end);
    }

    return fail(Code::Unsupported, "Unsupported math command",
                command.begin, command.end);
}

bool LatexParser::parse_text_group(Box &out, double text_size) {
    if (position_ >= length_ || source_[position_] != '{')
        return fail_here(Code::Parse, "Expected a braced text argument");
    const size_t opening = position_;
    if (!enter(opening)) return false;
    ++position_;
    Box candidate;
    double cursor = 0;
    size_t nested = 0;
    while (position_ < length_) {
        const size_t begin = position_;
        if (source_[position_] == '}' && nested == 0) {
            ++position_;
            leave();
            candidate.width = cursor;
            out = static_cast<Box &&>(candidate);
            return true;
        }
        if (source_[position_] == '{') {
            if (!enter(position_)) {
                leave();
                return false;
            }
            ++nested;
            ++position_;
            Box brace;
            if (!box_make_glyph(fonts_, '{', text_size, Face::Text, brace, error_) ||
                !box_place(candidate, brace, cursor, 0, error_)) {
                leave();
                return false;
            }
            cursor += brace.width;
            continue;
        }
        if (source_[position_] == '}') {
            --nested;
            ++position_;
            Box brace;
            if (!box_make_glyph(fonts_, '}', text_size, Face::Text, brace, error_) ||
                !box_place(candidate, brace, cursor, 0, error_)) {
                leave();
                return false;
            }
            cursor += brace.width;
            leave();
            continue;
        }

        uint32_t codepoint = 0;
        size_t bytes = 0;
        if (source_[position_] == '\\') {
            Command escaped{};
            if (!read_command(escaped)) {
                leave();
                return false;
            }
            if (escaped.word)
                return fail(Code::Unsupported, "Only escaped characters are allowed in text",
                            escaped.begin, escaped.end);
            const char symbol = source_[escaped.name_begin];
            if (symbol != '{' && symbol != '}' && symbol != '\\' &&
                symbol != '%' && symbol != '#' && symbol != '$' &&
                symbol != '_' && symbol != '&') {
                leave();
                return fail(Code::Unsupported, "Unsupported text escape",
                            escaped.begin, escaped.end);
            }
            codepoint = static_cast<uint8_t>(symbol);
            bytes = 0;
        } else {
            if (!decode_codepoint(position_, codepoint, bytes)) {
                leave();
                return fail_here(Code::InvalidUtf8, "Invalid UTF-8 in text");
            }
            if (codepoint == '\n' || codepoint == '\r') codepoint = ' ';
            position_ += bytes;
        }
        Box glyph_box_value;
        if (!box_make_glyph(fonts_, codepoint, text_size, Face::Text,
                            glyph_box_value, error_)) {
            if (error_.code == Code::Font)
                error_.set(Code::Font, "A required text glyph is unavailable",
                           begin, position_ > begin ? position_ : begin + 1);
            leave();
            return false;
        }
        if (!box_place(candidate, glyph_box_value, cursor, 0, error_)) {
            leave();
            return false;
        }
        cursor += glyph_box_value.width;
        if (!isfinite(cursor)) {
            leave();
            return fail(Code::Limit, "Text width overflow", begin, position_);
        }
    }
    leave();
    return fail(Code::Parse, "Unclosed text argument", opening, opening + 1);
}

bool LatexParser::parse_delimiter(uint32_t &codepoint, size_t &begin) {
    begin = position_;
    if (position_ >= length_)
        return fail(Code::Parse, "Missing delimiter", position_, position_);
    if (source_[position_] != '\\') {
        const char value = source_[position_];
        ++position_;
        if (value == '.') codepoint = 0;
        else if (value == '(' || value == ')' || value == '[' || value == ']' ||
                 value == '|' || value == '<' || value == '>') {
            codepoint = static_cast<uint8_t>(value);
        } else {
            return fail(Code::Parse, "Unsupported scalable delimiter",
                        begin, position_);
        }
        return true;
    }

    Command command{};
    if (!read_command(command)) return false;
    if (!command.word) {
        const char value = source_[command.name_begin];
        if (value == '{' || value == '}') codepoint = static_cast<uint8_t>(value);
        else if (value == '|') codepoint = 0x2016;
        else if (value == '.') codepoint = 0;
        else return fail(Code::Unsupported, "Unsupported scalable delimiter",
                         command.begin, command.end);
        return true;
    }
    for (size_t i = 0; i < sizeof(kSymbols) / sizeof(kSymbols[0]); ++i) {
        if (command_is(command, kSymbols[i].name) &&
            (kSymbols[i].atom_class == 5 || kSymbols[i].atom_class == 6)) {
            codepoint = kSymbols[i].codepoint;
            return true;
        }
    }
    return fail(Code::Unsupported, "Unsupported scalable delimiter",
                command.begin, command.end);
}

bool LatexParser::wrap_delimited(const Box &body, uint32_t left, uint32_t right,
                                Box &out, const Command &left_command,
                                const Command &right_command) {
    const double gap = size_ * 0.08;
    const double left_gap = left ? gap : 0;
    const double right_gap = right ? gap : 0;
    double ascent = body.ascent + gap;
    double descent = body.descent + gap;
    if (ascent < size_ * 0.65) ascent = size_ * 0.65;
    if (descent < size_ * 0.28) descent = size_ * 0.28;
    Box left_box;
    Box right_box;
    if (!layout_result(box_delimiter(fonts_, left, size_, ascent, descent,
                                      left_box, error_), left_command) ||
        !layout_result(box_delimiter(fonts_, right, size_, ascent, descent,
                                      right_box, error_), right_command))
        return false;
    Box candidate;
    double x = 0;
    if (!layout_result(box_place(candidate, left_box, x, 0, error_), left_command))
        return false;
    x += left_box.width + left_gap;
    if (!layout_result(box_place(candidate, body, x, 0, error_), left_command))
        return false;
    x += body.width + right_gap;
    if (!layout_result(box_place(candidate, right_box, x, 0, error_), right_command))
        return false;
    candidate.width = x + right_box.width;
    out = static_cast<Box &&>(candidate);
    return true;
}

bool LatexParser::parse_environment(Box &out, const Command &command,
                                   bool display, Face face) {
    if (position_ >= length_ || source_[position_] != '{')
        return fail(Code::Parse, "Expected an environment name after begin",
                    command.begin, command.end);
    ++position_;
    const size_t environment_begin = position_;
    while (position_ < length_ && ascii_letter(source_[position_])) ++position_;
    const size_t environment_length = position_ - environment_begin;
    if (!environment_length || environment_length >= 32 ||
        position_ >= length_ || source_[position_] != '}')
        return fail(Code::Parse, "Malformed environment name",
                    environment_begin, position_ > environment_begin
                                           ? position_ : environment_begin + 1);
    char environment[32];
    memcpy(environment, source_ + environment_begin, environment_length);
    environment[environment_length] = '\0';
    GridLayout layout = GridLayout::Centered;
    uint32_t left = 0;
    uint32_t right = 0;
    bool surrounded = false;
    if (slice_equals(environment, environment_length, "matrix")) {
    } else if (slice_equals(environment, environment_length, "pmatrix")) {
        left = '('; right = ')'; surrounded = true;
    } else if (slice_equals(environment, environment_length, "bmatrix")) {
        left = '['; right = ']'; surrounded = true;
    } else if (slice_equals(environment, environment_length, "vmatrix")) {
        left = '|'; right = '|'; surrounded = true;
    } else if (slice_equals(environment, environment_length, "Vmatrix")) {
        left = 0x2016; right = 0x2016; surrounded = true;
    } else if (slice_equals(environment, environment_length, "cases")) {
        layout = GridLayout::Cases; left = '{'; surrounded = true;
    } else if (slice_equals(environment, environment_length, "aligned")) {
        layout = GridLayout::Aligned;
    } else if (slice_equals(environment, environment_length, "gathered")) {
        layout = GridLayout::Gathered;
    } else {
        return fail(Code::Unsupported, "Unsupported math environment",
                    environment_begin, environment_begin + environment_length);
    }
    const size_t environment_end = position_;
    if (!enter(command.begin)) return false;
    ++position_;

    Array<Box> cells;
    Array<size_t> row_lengths;
    size_t rows = 0;
    size_t columns_in_row = 0;
    bool finished = false;
    while (!finished) {
        if (rows >= 16) {
            leave();
            return fail(Code::Limit, "Matrix row count exceeds 16",
                        position_, position_ < length_ ? position_ + 1 : position_);
        }
        Box cell;
        if (!parse_expression(cell, display, face, Stop::GridCell,
                             environment, environment_length)) {
            leave();
            return false;
        }
        if (cells.size() >= 256 || columns_in_row >= 16) {
            leave();
            return fail(Code::Limit, "Matrix dimensions exceed 16 by 16",
                        position_, position_ < length_ ? position_ + 1 : position_);
        }
        if (!cells.push(static_cast<Box &&>(cell))) {
            leave();
            return fail(Code::NoMemory, "Cannot allocate matrix cells",
                        command.begin, command.end);
        }
        ++columns_in_row;
        if (position_ < length_ && source_[position_] == '&') {
            if (columns_in_row >= 16) {
                leave();
                return fail(Code::Limit, "Matrix column count exceeds 16",
                            position_, position_ + 1);
            }
            ++position_;
            continue;
        }
        if (position_ >= length_) {
            leave();
            return fail(Code::Parse, "Unclosed math environment",
                        environment_begin, environment_end);
        }
        size_t row_break_end = 0;
        if (at_row_break(&row_break_end)) {
            if (!row_lengths.push(columns_in_row)) {
                leave();
                return fail(Code::NoMemory, "Cannot allocate matrix row data",
                            command.begin, command.end);
            }
            ++rows;
            columns_in_row = 0;
            position_ = row_break_end;
            size_t terminator_end = 0;
            if (at_environment_end(environment, environment_length, &terminator_end)) {
                position_ = terminator_end;
                finished = true;
            }
            continue;
        }
        size_t terminator_end = 0;
        if (!at_environment_end(environment, environment_length, &terminator_end)) {
            leave();
            return fail_here(Code::Parse, "Expected an alignment marker or matching end");
        }
        if (!row_lengths.push(columns_in_row)) {
            leave();
            return fail(Code::NoMemory, "Cannot allocate matrix row data",
                        command.begin, command.end);
        }
        ++rows;
        position_ = terminator_end;
        finished = true;
    }
    leave();

    size_t columns = 0;
    for (size_t row = 0; row < row_lengths.size(); ++row)
        if (row_lengths[row] > columns) columns = row_lengths[row];
    if (!columns || !rows || columns > 16 || rows > 16)
        return fail(Code::Limit, "Matrix dimensions must be between 1 and 16",
                    environment_begin, environment_end);
    if ((layout == GridLayout::Cases && columns > 2) ||
        (layout == GridLayout::Gathered && columns > 1))
        return fail(Code::Format, "Environment has an invalid number of columns",
                    environment_begin, environment_end);
    if (rows > SIZE_MAX / columns)
        return fail(Code::Limit, "Matrix cell count overflow",
                    environment_begin, environment_end);
    Array<Box> rectangular;
    const size_t cell_count = rows * columns;
    if (!rectangular.resize(cell_count))
        return fail(Code::NoMemory, "Cannot allocate rectangular matrix",
                    environment_begin, environment_end);
    size_t source_index = 0;
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column < row_lengths[row]; ++column) {
            rectangular[row * columns + column] =
                static_cast<Box &&>(cells[source_index++]);
        }
    }
    Box grid;
    Command environment_command = command;
    environment_command.end = environment_end + 1;
    if (!layout_result(box_grid(rectangular.data(), rows, columns, layout,
                                  size_, grid, error_), environment_command))
        return false;
    if (surrounded) {
        if (!wrap_delimited(grid, left, right, out,
                             environment_command, environment_command)) return false;
    } else {
        out = static_cast<Box &&>(grid);
    }
    return true;
}

bool parse_latex(const char *source, size_t length, double size, bool display,
                 FontBook &fonts, Box &out, Error &error) {
    error.clear();
    if (!source)
        return error.set(Code::Parse, "Formula source is null"), false;
    if (length > kMaxSourceBytes)
        return error.set(Code::Limit, "Formula source exceeds 64 KiB",
                         kMaxSourceBytes, kMaxSourceBytes + 1), false;
    if (!isfinite(size) || size <= 0)
        return error.set(Code::Limit, "Formula effective size must be finite and positive"),
               false;
    size_t invalid_byte = 0;
    if (!detail::valid_utf8(source, length, &invalid_byte))
        return error.set(Code::InvalidUtf8, "Formula source is not valid UTF-8",
                         invalid_byte, invalid_byte + 1), false;
    // The caller supplies pixels after DPI/scale conversion. Keep FreeType's
    // nominal glyph size bounded, and carry larger sizes in scene transforms.
    const double layout_size = size > kMaxPointSize ? kMaxPointSize : size;
    LatexParser parser(source, length, layout_size, display, fonts, error);
    Box candidate;
    if (!parser.parse(candidate)) return false;
    if (size != layout_size && !box_scale(candidate, size / layout_size, error))
        return false;
    out = static_cast<Box &&>(candidate);
    return true;
}

} // namespace rm
