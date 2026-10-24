#ifndef RELIEFMATH_PARSER_H
#define RELIEFMATH_PARSER_H

#include "boxes.h"

namespace rm {

class LatexParser {
    enum class Stop : uint8_t { End, Group, GridCell, Right, Bracket };
    enum class AtomClass : uint8_t {
        Ordinary, Operator, LargeOperator, Binary, Relation, Open, Close,
        Punctuation, Inner
    };
    struct Command {
        size_t begin;
        size_t end;
        size_t name_begin;
        size_t name_length;
        bool word;
    };

    const char *source_;
    size_t length_;
    size_t position_ = 0;
    size_t depth_ = 0;
    double size_;
    bool display_;
    FontBook &fonts_;
    Error &error_;

    bool parse_source(Box &);
    bool parse_expression(Box &, bool, Face, Stop, const char *, size_t);
    bool parse_atom(Box &, AtomClass &, bool &, double &, bool, Face, bool &);
    bool parse_group(Box &, bool, Face);
    bool parse_script_argument(Box &, bool, Face);
    bool parse_command(Box &, AtomClass &, bool &, double &, bool, Face,
                       const Command &);
    bool parse_environment(Box &, const Command &, bool, Face);
    bool parse_text_group(Box &, double);
    bool parse_delimiter(uint32_t &, size_t &);
    bool read_command(Command &);
    bool at_control(const char *, size_t, size_t *) const;
    bool at_environment_end(const char *, size_t, size_t *) const;
    bool at_row_break(size_t *) const;
    bool skip_ignored();
    bool decode_codepoint(size_t, uint32_t &, size_t &) const;
    bool enter(size_t);
    void leave();
    bool fail(Code, const char *, size_t, size_t);
    bool fail_here(Code, const char *);
    bool command_is(const Command &, const char *) const;
    double inter_atom_space(AtomClass, AtomClass, double) const;
    bool make_glyph(Box &, uint32_t, Face, size_t, size_t);
    bool make_word(Box &, const char *, Face, double, AtomClass, size_t, size_t);
    bool layout_result(bool, const Command &);
    bool wrap_delimited(const Box &, uint32_t, uint32_t, Box &,
                        const Command &, const Command &);

public:
    LatexParser(const char *, size_t, double, bool, FontBook &, Error &);
    bool parse(Box &);
};

} // namespace rm

#endif
