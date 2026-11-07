#include <assert.h>
#include <errno.h>
#include <string.h>
#include "../../userland/apps/installer/installer_tty.c"

static const char *input_text;
static size_t input_position;
static int interrupt_read, restore_error, termios_calls;

ssize_t __wrap_read(int fd, void *buffer, size_t length)
{
    assert(fd == 0 && length == 1);
    if (interrupt_read) { interrupt_read = 0; errno = EINTR; return -1; }
    if (!input_text[input_position]) return 0;
    *(char *)buffer = input_text[input_position++];
    return 1;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t length)
{
    assert(fd == 1);
    (void)buffer;
    return (ssize_t)length;
}

int __wrap_tcgetattr(int fd, struct termios *attributes)
{
    assert(fd == 0);
    memset(attributes, 0, sizeof(*attributes));
    attributes->c_lflag = ECHO | ECHONL | ICANON;
    return 0;
}

int __wrap_tcsetattr(int fd, int action, const struct termios *attributes)
{
    assert(fd == 0 && action == TCSANOW);
    ++termios_calls;
    if (termios_calls & 1) assert(!(attributes->c_lflag & (ECHO | ECHONL)));
    else {
        assert(attributes->c_lflag & ECHO);
        if (restore_error) { errno = EIO; return -1; }
    }
    return 0;
}

static void feed(const char *text)
{
    input_text = text;
    input_position = 0;
    termios_calls = 0;
}

/* Drive the complete TTY parser with real answers; storage callbacks assert
 * that firmware selection already happened before any partition operation. */
static struct installer_setup flow_setup;
static unsigned flow_modes;
static int flow_stage;
static void flow_choices(void)
{
    assert(flow_setup.skip_bios == !(flow_modes & 1));
    assert(flow_setup.skip_uefi == !(flow_modes & 2));
}
static void flow_disks(void) { flow_stage = 1; }
static void flow_disk_line(char *s, uint32_t n, const struct reliefos_block_disk_info *d)
{ (void)d; snprintf(s, n, "fixture disk"); }
static void flow_parts(void)
{ assert(flow_stage == 1 || flow_stage == 3); flow_choices(); if (flow_stage == 1) flow_stage = 2; }
static void flow_part_line(char *s, uint32_t n, const struct reliefos_block_partition *p)
{ (void)p; snprintf(s, n, "fixture partition"); }
static int flow_auto(void) { assert(flow_stage == 2); flow_choices(); flow_stage = 3; return 0; }
static int flow_valid(void) { return flow_stage == 3; }
static int flow_unused(void) { assert(0); return -1; }
static int flow_create(uint32_t f, uint32_t n, const char *s)
{ (void)f; (void)n; (void)s; return flow_unused(); }
static int flow_number(uint32_t n) { (void)n; return flow_unused(); }
static int flow_name(const char *s) { (void)s; return flow_unused(); }
static int flow_format(uint32_t f, const char *s) { (void)f; (void)s; return flow_unused(); }
static void flow_probe(void) { assert(flow_stage == 1); flow_setup.skip_bios = flow_setup.skip_uefi = 0; flow_stage = 2; }
static void flow_install(void)
{ assert(flow_stage == 3 && flow_setup.skip_xorg); flow_choices(); flow_stage = 4; }
static void flow_update(void) { assert(flow_stage == 2); flow_choices(); flow_stage = 4; }
int installer_setup_valid(const struct installer_setup *s)
{ return !strcmp(s->username, "alice") && !strcmp(s->password, "p") &&
         !strcmp(s->password_confirm, "p") && !strcmp(s->root_password, "r") &&
         !strcmp(s->root_password_confirm, "r"); }
int reliefos_system_reboot(void) { assert(0); return -1; }
int reliefos_system_shutdown(void) { assert(0); return -1; }

static void test_tty_flow(int update, unsigned modes)
{
    struct reliefos_block_disk_info disk = {0};
    struct reliefos_block_partition part = {0};
    uint32_t disk_count = 1, part_count = 0;
    int32_t selected = -1, root = -1, esp = -1, selected_disk = -1;
    uint8_t automatic = 1, mode = 0, success = 0, page = 0;
    struct installer_tty_context context = {
        &flow_setup, &disk, &disk_count, &part, &part_count, &selected, &root, &esp,
        &automatic, &selected_disk, &mode, &success, &page,
        flow_disks, flow_disk_line, flow_parts, flow_part_line, flow_valid, flow_auto,
        flow_unused, flow_create, flow_number, flow_name, flow_number, flow_unused,
        flow_format, flow_probe, flow_install, flow_update,
    };
    char answers[192];
    memset(&flow_setup, 0, sizeof(flow_setup));
    flow_modes = modes;
    snprintf(answers, sizeof(answers), "%s\n0\n%s\n%s\n%s",
        update ? "update" : "install", modes & 1 ? "y" : "n", modes & 2 ? "y" : "n",
        update ? "UPDATE\n" : "auto\ndone\nalice\np\np\nr\nr\nn\nn\nINSTALL\n");
    feed(answers);
    /* Leave success false to return immediately after the execution callback. */
    assert(installer_tty_main(&context) == 1 && flow_stage == 4);
    assert(input_position == strlen(answers) && selected_disk == 0);
}

int main(void)
{
    char output[RELIEFOS_AUTH_PASSWORD_LEN], overlong[140];
    struct installer_setup setup = {0};
    feed("U!lower\n");
    interrupt_read = 1;
    assert(tty_secret(NULL, output, sizeof(output)) == 1);
    assert(!strcmp(output, "U!lower") && termios_calls == 2);
    for (unsigned i = 0; i < 32; ++i) memcpy(overlong + i * 4, "\xf0\x9f\x94\x91", 4);
    strcpy(overlong + 128, "x\nnext\n");
    feed(overlong);
    assert(tty_secret(NULL, output, sizeof(output)) == 0 && errno == EOVERFLOW);
    assert(tty_read_line(NULL, output, sizeof(output)) == 1);
    assert(!strcmp(output, "next"));
    feed("r\n");
    restore_error = 1;
    assert(tty_secret(NULL, output, sizeof(output)) == 0 && errno == EIO);
    restore_error = 0;

    feed("\n");
    setup.skip_xorg = 1;
    assert(tty_install_xorg(&setup) == 1 && setup.skip_xorg == 0);
    feed("YeS\n");
    setup.skip_xorg = 1;
    assert(tty_install_xorg(&setup) == 1 && setup.skip_xorg == 0);
    feed("nO\n");
    setup.skip_xorg = 0;
    assert(tty_install_xorg(&setup) == 1 && setup.skip_xorg == 1);
    feed("maybe\nno\n");
    setup.skip_xorg = 0;
    assert(tty_install_xorg(&setup) == 1 && setup.skip_xorg == 1);
    feed("0123456789abcdefghijklmnop\nyes\n");
    setup.skip_xorg = 1;
    assert(tty_install_xorg(&setup) == 1 && setup.skip_xorg == 0);
    feed("");
    setup.skip_xorg = 0;
    assert(tty_install_xorg(&setup) == 0 && setup.skip_xorg == 0);
    for (unsigned modes = 0; modes < 4; ++modes) {
        setup.skip_bios = modes & 1;
        setup.skip_uefi = (modes >> 1) & 1;
        feed("\n\n");
        assert(tty_bootloaders(&setup) == 1);
        assert(setup.skip_bios == (modes & 1) && setup.skip_uefi == ((modes >> 1) & 1));
    }
    feed("invalid\nYeS\nnO\n");
    assert(tty_bootloaders(&setup) == 1 && !setup.skip_bios && setup.skip_uefi);
    feed("\n");setup.swap_enabled=false;
    assert(installer_tty_select_swap(&setup)==1 && !setup.swap_enabled);
    feed("yes\n2049\n");
    assert(installer_tty_select_swap(&setup)==1 && setup.swap_enabled && setup.swap_size_mib==2049);
    feed("yes\n0\n18446744073709551616\n512\n");
    assert(installer_tty_select_swap(&setup)==1 && setup.swap_enabled && setup.swap_size_mib==512);
    feed("no\n");
    assert(installer_tty_select_swap(&setup)==1 && !setup.swap_enabled);
    for (unsigned modes = 0; modes < 4; ++modes) {
        test_tty_flow(0, modes);
        test_tty_flow(1, modes);
    }
    puts("installer input: line handling, Xorg/BIOS/UEFI choices and terminal restoration PASS");
    return 0;
}
