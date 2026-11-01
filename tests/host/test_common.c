/*
 * Contract tests for the shared host-tool primitives in tools/host/common/.
 *
 * These assert the documented ownership and error behaviour from the migration
 * plan section 8, not any particular internal representation.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "test-support.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../tools/host/common/buffer.h"
#include "../../tools/host/common/io.h"
#include "../../tools/host/common/process.h"

static void test_buffer_growth_preserves_content(void)
{
    struct byte_buffer buffer = { NULL, 0, 0 };
    static const unsigned char first[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const unsigned char second[4] = { 9, 8, 7, 6 };

    TEST_ASSERT_EQ(0, buffer_reserve(&buffer, sizeof(first)));
    TEST_ASSERT(buffer.capacity >= sizeof(first));
    memcpy(buffer.data, first, sizeof(first));
    buffer.len = sizeof(first);

    /* Growing must keep the bytes the caller already staged. */
    TEST_ASSERT_EQ(0, buffer_reserve(&buffer, buffer.len + sizeof(second)));
    TEST_ASSERT(buffer.capacity >= buffer.len + sizeof(second));
    TEST_ASSERT(memcmp(buffer.data, first, sizeof(first)) == 0);

    buffer_destroy(&buffer);
    TEST_ASSERT(buffer.data == NULL);
    TEST_ASSERT_EQ(0, buffer.len);
    TEST_ASSERT_EQ(0, buffer.capacity);
}

static void test_buffer_rejects_overflow_without_losing_data(void)
{
    struct byte_buffer buffer = { NULL, 0, 0 };
    unsigned char *original;

    TEST_ASSERT_EQ(0, buffer_reserve(&buffer, 32));
    memset(buffer.data, 0xa5, 32);
    buffer.len = 32;
    original = buffer.data;

    /* len + required must not wrap around the address space. */
    TEST_ASSERT_EQ(-1, buffer_reserve(&buffer, SIZE_MAX));
    TEST_ASSERT(buffer.data == original);
    TEST_ASSERT_EQ(32, buffer.len);
    TEST_ASSERT_EQ(0xa5, buffer.data[0]);

    buffer_destroy(&buffer);
}

static void test_buffer_destroy_accepts_zeroed_struct(void)
{
    struct byte_buffer buffer = { NULL, 0, 0 };

    buffer_destroy(&buffer);
    TEST_ASSERT(buffer.data == NULL);
}

static void test_write_creates_file_with_requested_mode(void)
{
    char root[128];
    char path[256];
    struct stat info;
    static const char payload[] = "first\n";

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, test_join(path, sizeof(path), root, "made-by-tool"));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, payload, sizeof(payload) - 1, 0600));

    TEST_ASSERT_EQ(0, stat(path, &info));
    TEST_ASSERT_EQ((long)(info.st_mode & 07777), 0600);

    {
        size_t read_size = 0;
        char *text = test_read_all(path, &read_size);

        TEST_ASSERT(text != NULL);
        if (text != NULL) {
            TEST_ASSERT_STR_EQ("first\n", text);
            TEST_ASSERT_EQ((long)sizeof(payload) - 1, (long)read_size);
            free(text);
        }
    }
    test_remove_tree(root);
}

static void test_write_of_empty_payload_produces_empty_file(void)
{
    char root[128];
    char path[256];
    struct stat info;

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, test_join(path, sizeof(path), root, "empty"));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, "", 0, 0644));
    TEST_ASSERT_EQ(0, stat(path, &info));
    TEST_ASSERT_EQ((long)info.st_size, 0);
    test_remove_tree(root);
}

static void test_write_keeps_mtime_when_content_is_unchanged(void)
{
    char root[128];
    char path[256];
    struct stat before;
    struct stat after;
    static const char payload[] = "same content\n";

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, test_join(path, sizeof(path), root, "stable"));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, payload, sizeof(payload) - 1, 0644));
    TEST_ASSERT_EQ(0, stat(path, &before));

    /* Non-zero interval so a stray rewrite would be observable. */
    {
        struct timespec pause = { 0, 60 * 1000 * 1000 };

        nanosleep(&pause, NULL);
    }
    TEST_ASSERT_EQ(0, write_file_if_changed(path, payload, sizeof(payload) - 1, 0644));
    TEST_ASSERT_EQ(0, stat(path, &after));
    TEST_ASSERT_EQ((long)before.st_mtime, (long)after.st_mtime);
    TEST_ASSERT_EQ((long)before.st_mtim.tv_nsec, (long)after.st_mtim.tv_nsec);
    TEST_ASSERT_EQ((long)before.st_ino, (long)after.st_ino);
    test_remove_tree(root);
}

static void test_write_replaces_changed_content_and_leaves_no_temporary(void)
{
    char root[128];
    char path[256];
    DIR *directory;
    int leftovers = 0;
    struct dirent *entry;
    static const char payload[] = "replacement\n";

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, test_join(path, sizeof(path), root, "replaced"));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, "old\n", 4, 0644));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, payload, sizeof(payload) - 1, 0644));

    {
        size_t size = 0;
        char *text = test_read_all(path, &size);

        TEST_ASSERT(text != NULL);
        if (text != NULL) {
            TEST_ASSERT_STR_EQ("replacement\n", text);
            free(text);
        }
    }

    directory = opendir(root);
    TEST_ASSERT(directory != NULL);
    if (directory != NULL) {
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, "replaced") != 0 &&
                strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                leftovers++;
            }
        }
        closedir(directory);
    }
    TEST_ASSERT_EQ(0, leftovers);
    test_remove_tree(root);
}

static void test_write_failure_keeps_original_file(void)
{
    char root[128];
    char path[256];
    size_t size = 0;
    char *text;
    pid_t child;

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, test_join(path, sizeof(path), root, "victim"));
    TEST_ASSERT_EQ(0, write_file_if_changed(path, "keep me\n", 8, 0644));

    /* Force a partial temporary-file write followed by EFBIG, including for
     * root with DAC override. Isolate the resource limit and signal in a child. */
    child = fork();
    TEST_ASSERT(child >= 0);
    if (child == 0) {
        struct rlimit limit;
        int result;

        if (getrlimit(RLIMIT_FSIZE, &limit) != 0 ||
            signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
            _exit(2);
        }
        limit.rlim_cur = 1;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            _exit(3);
        }
        errno = 0;
        result = write_file_if_changed(path, "overwrite\n", 10, 0644);
        _exit(result == -1 && errno == EFBIG ? 0 : 4);
    }
    if (child > 0) {
        int status = 0;
        pid_t waited;

        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        TEST_ASSERT_EQ(child, waited);
        TEST_ASSERT(WIFEXITED(status));
        TEST_ASSERT_EQ(0, WEXITSTATUS(status));
    }

    text = test_read_all(path, &size);
    TEST_ASSERT(text != NULL);
    if (text != NULL) {
        TEST_ASSERT_STR_EQ("keep me\n", text);
        free(text);
    }
    {
        DIR *directory = opendir(root);
        struct dirent *entry;
        unsigned leftovers = 0;

        TEST_ASSERT(directory != NULL);
        if (directory != NULL) {
            while ((entry = readdir(directory)) != NULL) {
                if (strcmp(entry->d_name, "victim") != 0 &&
                    strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                    leftovers++;
                }
            }
            closedir(directory);
        }
        TEST_ASSERT_EQ(0, leftovers);
    }
    test_remove_tree(root);
}

static void test_run_process_success(void)
{
    char *argv[] = { (char *)"/bin/true", NULL };
    int child_status = -1;

    errno = 0;
    TEST_ASSERT_EQ(0, run_process("/bin/true", argv, NULL, &child_status));
    TEST_ASSERT(errno == 0);
    TEST_ASSERT(WIFEXITED(child_status));
    TEST_ASSERT_EQ(0, WEXITSTATUS(child_status));
}

static void test_run_process_uses_working_directory(void)
{
    char root[128];
    char marker_path[256];
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)"pwd > .pwd-marker", NULL };
    int child_status = -1;
    size_t size = 0;
    char *text;

    TEST_ASSERT(test_temp_dir(root, sizeof(root)) != NULL);
    TEST_ASSERT_EQ(0, run_process("/bin/sh", argv, root, &child_status));
    TEST_ASSERT(WIFEXITED(child_status));
    TEST_ASSERT_EQ(0, WEXITSTATUS(child_status));

    /* The redirect target only exists if the child resolved "." inside the
     * requested directory, so a silently ignored working_dir fails here. */
    TEST_ASSERT_EQ(0, test_join(marker_path, sizeof(marker_path), root, ".pwd-marker"));
    text = test_read_all(marker_path, &size);
    TEST_ASSERT(text != NULL);
    if (text != NULL) {
        TEST_ASSERT(size > 1);
        free(text);
    }
    test_remove_tree(root);
}

static void test_run_process_nonzero_exit_is_child_status(void)
{
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)"exit 3", NULL };
    int child_status = -1;

    TEST_ASSERT_EQ(0, run_process("/bin/sh", argv, NULL, &child_status));
    TEST_ASSERT(WIFEXITED(child_status));
    TEST_ASSERT_EQ(3, WEXITSTATUS(child_status));
}

static void test_run_process_reports_signals(void)
{
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)"kill -TERM $$", NULL };
    int child_status = -1;

    /* A tool that swallows the signal would report success here, which is the
     * exact failure mode the contract forbids. */
    TEST_ASSERT_EQ(0, run_process("/bin/sh", argv, NULL, &child_status));
    TEST_ASSERT(WIFSIGNALED(child_status));
    TEST_ASSERT_EQ(SIGTERM, WTERMSIG(child_status));
}

static void test_run_process_missing_program_fails_with_errno(void)
{
    char *argv[] = { (char *)"/nonexistent/reliefos-host-tool-test-binary", NULL };
    int child_status = -1;

    errno = 0;
    TEST_ASSERT_EQ(-1, run_process("/nonexistent/reliefos-host-tool-test-binary",
        argv, NULL, &child_status));
    TEST_ASSERT(errno != 0);
}

static void test_run_process_rejects_unusable_working_directory(void)
{
    char *argv[] = { (char *)"/bin/true", NULL };
    int child_status = -1;

    errno = 0;
    TEST_ASSERT_EQ(-1, run_process("/bin/true", argv,
        "/nonexistent-directory-for-reliefos-test", &child_status));
    TEST_ASSERT(errno != 0);
}

int main(void)
{
    test_buffer_growth_preserves_content();
    test_buffer_rejects_overflow_without_losing_data();
    test_buffer_destroy_accepts_zeroed_struct();
    test_write_creates_file_with_requested_mode();
    test_write_of_empty_payload_produces_empty_file();
    test_write_keeps_mtime_when_content_is_unchanged();
    test_write_replaces_changed_content_and_leaves_no_temporary();
    test_write_failure_keeps_original_file();
    test_run_process_success();
    test_run_process_uses_working_directory();
    test_run_process_nonzero_exit_is_child_status();
    test_run_process_reports_signals();
    test_run_process_missing_program_fails_with_errno();
    test_run_process_rejects_unusable_working_directory();
    return reliefos_test_report("host/common");
}
