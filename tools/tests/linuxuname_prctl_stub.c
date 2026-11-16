/* Host command tests only; kernel behavior is covered by ReliefNT probes. */
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <reliefos/prctl.h>

int prctl(int option, ...)
{
    va_list args;
    va_start(args, option);
    unsigned long value = va_arg(args, unsigned long);
    unsigned long third = va_arg(args, unsigned long);
    unsigned long fourth = va_arg(args, unsigned long);
    unsigned long fifth = va_arg(args, unsigned long);
    va_end(args);
    if (option != RELIEFOS_PR_SET_UNAME_LINUX || value != 1 || third || fourth || fifth ||
        getenv("LINUXUNAME_PRCTL_FAIL")) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
