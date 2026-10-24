#include "document.h"
#include "formula.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static double seconds() {
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void make_csv(const char *path) {
    FILE *file = fopen(path, "wb");
    assert(file);
    for (int row = 0; row < 10000; ++row) {
        for (int column = 0; column < 20; ++column) {
            if (column) assert(fputc(',', file) != EOF);
            assert(fprintf(file, "%d", row * 20 + column) > 0);
        }
        assert(fwrite("\r\n", 1, 2, file) == 2);
    }
    assert(fclose(file) == 0);
}

static void report(const char *name, double start, double end) {
    printf("%s: %.3f s\n", name, end - start);
}

static long file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fclose(file) == 0);
    return size;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char source[1200], exported[1200], native[1200];
    assert(snprintf(source, sizeof(source), "%s/scale.csv", argv[1]) > 0);
    assert(snprintf(exported, sizeof(exported), "%s/scale-export.csv", argv[1]) > 0);
    assert(snprintf(native, sizeof(native), "%s/scale.rexl", argv[1]) > 0);
    make_csv(source);

    rx::Text error;
    rx::Workbook imported;
    double start = seconds();
    assert(rx::import_csv(source, imported, error));
    report("CSV import", start, seconds());
    assert(imported.cell_count() == 200000);
    const uint32_t data_id = imported.active_id;
    assert(imported.sheet(data_id)->name.set("Data"));
    rx::Sheet summary;
    summary.id = imported.next_id++;
    assert(summary.name.set("Summary"));
    const uint32_t summary_id = summary.id;
    assert(imported.sheets.push(static_cast<rx::Sheet &&>(summary)));
    assert(imported.set_input(summary_id, 0, 0, "Total", true));
    assert(imported.set_input(summary_id, 0, 1, "=SUM(Data!A1:T10000)"));
    assert(imported.set_input(summary_id, 1, 0, "Average", true));
    assert(imported.set_input(summary_id, 1, 1, "=AVERAGE(Data!A1:T10000)"));
    assert(imported.set_input(summary_id, 2, 0, "Count", true));
    assert(imported.set_input(summary_id, 2, 1, "=COUNT(Data!A1:T10000)"));
    assert(rx::recalculate(imported));
    assert(imported.get(summary_id, 0, 1)->result.number == 19999900000.0);
    assert(imported.get(summary_id, 1, 1)->result.number == 99999.5);
    assert(imported.get(summary_id, 2, 1)->result.number == 200000.0);

    start = seconds();
    assert(rx::export_csv(exported, imported, data_id, error));
    report("CSV export", start, seconds());

    rx::Workbook csv_round_trip;
    start = seconds();
    assert(rx::import_csv(exported, csv_round_trip, error));
    report("CSV reimport", start, seconds());
    assert(csv_round_trip.cell_count() == 200000);

    start = seconds();
    assert(rx::save_workbook(native, imported, error));
    report("Native save", start, seconds());

    rx::Workbook native_round_trip;
    start = seconds();
    assert(rx::load_workbook(native, native_round_trip, error));
    report("Native load", start, seconds());
    assert(native_round_trip.cell_count() == 200006);
    assert(native_round_trip.get(native_round_trip.active_id, 9999, 19)->input == "199999");
    assert(native_round_trip.sheets.size() == 2);
    assert(native_round_trip.sheet(summary_id)->name == "Summary");
    assert(native_round_trip.get(summary_id, 0, 1)->result.number == 19999900000.0);
    assert(native_round_trip.get(summary_id, 1, 1)->result.number == 99999.5);
    assert(native_round_trip.get(summary_id, 2, 1)->result.number == 200000.0);
    printf("data_cells=200000 total_cells=200006 sheets=2 source_bytes=%ld csv_bytes=%ld native_bytes=%ld\n",
           file_size(source), file_size(exported), file_size(native));
    return 0;
}
