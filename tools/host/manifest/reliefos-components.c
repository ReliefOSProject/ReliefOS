#define _POSIX_C_SOURCE 200809L
/* Deliberately constrained TOML schema: bare keys, basic single-line strings,
 * booleans, version integer, and arrays of strings. Unsupported TOML syntax is
 * rejected, never guessed. Bounds cap manifest complexity and Make injection.
 */
#include "tools/host/common/io.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIMIT 256
struct component {
  char id[96], symbol[96], kind[96];
  char label[1024], category[1024], extensions[1024];
  int stage, entry, sdk, api, open_with, image_supported;
  char deps[32][96];
  size_t ndeps;
  char api_deps[32][96];
  size_t napi_deps;
  int api_capable, api_state;
  unsigned seen;
  int def, required, enabled, state;
};
static struct component components[LIMIT];
static size_t count;
static const char *cursor;
static void fail(const char *message) {
  fprintf(stderr, "components: %s\n", message);
  exit(1);
}
static void space(void) {
  for (;;) {
    while (isspace((unsigned char)*cursor))
      cursor++;
    if (*cursor != '#')
      break;
    while (*cursor && *cursor != '\n')
      cursor++;
  }
}
static void expect(char ch) {
  space();
  if (*cursor != ch)
    fail("unsupported or malformed TOML syntax");
  cursor++;
}
static void end_line(void) {
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
    cursor++;
  if (*cursor && *cursor != '\n' && *cursor != '#')
    fail("trailing TOML data");
}
static void token(char *out, size_t capacity, int quoted) {
  size_t n = 0;
  space();
  if (quoted)
    expect('"');
  while (*cursor && (quoted ? *cursor != '"'
                            : (isalnum((unsigned char)*cursor) ||
                               *cursor == '_' || *cursor == '-'))) {
    if (*cursor == '\\' || *cursor == '\n' || *cursor == '\r' ||
        n + 1 >= capacity)
      fail("unsupported escape, multiline string or oversized token");
    out[n++] = *cursor++;
  }
  if (quoted) {
    if (*cursor != '"')
      fail("unterminated string");
    cursor++;
  }
  if (!n && !quoted)
    fail("missing key/value");
  out[n] = 0;
}
static void safe_name(const char *s, int symbol) {
  if (!*s)
    fail("empty identifier");
  for (; *s; s++)
    if (!(isalnum((unsigned char)*s) || *s == '_' || (!symbol && *s == '-')))
      fail("unsafe identifier");
}
static int boolean(void) {
  char s[96];
  token(s, sizeof s, 0);
  if (!strcmp(s, "true"))
    return 1;
  if (!strcmp(s, "false"))
    return 0;
  fail("expected boolean");
  return 0;
}
static void field(struct component *c) {
  static const char *keys[] = {
      "id",         "symbol",    "kind",         "default",
      "required",   "stage",     "entry",        "sdk",
      "api",        "label",     "category",     "depends",
      "extensions", "open_with", "api_requires", "api_stage_path",
      "image_supported"};
  char key[96], value[1024];
  size_t k;
  token(key, sizeof key, 0);
  expect('=');
  for (k = 0; k < sizeof keys / sizeof keys[0]; k++)
    if (!strcmp(key, keys[k]))
      break;
  if (k == sizeof keys / sizeof keys[0])
    fail("unknown component field");
  if (c->seen & (1u << k))
    fail("duplicate component field");
  c->seen |= 1u << k;
  if (k == 11 || k == 12 || k == 14) {
    expect('[');
    space();
    while (*cursor != ']') {
      token(value, sizeof value, 1);
      if (k == 11) {
        safe_name(value, 0);
        if (strlen(value) >= 96 || c->ndeps == 32)
          fail("dependency limit");
        memcpy(c->deps[c->ndeps++], value, strlen(value) + 1);
      }
      if (k == 14) {
        safe_name(value, 0);
        if (strlen(value) >= 96 || c->napi_deps == 32)
          fail("API dependency limit");
        memcpy(c->api_deps[c->napi_deps++], value, strlen(value) + 1);
      }
      if (k == 12) {
        size_t used = strlen(c->extensions), length = strlen(value);
        if (used + length + 2 > sizeof c->extensions || strchr(value, '\t'))
          fail("invalid extensions");
        if (used)
          c->extensions[used++] = ',';
        memcpy(c->extensions + used, value, length + 1);
      }
      space();
      if (*cursor == ']')
        break;
      expect(',');
      space();
    }
    expect(']');
  } else if ((k >= 3 && k <= 8) || k == 13 || k == 16) {
    int b = boolean();
    if (k == 3)
      c->def = b;
    if (k == 4)
      c->required = b;
    if (k == 5)
      c->stage = b;
    if (k == 6)
      c->entry = b;
    if (k == 7)
      c->sdk = b;
    if (k == 8)
      c->api = b;
    if (k == 13)
      c->open_with = b;
    if (k == 16)
      c->image_supported = b;
  } else {
    token(value, sizeof value, 1);
    if (strchr(value, '\t'))
      fail("tab in component string");
    if (k == 9)
      memcpy(c->label, value, strlen(value) + 1);
    if (k == 10)
      memcpy(c->category, value, strlen(value) + 1);
    if (k < 3) {
      if (strlen(value) >= 96)
        fail("identifier limit");
      if (k < 2)
        safe_name(value, k == 1);
      memcpy(k == 0   ? c->id
             : k == 1 ? c->symbol
                      : c->kind,
             value, strlen(value) + 1);
    }
  }
  /* Only whitespace/comment/newline may follow a value. */
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
    cursor++;
  if (*cursor && *cursor != '\n' && *cursor != '#')
    fail("trailing TOML data");
}
static void parse(const char *data) {
  char key[96];
  struct component *c = NULL;
  cursor = data;
  token(key, sizeof key, 0);
  if (strcmp(key, "version"))
    fail("version must be first");
  expect('=');
  token(key, sizeof key, 0);
  if (strcmp(key, "1"))
    fail("unsupported schema version");
  end_line();
  for (;;) {
    space();
    if (!*cursor)
      break;
    if (*cursor == '[') {
      expect('[');
      expect('[');
      token(key, sizeof key, 0);
      if (strcmp(key, "components"))
        fail("unknown table");
      expect(']');
      expect(']');
      end_line();
      if (count == LIMIT)
        fail("too many components");
      c = &components[count++];
      c->image_supported = 1;
    } else {
      if (!c)
        fail("field outside component");
      field(c);
    }
  }
  for (size_t i = 0; i < count; i++) {
    c = &components[i];
    if ((c->seen & 31) != 31)
      fail("missing required component fields");
    if (strcmp(c->kind, "system-app") && strcmp(c->kind, "program-app") &&
        strcmp(c->kind, "package-app") && strcmp(c->kind, "tool") &&
        strcmp(c->kind, "library"))
      fail("unknown kind");
    if (c->stage && !c->image_supported)
      fail("image membership is unsupported for this component");
    if (c->entry && !strcmp(c->kind, "library"))
      fail("application entry is unsupported for this component");
    for (size_t j = 0; j < i; j++)
      if (!strcmp(c->id, components[j].id) ||
          !strcmp(c->symbol, components[j].symbol))
        fail("duplicate component");
    c->enabled = c->required || c->def;
    c->api_capable = c->api;
  }
}
static size_t lookup(const char *id) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(id, components[i].id))
      return i;
  fail("unknown dependency");
  return 0;
}
static void visit(size_t i, int enable) {
  struct component *c = &components[i];
  if (c->state == 1)
    fail("dependency cycle");
  if (c->state == 2 && !enable)
    return;
  c->state = 1;
  if (enable)
    c->enabled = 1;
  for (size_t j = 0; j < c->ndeps; j++)
    visit(lookup(c->deps[j]), enable);
  c->state = 2;
}
static void visit_api(size_t i, int enable) {
  struct component *c = &components[i];
  if (c->api_state == 1)
    fail("API dependency cycle");
  if (c->api_state == 2 && !enable)
    return;
  c->api_state = 1;
  if (enable) {
    visit(i, 1);
    c->api = 1;
  }
  for (size_t j = 0; j < c->napi_deps; j++)
    visit_api(lookup(c->api_deps[j]), enable);
  c->api_state = 2;
}
static void config(const char *path) {
  FILE *f = fopen(path, "r");
  char *line = NULL;
  size_t capacity = 0;
  if (!f)
    fail("cannot open config");
  while (getline(&line, &capacity, f) >= 0) {
    for (size_t i = 0; i < count; i++) {
      /* SDK/API metadata stays manifest-owned: no production action consumes
       * per-component configuration switches for those fields. */
      const char *suffixes[] = {"BUILD", "IMAGE", "ENTRY"};
      int *flags[] = {&components[i].enabled, &components[i].stage,
                      &components[i].entry};
      for (size_t flag = 0; flag < sizeof suffixes / sizeof suffixes[0]; ++flag) {
        if ((flag == 1 && !components[i].image_supported) ||
            (flag == 2 && !strcmp(components[i].kind, "library")))
          continue;
        char key[160];
        int n = snprintf(key, sizeof key, "CONFIG_RELIEFOS_COMPONENT_%s_%s",
                         components[i].symbol, suffixes[flag]);
        if (n < 0 || (size_t)n >= sizeof key)
          fail("config key overflow");
        int value = -1;
        if (!strncmp(line, key, (size_t)n) && line[n] == '=')
          value = line[n + 1] == 'y';
        if (!strncmp(line, "# ", 2) && !strncmp(line + 2, key, (size_t)n) &&
            !strncmp(line + 2 + n, " is not set", 11))
          value = 0;
        if (value >= 0 && !components[i].required)
          *flags[flag] = value;
      }
    }
  }
  free(line);
  if (ferror(f))
    fail("config read failed");
  if (fclose(f))
    fail("config close failed");
  for (size_t i = 0; i < count; i++)
    visit(i, 0);
  for (size_t i = 0; i < count; i++)
    if (components[i].enabled)
      visit(i, 1);
  for (size_t i = 0; i < count; i++) {
    components[i].api = components[i].enabled && components[i].api_capable &&
                        components[i].api;
    visit_api(i, 0);
  }
  for (size_t i = 0; i < count; i++)
    if (components[i].api)
      visit_api(i, 1);
  for (size_t i = 0; i < count; i++) {
    struct component *c = &components[i];
    c->stage = c->enabled && c->image_supported && c->stage;
    c->entry = c->stage && strcmp(c->kind, "library") != 0 && c->entry;
    c->sdk = c->enabled && c->sdk;
    c->api = c->enabled && c->api;
  }
}
static void append(struct byte_buffer *out, const char *s) {
  size_t n = strlen(s);
  if (n > SIZE_MAX - out->len || buffer_reserve(out, out->len + n))
    fail("out of memory");
  memcpy(out->data + out->len, s, n);
  out->len += n;
}
static void selection_json(const char *path) {
  char *data = NULL;
  size_t size = 0;
  FILE *f = open_memstream(&data, &size);
  if (!f)
    fail("selection allocation failed");
  fputs("{\"schema_version\":1,\"components\":{", f);
  for (size_t i = 0; i < count; ++i) {
    struct component *c = &components[i];
    if (i)
      fputc(',', f);
    /* IDs, symbols and kinds have been validated by parse(). */
    fprintf(f,
            "\"%s\":{\"id\":\"%s\",\"symbol\":\"%s\",\"kind\":\"%s\","
            "\"required\":%s,\"build\":%s,\"image\":%s,\"entry\":%s,"
            "\"sdk\":%s,\"api\":%s,\"depends\":[",
            c->id, c->id, c->symbol, c->kind, c->required ? "true" : "false",
            c->enabled ? "true" : "false", c->stage ? "true" : "false",
            c->entry ? "true" : "false", c->sdk ? "true" : "false",
            c->api ? "true" : "false");
    for (size_t j = 0; j < c->ndeps; ++j)
      fprintf(f, "%s\"%s\"", j ? "," : "", c->deps[j]);
    fputs("],\"api_requires\":[", f);
    for (size_t j = 0; j < c->napi_deps; j++)
      fprintf(f, "%s\"%s\"", j ? "," : "", c->api_deps[j]);
    fputs("]}", f);
  }
  fputs("}}\n", f);
  int bad = ferror(f);
  if (fclose(f))
    bad = 1;
  if (bad || write_file_if_changed(path, data, size, 0644))
    fail("selection publication failed");
  free(data);
}
int main(int argc, char **argv) {
  const char *input = NULL, *cfg = NULL, *output = NULL, *metadata = NULL,
             *selection = NULL;
  struct byte_buffer in = {0}, out = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) {
      puts("reliefos-components --input components.toml --config .config "
           "--output components.mk");
      return 0;
    }
    if (i + 1 == argc)
      fail("missing option argument");
    if (!strcmp(argv[i], "--input"))
      input = argv[++i];
    else if (!strcmp(argv[i], "--config"))
      cfg = argv[++i];
    else if (!strcmp(argv[i], "--output"))
      output = argv[++i];
    else if (!strcmp(argv[i], "--metadata"))
      metadata = argv[++i];
    else if (!strcmp(argv[i], "--selection"))
      selection = argv[++i];
    else
      fail("unknown option");
  }
  if (!input || !cfg || !output)
    fail("input, config and output required");
  if (read_file_all(input, &in) || in.len > 1024 * 1024 ||
      buffer_reserve(&in, in.len + 1))
    fail("manifest read failed or exceeds 1 MiB");
  if (memchr(in.data, 0, in.len))
    fail("NUL in manifest");
  in.data[in.len] = 0;
  parse((char *)in.data);
  config(cfg);
  if (selection)
    selection_json(selection);
  const char *names[] = {
      "RELIEFOS_COMPONENTS_ENABLED := ", "RELIEFOS_COMPONENTS_DISABLED := ",
      "RELIEFOS_COMPONENT_APPS := ", "RELIEFOS_COMPONENTS_SDK := ",
      "RELIEFOS_DISABLED_APPS := "};
  for (size_t mode = 0; mode < 5; mode++) {
    append(&out, names[mode]);
    for (size_t i = 0; i < count; i++)
      if (((mode == 1 || mode == 4) ? !components[i].enabled : components[i].enabled) &&
          ((mode != 2 && mode != 4) || strstr(components[i].kind, "-app")) &&
          (mode != 3 || components[i].sdk)) {
        append(&out, components[i].id);
        append(&out, " ");
      }
    append(&out, "\n");
  }
  if (write_file_if_changed(output, out.data, out.len, 0644))
    fail("output write failed");
  if (metadata) {
    buffer_destroy(&out);
    for (size_t i = 0; i < count; ++i) {
      struct component *c = &components[i];
      char line[4096];
      int n = snprintf(line, sizeof line,
                       "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%d\n", c->id,
                       c->kind, c->enabled, c->stage, c->entry, c->sdk, c->api,
                       c->label, c->category, c->extensions, c->open_with);
      if (n < 0 || (size_t)n >= sizeof line)
        fail("component metadata overflow");
      append(&out, line);
    }
    if (write_file_if_changed(metadata, out.data, out.len, 0644))
      fail("metadata write failed");
  }
  buffer_destroy(&in);
  buffer_destroy(&out);
  return 0;
}
