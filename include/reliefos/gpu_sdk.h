#ifndef RELIEFOS_GPU_SDK_H
#define RELIEFOS_GPU_SDK_H

/* Compatibility entry points for the retired GPU SDK. Calls return -1 with
 * errno set to ENODEV (EINVAL for a null pointer) and leave caller buffers
 * untouched. */
#include <reliefos/gpu.h>
#include <stdint.h>

#define GPU_SDK_ABI_VERSION 1U

typedef struct reliefos_gpu_info gpu_sdk_info_t;
typedef struct reliefos_gpu_context gpu_sdk_context_t;
typedef struct reliefos_gpu_vertex gpu_sdk_vertex_t;
typedef struct reliefos_gpu_draw gpu_sdk_draw_t;
typedef struct reliefos_gpu_frame gpu_sdk_frame_t;
typedef struct reliefos_gpu_destroy gpu_sdk_destroy_t;
typedef struct reliefos_gpu_diagnostics gpu_sdk_diagnostics_t;

#define GPU_SDK_MAX_VERTICES RELIEFOS_GPU_MAX_VERTICES
#define GPU_SDK_MAX_DRAWS RELIEFOS_GPU_MAX_DRAWS
#define GPU_SDK_MAX_WIDTH RELIEFOS_GPU_MAX_WIDTH
#define GPU_SDK_MAX_HEIGHT RELIEFOS_GPU_MAX_HEIGHT
#define GPU_SDK_AVAILABLE RELIEFOS_GPU_AVAILABLE
#define GPU_SDK_BUSY_ESTIMATED RELIEFOS_GPU_BUSY_ESTIMATED
#define GPU_SDK_FILL_POINT RELIEFOS_GPU_FILL_POINT
#define GPU_SDK_FILL_LINE RELIEFOS_GPU_FILL_LINE
#define GPU_SDK_FILL_SOLID RELIEFOS_GPU_FILL_SOLID
#define GPU_SDK_ERROR_NONE RELIEFOS_GPU_ERROR_NONE
#define GPU_SDK_ERROR_REAP RELIEFOS_GPU_ERROR_REAP
#define GPU_SDK_ERROR_PREPARE RELIEFOS_GPU_ERROR_PREPARE
#define GPU_SDK_ERROR_UPLOAD RELIEFOS_GPU_ERROR_UPLOAD
#define GPU_SDK_ERROR_STATE RELIEFOS_GPU_ERROR_STATE
#define GPU_SDK_ERROR_DRAW RELIEFOS_GPU_ERROR_DRAW
#define GPU_SDK_ERROR_READBACK RELIEFOS_GPU_ERROR_READBACK
#define GPU_SDK_ERROR_FENCE RELIEFOS_GPU_ERROR_FENCE
#define GPU_SDK_ERROR_COPYOUT RELIEFOS_GPU_ERROR_COPYOUT

int gpu_sdk_diagnostics(gpu_sdk_diagnostics_t *diagnostics);
int gpu_sdk_info(gpu_sdk_info_t *info);
int gpu_sdk_create(gpu_sdk_context_t *context);
int gpu_sdk_render(const gpu_sdk_frame_t *frame);
int gpu_sdk_destroy(uint64_t handle);

#endif
