#include <reliefnt/console.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/block/core/storage_internal.h"
#include "../../kernel/reliefnt/block/core/storage_state_data.c"

static int task_file_errors;
static int timeouts;
bool storage_async_can_yield(void) { return storage_io_async_context; }
void storage_memzero(void *out, size_t n) { memset(out, 0, n); }
uint64_t time_ticks(void) { return 1; }
uint32_t sched_current_pid(void) { return 7; }
void storage_release_task_io(uint32_t pid) { (void)pid; }
static void fixture_console_vprintf(const char *format, va_list arguments)
{
    if (strstr(format, "ahci %s")) {
        const char *reason = va_arg(arguments, const char *);
        task_file_errors += !strcmp(reason, "task-file error");
        timeouts += strstr(reason, "timeout") != NULL;
    }
}

void console_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}

#include "../../kernel/reliefnt/drivers/storage/ahci/storage_ahci.c"

int main(void)
{
    struct ahci_hba_port port = {0};
    for (int asynchronous = 0; asynchronous < 2; ++asynchronous) {
        storage_io_async_context = asynchronous;
        port.ci = 1;
        port.is = AHCI_PORT_IS_TFES;
        ahci_pending_command.port = &port;
        ahci_pending_command.active = 1;
        ahci_pending_command.start_tick = 1;
        assert(ahci_pending_poll() == -5);
        assert(!ahci_pending_command.active);
        assert(port.ci == 1); /* Software must not clear a hardware-owned bit. */
        assert(!timeouts && task_file_errors == asynchronous + 1);
    }
    port.ci = 0;
    port.is = 0;
    ahci_pending_command.port = &port;
    ahci_pending_command.active = 1;
    assert(ahci_pending_poll() == 0 && !ahci_pending_command.active);
    port.ci = 1;
    ahci_pending_command.port = &port;
    ahci_pending_command.active = 1;
    ahci_pending_command.start_tick = 1;
    assert(ahci_pending_poll() == -RELIEFOS_EAGAIN && ahci_pending_command.active);
    puts("AHCI completion: error with PxCI set, success and asynchronous pending PASS");
}
