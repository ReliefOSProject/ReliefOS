#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>

#define T(text) gettext(text)

static void init_localization(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("shell", RELIEFOS_LAYOUT_LOCALE);
    bind_textdomain_codeset("shell", "UTF-8");
    textdomain("shell");
}

#include <dirent.h>
#include <errno.h>
#include <reliefos/fs.h>
#include <reliefos/launch.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHELL_LINE_CAP 192U
#define SHELL_ARG_CAP 12U

static int shell_same(const char *left, const char *right)
{
    if (!left || !right) {
        return 0;
    }
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static void shell_write(const char *text)
{
    if (text) {
        write(1, text, strlen(text));
    }
}

static void shell_line(const char *text)
{
    shell_write(text);
    shell_write("\n");
}

static void shell_prompt(void)
{
    char cwd[RELIEFOS_FS_PATH_LEN];
    shell_write("\x1b[96mreliefos\x1b[0m:");
    if (getcwd(cwd, sizeof(cwd))) {
        shell_write(cwd);
    } else {
        shell_write("?");
    }
    shell_write("$ ");
}

static int shell_split(char *line, char *argv[SHELL_ARG_CAP + 1U])
{
    uint32_t argc = 0;
    char *cursor = line;
    while (cursor && *cursor) {
        char quote = 0;
        uint8_t quote_closed = 0;
        char *start;
        char *write_cursor;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }
        if (argc == SHELL_ARG_CAP) {
            return -2;
        }
        start = cursor;
        write_cursor = cursor;
        if (*cursor == '\'' || *cursor == '"') {
            quote = *cursor++;
            start = cursor;
            write_cursor = cursor;
        }
        argv[argc++] = start;
        while (*cursor) {
            if (quote) {
                if (*cursor == quote) {
                    ++cursor;
                    quote_closed = 1;
                    break;
                }
            } else if (*cursor == ' ' || *cursor == '\t') {
                break;
            }
            if (*cursor == '\\' && cursor[1]) {
                ++cursor;
            }
            *write_cursor++ = *cursor++;
        }
        if (quote && !quote_closed) {
            return -1;
        }
        *write_cursor = 0;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
    }
    argv[argc] = 0;
    return (int)argc;
}

static void shell_list(const char *path)
{
    const char *target = path && path[0] ? path : ".";
    DIR *dir = opendir(target);
    struct dirent *entry;
    if (!dir) {
        printf(T("ls: cannot open %s (%d)\n"), target, errno);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[RELIEFOS_FS_PATH_LEN];
        struct stat st;
        const char *kind = "file";
        uint32_t pos = 0;
        child[0] = 0;
        while (target[pos] && pos + 1U < sizeof(child)) {
            child[pos] = target[pos];
            ++pos;
        }
        if (pos && child[pos - 1] != '/' && pos + 2U < sizeof(child)) {
            child[pos++] = '/';
        }
        if (pos + 1U < sizeof(child)) {
            uint32_t name = 0;
            while (entry->d_name[name] && pos + 1U < sizeof(child)) {
                child[pos++] = entry->d_name[name++];
            }
            child[pos] = 0;
        }
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) kind = "dir ";
            else if (S_ISCHR(st.st_mode)) kind = "dev ";
        }
        printf(T("%s  %s\n"), kind, entry->d_name);
    }
    closedir(dir);
}

static void shell_cat(const char *path)
{
    char buffer[192];
    int fd;
    long got;
    if (!path || !path[0]) {
        shell_line(T("cat: a file path is required"));
        return;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        printf(T("cat: cannot open %s (%d)\n"), path, fd);
        return;
    }
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        write(1, buffer, (size_t)got);
    }
    if (got < 0) {
        printf(T("cat: read failed (%d)\n"), (int)got);
    }
    close(fd);
    shell_write("\n");
}

static void shell_echo(char *const argv[])
{
    for (uint32_t index = 1; argv[index]; ++index) {
        if (index > 1) {
            shell_write(" ");
        }
        shell_write(argv[index]);
    }
    shell_write("\n");
}

static void shell_start(char *argv[])
{
    int pid = reliefos_launch_argv(argv);
    if (pid >= 0) {
        printf(T("started %s (pid %d)\n"), argv[0], pid);
        return;
    }
    printf(T("unable to start %s: %s\n"), argv[0], T(reliefos_launch_error_text(pid)));
}

static int shell_dispatch(char *line)
{
    char *argv[SHELL_ARG_CAP + 1U];
    int argc = shell_split(line, argv);
    if (argc == 0) {
        return 0;
    }
    if (argc == -1) {
        shell_line(T("shell: unmatched quote"));
        return 0;
    }
    if (argc < 0) {
        shell_line(T("shell: too many arguments"));
        return 0;
    }
    if (shell_same(argv[0], "help")) {
        shell_line(T("Built-ins: help clear pwd cd ls cat echo run exit"));
    } else if (shell_same(argv[0], "clear")) {
        shell_write("\x1b[2J\x1b[H");
    } else if (shell_same(argv[0], "pwd")) {
        char cwd[RELIEFOS_FS_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd))) {
            shell_line(cwd);
        }
    } else if (shell_same(argv[0], "cd")) {
        int result = chdir(argc > 1 ? argv[1] : "/");
        if (result < 0) {
            printf(T("cd: %s (%d)\n"), argc > 1 ? argv[1] : "/", result);
        }
    } else if (shell_same(argv[0], "ls")) {
        shell_list(argc > 1 ? argv[1] : ".");
    } else if (shell_same(argv[0], "cat")) {
        shell_cat(argc > 1 ? argv[1] : 0);
    } else if (shell_same(argv[0], "echo")) {
        shell_echo(argv);
    } else if (shell_same(argv[0], "run")) {
        if (argc < 2) {
            shell_line(T("run: an application path is required"));
        } else {
            shell_start(&argv[1]);
        }
    } else if (shell_same(argv[0], "exit")) {
        shell_line(T("closing shell"));
        return 1;
    } else {
        shell_start(argv);
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    init_localization();
    char line[SHELL_LINE_CAP];
    uint32_t length = 0;
    char input;
    (void)envp;
    if (argc > 1 && argv[1] && argv[1][0]) {
        (void)chdir(argv[1]);
    }
    shell_line(T("ReliefOS command shell 2"));
    shell_line(T("Type help to see available commands."));
    if (argc > 2 && argv[2] && argv[2][0]) {
        char startup[SHELL_LINE_CAP];
        uint32_t index = 0;
        while (argv[2][index] && index + 1U < sizeof(startup)) {
            startup[index] = argv[2][index];
            ++index;
        }
        startup[index] = 0;
        if (shell_dispatch(startup)) {
            return 0;
        }
    }
    shell_prompt();
    for (;;) {
        if (read(0, &input, 1) <= 0) {
            sleep_ms(8);
            continue;
        }
        if (input == '\r') {
            continue;
        }
        if (input == '\n') {
            line[length] = 0;
            shell_write("\n");
            if (shell_dispatch(line)) {
                return 0;
            }
            length = 0;
            shell_prompt();
            continue;
        }
        if (input == '\b' || (uint8_t)input == 127U) {
            if (length > 0) {
                --length;
                shell_write("\b \b");
            }
            continue;
        }
        if ((uint8_t)input == 21U) {
            while (length > 0) {
                --length;
                shell_write("\b \b");
            }
            continue;
        }
        if ((uint8_t)input >= 32U && length + 1U < sizeof(line)) {
            line[length++] = input;
            write(1, &input, 1);
        }
    }
}
