#include "export.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
using namespace rm;

namespace {
pid_t deferred_child = -1;
unsigned deferred_waits = 0;
bool require_nonblocking_wait = false;
}

extern "C" pid_t __real_waitpid(pid_t, int *, int);
extern "C" pid_t __wrap_waitpid(pid_t child, int *status, int options) {
    if (require_nonblocking_wait) assert(options & WNOHANG);
    // Force cancel's legal "not exited yet" result without consuming child status.
    if (child == deferred_child && (options & WNOHANG)) {
        ++deferred_waits;
        return 0;
    }
    return __real_waitpid(child, status, options);
}

namespace {
uint64_t milliseconds() {
    timespec now{};
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec / 1000000);
}

void pump(ExportQueue &queue) {
    require_nonblocking_wait = true;
    queue.pump();
    require_nonblocking_wait = false;
}

void cancel(ExportQueue &queue) {
    require_nonblocking_wait = true;
    queue.cancel();
    require_nonblocking_wait = false;
}

Text path_in(const char *directory, const char *name) {
    char path[4096];
    const int size = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(size > 0 && size < int(sizeof(path)));
    Text result;
    assert(result.set(path));
    return result;
}

Text make_directory(const char *base, const char *name) {
    Text path = path_in(base, name);
    assert(mkdir(path.c_str(), 0700) == 0);
    return path;
}

void expect_file(const char *path, const char *expected) {
    FILE *file = fopen(path, "rb");
    assert(file);
    char contents[512];
    const size_t size = fread(contents, 1, sizeof(contents), file);
    assert(!ferror(file) && feof(file));
    assert(size == strlen(expected) && memcmp(contents, expected, size) == 0);
    assert(fclose(file) == 0);
}

void expect_absent(const char *path) {
    assert(access(path, F_OK) < 0 && errno == ENOENT);
}

ExportOptions options_for(const char *directory, ExportFormat format = ExportFormat::Svg) {
    ExportOptions options;
    assert(options.directory.set(directory));
    assert(options.pattern.set("{index}-{name}"));
    options.format = format;
    return options;
}

void finish(ExportQueue &queue) {
    const uint64_t deadline = milliseconds() + 8000;
    while (queue.busy() && milliseconds() < deadline) {
        pump(queue);
        usleep(1000);
    }
    assert(!queue.busy() && queue.position() == queue.items().size());
}

bool send_frame(Kind kind, const Snapshot &snapshot, const Bytes *payload = nullptr) {
    Frame frame;
    frame.kind = kind;
    frame.request_id = snapshot.request_id;
    frame.formula_id = snapshot.formula.id;
    frame.generation = snapshot.formula.generation;
    if (payload && !frame.payload.append(payload->data(), payload->size())) return false;
    Bytes wire;
    Error error;
    if (!encode_frame(frame, wire, error)) return false;
    size_t offset = 0;
    while (offset < wire.size()) {
        const ssize_t count = ::write(STDOUT_FILENO, wire.data() + offset, wire.size() - offset);
        if (count > 0) offset += size_t(count);
        else if (count < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool respond(const Snapshot &snapshot) {
    Error error;
    Bytes payload;
    if (snapshot.formula.source == "hold") {
        const char *pid_path = getenv("RM_TEST_EXPORT_PID_FILE");
        assert(pid_path);
        FILE *file = fopen(pid_path, "w");
        assert(file);
        assert(fprintf(file, "%ld\n", long(getpid())) > 0);
        assert(fclose(file) == 0);
        for (;;) pause();
    }
    if (snapshot.formula.source == "fail") {
        Error failure;
        failure.set(Code::Parse, "bad token", 1, 3);
        return encode_failure_payload(failure, payload, error) &&
               send_frame(Kind::Failure, snapshot, &payload);
    }
    ArtifactInfo info;
    if (snapshot.artifacts == WantSvg) {
        info.type = ArtifactType::Svg;
        if (!payload.append("<svg>", 5) ||
            !payload.append(snapshot.formula.source.c_str(), snapshot.formula.source.size()) ||
            !payload.append("</svg>", 6)) return false;
    } else {
        if (snapshot.artifacts != WantPng) return false;
        info.type = ArtifactType::Png;
        if (!payload.append("\x89PNG\r\n\x1a\n", 8) ||
            !payload.append(snapshot.formula.source.c_str(), snapshot.formula.source.size())) return false;
    }
    info.bytes = payload.size();
    Bytes metadata, done;
    return encode_artifact_info(info, metadata, error) &&
           send_frame(Kind::ArtifactBegin, snapshot, &metadata) &&
           send_frame(Kind::ArtifactChunk, snapshot, &payload) &&
           send_frame(Kind::ArtifactEnd, snapshot) &&
           encode_done_payload(0, done, error) && send_frame(Kind::Done, snapshot, &done);
}

int fake_worker() {
    Decoder decoder;
    Snapshot snapshot;
    uint32_t source_bytes = 0;
    Bytes source;
    bool began = false;
    for (;;) {
        uint8_t buffer[4096];
        const ssize_t size = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size == 0) return 0;
        if (size < 0) return 2;
        Error error;
        if (!decoder.feed(buffer, size_t(size), error)) return 3;
        Frame frame;
        while (decoder.take(frame)) {
            if (!began) {
                if (frame.kind != Kind::Begin ||
                    !decode_begin_payload(frame, snapshot, source_bytes, error)) return 4;
                source.clear();
                began = true;
            } else {
                if (frame.request_id != snapshot.request_id || frame.formula_id != snapshot.formula.id ||
                    frame.generation != snapshot.formula.generation) return 5;
                if (frame.kind == Kind::SourceChunk) {
                    if (frame.payload.size() > source_bytes - source.size() ||
                        !source.append(frame.payload.data(), frame.payload.size())) return 6;
                } else if (frame.kind == Kind::End) {
                    if (source.size() != source_bytes || frame.payload.size() ||
                        !snapshot.formula.source.set(reinterpret_cast<const char *>(source.data()), source.size()) ||
                        !respond(snapshot)) return 7;
                    began = false;
                } else return 8;
            }
        }
    }
}

void test_filename_and_planning() {
    Error error;
    Text path;
    assert(export_filename("/home/test", "{index}-{name}", "积分/../公式", 2, ExportFormat::Svg, path, error));
    assert(path == "/home/test/002-积分_公式.svg");
    assert(!export_filename("/home/test", "../{name}", "x", 1, ExportFormat::Png, path, error));
    assert(!export_filename("/home/test", "{unknown}", "x", 1, ExportFormat::Png, path, error));
    Document document;
    FormulaId id;
    assert(add_formula(document, "One", "x", id, error));
    ExportQueue queue;
    ExportOptions options = options_for("/no/such/reliefmath", ExportFormat::Latex);
    assert(queue.start("/unneeded", document, nullptr, 0, 42, options, error));
    assert(set_formula_source(document, id, "changed", error));
    assert(queue.items().size() == 1 && queue.items()[0].snapshot.formula.source == "x");
    cancel(queue);
    assert(!queue.busy() && queue.items()[0].status == ExportStatus::Cancelled);
    pump(queue);
}

void test_start_snapshot_survives_document_edits(const char *worker, const char *base) {
    struct Fixture { ExportFormat format; const char *directory, *first, *second, *first_bytes, *second_bytes; };
    const Fixture fixtures[] = {
        {ExportFormat::Svg, "snapshot-svg", "001-Original.svg", "002-Later.svg", "<svg>x_old</svg>", "<svg>y_old</svg>"},
        {ExportFormat::Png, "snapshot-png", "001-Original.png", "002-Later.png", "\x89PNG\r\n\x1a\n" "x_old", "\x89PNG\r\n\x1a\n" "y_old"},
        {ExportFormat::Latex, "snapshot-tex", "001-Original.tex", "002-Later.tex", "x_old\n", "y_old\n"}
    };
    for (const Fixture &fixture : fixtures) {
        Text directory = make_directory(base, fixture.directory);
        Document document;
        FormulaId first, second;
        Error error;
        assert(add_formula(document, "Original", "x_old", first, error));
        assert(add_formula(document, "Later", "y_old", second, error));
        Style original;
        original.point_size = 32;
        original.padding = 8;
        original.rgba = 0x112233ff;
        original.display = false;
        assert(set_formula_style(document, first, original, error));
        const uint64_t generation = document.formulas[0].generation;
        ExportOptions options = options_for(directory.c_str(), fixture.format);
        options.scale = 1.5;
        options.dpi = 144;
        ExportQueue queue;
        assert(queue.start(worker, document, nullptr, 0, 77, options, error));
        pump(queue); // Begin the first item before changing the live document.
        assert(queue.items()[0].status == (fixture.format == ExportFormat::Latex ?
                                          ExportStatus::Writing : ExportStatus::Rendering));
        assert(set_formula_source(document, first, "x_new", error));
        assert(rename_formula(document, first, "Renamed", error));
        assert(set_formula_style(document, first, Style{}, error));
        assert(delete_formula(document, second, error));
        FormulaId added;
        assert(add_formula(document, "Added", "z_new", added, error));
        assert(options.directory.set("/no/such/reliefmath"));
        assert(options.pattern.set("changed"));
        finish(queue);
        assert(queue.items().size() == 2);
        const Snapshot &snapshot = queue.items()[0].snapshot;
        assert(snapshot.formula.id == first && snapshot.formula.generation == generation);
        assert(snapshot.formula.name == "Original" && snapshot.formula.source == "x_old");
        assert(snapshot.formula.style.point_size == 32 && snapshot.formula.style.padding == 8);
        assert(snapshot.formula.style.rgba == 0x112233ff && !snapshot.formula.style.display);
        assert(snapshot.session_id == 77 && snapshot.scale == 1.5 && snapshot.dpi == 144);
        assert(queue.items()[1].snapshot.formula.id == second);
        assert(queue.items()[0].status == ExportStatus::Done && queue.items()[1].status == ExportStatus::Done);
        Text first_path = path_in(directory.c_str(), fixture.first);
        Text second_path = path_in(directory.c_str(), fixture.second);
        expect_file(first_path.c_str(), fixture.first_bytes);
        expect_file(second_path.c_str(), fixture.second_bytes);
        assert(unlink(first_path.c_str()) == 0 && unlink(second_path.c_str()) == 0);
        assert(rmdir(directory.c_str()) == 0);
    }
}

void test_batch_continues_after_individual_failures(const char *worker, const char *base) {
    Text directory = make_directory(base, "batch");
    Text conflict = path_in(directory.c_str(), "003-Collision.svg");
    FILE *existing = fopen(conflict.c_str(), "wb");
    assert(existing && fputs("keep existing", existing) >= 0 && fclose(existing) == 0);
    Document document;
    FormulaId id;
    Error error;
    assert(add_formula(document, "Bad", "fail", id, error));
    assert(add_formula(document, "Good", "good", id, error));
    assert(add_formula(document, "Collision", "blocked", id, error));
    assert(add_formula(document, "Tail", "tail", id, error));
    ExportQueue queue;
    ExportOptions options = options_for(directory.c_str());
    assert(queue.start(worker, document, nullptr, 0, 88, options, error));
    finish(queue);
    assert(queue.items()[0].status == ExportStatus::Failed && queue.items()[0].error.code == Code::Parse);
    assert(queue.items()[0].error.begin == 1 && queue.items()[0].error.end == 3);
    assert(queue.items()[0].snapshot.formula.source == "fail");
    assert(queue.items()[1].status == ExportStatus::Done && queue.items()[1].error.code == Code::None);
    assert(queue.items()[2].status == ExportStatus::Failed && queue.items()[2].error.code == Code::Conflict);
    assert(queue.items()[3].status == ExportStatus::Done && queue.items()[3].error.code == Code::None);
    Text bad = path_in(directory.c_str(), "001-Bad.svg");
    Text good = path_in(directory.c_str(), "002-Good.svg");
    Text tail = path_in(directory.c_str(), "004-Tail.svg");
    expect_absent(bad.c_str());
    expect_file(good.c_str(), "<svg>good</svg>");
    expect_file(conflict.c_str(), "keep existing");
    expect_file(tail.c_str(), "<svg>tail</svg>");
    assert(unlink(good.c_str()) == 0 && unlink(conflict.c_str()) == 0 && unlink(tail.c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0);
}

void test_cancelled_inactive_queue_reaps_worker(const char *worker, const char *base) {
    Text directory = make_directory(base, "cancel");
    Text pid_path = path_in(directory.c_str(), "worker.pid");
    assert(setenv("RM_TEST_EXPORT_PID_FILE", pid_path.c_str(), 1) == 0);
    Document document;
    FormulaId id;
    Error error;
    assert(add_formula(document, "Finished", "before", id, error));
    assert(add_formula(document, "Waiting", "hold", id, error));
    assert(add_formula(document, "Pending", "after", id, error));
    ExportQueue queue;
    ExportOptions options = options_for(directory.c_str());
    assert(queue.start(worker, document, nullptr, 0, 99, options, error));
    long child_number = -1;
    const uint64_t deadline = milliseconds() + 8000;
    while (child_number <= 0 && milliseconds() < deadline) {
        pump(queue);
        FILE *file = fopen(pid_path.c_str(), "r");
        if (file) {
            if (fscanf(file, "%ld", &child_number) != 1) child_number = -1;
            assert(fclose(file) == 0);
        }
        usleep(1000);
    }
    assert(child_number > 0);
    const pid_t child = pid_t(child_number);
    assert(queue.busy() && queue.position() == 1);
    assert(queue.items()[0].status == ExportStatus::Done);
    assert(queue.items()[1].status == ExportStatus::Rendering);
    deferred_child = child;
    deferred_waits = 0;
    cancel(queue);
    assert(deferred_waits > 0); // cancel definitely did not reap, regardless of scheduling.
    assert(!queue.busy() && queue.position() == 3);
    for (unsigned n = 0; n < 3; ++n) pump(queue);
    siginfo_t status{};
    const uint64_t exit_deadline = milliseconds() + 8000;
    do {
        assert(waitid(P_PID, id_t(child), &status, WEXITED | WNOHANG | WNOWAIT) == 0);
        if (status.si_pid == child) break;
        usleep(1000);
    } while (milliseconds() < exit_deadline);
    assert(status.si_pid == child && status.si_code == CLD_KILLED && status.si_status == SIGKILL);
    deferred_child = -1;
    pump(queue); // The child is now a known zombie; only ExportQueue can reap it here.
    errno = 0;
    const int result = waitid(P_PID, id_t(child), &status, WEXITED | WNOHANG | WNOWAIT);
    if (result == 0) fputs("inactive ExportQueue left a cancelled renderer zombie\n", stderr);
    assert(result == -1 && errno == ECHILD);
    pump(queue);
    cancel(queue);
    assert(!queue.busy() && queue.position() == 3);
    assert(queue.items()[0].status == ExportStatus::Done);
    assert(queue.items()[1].status == ExportStatus::Cancelled);
    assert(queue.items()[2].status == ExportStatus::Cancelled);
    Text finished = path_in(directory.c_str(), "001-Finished.svg");
    Text waiting = path_in(directory.c_str(), "002-Waiting.svg");
    Text pending = path_in(directory.c_str(), "003-Pending.svg");
    expect_file(finished.c_str(), "<svg>before</svg>");
    expect_absent(waiting.c_str());
    expect_absent(pending.c_str());
    assert(unsetenv("RM_TEST_EXPORT_PID_FILE") == 0);
    assert(unlink(finished.c_str()) == 0 && unlink(pid_path.c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0);
}
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--render-worker") == 0) return fake_worker();
    assert(argc == 2);
    test_filename_and_planning();
    test_start_snapshot_survives_document_edits(argv[0], argv[1]);
    test_batch_continues_after_individual_failures(argv[0], argv[1]);
    test_cancelled_inactive_queue_reaps_worker(argv[0], argv[1]);
    puts("ReliefMath export snapshot, partial failure, cancellation and idle child reap tests passed");
}
