#ifndef RELIEFSETTINGS_MODEL_H
#define RELIEFSETTINGS_MODEL_H

#include "../reliefapk/storage.h"

namespace rs {
using ra::Array;
using ra::Text;
struct Mode {
    Text name, rate;
};
struct Output {
    Text name;
    Array<Mode> modes;
    int current = -1;
};
struct DisplaySetting {
    Text output, mode, rate;
};
struct Variant {
    Text id, label;
};
struct KeyboardLayout {
    Text id, label;
    Array<Variant> variants;
};
struct Desktop {
    Text layout, variant;
    Array<DisplaySetting> displays;
};

bool parse_outputs(const char *, Array<Output> &, Text &);
bool parse_layouts(const char *, Array<KeyboardLayout> &, Text &);
bool parse_desktop(const char *, Desktop &, Text &);
bool encode_desktop(const Desktop &, Text &, Text &);
bool valid_zone(const char *);
bool read_language(const char *, Text &, Text &);
bool update_language(const char *old, const char *language, Text &, Text &);
bool config_directory(Text &, Text &);
bool read_text(const char *path, Text &, Text &, size_t limit = 1024 * 1024);
bool append(Text &, const char *);
bool token(const char *, const char *extra = "_-.+");
bool rate_equal(const Text &, const Text &);
} // namespace rs
#endif
