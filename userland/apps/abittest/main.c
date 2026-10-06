/* Linux ABI v1 runtime smoke tests: signal handler frame, Unix98 PTY
 * hangup and evdev EVIOCGRAB/capability queries. */
#include <errno.h>
#include <fcntl.h>
#include <reliefos/syscall.h>
#include <linux/input.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t signal_hits;
static volatile unsigned signal_progress;

static void on_signal(int sig)
{
    (void)sig;
    ++signal_hits;
}

static int test_signal(void)
{
    struct sigaction action;
    struct sigaction previous;
    sigset_t mask;
    sigset_t empty;
    unsigned expected;

    signal_hits = 0;
    action = (struct sigaction){0};
    action.sa_handler = on_signal;
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, &previous) < 0) {
        printf("[abittest] signal FAIL sigaction errno=%d\n", errno);
        return -1;
    }
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, 0) < 0) {
        printf("[abittest] signal FAIL block errno=%d\n", errno);
        return -1;
    }
    expected = signal_progress = 0;
    for (volatile unsigned i = 0; i < 100000U; ++i) signal_progress = i;
    expected = signal_progress;
    if (raise(SIGUSR1) < 0) {
        printf("[abittest] signal FAIL raise pending errno=%d\n", errno);
        return -1;
    }
    if (signal_hits != 0) {
        printf("[abittest] signal FAIL handler ran while blocked\n");
        return -1;
    }
    sigemptyset(&empty);
    if (sigprocmask(SIG_UNBLOCK, &empty, 0) < 0 || signal_hits != 0) {
        printf("[abittest] signal FAIL empty unblock changed pending delivery\n");
        return -1;
    }
    if (sigprocmask(SIG_UNBLOCK, &mask, 0) < 0) {
        printf("[abittest] signal FAIL unblock errno=%d\n", errno);
        return -1;
    }
    if (signal_hits != 1 || signal_progress != expected) {
        printf("[abittest] signal FAIL hits=%d progress=%u expected=%u\n",
               (int)signal_hits, signal_progress, expected);
        return -1;
    }
    if (raise(SIGUSR1) < 0 || signal_hits != 2 || signal_progress != expected) {
        printf("[abittest] signal FAIL direct hits=%d\n", (int)signal_hits);
        return -1;
    }
    action.sa_handler = SIG_DFL;
    (void)sigaction(SIGUSR1, &action, 0);
    printf("[abittest] signal PASS\n");
    return 0;
}

static int test_pty_hangup(void)
{
    int master = -1, slave = -1;
    char byte;
    struct pollfd pfd;

    if (signal(SIGHUP, SIG_IGN) == SIG_ERR) {
        printf("[abittest] pty FAIL ignore SIGHUP errno=%d\n", errno);
        return -1;
    }
    if (openpty(&master, &slave, 0, 0, 0) < 0) {
        printf("[abittest] pty FAIL openpty errno=%d\n", errno);
        return -1;
    }
    if (write(master, "A", 1) != 1) {
        printf("[abittest] pty FAIL master write errno=%d\n", errno);
        return -1;
    }
    close(master);
    master = -1;
    if (read(slave, &byte, 1) != 1 || byte != 'A') {
        printf("[abittest] pty FAIL drain queued byte='%c'\n", byte);
        return -1;
    }
    if (read(slave, &byte, 1) != 0) {
        printf("[abittest] pty FAIL hangup EOF errno=%d\n", errno);
        return -1;
    }
    pfd.fd = slave;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLHUP)) {
        printf("[abittest] pty FAIL poll revents=0x%x\n", pfd.revents);
        return -1;
    }
    errno = 0;
    if (write(slave, "B", 1) != -1 || errno != EIO) {
        printf("[abittest] pty FAIL slave write errno=%d\n", errno);
        return -1;
    }
    close(slave);
    printf("[abittest] pty PASS\n");
    return 0;
}

static int test_evdev(void)
{
    int first = open("/dev/input/event0", O_RDWR, 0);
    int second = open("/dev/input/event0", O_RDWR, 0);
    unsigned char bits[KEY_CNT / 8 + 1];
    unsigned char keys[16];
    char name[64];

    if (first < 0 || second < 0) {
        printf("[abittest] evdev FAIL open first=%d second=%d\n", first, second);
        return -1;
    }
    if (syscall3(SYS_ioctl, first, EVIOCGNAME(sizeof(name)), (long)name) < 0 ||
        syscall3(SYS_ioctl, first, EVIOCGBIT(EV_KEY, sizeof(bits)), (long)bits) < 0 ||
        syscall3(SYS_ioctl, first, EVIOCGKEY(sizeof(keys)), (long)keys) < 0) {
        printf("[abittest] evdev FAIL capability query errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, second, EVIOCGRAB, 0) >= 0 || errno != EINVAL) {
        printf("[abittest] evdev FAIL unowned release errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, first, EVIOCGRAB, 2) < 0) {
        printf("[abittest] evdev FAIL first grab errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, first, EVIOCGRAB, 1) >= 0 || errno != EBUSY) {
        printf("[abittest] evdev FAIL repeated grab errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, second, EVIOCGRAB, 1) >= 0 ||
        errno != EBUSY) {
        printf("[abittest] evdev FAIL second grab errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, first, EVIOCGRAB, 0) < 0) {
        printf("[abittest] evdev FAIL release grab errno=%d\n", errno);
        return -1;
    }
    if (syscall3(SYS_ioctl, second, EVIOCGRAB, 1) < 0) {
        printf("[abittest] evdev FAIL re-grab errno=%d\n", errno);
        return -1;
    }
    close(first);
    close(second);
    printf("[abittest] evdev PASS\n");
    return 0;
}

int main(void)
{
    int result = 0;
    puts("[abittest] Linux ABI v1 runtime tests starting");
    result |= test_signal() < 0;
    result |= test_pty_hangup() < 0;
    result |= test_evdev() < 0;
    puts(result ? "[abittest] FAIL" : "[abittest] ALL PASS");
    return result ? 1 : 0;
}
