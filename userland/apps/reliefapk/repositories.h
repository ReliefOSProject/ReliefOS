#ifndef RELIEFAPK_REPOSITORIES_H
#define RELIEFAPK_REPOSITORIES_H
#include "storage.h"
namespace ra {
struct RepositoryLine {
    Text raw, tag, url;
    bool recognized = false, enabled = false, changed = false;
};
class Repositories {
  public:
    Text path, original, error;
    Array<RepositoryLine> lines;
    bool load(const char *filename);
    bool parse(const char *bytes, size_t length);
    bool replace(size_t index, const char *tag, const char *url, bool enabled);
    bool add(const char *tag, const char *url, bool enabled = true);
    bool serialize(Text &result) const;
};
bool valid_repository(const char *tag, const char *url);
bool secure_cache_directory(const char *path, Text &error);
bool standard_repository_path(const char *path);
bool save_repository_file(const char *path, const char *expected, const char *contents, Text &error);
} // namespace ra
#endif
