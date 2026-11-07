#include "snippet.h"

#include "catalog.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace rm {
namespace {

bool failure(Error &error, Code code, const char *message) {
    error.set(code, message);
    return false;
}

class Builder {
public:
    char *bytes;
    size_t size;
    size_t capacity;

    Builder() : bytes(0), size(0), capacity(0) {}
    ~Builder() { free(bytes); }

    bool append(const char *text, size_t length, Error &error) {
        if (length > kMaxSourceBytes - size) {
            return failure(error, Code::Limit, "Snippet exceeds the source byte limit");
        }
        const size_t needed = size + length + 1;
        if (needed > capacity) {
            size_t next = capacity ? capacity : 64;
            while (next < needed) {
                if (next > (kMaxSourceBytes + 1) / 2) {
                    next = kMaxSourceBytes + 1;
                    break;
                }
                next *= 2;
            }
            void *replacement = realloc(bytes, next);
            if (!replacement) {
                return failure(error, Code::NoMemory, "Unable to build snippet source");
            }
            bytes = static_cast<char *>(replacement);
            capacity = next;
        }
        if (length) memcpy(bytes + size, text, length);
        size += length;
        bytes[size] = '\0';
        return true;
    }
};

bool append_text(Builder &builder, const char *text, size_t length,
                 Error &error) {
    return builder.append(text, length, error);
}

bool append_literal(Builder &builder, const char *text, Error &error) {
    return append_text(builder, text, strlen(text), error);
}

bool append_stop(Array<Range> &stops, size_t begin, size_t end, Error &error) {
    if (begin >= end) {
        return failure(error, Code::Format, "Snippet placeholder must not be empty");
    }
    if (!stops.push(Range{begin, end})) {
        return failure(error, Code::NoMemory, "Unable to allocate snippet placeholders");
    }
    return true;
}

bool append_slot(Builder &builder, Array<Range> &stops, const char *fallback,
                 const ToolArgs &args, bool use_selection, Error &error) {
    const char *value = fallback;
    size_t value_size = strlen(fallback);
    if (use_selection && !args.selected.empty()) {
        value = args.selected.c_str();
        value_size = args.selected.size();
    }
    const size_t begin = builder.size;
    if (!append_text(builder, value, value_size, error)) return false;
    return append_stop(stops, begin, builder.size, error);
}

const ToolEntry *entry_for_tool(Tool tool) {
    size_t count = 0;
    const ToolEntry *entries = catalog_entries(&count);
    for (size_t index = 0; index < count; ++index) {
        if (entries[index].tool == tool) return &entries[index];
    }
    return 0;
}

bool append_wrapped(Builder &builder, Array<Range> &stops, const char *before,
                    const char *after, const char *fallback,
                    const ToolArgs &args, Error &error) {
    return append_literal(builder, before, error) &&
           append_slot(builder, stops, fallback, args, true, error) &&
           append_literal(builder, after, error);
}

bool append_matrix(Builder &builder, Array<Range> &stops, const char *environment,
                   const ToolArgs &args, bool aligned, Error &error) {
    uint16_t rows = args.rows ? args.rows : 2;
    uint16_t columns = args.columns ? args.columns : 2;
    if (rows > 16 || columns > 16) {
        return failure(error, Code::Limit, "Matrix dimensions are limited to 16 by 16");
    }
    if (!append_literal(builder, "\\begin{", error) ||
        !append_literal(builder, environment, error) ||
        !append_literal(builder, "}", error)) {
        return false;
    }
    for (uint16_t row = 0; row < rows; ++row) {
        if (row && !append_literal(builder, " \\\\ ", error)) return false;
        for (uint16_t column = 0; column < columns; ++column) {
            if (column) {
                if (aligned && column == 1) {
                    if (!append_literal(builder, " &= ", error)) return false;
                } else if (!append_literal(builder, " & ", error)) {
                    return false;
                }
            }
            char fallback[2] = {static_cast<char>('a' + ((row * columns + column) % 26)), '\0'};
            if (!append_slot(builder, stops, fallback, args,
                             row == 0 && column == 0, error)) {
                return false;
            }
        }
    }
    return append_literal(builder, "\\end{", error) &&
           append_literal(builder, environment, error) &&
           append_literal(builder, "}", error);
}

bool append_big_operator(Builder &builder, Array<Range> &stops,
                         const char *command, const ToolArgs &args,
                         Error &error) {
    return append_literal(builder, command, error) &&
           append_literal(builder, "_{", error) &&
           append_slot(builder, stops, "i=1", args, false, error) &&
           append_literal(builder, "}^{", error) &&
           append_slot(builder, stops, "n", args, false, error) &&
           append_literal(builder, "} ", error) &&
           append_slot(builder, stops, "x", args, true, error);
}

bool append_integral(Builder &builder, Array<Range> &stops,
                     const char *command, const ToolArgs &args,
                     const char *integrand, const char *differential,
                     Error &error) {
    return append_literal(builder, command, error) &&
           append_literal(builder, "_{", error) &&
           append_slot(builder, stops, "a", args, false, error) &&
           append_literal(builder, "}^{", error) &&
           append_slot(builder, stops, "b", args, false, error) &&
           append_literal(builder, "} ", error) &&
           append_slot(builder, stops, integrand, args, true, error) &&
           append_literal(builder, " \\, ", error) &&
           append_literal(builder, differential, error);
}

bool build_source(Tool tool, const ToolArgs &args, Builder &builder,
                  Array<Range> &stops, Error &error) {
    switch (tool) {
    case Tool::Fraction:
        return append_literal(builder, "\\frac{", error) &&
               append_slot(builder, stops, "a", args, true, error) &&
               append_literal(builder, "}{", error) &&
               append_slot(builder, stops, "b", args, false, error) &&
               append_literal(builder, "}", error);
    case Tool::DisplayFraction:
        return append_literal(builder, "\\displaystyle\\frac{", error) &&
               append_slot(builder, stops, "a", args, true, error) &&
               append_literal(builder, "}{", error) &&
               append_slot(builder, stops, "b", args, false, error) &&
               append_literal(builder, "}", error);
    case Tool::Root:
    case Tool::IndexedRoot:
        if (!append_literal(builder, "\\sqrt", error)) return false;
        if (tool == Tool::IndexedRoot || args.numbered_root) {
            if (!append_literal(builder, "[", error) ||
                !append_slot(builder, stops, "n", args, false, error) ||
                !append_literal(builder, "]", error)) {
                return false;
            }
        }
        return append_literal(builder, "{", error) &&
               append_slot(builder, stops, "x", args, true, error) &&
               append_literal(builder, "}", error);
    case Tool::Superscript:
        return append_slot(builder, stops, "a", args, true, error) &&
               append_literal(builder, "^{", error) &&
               append_slot(builder, stops, "b", args, false, error) &&
               append_literal(builder, "}", error);
    case Tool::Subscript:
        return append_slot(builder, stops, "a", args, true, error) &&
               append_literal(builder, "_{", error) &&
               append_slot(builder, stops, "i", args, false, error) &&
               append_literal(builder, "}", error);
    case Tool::Parentheses:
    case Tool::AutoParentheses:
        return append_wrapped(builder, stops, "\\left( ", " \\right)", "x", args, error);
    case Tool::SquareBrackets:
    case Tool::AutoSquareBrackets:
        return append_wrapped(builder, stops, "\\left[ ", " \\right]", "x", args, error);
    case Tool::CurlyBrackets:
    case Tool::AutoCurlyBrackets:
        return append_wrapped(builder, stops, "\\left\\{ ", " \\right\\}", "x", args, error);
    case Tool::AngleBrackets:
        return append_wrapped(builder, stops, "\\left\\langle ", " \\right\\rangle", "x", args, error);
    case Tool::FloorBrackets:
        return append_wrapped(builder, stops, "\\left\\lfloor ", " \\right\\rfloor", "x", args, error);
    case Tool::CeilingBrackets:
        return append_wrapped(builder, stops, "\\left\\lceil ", " \\right\\rceil", "x", args, error);
    case Tool::AutoAbsolute:
    case Tool::AbsoluteValue:
        return append_wrapped(builder, stops, "\\left| ", " \\right|", "x", args, error);
    case Tool::Norm:
        return append_wrapped(builder, stops, "\\left\\| ", " \\right\\|", "x", args, error);
    case Tool::Sum:
        return append_big_operator(builder, stops, "\\sum", args, error);
    case Tool::Product:
        return append_big_operator(builder, stops, "\\prod", args, error);
    case Tool::Integral:
        return append_integral(builder, stops, "\\int", args, "f(x)", "dx", error);
    case Tool::DoubleIntegral:
        return append_integral(builder, stops, "\\iint", args, "f(x,y)",
                               "dx \\, dy", error);
    case Tool::TripleIntegral:
        return append_integral(builder, stops, "\\iiint", args, "f(x,y,z)",
                               "dx \\, dy \\, dz", error);
    case Tool::ClosedIntegral:
        return append_literal(builder, "\\oint_C ", error) &&
               append_slot(builder, stops, "f(x)", args, true, error) &&
               append_literal(builder, " \\, dx", error);
    case Tool::Limit:
        return append_literal(builder, "\\lim_{", error) &&
               append_slot(builder, stops, "x \\to 0", args, false, error) &&
               append_literal(builder, "} ", error) &&
               append_slot(builder, stops, "f(x)", args, true, error);
    case Tool::Sin:
    case Tool::Cos:
    case Tool::Tan:
    case Tool::Log:
    case Tool::Ln:
    case Tool::Exp:
    case Tool::Min:
    case Tool::Max: {
        const ToolEntry *entry = entry_for_tool(tool);
        if (!entry) return failure(error, Code::Unsupported, "Unknown function tool");
        return append_literal(builder, entry->command, error) &&
               append_literal(builder, "{", error) &&
               append_slot(builder, stops, "x", args, true, error) &&
               append_literal(builder, "}", error);
    }
    case Tool::Matrix:
        return append_matrix(builder, stops, "matrix", args, false, error);
    case Tool::ParenthesizedMatrix:
        return append_matrix(builder, stops, "pmatrix", args, false, error);
    case Tool::BracketedMatrix:
        return append_matrix(builder, stops, "bmatrix", args, false, error);
    case Tool::Cases:
        return append_matrix(builder, stops, "cases", args, false, error);
    case Tool::Aligned:
        return append_matrix(builder, stops, "aligned", args, true, error);
    case Tool::Vector:
        return append_wrapped(builder, stops, "\\vec{", "}", "x", args, error);
    case Tool::Hat:
        return append_wrapped(builder, stops, "\\hat{", "}", "x", args, error);
    case Tool::Overline:
        return append_wrapped(builder, stops, "\\overline{", "}", "x", args, error);
    case Tool::Text:
        return append_wrapped(builder, stops, "\\text{", "}", "a", args, error);
    case Tool::Roman:
        return append_wrapped(builder, stops, "\\mathrm{", "}", "a", args, error);
    case Tool::Bold:
        return append_wrapped(builder, stops, "\\mathbf{", "}", "a", args, error);
    case Tool::Italic:
        return append_wrapped(builder, stops, "\\mathit{", "}", "a", args, error);
    default: {
        const ToolEntry *entry = entry_for_tool(tool);
        if (!entry) return failure(error, Code::Unsupported, "Unknown mathematical tool");
        if (!append_literal(builder, entry->insert, error)) return false;
        return true;
    }
    }
}

} // namespace

bool build_snippet(Tool tool, const ToolArgs &args, Snippet &out_snippet,
                   Error &error) {
    error.clear();
    out_snippet.source.clear();
    out_snippet.stops.clear();

    Builder builder;
    if (!build_source(tool, args, builder, out_snippet.stops, error)) {
        out_snippet.stops.clear();
        return false;
    }
    if (!builder.size) {
        out_snippet.stops.clear();
        return failure(error, Code::Format, "Generated snippet is empty");
    }
    if (!out_snippet.source.set(builder.bytes, builder.size)) {
        out_snippet.stops.clear();
        const Code code = builder.size > kMaxTextBytes ? Code::Limit : Code::NoMemory;
        return failure(error, code, "Unable to store generated snippet");
    }
    return true;
}

} // namespace rm
