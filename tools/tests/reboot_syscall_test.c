#include <reliefnt/console.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_process.c"

static struct task caller;
static struct task *current = &caller;
static jmp_buf transition;
static int action;

struct task *sched_current_task(void) { return current; }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
void power_reboot(void) { action = 1; longjmp(transition, 1); }
void power_shutdown(void) { action = 2; longjmp(transition, 1); }

static void expect_power(uint64_t magic1, uint64_t magic2, uint64_t command, int expected)
{
    action = 0;
    if (!setjmp(transition)) {
        int64_t result = syscall_process_control(LINUX_SYS_REBOOT, magic1, magic2, command, 0);
        fprintf(stderr, "reboot syscall returned %lld instead of power action %d\n",
                (long long)result, expected);
        assert(0);
    }
    assert(action == expected);
}

int main(void)
{
    /* Exact arguments emitted by unmodified musl reboot(int). */
    expect_power(0xfee1deadU, 672274793U, 0x01234567U, 1);
    expect_power(0xfee1deadU, 672274793U, 0x4321fedcU, 2);
    const uint32_t magics[] = {672274793U, 85072278U, 369367448U, 537993216U};
    for (unsigned i = 0; i < sizeof(magics) / sizeof(magics[0]); ++i) {
        expect_power(0xfffffffffee1deadULL, 0x123400000000ULL | magics[i],
                     0x9876000001234567ULL, 1);
    }
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0, 672274793U, 0x01234567U, 0) == -LINUX_EINVAL);
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0xfee1deadU, 0, 0x01234567U, 0) == -LINUX_EINVAL);
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0xfee1deadU, 672274793U, 0x1234, 0) == -LINUX_EINVAL);
    /* The obsolete one-argument raw ABI must not bypass the magic check. */
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0x01234567U, 0, 0, 0) == -LINUX_EINVAL);
    caller.uid = 1000;
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0xfee1deadU, 672274793U, 0x01234567U, 0) == -LINUX_EPERM);
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0, 0, 0, 0) == -LINUX_EPERM);
    current = NULL;
    assert(syscall_process_control(LINUX_SYS_REBOOT, 0xfee1deadU, 672274793U, 0x4321fedcU, 0) == -LINUX_EPERM);
    puts("PASS reboot syscall: musl arguments, magic variants, int widths, invalid requests and privilege gate");
}
