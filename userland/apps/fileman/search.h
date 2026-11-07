#ifndef RELIEFOS_FILEMAN_SEARCH_H
#define RELIEFOS_FILEMAN_SEARCH_H

struct FileSearchOptions {
    bool recursive = true;
    bool show_hidden = false;
    bool ignore_ascii_case = true;
    unsigned max_results = 1000;
    unsigned max_depth = 32;
    unsigned long max_scanned = 1000000UL;
};

struct FileSearchResult {
    char path[256];
    bool directory;
};

class FileSearch {
public:
    FileSearch();
    ~FileSearch();
    FileSearch(const FileSearch &) = delete;
    FileSearch &operator=(const FileSearch &) = delete;

    int start(const char *root, const char *query,
              const FileSearchOptions &options = FileSearchOptions());
    void step(unsigned budget = 32);
    void cancel();
    bool running() const;
    bool canceled() const;
    bool limited() const;
    unsigned result_count() const;
    const FileSearchResult *result(unsigned index) const;
    unsigned long scanned() const;
    unsigned long skipped() const;

private:
    void *state_;
};

#endif
