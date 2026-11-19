#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW

/** @brief Count progress output without writing a real terminal or serial port. */
static int progress_printf(const char *format, ...);
#define printf progress_printf
#include "../../userland/apps/installer/install_ops.c"
#undef printf

static unsigned output_calls, refresh_calls;
static unsigned long test_time;

/** @brief Count status output emitted by production progress presentation. */
static int progress_printf(const char *format, ...)
{
    (void)format;
    ++output_calls;
    return 1;
}

/** @brief Supply a controlled monotonic clock to the presentation throttle. */
unsigned long reliefos_uptime_ms(void) { return test_time; }
/** @brief Count GUI refreshes; the host test owns no application window. */
void refresh_ui(void) { ++refresh_calls; }

/** @brief Short I/O slices must not turn either frontend into a per-slice logger. */
int main(void)
{
    copy_total_bytes = 1024 * 1024;
    for (unsigned tty = 0; tty < 2; ++tty) {
        installer_tty_mode = tty;
        output_calls = refresh_calls = 0;
        test_time = 1000 * (tty + 1);
        for (unsigned i = 0; i < 100; ++i) {
            copy_done_bytes = i * 4096;
            show_copy_progress("/target/file");
        }
        assert(output_calls <= 2);
        assert(refresh_calls == 1u);
        unsigned before = output_calls;
        test_time += COPY_PRESENT_INTERVAL_MS;
        show_copy_progress("/target/next-file");
        assert(output_calls > before);
    }
    /* Advancing the clock between many tiny files must not defeat TTY
     * deduplication merely because the pathname changes at the same percent. */
    installer_tty_mode = 1;
    copy_done_bytes = 0;
    output_calls = 0;
    for (unsigned i = 0; i < 100; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "/target/terminfo/file-%u", i);
        test_time += COPY_PRESENT_INTERVAL_MS;
        show_copy_progress(name);
    }
    assert(output_calls <= 2);
    assert(strstr(detail_text, "/target/terminfo/file-99"));
    unsigned before = output_calls;
    copy_done_bytes = copy_total_bytes / 2;
    test_time += COPY_PRESENT_INTERVAL_MS;
    show_copy_progress("/target/big-file");
    assert(output_calls > before);
    /* Links and empty files take work even with zero newly copied bytes. */
    copy_total = 100;
    copy_done = 50;
    copy_done_bytes = 0;
    test_time += COPY_PRESENT_INTERVAL_MS;
    show_copy_progress("/target/lib/link.so");
    assert(progress_value > 35 && progress_value < 90);
    assert(strstr(detail_text, "50/100"));
    assert(strstr(detail_text, "/target/lib/link.so"));
    copy_done_bytes = copy_total_bytes;
    copy_done = copy_total;
    test_time += COPY_PRESENT_INTERVAL_MS;
    show_copy_progress("/target");
    assert(progress_value == 90);
    puts("PASS installer progress: bounded output and advances on metadata-only work");
    return 0;
}
