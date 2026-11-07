#define _XOPEN_SOURCE 700
#include "search.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const size_t kPathCapacity = sizeof(FileSearchResult::path);
const unsigned kMaximumRepresentableDepth = 127;

struct SearchFrame {
    DIR *directory;
    unsigned depth;
    char path[256];
};

struct SearchState {
    bool recursive;
    bool show_hidden;
    bool ignore_ascii_case;
    unsigned max_results;
    unsigned max_depth;
    unsigned long max_scanned;
    char *query;
    SearchFrame *frames;
    unsigned frame_count;
    unsigned frame_capacity;
    FileSearchResult *results;
    unsigned result_count;
    unsigned result_capacity;
    unsigned long scanned;
    unsigned long skipped;
    bool running;
    bool canceled;
    bool limited;
};

static void close_frames(SearchState *state)
{
    while (state->frame_count) {
        SearchFrame *frame = &state->frames[state->frame_count - 1];
        if (frame->directory) closedir(frame->directory);
        frame->directory = 0;
        --state->frame_count;
    }
}

static void clear_search(SearchState *state)
{
    if (!state) return;
    close_frames(state);
    free(state->frames);
    free(state->results);
    free(state->query);
    state->frames = 0;
    state->frame_capacity = 0;
    state->results = 0;
    state->result_count = 0;
    state->result_capacity = 0;
    state->query = 0;
    state->scanned = 0;
    state->skipped = 0;
    state->running = false;
    state->canceled = false;
    state->limited = false;
}

static void mark_skipped(SearchState *state)
{
    if (state->skipped < ULONG_MAX) ++state->skipped;
}

static unsigned char fold_ascii(unsigned char value, bool ignore_case)
{
    if (ignore_case && value >= 'A' && value <= 'Z')
        return static_cast<unsigned char>(value + ('a' - 'A'));
    return value;
}

static bool name_matches(const char *name, const char *query, bool ignore_case)
{
    size_t name_length = strlen(name);
    size_t query_length = strlen(query);
    if (!query_length || query_length > name_length) return false;
    for (size_t offset = 0; offset <= name_length - query_length; ++offset) {
        size_t index = 0;
        while (index < query_length &&
               fold_ascii(static_cast<unsigned char>(name[offset + index]),
                          ignore_case) ==
                   fold_ascii(static_cast<unsigned char>(query[index]),
                              ignore_case)) {
            ++index;
        }
        if (index == query_length) return true;
    }
    return false;
}

static bool join_path(char *destination, const char *parent, const char *name)
{
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    bool root = parent_length == 1 && parent[0] == '/';
    size_t separator_length = root ? 0 : 1;
    if (parent_length + separator_length + name_length >= kPathCapacity)
        return false;
    memcpy(destination, parent, parent_length);
    size_t offset = parent_length;
    if (!root) destination[offset++] = '/';
    memcpy(destination + offset, name, name_length);
    destination[offset + name_length] = 0;
    return true;
}

static void finish_search(SearchState *state)
{
    close_frames(state);
    state->running = false;
}

static bool push_directory(SearchState *state, SearchFrame *parent,
                           const char *name, const char *full_path,
                           unsigned depth)
{
    int descriptor = openat(dirfd(parent->directory), name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        mark_skipped(state);
        return false;
    }
    DIR *directory = fdopendir(descriptor);
    if (!directory) {
        int error = errno;
        close(descriptor);
        errno = error;
        mark_skipped(state);
        return false;
    }
    if (state->frame_count >= state->frame_capacity) {
        closedir(directory);
        mark_skipped(state);
        return false;
    }
    SearchFrame *child = &state->frames[state->frame_count++];
    child->directory = directory;
    child->depth = depth;
    memcpy(child->path, full_path, strlen(full_path) + 1);
    return true;
}

static void append_result(SearchState *state, const char *path, bool directory)
{
    if (state->result_count >= state->max_results) {
        state->limited = true;
        finish_search(state);
        return;
    }
    if (state->result_count == state->result_capacity) {
        unsigned capacity = state->result_capacity ? state->result_capacity * 2 : 16;
        if (capacity < state->result_capacity || capacity > state->max_results)
            capacity = state->max_results;
        if (!capacity || static_cast<size_t>(capacity) >
                             SIZE_MAX / sizeof(FileSearchResult)) {
            mark_skipped(state);
            state->limited = true;
            finish_search(state);
            return;
        }
        FileSearchResult *results = static_cast<FileSearchResult *>(
            calloc(capacity, sizeof(FileSearchResult)));
        if (!results) {
            mark_skipped(state);
            state->limited = true;
            finish_search(state);
            return;
        }
        if (state->result_count) {
            memcpy(results, state->results,
                   state->result_count * sizeof(FileSearchResult));
        }
        free(state->results);
        state->results = results;
        state->result_capacity = capacity;
    }
    FileSearchResult *result = &state->results[state->result_count++];
    memcpy(result->path, path, strlen(path) + 1);
    result->directory = directory;
}

} // namespace

FileSearch::FileSearch() : state_(calloc(1, sizeof(SearchState))) {}

FileSearch::~FileSearch()
{
    SearchState *state = static_cast<SearchState *>(state_);
    if (state) {
        clear_search(state);
        free(state);
    }
}

int FileSearch::start(const char *root, const char *query,
                      const FileSearchOptions &options)
{
    SearchState *state = static_cast<SearchState *>(state_);
    if (!state) return -ENOMEM;
    clear_search(state);
    if (!root || !*root || !query || !*query) return -EINVAL;
    if (strlen(query) >= kPathCapacity) return -ENAMETOOLONG;

    char *canonical_root = realpath(root, 0);
    if (!canonical_root) return errno ? -errno : -EIO;
    size_t root_length = strlen(canonical_root);
    if (root_length >= kPathCapacity) {
        free(canonical_root);
        return -ENAMETOOLONG;
    }

    size_t query_length = strlen(query);
    state->query = static_cast<char *>(malloc(query_length + 1));
    if (!state->query) {
        free(canonical_root);
        clear_search(state);
        return -ENOMEM;
    }
    memcpy(state->query, query, query_length + 1);
    state->recursive = options.recursive;
    state->show_hidden = options.show_hidden;
    state->ignore_ascii_case = options.ignore_ascii_case;
    state->max_results = options.max_results;
    state->max_depth = options.max_depth;
    state->max_scanned = options.max_scanned;

    unsigned depth_limit = state->recursive ? state->max_depth : 0;
    if (depth_limit > kMaximumRepresentableDepth)
        depth_limit = kMaximumRepresentableDepth;
    state->frame_capacity = depth_limit + 1;
    state->frames = static_cast<SearchFrame *>(
        calloc(state->frame_capacity, sizeof(SearchFrame)));
    if (!state->frames) {
        free(canonical_root);
        clear_search(state);
        return -ENOMEM;
    }

    DIR *directory = opendir(canonical_root);
    if (!directory) {
        int error = errno;
        free(canonical_root);
        clear_search(state);
        return error ? -error : -EIO;
    }
    SearchFrame *root_frame = &state->frames[0];
    root_frame->directory = directory;
    root_frame->depth = 0;
    memcpy(root_frame->path, canonical_root, root_length + 1);
    state->frame_count = 1;
    state->running = true;
    free(canonical_root);
    return 0;
}

void FileSearch::step(unsigned budget)
{
    SearchState *state = static_cast<SearchState *>(state_);
    if (!state || !state->running || !budget) return;

    while (state->running && budget) {
        if (state->scanned >= state->max_scanned) {
            state->limited = true;
            finish_search(state);
            break;
        }

        SearchFrame *frame = &state->frames[state->frame_count - 1];
        --budget;
        errno = 0;
        struct dirent *entry = readdir(frame->directory);
        if (!entry) {
            if (errno) mark_skipped(state);
            closedir(frame->directory);
            frame->directory = 0;
            --state->frame_count;
            if (!state->frame_count) state->running = false;
            continue;
        }

        if (state->scanned < ULONG_MAX) ++state->scanned;
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!state->show_hidden && name[0] == '.') continue;

        struct stat status;
        if (fstatat(dirfd(frame->directory), name, &status,
                    AT_SYMLINK_NOFOLLOW) < 0) {
            mark_skipped(state);
            continue;
        }

        char full_path[256];
        bool path_fits = join_path(full_path, frame->path, name);
        bool is_directory = S_ISDIR(status.st_mode);
        if (name_matches(name, state->query, state->ignore_ascii_case)) {
            if (!path_fits) {
                mark_skipped(state);
                continue;
            }
            append_result(state, full_path, is_directory);
            if (!state->running) break;
        }

        if (!is_directory || !state->recursive) continue;
        unsigned child_depth = frame->depth + 1;
        if (child_depth > state->max_depth ||
            child_depth >= state->frame_capacity || !path_fits) {
            mark_skipped(state);
            continue;
        }
        (void)push_directory(state, frame, name, full_path, child_depth);
    }
}

void FileSearch::cancel()
{
    SearchState *state = static_cast<SearchState *>(state_);
    if (!state || !state->running) return;
    finish_search(state);
    state->canceled = true;
}

bool FileSearch::running() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state && state->running;
}

bool FileSearch::canceled() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state && state->canceled;
}

bool FileSearch::limited() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state && state->limited;
}

unsigned FileSearch::result_count() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state ? state->result_count : 0;
}

const FileSearchResult *FileSearch::result(unsigned index) const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state && index < state->result_count ? &state->results[index] : 0;
}

unsigned long FileSearch::scanned() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state ? state->scanned : 0;
}

unsigned long FileSearch::skipped() const
{
    const SearchState *state = static_cast<const SearchState *>(state_);
    return state ? state->skipped : 0;
}
