/* Legacy GPU ABI exports retain their null-argument error convention. */
#include <reliefos/gpu_sdk.h>
#include <errno.h>

int reliefos_gpu_diagnostics(struct reliefos_gpu_diagnostics *diagnostic)
{
    return diagnostic ? gpu_sdk_diagnostics(diagnostic) : -EINVAL;
}

int reliefos_gpu_info(struct reliefos_gpu_info *info)
{
    return info ? gpu_sdk_info(info) : -EINVAL;
}

int reliefos_gpu_create(struct reliefos_gpu_context *context)
{
    return context ? gpu_sdk_create(context) : -EINVAL;
}

int reliefos_gpu_render(const struct reliefos_gpu_frame *frame)
{
    return frame ? gpu_sdk_render(frame) : -EINVAL;
}

int reliefos_gpu_destroy(uint64_t handle)
{
    return gpu_sdk_destroy(handle);
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_gpu_create) leonos_gpu_create __attribute__((alias("reliefos_gpu_create")));
extern __typeof__(reliefos_gpu_destroy) leonos_gpu_destroy __attribute__((alias("reliefos_gpu_destroy")));
extern __typeof__(reliefos_gpu_diagnostics) leonos_gpu_diagnostics __attribute__((alias("reliefos_gpu_diagnostics")));
extern __typeof__(reliefos_gpu_info) leonos_gpu_info __attribute__((alias("reliefos_gpu_info")));
extern __typeof__(reliefos_gpu_render) leonos_gpu_render __attribute__((alias("reliefos_gpu_render")));
