#ifndef RELIEFOS_GPU_H
#define RELIEFOS_GPU_H

/*
 * Compatibility entry points for the retired GPU API. Calls return -1 with
 * errno set to ENODEV and leave caller buffers untouched. Legacy calls with
 * a null pointer return -EINVAL directly.
 */
#include <reliefos/gpu_abi.h>
#include <stdint.h>

int reliefos_gpu_diagnostics(struct reliefos_gpu_diagnostics *diagnostics);
int reliefos_gpu_info(struct reliefos_gpu_info *info);
int reliefos_gpu_create(struct reliefos_gpu_context *context);
int reliefos_gpu_render(const struct reliefos_gpu_frame *frame);
int reliefos_gpu_destroy(uint64_t handle);

#endif
