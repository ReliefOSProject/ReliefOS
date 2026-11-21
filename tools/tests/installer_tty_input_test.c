/* The real ncurses widgets and fork/pipe transport run on the host. Storage
 * callbacks represent a disposable disk, so keyboard tests cannot erase one. */
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncurses.h>
#include <assert.h>
#include <stdarg.h>
#include <wchar.h>
#include <locale.h>
#include <sys/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW
#include "../../userland/apps/installer/install_ops.c"
#include "../../userland/apps/installer/installer_tui.h"

int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (!strcmp(path, "/var/log/installer.log") && getenv("INSTALLER_TEST_LOG")) path = getenv("INSTALLER_TEST_LOG");
    return __real_open(path, flags, mode);
}

void refresh_ui(void) { installer_tui_publish_progress(); }
unsigned long reliefos_uptime_ms(void) { return 1000; }
int reliefos_system_reboot(void) { errno = EPERM; return -1; }
int reliefos_system_shutdown(void) { errno = EPERM; return -1; }
int reliefos_block_partition_uuid(const char *disk, uint32_t index, char uuid[37])
{ (void)disk; (void)index; strcpy(uuid, "00000000-0000-0000-0000-000000000001"); return 0; }
int reliefos_block_partition_path(const char *disk, uint32_t index, char *path, uint32_t capacity)
{
    int result = snprintf(path, capacity, "%s%u", disk, index + 1);
    return result < 0 || (unsigned)result >= capacity ? -ENAMETOOLONG : 0;
}
const char *reliefos_block_gpt_type_name(uint32_t type) { (void)type; return "fixture"; }
const char *reliefos_block_filesystem_name(uint32_t filesystem)
{
    if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32) return "FAT32";
    if (filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4) return "ext4";
    return "unknown";
}
int installer_setup_valid(const struct installer_setup *value)
{
    return *value->username && *value->password && !strcmp(value->password, value->password_confirm) &&
        *value->root_password && !strcmp(value->root_password, value->root_password_confirm);
}

static int abnormal_worker, update_failure, install_failure;
static void record_settings(void)
{
    const char *path = getenv("INSTALLER_TEST_RESULT");
    if (!path) return;
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "user=%s xorg=%u bios=%u uefi=%u swap=%u size=%llu\n", setup.username,
        setup.desktop != INSTALLER_DESKTOP_CONSOLE, !setup.skip_bios, !setup.skip_uefi, setup.swap_enabled,
        (unsigned long long)setup.swap_size_mib);
    fclose(file);
}
static int fixture_install(void)
{
    if (abnormal_worker) _exit(7);
    if (install_failure) {
        install_success = 0;
        strcpy(status_text, "Installation failed");
        strcpy(detail_text, "Copy failed ret=-30");
        return -EROFS;
    }
    progress_value = 50;
    strcpy(status_text, "Copying fixture files");
    refresh_ui();
    record_settings();
    install_success = 1;
    page = INSTALLER_PAGE_FINISH;
    return 0;
}
static void fixture_disks(void)
{
    disk_count = 1;
    strcpy(disks[0].path, "/dev/fixture");
    strcpy(disks[0].name, "Disposable test disk");
    disks[0].sector_count = 16777216;
    disks[0].sector_size = 512;
    selected_disk = 0;
}
static void fixture_partitions(void)
{
    partition_count = 0; selected_partition = -1;
    if (!getenv("INSTALLER_TEST_PARTITIONS")) return;
    /* Deliberately unordered GPT slots, with a 512 MiB gap before root. */
    partitions[0] = (struct reliefos_block_partition){
        .index = 7, .filesystem = RELIEFOS_BLOCK_FILESYSTEM_EXT4,
        .gpt_type = RELIEFOS_BLOCK_GPT_LINUX, .first_lba = 1314816,
        .sector_count = 15460352, .path = "/dev/fixture8", .name = "中文系统分区"};
    partitions[1] = (struct reliefos_block_partition){
        .index = 1, .gpt_type = RELIEFOS_BLOCK_GPT_BIOS_BOOT,
        .first_lba = 2048, .sector_count = 2048, .path = "/dev/fixture2", .name = "BIOS"};
    partitions[2] = (struct reliefos_block_partition){
        .index = 4, .filesystem = RELIEFOS_BLOCK_FILESYSTEM_FAT32,
        .gpt_type = RELIEFOS_BLOCK_GPT_ESP, .first_lba = 4096,
        .sector_count = 262144, .path = "/dev/fixture5", .name = "EFI"};
    partition_count = 3; selected_partition = 0;
    installer_root_partition = 7; installer_esp_partition = 4;
}
void __wrap_installer_partition_reset(void) { installer_partition_auto = 1; fixture_partitions(); }
int __wrap_installer_partition_error(void) { return 0; }
int __wrap_installer_partition_disk_error(void) { return 0; }
int __wrap_installer_partition_select_disk(int32_t choice)
{
    selected_disk = choice;
    fixture_partitions();
    return 0;
}
int __wrap_installer_partition_select_mode(uint8_t mode)
{
    if (install_mode != mode) {
        install_mode = mode;
        installer_partition_auto = 1;
        fixture_partitions();
    }
    return 0;
}
int __wrap_installer_partition_changes(char *out, uint32_t capacity)
{
    if (capacity) snprintf(out, capacity, "Pending partition plan");
    return getenv("INSTALLER_TEST_SUMMARY_FAILURE") ? -ENOSPC : 0;
}

static int fixture_valid(void) { return installer_partition_auto; }
static int fixture_noop(void) { return 0; }
static int fixture_create(uint32_t filesystem, uint32_t size, const char *name)
{ (void)filesystem; (void)size; (void)name; return 0; }
static int fixture_size(uint32_t size) { (void)size; return 0; }
static int fixture_name(const char *name) { (void)name; return 0; }
/** @brief Record which real GPT slot receives a keyboard-selected operation.
 * @param name Borrowed proposed partition name. @return Zero on success.
 */
static int fixture_rename(const char *name)
{
    const char *path = getenv("INSTALLER_TEST_OPERATION");
    if (!path) return fixture_name(name);
    assert(selected_partition >= 0 && (uint32_t)selected_partition < partition_count);
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "slot=%u name=%s\n", partitions[selected_partition].index, name);
    assert(!fclose(file));
    return 0;
}
static int fixture_format(uint32_t filesystem, const char *name) { (void)filesystem; (void)name; return 0; }
static struct installer_tty_context fixture_context = {
    &setup, disks, &disk_count, partitions, &partition_count, &selected_partition,
    &installer_root_partition, &installer_esp_partition, &installer_partition_auto,
    &selected_disk, &install_mode, &install_success, &page, fixture_disks,
    format_disk_line, fixture_partitions, format_partition_line, fixture_valid,
    fixture_noop, fixture_noop, fixture_create, fixture_size, fixture_rename,
    fixture_size, fixture_noop, fixture_format, fixture_install, installer_boot_entry_path
};

int __real_doupdate(void);
/** @brief Capture the real ncurses screen for externally driven PTY assertions.
 * @return The underlying ncurses refresh result.
 */
int __wrap_doupdate(void)
{
    int result = __real_doupdate();
    const char *path = getenv("INSTALLER_TEST_SCREEN");
    if (!path) return result;
    char temporary[4096];
    assert(snprintf(temporary, sizeof(temporary), "%s.tmp", path) < (int)sizeof(temporary));
    FILE *file = fopen(temporary, "w");
    assert(file);
    for (int row = 0; row < LINES; ++row) {
        for (int column = 0; column < COLS; ++column) {
            cchar_t cell;
            wchar_t characters[CCHARW_MAX];
            attr_t attributes;
            short color;
            assert(mvwin_wch(curscr, row, column, &cell) != ERR);
            assert(getcchar(&cell, characters, &attributes, &color, NULL) != ERR);
            fprintf(file, "%ls", characters);
            if (wcwidth(characters[0]) == 2) ++column;
        }
        fputc('\n', file);
    }
    assert(!fclose(file));
    assert(!rename(temporary, path));
    return result;
}
int installer_update_check_target(const char *disk, const char *root, const char *esp,
    struct installer_update_info *info, installer_update_observer observer, void *context)
{
    (void)root; (void)esp;
    memset(info, 0, sizeof(*info));
    snprintf(info->disk, sizeof(info->disk), "%s", disk);
    strcpy(info->root_device, "/dev/fixture2");
    strcpy(info->esp_device, "/dev/fixture1");
    strcpy(info->installed_version, "5.3.1");
    strcpy(info->media_version, "5.3.2");
    strcpy(info->root_uuid, "00000000-0000-0000-0000-000000000001");
    strcpy(info->index_sha256, "verified-fixture-index");
    info->boot_modes = 3;
    observer(context, 100, "Update checks completed");
    return 0;
}
int installer_update_apply(const struct installer_update_info *expected, unsigned modes,
    struct installer_update_info *result, installer_update_observer observer, void *context)
{
    if (strcmp(expected->index_sha256, "verified-fixture-index")) return -ESTALE;
    *result = *expected;
    observer(context, 50, "Applying signed system packages");
    if (update_failure) {
        strcpy(result->failure, "Applying signed system packages");
        result->error_number = EPROTO;
        return -EPROTO;
    }
    setup.skip_bios = !(modes & 1); setup.skip_uefi = !(modes & 2);
    record_settings();
    return 0;
}

/** @brief Reject an invalid snapshot without retaining a hidden operation target. */
static void partition_snapshot_tests(void)
{
    fixture_disks();
    partition_count = 1;
    partitions[0] = (struct reliefos_block_partition){.index = 0,
        .first_lba = 2048, .sector_count = 2048, .path = "/dev/fixture1"};
    int chosen = 0;
    struct tui_partition_table table = {0};
    assert(tui_partition_table_update(&table, &fixture_context, 0, &chosen) == 0);
    assert(chosen == 0);
    partitions[0].sector_count = UINT64_MAX;
    assert(tui_partition_table_update(&table, &fixture_context, 0, &chosen) == -EINVAL);
    assert(table.count == 0 && chosen == -1);
    partition_count = 0;
}

static void edit_tests(void)
{
    char value[160] = "";
    int cursor = 0;
    assert(tui_edit_value(value, sizeof(value), &cursor, L'中', 0, 0));
    assert(tui_edit_value(value, sizeof(value), &cursor, L'a', 0, 0));
    assert(tui_edit_value(value, sizeof(value), &cursor, L'文', 0, 0));
    assert(!strcmp(value, "中a文") && cursor == 7);
    assert(tui_edit_value(value, sizeof(value), &cursor, KEY_LEFT, 1, 0));
    assert(tui_edit_value(value, sizeof(value), &cursor, KEY_BACKSPACE, 1, 0));
    assert(!strcmp(value, "中文") && cursor == 3);
    assert(tui_edit_value(value, sizeof(value), &cursor, KEY_DC, 1, 0));
    assert(!strcmp(value, "中"));
    assert(tui_edit_value(value, sizeof(value), &cursor, 21, 0, 1));
    for (int i = 0; i < 32; ++i) assert(tui_edit_value(value, sizeof(value), &cursor, L'日', 0, 1));
    assert(!tui_edit_value(value, sizeof(value), &cursor, L'x', 0, 1));
    assert(!tui_edit_value(value, sizeof(value), &cursor, L' ', 0, 1));
    assert(tui_edit_value(value, sizeof(value), &cursor, KEY_BACKSPACE, 1, 1));
    assert(cursor == 93 && strlen(value) == 93);
    char small[4] = ""; cursor = 0;
    assert(tui_edit_value(small, sizeof(small), &cursor, L'中', 0, 0));
    assert(!tui_edit_value(small, sizeof(small), &cursor, L'x', 0, 0));
    assert(!strcmp(small, "中"));
}

static void screen_tests(void)
{
    FILE *input = fopen("/dev/null", "r"), *output = fopen("/dev/null", "w");
    assert(input && output);
    SCREEN *screen = newterm("xterm", output, input);
    assert(screen);
    noecho(); keypad(stdscr, TRUE); start_color();
    assert(resizeterm(24, 80) != ERR);
    struct tui_view view = {.previous_focus = -1};
    char secret[160] = "fixture-secret";
    tui_begin(&view, "Account");
    tui_input(&view, "Password", secret, sizeof(secret), 1);
    unget_wch(L'中');
    assert(tui_step(&view, "Next", 1, 1, 0) == TUI_NONE);
    assert(!strcmp(secret, "fixture-secret中"));
    /* The screen contains masking characters, while the buffer retains UTF-8. */
    assert((mvwinch(curscr, 3, 3) & A_CHARTEXT) == '*');
    ungetch(KEY_F(2));
    assert(tui_step(&view, "Next", 1, 1, 0) == TUI_BACK);
    ungetch(KEY_F(3));
    assert(tui_step(&view, "Next", 1, 1, 0) == TUI_NEXT);
    assert(resizeterm(18, 50) != ERR);
    ungetch(KEY_F(3));
    assert(tui_step(&view, "Next", 1, 1, 0) == TUI_NONE);
    assert(resizeterm(24, 80) != ERR);
    struct tui_job job = {.kind = TUI_JOB_CHECK};
    fixture_disks();
    assert(tui_run_job(&fixture_context, &job) == 0);
    assert(!strcmp(job.info.index_sha256, "verified-fixture-index"));
    job.kind = TUI_JOB_UPDATE;
    assert(tui_run_job(&fixture_context, &job) == 0);
    assert(!strcmp(status_text, "Applying signed system packages"));
    update_failure = 1;
    assert(tui_run_job(&fixture_context, &job) == -EPROTO);
    assert(strstr(detail_text, strerror(EPROTO)));
    update_failure = 0;
    job.kind = TUI_JOB_INSTALL;
    assert(tui_run_job(&fixture_context, &job) == 0 && install_success);
    install_failure = 1;
    assert(tui_run_job(&fixture_context, &job) == -EROFS && !install_success);
    assert(!strcmp(status_text, "Installation failed"));
    assert(strstr(detail_text, strerror(EROFS)));
    assert(strstr(detail_text, "Copy failed ret=-30"));
    install_failure = 0;
    abnormal_worker = 1;
    assert(tui_run_job(&fixture_context, &job) == -EIO);
    assert(!strcmp(status_text, "Operation failed"));
    endwin(); delscreen(screen); fclose(input); fclose(output);
}


int main(int argc, char **argv)
{
    assert(setlocale(LC_ALL, "C.UTF-8"));
    if (argc == 2 && !strcmp(argv[1], "--interactive")) {
        install_failure = getenv("INSTALLER_TEST_INSTALL_FAILURE") != NULL;
        return installer_tty_main(&fixture_context);
    }
    partition_snapshot_tests(); edit_tests(); screen_tests();
    puts("PASS ncurses UTF-8 editing, password masking, navigation, resize and worker result/abnormal exit");
    return 0;
}
