#include "document.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef RPPT_TEST_RENAMEAT2_WRAP
#include <linux/fs.h>
#include <stdarg.h>
#include <sys/syscall.h>
#endif

using namespace rp;

namespace {

bool same_text(const Text &a, const Text &b) {
    return a.size() == b.size() && (!a.size() || !memcmp(a.c_str(), b.c_str(), a.size()));
}

bool same_object(const Object &a, const Object &b) {
    return a.id == b.id && a.kind == b.kind && a.rect.x == b.rect.x && a.rect.y == b.rect.y &&
        a.rect.w == b.rect.w && a.rect.h == b.rect.h && a.style.text == b.style.text &&
        a.style.fill == b.style.fill && a.style.stroke == b.style.stroke &&
        a.style.font_size == b.style.font_size && a.style.line_width == b.style.line_width &&
        a.style.family == b.style.family && a.style.bold == b.style.bold &&
        a.style.italic == b.style.italic && a.style.underline == b.style.underline &&
        a.style.bullets == b.style.bullets && a.style.filled == b.style.filled &&
        a.style.outlined == b.style.outlined && a.style.alignment == b.style.alignment &&
        a.asset == b.asset && same_text(a.text, b.text);
}

bool same_deck(const Deck &a, const Deck &b) {
    if (a.width != b.width || a.height != b.height || a.next_id != b.next_id ||
        a.revision != b.revision || !same_text(a.title, b.title) || !same_text(a.author, b.author) ||
        a.slides.size() != b.slides.size() || a.assets.size() != b.assets.size()) return false;
    for (size_t i = 0; i < a.assets.size(); ++i) {
        if (a.assets[i].width != b.assets[i].width || a.assets[i].height != b.assets[i].height ||
            !same_text(a.assets[i].rgba, b.assets[i].rgba)) return false;
    }
    for (size_t i = 0; i < a.slides.size(); ++i) {
        const Slide &x = a.slides[i], &y = b.slides[i];
        if (x.background != y.background || !same_text(x.notes, y.notes) ||
            x.objects.size() != y.objects.size()) return false;
        for (size_t j = 0; j < x.objects.size(); ++j)
            if (!same_object(x.objects[j], y.objects[j])) return false;
    }
    return true;
}

Deck make_complete_deck() {
    Deck deck;
    assert(deck.init());
    deck.width = 1200;
    deck.height = 675;
    deck.revision = 42;
    assert(deck.title.set("演示文稿", strlen("演示文稿")));
    assert(deck.author.set("林", strlen("林")));
    assert(deck.slides[0].notes.set("备注：第一张", strlen("备注：第一张")));
    deck.slides[0].background = 0x102938;

    const char rgba[] = {char(0), char(1), char(127), char(255), char(32), char(64), char(96), char(128)};
    Asset asset;
    asset.width = 2;
    asset.height = 1;
    assert(asset.rgba.set(rgba, sizeof(rgba)));
    assert(deck.assets.push(static_cast<Asset &&>(asset)));

    const Kind kinds[] = {Kind::Text, Kind::Rectangle, Kind::Ellipse, Kind::Line, Kind::Arrow, Kind::Image};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        Object object;
        object.id = static_cast<uint32_t>(i + 1);
        object.kind = kinds[i];
        object.rect = {float(i * 150), float(i * 70), 120, 60};
        object.style.text = uint32_t(0x101010 + i);
        object.style.fill = uint32_t(0x202020 + i);
        object.style.stroke = uint32_t(0x303030 + i);
        object.style.font_size = float(12 + i);
        object.style.line_width = float(i) * 0.5f;
        object.style.family = static_cast<uint8_t>(i % 3);
        object.style.bold = (i & 1) != 0;
        object.style.italic = (i & 2) != 0;
        object.style.underline = (i & 1) == 0;
        object.style.bullets = (i & 2) != 0;
        object.style.filled = (i & 1) != 0;
        object.style.outlined = (i & 2) == 0;
        object.style.alignment = static_cast<Alignment>(i % 3);
        if (i == 5) object.asset = 0;
        char label[64];
        int n = snprintf(label, sizeof(label), "对象%u", object.id);
        assert(n > 0 && size_t(n) < sizeof(label) && object.text.set(label, size_t(n)));
        assert(deck.slides[0].objects.push(static_cast<Object &&>(object)));
    }
    deck.next_id = 7;

    Slide second;
    second.background = 0xabcdef;
    assert(second.notes.set("第二张备注\n保留换行", strlen("第二张备注\n保留换行")));
    Object back;
    back.id = 7;
    back.kind = Kind::Rectangle;
    back.rect = {20, 30, 100, 80};
    back.style.filled = true;
    assert(back.text.set("底层", strlen("底层")));
    assert(second.objects.push(static_cast<Object &&>(back)));
    Object front;
    front.id = 8;
    front.kind = Kind::Text;
    front.rect = {40, 50, 140, 90};
    front.style.bold = true;
    front.style.underline = true;
    front.style.alignment = Alignment::Right;
    assert(front.text.set("顶层", strlen("顶层")));
    assert(second.objects.push(static_cast<Object &&>(front)));
    assert(deck.slides.push(static_cast<Slide &&>(second)));
    deck.next_id = 9;
    Text error;
    assert(deck.validate(error));
    return deck;
}

uint16_t u16(const char *p) {
    const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
    return uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8);
}

uint32_t u32(const char *p) {
    const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
        (uint32_t(b[3]) << 24);
}

uint64_t u64(const char *p) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= uint64_t(static_cast<unsigned char>(p[i])) << (i * 8);
    return result;
}

void put_u32(char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = char(value >> (i * 8));
}

void put_u64(char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = char(value >> (i * 8));
}

struct RecordOffsets {
    size_t first_kind = SIZE_MAX;
    size_t first_alignment = SIZE_MAX;
    size_t image_asset = SIZE_MAX;
    size_t first_text = SIZE_MAX;
    size_t title_length = SIZE_MAX;
    size_t second_id = SIZE_MAX;
    size_t slide_count = SIZE_MAX;
};

size_t skip_string(const char *bytes, size_t at, size_t size) {
    assert(at <= size && size - at >= 4);
    uint32_t length = u32(bytes + at);
    assert(length <= size - at - 4);
    return at + 4 + length;
}

RecordOffsets find_offsets(const Text &bytes) {
    RecordOffsets offsets;
    const char *raw = bytes.c_str();
    const size_t size = bytes.size();
    size_t at = 16 + 20; // Header followed by dimensions, ID high-water, and revision.
    offsets.title_length = at;
    at = skip_string(raw, at, size);
    at = skip_string(raw, at, size);
    assert(size - at >= 4);
    uint32_t assets = u32(raw + at);
    at += 4;
    for (uint32_t i = 0; i < assets; ++i) {
        assert(size - at >= 12);
        uint32_t data_length = u32(raw + at + 8);
        at += 12;
        assert(data_length <= size - at);
        at += data_length;
    }
    offsets.slide_count = at;
    assert(size - at >= 4);
    uint32_t slides = u32(raw + at);
    at += 4;
    for (uint32_t s = 0; s < slides; ++s) {
        assert(size - at >= 8);
        at += 4;
        at = skip_string(raw, at, size);
        uint32_t objects = u32(raw + at);
        at += 4;
        for (uint32_t o = 0; o < objects; ++o) {
            size_t id_offset = at;
            if (s == 0 && o == 0) {
                offsets.first_kind = at + 4;
                offsets.first_alignment = at + 7;
            }
            if (s == 0 && o == 1) offsets.second_id = id_offset;
            assert(size - at >= 52);
            size_t asset_offset = at + 44;
            at += 48;
            uint32_t text_length = u32(raw + at);
            if (s == 0 && o == 5) offsets.image_asset = asset_offset;
            if (s == 0 && o == 0) {
                offsets.first_text = at + 4;
            }
            at += 4;
            assert(text_length <= size - at);
            at += text_length;
        }
    }
    assert(at == size);
    return offsets;
}

void expect_decode_failure(const Text &bytes, const char *required_error = nullptr) {
    Deck output = make_complete_deck();
    Deck before;
    assert(before.clone_from(output));
    Text error;
    assert(!decode_deck(bytes.c_str(), bytes.size(), output, error));
    assert(!error.empty());
    if (required_error) assert(strstr(error.c_str(), required_error));
    assert(same_deck(output, before));
}

void write_all(int fd, const char *bytes, size_t length) {
    size_t at = 0;
    while (at < length) {
        ssize_t n = write(fd, bytes + at, length - at);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        at += size_t(n);
    }
}

Text read_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    struct stat st{};
    assert(fstat(fd, &st) == 0 && st.st_size >= 0);
    Text data;
    char *buffer = static_cast<char *>(malloc(size_t(st.st_size) ? size_t(st.st_size) : 1));
    assert(buffer);
    size_t at = 0;
    while (at < size_t(st.st_size)) {
        ssize_t n = read(fd, buffer + at, size_t(st.st_size) - at);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        at += size_t(n);
    }
    assert(data.set(buffer, at));
    free(buffer);
    assert(close(fd) == 0);
    return data;
}

void test_native_roundtrip_and_header() {
    Deck original = make_complete_deck();
    Text encoded, error;
    assert(encode_deck(original, encoded, error));
    assert(error.empty() && encoded.size() > 16);
    assert(!memcmp(encoded.c_str(), "RPPT", 4));
    assert(u16(encoded.c_str() + 4) == 1 && u16(encoded.c_str() + 6) == 0);
    assert(u64(encoded.c_str() + 8) == encoded.size() - 16);

    Deck decoded;
    assert(decode_deck(encoded.c_str(), encoded.size(), decoded, error));
    assert(error.empty() && same_deck(original, decoded));
    assert(decoded.slides[0].objects[0].kind == Kind::Text);
    assert(decoded.slides[0].objects[1].kind == Kind::Rectangle);
    assert(decoded.slides[0].objects[2].kind == Kind::Ellipse);
    assert(decoded.slides[0].objects[3].kind == Kind::Line);
    assert(decoded.slides[0].objects[4].kind == Kind::Arrow);
    assert(decoded.slides[0].objects[5].kind == Kind::Image);
    assert(decoded.slides[1].objects[0].id == 7 && decoded.slides[1].objects[1].id == 8);
    const char expected_rgba[] = {char(0), char(1), char(127), char(255), char(32), char(64), char(96), char(128)};
    assert(decoded.assets[0].rgba.size() == sizeof(expected_rgba));
    assert(!memcmp(decoded.assets[0].rgba.c_str(), expected_rgba, sizeof(expected_rgba)));
}

void test_corrupt_input_is_rejected_without_changing_output() {
    Deck original = make_complete_deck();
    Text good, error;
    assert(encode_deck(original, good, error));
    RecordOffsets offsets = find_offsets(good);

    Text bad;
    assert(bad.set(good.c_str(), good.size() - 1));
    expect_decode_failure(bad);

    assert(bad.set(good.c_str(), good.size()));
    bad.c_str();
    char *copy = static_cast<char *>(malloc(good.size() + 1));
    assert(copy);
    memcpy(copy, good.c_str(), good.size());
    copy[4] = 2;
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad, "不支持此原生演示文稿版本");
    memcpy(copy, good.c_str(), good.size());
    copy[6] = 1;
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);
    memcpy(copy, good.c_str(), good.size());
    put_u64(copy + 8, good.size() - 17);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    copy[offsets.first_kind] = char(0xff);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + 16, 0x7fc00000U); // NaN page width, encoded little endian.
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + 16, 0x7f800000U); // Infinite page width.
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    copy[offsets.first_alignment] = char(0xff);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + offsets.title_length, UINT32_MAX);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + offsets.image_asset, 1);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + offsets.second_id, 1);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    copy[offsets.first_text] = char(0xff);
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    put_u32(copy + offsets.slide_count, uint32_t(MaxSlides + 1));
    assert(bad.set(copy, good.size()));
    expect_decode_failure(bad);

    memcpy(copy, good.c_str(), good.size());
    copy[good.size()] = 'x';
    put_u64(copy + 8, good.size() - 16 + 1);
    assert(bad.set(copy, good.size() + 1));
    expect_decode_failure(bad);

    Deck before;
    assert(before.clone_from(original));
    assert(!decode_deck(nullptr, MaxFileBytes + 1, original, error));
    assert(same_deck(original, before));
    free(copy);
}

void test_encode_rejects_invalid_deck_without_touching_output() {
    Deck invalid = make_complete_deck();
    invalid.slides[0].objects[0].style.font_size = INFINITY;
    Text output, error;
    assert(output.set("previous bytes", strlen("previous bytes")));
    assert(!encode_deck(invalid, output, error));
    assert(output == "previous bytes" && !error.empty());
}

void test_file_stamp_atomic_write_conflicts_and_type_rejection(const char *root) {
    char target[4096], missing[4096], link[4096], fifo[4096], payload[4096], locked_dir[4096], locked_file[4096];
    assert(snprintf(target, sizeof(target), "%s/native.rppt", root) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/no-such-dir/file", root) > 0);
    assert(snprintf(link, sizeof(link), "%s/alias", root) > 0);
    assert(snprintf(fifo, sizeof(fifo), "%s/pipe", root) > 0);
    assert(snprintf(payload, sizeof(payload), "%s/payload", root) > 0);
    assert(snprintf(locked_dir, sizeof(locked_dir), "%s/locked", root) > 0);
    assert(snprintf(locked_file, sizeof(locked_file), "%s/locked/original", root) > 0);
    Text error;
    FileStamp absent;
    assert(file_stamp(target, absent, error) && !absent.exists);
    const char first[] = {'f', 'i', 'r', 's', 't', 0, 'x'};
    assert(atomic_write(target, first, sizeof(first), error, &absent));
    Text actual = read_file(target);
    assert(actual.size() == sizeof(first) && !memcmp(actual.c_str(), first, sizeof(first)));
    struct stat st{};
    assert(lstat(target, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    FileStamp first_stamp;
    assert(file_stamp(target, first_stamp, error) && first_stamp.exists);

    const char outside[] = "external edit";
    int fd = open(target, O_WRONLY | O_TRUNC | O_CLOEXEC);
    assert(fd >= 0);
    write_all(fd, outside, sizeof(outside) - 1);
    assert(fsync(fd) == 0 && close(fd) == 0);
    FileStamp external_stamp;
    assert(file_stamp(target, external_stamp, error));
    assert(!same_stamp(first_stamp, external_stamp));
    assert(!atomic_write(target, "lost update", 11, error, &first_stamp));
    assert(read_file(target) == "external edit");

    FileStamp external_before_metadata;
    assert(file_stamp(target, external_before_metadata, error));
    assert(chmod(target, 0640) == 0);
    assert(!atomic_write(target, "lost metadata update", 20, error, &external_before_metadata));
    assert(read_file(target) == "external edit");

    FileStamp before_identity_swap;
    assert(file_stamp(target, before_identity_swap, error));
    char replacement[4096];
    assert(snprintf(replacement, sizeof(replacement), "%s/replacement", root) > 0);
    int replacement_fd = open(replacement, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(replacement_fd >= 0);
    write_all(replacement_fd, outside, sizeof(outside) - 1);
    assert(close(replacement_fd) == 0 && rename(replacement, target) == 0);
    assert(!atomic_write(target, "lost inode update", 17, error, &before_identity_swap));
    assert(read_file(target) == "external edit");

    assert(file_stamp(target, external_stamp, error));
    assert(atomic_write(target, "fresh", 5, error, &external_stamp));
    actual = read_file(target);
    assert(actual == "fresh");
    assert(lstat(target, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(!atomic_write(missing, "x", 1, error));

    fd = open(payload, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all(fd, "unchanged", 9);
    assert(close(fd) == 0);
    assert(symlink(payload, link) == 0);
    FileStamp link_stamp;
    assert(!file_stamp(link, link_stamp, error));
    assert(!atomic_write(link, "replacement", 11, error));
    assert(read_file(payload) == "unchanged");
    assert(unlink(link) == 0);

    assert(mkfifo(fifo, 0600) == 0);
    assert(!file_stamp(fifo, link_stamp, error));
    assert(!atomic_write(fifo, "replacement", 11, error));
    assert(unlink(fifo) == 0);

    assert(mkdir(locked_dir, 0700) == 0);
    fd = open(locked_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all(fd, "keep original", 13);
    assert(close(fd) == 0);
    FileStamp locked_stamp;
    assert(file_stamp(locked_file, locked_stamp, error));
    assert(chmod(locked_dir, 0500) == 0);
    assert(!atomic_write(locked_file, "new bytes", 9, error, &locked_stamp));
    assert(read_file(locked_file) == "keep original");
    assert(chmod(locked_dir, 0700) == 0);
    assert(unlink(locked_file) == 0 && rmdir(locked_dir) == 0);
}

void test_native_files_load_save_and_failure_atomicity(const char *root) {
    char target[4096], symlink_path[4096], oversized[4096];
    assert(snprintf(target, sizeof(target), "%s/roundtrip.rppt", root) > 0);
    assert(snprintf(symlink_path, sizeof(symlink_path), "%s/open-link.rppt", root) > 0);
    assert(snprintf(oversized, sizeof(oversized), "%s/oversized-native.rppt", root) > 0);
    Deck original = make_complete_deck(), loaded;
    Text error;
    FileStamp expected;
    assert(file_stamp(target, expected, error) && !expected.exists);
    assert(save_deck(target, original, error, &expected));
    assert(load_deck(target, loaded, error));
    assert(same_deck(original, loaded));

    Deck unchanged;
    assert(unchanged.init());
    unchanged.title.set("keep on failed open", strlen("keep on failed open"));
    const uint64_t revision = unchanged.revision;
    assert(!load_deck(symlink_path, unchanged, error));
    assert(unchanged.title == "keep on failed open" && unchanged.revision == revision);
    assert(symlink(target, symlink_path) == 0);
    assert(!load_deck(symlink_path, unchanged, error));
    assert(unchanged.title == "keep on failed open" && unchanged.revision == revision);
    assert(unlink(symlink_path) == 0);

    int oversized_fd = open(oversized, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(oversized_fd >= 0);
    assert(ftruncate(oversized_fd, static_cast<off_t>(MaxFileBytes + 1)) == 0);
    assert(close(oversized_fd) == 0);
    Deck before_oversized_load;
    assert(before_oversized_load.clone_from(unchanged));
    assert(!load_deck(oversized, unchanged, error));
    assert(strstr(error.c_str(), "96 MiB"));
    assert(same_deck(unchanged, before_oversized_load));
    assert(unlink(oversized) == 0);

    FileStamp stale;
    assert(file_stamp(target, stale, error));
    int fd = open(target, O_WRONLY | O_APPEND | O_CLOEXEC);
    assert(fd >= 0);
    write_all(fd, "x", 1);
    assert(close(fd) == 0);
    Text before = read_file(target);
    assert(!save_deck(target, original, error, &stale));
    assert(same_text(before, read_file(target)));
}

void test_atomic_write_export_size_boundaries(const char *root) {
    constexpr size_t MaxExportBytes = 256u * 1024u * 1024u;
    char target[4096], too_large[4096];
    assert(snprintf(target, sizeof(target), "%s/large-export.html", root) > 0);
    assert(snprintf(too_large, sizeof(too_large), "%s/oversized-export.html", root) > 0);
    char *zeroes = static_cast<char *>(calloc(1, MaxExportBytes + 1));
    assert(zeroes);

    Text error;
    FileStamp absent;
    assert(file_stamp(target, absent, error) && !absent.exists);
    assert(atomic_write(target, zeroes, MaxExportBytes, error, &absent));
    FileStamp large_stamp;
    assert(file_stamp(target, large_stamp, error) && large_stamp.exists &&
        large_stamp.size == MaxExportBytes);
    struct stat st{};
    assert(lstat(target, &st) == 0 && S_ISREG(st.st_mode) &&
        static_cast<uint64_t>(st.st_size) == MaxExportBytes && (st.st_mode & 0777) == 0600);

    Deck output = make_complete_deck();
    Deck before_load;
    assert(before_load.clone_from(output));
    assert(!load_deck(target, output, error));
    assert(strstr(error.c_str(), "96 MiB"));
    assert(same_deck(output, before_load));

    const char replacement[] = "done";
    assert(atomic_write(target, replacement, sizeof(replacement) - 1, error, &large_stamp));
    assert(read_file(target) == "done");

    FileStamp rejected_stamp;
    assert(file_stamp(too_large, rejected_stamp, error) && !rejected_stamp.exists);
    assert(!atomic_write(too_large, zeroes, MaxExportBytes + 1, error, &rejected_stamp));
    assert(strstr(error.c_str(), "256 MiB"));
    errno = 0;
    assert(lstat(too_large, &st) < 0 && errno == ENOENT);
    free(zeroes);
    assert(unlink(target) == 0);
}

void test_private_recovery_reservation_and_cross_process_lock(const char *root) {
    char state[4096], linked[4096], target[4096];
    assert(snprintf(state, sizeof(state), "%s/state-home", root) > 0);
    assert(snprintf(linked, sizeof(linked), "%s/state-link", root) > 0);
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);
    Text directory, error;
    assert(recovery_directory(directory, error));
    assert(strstr(directory.c_str(), "/reliefppt/recovery") != nullptr);
    struct stat st{};
    assert(lstat(directory.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid());
    assert((st.st_mode & 0777) == 0700);

    Text first, second;
    assert(reserve_recovery_path(first, error));
    assert(reserve_recovery_path(second, error));
    assert(first != second && first.size() > 5 &&
        !strcmp(first.c_str() + first.size() - 5, ".rppt"));
    assert(snprintf(target, sizeof(target), "%s", first.c_str()) > 0);
    assert(lstat(target, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid());
    assert((st.st_mode & 0777) == 0600);

    int descriptor = -1;
    assert(lock_recovery_path(first.c_str(), descriptor, error) && descriptor >= 0);
    int another = -1;
    assert(!lock_recovery_path(first.c_str(), another, error) && another == -1);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        Text child_error;
        int child_fd = -1;
        bool acquired = lock_recovery_path(first.c_str(), child_fd, child_error);
        if (acquired) unlock_recovery_path(child_fd);
        _exit(acquired ? 3 : 0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlock_recovery_path(descriptor);
    assert(descriptor == -1);
    assert(lock_recovery_path(first.c_str(), descriptor, error));
    unlock_recovery_path(descriptor);

    char lock_path[4096];
    assert(snprintf(lock_path, sizeof(lock_path), "%s.lock", first.c_str()) > 0);
    assert(unlink(lock_path) == 0);
    assert(symlink(target, lock_path) == 0);
    assert(!lock_recovery_path(first.c_str(), descriptor, error));
    assert(unlink(lock_path) == 0);

    assert(symlink(state, linked) == 0);
    assert(setenv("XDG_STATE_HOME", linked, 1) == 0);
    Text rejected;
    assert(!recovery_directory(rejected, error));
    assert(unlink(linked) == 0);
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);

    assert(unlink(first.c_str()) == 0);
    assert(unlink(second.c_str()) == 0);
    assert(unlink(lock_path) == 0 || errno == ENOENT);
    assert(snprintf(lock_path, sizeof(lock_path), "%s.lock", second.c_str()) > 0);
    assert(unlink(lock_path) == 0 || errno == ENOENT);
}

void set_recovery_mtime(const Text &path, time_t seconds) {
    struct timespec times[2] = {{seconds, 0}, {seconds, 0}};
    assert(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
}

void test_latest_valid_recovery_selection(const char *root) {
    char state[4096], path[4096];
    assert(snprintf(state, sizeof(state), "%s/recovery-state", root) > 0);
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);
    Text directory, error, older_path, newer_path, malformed_path, active_path;
    assert(recovery_directory(directory, error));
    assert(reserve_recovery_path(older_path, error));
    assert(reserve_recovery_path(newer_path, error));
    assert(reserve_recovery_path(malformed_path, error));
    assert(reserve_recovery_path(active_path, error));

    Deck older = make_complete_deck();
    Deck newer = make_complete_deck();
    Deck active = make_complete_deck();
    assert(older.title.set("Recovery marker 2468", strlen("Recovery marker 2468")));
    assert(newer.title.set("Recovery marker 8642", strlen("Recovery marker 8642")));
    assert(active.title.set("active recovery", strlen("active recovery")));
    assert(save_deck(older_path.c_str(), older, error));
    assert(save_deck(newer_path.c_str(), newer, error));
    assert(save_deck(active_path.c_str(), active, error));

    int malformed_fd = open(malformed_path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    assert(malformed_fd >= 0);
    write_all(malformed_fd, "not an RPPT document", strlen("not an RPPT document"));
    assert(close(malformed_fd) == 0);
    set_recovery_mtime(older_path, 1700000000);
    set_recovery_mtime(newer_path, 1700000060);
    set_recovery_mtime(malformed_path, 1700000120);
    set_recovery_mtime(active_path, 1700000180);

    FileStamp before[4];
    const Text *paths[] = {&older_path, &newer_path, &malformed_path, &active_path};
    for (size_t i = 0; i < 4; ++i) assert(file_stamp(paths[i]->c_str(), before[i], error));

    int active_lock = -1;
    assert(lock_recovery_path(active_path.c_str(), active_lock, error));
    Text selected;
    Deck selected_deck;
    int selected_lock = -1;
    assert(find_latest_recovery(selected, selected_lock, selected_deck, error));
    assert(selected == newer_path && selected_lock >= 0);
    assert(selected_deck.title == "Recovery marker 8642");
    int duplicate_lock = -1;
    assert(!lock_recovery_path(selected.c_str(), duplicate_lock, error));
    unlock_recovery_path(selected_lock);

    unlock_recovery_path(active_lock);
    assert(find_latest_recovery(selected, selected_lock, selected_deck, error));
    assert(selected == active_path && selected_lock >= 0);
    assert(selected_deck.title == "active recovery");
    struct stat selected_stat{};
    assert(lstat(selected.c_str(), &selected_stat) == 0 && S_ISREG(selected_stat.st_mode) &&
        selected_stat.st_uid == getuid() && (selected_stat.st_mode & 0777) == 0600);
    unlock_recovery_path(selected_lock);

    for (size_t i = 0; i < 4; ++i) {
        FileStamp after;
        assert(file_stamp(paths[i]->c_str(), after, error) && same_stamp(before[i], after));
        assert(unlink(paths[i]->c_str()) == 0);
        assert(snprintf(path, sizeof(path), "%s.lock", paths[i]->c_str()) > 0);
        assert(unlink(path) == 0 || errno == ENOENT);
    }
    assert(snprintf(path, sizeof(path), "%s/reliefppt/recovery", state) > 0 && rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/reliefppt", state) > 0 && rmdir(path) == 0);
    assert(rmdir(state) == 0);
}

} // namespace

#ifdef RPPT_TEST_RENAMEAT2_WRAP

namespace {

int forced_renameat2_errno = 0;
bool inject_competing_target = false;
int renameat2_call_count = 0;

void test_renameat2_fallback_case(const char *root, const char *name,
                                  int injected_errno, bool race_target) {
    char path[4096];
    assert(snprintf(path, sizeof(path), "%s/%s", root, name) > 0);
    struct stat st{};
    errno = 0;
    assert(lstat(path, &st) < 0 && errno == ENOENT);

    FileStamp absent;
    Text error;
    assert(file_stamp(path, absent, error) && !absent.exists);
    forced_renameat2_errno = injected_errno;
    inject_competing_target = race_target;
    renameat2_call_count = 0;

    const char payload[] = "reliefppt payload";
    const bool saved = atomic_write(path, payload, sizeof(payload) - 1, error, &absent);
    assert(renameat2_call_count == 1);
    if (race_target) {
        assert(!saved);
        assert(read_file(path) == "outside writer");
    } else {
        assert(saved);
        assert(read_file(path) == payload);
        assert(lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    }
    assert(unlink(path) == 0);
    forced_renameat2_errno = 0;
    inject_competing_target = false;
}

} // namespace

extern "C" long __wrap_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    if (number != SYS_renameat2) {
        va_end(args);
        errno = ENOSYS;
        return -1;
    }
    int old_directory = va_arg(args, int);
    const char *old_name = va_arg(args, const char *);
    int new_directory = va_arg(args, int);
    const char *new_name = va_arg(args, const char *);
    int flags = va_arg(args, int);
    va_end(args);

    assert(old_directory == new_directory);
    assert(old_name && new_name && flags == RENAME_NOREPLACE);
    ++renameat2_call_count;
    if (inject_competing_target) {
        int fd = openat(new_directory, new_name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(fd >= 0);
        const char competing[] = "outside writer";
        write_all(fd, competing, sizeof(competing) - 1);
        assert(close(fd) == 0);
    }
    errno = forced_renameat2_errno;
    return -1;
}

int main() {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char root[4096];
    int n = snprintf(root, sizeof(root), "%s/reliefppt-renameat2-XXXXXX", tmp);
    assert(n > 0 && size_t(n) < sizeof(root) && mkdtemp(root));
    test_renameat2_fallback_case(root, "eopnotsupp.rppt", EOPNOTSUPP, false);
    test_renameat2_fallback_case(root, "enosys.rppt", ENOSYS, false);
    test_renameat2_fallback_case(root, "einval.rppt", EINVAL, false);
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    test_renameat2_fallback_case(root, "enotsup.rppt", ENOTSUP, false);
#endif
    test_renameat2_fallback_case(root, "race.rppt", EOPNOTSUPP, true);
    assert(rmdir(root) == 0);
    puts("ReliefPPT renameat2 unsupported-error fallback tests passed");
    return 0;
}

#else

int main() {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char root[4096];
    int n = snprintf(root, sizeof(root), "%s/reliefppt-document-XXXXXX", tmp);
    assert(n > 0 && size_t(n) < sizeof(root) && mkdtemp(root));
    test_native_roundtrip_and_header();
    test_corrupt_input_is_rejected_without_changing_output();
    test_encode_rejects_invalid_deck_without_touching_output();
    test_file_stamp_atomic_write_conflicts_and_type_rejection(root);
    test_native_files_load_save_and_failure_atomicity(root);
    test_atomic_write_export_size_boundaries(root);
    test_private_recovery_reservation_and_cross_process_lock(root);
    test_latest_valid_recovery_selection(root);
    puts("ReliefPPT document tests passed");
    char path[4096];
    assert(snprintf(path, sizeof(path), "%s/native.rppt", root) > 0 && unlink(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/payload", root) > 0 && unlink(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/roundtrip.rppt", root) > 0 && unlink(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/state-home/reliefppt/recovery", root) > 0 && rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/state-home/reliefppt", root) > 0 && rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/state-home", root) > 0 && rmdir(path) == 0);
    assert(rmdir(root) == 0);
    return 0;
}

#endif
