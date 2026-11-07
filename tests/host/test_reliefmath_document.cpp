#include "document.h"
#include "file_io.h"
#include "recovery.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

static_assert(rm::kMaxDocumentBytes == 16u * 1024u * 1024u,
              "the .rmath document limit must stay at 16 MiB");
static_assert(rm::kMaxAtomicFileBytes >= 64u * 1024u * 1024u,
              "raw atomic storage must fit one maximum protocol artifact");

char test_root[1200];

struct DirectorySyncFault {
    char path[1600]{};
    char entry_path[1600]{};
    int error_number = 0;
    unsigned interrupts = 0;
    unsigned calls = 0;
};

DirectorySyncFault directory_sync_fault;

void make_path(char *out, size_t capacity, const char *name) {
    int count = snprintf(out, capacity, "%s/%s", test_root, name);
    assert(count > 0 && static_cast<size_t>(count) < capacity);
}

void make_document(rm::Document &document, const char *source = "x+y") {
    rm::Error error;
    rm::FormulaId id = 0;
    assert(document.title.set("面积文档", strlen("面积文档")));
    assert(rm::add_formula(document, "中文公式", source, id, error));
    assert(id == 1);
    rm::Style style = document.formulas[0].style;
    style.rgba = 0x11223344;
    style.point_size = 0x0123;
    style.padding = 0x0456;
    style.display = true;
    assert(rm::set_formula_style(document, id, style, error));
}

void write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const uint8_t *at = static_cast<const uint8_t *>(data);
    size_t remaining = size;
    while (remaining) {
        ssize_t count = write(fd, at, remaining);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        at += static_cast<size_t>(count);
        remaining -= static_cast<size_t>(count);
    }
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}

size_t read_file(const char *path, uint8_t *out, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size >= 0);
    assert(static_cast<uint64_t>(st.st_size) <= capacity);
    size_t expected = static_cast<size_t>(st.st_size);
    size_t offset = 0;
    while (offset < expected) {
        ssize_t count = read(fd, out + offset, expected - offset);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        offset += static_cast<size_t>(count);
    }
    assert(close(fd) == 0);
    return expected;
}

bool file_is(const char *path, const void *expected, size_t expected_size) {
    uint8_t bytes[4096];
    size_t actual = read_file(path, bytes, sizeof(bytes));
    return actual == expected_size && memcmp(bytes, expected, expected_size) == 0;
}

uint32_t read_u32(const uint8_t *bytes, size_t at) {
    return static_cast<uint32_t>(bytes[at]) |
        (static_cast<uint32_t>(bytes[at + 1]) << 8) |
        (static_cast<uint32_t>(bytes[at + 2]) << 16) |
        (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

uint64_t read_u64(const uint8_t *bytes, size_t at) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 8; ++shift)
        value |= static_cast<uint64_t>(bytes[at + shift]) << (shift * 8);
    return value;
}

void write_u32(uint8_t *bytes, size_t at, uint32_t value) {
    for (unsigned shift = 0; shift < 4; ++shift)
        bytes[at + shift] = static_cast<uint8_t>(value >> (shift * 8));
}

void write_u64(uint8_t *bytes, size_t at, uint64_t value) {
    for (unsigned shift = 0; shift < 8; ++shift)
        bytes[at + shift] = static_cast<uint8_t>(value >> (shift * 8));
}

bool same_basic_document(const rm::Document &a, const rm::Document &b) {
    return rm::same_document_content(a, b) && a.revision == b.revision &&
        a.formulas.size() == b.formulas.size();
}

void test_v1_exact_bytes_and_round_trip() {
    rm::Document original;
    make_document(original, "x");
    original.title.clear();
    original.formulas[0].name.set("N", 1);
    original.formulas[0].id = UINT64_C(0x0102030405060708);
    original.formulas[0].style.rgba = UINT32_C(0x11223344);
    original.formulas[0].style.point_size = UINT16_C(0x0123);
    original.formulas[0].style.padding = UINT16_C(0x0456);
    original.formulas[0].style.display = true;
    original.next_id = original.formulas[0].id + 1;

    const uint8_t expected[] = {
        'R','M','A','T','H','\r','\n',0,
        1,0,0,0, 66,0,0,0, 1,0,0,0, 0,0,0,0,
        9,7,6,5,4,3,2,1,
        8,7,6,5,4,3,2,1,
        1,0,0,0, 1,0,0,0,
        0x44,0x33,0x22,0x11, 0x23,0x01, 0x56,0x04, 1,
        0,0,0,0,0,0,0,
        'N','x'
    };
    rm::Error error;
    rm::Bytes encoded;
    assert(rm::encode_document(original, encoded, error));
    assert(encoded.size() == sizeof(expected));
    assert(memcmp(encoded.data(), expected, sizeof(expected)) == 0);
    assert(read_u32(encoded.data(), 12) == sizeof(expected));
    assert(read_u64(encoded.data(), 24) == original.next_id);

    rm::Document loaded;
    assert(rm::decode_document(encoded.data(), encoded.size(), loaded, error));
    assert(loaded.formulas.size() == 1);
    assert(loaded.formulas[0].id == original.formulas[0].id);
    assert(loaded.formulas[0].name == "N" && loaded.formulas[0].source == "x");
    assert(loaded.formulas[0].style.rgba == original.formulas[0].style.rgba);
    assert(loaded.formulas[0].style.point_size == original.formulas[0].style.point_size);
    assert(loaded.formulas[0].style.padding == original.formulas[0].style.padding);
    assert(loaded.formulas[0].style.display == original.formulas[0].style.display);
    assert(loaded.formulas[0].generation == 1 && loaded.revision == 0);

    rm::Document unicode;
    make_document(unicode, "\\text{面积}=\\pi r^2");
    assert(rm::encode_document(unicode, encoded, error));
    rm::Document unicode_loaded;
    assert(rm::decode_document(encoded.data(), encoded.size(), unicode_loaded, error));
    assert(unicode_loaded.formulas[0].name == "中文公式");
    assert(unicode_loaded.formulas[0].source == unicode.formulas[0].source);
}

void expect_bad_bytes_preserve(const uint8_t *bytes, size_t size,
                               const rm::Document &sentinel) {
    rm::Document output;
    rm::Error error;
    assert(rm::clone_document(sentinel, output, error));
    rm::Document before;
    assert(rm::clone_document(output, before, error));
    assert(!rm::decode_document(bytes, size, output, error));
    assert(same_basic_document(output, before));
}

void test_all_truncations_and_malformed_fields_preserve_document() {
    rm::Document source;
    make_document(source);
    rm::Document sentinel;
    make_document(sentinel, "keep this document");
    rm::Error error;
    rm::Bytes encoded;
    assert(rm::encode_document(source, encoded, error));

    for (size_t cut = 0; cut < encoded.size(); ++cut)
        expect_bad_bytes_preserve(encoded.data(), cut, sentinel);

    uint8_t *damaged = static_cast<uint8_t *>(malloc(encoded.size() + 1));
    assert(damaged);
    memcpy(damaged, encoded.data(), encoded.size());
    damaged[8] = 2;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    write_u32(damaged, 12, static_cast<uint32_t>(encoded.size() - 1));
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    write_u32(damaged, 16, UINT32_MAX);
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    write_u32(damaged, 20, UINT32_MAX);
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    const size_t record = 32 + source.title.size();
    memcpy(damaged, encoded.data(), encoded.size());
    damaged[32] = 0xff;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    damaged[record + 32] = 0xff;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    damaged[record + 32 + source.formulas[0].name.size()] = 0xff;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    damaged[record + 25] = 1;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    damaged[record + 24] = 2;
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    write_u64(damaged, record, 0);
    expect_bad_bytes_preserve(damaged, encoded.size(), sentinel);

    memcpy(damaged, encoded.data(), encoded.size());
    damaged[encoded.size()] = 0;
    expect_bad_bytes_preserve(damaged, encoded.size() + 1, sentinel);

    uint8_t *oversized = static_cast<uint8_t *>(calloc(16U * 1024U * 1024U + 1, 1));
    assert(oversized);
    expect_bad_bytes_preserve(oversized, 16U * 1024U * 1024U + 1, sentinel);
    free(oversized);
    free(damaged);
}

void test_duplicate_ids_are_rejected() {
    rm::Document source;
    rm::Error error;
    rm::FormulaId first = 0, second = 0;
    assert(rm::add_formula(source, "one", "1", first, error));
    assert(rm::add_formula(source, "two", "2", second, error));
    rm::Bytes encoded;
    assert(rm::encode_document(source, encoded, error));
    size_t second_record = 32 + source.title.size() + 32 +
        source.formulas[0].name.size() + source.formulas[0].source.size();
    write_u64(encoded.data(), second_record, first);
    rm::Document destination;
    make_document(destination, "preserved");
    rm::Document before;
    assert(rm::clone_document(destination, before, error));
    assert(!rm::decode_document(encoded.data(), encoded.size(), destination, error));
    assert(same_basic_document(destination, before));
}

void make_boundary_document(rm::Document &document, size_t last_source_size) {
    const size_t common_source_size = 65503;
    char *source = static_cast<char *>(malloc(65537));
    assert(source);
    memset(source, 'x', 65536);
    source[65536] = 0;
    rm::Error error;
    for (size_t index = 0; index < 256; ++index) {
        source[index == 255 ? last_source_size : common_source_size] = 0;
        rm::FormulaId id = 0;
        assert(rm::add_formula(document, "x", source, id, error));
        assert(id == index + 1);
        source[index == 255 ? last_source_size : common_source_size] = 'x';
    }
    free(source);
}

void test_exact_16_mib_boundary_and_allocation_failure_are_atomic() {
    rm::Document exact;
    make_boundary_document(exact, 65471);
    rm::Bytes encoded;
    rm::Error error;
    assert(rm::encode_document(exact, encoded, error));
    assert(encoded.size() == 16U * 1024U * 1024U);
    rm::Document decoded;
    assert(rm::decode_document(encoded.data(), encoded.size(), decoded, error));
    assert(decoded.formulas.size() == 256);

    rm::Document oversized;
    make_boundary_document(oversized, 65472);
    rm::Bytes preserved;
    const uint8_t marker[] = {0xde, 0xad, 0xbe, 0xef};
    assert(preserved.append(marker, sizeof(marker)));
    assert(!rm::encode_document(oversized, preserved, error));
    assert(error.code == rm::Code::Limit);
    assert(preserved.size() == sizeof(marker));
    assert(memcmp(preserved.data(), marker, sizeof(marker)) == 0);

    rm::Document small;
    make_document(small);
    rm::Bytes keep;
    assert(keep.append(marker, sizeof(marker)));
    rm::testing::fail_allocations_after(0);
    assert(!rm::encode_document(small, keep, error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    assert(keep.size() == sizeof(marker) && memcmp(keep.data(), marker, sizeof(marker)) == 0);
}

void test_file_round_trip_conflict_and_symlink_rejection() {
    char path[1400], link_path[1400];
    make_path(path, sizeof(path), "formula file with spaces.rmath");
    make_path(link_path, sizeof(link_path), "formula-link.rmath");
    rm::Document original;
    make_document(original, "\\text{中文} + x");
    rm::FileStamp absent{}, saved{};
    rm::Error error;
    assert(rm::atomic_save(path, original, absent, saved, error));
    assert(saved.exists && saved.size > 32);
    rm::FileStamp measured;
    assert(rm::file_stamp(path, measured, error));
    assert(rm::same_stamp(saved, measured));

    rm::Document loaded;
    rm::FileStamp loaded_stamp;
    assert(rm::read_document(path, loaded, loaded_stamp, error));
    assert(rm::same_stamp(saved, loaded_stamp));
    assert(rm::same_document_content(original, loaded));
    assert(loaded.revision == 0);

    rm::FormulaId id = loaded.formulas[0].id;
    assert(rm::set_formula_source(loaded, id, "external baseline", error));
    rm::FileStamp changed_stamp;
    assert(rm::atomic_save(path, loaded, saved, changed_stamp, error));
    assert(!rm::same_stamp(saved, changed_stamp));

    rm::Document candidate;
    make_document(candidate, "must not erase external edit");
    const char external[] = "outside edit";
    write_file(path, external, sizeof(external) - 1);
    assert(!rm::atomic_save(path, candidate, changed_stamp, saved, error));
    assert(error.code == rm::Code::Conflict);
    assert(file_is(path, external, sizeof(external) - 1));

    assert(symlink(path, link_path) == 0);
    rm::FileStamp link_stamp;
    assert(!rm::file_stamp(link_path, link_stamp, error));
    assert(!rm::read_document(link_path, loaded, loaded_stamp, error));
    assert(!rm::atomic_save(link_path, candidate, changed_stamp, saved, error));
    assert(file_is(path, external, sizeof(external) - 1));
    assert(unlink(link_path) == 0);
}

void test_raw_atomic_limit_is_independent_of_document_limit() {
    char path[1400];
    make_path(path, sizeof(path), "oversize raw artifact.bin");
    uint8_t byte = 0;
    rm::FileStamp absent{};
    rm::FileStamp output{};
    rm::Error error;
    assert(rm::kMaxAtomicFileBytes == 128u * 1024u * 1024u);
    assert(!rm::atomic_save_bytes(path, &byte, rm::kMaxAtomicFileBytes + 1,
                                  absent, output, error));
    assert(error.code == rm::Code::Limit);
    assert(access(path, F_OK) != 0);
}

bool has_temp_file(const char *directory) {
    DIR *dir = opendir(directory);
    assert(dir);
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (!strncmp(entry->d_name, ".rmath-tmp.", 11)) found = true;
    assert(closedir(dir) == 0);
    return found;
}

void test_short_writes_eintr_and_pre_rename_failures_preserve_target() {
    char path[1400];
    make_path(path, sizeof(path), "fault target.rmath");
    const char original[] = "original bytes";
    write_file(path, original, sizeof(original) - 1);
    rm::FileStamp expected;
    rm::Error error;
    assert(rm::file_stamp(path, expected, error));
    rm::Document document;
    make_document(document, "replacement content");
    rm::FileStamp after;

    rm::testing::IoTestControl control{};
    control.max_write_size = 3;
    control.write_eintr_count = 2;
    rm::testing::set_io_test_control(control);
    assert(rm::atomic_save(path, document, expected, after, error));
    rm::testing::reset_io_test_control();
    assert(!file_is(path, original, sizeof(original) - 1));
    assert(!has_temp_file(test_root));

    uint8_t current[4096];
    size_t current_size = read_file(path, current, sizeof(current));
    rm::FileStamp current_stamp;
    assert(rm::file_stamp(path, current_stamp, error));

    control = rm::testing::IoTestControl{};
    control.fail_write_after = 0;
    control.fail_write_errno = ENOSPC;
    rm::testing::set_io_test_control(control);
    assert(!rm::atomic_save(path, document, current_stamp, after, error));
    rm::testing::reset_io_test_control();
    assert(file_is(path, current, current_size));
    assert(!has_temp_file(test_root));

    control = rm::testing::IoTestControl{};
    control.fail_file_sync_errno = EIO;
    rm::testing::set_io_test_control(control);
    assert(!rm::atomic_save(path, document, current_stamp, after, error));
    rm::testing::reset_io_test_control();
    assert(file_is(path, current, current_size));
    assert(!has_temp_file(test_root));

    control = rm::testing::IoTestControl{};
    control.fail_rename_errno = EIO;
    rm::testing::set_io_test_control(control);
    assert(!rm::atomic_save(path, document, current_stamp, after, error));
    rm::testing::reset_io_test_control();
    assert(file_is(path, current, current_size));
    assert(!has_temp_file(test_root));

    control = rm::testing::IoTestControl{};
    control.fail_directory_sync_errno = EIO;
    rm::testing::set_io_test_control(control);
    assert(!rm::atomic_save(path, document, current_stamp, after, error));
    rm::testing::reset_io_test_control();
    assert(error.code == rm::Code::Io);
    assert(strstr(error.message.c_str(), "replaced") != nullptr);
    rm::Document durable_or_not;
    rm::FileStamp final_stamp;
    assert(rm::read_document(path, durable_or_not, final_stamp, error));
    assert(rm::same_document_content(document, durable_or_not));
    assert(!has_temp_file(test_root));
}

void test_bad_file_load_keeps_document_and_stamp() {
    char path[1400];
    make_path(path, sizeof(path), "malformed.rmath");
    const uint8_t bad[] = {'R','M','A','T','H','\r','\n',0,1};
    write_file(path, bad, sizeof(bad));
    rm::Document output;
    make_document(output, "keep original state");
    rm::Document before;
    rm::Error error;
    assert(rm::clone_document(output, before, error));
    rm::FileStamp stamp{};
    stamp.exists = true;
    stamp.content_digest = UINT64_C(0x123456789abcdef0);
    const rm::FileStamp old_stamp = stamp;
    assert(!rm::read_document(path, output, stamp, error));
    assert(same_basic_document(output, before));
    assert(stamp.exists == old_stamp.exists &&
           stamp.content_digest == old_stamp.content_digest);

    rm::testing::IoTestControl control{};
    control.fail_read_open_errno = EACCES;
    rm::testing::set_io_test_control(control);
    assert(!rm::read_document(path, output, stamp, error));
    rm::testing::reset_io_test_control();
    assert(error.code == rm::Code::Io);
    assert(same_basic_document(output, before));
}

struct ProgressState {
    size_t callbacks = 0;
    rm::IoPhase last_phase = rm::IoPhase::Read;
    size_t last_completed = 0;
    bool cancel_after_write = false;
};

bool cancel_when_write_progressed(void *context) {
    ProgressState *state = static_cast<ProgressState *>(context);
    return state->cancel_after_write && state->last_phase == rm::IoPhase::Write &&
        state->last_completed != 0;
}

void observe_io_progress(void *context, rm::IoPhase phase, size_t completed, size_t) {
    ProgressState *state = static_cast<ProgressState *>(context);
    ++state->callbacks;
    state->last_phase = phase;
    state->last_completed = completed;
    if (phase == rm::IoPhase::Write && completed) state->cancel_after_write = true;
}

bool always_cancel(void *) { return true; }

struct ReadProgressState {
    bool cancel = false;
    size_t callbacks = 0;
};

bool cancel_after_read_chunk(void *context) {
    return static_cast<ReadProgressState *>(context)->cancel;
}

void observe_read_progress(void *context, rm::IoPhase phase,
                           size_t completed, size_t) {
    ReadProgressState *state = static_cast<ReadProgressState *>(context);
    ++state->callbacks;
    if (phase == rm::IoPhase::Read && completed >= 64u * 1024u)
        state->cancel = true;
}

void test_io_progress_and_cancel_before_rename() {
    char path[1400];
    make_path(path, sizeof(path), "cancel target.rmath");
    const char original[] = "keep before cancelled save";
    write_file(path, original, sizeof(original) - 1);
    rm::Document sentinel;
    make_document(sentinel, "unchanged on cancelled read");
    rm::Document before;
    rm::Error error;
    assert(rm::clone_document(sentinel, before, error));
    rm::FileStamp stamp{};
    stamp.exists = true;
    stamp.content_digest = UINT64_C(0xfeedface);
    const rm::FileStamp old_stamp = stamp;
    rm::IoControl cancel_read{always_cancel, nullptr, nullptr};
    assert(!rm::read_document(path, sentinel, stamp, error, &cancel_read));
    assert(error.code == rm::Code::Cancelled);
    assert(same_basic_document(sentinel, before));
    assert(stamp.exists == old_stamp.exists && stamp.content_digest == old_stamp.content_digest);

    rm::Document large_document;
    assert(large_document.title.set("Chunk cancellation", strlen("Chunk cancellation")));
    char *large_source = static_cast<char *>(malloc(rm::kMaxSourceBytes + 1));
    assert(large_source);
    memset(large_source, 'x', rm::kMaxSourceBytes);
    large_source[rm::kMaxSourceBytes] = 0;
    rm::FormulaId large_id = 0;
    assert(rm::add_formula(large_document, "large", large_source, large_id, error));
    free(large_source);
    rm::Bytes large_bytes;
    assert(rm::encode_document(large_document, large_bytes, error));
    assert(large_bytes.size() > 64u * 1024u);
    char large_path[1400];
    make_path(large_path, sizeof(large_path), "chunked read.rmath");
    write_file(large_path, large_bytes.data(), large_bytes.size());
    ReadProgressState read_progress;
    rm::IoControl chunk_cancel{cancel_after_read_chunk, &read_progress,
                               observe_read_progress};
    assert(!rm::read_document(large_path, sentinel, stamp, error, &chunk_cancel));
    assert(error.code == rm::Code::Cancelled);
    assert(read_progress.cancel && read_progress.callbacks >= 2);
    assert(same_basic_document(sentinel, before));
    assert(stamp.exists == old_stamp.exists && stamp.content_digest == old_stamp.content_digest);

    rm::FileStamp expected;
    assert(rm::file_stamp(path, expected, error));
    rm::Document replacement;
    make_document(replacement, "cancel after a short write");
    ProgressState progress;
    rm::IoControl control{cancel_when_write_progressed, &progress, observe_io_progress};
    rm::FileStamp new_stamp{};
    assert(!rm::atomic_save(path, replacement, expected, new_stamp, error, &control));
    assert(error.code == rm::Code::Cancelled);
    assert(progress.callbacks != 0 && progress.cancel_after_write);
    assert(file_is(path, original, sizeof(original) - 1));
    assert(!has_temp_file(test_root));
}

void test_recovery_directory_sync_failures_are_reported() {
    fputs("document suite: recovery directory sync failure regressions\n", stderr);
    rm::Document document;
    make_document(document, "durable recovery path");
    for (unsigned stage = 0; stage < 4; ++stage) {
        char branch[80], base[1400], target[1600];
        int count = snprintf(branch, sizeof(branch), "directory-sync-%u", stage);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(branch));
        make_path(base, sizeof(base), branch);
        count = snprintf(target, sizeof(target), "%s/reliefmath/recovery", base);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(target));
        const char *suffixes[] = {"", "", "/reliefmath", "/reliefmath/recovery"};
        directory_sync_fault = DirectorySyncFault{};
        count = snprintf(directory_sync_fault.path, sizeof(directory_sync_fault.path),
                         "%s%s", stage == 0 ? test_root : base, suffixes[stage]);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(directory_sync_fault.path));
        if (stage < 3) {
            const char *entries[] = {"", "/reliefmath", "/reliefmath/recovery"};
            count = snprintf(directory_sync_fault.entry_path,
                             sizeof(directory_sync_fault.entry_path), "%s%s", base, entries[stage]);
            assert(count > 0 && static_cast<size_t>(count) < sizeof(directory_sync_fault.entry_path));
        }
        directory_sync_fault.error_number = EIO;

        rm::RecoveryStore store;
        rm::Error error;
        // A retry must also reject an entry left behind by an unsuccessful sync.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const bool opened = store.open(target, error);
            if (opened)
                fprintf(stderr, "directory sync stage %u: open unexpectedly succeeded\n", stage);
            assert(!opened && "recovery open must propagate directory fsync failure");
            assert(error.code == rm::Code::Io);
            assert(directory_sync_fault.calls > attempt);
            assert(store.current_id() == 0 && store.items().size() == 0);
            assert(!store.save(document, error));
            assert(error.code == rm::Code::Format);
            if (stage < 2) {
                struct stat st;
                assert(lstat(target, &st) < 0 && errno == ENOENT);
            }
        }

        directory_sync_fault = DirectorySyncFault{};
        assert(store.open(target, error));
        assert(store.save(document, error));
        assert(store.discard_own(error));
    }
}

void test_recovery_directory_sync_retries_eintr_and_persists_permissions() {
    char target[1600];
    make_path(target, sizeof(target), "directory-sync-eintr");
    assert(mkdir(target, 0755) == 0);
    for (unsigned stage = 0; stage < 2; ++stage) {
        directory_sync_fault = DirectorySyncFault{};
        int count = snprintf(directory_sync_fault.path, sizeof(directory_sync_fault.path),
                             "%s", stage == 0 ? test_root : target);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(directory_sync_fault.path));
        directory_sync_fault.interrupts = 1;
        rm::RecoveryStore store;
        rm::Error error;
        assert(store.open(target, error));
        assert(directory_sync_fault.interrupts == 0 && directory_sync_fault.calls == 2);
        struct stat st;
        assert(stat(target, &st) == 0 && (st.st_mode & 0777) == 0700);
    }
    directory_sync_fault = DirectorySyncFault{};
}

void test_default_recovery_sync_failure_preserves_path_and_store() {
    char xdg[1400];
    make_path(xdg, sizeof(xdg), "xdg-directory-sync-failure");
    assert(setenv("XDG_STATE_HOME", xdg, 1) == 0);
    directory_sync_fault = DirectorySyncFault{};
    int count = snprintf(directory_sync_fault.path, sizeof(directory_sync_fault.path),
                         "%s", test_root);
    assert(count > 0 && static_cast<size_t>(count) < sizeof(directory_sync_fault.path));
    directory_sync_fault.error_number = EIO;
    rm::Text path;
    assert(path.set("preserved recovery path"));
    rm::Error error;
    assert(!rm::recovery_directory(path, error));
    assert(error.code == rm::Code::Io && path == "preserved recovery path");
    rm::RecoveryStore store;
    assert(!store.open_default(error));
    assert(error.code == rm::Code::Io && store.current_id() == 0);
    assert(directory_sync_fault.calls == 2);
    directory_sync_fault = DirectorySyncFault{};
    assert(store.open_default(error));
}

void test_recovery_locks_crash_discovery_and_restore_does_not_touch_original() {
    char recovery_path[1400], original_path[1400];
    make_path(recovery_path, sizeof(recovery_path), "state/reliefmath/recovery");
    make_path(original_path, sizeof(original_path), "original.rmath");
    const char original[] = "original file remains untouched";
    write_file(original_path, original, sizeof(original) - 1);

    rm::Document document;
    make_document(document, "recovered source");
    rm::Error error;
    uint64_t id = 0;
    {
        rm::RecoveryStore first;
        assert(first.open(recovery_path, error));
        assert(first.save(document, original_path, error));
        id = first.current_id();
        assert(id != 0);
        assert(first.list(error));
        assert(first.items().size() == 1 && first.items()[0].id == id);
        assert(first.items()[0].original_path_hint == original_path);
        assert(first.list(error));
        assert(first.items().size() == 1 && first.items()[0].id == id);

        rm::RecoveryStore second;
        assert(second.open(recovery_path, error));
        assert(second.list(error));
        assert(second.items().size() == 0);
        rm::Document unavailable;
        assert(!second.load(id, unavailable, error));
        assert(error.code == rm::Code::Conflict);
    }

    rm::RecoveryStore recovered_store;
    assert(recovered_store.open(recovery_path, error));
    assert(recovered_store.list(error));
    assert(recovered_store.items().size() == 1);
    assert(recovered_store.items()[0].id == id);
    rm::Document recovered;
    assert(recovered_store.load(id, recovered, error));
    assert(rm::same_document_content(document, recovered));
    assert(file_is(original_path, original, sizeof(original) - 1));

    // Choosing Cancel after a recovery prompt keeps the independent copy discoverable.
    assert(recovered_store.list(error));
    assert(recovered_store.items().size() == 1);
    rm::Document repeated;
    assert(recovered_store.load(id, repeated, error));
    assert(rm::same_document_content(document, repeated));

    struct stat st;
    assert(stat(recovery_path, &st) == 0 && S_ISDIR(st.st_mode));
    assert(st.st_uid == geteuid() && (st.st_mode & 0777) == 0700);
    assert(recovered_store.discard(id, error));
    assert(recovered_store.list(error));
    assert(recovered_store.items().size() == 0);
    assert(file_is(original_path, original, sizeof(original) - 1));
}

void test_discard_own_is_safe_for_shutdown() {
    char recovery_path[1400];
    make_path(recovery_path, sizeof(recovery_path), "state/shutdown/recovery");
    rm::Document document;
    make_document(document, "shutdown cleanup");
    rm::Error error;
    rm::RecoveryStore store;
    assert(store.open(recovery_path, error));
    assert(store.discard_own(error));
    assert(error.code == rm::Code::None);
    rm::IoControl cancelled{always_cancel, nullptr, nullptr};
    assert(!store.save(document, error, &cancelled));
    assert(error.code == rm::Code::Cancelled && store.current_id() != 0);
    assert(store.discard_own(error));
    assert(store.current_id() == 0);
    assert(store.save(document, error));
    const uint64_t own_id = store.current_id();
    assert(own_id != 0);
    assert(store.discard_own(error));
    assert(store.current_id() == 0);
    assert(store.items().size() == 0);
    assert(store.discard_own(error));
    assert(store.list(error));
    assert(store.items().size() == 0);
}

void test_recovery_stale_lock_after_process_exit_and_directory_symlink_rejection() {
    char crash_dir[1400], link_dir[1400], real_dir[1400];
    make_path(crash_dir, sizeof(crash_dir), "crash-recovery");
    make_path(link_dir, sizeof(link_dir), "recovery-link");
    make_path(real_dir, sizeof(real_dir), "real-recovery");
    rm::Document child_document;
    make_document(child_document, "crash snapshot");

    int channel[2];
    assert(pipe(channel) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(channel[0]);
        rm::RecoveryStore store;
        rm::Error child_error;
        if (!store.open(crash_dir, child_error) || !store.save(child_document, child_error))
            _exit(10);
        uint64_t child_id = store.current_id();
        if (write(channel[1], &child_id, sizeof(child_id)) != static_cast<ssize_t>(sizeof(child_id)))
            _exit(11);
        close(channel[1]);
        _exit(0); // Simulate process death without RecoveryStore's destructor.
    }
    close(channel[1]);
    uint64_t id = 0;
    assert(read(channel[0], &id, sizeof(id)) == static_cast<ssize_t>(sizeof(id)));
    close(channel[0]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    rm::RecoveryStore parent;
    rm::Error error;
    assert(parent.open(crash_dir, error));
    assert(parent.list(error));
    assert(parent.items().size() == 1 && parent.items()[0].id == id);
    rm::Document restored;
    assert(parent.load(id, restored, error));
    assert(rm::same_document_content(child_document, restored));

    assert(mkdir(real_dir, 0700) == 0);
    assert(symlink(real_dir, link_dir) == 0);
    rm::RecoveryStore unsafe;
    assert(!unsafe.open(link_dir, error));
    assert(error.code == rm::Code::Format || error.code == rm::Code::Io);
    assert(unlink(link_dir) == 0);
}

void test_default_xdg_path_and_missing_home() {
    char xdg[1400], home[1400];
    make_path(xdg, sizeof(xdg), "xdg-state");
    make_path(home, sizeof(home), "home");
    assert(mkdir(home, 0700) == 0);
    assert(setenv("XDG_STATE_HOME", xdg, 1) == 0);
    assert(setenv("HOME", home, 1) == 0);
    rm::Text path;
    rm::Error error;
    assert(rm::recovery_directory(path, error));
    assert(strstr(path.c_str(), "/reliefmath/recovery") != nullptr);
    rm::RecoveryStore store;
    assert(store.open_default(error));
    assert(store.save(rm::Document{}, error));

    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("HOME") == 0);
    rm::Text preserved;
    assert(preserved.set("unchanged", 9));
    assert(!rm::recovery_directory(preserved, error));
    assert(preserved == "unchanged");
    assert(setenv("HOME", home, 1) == 0);
}

void remove_tree_contents(const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[1600];
        int count = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(path));
        struct stat st;
        assert(lstat(path, &st) == 0);
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            remove_tree_contents(path);
            assert(rmdir(path) == 0);
        } else {
            assert(unlink(path) == 0);
        }
    }
    assert(closedir(dir) == 0);
}

} // namespace

extern "C" int __real_fsync(int fd);

// Linker wrapping keeps fault injection out of the synchronous production API.
extern "C" int __wrap_fsync(int fd) {
    const int saved_errno = errno;
    if (directory_sync_fault.path[0]) {
        struct stat expected, actual, entry;
        if (stat(directory_sync_fault.path, &expected) == 0 &&
            fstat(fd, &actual) == 0 && S_ISDIR(actual.st_mode) &&
            expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino &&
            (!directory_sync_fault.entry_path[0] ||
             lstat(directory_sync_fault.entry_path, &entry) == 0)) {
            ++directory_sync_fault.calls;
            if (directory_sync_fault.interrupts) {
                --directory_sync_fault.interrupts;
                errno = EINTR;
                return -1;
            }
            if (directory_sync_fault.error_number) {
                errno = directory_sync_fault.error_number;
                return -1;
            }
        }
    }
    errno = saved_errno;
    return __real_fsync(fd);
}

int main() {
    assert(mkdir("out/tmp", 0700) == 0 || errno == EEXIST);
    char working_directory[900];
    assert(getcwd(working_directory, sizeof(working_directory)));
    int count = snprintf(test_root, sizeof(test_root),
                         "%s/out/tmp/reliefmath-document-test-XXXXXX", working_directory);
    assert(count > 0 && static_cast<size_t>(count) < sizeof(test_root));
    assert(mkdtemp(test_root));
    test_recovery_directory_sync_failures_are_reported();
    test_recovery_directory_sync_retries_eintr_and_persists_permissions();
    test_default_recovery_sync_failure_preserves_path_and_store();
    test_v1_exact_bytes_and_round_trip();
    test_all_truncations_and_malformed_fields_preserve_document();
    test_duplicate_ids_are_rejected();
    test_exact_16_mib_boundary_and_allocation_failure_are_atomic();
    test_file_round_trip_conflict_and_symlink_rejection();
    test_raw_atomic_limit_is_independent_of_document_limit();
    test_short_writes_eintr_and_pre_rename_failures_preserve_target();
    test_bad_file_load_keeps_document_and_stamp();
    test_io_progress_and_cancel_before_rename();
    test_recovery_locks_crash_discovery_and_restore_does_not_touch_original();
    test_recovery_stale_lock_after_process_exit_and_directory_symlink_rejection();
    test_discard_own_is_safe_for_shutdown();
    test_default_xdg_path_and_missing_home();
    remove_tree_contents(test_root);
    assert(rmdir(test_root) == 0);
    return 0;
}
