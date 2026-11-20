/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <reliefos/gpu_sdk.h>

int __wrap_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    assert(!"GPU compatibility calls must not open a device");
    return -1;
}

long __wrap_syscall3(long number, long arg0, long arg1, long arg2)
{
    (void)number;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    assert(!"GPU compatibility calls must not issue a device ioctl");
    return -1;
}

static void expect_error(int result, int expected_errno)
{
    assert(result == -1);
    assert(errno == expected_errno);
}

int main(void)
{
    struct reliefos_gpu_info info = {.size = 1, .version = 2};
    struct reliefos_gpu_diagnostics diagnostics = {.size = 1, .version = 2};
    struct reliefos_gpu_context context = {.size = 1, .version = 2};
    const struct reliefos_gpu_frame frame = {0};

    expect_error(gpu_sdk_info(&info), ENODEV);
    expect_error(gpu_sdk_diagnostics(&diagnostics), ENODEV);
    expect_error(gpu_sdk_create(&context), ENODEV);
    expect_error(gpu_sdk_render(&frame), ENODEV);
    expect_error(gpu_sdk_destroy(0), ENODEV);

    expect_error(reliefos_gpu_info(&info), ENODEV);
    expect_error(reliefos_gpu_diagnostics(&diagnostics), ENODEV);
    expect_error(reliefos_gpu_create(&context), ENODEV);
    expect_error(reliefos_gpu_render(&frame), ENODEV);
    expect_error(reliefos_gpu_destroy(0), ENODEV);

    assert(info.size == 1 && info.version == 2);
    assert(diagnostics.size == 1 && diagnostics.version == 2);
    assert(context.size == 1 && context.version == 2);

    expect_error(gpu_sdk_info(NULL), EINVAL);
    expect_error(gpu_sdk_diagnostics(NULL), EINVAL);
    expect_error(gpu_sdk_create(NULL), EINVAL);
    expect_error(gpu_sdk_render(NULL), EINVAL);
    assert(reliefos_gpu_info(NULL) == -EINVAL);
    assert(reliefos_gpu_diagnostics(NULL) == -EINVAL);
    assert(reliefos_gpu_create(NULL) == -EINVAL);
    assert(reliefos_gpu_render(NULL) == -EINVAL);
    return 0;
}
