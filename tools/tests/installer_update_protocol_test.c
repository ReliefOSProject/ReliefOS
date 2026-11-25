/* SPDX-License-Identifier: Apache-2.0 */
#include "../../userland/apps/installer/installer_update.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct installer_update_session session;
static struct installer_update_info info;
static unsigned progress;
static char message[256];

static int line(const char *text)
{
    return installer_update_session_line(&session, &info, &progress, message, sizeof(message), text);
}

static void reset(void)
{
    memset(&session, 0, sizeof(session));
    memset(&info, 0, sizeof(info));
    progress = 0;
    message[0] = 0;
}

static void snapshot(void)
{
    static const char *const records[] = {
        "info\tdisk\t/dev/sda", "info\troot_device\t/dev/sda3", "info\tesp_device\t/dev/sda2",
        "info\troot_uuid\t11111111-2222-3333-4444-555555555555",
        "info\tesp_uuid\taaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "info\tboot_file\t",
        "info\tinstalled_version\t5.3.3", "info\tmedia_version\t5.3.4", "info\tprofile\txorg",
        "info\tinstalled_kernel\t5.3.3-r0", "info\tmedia_kernel\t5.3.4-r0",
        "info\tindex_sha256\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "info\tdatabase_sha256\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "info\tworld_sha256\t0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "info\tpending\t", "info\tfree_bytes\t1000000000", "info\trequired_bytes\t1000000",
        "info\tboot_modes\t3",
    };
    for (unsigned i = 0; i < sizeof(records) / sizeof(records[0]); ++i) assert(line(records[i]) == 1);
}

int main(void)
{
    reset(); snapshot();
    assert(line("progress\t100\tUpdate checks completed") == 2 && progress == 100);
    assert(installer_update_session_finish(&session, &info) == -EPROTO);
    assert(line("result\t0\tUpdate checks completed") == 5);
    assert(!installer_update_session_finish(&session, &info));
    assert(line("result\t0\tDuplicate completion") < 0);
    assert(installer_update_session_finish(&session, &info) == -EPROTO);

    reset();
    assert(line("result\t0\tMissing metadata") < 0);
    reset(); snapshot();
    assert(line("info\tdisk\t/dev/sdb") < 0);
    assert(line("result\t0\tCannot recover a protocol error") < 0);
    reset(); snapshot(); strcpy(info.root_uuid, "not-a-uuid");
    assert(line("result\t0\tInvalid metadata") < 0);
    reset(); snapshot(); strcpy(info.index_sha256, "unsigned-index");
    assert(line("result\t0\tInvalid digest") < 0);

    reset();
    assert(line("error\t125\tCheck canceled") == 3);
    assert(line("result\t125\tCheck canceled") == 5);
    assert(installer_update_session_finish(&session, &info) == -ECANCELED);
    reset();
    assert(line("error\t5\tRead failed") == 3);
    assert(line("result\t0\tCannot report success after failure") < 0);
    reset();
    assert(line("error\t5\tRead failed") == 3);
    assert(line("result\t110\tMismatched error") < 0);
    reset();
    assert(line("result\t110\tRead timed out") == 5);
    assert(installer_update_session_finish(&session, &info) == -ETIMEDOUT);
    assert(!strcmp(info.failure, "Read timed out"));
    reset();
    assert(line("ordinary output on the control stream") < 0);
    reset();
    assert(line("result\t0\tBad\tmessage") < 0);
    reset();
    char oversized[600]; memset(oversized, 'x', sizeof(oversized)); oversized[599] = 0;
    assert(line(oversized) < 0);
    puts("PASS complete update sessions, cancellation, truncation, invalid and duplicate records");
    return 0;
}
