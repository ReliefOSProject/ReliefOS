/* Compatibility exports for the retired GPU SDK. */
#include <reliefos/gpu_sdk.h>
#include <errno.h>

static int gpu_sdk_error(int error)
{
    errno = error;
    return -1;
}

int gpu_sdk_diagnostics(gpu_sdk_diagnostics_t *diagnostic)
{
    return gpu_sdk_error(diagnostic ? ENODEV : EINVAL);
}

int gpu_sdk_info(gpu_sdk_info_t *info)
{
    return gpu_sdk_error(info ? ENODEV : EINVAL);
}

int gpu_sdk_create(gpu_sdk_context_t *context)
{
    return gpu_sdk_error(context ? ENODEV : EINVAL);
}

int gpu_sdk_render(const gpu_sdk_frame_t *frame)
{
    return gpu_sdk_error(frame ? ENODEV : EINVAL);
}

int gpu_sdk_destroy(uint64_t handle)
{
    (void)handle;
    return gpu_sdk_error(ENODEV);
}
