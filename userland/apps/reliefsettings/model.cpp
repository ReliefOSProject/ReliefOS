#include "model.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <unistd.h>

namespace rs {
namespace {
bool fail(Text &error, const char *message) {
    error.set(message);
    return false;
}
char *trim(char *line) {
    while (*line == ' ' || *line == '\t')
        ++line;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r'))
        line[--n] = 0;
    return line;
}
double rate_number(const char *s) {
    if (!s || !*s || strlen(s) > 20)
        return 0;
    locale_t numeric = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    if (!numeric)
        return 0;
    char *end;
    double rate = strtod_l(s, &end, numeric);
    bool valid = !*end && rate > 0 && rate < 10000;
    freelocale(numeric);
    return valid ? rate : 0;
}
bool rate_value(const char *s) {
    return rate_number(s) > 0;
}
} // namespace
bool rate_equal(const Text &a, const Text &b) {
    double first = rate_number(a.c_str());
    return first > 0 && first == rate_number(b.c_str());
}
bool token(const char *s, const char *extra) {
    if (!s || !*s || *s == '-' || strlen(s) > 128)
        return false;
    for (; *s; ++s) {
        unsigned char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              strchr(extra, c)))
            return false;
    }
    return true;
}
bool append(Text &text, const char *suffix) {
    size_t n = strlen(suffix);
    if (n > SIZE_MAX - text.size() - 1)
        return false;
    char *bytes = static_cast<char *>(malloc(text.size() + n + 1));
    if (!bytes)
        return false;
    memcpy(bytes, text.c_str(), text.size());
    memcpy(bytes + text.size(), suffix, n + 1);
    bool ok = text.set(bytes);
    free(bytes);
    return ok;
}
bool read_text(const char *path, Text &text, Text &error, size_t limit) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(error, strerror(errno));
    Text next;
    bool ok = true;
    for (;;) {
        char bytes[4097];
        ssize_t n = read(fd, bytes, sizeof(bytes) - 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = fail(error, strerror(errno));
            break;
        }
        if (!n)
            break;
        if (next.size() + n > limit || memchr(bytes, 0, n)) {
            ok = fail(error, "Invalid or oversized configuration file.");
            break;
        }
        bytes[n] = 0;
        if (!append(next, bytes)) {
            ok = fail(error, "Out of memory.");
            break;
        }
    }
    close(fd);
    if (ok)
        text = next;
    return ok;
}
bool parse_outputs(const char *text, Array<Output> &result, Text &error) {
    Array<Output> outputs;
    char *copy = strdup(text);
    if (!copy)
        return fail(error, "Out of memory.");
    bool ok = true;
    int selected = -1;
    char *lines;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(nullptr, "\n", &lines)) {
        bool indented = *line == ' ' || *line == '\t';
        char *words;
        char *name = strtok_r(line, " \t\r", &words);
        if (!name)
            continue;
        if (!indented) {
            selected = -1;
            char *state = strtok_r(nullptr, " \t\r", &words);
            if (!state || strcmp(state, "connected") || !token(name))
                continue;
            Output output;
            if (outputs.size() >= 64 || !output.name.set(name) ||
                !outputs.push(static_cast<Output &&>(output))) {
                ok = false;
                break;
            }
            selected = static_cast<int>(outputs.size() - 1);
        } else if (selected >= 0 && token(name)) {
            Output &output = outputs[selected];
            for (char *r = strtok_r(nullptr, " \t\r", &words); r;
                 r = strtok_r(nullptr, " \t\r", &words)) {
                bool current = strchr(r, '*');
                r[strcspn(r, "*+")] = 0;
                if (!rate_value(r))
                    continue;
                Mode mode;
                if (output.modes.size() >= 1024 || !mode.name.set(name) || !mode.rate.set(r) ||
                    !output.modes.push(mode)) {
                    ok = false;
                    break;
                }
                if (current)
                    output.current = static_cast<int>(output.modes.size() - 1);
            }
        }
        if (!ok)
            break;
    }
    free(copy);
    if (!ok)
        return fail(error, "Display information is too large.");
    if (outputs.empty())
        return fail(error, "The X server did not report any connected displays.");
    result = static_cast<Array<Output> &&>(outputs);
    return true;
}
bool parse_layouts(const char *text, Array<KeyboardLayout> &result, Text &error) {
    Array<KeyboardLayout> layouts;
    char *copy = strdup(text);
    if (!copy)
        return fail(error, "Out of memory.");
    enum { Other, Layout, Variants } section = Other;
    bool ok = true;
    char *lines;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(nullptr, "\n", &lines)) {
        line = trim(line);
        if (*line == '!') {
            line = trim(line + 1);
            section =
                !strcmp(line, "layout") ? Layout : (!strcmp(line, "variant") ? Variants : Other);
            continue;
        }
        if (!*line || section == Other)
            continue;
        char *end = line + strcspn(line, " \t");
        if (!*end)
            continue;
        *end++ = 0;
        char *label = trim(end);
        if (!token(line))
            continue;
        if (section == Layout) {
            KeyboardLayout layout;
            ok = layout.id.set(line) && layout.label.set(label) &&
                 layouts.push(static_cast<KeyboardLayout &&>(layout));
        } else {
            char *colon = strchr(label, ':');
            if (!colon)
                continue;
            *colon++ = 0;
            for (size_t i = 0; i < layouts.size(); ++i) {
                if (!(layouts[i].id == trim(label)))
                    continue;
                Variant v;
                ok = v.id.set(line) && v.label.set(trim(colon)) && layouts[i].variants.push(v);
                break;
            }
        }
        if (!ok || layouts.size() > 1024) {
            ok = false;
            break;
        }
    }
    free(copy);
    if (!ok)
        return fail(error, "Keyboard information is too large.");
    if (layouts.empty())
        return fail(error, "No XKB keyboard layouts are installed.");
    result = static_cast<Array<KeyboardLayout> &&>(layouts);
    return true;
}
bool parse_desktop(const char *text, Desktop &result, Text &error) {
    Desktop next;
    char *copy = strdup(text);
    if (!copy)
        return fail(error, "Out of memory.");
    bool ok = true;
    char *lines;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(nullptr, "\n", &lines)) {
        line = trim(line);
        if (!*line || *line == '#')
            continue;
        char *value = strchr(line, '=');
        if (!value) {
            ok = false;
            break;
        }
        *value++ = 0;
        value = trim(value);
        if (!strcmp(line, "XKB_LAYOUT"))
            ok = (!*value || token(value)) && next.layout.set(value);
        else if (!strcmp(line, "XKB_VARIANT"))
            ok = (!*value || token(value)) && next.variant.set(value);
        else if (!strcmp(line, "DISPLAY")) {
            char *words;
            char *output = strtok_r(value, " \t", &words);
            char *mode = strtok_r(nullptr, " \t", &words);
            char *rate = strtok_r(nullptr, " \t", &words);
            DisplaySetting display;
            ok = token(output) && token(mode) && rate_value(rate) &&
                 !strtok_r(nullptr, " \t", &words);
            for (size_t i = 0; ok && i < next.displays.size(); ++i)
                ok = !(next.displays[i].output == output);
            ok = ok && next.displays.size() < 64 && display.output.set(output) &&
                 display.mode.set(mode) && display.rate.set(rate) && next.displays.push(display);
        } else
            ok = false;
        if (!ok)
            break;
    }
    free(copy);
    if (!ok || (!next.variant.empty() && next.layout.empty()))
        return fail(error, "Invalid desktop configuration. Correct the file before saving.");
    result = static_cast<Desktop &&>(next);
    return true;
}
bool encode_desktop(const Desktop &value, Text &result, Text &error) {
    Text next;
    bool ok = append(next, "# ReliefSettings desktop preferences\nXKB_LAYOUT=") &&
              append(next, value.layout.c_str()) && append(next, "\nXKB_VARIANT=") &&
              append(next, value.variant.c_str()) && append(next, "\n");
    for (size_t i = 0; ok && i < value.displays.size(); ++i) {
        const auto &display = value.displays[i];
        ok = append(next, "DISPLAY=") && append(next, display.output.c_str()) &&
             append(next, " ") && append(next, display.mode.c_str()) && append(next, " ") &&
             append(next, display.rate.c_str()) && append(next, "\n");
    }
    Desktop check;
    if (!ok)
        return fail(error, "Out of memory.");
    if (!parse_desktop(next.c_str(), check, error))
        return false;
    result = next;
    return true;
}
bool valid_zone(const char *zone) {
    if (!zone || !*zone || strlen(zone) > 255 || *zone == '/')
        return false;
    const char *part = zone;
    for (const char *p = zone;; ++p) {
        if (*p == '/' || !*p) {
            size_t n = p - part;
            if (!n || (n == 1 && *part == '.') || (n == 2 && !memcmp(part, "..", 2)))
                return false;
            if (!*p)
                return true;
            part = p + 1;
        } else if (!(isalnum(static_cast<unsigned char>(*p)) || strchr("_+-", *p)))
            return false;
    }
}
bool read_language(const char *text, Text &language, Text &error) {
    char *copy = strdup(text);
    if (!copy)
        return fail(error, "Out of memory.");
    Text next;
    bool ok = true;
    char *lines;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(nullptr, "\n", &lines)) {
        line = trim(line);
        if (strncmp(line, "LANG=", 5))
            continue;
        char *value = trim(line + 5);
        size_t n = strlen(value);
        if (n >= 2 &&
            ((*value == '"' && value[n - 1] == '"') || (*value == '\'' && value[n - 1] == '\''))) {
            value[n - 1] = 0;
            ++value;
        }
        if (*value && !token(value, "_-.@")) {
            ok = false;
            break;
        }
        ok = next.set(value);
        if (!ok)
            break;
    }
    free(copy);
    if (!ok)
        return fail(error, "Invalid language configuration.");
    language = next;
    return true;
}
bool update_language(const char *old, const char *language, Text &result, Text &error) {
    if (*language && !token(language, "_-.@"))
        return fail(error, "Unsupported language.");
    Text next;
    for (const char *line = old; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? static_cast<size_t>(end - line + 1) : strlen(line);
        const char *key = line;
        while (*key == ' ' || *key == '\t')
            ++key;
        if (strncmp(key, "LANG=", 5)) {
            Text part;
            if (!part.set(line, n) || !append(next, part.c_str()))
                return fail(error, "Out of memory.");
        }
        line += n;
    }
    if (!next.empty() && next.c_str()[next.size() - 1] != '\n' && !append(next, "\n"))
        return fail(error, "Out of memory.");
    if (*language && !(append(next, "LANG=") && append(next, language) && append(next, "\n")))
        return fail(error, "Out of memory.");
    result = next;
    return true;
}
bool config_directory(Text &directory, Text &error) {
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && *base == '/') {
        if (directory.set(base) && append(directory, "/reliefos"))
            return true;
    } else {
        base = getenv("HOME");
        if (!base || *base != '/')
            return fail(error, "HOME must be an absolute path.");
        if (directory.set(base) && append(directory, "/.config/reliefos"))
            return true;
    }
    return fail(error, "Out of memory.");
}
} // namespace rs
