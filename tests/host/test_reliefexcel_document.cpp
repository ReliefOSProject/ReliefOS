#include "document.h"
#include "formula.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static char test_root[1024];

static void path(char *out, size_t capacity, const char *name) {
    int n = snprintf(out, capacity, "%s/%s", test_root, name);
    assert(n > 0 && (size_t)n < capacity);
}

static void write_bytes(const char *name, const void *bytes, size_t length) {
    char target[1200];
    path(target, sizeof(target), name);
    FILE *file = fopen(target, "wb");
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static size_t read_bytes(const char *name, unsigned char *bytes, size_t capacity) {
    char target[1200];
    path(target, sizeof(target), name);
    FILE *file = fopen(target, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0 && (size_t)size <= capacity);
    assert(fseek(file, 0, SEEK_SET) == 0);
    assert(fread(bytes, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    return (size_t)size;
}

static bool cancel_progress(void *, unsigned, unsigned) { return false; }

struct SaveProgressState { bool cancelled_after_write = false; };

static bool cancel_after_first_output_quantum(void *context, unsigned completed, unsigned total) {
    SaveProgressState *state = static_cast<SaveProgressState *>(context);
    if (completed && completed < total) {
        state->cancelled_after_write = true;
        return false;
    }
    return true;
}

static unsigned progress_calls;
static bool observe_progress(void *, unsigned, unsigned) {
    ++progress_calls;
    return true;
}

struct ParseProgressState {
    bool read_complete = false;
    bool cancelled_during_parse = false;
};

struct CsvConflictState {
    const char *path = nullptr;
    bool changed = false;
};

static bool cancel_during_csv_parse(void *context, unsigned completed, unsigned total) {
    ParseProgressState *state = static_cast<ParseProgressState *>(context);
    if (total && completed == total) state->read_complete = true;
    if (state->read_complete && completed && completed < total) {
        state->cancelled_during_parse = true;
        return false;
    }
    return true;
}

static bool edit_csv_target_after_stamp_check(void *context, unsigned completed, unsigned total) {
    CsvConflictState *state = static_cast<CsvConflictState *>(context);
    if (!state->changed && completed == 0 && total > 1) {
        int fd = open(state->path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const char external[] = "external edit";
        bool ok = write(fd, external, sizeof(external) - 1) == (ssize_t)(sizeof(external) - 1) &&
            ftruncate(fd, (off_t)(sizeof(external) - 1)) == 0 && fsync(fd) == 0;
        int close_result = close(fd);
        if (ok && close_result == 0) state->changed = true;
        else return false;
    }
    return true;
}

static bool same_file(const char *name, const char *expected, size_t length) {
    unsigned char bytes[4096];
    size_t size = read_bytes(name, bytes, sizeof(bytes));
    return size == length && !memcmp(bytes, expected, length);
}

static bool has_temporary_save(const char *prefix) {
    DIR *directory = opendir(test_root);
    assert(directory);
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (!strncmp(entry->d_name, prefix, strlen(prefix))) found = true;
    assert(closedir(directory) == 0);
    return found;
}

static bool contains_bytes(const unsigned char *bytes, size_t size,
                           const char *needle, size_t needle_size) {
    if (needle_size > size) return false;
    for (size_t i = 0; i <= size - needle_size; ++i)
        if (!memcmp(bytes + i, needle, needle_size)) return true;
    return false;
}

static void make_workbook(rx::Workbook &book, uint32_t &second_id) {
    assert(book.init());
    rx::Sheet &first = book.sheets[0];
    assert(first.name.set("预算表", strlen("预算表")));
    assert(book.set_input(first.id, 0, 0, "商品", true));
    assert(book.set_input(first.id, 0, 1, "金额", true));
    assert(book.set_input(first.id, 1, 0, "苹果", true));
    assert(book.set_input(first.id, 1, 1, "12.5"));
    assert(book.set_input(first.id, 2, 0, "=\"总计\"", false));
    rx::Style style;
    style.bold = 1;
    style.background = 0xffe080;
    style.format = rx::NumberFormat::Currency;
    style.alignment = rx::Alignment::Right;
    assert(first.set_style(0, 0, style));
    assert(first.merges.push({0, 2, 0, 3}));
    assert(first.column_widths.resize(3));
    first.column_widths[1] = 142;
    assert(first.row_heights.resize(3));
    first.row_heights[2] = 31;
    assert(first.filters.push({0, rx::Text{}, 1}));
    assert(first.filters[0].query.set("苹果", strlen("苹果")));
    first.filter_range = {0, 0, 5, 2};
    first.filter_enabled = true;
    first.filter_header = true;
    first.freeze_row = true;
    first.freeze_column = true;
    first.gridlines = false;
    first.zoom = 125;
    first.cursor_row = 2;
    first.cursor_col = 1;
    first.scroll_row = 1;
    first.scroll_col = 1;

    rx::Sheet second;
    second.id = book.next_id++;
    second_id = second.id;
    assert(second.name.set("明细", strlen("明细")));
    assert(second.set_input(4, 3, "=2+3"));
    assert(second.set_input(5, 4, "other", true));
    assert(book.sheets.push(static_cast<rx::Sheet &&>(second)));
    book.active_id = second_id;
}

static void skip_bytes(size_t &at, size_t count, size_t size) {
    assert(count <= size - at);
    at += count;
}

static uint32_t read_u32(const unsigned char *bytes, size_t &at, size_t size) {
    assert(4 <= size - at);
    uint32_t value = (uint32_t)bytes[at] | ((uint32_t)bytes[at + 1] << 8) |
        ((uint32_t)bytes[at + 2] << 16) | ((uint32_t)bytes[at + 3] << 24);
    at += 4;
    return value;
}

static void skip_string(const unsigned char *bytes, size_t &at, size_t size) {
    uint32_t length = read_u32(bytes, at, size);
    skip_bytes(at, length, size);
}

// Locate the second sheet's second cell coordinate pair in the versioned
// stream so the corruption test exercises duplicate-coordinate validation.
static size_t second_cell_coordinates(const unsigned char *bytes, size_t size) {
    size_t at = 16;
    skip_bytes(at, 16, size); // next_id, active_id, revision
    assert(read_u32(bytes, at, size) == 2); // sheets
    for (unsigned sheet = 0; sheet < 2; ++sheet) {
        skip_bytes(at, 4, size); // id
        skip_string(bytes, at, size);
        skip_bytes(at, 4 * 5 + 5, size); // view and flags
        uint32_t columns = read_u32(bytes, at, size);
        skip_bytes(at, (size_t)columns * 4, size);
        uint32_t rows = read_u32(bytes, at, size);
        skip_bytes(at, (size_t)rows * 4, size);
        skip_bytes(at, 16, size); // filter range
        uint32_t filters = read_u32(bytes, at, size);
        for (uint32_t i = 0; i < filters; ++i) {
            skip_bytes(at, 5, size);
            skip_string(bytes, at, size);
        }
        uint32_t merges = read_u32(bytes, at, size);
        skip_bytes(at, (size_t)merges * 16, size);
        uint32_t cells = read_u32(bytes, at, size);
        for (uint32_t cell = 0; cell < cells; ++cell) {
            size_t coordinates = at;
            skip_bytes(at, 8 + 1 + 4 + 4 + 2 + 6, size);
            skip_string(bytes, at, size);
            if (sheet == 1 && cell == 1) return coordinates;
        }
    }
    assert(!"second cell record not found");
    return 0;
}

static size_t first_merge_coordinates(const unsigned char *bytes, size_t size) {
    size_t at = 16;
    skip_bytes(at, 16, size); // workbook metadata
    assert(read_u32(bytes, at, size) >= 1); // sheets
    skip_bytes(at, 4, size); // sheet id
    skip_string(bytes, at, size);
    skip_bytes(at, 4 * 5 + 5, size); // view and flags
    uint32_t columns = read_u32(bytes, at, size);
    skip_bytes(at, (size_t)columns * 4, size);
    uint32_t rows = read_u32(bytes, at, size);
    skip_bytes(at, (size_t)rows * 4, size);
    skip_bytes(at, 16, size); // filter range
    uint32_t filters = read_u32(bytes, at, size);
    for (uint32_t i = 0; i < filters; ++i) {
        skip_bytes(at, 5, size);
        skip_string(bytes, at, size);
    }
    assert(read_u32(bytes, at, size) > 0); // merged ranges
    return at;
}

static size_t first_cell_bold_style_byte(const unsigned char *bytes, size_t size) {
    size_t at = 16;
    skip_bytes(at, 16, size); // workbook metadata
    assert(read_u32(bytes, at, size) >= 1); // sheets
    skip_bytes(at, 4, size); // sheet id
    skip_string(bytes, at, size);
    skip_bytes(at, 4 * 5 + 5, size); // view and flags
    uint32_t columns = read_u32(bytes, at, size);
    skip_bytes(at, (size_t)columns * 4, size);
    uint32_t rows = read_u32(bytes, at, size);
    skip_bytes(at, (size_t)rows * 4, size);
    skip_bytes(at, 16, size); // filter range
    uint32_t filters = read_u32(bytes, at, size);
    for (uint32_t i = 0; i < filters; ++i) {
        skip_bytes(at, 5, size);
        skip_string(bytes, at, size);
    }
    uint32_t merges = read_u32(bytes, at, size);
    skip_bytes(at, (size_t)merges * 16, size);
    assert(read_u32(bytes, at, size) > 0); // cells
    return at + 4 + 4 + 1 + 4 + 4 + 2; // coordinates, literal, colors and font size
}

static void test_native_round_trip_and_corrupt_rejection() {
    rx::Workbook source;
    uint32_t second_id = 0;
    make_workbook(source, second_id);
    char native_path[1200];
    path(native_path, sizeof(native_path), "book.rexl");
    rx::Text error;
    progress_calls = 0;
    assert(rx::save_workbook(native_path, source, error, nullptr, observe_progress));
    assert(progress_calls != 0);

    unsigned char bytes[1024 * 1024];
    size_t size = read_bytes("book.rexl", bytes, sizeof(bytes));
    assert(size > 16 && !memcmp(bytes, "REXL", 4));
    assert(bytes[4] == 1 && bytes[5] == 0);
    uint64_t payload = 0;
    for (unsigned i = 0; i < 8; ++i) payload |= (uint64_t)bytes[8 + i] << (8 * i);
    assert(payload == size - 16);

    rx::Workbook restored;
    assert(rx::load_workbook(native_path, restored, error));
    assert(restored.sheets.size() == 2 && restored.active_id == second_id);
    const rx::Sheet *first = restored.sheet(source.sheets[0].id);
    assert(first && first->name == "预算表");
    assert(first->zoom == 125 && first->cursor_row == 2 && first->cursor_col == 1);
    assert(first->scroll_row == 1 && first->scroll_col == 1);
    assert(first->freeze_row && first->freeze_column && !first->gridlines);
    assert(first->filter_enabled && first->filter_header && first->filters.size() == 1);
    assert(first->filters[0].query == "苹果");
    assert(first->merges.size() == 1 && first->merges[0].r1 == 0 && first->merges[0].c2 == 3);
    assert(first->column_widths.size() == 3 && first->column_widths[1] == 142);
    assert(first->row_heights.size() == 3 && first->row_heights[2] == 31);
    assert(first->get(0, 0)->style == source.sheets[0].get(0, 0)->style);
    assert(first->get(2, 0)->input == "=\"总计\"");
    assert(restored.get(second_id, 4, 3)->input == "=2+3");
    assert(restored.get(second_id, 4, 3)->result.kind == rx::ValueKind::Number);
    assert(restored.get(second_id, 4, 3)->result.number == 5);

    char linked_native_path[1200];
    path(linked_native_path, sizeof(linked_native_path), "book-link.rexl");
    assert(symlink("book.rexl", linked_native_path) == 0);
    rx::Workbook linked;
    assert(rx::load_workbook(linked_native_path, linked, error));
    assert(linked.get(second_id, 4, 3)->input == "=2+3");

    rx::Workbook preserved;
    assert(preserved.init());
    assert(preserved.set_input(preserved.active_id, 7, 7, "keep"));
    const uint32_t preserved_id = preserved.active_id;
    unsigned char damaged[1024 * 1024];
    memcpy(damaged, bytes, size);
    damaged[4] = 2; // unsupported version
    write_bytes("unknown.rexl", damaged, size);
    char corrupt_path[1200];
    path(corrupt_path, sizeof(corrupt_path), "unknown.rexl");
    assert(!rx::load_workbook("", preserved, error)); // bad path is rejected
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    memcpy(damaged, bytes, size);
    size_t input = first_cell_bold_style_byte(damaged, size) + 10;
    damaged[input] = 0xc0; // invalid UTF-8 lead byte
    write_bytes("invalid-utf8.rexl", damaged, size);
    path(corrupt_path, sizeof(corrupt_path), "invalid-utf8.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    memcpy(damaged, bytes, size);
    size_t merge = first_merge_coordinates(damaged, size);
    damaged[merge + 8] = 0xff; // invalid merged range bottom row
    damaged[merge + 9] = 0xff;
    damaged[merge + 10] = 0xff;
    damaged[merge + 11] = 0x7f;
    write_bytes("invalid-merge.rexl", damaged, size);
    path(corrupt_path, sizeof(corrupt_path), "invalid-merge.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    memcpy(damaged, bytes, size);
    merge = first_merge_coordinates(damaged, size);
    uint32_t duplicate_merge_count = 2;
    damaged[merge - 4] = (unsigned char)duplicate_merge_count;
    damaged[merge - 3] = 0;
    damaged[merge - 2] = 0;
    damaged[merge - 1] = 0;
    memmove(damaged + merge + 16, damaged + merge, size - merge);
    memcpy(damaged + merge, bytes + merge, 16);
    uint64_t new_payload = size;
    for (unsigned i = 0; i < 8; ++i)
        damaged[8 + i] = (unsigned char)(new_payload >> (8 * i));
    write_bytes("overlapping-merges.rexl", damaged, size + 16);
    path(corrupt_path, sizeof(corrupt_path), "overlapping-merges.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    memcpy(damaged, bytes, size);
    damaged[first_cell_bold_style_byte(damaged, size)] = 2; // invalid style flag
    write_bytes("invalid-style.rexl", damaged, size);
    path(corrupt_path, sizeof(corrupt_path), "invalid-style.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    write_bytes("truncated.rexl", bytes, size - 1);
    path(corrupt_path, sizeof(corrupt_path), "truncated.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    memcpy(damaged, bytes, size);
    size_t duplicate = second_cell_coordinates(damaged, size);
    damaged[duplicate + 0] = 4; // duplicate the first second-sheet cell at D5
    damaged[duplicate + 1] = 0;
    damaged[duplicate + 2] = 0;
    damaged[duplicate + 3] = 0;
    damaged[duplicate + 4] = 3;
    damaged[duplicate + 5] = 0;
    damaged[duplicate + 6] = 0;
    damaged[duplicate + 7] = 0;
    write_bytes("duplicate.rexl", damaged, size);
    path(corrupt_path, sizeof(corrupt_path), "duplicate.rexl");
    assert(!rx::load_workbook(corrupt_path, preserved, error));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");

    assert(!rx::load_workbook(native_path, preserved, error, cancel_progress));
    assert(preserved.get(preserved_id, 7, 7)->input == "keep");
}

static void test_large_merge_validation_is_bounded() {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(5);
        rx::Workbook book;
        rx::Text error;
        if (!book.init()) _exit(1);
        rx::Sheet &sheet = book.sheets[0];
        for (int i = 0; i < 50000; ++i) {
            int row = i / 128;
            int column = (i % 128) * 2;
            if (!sheet.merges.push({row, column, row, column + 1})) _exit(2);
        }
        char target[1200];
        path(target, sizeof(target), "many-merges.rexl");
        bool saved = rx::save_workbook(target, book, error);
        _exit(saved ? 0 : 3);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_atomic_save_conflict_and_cancellation() {
    const char original[] = "old workbook";
    write_bytes("conflict.rexl", original, sizeof(original) - 1);
    char target[1200];
    path(target, sizeof(target), "conflict.rexl");
    rx::FileStamp stamp;
    rx::Text error;
    assert(rx::file_stamp(target, stamp, error) && stamp.exists);
    const char external[] = "external edit";
    write_bytes("conflict.rexl", external, sizeof(external) - 1);

    rx::Workbook book;
    assert(book.init());
    assert(book.set_input(book.active_id, 0, 0, "new"));
    assert(!rx::save_workbook(target, book, error, &stamp));
    assert(same_file("conflict.rexl", external, sizeof(external) - 1));

    write_bytes("cancel.rexl", original, sizeof(original) - 1);
    path(target, sizeof(target), "cancel.rexl");
    assert(!rx::save_workbook(target, book, error, nullptr, cancel_progress));
    assert(same_file("cancel.rexl", original, sizeof(original) - 1));
    assert(!rx::file_stamp("", stamp, error));

    char mode_target[1200];
    path(mode_target, sizeof(mode_target), "mode.rexl");
    assert(rx::save_workbook(mode_target, book, error));
    struct stat mode_stat;
    assert(stat(mode_target, &mode_stat) == 0 && (mode_stat.st_mode & 0777) == 0600);
    assert(chmod(mode_target, 0640) == 0);
    assert(rx::save_workbook(mode_target, book, error));
    assert(stat(mode_target, &mode_stat) == 0 && (mode_stat.st_mode & 0777) == 0640);

    const char partial_original[] = "preserve through partial write";
    write_bytes("cancel-partial.rexl", partial_original, sizeof(partial_original) - 1);
    path(target, sizeof(target), "cancel-partial.rexl");
    rx::Workbook large;
    assert(large.init());
    char cell_text[65];
    memset(cell_text, 'x', sizeof(cell_text) - 1);
    cell_text[sizeof(cell_text) - 1] = 0;
    for (int row = 0; row < 3000; ++row)
        assert(large.set_input(large.active_id, row, 0, cell_text));
    SaveProgressState partial_progress;
    assert(!rx::save_workbook(target, large, error, nullptr,
                              cancel_after_first_output_quantum, &partial_progress));
    assert(partial_progress.cancelled_after_write);
    assert(error == "Operation cancelled");
    assert(same_file("cancel-partial.rexl", partial_original, sizeof(partial_original) - 1));
    assert(!has_temporary_save("cancel-partial.rexl.tmp."));

    const char symlink_original[] = "keep symlink target";
    write_bytes("save-symlink-target.rexl", symlink_original, sizeof(symlink_original) - 1);
    char symlink_path[1200];
    path(symlink_path, sizeof(symlink_path), "save-symlink.rexl");
    assert(symlink("save-symlink-target.rexl", symlink_path) == 0);
    assert(!rx::save_workbook(symlink_path, book, error));
    assert(same_file("save-symlink-target.rexl", symlink_original, sizeof(symlink_original) - 1));
}

static void test_external_edit_with_restored_mtime_is_a_conflict() {
    const char original[] = "original data";
    const char external[] = "external data";
    assert(sizeof(original) == sizeof(external));
    write_bytes("same-stamp.rexl", original, sizeof(original) - 1);
    char target[1200];
    path(target, sizeof(target), "same-stamp.rexl");
    struct stat original_stat;
    assert(stat(target, &original_stat) == 0);

    rx::FileStamp expected;
    rx::Text error;
    assert(rx::file_stamp(target, expected, error) && expected.exists);
    int fd = open(target, O_WRONLY | O_CLOEXEC);
    assert(fd >= 0);
    assert(write(fd, external, sizeof(external) - 1) == (ssize_t)(sizeof(external) - 1));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);

    struct timespec times[2];
#if defined(__APPLE__)
    times[0] = original_stat.st_atimespec;
    times[1] = original_stat.st_mtimespec;
#else
    times[0] = original_stat.st_atim;
    times[1] = original_stat.st_mtim;
#endif
    assert(utimensat(AT_FDCWD, target, times, 0) == 0);

    rx::Workbook book;
    assert(book.init());
    assert(book.set_input(book.active_id, 0, 0, "new workbook"));
    assert(!rx::save_workbook(target, book, error, &expected));
    assert(same_file("same-stamp.rexl", external, sizeof(external) - 1));
}

static void test_csv_export_preserves_external_edit_during_save() {
    const char original[] = "original file";
    const char external[] = "external edit";
    assert(sizeof(original) == sizeof(external));
    write_bytes("csv-race.csv", original, sizeof(original) - 1);
    char target[1200];
    path(target, sizeof(target), "csv-race.csv");

    rx::Workbook book;
    assert(book.init());
    assert(book.set_input(book.active_id, 0, 0, "new export", true));
    rx::Text error;
    rx::FileStamp expected;
    assert(rx::file_stamp(target, expected, error) && expected.exists);
    CsvConflictState conflict{target};
    assert(!rx::export_csv(target, book, book.active_id, error, true,
                           edit_csv_target_after_stamp_check, &conflict, &expected));
    assert(conflict.changed);
    assert(same_file("csv-race.csv", external, sizeof(external) - 1));
    assert(!has_temporary_save("csv-race.csv.tmp."));
}

static void test_csv_import_quotes_numeric_and_formula_safety() {
    const unsigned char csv[] = {
        0xef, 0xbb, 0xbf,
        'N','a','m','e',',','A','m','o','u','n','t',',','I','n','p','u','t',',','N','o','t','e','\r','\n',
        '"','A','l','p','h','a',',',' ','b','e','t','a','"',',','0','0','1','2','3',',','=', '2','+','2',',',
        '"','l','i','n','e',' ','1','\r','\n','l','i','n','e',' ','2','"','\r','\n',
        'O','k',',','4','2',',','1','2','a','b','c',',','\r','\n'
    };
    write_bytes("import.csv", csv, sizeof(csv));
    char csv_path[1200];
    path(csv_path, sizeof(csv_path), "import.csv");
    char linked_csv_path[1200];
    path(linked_csv_path, sizeof(linked_csv_path), "import-link.csv");
    assert(symlink("import.csv", linked_csv_path) == 0);
    rx::Workbook output;
    assert(output.init());
    assert(output.set_input(output.active_id, 9, 9, "preserve"));
    const uint32_t old_id = output.active_id;
    rx::Text error;
    assert(rx::import_csv(linked_csv_path, output, error));
    assert(output.sheets.size() == 1 && output.get(old_id, 9, 9) == nullptr);
    const rx::Sheet &sheet = output.sheets[0];
    assert(sheet.get(1, 0)->input == "Alpha, beta" && sheet.get(1, 0)->literal);
    assert(sheet.get(1, 1)->input == "00123" && sheet.get(1, 1)->literal);
    assert(sheet.get(1, 2)->input == "=2+2" && sheet.get(1, 2)->literal);
    assert(rx::evaluate(output, old_id, 1, 2).kind == rx::ValueKind::Text);
    assert(sheet.get(1, 3)->input == "line 1\r\nline 2");
    assert(sheet.get(2, 1)->input == "42" && !sheet.get(2, 1)->literal);
    assert(sheet.get(2, 2)->input == "12abc" && sheet.get(2, 2)->literal);

    assert(!rx::import_csv(csv_path, output, error, ',', cancel_progress));
    assert(output.get(output.active_id, 1, 2)->input == "=2+2");
}

static void test_csv_export_values_and_tsv_codec() {
    rx::Workbook book;
    assert(book.init());
    const uint32_t id = book.active_id;
    assert(book.set_input(id, 0, 0, "=1+2"));
    assert(book.set_input(id, 0, 1, "quote \" and, comma\nsecond line", true));
    assert(book.set_input(id, 1, 0, "=RUN()", true));
    assert(book.set_input(id, 1, 1, "0012", true));
    rx::Text error;
    char csv_path[1200];
    path(csv_path, sizeof(csv_path), "export.csv");
    assert(rx::export_csv(csv_path, book, id, error, true));
    unsigned char bytes[4096];
    size_t size = read_bytes("export.csv", bytes, sizeof(bytes));
    assert(size > 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf);
    const char first_line[] = "3,\"quote \"\" and, comma\nsecond line\"\r\n";
    const char second_line[] = "=RUN(),0012\r\n";
    assert(contains_bytes(bytes, size, first_line, sizeof(first_line) - 1));
    assert(contains_bytes(bytes, size, second_line, sizeof(second_line) - 1));
    const char old_export[] = "preserve existing export";
    write_bytes("export-cancel.csv", old_export, sizeof(old_export) - 1);
    char cancel_path[1200];
    path(cancel_path, sizeof(cancel_path), "export-cancel.csv");
    assert(!rx::export_csv(cancel_path, book, id, error, true, cancel_progress));
    assert(same_file("export-cancel.csv", old_export, sizeof(old_export) - 1));

    rx::Workbook tsv;
    assert(rx::decode_delimited("a\t\"b\tc\"\r\n\"line1\nline2\"\t001\r\n",
        strlen("a\t\"b\tc\"\r\n\"line1\nline2\"\t001\r\n"), tsv, error, '\t'));
    assert(tsv.get(tsv.active_id, 0, 1)->input == "b\tc");
    assert(tsv.get(tsv.active_id, 1, 0)->input == "line1\nline2");
    assert(tsv.get(tsv.active_id, 1, 1)->input == "001" && tsv.get(tsv.active_id, 1, 1)->literal);
    rx::Text encoded;
    assert(rx::encode_delimited(tsv, tsv.active_id, {0, 0, 1, 1}, encoded, error, '\t'));
    rx::Workbook round_trip;
    assert(rx::decode_delimited(encoded.c_str(), encoded.size(), round_trip, error, '\t'));
    assert(round_trip.get(round_trip.active_id, 0, 1)->input == "b\tc");
    assert(round_trip.get(round_trip.active_id, 1, 0)->input == "line1\nline2");
}

static void test_csv_malformed_utf8_and_resource_limit_leave_target() {
    rx::Workbook output;
    assert(output.init());
    const uint32_t id = output.active_id;
    assert(output.set_input(id, 0, 0, "keep"));
    rx::Text error;
    const char malformed[] = {'a', ',', char(0xc0), char(0xaf)};
    assert(!rx::decode_delimited(malformed, sizeof(malformed), output, error));
    assert(output.get(id, 0, 0)->input == "keep");

    const char malformed_quote[] = "a,\"unterminated";
    assert(!rx::decode_delimited(malformed_quote, sizeof(malformed_quote) - 1, output, error));
    assert(output.get(id, 0, 0)->input == "keep");
}

static void test_csv_parser_can_cancel_inside_a_large_record() {
    const size_t field_count = 64;
    const size_t field_size = 16 * 1024;
    const size_t length = field_count * field_size + field_count - 1;
    char *csv = (char *)malloc(length);
    assert(csv);
    size_t offset = 0;
    for (size_t field = 0; field < field_count; ++field) {
        memset(csv + offset, 'x', field_size);
        offset += field_size;
        if (field + 1 != field_count) csv[offset++] = ',';
    }
    assert(offset == length);
    write_bytes("large-record.csv", csv, length);
    free(csv);

    char csv_path[1200];
    path(csv_path, sizeof(csv_path), "large-record.csv");
    rx::Workbook output;
    assert(output.init());
    const uint32_t id = output.active_id;
    assert(output.set_input(id, 4, 4, "preserve"));
    rx::Text error;
    ParseProgressState progress;
    assert(!rx::import_csv(csv_path, output, error, ',', cancel_during_csv_parse, &progress));
    assert(progress.cancelled_during_parse);
    assert(error == "Operation cancelled");
    assert(output.get(id, 4, 4)->input == "preserve");
}

static void test_recovery_directory_uses_private_xdg_state() {
    char state[1200];
    path(state, sizeof(state), "state");
    assert(mkdir(state, 0700) == 0);
    char app[1400], recovery[1400];
    int n = snprintf(app, sizeof(app), "%s/reliefexcel", state);
    assert(n > 0 && (size_t)n < sizeof(app));
    n = snprintf(recovery, sizeof(recovery), "%s/recovery", app);
    assert(n > 0 && (size_t)n < sizeof(recovery));
    assert(mkdir(app, 0755) == 0);
    assert(chmod(app, 0755) == 0);
    assert(mkdir(recovery, 0755) == 0);
    assert(chmod(recovery, 0755) == 0);
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);
    rx::Text directory;
    rx::Text error;
    assert(rx::recovery_directory(directory, error));
    char expected[1400];
    n = snprintf(expected, sizeof(expected), "%s/reliefexcel/recovery", state);
    assert(n > 0 && (size_t)n < sizeof(expected));
    assert(directory == expected);
    struct stat st;
    assert(stat(directory.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    assert((st.st_mode & 0777) == 0700);
    assert(stat(app, &st) == 0 && (st.st_mode & 0777) == 0700);

    char linked_state[1200], external[1200], linked_app[1400];
    path(linked_state, sizeof(linked_state), "state-with-link");
    path(external, sizeof(external), "external-recovery-target");
    assert(mkdir(linked_state, 0700) == 0);
    assert(mkdir(external, 0700) == 0);
    n = snprintf(linked_app, sizeof(linked_app), "%s/reliefexcel", linked_state);
    assert(n > 0 && (size_t)n < sizeof(linked_app));
    assert(symlink(external, linked_app) == 0);
    assert(setenv("XDG_STATE_HOME", linked_state, 1) == 0);
    assert(!rx::recovery_directory(directory, error));
    assert(stat(external, &st) == 0 && S_ISDIR(st.st_mode));
    char external_child[1400];
    n = snprintf(external_child, sizeof(external_child), "%s/recovery", external);
    assert(n > 0 && (size_t)n < sizeof(external_child));
    assert(access(external_child, F_OK) < 0 && errno == ENOENT);
}

static void test_recovery_reservations_are_unique_and_private() {
    char state[1200];
    path(state, sizeof(state), "reservation-state");
    assert(mkdir(state, 0700) == 0);
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);

    rx::Text directory, error;
    assert(rx::recovery_directory(directory, error));
    char legacy_path[1400];
    int n = snprintf(legacy_path, sizeof(legacy_path), "%s/session-123.rexl", directory.c_str());
    assert(n > 0 && (size_t)n < sizeof(legacy_path));
    const char legacy[] = "keep older recovery";
    FILE *legacy_file = fopen(legacy_path, "wb");
    assert(legacy_file);
    assert(fwrite(legacy, 1, sizeof(legacy) - 1, legacy_file) == sizeof(legacy) - 1);
    assert(fclose(legacy_file) == 0);

    rx::Text first, second;
    assert(rx::reserve_recovery_path(first, error));
    assert(rx::reserve_recovery_path(second, error));
    assert(first != second);
    assert(first.size() >= 5 && !strcmp(first.c_str() + first.size() - 5, ".rexl"));
    assert(second.size() >= 5 && !strcmp(second.c_str() + second.size() - 5, ".rexl"));

    struct stat first_stat, second_stat;
    assert(stat(first.c_str(), &first_stat) == 0 && S_ISREG(first_stat.st_mode));
    assert(stat(second.c_str(), &second_stat) == 0 && S_ISREG(second_stat.st_mode));
    assert((first_stat.st_mode & 0777) == 0600 && (second_stat.st_mode & 0777) == 0600);
    assert(first_stat.st_uid == geteuid() && second_stat.st_uid == geteuid());
    assert(first_stat.st_size == 0 && second_stat.st_size == 0);
    DIR *recovery_dir = opendir(directory.c_str());
    assert(recovery_dir);
    unsigned recovery_files = 0;
    bool staging_file_left = false;
    struct dirent *recovery_entry;
    while ((recovery_entry = readdir(recovery_dir))) {
        if (!strcmp(recovery_entry->d_name, ".") || !strcmp(recovery_entry->d_name, "..")) continue;
        size_t name_length = strlen(recovery_entry->d_name);
        if (name_length >= 5 && !strcmp(recovery_entry->d_name + name_length - 5, ".rexl"))
            ++recovery_files;
        else staging_file_left = true;
    }
    assert(closedir(recovery_dir) == 0);
    assert(recovery_files == 3 && !staging_file_left);

    rx::Workbook first_book, second_book;
    assert(first_book.init() && second_book.init());
    assert(first_book.set_input(first_book.active_id, 0, 0, "first session"));
    assert(second_book.set_input(second_book.active_id, 0, 0, "second session"));
    assert(rx::save_workbook(first.c_str(), first_book, error));
    assert(rx::save_workbook(second.c_str(), second_book, error));
    rx::Workbook first_loaded, second_loaded;
    assert(rx::load_workbook(first.c_str(), first_loaded, error));
    assert(rx::load_workbook(second.c_str(), second_loaded, error));
    assert(first_loaded.get(first_loaded.active_id, 0, 0)->input == "first session");
    assert(second_loaded.get(second_loaded.active_id, 0, 0)->input == "second session");

    char preserved[128];
    FILE *check_legacy = fopen(legacy_path, "rb");
    assert(check_legacy);
    assert(fread(preserved, 1, sizeof(legacy) - 1, check_legacy) == sizeof(legacy) - 1);
    assert(fclose(check_legacy) == 0);
    assert(!memcmp(preserved, legacy, sizeof(legacy) - 1));
}

static void child_checks_recovery_lock(const char *recovery_path, bool should_lock) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int descriptor = -1;
        rx::Text error;
        bool locked = rx::lock_recovery_path(recovery_path, descriptor, error);
        bool ok = locked == should_lock && (locked ? descriptor >= 0 : descriptor == -1);
        if (locked) rx::unlock_recovery_path(descriptor);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_recovery_lock_is_exclusive_and_refuses_unsafe_sidecars() {
    char recovery_path[1200], lock_path[1200];
    path(recovery_path, sizeof(recovery_path), "locked-session.rexl");
    int n = snprintf(lock_path, sizeof(lock_path), "%s.lock", recovery_path);
    assert(n > 0 && (size_t)n < sizeof(lock_path));
    write_bytes("locked-session.rexl", "recovery", sizeof("recovery") - 1);

    int descriptor = -1;
    rx::Text error;
    assert(rx::lock_recovery_path(recovery_path, descriptor, error));
    assert(descriptor >= 0 && error.empty());
    struct stat lock_stat;
    assert(lstat(lock_path, &lock_stat) == 0 && S_ISREG(lock_stat.st_mode));
    assert(lock_stat.st_uid == geteuid() && (lock_stat.st_mode & 0777) == 0600);
    child_checks_recovery_lock(recovery_path, false);
    rx::unlock_recovery_path(descriptor);
    assert(descriptor == -1);
    child_checks_recovery_lock(recovery_path, true);

    const char malformed[] = "preserve malformed lock";
    write_bytes("malformed-session.rexl.lock", malformed, sizeof(malformed) - 1);
    char malformed_recovery[1200];
    path(malformed_recovery, sizeof(malformed_recovery), "malformed-session.rexl");
    char malformed_lock[1200];
    n = snprintf(malformed_lock, sizeof(malformed_lock), "%s.lock", malformed_recovery);
    assert(n > 0 && (size_t)n < sizeof(malformed_lock));
    assert(chmod(malformed_lock, 0644) == 0);
    descriptor = -1;
    assert(!rx::lock_recovery_path(malformed_recovery, descriptor, error));
    assert(descriptor == -1 && !error.empty());
    struct stat malformed_stat;
    assert(lstat(malformed_lock, &malformed_stat) == 0 && S_ISREG(malformed_stat.st_mode));
    assert((malformed_stat.st_mode & 0777) == 0644);
    assert(same_file("malformed-session.rexl.lock", malformed, sizeof(malformed) - 1));

    write_bytes("symlink-lock-target", "preserve target", sizeof("preserve target") - 1);
    char symlink_recovery[1200], symlink_lock[1200], symlink_target[1200];
    path(symlink_recovery, sizeof(symlink_recovery), "symlink-session.rexl");
    n = snprintf(symlink_lock, sizeof(symlink_lock), "%s.lock", symlink_recovery);
    assert(n > 0 && (size_t)n < sizeof(symlink_lock));
    path(symlink_target, sizeof(symlink_target), "symlink-lock-target");
    assert(symlink(symlink_target, symlink_lock) == 0);
    descriptor = -1;
    assert(!rx::lock_recovery_path(symlink_recovery, descriptor, error));
    assert(descriptor == -1 && !error.empty());
    struct stat symlink_stat;
    assert(lstat(symlink_lock, &symlink_stat) == 0 && S_ISLNK(symlink_stat.st_mode));
    assert(same_file("symlink-lock-target", "preserve target", sizeof("preserve target") - 1));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(strlen(argv[1]) < sizeof(test_root));
    strcpy(test_root, argv[1]);
    test_native_round_trip_and_corrupt_rejection();
    test_large_merge_validation_is_bounded();
    test_atomic_save_conflict_and_cancellation();
    test_external_edit_with_restored_mtime_is_a_conflict();
    test_csv_export_preserves_external_edit_during_save();
    test_csv_import_quotes_numeric_and_formula_safety();
    test_csv_export_values_and_tsv_codec();
    test_csv_malformed_utf8_and_resource_limit_leave_target();
    test_csv_parser_can_cancel_inside_a_large_record();
    test_recovery_directory_uses_private_xdg_state();
    test_recovery_reservations_are_unique_and_private();
    test_recovery_lock_is_exclusive_and_refuses_unsafe_sidecars();
    return 0;
}
