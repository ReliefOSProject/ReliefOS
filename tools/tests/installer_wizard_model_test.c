/* Test the real route policy independently of either UI toolkit or storage. */
#include "../../userland/apps/installer/model.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/** @brief Check both actual routes and their safe backward-navigation boundary. */
int main(void)
{
    const enum installer_page fresh[] = {
        INSTALLER_PAGE_LANGUAGE, INSTALLER_PAGE_LICENSE, INSTALLER_PAGE_MODE,
        INSTALLER_PAGE_DISK, INSTALLER_PAGE_BOOTLOADERS, INSTALLER_PAGE_PARTITIONS,
        INSTALLER_PAGE_SYSTEM, INSTALLER_PAGE_ACCOUNTS, INSTALLER_PAGE_CONFIRM,
        INSTALLER_PAGE_PROGRESS, INSTALLER_PAGE_FINISH
    };
    const enum installer_page update[] = {
        INSTALLER_PAGE_LANGUAGE, INSTALLER_PAGE_LICENSE, INSTALLER_PAGE_MODE,
        INSTALLER_PAGE_DISK, INSTALLER_PAGE_CONFIRM, INSTALLER_PAGE_PROGRESS,
        INSTALLER_PAGE_FINISH
    };
    for (int mode = INSTALLER_MODE_FRESH; mode <= INSTALLER_MODE_UPDATE; ++mode) {
        const enum installer_page *expected = mode == INSTALLER_MODE_FRESH ? fresh : update;
        int count = mode == INSTALLER_MODE_FRESH ? (int)(sizeof(fresh) / sizeof(fresh[0]))
            : (int)(sizeof(update) / sizeof(update[0]));
        enum installer_page actual[INSTALLER_PAGE_COUNT];
        assert(installer_model_steps(mode, actual, INSTALLER_PAGE_COUNT) == count);
        assert(installer_model_steps(mode, NULL, 0) == count);
        assert(installer_model_steps(mode, NULL, INSTALLER_PAGE_COUNT) == count);
        assert(!memcmp(actual, expected, (size_t)count * sizeof(*actual)));
        for (int i = 0; i < count; ++i) {
            assert(installer_model_step_index(actual[i], mode) == i);
            assert(*installer_model_page_title(actual[i], mode));
            if (i + 1 < count && actual[i] != INSTALLER_PAGE_PROGRESS)
                assert(installer_model_next(actual[i], mode) == actual[i + 1]);
            if (i && actual[i] != INSTALLER_PAGE_PROGRESS && actual[i] != INSTALLER_PAGE_FINISH)
                assert(installer_model_prev(actual[i], mode) == actual[i - 1]);
            assert(!installer_model_can_visit(INSTALLER_PAGE_PROGRESS, actual[i], mode, 0));
            assert(!installer_model_can_visit(INSTALLER_PAGE_FINISH, actual[i], mode, 1));
        }
        assert(installer_model_step_index(INSTALLER_PAGE_WELCOME, mode) == -1);
        assert(installer_model_action(INSTALLER_PAGE_FINISH, mode, 0) == INSTALLER_ACTION_RETRY);
        assert(installer_model_prev(INSTALLER_PAGE_FINISH, mode) == INSTALLER_PAGE_DISK);
        assert(installer_model_can_visit(INSTALLER_PAGE_CONFIRM, INSTALLER_PAGE_DISK, mode, 0));
        assert(!installer_model_can_visit(INSTALLER_PAGE_DISK, INSTALLER_PAGE_CONFIRM, mode, 0));
        assert(!installer_model_can_visit(INSTALLER_PAGE_DISK, INSTALLER_PAGE_DISK, mode, 0));
    }
    enum installer_page truncated[2] = {INSTALLER_PAGE_COUNT, INSTALLER_PAGE_COUNT};
    assert(installer_model_steps(INSTALLER_MODE_FRESH, truncated, 1) == 11);
    assert(truncated[0] == INSTALLER_PAGE_LANGUAGE && truncated[1] == INSTALLER_PAGE_COUNT);
    assert(installer_model_step_index(INSTALLER_PAGE_SYSTEM, INSTALLER_MODE_UPDATE) == -1);
    assert(!installer_model_can_go_back(INSTALLER_PAGE_LANGUAGE, 0));
    assert(!installer_model_can_go_back(INSTALLER_PAGE_PROGRESS, 0));
    assert(!installer_model_can_cancel(INSTALLER_PAGE_PROGRESS, 0));
    assert(!installer_model_can_cancel(INSTALLER_PAGE_FINISH, 1));
    assert(!installer_model_can_go_back(INSTALLER_PAGE_COUNT, 0));
    puts("PASS installer routes, inverse navigation, update isolation and failed-attempt review");
    return 0;
}
