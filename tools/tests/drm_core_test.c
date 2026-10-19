#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/wait.h>
#include <reliefnt/drm.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <linux/poll.h>
#include <reliefnt/console.h>
#include <reliefnt/mm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/paging.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <reliefnt/svga.h>
#include "../../kernel/reliefnt/drivers/video/svga/svga_protocol.h"

#include "../../kernel/reliefnt/drivers/video/drm/drm_device.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_faildiag.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_file.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_ioctl.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_kms.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_bo.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_vmwgfx_caps.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_vmwgfx_resource.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_prime.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_vmwgfx_execbuf_parser.c"
#include "../../kernel/reliefnt/drivers/video/drm/drm_vmwgfx_execbuf.c"

#define DRM_IOCTL_VMW_ALLOC_BO \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO, union drm_vmw_alloc_bo_arg)
#define DRM_IOCTL_VMW_UNREF_BO \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_DMABUF, struct drm_vmw_handle_close_arg)
#define DRM_IOCTL_VMW_SYNCCPU \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_SYNCCPU, struct drm_vmw_synccpu_arg)
#define DRM_IOCTL_VMW_GET_PARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM, struct drm_vmw_getparam_arg)
#define DRM_IOCTL_VMW_CREATE_CONTEXT \
    DRM_IOR(DRM_COMMAND_BASE + DRM_VMW_CREATE_CONTEXT, struct drm_vmw_context_arg)
#define DRM_IOCTL_VMW_UNREF_CONTEXT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_CONTEXT, struct drm_vmw_context_arg)
#define DRM_IOCTL_VMW_CREATE_SURFACE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_CREATE_SURFACE, union drm_vmw_surface_create_arg)
#define DRM_IOCTL_VMW_UNREF_SURFACE \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_SURFACE, struct drm_vmw_surface_arg)
#define DRM_IOCTL_VMW_REF_SURFACE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_REF_SURFACE, union drm_vmw_surface_reference_arg)
#define DRM_IOCTL_VMW_GET_3D_CAP \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_GET_3D_CAP, struct drm_vmw_get_3d_cap_arg)
#define DRM_IOCTL_VMW_EXECBUF \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_EXECBUF, struct drm_vmw_execbuf_arg)
#define DRM_IOCTL_VMW_EXECBUF_SHORT \
    _IOC(_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_VMW_EXECBUF, 32)
#define DRM_IOCTL_VMW_FENCE_SIGNALED \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_FENCE_SIGNALED, \
             struct drm_vmw_fence_signaled_arg)
#define DRM_IOCTL_VMW_FENCE_WAIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_FENCE_WAIT, \
             struct drm_vmw_fence_wait_arg)
#define DRM_IOCTL_VMW_FENCE_UNREF \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_FENCE_UNREF, struct drm_vmw_fence_arg)
#define DRM_TEST_RESOURCE_HANDLE_MIN 0x01000000u
#define DRM_TEST_COMPAT_DEVCAP_MAX 262u
#define DRM_TEST_RENDER_NODE_KIND 35u
#define DRM_TEST_FORMAT_NEGATIVE_MASK \
    (SVGA3DFORMAT_OP_NOFILTER | SVGA3DFORMAT_OP_NOALPHABLEND | \
     SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP)
/* Canonical SVGA3dSurface1Flags usage bits, kept literal so this fixture
 * independently checks the protocol values imported by production code. */
#define DRM_TEST_SURFACE_BIND_VERTEX_BUFFER (1u << 20)
#define DRM_TEST_SURFACE_BIND_INDEX_BUFFER (1u << 21)
#define DRM_TEST_SURFACE_BIND_CONSTANT_BUFFER (1u << 22)
#define DRM_TEST_SURFACE_BIND_SHADER_RESOURCE (1u << 23)
#define DRM_TEST_SURFACE_BIND_RENDER_TARGET (1u << 24)
#define DRM_TEST_SURFACE_BIND_DEPTH_STENCIL (1u << 25)
#define DRM_TEST_SURFACE_BIND_STREAM_OUTPUT (1u << 26)
_Static_assert(SVGA3D_SURFACE_BIND_VERTEX_BUFFER == DRM_TEST_SURFACE_BIND_VERTEX_BUFFER, "SVGA vertex bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_INDEX_BUFFER == DRM_TEST_SURFACE_BIND_INDEX_BUFFER, "SVGA index bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_CONSTANT_BUFFER == DRM_TEST_SURFACE_BIND_CONSTANT_BUFFER, "SVGA constant bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_SHADER_RESOURCE == DRM_TEST_SURFACE_BIND_SHADER_RESOURCE, "SVGA shader-resource bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_RENDER_TARGET == DRM_TEST_SURFACE_BIND_RENDER_TARGET, "SVGA render-target bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_DEPTH_STENCIL == DRM_TEST_SURFACE_BIND_DEPTH_STENCIL, "SVGA depth-stencil bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_STREAM_OUTPUT == DRM_TEST_SURFACE_BIND_STREAM_OUTPUT, "SVGA stream-output bind bit");

static unsigned allocations;
static char drm_test_diag_log_buffer[16384];
static unsigned drm_test_diag_log_calls;
static int fail_kernel_malloc_after = -1;
static unsigned wait_queue_add_calls;
static unsigned block_current_calls;
static unsigned sleep_current_calls;
static uint64_t sleep_current_deadline;
static unsigned reschedule_current_calls;
static struct task *reschedule_expected_task;
static uint32_t reschedule_expected_handle;
static int32_t reschedule_expected_lazy;
static uint64_t reschedule_expected_deadline;
static uint64_t mm_pages_allocated;
static uint64_t mm_pages_freed;
static bool mm_allocation_fails;
static uint64_t fail_copyout_address;
static struct svga_info drm_test_svga_info = {
    .fifo_caps = 0x12345679u,
    .host_version = 0x00020001u,
    .guest_version = 0x00020001u,
    .generation = 1,
    .surface_limit_bytes = 64u * 1024u * 1024u,
    .compat_surface_limit_bytes = 64u * 1024u * 1024u,
    .fifo_ready = true,
    .available = true,
};
static uint32_t drm_test_svga_caps[DRM_TEST_COMPAT_DEVCAP_MAX];
static bool drm_test_svga_cap_valid[DRM_TEST_COMPAT_DEVCAP_MAX];
static unsigned drm_test_context_create_calls;
static unsigned drm_test_context_destroy_calls;
static unsigned drm_test_surface_create_calls;
static unsigned drm_test_surface_destroy_calls;
static unsigned drm_test_context_destroy_busy;
static unsigned drm_test_surface_destroy_busy;
static int drm_test_context_create_status;
static int drm_test_surface_create_status;

void console_log(enum console_log_level level, const char *format, ...)
{
    va_list arguments;
    size_t used = strlen(drm_test_diag_log_buffer);
    (void)level;
    va_start(arguments, format);
    if (used < sizeof(drm_test_diag_log_buffer))
        (void)vsnprintf(drm_test_diag_log_buffer + used,
                        sizeof(drm_test_diag_log_buffer) - used,
                        format, arguments);
    va_end(arguments);
    ++drm_test_diag_log_calls;
}

static void drm_test_diag_log_reset(void)
{
    drm_test_diag_log_buffer[0] = 0;
    drm_test_diag_log_calls = 0;
}
static struct svga_surface_desc drm_test_last_surface_desc;
static uint64_t drm_test_next_backend_handle = 0xfeed000000000001ULL;
static svga_handle drm_test_context_destroy_attempts[64];
static unsigned drm_test_context_destroy_attempt_count;
static unsigned drm_test_shader_reaper_calls;
static uint32_t drm_test_shader_reaper_budget;
static uint32_t drm_test_shader_backings_reaped;
static uint32_t drm_test_fence_issued, drm_test_fence_passed;
static uint32_t drm_test_batch_submit_calls, drm_test_batch_abort_calls;
static uint32_t drm_test_batch_append_calls;
static uint32_t drm_test_batch_generation;
static uint32_t drm_test_batch_busy_submits;
static int drm_test_batch_submit_failure;
static bool drm_test_batch_abort_rewrites_failure;
static uint32_t drm_test_present_sid;
static struct svga_present_rect drm_test_present_rect;
static struct svga_batch *drm_test_pending_present_batch;
static uint32_t drm_test_pending_present_rect_word;
static struct svga_present_rect drm_test_published_present_rect;
static uint32_t drm_test_present_submitted;
static uint64_t drm_test_ticks = 100;
static uint32_t drm_test_fence_status_calls;
static uint32_t drm_test_fence_status_sequences[256];
static int drm_test_fence_status_script[128];
static bool drm_test_fence_signaled_script[128];
static uint32_t drm_test_fence_status_script_count;
static uint32_t drm_test_fence_status_script_index;
static uint32_t drm_test_unlocked_fence_status_calls;
static struct drm_file *drm_test_replace_fence_file;
static uint32_t drm_test_replace_fence_handle;
static struct task *drm_test_replace_continuation_task;
static struct drm_vmwgfx_task_continuation *
    drm_test_replacement_continuation;
static struct task *drm_test_exit_task_during_unlocked_poll;
static bool drm_test_execution_owned = true;
static bool drm_test_interrupts_enabled;
static int drm_test_suspend_status;
static int drm_test_resume_status;
static uint32_t drm_test_suspend_calls;
static uint32_t drm_test_resume_calls;
static uint32_t drm_test_clock_getres_calls;
static int drm_test_clock_getres_status;
static int64_t drm_test_clock_resolution_ns = 1000;
static uint64_t drm_test_uptime_value;
static uint64_t drm_test_uptime_step;
static uint64_t drm_test_uptime_calls;
static uint64_t drm_test_uptime_advance_after_calls;
static uint64_t drm_test_uptime_advance_to;
static uint64_t drm_test_uptime_script[8];
static uint32_t drm_test_uptime_script_count;
static uint32_t drm_test_uptime_script_index;
static uint32_t drm_test_quiesced_generation, drm_test_quiesced_seq;
static svga_handle drm_test_last_retained_gmr;
static svga_handle drm_test_borrowed_detach_fence_handle;
static uint32_t drm_test_borrowed_detach_attempts;
static uint32_t drm_test_borrowed_detach_publications;
static uint32_t drm_test_borrowed_detach_busy_returns;
static uint32_t drm_test_borrowed_owner_put_calls;
enum drm_test_stage_kind {
    DRM_TEST_STAGE_SHADER_DEFINE = 1,
    DRM_TEST_STAGE_SHADER_BIND,
    DRM_TEST_STAGE_SHADER_CONSTANTS,
    DRM_TEST_STAGE_SHADER_DESTROY,
};
struct drm_test_stage_event { uint32_t kind, first, second; };
static struct drm_test_stage_event drm_test_stage_events[64];
static uint32_t drm_test_stage_event_count;
#define DRM_TEST_SHADER_CONSTANT_REGISTER_MAX 256u
#define DRM_TEST_SHADER_CONSTANT_CALL_MAX 16u
struct drm_test_shader_constant_capture {
    uint32_t reg, type, const_type, registers;
    uint32_t values[DRM_TEST_SHADER_CONSTANT_REGISTER_MAX * 4u];
};
static struct drm_test_shader_constant_capture
    drm_test_shader_constant_captures[DRM_TEST_SHADER_CONSTANT_CALL_MAX];
static uint32_t drm_test_shader_constant_capture_count;
static void drm_test_record_stage(uint32_t kind, uint32_t first,
                                  uint32_t second);
struct drm_test_borrowed {
    svga_handle handle;
    void *owner;
    void (*owner_put)(void *owner);
    uint32_t retirement_fence_seq;
    bool destruction_published;
};
static struct drm_test_borrowed drm_test_borrowed[128];
static uint32_t drm_test_borrowed_count;
static svga_handle drm_test_next_gmr = 0xa000000000000001ULL;

int svga_fence_try_insert(svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    uint32_t seq = ++drm_test_fence_issued;
    *out = ((uint64_t)drm_test_svga_info.generation << 32) | seq;
    return 0;
}

int svga_fence_status(svga_handle fence, bool *signaled)
{
    if (!signaled) return SVGA_EINVAL;
    if ((fence >> 32) != drm_test_svga_info.generation ||
        !(uint32_t)fence || (uint32_t)fence > drm_test_fence_issued)
        return SVGA_EIO;
    if (drm_test_fence_status_calls <
        sizeof(drm_test_fence_status_sequences) /
            sizeof(drm_test_fence_status_sequences[0]))
        drm_test_fence_status_sequences[drm_test_fence_status_calls] =
            (uint32_t)fence;
    ++drm_test_fence_status_calls;
    if (!drm_test_execution_owned) {
        assert(drm_test_interrupts_enabled);
        ++drm_test_unlocked_fence_status_calls;
        if (drm_test_replace_continuation_task) {
            struct task *task = drm_test_replace_continuation_task;
            struct drm_vmwgfx_task_continuation *replacement =
                drm_test_replacement_continuation;
            assert(replacement);
            drm_test_replace_continuation_task = NULL;
            drm_test_replacement_continuation = NULL;
            drm_vmwgfx_cancel_task_continuation(task);
            assert(!task->drm_vmwgfx_continuation);
            task->drm_vmwgfx_continuation = replacement;
        }
        if (drm_test_exit_task_during_unlocked_poll) {
            struct task *task = drm_test_exit_task_during_unlocked_poll;
            drm_test_exit_task_during_unlocked_poll = NULL;
            task->state = TASK_EXITED;
        }
        if (drm_test_replace_fence_file) {
            assert(drm_test_replace_fence_handle);
            struct drm_file *file = drm_test_replace_fence_file;
            uint32_t handle = drm_test_replace_fence_handle;
            assert(drm_vmwgfx_fence_remove(file, handle));
            file->next_fence_handle = handle;
            struct drm_vmwgfx_fence *replacement =
                drm_vmwgfx_fence_reserve(file, true, NULL);
            assert(replacement && replacement->handle == handle);
            svga_handle token = 0;
            assert(svga_fence_try_insert(&token) == 0);
            replacement->token = token;
            drm_vmwgfx_fence_publish(file, replacement);
            drm_vmwgfx_fence_put(replacement);
            drm_test_replace_fence_file = NULL;
            drm_test_replace_fence_handle = 0;
        }
    } else {
        assert(!drm_test_interrupts_enabled);
    }
    if (drm_test_fence_status_script_index <
        drm_test_fence_status_script_count) {
        uint32_t index = drm_test_fence_status_script_index++;
        *signaled = drm_test_fence_signaled_script[index];
        return drm_test_fence_status_script[index];
    }
    *signaled = drm_test_fence_passed >= (uint32_t)fence;
    return 0;
}

int svga_fence_progress_snapshot(struct svga_fence_progress *out)
{
    if (!out) return SVGA_EINVAL;
    *out = (struct svga_fence_progress){
        .generation = drm_test_svga_info.generation,
        .issued_seq = drm_test_fence_issued,
        .passed_seq = drm_test_fence_passed,
    };
    return 0;
}

bool svga_fence_is_quiesced(svga_handle fence)
{
    uint32_t generation = (uint32_t)(fence >> 32);
    uint32_t sequence = (uint32_t)fence;
    return generation && sequence &&
        generation == drm_test_quiesced_generation &&
        (int32_t)(drm_test_quiesced_seq - sequence) >= 0;
}

int svga_batch_init_sized(struct svga_batch *batch, uint32_t generation,
                          uint32_t *storage, uint32_t output_capacity,
                          uint32_t source_bytes)
{
    if (!batch || !storage || !output_capacity || !source_bytes ||
        output_capacity > SVGA_BATCH_MAX_OUTPUT_BYTES ||
        source_bytes > SVGA_BATCH_MAX_INPUT_BYTES) return SVGA_EINVAL;
    struct svga_batch_metadata *metadata = kernel_malloc(1);
    if (!metadata) return SVGA_ENOMEM;
    *batch = (struct svga_batch){.generation = generation,
        .capacity = output_capacity, .source_bytes = source_bytes,
        .words = storage, .metadata = metadata};
    drm_test_batch_generation = generation;
    return 0;
}

int svga_batch_append(struct svga_batch *batch, uint32_t command, bool three_d,
                      const void *payload, uint32_t bytes)
{
    if (!batch || !batch->metadata || (bytes && !payload) || (bytes & 3u))
        return SVGA_EINVAL;
    uint32_t packet_bytes = 8u + bytes;
    if (packet_bytes > batch->capacity - batch->bytes) return SVGA_ENOMEM;
    if (command == SVGA_3D_CMD_PRESENT) {
        assert(three_d && bytes == sizeof(SVGA3dCmdPresent) +
                                sizeof(struct svga_present_rect));
        const SVGA3dCmdPresent *present = payload;
        const struct svga_present_rect *rect =
            (const struct svga_present_rect *)(present + 1);
        drm_test_present_sid = present->sid;
        drm_test_present_rect = *rect;
        drm_test_pending_present_batch = batch;
        drm_test_pending_present_rect_word = batch->bytes / sizeof(uint32_t) + 3u;
    }
    batch->bytes += packet_bytes;
    ++drm_test_batch_append_calls;
    return 0;
}

int svga_batch_submit(struct svga_batch *batch, svga_handle *fence)
{
    if (!batch || !batch->metadata || !fence ||
        batch->generation != drm_test_batch_generation) return SVGA_EINVAL;
    ++drm_test_batch_submit_calls;
    if (drm_test_batch_busy_submits) {
        --drm_test_batch_busy_submits;
        return SVGA_EBUSY;
    }
    if (drm_test_batch_submit_failure) {
        batch->submit_failure = (struct svga_batch_submit_failure){
            .reason = SVGA_BATCH_SUBMIT_REASON_SHADER,
            .index = 7u,
            .detail0 = SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID,
            .detail1 = 19u,
            .detail2 = 20u,
        };
        return drm_test_batch_submit_failure;
    }
    if (drm_test_pending_present_batch == batch) {
        __builtin_memcpy(&drm_test_published_present_rect,
            batch->words + drm_test_pending_present_rect_word,
            sizeof(drm_test_published_present_rect));
        drm_test_pending_present_batch = NULL;
        ++drm_test_present_submitted;
    }
    batch->submitted = true;
    kernel_free(batch->metadata);
    batch->metadata = NULL;
    uint32_t seq = ++drm_test_fence_issued;
    *fence = ((uint64_t)drm_test_svga_info.generation << 32) | seq;
    return 0;
}

int svga_batch_abort(struct svga_batch *batch)
{
    if (!batch || !batch->metadata || batch->submitted) return SVGA_EINVAL;
    ++drm_test_batch_abort_calls;
    if (drm_test_pending_present_batch == batch)
        drm_test_pending_present_batch = NULL;
    if (drm_test_batch_abort_rewrites_failure)
        batch->submit_failure = (struct svga_batch_submit_failure){
            .reason = SVGA_BATCH_SUBMIT_REASON_NONE,
            .index = SVGA_BATCH_SUBMIT_INDEX_NONE,
            .detail0 = 0,
            .detail1 = 0,
            .detail2 = 0,
        };
    kernel_free(batch->metadata);
    batch->metadata = NULL;
    return 0;
}

int svga_batch_retain_context(struct svga_batch *batch, svga_handle context)
{ if (!batch || !batch->metadata || !context) return SVGA_EINVAL; ++batch->context_count; return 0; }
int svga_batch_retain_surface(struct svga_batch *batch, svga_handle surface)
{ if (!batch || !batch->metadata || !surface) return SVGA_EINVAL; ++batch->surface_count; return 0; }
int svga_batch_retain_gmr(struct svga_batch *batch, svga_handle gmr)
{
    if (!batch || !batch->metadata || !gmr) return SVGA_EINVAL;
    drm_test_last_retained_gmr = gmr;
    ++batch->gmr_count;
    return 0;
}
int svga_batch_stage_render_target(struct svga_batch *batch, svga_handle context,
                                  uint32_t target, svga_handle surface,
                                  uint32_t face, uint32_t mipmap)
{ (void)target; (void)surface; (void)face; (void)mipmap; return batch && context ? 0 : SVGA_EINVAL; }
int svga_batch_stage_texture(struct svga_batch *batch, svga_handle context,
                             uint32_t stage, svga_handle surface)
{ (void)stage; (void)surface; return batch && context ? 0 : SVGA_EINVAL; }
int svga_batch_shader_define(struct svga_batch *batch, svga_handle context,
                             uint32_t local_id, uint32_t type,
                             const void *bytecode, uint32_t bytes)
{ (void)bytecode; if (!batch || !context) return SVGA_EINVAL; drm_test_record_stage(DRM_TEST_STAGE_SHADER_DEFINE, local_id, bytes); (void)type; return 0; }
int svga_batch_shader_destroy(struct svga_batch *batch, svga_handle context,
                              uint32_t local_id, uint32_t type)
{ if (!batch || !context) return SVGA_EINVAL; drm_test_record_stage(DRM_TEST_STAGE_SHADER_DESTROY, local_id, type); return 0; }
int svga_batch_bind_shader(struct svga_batch *batch, svga_handle context,
                           uint32_t type, uint32_t local_id)
{ if (!batch || !context) return SVGA_EINVAL; drm_test_record_stage(DRM_TEST_STAGE_SHADER_BIND, local_id, type); return 0; }
int svga_batch_shader_constants(struct svga_batch *batch, svga_handle context,
                                uint32_t reg, uint32_t type, uint32_t const_type,
                                const uint32_t *values, uint32_t registers)
{
    if (!batch || !context || !values || !registers ||
        registers > DRM_TEST_SHADER_CONSTANT_REGISTER_MAX ||
        drm_test_shader_constant_capture_count >=
            DRM_TEST_SHADER_CONSTANT_CALL_MAX)
        return SVGA_EINVAL;
    struct drm_test_shader_constant_capture *capture =
        &drm_test_shader_constant_captures[
            drm_test_shader_constant_capture_count++];
    capture->reg = reg;
    capture->type = type;
    capture->const_type = const_type;
    capture->registers = registers;
    __builtin_memcpy(capture->values, values,
                     registers * 4u * sizeof(capture->values[0]));
    drm_test_record_stage(DRM_TEST_STAGE_SHADER_CONSTANTS, reg, registers);
    return 0;
}
int svga_batch_query_begin(struct svga_batch *batch, svga_handle context,
                           uint32_t type)
{ (void)type; return batch && context ? 0 : SVGA_EINVAL; }
int svga_batch_query_end(struct svga_batch *batch, svga_handle context,
                         uint32_t type, svga_handle result, uint32_t offset)
{ (void)type; (void)result; (void)offset; return batch && context ? 0 : SVGA_EINVAL; }
int svga_batch_query_wait(struct svga_batch *batch, svga_handle context,
                          uint32_t type, svga_handle result, uint32_t offset)
{ (void)type; (void)result; (void)offset; return batch && context ? 0 : SVGA_EINVAL; }

int svga_gmr_borrowed_attach(uint64_t phys, uint32_t bytes, void *owner,
                             void (*owner_put)(void *owner), svga_handle *out)
{
    if (!phys || !bytes || !owner || !owner_put || !out ||
        drm_test_borrowed_count >= 128) return SVGA_EINVAL;
    svga_handle handle = drm_test_next_gmr++;
    drm_test_borrowed[drm_test_borrowed_count++] = (struct drm_test_borrowed){
        .handle = handle, .owner = owner, .owner_put = owner_put};
    *out = handle;
    return 0;
}

int svga_gmr_borrowed_mob_id(svga_handle handle, uint32_t *mobid)
{
    if (!handle || !mobid) return SVGA_EINVAL;
    *mobid = svga_resource_id(handle);
    return 0;
}

int svga_gmr_borrowed_detach(svga_handle handle)
{
    ++drm_test_borrowed_detach_attempts;
    for (uint32_t i = 0; i < drm_test_borrowed_count; ++i) {
        if (drm_test_borrowed[i].handle != handle) continue;
        if (handle == drm_test_borrowed_detach_fence_handle) {
            struct drm_test_borrowed *pending = &drm_test_borrowed[i];
            if (!pending->destruction_published) {
                pending->destruction_published = true;
                pending->retirement_fence_seq = ++drm_test_fence_issued;
                ++drm_test_borrowed_detach_publications;
            }
            if (drm_test_fence_passed < pending->retirement_fence_seq) {
                ++drm_test_borrowed_detach_busy_returns;
                return SVGA_EBUSY;
            }
        }
        struct drm_test_borrowed borrowed = drm_test_borrowed[i];
        drm_test_borrowed[i] = drm_test_borrowed[--drm_test_borrowed_count];
        ++drm_test_borrowed_owner_put_calls;
        borrowed.owner_put(borrowed.owner);
        return 0;
    }
    return SVGA_EINVAL;
}

int svga_backend_reap_retired(uint32_t budget, uint32_t *reaped)
{
    ++drm_test_shader_reaper_calls;
    drm_test_shader_reaper_budget = budget;
    uint32_t count = budget < 2u ? budget : 2u;
    drm_test_shader_backings_reaped += count;
    if (reaped) *reaped = count;
    return 0;
}

void svga_get_info(struct svga_info *out)
{
    if (out) *out = drm_test_svga_info;
}

int svga_format(uint32_t format, uint32_t *block_width,
                uint32_t *block_height, uint32_t *bytes_per_block,
                uint32_t *capability)
{
    if (!block_width || !block_height || !bytes_per_block || !capability)
        return SVGA_EINVAL;
    *block_width = *block_height = 1;
    *bytes_per_block = 4;
    *capability = 0;
    if (format == SVGA3D_X8R8G8B8 || format == SVGA3D_A8R8G8B8) return 0;
    if (format == SVGA3D_ALPHA8) {
        *bytes_per_block = 1;
        return 0;
    }
    return SVGA_ENOTSUP;
}

int svga3d_capability(uint32_t index, uint32_t *out)
{
    if (!out || index >= DRM_TEST_COMPAT_DEVCAP_MAX ||
        !drm_test_svga_cap_valid[index])
        return SVGA_ENOTSUP;
    *out = drm_test_svga_caps[index];
    return 0;
}

int svga3d_format_operations(uint32_t format, uint32_t *out)
{
    if (!out) return SVGA_EINVAL;
    if (format == SVGA3D_ALPHA8) {
        *out = SVGA3DFORMAT_OP_TEXTURE |
               (drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] &
                DRM_TEST_FORMAT_NEGATIVE_MASK);
        return 0;
    }
    if (format == SVGA3D_X8R8G8B8 || format == SVGA3D_A8R8G8B8) {
        uint32_t cap = format == SVGA3D_X8R8G8B8
            ? SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8
            : SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8;
        *out = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
               SVGA3DFORMAT_OP_VOLUMETEXTURE |
               SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
               SVGA3DFORMAT_OP_SRGBREAD |
               (drm_test_svga_caps[cap] & DRM_TEST_FORMAT_NEGATIVE_MASK);
        return 0;
    }
    if (format == SVGA3D_Z_D16 || format == SVGA3D_Z_D24S8) {
        uint32_t cap = format == SVGA3D_Z_D16
            ? SVGA3D_DEVCAP_SURFACEFMT_Z_D16
            : SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8;
        *out = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL;
        *out |= drm_test_svga_caps[cap] & DRM_TEST_FORMAT_NEGATIVE_MASK;
        return 0;
    }
    *out = 0;
    return format == SVGA3D_BUFFER ? 0 : SVGA_ENOTSUP;
}

int svga3d_context_try_create(svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    *out = ++drm_test_next_backend_handle;
    ++drm_test_context_create_calls;
    return drm_test_context_create_status;
}

int svga3d_context_try_destroy(svga_handle handle)
{
    assert(handle);
    if (drm_test_context_destroy_attempt_count <
        sizeof(drm_test_context_destroy_attempts) /
            sizeof(drm_test_context_destroy_attempts[0]))
        drm_test_context_destroy_attempts[drm_test_context_destroy_attempt_count] =
            handle;
    ++drm_test_context_destroy_attempt_count;
    ++drm_test_context_destroy_calls;
    if (drm_test_context_destroy_busy) {
        --drm_test_context_destroy_busy;
        return SVGA_EBUSY;
    }
    return 0;
}

int svga3d_surface_try_create(const struct svga_surface_desc *desc,
                              svga_handle *out)
{
    if (!desc || !out) return SVGA_EINVAL;
    drm_test_last_surface_desc = *desc;
    *out = ++drm_test_next_backend_handle;
    ++drm_test_surface_create_calls;
    return drm_test_surface_create_status;
}

int svga3d_surface_try_destroy(svga_handle handle)
{
    assert(handle);
    ++drm_test_surface_destroy_calls;
    if (drm_test_surface_destroy_busy) {
        --drm_test_surface_destroy_busy;
        return SVGA_EBUSY;
    }
    return 0;
}

uint32_t svga_resource_id(svga_handle resource)
{
    return (uint32_t)(resource & 0x00ffffffu);
}

#define DRM_TEST_WIDTH 64u
#define DRM_TEST_HEIGHT 48u
#define DRM_FORMAT_XRGB8888_TEST 0x34325258u
static struct framebuffer test_framebuffer = {
    .width = DRM_TEST_WIDTH,
    .height = DRM_TEST_HEIGHT,
    .pitch = DRM_TEST_WIDTH * 4u,
    .max_width = DRM_TEST_WIDTH,
    .max_height = DRM_TEST_HEIGHT,
    .max_bytes = DRM_TEST_WIDTH * DRM_TEST_HEIGHT * 4u,
    .backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA,
    .bpp = 32,
    .bytes_per_pixel = 4,
    .available = true,
};
static uint32_t displayed_pixels[DRM_TEST_WIDTH * DRM_TEST_HEIGHT];
static unsigned scanout_begin_calls;
static unsigned scanout_release_calls;
static unsigned scanout_present_calls;

const struct framebuffer *framebuffer_get(void)
{
    return &test_framebuffer;
}

int framebuffer_drm_scanout_begin(void)
{
    ++scanout_begin_calls;
    return 0;
}

void framebuffer_drm_scanout_release(void)
{
    ++scanout_release_calls;
}

int framebuffer_drm_present_region(const void *pixels, uint32_t pitch,
                                   uint32_t format, uint32_t x1, uint32_t y1,
                                   uint32_t x2, uint32_t y2)
{
    assert(pixels && pitch >= DRM_TEST_WIDTH * 4u);
    assert(format == DRM_FORMAT_XRGB8888_TEST);
    assert(x1 < x2 && y1 < y2 && x2 <= DRM_TEST_WIDTH && y2 <= DRM_TEST_HEIGHT);
    const uint8_t *source = pixels;
    for (uint32_t y = y1; y < y2; ++y)
        memcpy(displayed_pixels + (uint64_t)y * DRM_TEST_WIDTH + x1,
               source + (uint64_t)y * pitch + (uint64_t)x1 * 4u,
               (x2 - x1) * 4u);
    ++scanout_present_calls;
    return 0;
}

void *paging_kernel_direct_map(uint64_t physical)
{
    return (void *)(uintptr_t)physical;
}

void *kernel_malloc(size_t size)
{
    if (fail_kernel_malloc_after == 0) return NULL;
    if (fail_kernel_malloc_after > 0) --fail_kernel_malloc_after;
    void *value = malloc(size);
    if (value) ++allocations;
    return value;
}

void kernel_free(void *value)
{
    if (value) {
        assert(allocations > 0);
        --allocations;
        free(value);
    }
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    if (flags) *flags = 0;
    while (__atomic_exchange_n(&lock->state, 1u, __ATOMIC_ACQUIRE)) { }
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
}

void kernel_wait_queue_block_current(struct kernel_wait_queue *queue)
{
    (void)queue;
}

int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{
    (void)queue;
    (void)task;
    ++wait_queue_add_calls;
    return queue && queue->count >= KERNEL_WAIT_QUEUE_MAX ? -1 : 0;
}

void sched_block_current(void)
{
    ++block_current_calls;
}

void sched_sleep_current_until(uint64_t wake_tick)
{
    ++sleep_current_calls;
    sleep_current_deadline = wake_tick;
}

void sched_request_reschedule_current(void)
{
    ++reschedule_current_calls;
    if (reschedule_expected_task) {
        const struct drm_vmwgfx_task_continuation *continuation =
            reschedule_expected_task->drm_vmwgfx_continuation;
        assert(continuation &&
               continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
               continuation->snapshot.wait.handle ==
                   reschedule_expected_handle &&
               continuation->snapshot.wait.lazy ==
                   reschedule_expected_lazy &&
               continuation->deadline == reschedule_expected_deadline);
    }
}

uint64_t time_ticks(void)
{
    return drm_test_ticks;
}

int time_clock_getres(int32_t clock, struct linux_timespec *resolution)
{
    ++drm_test_clock_getres_calls;
    assert(clock == LINUX_CLOCK_MONOTONIC);
    if (drm_test_clock_getres_status) return drm_test_clock_getres_status;
    if (resolution) {
        resolution->tv_sec = 0;
        resolution->tv_nsec = drm_test_clock_resolution_ns;
    }
    return 0;
}

uint64_t time_uptime_us(void)
{
    ++drm_test_uptime_calls;
    if (drm_test_uptime_script_index < drm_test_uptime_script_count)
        return drm_test_uptime_script[drm_test_uptime_script_index++];
    if (drm_test_uptime_advance_after_calls &&
        drm_test_uptime_calls >= drm_test_uptime_advance_after_calls) {
        uint64_t previous = drm_test_uptime_value;
        drm_test_uptime_value = drm_test_uptime_advance_to;
        drm_test_uptime_advance_after_calls = 0;
        return previous;
    }
    uint64_t result = drm_test_uptime_value;
    drm_test_uptime_value += drm_test_uptime_step;
    return result;
}

int kernel_execution_suspend_irqrestore(
    struct kernel_execution_suspend_token *token)
{
    ++drm_test_suspend_calls;
    assert(token && !token->active);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled);
    if (drm_test_suspend_status) return drm_test_suspend_status;
    token->active = 1;
    drm_test_execution_owned = false;
    drm_test_interrupts_enabled = true;
    return 0;
}

int kernel_execution_resume_irqsave(
    struct kernel_execution_suspend_token *token)
{
    ++drm_test_resume_calls;
    assert(token && token->active);
    assert(!drm_test_execution_owned && drm_test_interrupts_enabled);
    if (drm_test_resume_status) return drm_test_resume_status;
    token->active = 0;
    drm_test_execution_owned = true;
    drm_test_interrupts_enabled = false;
    return 0;
}

static void drm_test_fence_status_script_reset(void)
{
    drm_test_fence_status_script_count = 0;
    drm_test_fence_status_script_index = 0;
}

static void drm_test_fence_status_script_push(int status, bool signaled)
{
    assert(drm_test_fence_status_script_count <
           sizeof(drm_test_fence_status_script) /
               sizeof(drm_test_fence_status_script[0]));
    uint32_t index = drm_test_fence_status_script_count++;
    drm_test_fence_status_script[index] = status;
    drm_test_fence_signaled_script[index] = signaled;
}

static void drm_test_uptime_reset(uint64_t value, uint64_t step)
{
    drm_test_uptime_value = value;
    drm_test_uptime_step = step;
    drm_test_uptime_calls = 0;
    drm_test_uptime_advance_after_calls = 0;
    drm_test_uptime_advance_to = 0;
    drm_test_uptime_script_count = 0;
    drm_test_uptime_script_index = 0;
}

uint64_t mm_alloc_pages(uint32_t page_count)
{
    if (!page_count || mm_allocation_fails) return 0;
    size_t bytes = (size_t)page_count * 4096u;
    void *memory = aligned_alloc(4096u, bytes);
    if (!memory) return 0;
    memset(memory, 0, bytes);
    mm_pages_allocated += page_count;
    return (uint64_t)(uintptr_t)memory;
}

void mm_free_pages(uint64_t physical, uint32_t page_count)
{
    assert(physical && page_count);
    assert(!(physical & 4095u));
    mm_pages_freed += page_count;
    free((void *)(uintptr_t)physical);
}

bool user_range_ok(uint64_t address, uint64_t length)
{
    return length == 0 || (address >= 4096 && address <= UINTPTR_MAX - length);
}

bool user_range_writable(uint64_t address, uint64_t length)
{
    return user_range_ok(address, length);
}

int user_copy_to_task(struct task *task, uint64_t address,
                      const void *source, uint64_t length)
{
    (void)task;
    if (address == fail_copyout_address) return -LINUX_EFAULT;
    if (!user_range_writable(address, length)) return -LINUX_EFAULT;
    if (length) memcpy((void *)(uintptr_t)address, source, (size_t)length);
    return 0;
}

int user_copy_from_task(struct task *task, void *destination, uint64_t address,
                        uint64_t length)
{
    (void)task;
    if (!user_range_ok(address, length)) return -LINUX_EFAULT;
    if (length) memcpy(destination, (const void *)(uintptr_t)address, (size_t)length);
    return 0;
}

static struct task make_task(uint32_t tgid, bool admin)
{
    return (struct task){
        .pid = tgid,
        .tgid = tgid,
        .cap_effective = admin ? (1ULL << CAP_SYS_ADMIN) : 0,
    };
}

#define DRM_TEST_FD_GROUPS 16u
#define DRM_TEST_FD_SLOTS 16u
#define DRM_TEST_OFD_SLOTS 64u
static struct task_file drm_test_fd_slots[DRM_TEST_FD_GROUPS][DRM_TEST_FD_SLOTS];
static struct task_file drm_test_ofd_slots[DRM_TEST_OFD_SLOTS];

static uint32_t drm_test_fd_group(const struct task *task)
{
    return task ? task->pid % DRM_TEST_FD_GROUPS : 0;
}

struct task_file *task_file_for_fd(struct task *task, int fd)
{
    if (!task || fd < 0 || fd >= (int)DRM_TEST_FD_SLOTS) return NULL;
    struct task_file *slot = &drm_test_fd_slots[drm_test_fd_group(task)][fd];
    return slot->used ? task_file_description(slot) : NULL;
}

struct task_file *task_file_for_io(struct task *task, int fd)
{
    return task_file_for_fd(task, fd);
}

int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    if (!task || !out || minimum < 0) return -LINUX_EINVAL;
    uint32_t group = drm_test_fd_group(task);
    for (int fd = minimum; fd < (int)DRM_TEST_FD_SLOTS; ++fd) {
        struct task_file *slot = &drm_test_fd_slots[group][fd];
        if (slot->used) continue;
        *slot = (struct task_file){.used = 1};
        *out = slot;
        return fd;
    }
    return -LINUX_EMFILE;
}

static struct task_file *drm_test_promote_file(struct task_file *source)
{
    if (!source || !source->used) return NULL;
    if (source->description) return source->description;
    if (source->references) return source;
    for (uint32_t i = 0; i < DRM_TEST_OFD_SLOTS; ++i) {
        struct task_file *description = &drm_test_ofd_slots[i];
        if (description->used) continue;
        *description = *source;
        description->used = 1;
        description->references = 1;
        description->fd_flags = 0;
        source->description = description;
        return description;
    }
    return NULL;
}

int task_file_reference(struct task_file *destination, struct task_file *source)
{
    if (!destination) return -LINUX_EINVAL;
    struct task_file *description = drm_test_promote_file(source);
    if (!description) return -LINUX_EBADF;
    *destination = (struct task_file){
        .used = 1, .description = description,
    };
    ++description->references;
    return 0;
}

struct task_file *task_file_get(struct task_file *source)
{
    struct task_file *description = drm_test_promote_file(source);
    if (description) ++description->references;
    return description;
}

static void drm_test_ofd_release(struct task_file *description)
{
    if (description->drm_prime_file) drm_prime_file_close(description);
    *description = (struct task_file){0};
}

void task_file_put(struct task_file *description)
{
    if (description && description->references &&
        --description->references == 0)
        drm_test_ofd_release(description);
}

void task_discard_file_fd(struct task *task, int fd)
{
    if (!task || fd < 0 || fd >= (int)DRM_TEST_FD_SLOTS) return;
    struct task_file *slot = &drm_test_fd_slots[drm_test_fd_group(task)][fd];
    if (!slot->used) return;
    if (slot->description) {
        struct task_file *description = slot->description;
        *slot = (struct task_file){0};
        if (description->references && --description->references == 0)
            drm_test_ofd_release(description);
    } else {
        if (slot->drm_prime_file) drm_prime_file_close(slot);
        *slot = (struct task_file){0};
    }
}

int file_can_read(const struct task_file *file)
{
    return file && (file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY;
}

int file_can_write(const struct task_file *file)
{
    return file && (file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY;
}

uint64_t mm_free_memory_kib(void)
{
    return 12345u;
}

static struct pci_device vmware_device(void)
{
    return (struct pci_device){
        .bus = 0,
        .slot = 15,
        .function = 0,
        .vendor_id = 0x15ad,
        .device_id = 0x0405,
    };
}

static int ioctl_call(struct task *task, struct task_file *file,
                      uint32_t command, void *argument)
{
    return (int)drm_file_ioctl(task, file, command,
                               (uint64_t)(uintptr_t)argument);
}

static struct drm_vmw_fence_rep drm_test_submit_wait_fence(
    struct task *task, struct task_file *file)
{
    struct drm_vmw_fence_rep reply = {0};
    struct drm_vmw_execbuf_arg exec = {
        .fence_rep = (uint64_t)(uintptr_t)&reply,
        .version = 1,
    };
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_EXECBUF, &exec) == 0);
    assert(reply.handle && reply.seqno == drm_test_fence_issued);
    return reply;
}

static uint32_t test_append_packet(uint32_t *stream, uint32_t capacity_dwords,
                                   uint32_t offset, uint32_t command,
                                   const void *body, uint32_t bytes)
{
    assert(stream && !(bytes & 3u));
    assert(offset <= capacity_dwords &&
           2u + bytes / sizeof(uint32_t) <= capacity_dwords - offset);
    stream[offset++] = command;
    stream[offset++] = bytes;
    if (bytes) memcpy(&stream[offset], body, bytes);
    return offset + bytes / sizeof(uint32_t);
}

static void drm_test_record_stage(uint32_t kind, uint32_t first,
                                  uint32_t second)
{
    assert(drm_test_stage_event_count <
           sizeof(drm_test_stage_events) / sizeof(drm_test_stage_events[0]));
    drm_test_stage_events[drm_test_stage_event_count++] =
        (struct drm_test_stage_event){kind, first, second};
}

static void test_registration_and_open_lifetime(void)
{
    struct task unprivileged = make_task(100, false);
    struct task stranger = make_task(201, false);
    struct task admin = make_task(200, true);
    struct task_file first = {0}, second = {0}, third = {0};
    struct pci_device identity;
    struct pci_device invalid = vmware_device();
    invalid.vendor_id = 0x1234;

    assert(drm_device_register(&invalid) == -LINUX_EINVAL);
    assert(!drm_device_is_registered());
    assert(drm_file_open(&unprivileged, &first) == -LINUX_ENODEV);
    assert(!first.drm_file && allocations == 0);

    struct pci_device pci = vmware_device();
    assert(drm_device_register(&pci) == 0);
    assert(drm_device_register(&pci) == 0);
    assert(drm_device_pci_identity(&identity) == 0);
    assert(identity.bus == 0 && identity.slot == 15 && identity.function == 0);
    assert(drm_file_open(&unprivileged, &first) == 0);
    assert(first.drm_file && first.drm_file->is_master);
    assert(first.drm_file->authenticated);
    assert(drm_file_open(&admin, &second) == 0);
    assert(second.drm_file && !second.drm_file->is_master);
    struct drm_auth second_magic = {0};
    assert(ioctl_call(&admin, &second, DRM_IOCTL_GET_MAGIC, &second_magic) == 0);
    assert(second_magic.magic && first.drm_file->master->magics);
    assert(drm_file_set_master(&unprivileged, second.drm_file) == -LINUX_EACCES);
    assert(drm_file_set_master(&admin, second.drm_file) == -LINUX_EBUSY);
    assert(drm_file_drop_master(&stranger, first.drm_file) == -LINUX_EACCES);
    assert(drm_file_drop_master(&unprivileged, first.drm_file) == 0);
    second.drm_file->authenticated = false;
    assert(drm_file_set_master(&admin, second.drm_file) == 0);
    assert(second.drm_file->authenticated);
    assert(!first.drm_file->master->magics);
    drm_file_close(&first);
    assert(drm_device_get()->master == second.drm_file->master);
    drm_file_close(&second);
    assert(!drm_device_get()->master);
    assert(drm_file_open(&admin, &third) == 0 && third.drm_file->is_master);
    drm_file_close(&third);
    assert(allocations == 0);
}

static void test_render_open_never_takes_primary_master(void)
{
    struct task renderer = make_task(250, true);
    struct task primary_task = make_task(251, false);
    struct task_file render = {
        .node = {.first_cluster = DRM_TEST_RENDER_NODE_KIND},
    };
    struct task_file primary = {0};

    assert(drm_device_is_registered());
    assert(!drm_device_get()->master);
    memset(drm_test_svga_cap_valid, 0, sizeof(drm_test_svga_cap_valid));
    drm_test_svga_cap_valid[SVGA3D_DEVCAP_3D] = true;
    drm_test_svga_caps[SVGA3D_DEVCAP_3D] = 1;
    drm_test_svga_cap_valid[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION] = true;
    drm_test_svga_caps[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION] = 7;
    drm_test_svga_cap_valid[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION] = true;
    drm_test_svga_caps[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION] = 13;
    assert(drm_file_open(&renderer, &render) == 0);
    assert(render.drm_file);
    assert(!render.drm_file->master);
    assert(!render.drm_file->is_master);
    assert(!render.drm_file->authenticated);
    assert(!drm_device_get()->master);

    /* Opening a render node first must leave the next primary opener able to
     * acquire the actual primary master. Closing render must not release it. */
    assert(drm_file_open(&primary_task, &primary) == 0);
    assert(primary.drm_file && primary.drm_file->is_master);
    assert(primary.drm_file->master == drm_device_get()->master);
    struct drm_master *primary_master = primary.drm_file->master;
    drm_file_close(&render);
    assert(drm_device_get()->master == primary_master);
    drm_file_close(&primary);
    assert(!drm_device_get()->master);
    assert(allocations == 0);
}

static void test_render_ioctl_permission_matrix(void)
{
    struct task root = make_task(252, true);
    struct task user = make_task(253, false);
    struct task_file root_render = {
        .node = {.first_cluster = DRM_TEST_RENDER_NODE_KIND},
    };
    struct task_file user_render = {
        .node = {.first_cluster = DRM_TEST_RENDER_NODE_KIND},
    };
    struct drm_version version = {0};
    struct drm_get_cap cap = {.capability = DRM_CAP_PRIME};
    struct drm_vmw_getparam_arg param = {.param = DRM_VMW_PARAM_3D};
    char name[16] = {0};

    assert(drm_file_open(&root, &root_render) == 0);
    assert(drm_file_open(&user, &user_render) == 0);
    assert(!root_render.drm_file->authenticated &&
           !user_render.drm_file->authenticated);
    assert(!root_render.drm_file->master && !user_render.drm_file->master);

    version.name = name;
    version.name_len = sizeof(name);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_VERSION, &version) == 0);
    assert(!strcmp(name, "vmwgfx"));
    cap.value = UINT64_MAX;
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_GET_CAP, &cap) == 0 &&
           cap.value == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT));
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == 1);

    union drm_vmw_alloc_bo_arg alloc = {0};
    alloc.req.size = 4096;
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_VMW_ALLOC_BO,
                      &alloc) == 0);
    assert(alloc.rep.handle);
    struct drm_vmw_synccpu_arg sync = {
        .op = drm_vmw_synccpu_grab,
        .flags = drm_vmw_synccpu_write,
        .handle = alloc.rep.handle,
    };
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == 0);
    sync.op = drm_vmw_synccpu_release;
    sync.flags = drm_vmw_synccpu_read;
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == 0);
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_VMW_UNREF_BO,
                      &(struct drm_vmw_handle_close_arg){
                          .handle = alloc.rep.handle,
                      }) == 0);

    struct drm_auth magic = {0};
    struct drm_unique unique = {0};
    struct drm_mode_create_dumb dumb = {.width = 1, .height = 1, .bpp = 32};
    struct drm_mode_card_res resources = {0};
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_GET_MAGIC, &magic) ==
           -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_GET_UNIQUE, &unique) ==
           -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_AUTH_MAGIC, &magic) ==
           -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_SET_MASTER, NULL) ==
           -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_DROP_MASTER, NULL) ==
           -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_GEM_FLINK,
                      &(struct drm_gem_flink){0}) == -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_GEM_OPEN,
                      &(struct drm_gem_open){0}) == -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == -LINUX_EACCES);
    assert(ioctl_call(&root, &root_render, DRM_IOCTL_MODE_CREATE_DUMB,
                      &dumb) == -LINUX_EACCES);
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_GET_MAGIC, &magic) ==
           -LINUX_EACCES);
    assert(ioctl_call(&user, &user_render, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == -LINUX_EACCES);
    assert(drm_file_set_master(&root, root_render.drm_file) == -LINUX_EACCES);
    assert(drm_file_drop_master(&root, root_render.drm_file) == -LINUX_EACCES);

    drm_file_close(&user_render);
    drm_file_close(&root_render);
    assert(allocations == 0);
}

static void test_core_abi_and_validation(void)
{
    struct task master_task = make_task(300, true);
    struct task client_task = make_task(301, false);
    struct task_file master = {0}, client = {0};
    char name[4] = {0}, date[1] = {0}, desc[8] = {0};
    struct drm_version version = {
        .name_len = sizeof(name), .name = name,
        .date_len = sizeof(date), .date = date,
        .desc_len = sizeof(desc), .desc = desc,
    };
    struct drm_get_cap cap = {.capability = DRM_CAP_TIMESTAMP_MONOTONIC};
    struct drm_set_client_cap client_cap = {
        .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1,
    };
    struct drm_client client_info = {0};
    struct drm_stats stats = {0};
    struct drm_set_version set_version = {
        .drm_di_major = 1, .drm_di_minor = 4,
        .drm_dd_major = 2, .drm_dd_minor = 1,
    };

    assert(drm_file_open(&master_task, &master) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, &version) == 0);
    assert(version.version_major == 2 && version.version_minor == 1 &&
           version.version_patchlevel == 0);
    assert(version.name_len == 6 && !memcmp(name, "vmwg", 4));
    assert(version.date_len == 1 && date[0] == '0');
    assert(version.desc_len == strlen("Linux drm driver for VMware graphics devices"));

    assert(ioctl_call(&master_task, &master, DRM_IOCTL_SET_VERSION,
                      &set_version) == 0);
    assert(set_version.drm_di_major == 1 && set_version.drm_di_minor == 4);
    assert(set_version.drm_dd_major == 2 && set_version.drm_dd_minor == 1);
    struct drm_unique unique = {.unique_len = 0, .unique = NULL};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    static const char expected_busid[] = "pci:0000:00:0f.0";
    assert(unique.unique_len == sizeof(expected_busid) - 1);
    char busid[sizeof(expected_busid)] = {0};
    unique.unique = busid;
    unique.unique_len = sizeof(busid) - 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    assert(!memcmp(busid, expected_busid, sizeof(expected_busid)));
    char short_busid[4] = {'x', 'x', 'x', 'x'};
    unique.unique = short_busid;
    unique.unique_len = sizeof(short_busid) - 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    assert(unique.unique_len == sizeof(expected_busid) - 1);
    assert(!memcmp(short_busid, "xxxx", sizeof(short_busid)));

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 1);
    cap.capability = DRM_CAP_PRIME;
    cap.value = UINT64_MAX;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT));
    cap.capability = DRM_CAP_DUMB_BUFFER;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0 &&
           cap.value == 1);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_CLIENT_CAP,
                      &client_cap) == -LINUX_EOPNOTSUPP);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_VERSION,
                      (void *)(uintptr_t)1) == -LINUX_EFAULT);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_VERSION,
                      &set_version) == -LINUX_EACCES);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CLIENT,
                      &client_info) == -LINUX_EOPNOTSUPP);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_STATS,
                      &stats) == -LINUX_EOPNOTSUPP);

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, NULL) == -LINUX_EFAULT);
    version.name_len = 1;
    version.name = (char *)(uintptr_t)1;
    version.date_len = 0;
    version.desc_len = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, &version) == -LINUX_EFAULT);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION ^ 0x100u,
                      &version) == -LINUX_ENOTTY);
    assert(ioctl_call(&client_task, &client, _IO('d', 0xff), &version) == -LINUX_EINVAL);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_SET_UNIQUE,
                      &(struct drm_unique){0}) == -LINUX_EINVAL);

    drm_file_close(&client);
    drm_file_close(&master);
    assert(!drm_device_get()->master && allocations == 0);
}

static void test_legacy_magic_and_empty_readiness(void)
{
    struct task master_task = make_task(400, true);
    struct task client_task = make_task(401, false);
    struct task_file master = {0}, client = {0};
    struct drm_auth auth = {0};
    char byte;

    assert(drm_file_open(&master_task, &master) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_MAGIC, &auth) == 0);
    assert(auth.magic != 0 && !client.drm_file->authenticated);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_AUTH_MAGIC, &auth) == 0);
    assert(client.drm_file->authenticated);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_AUTH_MAGIC, &auth) == -LINUX_EINVAL);
    assert(drm_file_read(&client_task, &client, &byte, 0) == 0);
    assert(drm_file_poll(&client, 0xffffu) == 0);
    client.flags |= RELIEFOS_O_NONBLOCK;
    assert(drm_file_read(&client_task, &client, &byte, 1) == -LINUX_EAGAIN);
    client.flags &= ~RELIEFOS_O_NONBLOCK;
    assert(drm_file_read(&client_task, &client, &byte, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(wait_queue_add_calls == 1 && block_current_calls == 1);
    client.drm_file->event_wait.count = KERNEL_WAIT_QUEUE_MAX;
    assert(drm_file_read(&client_task, &client, &byte, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(sleep_current_calls == 1 && sleep_current_deadline == 101);

    drm_file_close(&client);
    assert(master.drm_file->master->magics == NULL);
    drm_file_close(&master);
    assert(allocations == 0);
}

static void test_dumb_and_vmwgfx_bo_contract(void)
{
    struct task owner_task = make_task(500, false);
    struct task foreign_task = make_task(501, false);
    struct task_file owner = {0}, foreign = {0};
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    struct drm_mode_create_dumb create = {
        .height = 9, .width = 17, .bpp = 32, .flags = 0,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    struct drm_mode_map_dumb map = {0};
    struct drm_gem_close gem_close = {0};
    union drm_vmw_alloc_bo_arg alloc = {0};
    struct drm_vmw_handle_close_arg unref = {0};
    struct drm_vmw_synccpu_arg sync = {0};
    union drm_vmw_alloc_bo_arg secondary_alloc = {0};

    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&foreign_task, &foreign) == 0);
    assert(owner.drm_file->authenticated);
    assert(!foreign.drm_file->authenticated);

    /* Linux vmwgfx marks per-file BO ioctls DRM_RENDER_ALLOW, so an
     * unauthenticated secondary primary-node opener can manage its own BOs. */
    secondary_alloc.req.size = 4096;
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_VMW_ALLOC_BO,
                      &secondary_alloc) == 0);
    assert(secondary_alloc.rep.handle && secondary_alloc.rep.map_handle);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_BO,
                      &(struct drm_vmw_handle_close_arg){
                          .handle = secondary_alloc.rep.handle,
                      }) == -LINUX_EINVAL);
    sync.op = drm_vmw_synccpu_grab;
    sync.flags = drm_vmw_synccpu_write;
    sync.handle = secondary_alloc.rep.handle;
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == 0);
    sync.op = drm_vmw_synccpu_release;
    sync.flags = drm_vmw_synccpu_read;
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == 0);
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_VMW_UNREF_BO,
                      &(struct drm_vmw_handle_close_arg){
                          .handle = secondary_alloc.rep.handle,
                      }) == 0);

    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 1);
    cap.capability = DRM_CAP_DUMB_PREFERRED_DEPTH;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 32);
    cap.capability = DRM_CAP_DUMB_PREFER_SHADOW;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 0);

    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &create) == 0);
    assert(create.handle && create.pitch == create.width * 4u);
    assert(create.size >= (uint64_t)create.pitch * create.height);
    assert((create.size & 4095u) == 0);

    map.handle = create.handle;
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_MODE_MAP_DUMB,
                      &map) == -LINUX_EINVAL);
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &(struct drm_mode_destroy_dumb){.handle = create.handle}) ==
           -LINUX_EINVAL);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0);
    assert(map.offset && !(map.offset & 4095u));

    struct drm_mode_create_dumb invalid = {
        .height = 1, .width = 1, .bpp = 24, .flags = 0,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &invalid) == -LINUX_EINVAL);
    assert(!invalid.handle && !invalid.pitch && !invalid.size);

    struct drm_bo_handle *handles_before = owner.drm_file->bo_handles;
    const uint64_t pages_before = mm_pages_allocated;
    const uint64_t pages_freed_before = mm_pages_freed;
    struct drm_mode_create_dumb copyout_failure = {
        .height = 2, .width = 1024, .bpp = 32,
    };
    fail_copyout_address = (uint64_t)(uintptr_t)&copyout_failure;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &copyout_failure) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(owner.drm_file->bo_handles == handles_before);
    assert(mm_pages_allocated - pages_before ==
           mm_pages_freed - pages_freed_before);

    mm_allocation_fails = true;
    alloc.req.size = 4096;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_ALLOC_BO, &alloc) ==
           -LINUX_ENOMEM);
    mm_allocation_fails = false;
    assert(owner.drm_file->bo_handles == handles_before);

    alloc.req.size = 5000;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_ALLOC_BO, &alloc) == 0);
    assert(alloc.rep.handle && alloc.rep.map_handle &&
           !(alloc.rep.map_handle & 4095u));
    assert(alloc.rep.cur_gmr_id == alloc.rep.handle);
    assert(alloc.rep.cur_gmr_offset == 0);
    uint64_t vmwgfx_pages = drm_bo_size(owner.drm_file->bo_handles->bo) / 4096u;

    struct drm_bo *mapped_bo = NULL;
    uint64_t bo_offset = UINT64_MAX;
    assert(drm_file_mmap_bo(&owner, alloc.rep.map_handle, 8192,
                            &mapped_bo, &bo_offset) == 0);
    assert(mapped_bo && bo_offset == 0 && drm_bo_size(mapped_bo) == 8192);
    uint8_t *backing = (uint8_t *)(uintptr_t)drm_bo_page_physical(mapped_bo, 0);
    for (uint64_t i = 0; i < drm_bo_size(mapped_bo); ++i) assert(backing[i] == 0);
    struct drm_bo *foreign_bo = NULL;
    assert(drm_file_mmap_bo(&foreign, alloc.rep.map_handle, 4096,
                            &foreign_bo, &bo_offset) == -LINUX_EINVAL);
    assert(drm_file_mmap_bo(&owner, alloc.rep.map_handle + 4096, 4096,
                            &foreign_bo, &bo_offset) == 0);
    assert(foreign_bo == mapped_bo && bo_offset == 4096);
    assert(drm_bo_page_physical(mapped_bo, bo_offset) ==
           drm_bo_page_physical(mapped_bo, 4096));
    drm_bo_put(foreign_bo);
    assert(drm_file_mmap_bo(&owner, UINT64_MAX - 4095, 4096,
                            &foreign_bo, &bo_offset) == -LINUX_EINVAL);

    sync.op = drm_vmw_synccpu_grab;
    sync.flags = drm_vmw_synccpu_write;
    sync.handle = alloc.rep.handle;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU, &sync) == 0);
    sync.op = drm_vmw_synccpu_release;
    sync.flags = drm_vmw_synccpu_read;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU, &sync) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU, &sync) ==
           -LINUX_EINVAL);

    /* Repeated valid grabs aggregate in the per-handle record, without
     * allocating one kernel heap node per ioctl. */
    const unsigned repeated_grabs = 1024;
    sync.op = drm_vmw_synccpu_grab;
    sync.flags = drm_vmw_synccpu_read;
    unsigned allocations_before_grabs = allocations;
    for (unsigned i = 0; i < repeated_grabs; ++i)
        assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU,
                          &sync) == 0);
    assert(allocations == allocations_before_grabs);
    sync.op = drm_vmw_synccpu_release;
    for (unsigned i = 0; i < repeated_grabs; ++i)
        assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU,
                          &sync) == 0);
    assert(allocations == allocations_before_grabs);
    sync.op = drm_vmw_synccpu_grab;
    sync.flags = 0;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU, &sync) ==
           -LINUX_EINVAL);
    sync.flags = drm_vmw_synccpu_write;
    sync.handle = UINT32_MAX;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_SYNCCPU, &sync) ==
           -LINUX_EINVAL);

    unref.handle = alloc.rep.handle;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_BO, &unref) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_BO, &unref) ==
           -LINUX_EINVAL);
    assert(drm_file_mmap_bo(&owner, alloc.rep.map_handle, 4096,
                            &foreign_bo, &bo_offset) == -LINUX_EINVAL);
    backing[0] = 0x5a;
    assert(backing[0] == 0x5a &&
           mm_pages_allocated - mm_pages_freed == vmwgfx_pages + 1);
    drm_bo_put(mapped_bo);
    assert(mm_pages_allocated - mm_pages_freed == 1);
    gem_close.handle = create.handle;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GEM_CLOSE, &gem_close) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_MAP_DUMB, &map) ==
           -LINUX_EINVAL);
    assert(mm_pages_freed == mm_pages_allocated);

    drm_file_close(&foreign);
    drm_file_close(&owner);
    assert(allocations == 0);
}

static void test_kms_resources_properties_and_scanout(void)
{
    struct task master_task = make_task(600, false);
    struct task stranger_task = make_task(601, false);
    struct task_file master = {0}, stranger = {0};
    struct drm_mode_card_res resources = {0};
    struct drm_mode_get_connector connector = {0};
    struct drm_mode_get_encoder encoder = {0};
    struct drm_mode_crtc crtc = {0};
    uint32_t crtc_ids[1] = {0}, connector_ids[1] = {0}, encoder_ids[1] = {0};
    struct drm_mode_modeinfo mode = {0};
    uint32_t connector_encoder_ids[1] = {0};

    memset(displayed_pixels, 0, sizeof(displayed_pixels));
    scanout_begin_calls = scanout_release_calls = scanout_present_calls = 0;
    assert(drm_file_open(&master_task, &master) == 0);
    assert(drm_file_open(&stranger_task, &stranger) == 0);

    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == 0);
    assert(resources.count_fbs == 0 && resources.count_crtcs == 1 &&
           resources.count_connectors == 1 && resources.count_encoders == 1);
    assert(resources.min_width == 1 && resources.max_width == DRM_TEST_WIDTH &&
           resources.min_height == 1 && resources.max_height == DRM_TEST_HEIGHT);
    resources.crtc_id_ptr = 1;
    resources.count_crtcs = 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == -LINUX_EFAULT);
    resources.crtc_id_ptr = 0;
    resources.count_crtcs = 0;

    resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    resources.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids;
    resources.encoder_id_ptr = (uint64_t)(uintptr_t)encoder_ids;
    resources.count_crtcs = resources.count_connectors = resources.count_encoders = 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == 0);
    assert(crtc_ids[0] && connector_ids[0] && encoder_ids[0]);

    connector.connector_id = connector_ids[0];
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCONNECTOR,
                      &connector) == 0);
    assert(connector.connector_id == connector_ids[0] && connector.encoder_id ==
           encoder_ids[0]);
    assert(connector.connector_type == DRM_MODE_CONNECTOR_VIRTUAL &&
           connector.connection == 1 && connector.count_modes == 1 &&
           connector.count_encoders == 1 && connector.count_props == 0);
    connector.count_modes = 1;
    connector.modes_ptr = (uint64_t)(uintptr_t)&mode;
    connector.count_encoders = 1;
    connector.encoders_ptr = (uint64_t)(uintptr_t)connector_encoder_ids;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCONNECTOR,
                      &connector) == 0);
    assert(mode.hdisplay == DRM_TEST_WIDTH && mode.vdisplay == DRM_TEST_HEIGHT &&
           (mode.type & DRM_MODE_TYPE_PREFERRED));
    assert(connector_encoder_ids[0] == encoder_ids[0]);
    connector.modes_ptr = 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCONNECTOR,
                      &connector) == -LINUX_EFAULT);
    connector.modes_ptr = (uint64_t)(uintptr_t)&mode;

    encoder.encoder_id = encoder_ids[0];
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETENCODER,
                      &encoder) == 0);
    assert(encoder.encoder_id == encoder_ids[0] &&
           encoder.encoder_type == DRM_MODE_ENCODER_VIRTUAL &&
           encoder.possible_crtcs == 1 && !encoder.possible_clones);
    encoder.encoder_id = UINT32_MAX;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETENCODER,
                      &encoder) == -LINUX_ENOENT);

    crtc.crtc_id = crtc_ids[0];
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCRTC,
                      &crtc) == 0);
    assert(!crtc.mode_valid && !crtc.fb_id);
    struct drm_mode_obj_get_properties properties = {
        .props_ptr = 1,
        .prop_values_ptr = 1,
        .obj_id = connector_ids[0],
        .obj_type = DRM_MODE_OBJECT_CONNECTOR,
    };
    assert(ioctl_call(&master_task, &master,
                      DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) == 0);
    assert(properties.count_props == 0);
    properties.obj_id = crtc_ids[0];
    properties.obj_type = DRM_MODE_OBJECT_CRTC;
    properties.count_props = 0;
    assert(ioctl_call(&master_task, &master,
                      DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) == 0);
    properties.obj_id = UINT32_MAX;
    assert(ioctl_call(&master_task, &master,
                      DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &properties) ==
           -LINUX_ENOENT);

    struct drm_mode_create_dumb create = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT, .bpp = 32,
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_CREATE_DUMB,
                      &create) == 0);
    struct drm_bo *bo = master.drm_file->bo_handles->bo;
    uint32_t *source = (uint32_t *)(uintptr_t)drm_bo_page_physical(bo, 0);
    for (uint32_t i = 0; i < DRM_TEST_WIDTH * DRM_TEST_HEIGHT; ++i)
        source[i] = 0xff000000u | i;

    struct drm_mode_fb_cmd2 fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = DRM_FORMAT_XRGB8888_TEST,
        .handles = {create.handle}, .pitches = {create.pitch},
    };
    struct drm_mode_fb_cmd2 copyout_fb = fb;
    fail_copyout_address = (uint64_t)(uintptr_t)&copyout_fb;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2,
                      &copyout_fb) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(!copyout_fb.fb_id && bo->references == 1);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2, &fb) == 0);
    assert(fb.fb_id && bo->references == 2);
    uint32_t listed_fb_id = 0;
    resources.fb_id_ptr = (uint64_t)(uintptr_t)&listed_fb_id;
    resources.count_fbs = 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETRESOURCES,
                      &resources) == 0);
    assert(resources.count_fbs == 1 && listed_fb_id == fb.fb_id);
    struct drm_mode_fb_cmd2 foreign_fb = fb;
    foreign_fb.fb_id = 0;
    assert(ioctl_call(&stranger_task, &stranger, DRM_IOCTL_MODE_ADDFB2,
                      &foreign_fb) == -LINUX_EINVAL);

    struct drm_mode_fb_cmd2 invalid_fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = DRM_FORMAT_XRGB8888_TEST,
        .handles = {create.handle}, .pitches = {create.pitch - 1u},
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2,
                      &invalid_fb) == -LINUX_EINVAL);
    assert(!invalid_fb.fb_id);
    invalid_fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = DRM_FORMAT_XRGB8888_TEST,
        .handles = {create.handle}, .pitches = {create.pitch},
        .offsets = {create.size},
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2,
                      &invalid_fb) == -LINUX_EINVAL);
    invalid_fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = DRM_FORMAT_XRGB8888_TEST,
        .handles = {create.handle}, .pitches = {create.pitch},
        .modifier = {1},
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2,
                      &invalid_fb) == -LINUX_EINVAL);

    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uint64_t)(uintptr_t)connector_ids,
        .count_connectors = 1,
        .crtc_id = crtc_ids[0], .fb_id = fb.fb_id,
        .mode_valid = 1, .mode = mode,
    };
    /* drmmode_ConvertToKMode() zeroes the structure, copies timing/flags and
     * the name, and leaves these metadata hints zero. */
    set.mode.type = 0;
    set.mode.vrefresh = 0;
    struct drm_mode_crtc unsupported_mode = set;
    ++unsupported_mode.mode.clock;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &unsupported_mode) == -LINUX_EINVAL);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &set) == 0);
    assert(scanout_begin_calls == 1 && scanout_present_calls == 1);
    assert(displayed_pixels[0] == source[0] &&
           displayed_pixels[DRM_TEST_WIDTH * DRM_TEST_HEIGHT - 1u] ==
               source[DRM_TEST_WIDTH * DRM_TEST_HEIGHT - 1u]);
    /* Mode names are descriptive metadata; timings and flags define the
     * fixed hardware mode. */
    set.mode.name[0] = '\0';
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &set) == 0);
    assert(scanout_begin_calls == 1 && scanout_present_calls == 2);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_DROP_MASTER, NULL) == 0);
    assert(scanout_release_calls == 1);
    crtc.fb_id = UINT32_MAX;
    crtc.mode_valid = UINT32_MAX;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCRTC,
                      &crtc) == 0);
    assert(!crtc.fb_id && !crtc.mode_valid);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_SET_MASTER, NULL) == 0);
    set.set_connectors_ptr = 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &set) == -LINUX_EFAULT);
    set.set_connectors_ptr = (uint64_t)(uintptr_t)connector_ids;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &set) == 0);
    assert(scanout_begin_calls == 2 && scanout_present_calls == 3);
    assert(ioctl_call(&stranger_task, &stranger, DRM_IOCTL_MODE_DIRTYFB,
                      &(struct drm_mode_fb_dirty_cmd){.fb_id = fb.fb_id}) ==
           -LINUX_EACCES);
    struct drm_mode_atomic atomic = {0};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ATOMIC,
                      &atomic) == -LINUX_EOPNOTSUPP);
    struct drm_mode_crtc_page_flip flip = {.crtc_id = crtc_ids[0]};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_PAGE_FLIP,
                      &flip) == -LINUX_EOPNOTSUPP);
    struct drm_mode_get_property property = {.prop_id = UINT32_MAX};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETPROPERTY,
                      &property) == -LINUX_ENOENT);

    source[6u * DRM_TEST_WIDTH + 5u] = 0xff123456u;
    struct drm_clip_rect dirty_clip = {5, 6, 7, 8};
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb.fb_id, .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&dirty_clip,
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_DIRTYFB,
                      &dirty) == 0);
    assert(displayed_pixels[6u * DRM_TEST_WIDTH + 5u] == 0xff123456u);
    unsigned presents_before_bad_clip = scanout_present_calls;
    dirty_clip.x2 = DRM_TEST_WIDTH + 1u;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_DIRTYFB,
                      &dirty) == -LINUX_EINVAL);
    assert(scanout_present_calls == presents_before_bad_clip);

    struct drm_mode_destroy_dumb destroy = {.handle = create.handle};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &destroy) == 0);
    assert(bo->references == 2); /* framebuffer and active CRTC own BO refs */
    uint32_t fb_id = fb.fb_id;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);
    assert(scanout_release_calls == 2);
    crtc.fb_id = UINT32_MAX;
    crtc.mode_valid = UINT32_MAX;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_GETCRTC,
                      &crtc) == 0);
    assert(!crtc.fb_id && !crtc.mode_valid);
    assert(mm_pages_freed == mm_pages_allocated);

    drm_file_close(&stranger);
    create.handle = 0;
    create.width = DRM_TEST_WIDTH;
    create.height = DRM_TEST_HEIGHT;
    create.bpp = 32;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_CREATE_DUMB,
                      &create) == 0);
    bo = master.drm_file->bo_handles->bo;
    fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = DRM_FORMAT_XRGB8888_TEST,
        .handles = {create.handle}, .pitches = {create.pitch},
    };
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_ADDFB2, &fb) == 0);
    set.fb_id = fb.fb_id;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_SETCRTC,
                      &set) == 0);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &(struct drm_mode_destroy_dumb){.handle = create.handle}) == 0);
    assert(bo->references == 2);
    drm_file_close(&master);
    assert(scanout_release_calls == 3);
    assert(mm_pages_freed == mm_pages_allocated);
    assert(allocations == 0);
}

static void drm_test_assert_present_job_rect(
    const struct drm_kms_present_job *job,
    uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    assert(job);
    assert(job->x1 == x1 && job->y1 == y1 &&
           job->x2 == x2 && job->y2 == y2);
    struct SVGA3dCopyRect rect;
    __builtin_memcpy(&rect, &job->output[3], sizeof(rect));
    assert(rect.x == x1 && rect.y == y1 &&
           rect.w == x2 - x1 && rect.h == y2 - y1 &&
           rect.srcx == x1 && rect.srcy == y1);
}

static void drm_test_kms_pending_damage_union(
    struct task *task, struct task_file *file, uint32_t fb_id,
    struct drm_clip_rect first, struct drm_clip_rect second,
    uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb_id, .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&first,
    };
    const uint32_t submitted_before = drm_test_present_submitted;
    const uint32_t submits_before = drm_test_batch_submit_calls;
    drm_test_batch_busy_submits = 2;

    assert(ioctl_call(task, file, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0);
    struct drm_kms_present_job *job = drm_kms_pending_job;
    assert(job && !job->submitted && job->batch_open);
    assert(drm_test_batch_busy_submits == 1u);
    drm_test_assert_present_job_rect(job, first.x1, first.y1,
                                    first.x2, first.y2);

    dirty.clips_ptr = (uint64_t)(uintptr_t)&second;
    assert(ioctl_call(task, file, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0);
    assert(drm_kms_pending_job == job);
    assert(!job->submitted && job->batch_open);
    assert(drm_test_batch_busy_submits == 0u);
    assert(drm_test_batch_submit_calls == submits_before + 2u);
    assert(drm_test_present_submitted == submitted_before);
    drm_test_assert_present_job_rect(job, x1, y1, x2, y2);

    drm_vmwgfx_process_retirements(1);
    assert(drm_kms_pending_job == NULL);
    assert(job->submitted && !job->batch_open);
    assert(drm_test_batch_submit_calls == submits_before + 3u);
    assert(drm_test_present_submitted == submitted_before + 1u);
    assert(drm_test_published_present_rect.x == x1 &&
           drm_test_published_present_rect.y == y1 &&
           drm_test_published_present_rect.w == x2 - x1 &&
           drm_test_published_present_rect.h == y2 - y1 &&
           drm_test_published_present_rect.srcx == x1 &&
           drm_test_published_present_rect.srcy == y1);

    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_process_retirements(16);
    assert(drm_kms_present_jobs == NULL &&
           drm_kms_present_job_count == 0u &&
           drm_kms_pending_job == NULL &&
           !drm_kms_pending_damage.valid);
}

static void test_kms_surface_present_pressure(void)
{
    const uint32_t drm_format_argb8888 = 0x34325241u;
    const uint32_t render_target_flags =
        SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE |
        DRM_TEST_SURFACE_BIND_RENDER_TARGET;
    struct task owner_task = make_task(610, false);
    struct task_file owner = {0};
    struct drm_vmw_size size = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT, .depth = 1,
    };
    union drm_vmw_surface_create_arg surface = {0};
    surface.req.flags = render_target_flags;
    surface.req.format = SVGA3D_A8R8G8B8;
    surface.req.mip_levels[0] = 1;
    surface.req.size_addr = (uint64_t)(uintptr_t)&size;
    surface.req.shareable = 1;
    surface.req.scanout = 1;
    struct drm_mode_fb_cmd2 fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_argb8888,
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    struct drm_mode_modeinfo mode;
    uint32_t connector = 2;
    scanout_begin_calls = scanout_release_calls = scanout_present_calls = 0;
    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &surface) == 0);
    /* Xorg 21.1's legacy depth-24/bpp-32 AddFB describes this ARGB-backed
     * GBM surface as XRGB8888. Alpha is ignored by the requested KMS format. */
    struct drm_mode_fb_cmd legacy_xrgb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pitch = DRM_TEST_WIDTH * 4u, .bpp = 32, .depth = 24,
        .handle = (uint32_t)surface.rep.sid,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB,
                      &legacy_xrgb) == 0);
    assert(legacy_xrgb.fb_id);
    uint32_t legacy_xrgb_fb_id = legacy_xrgb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB,
                      &legacy_xrgb_fb_id) == 0);
    fb.handles[0] = (uint32_t)surface.rep.sid;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2, &fb) == 0);
    assert(fb.fb_id);
    assert(drm_kms_mode_current(&mode));
    mode.type = 0;
    mode.vrefresh = 0;
    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uint64_t)(uintptr_t)&connector,
        .count_connectors = 1,
        .crtc_id = 1,
        .fb_id = fb.fb_id,
        .mode_valid = 1,
        .mode = mode,
    };
    uint32_t submitted_before = drm_test_present_submitted;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_SETCRTC, &set) == 0);
    assert(drm_test_present_submitted == submitted_before + 1u);
    struct drm_vmwgfx_surface *present_surface =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)surface.rep.sid);
    assert(present_surface);
    assert(drm_test_present_sid ==
           drm_vmwgfx_surface_backend_id(present_surface));
    drm_vmwgfx_surface_put(present_surface);
    assert(drm_test_present_rect.x == 0 && drm_test_present_rect.y == 0 &&
           drm_test_present_rect.w == DRM_TEST_WIDTH &&
           drm_test_present_rect.h == DRM_TEST_HEIGHT &&
           !drm_test_present_rect.srcx && !drm_test_present_rect.srcy);
    assert(scanout_present_calls == 0);

    drm_test_kms_pending_damage_union(&owner_task, &owner, fb.fb_id,
        (struct drm_clip_rect){5, 6, 7, 8},
        (struct drm_clip_rect){20, 21, 24, 25},
        5, 6, 24, 25);
    drm_test_kms_pending_damage_union(&owner_task, &owner, fb.fb_id,
        (struct drm_clip_rect){30, 20, 40, 30},
        (struct drm_clip_rect){35, 25, 45, 35},
        30, 20, 45, 35);
    drm_test_kms_pending_damage_union(&owner_task, &owner, fb.fb_id,
        (struct drm_clip_rect){20, 10, 40, 30},
        (struct drm_clip_rect){25, 15, 30, 20},
        20, 10, 40, 30);
    /* Keep the original full-frame PRESENT in the baseline used by the
     * pressure checks below; the three union cases above each publish once. */
    submitted_before = drm_test_present_submitted - 1u;

    struct drm_clip_rect clip = {5, 6, 7, 8};
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb.fb_id, .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&clip,
    };
    drm_test_batch_busy_submits = 1;
    unsigned submits_before_busy = drm_test_batch_submit_calls;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_DIRTYFB,
                      &dirty) == 0);
    assert(drm_test_batch_submit_calls == submits_before_busy + 1u);
    assert(drm_test_present_submitted == submitted_before + 1u);
    /* The modesetting client empties Damage after DIRTYFB returns. The idle
     * service must therefore publish the retained last rectangle without a
     * second userspace request, even though the resource retirement queue is
     * empty. */
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_present_submitted == submitted_before + 2u);
    assert(drm_test_present_rect.x == 5 && drm_test_present_rect.y == 6 &&
           drm_test_present_rect.w == 2 && drm_test_present_rect.h == 2 &&
           drm_test_present_rect.srcx == 5 && drm_test_present_rect.srcy == 6);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_present_submitted == submitted_before + 2u);

    /* A new busy rectangle remains bounded, but an owner/VT-style transition
     * cancels it. Already committed PRESENT fences still pin scanout until
     * they retire; retry service after the transition must not publish the
     * stale damage. */
    clip = (struct drm_clip_rect){20, 21, 24, 25};
    drm_test_batch_busy_submits = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_DIRTYFB,
                      &dirty) == 0);
    assert(drm_test_present_submitted == submitted_before + 2u);
    unsigned presents_before_drop = drm_test_present_submitted;
    unsigned releases_before_drop = scanout_release_calls;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_DROP_MASTER, NULL) == 0);
    assert(scanout_release_calls == releases_before_drop);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_present_submitted == presents_before_drop &&
           scanout_release_calls == releases_before_drop);

    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_process_retirements(2);
    assert(scanout_release_calls == releases_before_drop + 1u);
    drm_vmwgfx_process_retirements(2);
    assert(drm_test_present_submitted == presents_before_drop &&
           scanout_release_calls == releases_before_drop + 1u);
    drm_file_close(&owner);
    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_process_retirements(2);
    assert(allocations == 0);
}

static void test_kms_surface_format_compatibility(void)
{
    const uint32_t drm_format_xrgb8888 = DRM_FORMAT_XRGB8888_TEST;
    const uint32_t drm_format_argb8888 = 0x34325241u;
    const uint32_t render_target_flags =
        SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE |
        DRM_TEST_SURFACE_BIND_RENDER_TARGET;
    const unsigned allocations_before = allocations;
    struct task owner_task = make_task(620, false);
    struct task importer_task = make_task(621, false);
    struct task foreign_task = make_task(622, false);
    struct task_file owner = {0}, importer = {0}, foreign = {0};
    struct drm_vmw_size size = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT, .depth = 1,
    };
    union drm_vmw_surface_create_arg argb_surface = {0};
    argb_surface.req.flags = render_target_flags;
    argb_surface.req.format = SVGA3D_A8R8G8B8;
    argb_surface.req.mip_levels[0] = 1;
    argb_surface.req.size_addr = (uint64_t)(uintptr_t)&size;
    argb_surface.req.shareable = 1;
    argb_surface.req.scanout = 1;

    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&importer_task, &importer) == 0);
    assert(drm_file_open(&foreign_task, &foreign) == 0);
    assert(owner.drm_file != importer.drm_file &&
           owner.drm_file != foreign.drm_file &&
           importer.drm_file != foreign.drm_file);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &argb_surface) == 0);

    /* A numeric surface ID is not a handle in a different DRM open file. */
    struct drm_mode_fb_cmd2 foreign_fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {(uint32_t)argb_surface.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_MODE_ADDFB2,
                      &foreign_fb) == -LINUX_EINVAL);
    assert(!foreign_fb.fb_id && !foreign.drm_file->kms_fbs);

    /* A8R8G8B8 and X8R8G8B8 have the same RGB byte layout. XRGB ignores the
     * fourth byte, so the real Xorg legacy AddFB request is compatible. */
    struct drm_mode_fb_cmd2 xrgb_fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {(uint32_t)argb_surface.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == 0);
    assert(xrgb_fb.fb_id);
    uint32_t fb_id = xrgb_fb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);

    struct drm_mode_fb_cmd legacy_xrgb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pitch = DRM_TEST_WIDTH * 4u, .bpp = 32, .depth = 24,
        .handle = (uint32_t)argb_surface.rep.sid,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB,
                      &legacy_xrgb) == 0);
    assert(legacy_xrgb.fb_id);
    fb_id = legacy_xrgb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);

    /* Keep ARGB's alpha requirement and the common geometry/layout checks. */
    struct drm_mode_fb_cmd2 argb_fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_argb8888,
        .handles = {(uint32_t)argb_surface.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &argb_fb) == 0);
    fb_id = argb_fb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);
    xrgb_fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {(uint32_t)argb_surface.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
        .offsets = {4},
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);
    xrgb_fb.offsets[0] = 0;
    xrgb_fb.pitches[0] = DRM_TEST_WIDTH * 4u - 1u;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);
    xrgb_fb.pitches[0] = DRM_TEST_WIDTH * 4u;
    xrgb_fb.modifier[0] = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);

    /* PRIME FD_TO_HANDLE creates the required reference in a separate DRM
     * file. The same XRGB AddFB succeeds only after that actual import. */
    struct drm_prime_handle exported = {
        .handle = (uint32_t)argb_surface.rep.sid,
        .flags = LINUX_O_CLOEXEC | LINUX_O_RDWR,
        .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &exported) == 0);
    struct task_file *received_slot = NULL;
    int received_fd = task_allocate_fd(&importer_task, 0, &received_slot);
    assert(received_fd >= 0 && received_slot &&
           task_file_reference(received_slot,
               task_file_for_fd(&owner_task, exported.fd)) == 0);
    struct drm_prime_handle imported = {.fd = received_fd};
    assert(ioctl_call(&importer_task, &importer, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &imported) == 0 &&
           imported.handle == (uint32_t)argb_surface.rep.sid);
    struct drm_mode_fb_cmd2 imported_fb = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {imported.handle},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&importer_task, &importer, DRM_IOCTL_MODE_ADDFB2,
                      &imported_fb) == 0);
    fb_id = imported_fb.fb_id;
    assert(ioctl_call(&importer_task, &importer, DRM_IOCTL_MODE_RMFB,
                      &fb_id) == 0);
    struct drm_vmw_surface_arg imported_unref = {
        .sid = (int32_t)imported.handle,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&importer_task, &importer, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &imported_unref) == 0);

    /* X8 backing remains valid for XRGB; its fourth byte cannot satisfy an
     * ARGB request. Alpha-only storage is a different, incompatible layout. */
    union drm_vmw_surface_create_arg x8_surface = {0};
    x8_surface.req.flags = render_target_flags;
    x8_surface.req.format = SVGA3D_X8R8G8B8;
    x8_surface.req.mip_levels[0] = 1;
    x8_surface.req.size_addr = (uint64_t)(uintptr_t)&size;
    x8_surface.req.shareable = 1;
    x8_surface.req.scanout = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &x8_surface) == 0);
    xrgb_fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {(uint32_t)x8_surface.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == 0);
    fb_id = xrgb_fb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);
    legacy_xrgb.handle = (uint32_t)x8_surface.rep.sid;
    legacy_xrgb.fb_id = 0;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB,
                      &legacy_xrgb) == 0);
    fb_id = legacy_xrgb.fb_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);
    argb_fb.handles[0] = (uint32_t)x8_surface.rep.sid;
    argb_fb.fb_id = 0;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &argb_fb) == -LINUX_EINVAL && !argb_fb.fb_id);

    struct drm_vmw_size small_size = {.width = 32, .height = 24, .depth = 1};
    union drm_vmw_surface_create_arg non_scanout = {0};
    non_scanout.req.flags = render_target_flags;
    non_scanout.req.format = SVGA3D_A8R8G8B8;
    non_scanout.req.mip_levels[0] = 1;
    non_scanout.req.size_addr = (uint64_t)(uintptr_t)&size;
    non_scanout.req.shareable = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &non_scanout) == 0);
    xrgb_fb = (struct drm_mode_fb_cmd2){
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT,
        .pixel_format = drm_format_xrgb8888,
        .handles = {(uint32_t)non_scanout.rep.sid},
        .pitches = {DRM_TEST_WIDTH * 4u},
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);

    union drm_vmw_surface_create_arg nonshareable = {0};
    nonshareable.req.flags = render_target_flags;
    nonshareable.req.format = SVGA3D_A8R8G8B8;
    nonshareable.req.mip_levels[0] = 1;
    nonshareable.req.size_addr = (uint64_t)(uintptr_t)&size;
    nonshareable.req.scanout = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &nonshareable) == 0);
    xrgb_fb.handles[0] = (uint32_t)nonshareable.rep.sid;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);

    union drm_vmw_surface_create_arg too_small = {0};
    too_small.req.flags = render_target_flags;
    too_small.req.format = SVGA3D_A8R8G8B8;
    too_small.req.mip_levels[0] = 1;
    too_small.req.size_addr = (uint64_t)(uintptr_t)&small_size;
    too_small.req.shareable = 1;
    too_small.req.scanout = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &too_small) == 0);
    xrgb_fb.handles[0] = (uint32_t)too_small.rep.sid;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);

    struct drm_vmw_size alpha_size = {
        .width = DRM_TEST_WIDTH, .height = DRM_TEST_HEIGHT, .depth = 1,
    };
    union drm_vmw_surface_create_arg alpha_surface = {0};
    alpha_surface.req.flags = SVGA3D_SURFACE_HINT_TEXTURE |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE;
    alpha_surface.req.format = SVGA3D_ALPHA8;
    alpha_surface.req.mip_levels[0] = 1;
    alpha_surface.req.size_addr = (uint64_t)(uintptr_t)&alpha_size;
    alpha_surface.req.shareable = 1;
    alpha_surface.req.scanout = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &alpha_surface) == 0);
    xrgb_fb.handles[0] = (uint32_t)alpha_surface.rep.sid;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_ADDFB2,
                      &xrgb_fb) == -LINUX_EINVAL && !xrgb_fb.fb_id);

    const int32_t surface_ids[] = {
        argb_surface.rep.sid, x8_surface.rep.sid, non_scanout.rep.sid,
        nonshareable.rep.sid, too_small.rep.sid, alpha_surface.rep.sid,
    };
    for (unsigned i = 0; i < sizeof(surface_ids) / sizeof(surface_ids[0]); ++i) {
        struct drm_vmw_surface_arg unref = {
            .sid = surface_ids[i], .handle_type = DRM_VMW_HANDLE_LEGACY,
        };
        assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                          &unref) == 0);
    }
    task_discard_file_fd(&importer_task, received_fd);
    task_discard_file_fd(&owner_task, exported.fd);
    drm_file_close(&foreign);
    drm_file_close(&importer);
    drm_file_close(&owner);
    /* Surface retirement is deliberately bounded to two backend destroys per
     * wake, so drain this fixture's six unreferenced surfaces over several. */
    for (unsigned attempt = 0; attempt < 8 && allocations != allocations_before;
         ++attempt)
        drm_vmwgfx_process_retirements(16);
    assert(allocations == allocations_before);
}

static void test_prime_surface_fd_lifetime(void)
{
    const uint32_t render_target_flags =
        SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE |
        DRM_TEST_SURFACE_BIND_RENDER_TARGET;
    struct task owner_task = make_task(960, false);
    struct task client_task = make_task(961, false);
    struct task importer_task = make_task(962, false);
    struct task_file owner = {0}, client = {0}, importer = {0};
    struct drm_vmw_size size = {.width = 64, .height = 64, .depth = 1};
    union drm_vmw_surface_create_arg surface = {0};
    surface.req.flags = render_target_flags;
    surface.req.format = SVGA3D_A8R8G8B8;
    surface.req.mip_levels[0] = 1;
    surface.req.size_addr = (uint64_t)(uintptr_t)&size;
    surface.req.shareable = 1;
    surface.req.scanout = 1;

    owner_task.fsuid = 1001;
    owner_task.fsgid = 2002;
    client_task.fsuid = 3003;
    client_task.fsgid = 4004;
    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    assert(!client.drm_file->authenticated && !client.drm_file->is_master);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &surface) == 0);

    /* A fault while copying the exported descriptor back to userspace must
     * close the unpublished OFD and detach it from the surface cache. */
    struct drm_vmwgfx_surface *surface_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)surface.rep.sid);
    assert(surface_record &&
           !drm_vmwgfx_surface_prime_description(surface_record));
    drm_vmwgfx_surface_put(surface_record);
    struct drm_prime_handle failed_export = {
        .handle = (uint32_t)surface.rep.sid,
        .flags = LINUX_O_RDWR | LINUX_O_CLOEXEC,
        .fd = -1,
    };
    unsigned allocations_before_export_fault = allocations;
    fail_copyout_address = (uint64_t)(uintptr_t)&failed_export;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &failed_export) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(failed_export.fd == -1 &&
           allocations == allocations_before_export_fault);
    surface_record = drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                                   (uint32_t)surface.rep.sid);
    assert(surface_record &&
           !drm_vmwgfx_surface_prime_description(surface_record));
    drm_vmwgfx_surface_put(surface_record);

    int occupied_fds[DRM_TEST_FD_SLOTS];
    for (unsigned i = 0; i < DRM_TEST_FD_SLOTS; ++i) {
        struct task_file *slot = NULL;
        occupied_fds[i] = task_allocate_fd(&owner_task, 0, &slot);
        assert(occupied_fds[i] >= 0 && slot);
    }
    struct drm_prime_handle exhausted_export = {
        .handle = (uint32_t)surface.rep.sid,
        .flags = LINUX_O_RDWR | LINUX_O_CLOEXEC,
        .fd = -1,
    };
    unsigned allocations_before_fd_exhaustion = allocations;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &exhausted_export) == -LINUX_EMFILE);
    assert(exhausted_export.fd == -1 &&
           allocations == allocations_before_fd_exhaustion);
    surface_record = drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                                   (uint32_t)surface.rep.sid);
    assert(surface_record &&
           !drm_vmwgfx_surface_prime_description(surface_record));
    drm_vmwgfx_surface_put(surface_record);
    for (unsigned i = 0; i < DRM_TEST_FD_SLOTS; ++i)
        task_discard_file_fd(&owner_task, occupied_fds[i]);

    struct drm_mode_create_dumb non_surface = {
        .width = 8, .height = 8, .bpp = 32,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &non_surface) == 0);
    struct drm_prime_handle wrong_object = {
        .handle = non_surface.handle, .flags = LINUX_O_CLOEXEC, .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &wrong_object) == -LINUX_EINVAL);
    assert(wrong_object.fd == -1);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &(struct drm_mode_destroy_dumb){
                          .handle = non_surface.handle,
                      }) == 0);

    /* The first published export now owns the surface-cache entry. */
    struct drm_prime_handle first = {
        .handle = (uint32_t)surface.rep.sid,
        .flags = LINUX_O_RDWR | LINUX_O_CLOEXEC,
        .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &first) == 0);
    struct task_file *description = task_file_for_fd(&owner_task, first.fd);
    assert(description && description->drm_prime_file &&
           description->kind == TASK_FILE_KIND_DRM_PRIME &&
           (description->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDWR);
    assert(description->drm_prime_ino &&
           description->drm_prime_size == 64u * 64u * 4u &&
           description->drm_prime_uid == owner_task.fsuid &&
           description->drm_prime_gid == owner_task.fsgid);
    assert(drm_test_fd_slots[drm_test_fd_group(&owner_task)][first.fd].fd_flags ==
           RELIEFOS_FD_CLOEXEC);

    /* The live TTM description is cached per surface. Later exports inherit
     * its mode and first-export anonymous-inode owner, while CLOEXEC belongs
     * only to the newly allocated descriptor. */
    owner_task.fsuid = 5005;
    owner_task.fsgid = 6006;
    struct drm_prime_handle second = {
        .handle = (uint32_t)surface.rep.sid, .flags = 0, .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &second) == 0);
    assert(second.fd != first.fd &&
           task_file_for_fd(&owner_task, second.fd) == description &&
           description->references == 2 &&
           (description->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDWR &&
           description->drm_prime_uid == 1001 &&
           description->drm_prime_gid == 2002);
    assert(drm_test_fd_slots[drm_test_fd_group(&owner_task)][second.fd].fd_flags ==
           0);
    assert(drm_prime_file_poll(description, POLLIN | POLLOUT) ==
           (POLLIN | POLLOUT));
    assert(drm_prime_file_llseek(description, 0, RELIEFOS_SEEK_SET) == 0);
    assert(drm_prime_file_llseek(description, 0, RELIEFOS_SEEK_END) ==
           (int64_t)description->drm_prime_size);
    assert(drm_prime_file_llseek(description, 1, RELIEFOS_SEEK_SET) ==
           -LINUX_EINVAL);
    assert(drm_prime_file_rw(description, false) == -LINUX_EINVAL);
    assert(drm_prime_file_rw(description, true) == -LINUX_EINVAL);
    assert(drm_prime_file_mmap(description, LINUX_MAP_SHARED,
                               LINUX_PROT_WRITE) == -LINUX_EINVAL);
    struct drm_test_dma_buf_sync { uint64_t flags; } sync;
    const uint64_t sync_flags[] = {1u, 1u | 4u, 2u, 2u | 4u};
    for (unsigned i = 0; i < sizeof(sync_flags) / sizeof(sync_flags[0]); ++i) {
        sync.flags = sync_flags[i];
        assert(drm_prime_file_ioctl(&owner_task, description,
                                    0x40086200u,
                                    (uint64_t)(uintptr_t)&sync) == 0);
        /* Exercise the shared ioctl dispatcher used by SYS_ioctl. In the
         * native fd oracle the corresponding 64-bit call previously reached
         * the generic fallback and returned ENOTTY. */
        assert(drm_fd_ioctl(&owner_task, description, 0x40086200u,
                            (uint64_t)(uintptr_t)&sync) == 0);
    }
    sync.flags = 0;
    assert(drm_prime_file_ioctl(&owner_task, description, 0x40086200u,
                                (uint64_t)(uintptr_t)&sync) == -LINUX_EINVAL);
    assert(drm_fd_ioctl(&owner_task, description, 0x40086200u,
                        (uint64_t)(uintptr_t)&sync) == -LINUX_EINVAL);
    sync.flags = 1u | 8u;
    assert(drm_prime_file_ioctl(&owner_task, description, 0x40086200u,
                                (uint64_t)(uintptr_t)&sync) == -LINUX_EINVAL);
    assert(drm_fd_ioctl(&owner_task, description, 0x40086200u,
                        (uint64_t)(uintptr_t)&sync) == -LINUX_EINVAL);
    assert(drm_fd_ioctl(&owner_task, description, 0x40086201u,
                        (uint64_t)(uintptr_t)&sync) == -LINUX_ENOTTY);

    /* Model SCM_RIGHTS: a receiver gets a descriptor to the same OFD without
     * inheriting the sender's descriptor-local CLOEXEC bit. */
    struct task_file *received_slot = NULL;
    int received_fd = task_allocate_fd(&client_task, 0, &received_slot);
    assert(received_fd >= 0 && task_file_reference(received_slot, description) == 0);
    assert(task_file_for_fd(&client_task, received_fd) == description &&
           drm_test_fd_slots[drm_test_fd_group(&client_task)][received_fd].fd_flags ==
               0);
    struct drm_prime_handle invalid_import = {.fd = -1};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &invalid_import) == -LINUX_EBADF);
    struct drm_prime_handle nonexistent_import = {
        .fd = DRM_TEST_FD_SLOTS + 32,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &nonexistent_import) == -LINUX_EBADF);
    struct drm_vmwgfx_surface *invalid_surface = NULL;
    assert(drm_prime_surface_from_fd(&client_task, -1, drm_device_get(),
                                     &invalid_surface) == -LINUX_EBADF &&
           !invalid_surface);
    assert(drm_prime_surface_from_fd(&client_task, DRM_TEST_FD_SLOTS + 32,
                                     drm_device_get(), &invalid_surface) ==
               -LINUX_EBADF && !invalid_surface);
    struct task_file *wrong_object_slot = NULL;
    int wrong_object_fd = task_allocate_fd(&client_task, 0,
                                           &wrong_object_slot);
    assert(wrong_object_fd >= 0 && wrong_object_slot);
    assert(drm_prime_surface_from_fd(&client_task, wrong_object_fd,
                                     drm_device_get(), &invalid_surface) ==
               -LINUX_EINVAL && !invalid_surface);
    struct drm_prime_handle wrong_object_import = {.fd = wrong_object_fd};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &wrong_object_import) == -LINUX_EINVAL);
    union drm_vmw_surface_reference_arg wrong_object_reference = {0};
    wrong_object_reference.req.sid = wrong_object_fd;
    wrong_object_reference.req.handle_type = DRM_VMW_HANDLE_PRIME;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &wrong_object_reference) == -LINUX_EINVAL);
    task_discard_file_fd(&client_task, wrong_object_fd);
    union drm_vmw_surface_reference_arg invalid_prime_reference = {0};
    invalid_prime_reference.req.sid = -1;
    invalid_prime_reference.req.handle_type = DRM_VMW_HANDLE_PRIME;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &invalid_prime_reference) == -LINUX_EBADF);
    struct drm_prime_handle failed_import = {
        .fd = received_fd, .handle = UINT32_C(0xdeadbeef),
    };
    fail_copyout_address = (uint64_t)(uintptr_t)&failed_import;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &failed_import) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(failed_import.handle == UINT32_C(0xdeadbeef));
    struct drm_vmwgfx_surface *failed_surface =
        drm_vmwgfx_surface_lookup_get(client.drm_file,
                                      (uint32_t)surface.rep.sid);
    assert(!failed_surface);

    struct drm_prime_handle imported = {.fd = received_fd};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_PRIME_FD_TO_HANDLE,
                      &imported) == 0);
    assert(imported.handle == (uint32_t)surface.rep.sid);
    struct drm_vmwgfx_surface *client_surface =
        drm_vmwgfx_surface_lookup_get(client.drm_file, imported.handle);
    assert(client_surface);
    drm_vmwgfx_surface_put(client_surface);
    struct drm_device foreign_device = *drm_device_get();
    struct drm_vmwgfx_surface *foreign_surface = NULL;
    assert(drm_prime_surface_from_fd(&client_task, received_fd,
                                     &foreign_device, &foreign_surface) ==
               -LINUX_EINVAL && !foreign_surface);

    union drm_vmw_surface_reference_arg prime_reference = {0};
    prime_reference.req.sid = received_fd;
    prime_reference.req.handle_type = DRM_VMW_HANDLE_PRIME;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &prime_reference) == 0);
    assert(prime_reference.rep.format == SVGA3D_A8R8G8B8 &&
           prime_reference.rep.shareable == 1);

    surface_record = drm_vmwgfx_surface_lookup_get(
        owner.drm_file, (uint32_t)surface.rep.sid);
    assert(surface_record &&
           drm_vmwgfx_surface_prime_description(surface_record) == description);
    drm_vmwgfx_surface_put(surface_record);

    /* Dropping the creator/master cannot invalidate an imported object or
     * the received PRIME OFD. A later import remains a deliberate typed ref. */
    struct drm_vmw_surface_arg owner_unref = {
        .sid = surface.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &owner_unref) == 0);
    drm_file_close(&owner);
    assert(!drm_device_get()->master);
    task_discard_file_fd(&owner_task, first.fd);
    task_discard_file_fd(&owner_task, second.fd);
    assert(description->references == 1);

    assert(drm_file_open(&importer_task, &importer) == 0);
    struct task_file *importer_received_slot = NULL;
    int importer_received_fd = task_allocate_fd(
        &importer_task, 0, &importer_received_slot);
    assert(importer_received_fd >= 0 &&
           task_file_reference(importer_received_slot,
                               task_file_for_fd(&client_task,
                                                received_fd)) == 0);
    struct drm_prime_handle later_import = {.fd = importer_received_fd};
    assert(ioctl_call(&importer_task, &importer,
                      DRM_IOCTL_PRIME_FD_TO_HANDLE, &later_import) == 0 &&
           later_import.handle == (uint32_t)surface.rep.sid);
    task_discard_file_fd(&client_task, received_fd);
    task_discard_file_fd(&importer_task, importer_received_fd);
    surface_record = drm_vmwgfx_surface_lookup_get(importer.drm_file,
                                                  later_import.handle);
    assert(surface_record &&
           !drm_vmwgfx_surface_prime_description(surface_record));
    drm_vmwgfx_surface_put(surface_record);
    drm_file_close(&client);
    surface_record = drm_vmwgfx_surface_lookup_get(importer.drm_file,
                                                  later_import.handle);
    assert(surface_record);
    drm_vmwgfx_surface_put(surface_record);
    struct drm_vmw_surface_arg importer_unref = {
        .sid = (int32_t)later_import.handle,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&importer_task, &importer, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &importer_unref) == 0);
    assert(!drm_vmwgfx_surface_lookup_get(importer.drm_file,
                                          later_import.handle));
    drm_file_close(&importer);
    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_process_retirements(2);
    assert(allocations == 0);

    /* A live cached dma-buf OFD fixes its access mode and anonymous-inode
     * owner at the first export. Later descriptors may request write access,
     * but cannot upgrade that OFD; shared writable mmap checks access before
     * the unsupported mapping operation. */
    struct drm_vmw_size readonly_size = {.width = 8, .height = 8, .depth = 1};
    union drm_vmw_surface_create_arg readonly_surface = {0};
    readonly_surface.req.flags = SVGA3D_SURFACE_HINT_TEXTURE;
    readonly_surface.req.format = SVGA3D_A8R8G8B8;
    readonly_surface.req.mip_levels[0] = 1;
    readonly_surface.req.size_addr = (uint64_t)(uintptr_t)&readonly_size;
    readonly_surface.req.shareable = 1;
    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &readonly_surface) == 0);
    struct drm_prime_handle readonly_first = {
        .handle = (uint32_t)readonly_surface.rep.sid,
        .flags = LINUX_O_CLOEXEC,
        .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &readonly_first) == 0);
    struct task_file *readonly_description =
        task_file_for_fd(&owner_task, readonly_first.fd);
    assert(readonly_description &&
           (readonly_description->flags & RELIEFOS_O_ACCMODE) ==
               RELIEFOS_O_RDONLY &&
           drm_test_fd_slots[drm_test_fd_group(&owner_task)]
               [readonly_first.fd].fd_flags == RELIEFOS_FD_CLOEXEC);
    uint64_t readonly_ino = readonly_description->drm_prime_ino;
    uint32_t readonly_uid = readonly_description->drm_prime_uid;
    uint32_t readonly_gid = readonly_description->drm_prime_gid;
    owner_task.fsuid = 7007;
    owner_task.fsgid = 8008;
    struct drm_prime_handle readonly_second = {
        .handle = (uint32_t)readonly_surface.rep.sid,
        .flags = LINUX_O_RDWR,
        .fd = -1,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                      &readonly_second) == 0);
    assert(task_file_for_fd(&owner_task, readonly_second.fd) ==
               readonly_description && readonly_description->references == 2 &&
           (readonly_description->flags & RELIEFOS_O_ACCMODE) ==
               RELIEFOS_O_RDONLY &&
           readonly_description->drm_prime_ino == readonly_ino &&
           readonly_description->drm_prime_uid == readonly_uid &&
           readonly_description->drm_prime_gid == readonly_gid &&
           drm_test_fd_slots[drm_test_fd_group(&owner_task)]
               [readonly_second.fd].fd_flags == 0);
    assert(drm_prime_file_rw(readonly_description, false) == -LINUX_EINVAL);
    assert(drm_prime_file_rw(readonly_description, true) == -LINUX_EBADF);
    assert(drm_prime_file_mmap(readonly_description, LINUX_MAP_SHARED,
                               LINUX_PROT_WRITE) == -LINUX_EACCES);
    assert(drm_prime_file_mmap(readonly_description, LINUX_MAP_SHARED,
                               LINUX_PROT_READ) == -LINUX_EINVAL);
    struct drm_vmw_surface_arg readonly_unref = {
        .sid = readonly_surface.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &readonly_unref) == 0);
    task_discard_file_fd(&owner_task, readonly_first.fd);
    assert(readonly_description->references == 1);
    drm_file_close(&owner);
    task_discard_file_fd(&owner_task, readonly_second.fd);
    drm_vmwgfx_process_retirements(2);
    assert(allocations == 0);
}

static uint32_t drm_test_find_capability(const uint32_t *record,
                                        uint32_t cap_index)
{
    if (!record || record[1] != 0x100u) return UINT32_MAX;
    for (uint32_t i = 2; i < record[0]; i += 2) {
        if (record[i] == cap_index) return record[i + 1u];
    }
    return UINT32_MAX;
}

static void test_vmwgfx_resource_contract(void)
{
    unsigned reaper_calls = drm_test_shader_reaper_calls;
    unsigned backings_reaped = drm_test_shader_backings_reaped;
    drm_vmwgfx_process_retirements(9);
    assert(drm_test_shader_reaper_calls == reaper_calls + 1u);
    assert(drm_test_shader_reaper_budget == 2u);
    assert(drm_test_shader_backings_reaped == backings_reaped + 2u);
    struct task owner_task = make_task(900, true);
    struct task client_task = make_task(901, false);
    struct task foreign_task = make_task(902, false);
    struct task_file owner = {0}, client = {0}, foreign = {0};
    struct drm_vmw_getparam_arg param = {
        .param = DRM_VMW_PARAM_3D,
        .pad64 = 0xa5a5a5a5u,
    };
    uint32_t caps[600] = {0};

    memset(drm_test_svga_cap_valid, 1, sizeof(drm_test_svga_cap_valid));
    for (uint32_t i = 0; i < DRM_TEST_COMPAT_DEVCAP_MAX; ++i)
        drm_test_svga_caps[i] = 1u;
    drm_test_svga_caps[SVGA3D_DEVCAP_3D] = 1u;
    /* Pinned Mesa SVGA protocol shader-version enums use nonordinal values:
     * VS 3.0 is 7 and PS 3.0 is 13. The backend implements through 3.0. */
    drm_test_svga_caps[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION] = 9u;
    drm_test_svga_caps[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION] = 15u;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_RENDER_TARGETS] = 8u;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_POINT_SIZE] = 0x43000000u;
    drm_test_svga_caps[89] = 0x41a00000u; /* 20.0f */
    drm_test_svga_caps[90] = 0xbf800000u; /* negative 1.0f */
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_TEXTURES] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_VOLUME_EXTENT] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_CONTEXT_IDS] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_MAX_SURFACE_IDS] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET | SVGA3DFORMAT_OP_SRGBREAD |
        SVGA3DFORMAT_OP_SRGBWRITE;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
        SVGA3DFORMAT_OP_VOLUMETEXTURE |
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
        SVGA3DFORMAT_OP_SRGBREAD | SVGA3DFORMAT_OP_NOFILTER |
        SVGA3DFORMAT_OP_NOALPHABLEND |
        SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_Z_D16] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] = UINT32_MAX;
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5] = UINT32_MAX;

    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&client_task, &client) == 0);

    struct drm_mode_create_dumb namespace_bo = {
        .width = 8, .height = 8, .bpp = 32,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &namespace_bo) == 0);
    assert(namespace_bo.handle > 0 &&
           namespace_bo.handle < DRM_TEST_RESOURCE_HANDLE_MIN);
    struct drm_mode_destroy_dumb namespace_bo_destroy = {
        .handle = namespace_bo.handle,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &namespace_bo_destroy) == 0);

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0);
    assert(param.value == 1);
    uint32_t saved_vertex_version =
        drm_test_svga_caps[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION];
    drm_test_svga_caps[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION] = 5u;
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_3D};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == 0);
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_3D_CAPS_SIZE,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == 0);
    drm_test_svga_caps[SVGA3D_DEVCAP_VERTEX_SHADER_VERSION] =
        saved_vertex_version;
    uint32_t saved_fragment_version =
        drm_test_svga_caps[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION];
    drm_test_svga_caps[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION] = 11u;
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_3D};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == 0);
    drm_test_svga_caps[SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION] =
        saved_fragment_version;
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_3D};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == 1);
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_FIFO_CAPS,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == drm_test_svga_info.fifo_caps);
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_FIFO_HW_VERSION,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value == drm_test_svga_info.host_version);
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_MAX_SURF_MEMORY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0);
    assert(param.value == 64u * 1024u * 1024u);

    /* The GB host still exposes Linux's non-GB file profile. Its advertised
     * surface memory is half the backend-supported MOB/page budget, bounded
     * by the allocator's actual surface ceiling. */
    drm_test_svga_info.surface_limit_bytes = 128u * 1024u * 1024u;
    drm_test_svga_info.compat_surface_limit_bytes = 64u * 1024u * 1024u;
    drm_test_svga_info.probe.gb_objects = true;
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_MAX_SURF_MEMORY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0);
    assert(param.value == 64u * 1024u * 1024u);

    /* A lower non-GB host memory budget remains the exact reported value. */
    drm_test_svga_info.surface_limit_bytes = 64u * 1024u * 1024u;
    drm_test_svga_info.compat_surface_limit_bytes = 64u * 1024u * 1024u;
    drm_test_svga_info.probe.gb_objects = false;
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_MAX_SURF_MEMORY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0);
    assert(param.value == 64u * 1024u * 1024u);
    param = (struct drm_vmw_getparam_arg){
        .param = DRM_VMW_PARAM_3D_CAPS_SIZE,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == 0 && param.value >= 12 && param.value <= sizeof(caps));
    param = (struct drm_vmw_getparam_arg){.param = DRM_VMW_PARAM_HW_CAPS};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == -LINUX_EINVAL);
    param = (struct drm_vmw_getparam_arg){.param = UINT32_MAX};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_PARAM,
                      &param) == -LINUX_EINVAL);

    struct drm_vmw_get_3d_cap_arg get_caps = {
        .buffer = (uint64_t)(uintptr_t)caps,
        .max_size = sizeof(caps),
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == 0);
    assert(caps[0] >= 2 && !(caps[0] & 1u) && caps[1] == 0x100u);
    assert(caps[caps[0]] == 0);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_RENDER_TARGETS) == 4u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_VERTEX_SHADER_VERSION) == 7u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION) == 13u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_VERTEX_SHADER) == 1u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_FRAGMENT_SHADER) == 1u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_POINT_SIZE) == 0x42800000u);
    assert(drm_test_find_capability(caps, 89u) == 0x41200000u);
    assert(drm_test_find_capability(caps, 90u) == 0u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_TEXTURES) == 32u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH) == 16384u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_VOLUME_EXTENT) == 256u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_CONTEXT_IDS) == 63u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_SURFACE_IDS) == 255u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_MAX_LIGHTS) == 0u);
    assert(drm_test_find_capability(caps, 262u - 1u) == 0u);
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8) ==
           (SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
            SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET | SVGA3DFORMAT_OP_SRGBREAD));
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8) ==
           (SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
            SVGA3DFORMAT_OP_VOLUMETEXTURE |
            SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
            SVGA3DFORMAT_OP_SRGBREAD | SVGA3DFORMAT_OP_NOFILTER |
            SVGA3DFORMAT_OP_NOALPHABLEND |
            SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP));
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_SURFACEFMT_ALPHA8) ==
           (SVGA3DFORMAT_OP_TEXTURE | DRM_TEST_FORMAT_NEGATIVE_MASK));
    assert(drm_test_find_capability(caps, SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5) == 0u);
    assert(drm_vmw_filter_float(0x7fc00000u, 0x42800000u) == 0u);
    assert(drm_vmw_filter_float(0xbf800000u, 0x42800000u) == 0u);
    uint32_t saved_alpha8_host_ops =
        drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8];
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
        SVGA3DFORMAT_OP_SRGBWRITE;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == 0);
    assert(drm_test_find_capability(caps,
        SVGA3D_DEVCAP_SURFACEFMT_ALPHA8) == 0u);
    drm_test_svga_caps[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        saved_alpha8_host_ops;
    uint32_t header[2] = {0};
    get_caps.buffer = (uint64_t)(uintptr_t)header;
    get_caps.max_size = sizeof(header);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == 0);
    assert(header[0] == 2u && header[1] == 0x100u);
    get_caps.max_size = sizeof(uint32_t);
    get_caps.buffer = 1;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == -LINUX_EINVAL);
    get_caps.max_size = sizeof(header);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == -LINUX_EFAULT);
    get_caps.buffer = (uint64_t)(uintptr_t)header;
    get_caps.max_size = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == -LINUX_EINVAL);
    get_caps.max_size = sizeof(header);
    get_caps.pad64 = 1;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == -LINUX_EINVAL);
    bool saved_svga_available = drm_test_svga_info.available;
    drm_test_svga_info.available = false;
    get_caps.pad64 = 0;
    get_caps.max_size = sizeof(uint32_t);
    get_caps.buffer = 1;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_GET_3D_CAP,
                      &get_caps) == -LINUX_ENOMEM);
    drm_test_svga_info.available = saved_svga_available;

    /* The context record takes a master reference before its per-file ref is
     * allocated. If that second allocation fails, release both cleanly and
     * never call the backend or publish a resource ID. */
    unsigned context_allocations_before_failure = allocations;
    unsigned context_creates_before_failure = drm_test_context_create_calls;
    uint32_t master_refs_before_failure = owner.drm_file->master->references;
    struct drm_vmw_context_arg allocation_failed_context = {0};
    fail_kernel_malloc_after = 1;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &allocation_failed_context) == -LINUX_ENOMEM);
    fail_kernel_malloc_after = -1;
    assert(allocations == context_allocations_before_failure);
    assert(owner.drm_file->master->references == master_refs_before_failure);
    assert(drm_test_context_create_calls == context_creates_before_failure);
    assert(allocation_failed_context.cid == 0);

    unsigned context_creates_before_partial = drm_test_context_create_calls;
    unsigned context_destroys_before_partial = drm_test_context_destroy_calls;
    master_refs_before_failure = owner.drm_file->master->references;
    struct drm_vmw_context_arg partial_context = {0};
    drm_test_context_create_status = SVGA_EBUSY;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &partial_context) == -LINUX_EBUSY);
    drm_test_context_create_status = 0;
    assert(partial_context.cid == 0);
    assert(drm_test_context_create_calls == context_creates_before_partial + 1u);
    assert(drm_test_context_destroy_calls == context_destroys_before_partial);
    assert(owner.drm_file->master->references == master_refs_before_failure + 1u);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_context_destroy_calls == context_destroys_before_partial + 1u);
    assert(owner.drm_file->master->references == master_refs_before_failure);

    struct drm_vmw_context_arg context = {0};
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &context) == 0);
    assert(context.cid >= (int32_t)DRM_TEST_RESOURCE_HANDLE_MIN);
    struct drm_vmwgfx_context *typed_context =
        drm_vmwgfx_context_lookup_get(owner.drm_file, (uint32_t)context.cid);
    assert(typed_context);
    svga_handle context_backend =
        drm_vmwgfx_context_backend_handle(typed_context);
    assert(context_backend != SVGA_INVALID_HANDLE);
    assert(drm_vmwgfx_context_backend_id(typed_context) !=
           (uint32_t)context.cid);
    drm_vmwgfx_context_put(typed_context);
    struct drm_vmw_context_arg failed_context = {0};
    unsigned context_creates_before_fault = drm_test_context_create_calls;
    unsigned context_destroys_before_fault = drm_test_context_destroy_calls;
    fail_copyout_address = (uint64_t)(uintptr_t)&failed_context;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &failed_context) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(failed_context.cid == 0);
    assert(drm_test_context_create_calls == context_creates_before_fault + 1u);
    assert(drm_test_context_destroy_calls == context_destroys_before_fault);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_context_destroy_calls == context_destroys_before_fault + 1u);
    struct drm_vmw_context_arg wrong_context = {.cid = 0};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &context) == -LINUX_EINVAL);

    struct drm_vmw_size levels[2] = {
        {.width = 64, .height = 64, .depth = 1, .pad64 = 0xdeadbeefu},
        {.width = 32, .height = 32, .depth = 1},
    };
    struct drm_vmw_size malformed_levels[2] = {
        {.width = 64, .height = 64, .depth = 1},
        {.width = 31, .height = 32, .depth = 1},
    };
    unsigned surface_creates_before_invalid = drm_test_surface_create_calls;
    union drm_vmw_surface_create_arg invalid_surface = {0};
    invalid_surface.req.flags = 1u << 5;
    invalid_surface.req.format = SVGA3D_A8R8G8B8;
    invalid_surface.req.mip_levels[0] = 2;
    invalid_surface.req.size_addr = (uint64_t)(uintptr_t)malformed_levels;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &invalid_surface) == -LINUX_EINVAL);
    invalid_surface = (union drm_vmw_surface_create_arg){0};
    invalid_surface.req.flags = 1u << 5;
    invalid_surface.req.format = SVGA3D_A8R8G8B8;
    invalid_surface.req.mip_levels[0] = 1;
    invalid_surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    levels[0].width = 20000;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &invalid_surface) == -LINUX_EINVAL);
    levels[0].width = 64;
    invalid_surface = (union drm_vmw_surface_create_arg){0};
    invalid_surface.req.flags = 1u << 5;
    invalid_surface.req.format = SVGA3D_A8R8G8B8;
    invalid_surface.req.mip_levels[0] = 17;
    invalid_surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &invalid_surface) == -LINUX_EINVAL);
    invalid_surface = (union drm_vmw_surface_create_arg){0};
    invalid_surface.req.flags = 1u << 31;
    invalid_surface.req.format = SVGA3D_A8R8G8B8;
    invalid_surface.req.mip_levels[0] = 1;
    invalid_surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &invalid_surface) == -LINUX_EINVAL);
    assert(drm_test_surface_create_calls == surface_creates_before_invalid);

    struct drm_vmw_size byte_buffer_size = {
        .width = 32u * 1024u, .height = 1u, .depth = 1u,
    };
    union drm_vmw_surface_create_arg byte_buffer = {0};
    byte_buffer.req.flags = SVGA3D_SURFACE_HINT_VERTEXBUFFER;
    byte_buffer.req.format = SVGA3D_BUFFER;
    byte_buffer.req.mip_levels[0] = 1;
    byte_buffer.req.size_addr = (uint64_t)(uintptr_t)&byte_buffer_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &byte_buffer) == 0);
    struct drm_vmwgfx_surface *byte_buffer_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)byte_buffer.rep.sid);
    assert(byte_buffer_record);
    struct drm_vmwgfx_surface_metadata byte_buffer_metadata;
    assert(drm_vmwgfx_surface_get_metadata(byte_buffer_record,
                                           &byte_buffer_metadata) == 0);
    assert(byte_buffer_metadata.desc.format == SVGA3D_BUFFER &&
           byte_buffer_metadata.desc.width == 32u * 1024u &&
           byte_buffer_metadata.desc.height == 1u &&
           byte_buffer_metadata.desc.depth == 1u);
    drm_vmwgfx_surface_put(byte_buffer_record);
    struct drm_vmw_surface_arg byte_buffer_unref = {
        .sid = byte_buffer.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &byte_buffer_unref) == 0);
    drm_vmwgfx_process_retirements(1);

    const uint32_t unsupported_stream_output_flags[] = {
        DRM_TEST_SURFACE_BIND_STREAM_OUTPUT,
        DRM_TEST_SURFACE_BIND_STREAM_OUTPUT | SVGA3D_SURFACE_HINT_VERTEXBUFFER,
    };
    for (uint32_t i = 0; i < sizeof(unsupported_stream_output_flags) /
                            sizeof(unsupported_stream_output_flags[0]); ++i) {
        union drm_vmw_surface_create_arg unsupported_stream_output = {0};
        unsupported_stream_output.req.flags = unsupported_stream_output_flags[i];
        unsupported_stream_output.req.format = SVGA3D_BUFFER;
        unsupported_stream_output.req.mip_levels[0] = 1;
        unsupported_stream_output.req.size_addr =
            (uint64_t)(uintptr_t)&byte_buffer_size;
        unsigned backend_creates_before = drm_test_surface_create_calls;
        unsigned allocations_before = allocations;
        int stream_output_ret = ioctl_call(&owner_task, &owner,
            DRM_IOCTL_VMW_CREATE_SURFACE, &unsupported_stream_output);
        if (!stream_output_ret) {
            struct drm_vmw_surface_arg stream_output_unref = {
                .sid = unsupported_stream_output.rep.sid,
                .handle_type = DRM_VMW_HANDLE_LEGACY,
            };
            assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                              &stream_output_unref) == 0);
            drm_vmwgfx_process_retirements(1);
        }
        assert(stream_output_ret == -LINUX_EINVAL);
        assert(drm_test_surface_create_calls == backend_creates_before &&
               allocations == allocations_before);
    }

    union drm_vmw_surface_create_arg stream_output_before_alloc = {0};
    stream_output_before_alloc.req.flags = unsupported_stream_output_flags[1];
    stream_output_before_alloc.req.format = SVGA3D_BUFFER;
    stream_output_before_alloc.req.mip_levels[0] = 1;
    stream_output_before_alloc.req.size_addr =
        (uint64_t)(uintptr_t)&byte_buffer_size;
    unsigned stream_output_backend_before = drm_test_surface_create_calls;
    unsigned stream_output_allocations_before = allocations;
    fail_kernel_malloc_after = 1;
    int stream_output_before_alloc_ret = ioctl_call(&owner_task, &owner,
        DRM_IOCTL_VMW_CREATE_SURFACE, &stream_output_before_alloc);
    int stream_output_allocation_budget_after = fail_kernel_malloc_after;
    fail_kernel_malloc_after = -1;
    assert(stream_output_before_alloc_ret == -LINUX_EINVAL &&
           stream_output_allocation_budget_after == 1);
    assert(drm_test_surface_create_calls == stream_output_backend_before &&
           allocations == stream_output_allocations_before);

    unsigned surface_creates_before_partial = drm_test_surface_create_calls;
    unsigned surface_destroys_before_partial = drm_test_surface_destroy_calls;
    master_refs_before_failure = owner.drm_file->master->references;
    union drm_vmw_surface_create_arg partial_surface = {0};
    partial_surface.req.flags = 1u << 5;
    partial_surface.req.format = SVGA3D_A8R8G8B8;
    partial_surface.req.mip_levels[0] = 2;
    partial_surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    drm_test_surface_create_status = SVGA_EBUSY;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &partial_surface) == -LINUX_EBUSY);
    drm_test_surface_create_status = 0;
    /* The IOWR union is copied back even on an error; its overlapping sid
     * field therefore still contains the input flags, not a published ID. */
    assert(partial_surface.rep.sid >= 0 &&
           (uint32_t)partial_surface.rep.sid < DRM_TEST_RESOURCE_HANDLE_MIN);
    assert(drm_test_surface_create_calls == surface_creates_before_partial + 1u);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_partial);
    assert(owner.drm_file->master->references == master_refs_before_failure + 1u);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_partial + 1u);
    assert(owner.drm_file->master->references == master_refs_before_failure);

    union drm_vmw_surface_create_arg failed_surface = {0};
    failed_surface.req.flags = 1u << 5;
    failed_surface.req.format = SVGA3D_A8R8G8B8;
    failed_surface.req.mip_levels[0] = 2;
    failed_surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    unsigned surface_creates_before_fault = drm_test_surface_create_calls;
    unsigned surface_destroys_before_fault = drm_test_surface_destroy_calls;
    fail_copyout_address = (uint64_t)(uintptr_t)&failed_surface;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &failed_surface) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(drm_test_surface_create_calls == surface_creates_before_fault + 1u);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_fault);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_fault + 1u);

    const uint32_t mesa_surface_flags =
        SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE |
        DRM_TEST_SURFACE_BIND_RENDER_TARGET;
    struct drm_vmw_size mesa_surface_size = {
        .width = 64u, .height = 64u, .depth = 1u,
    };
    union drm_vmw_surface_create_arg mesa_surface = {0};
    mesa_surface.req.flags = mesa_surface_flags;
    mesa_surface.req.format = SVGA3D_A8R8G8B8;
    mesa_surface.req.mip_levels[0] = 1;
    mesa_surface.req.size_addr = (uint64_t)(uintptr_t)&mesa_surface_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &mesa_surface) == 0);
    assert(drm_test_last_surface_desc.format == SVGA3D_A8R8G8B8 &&
           drm_test_last_surface_desc.flags == mesa_surface_flags);
    struct drm_vmwgfx_surface *mesa_surface_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)mesa_surface.rep.sid);
    struct drm_vmwgfx_surface_metadata mesa_surface_metadata;
    assert(mesa_surface_record &&
           drm_vmwgfx_surface_get_metadata(mesa_surface_record,
                                           &mesa_surface_metadata) == 0);
    assert(mesa_surface_metadata.desc.flags == mesa_surface_flags);
    drm_vmwgfx_surface_put(mesa_surface_record);
    union drm_vmw_surface_reference_arg mesa_reference = {0};
    mesa_reference.req.sid = mesa_surface.rep.sid;
    mesa_reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_REF_SURFACE,
                      &mesa_reference) == 0);
    assert(mesa_reference.rep.flags == mesa_surface_flags &&
           mesa_reference.rep.format == SVGA3D_A8R8G8B8);
    struct drm_vmw_surface_arg mesa_unref = {
        .sid = mesa_surface.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &mesa_unref) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &mesa_unref) == 0);
    drm_vmwgfx_process_retirements(1);

    struct drm_vmw_size mesa_depth_size = {
        .width = 64u, .height = 64u, .depth = 1u,
    };
    const uint32_t mesa_depth_flags =
        SVGA3D_SURFACE_HINT_DEPTHSTENCIL |
        DRM_TEST_SURFACE_BIND_DEPTH_STENCIL;
    union drm_vmw_surface_create_arg mesa_depth = {0};
    mesa_depth.req.flags = mesa_depth_flags;
    mesa_depth.req.format = SVGA3D_Z_D24S8;
    mesa_depth.req.mip_levels[0] = 1;
    mesa_depth.req.size_addr = (uint64_t)(uintptr_t)&mesa_depth_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &mesa_depth) == 0);
    assert(drm_test_last_surface_desc.format == SVGA3D_Z_D24S8 &&
           drm_test_last_surface_desc.flags == mesa_depth_flags);
    struct drm_vmwgfx_surface *mesa_depth_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)mesa_depth.rep.sid);
    struct drm_vmwgfx_surface_metadata mesa_depth_metadata;
    assert(mesa_depth_record &&
           drm_vmwgfx_surface_get_metadata(mesa_depth_record,
                                           &mesa_depth_metadata) == 0);
    assert(mesa_depth_metadata.desc.flags == mesa_depth_flags);
    drm_vmwgfx_surface_put(mesa_depth_record);
    struct drm_vmw_surface_arg mesa_depth_unref = {
        .sid = mesa_depth.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &mesa_depth_unref) == 0);
    drm_vmwgfx_process_retirements(1);

    struct drm_vmw_size mesa_buffer_size = {
        .width = 32u * 1024u, .height = 1u, .depth = 1u,
    };
    const uint32_t mesa_buffer_flags =
        DRM_TEST_SURFACE_BIND_VERTEX_BUFFER |
        DRM_TEST_SURFACE_BIND_INDEX_BUFFER |
        DRM_TEST_SURFACE_BIND_CONSTANT_BUFFER |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE;
    union drm_vmw_surface_create_arg mesa_buffer = {0};
    mesa_buffer.req.flags = mesa_buffer_flags;
    mesa_buffer.req.format = SVGA3D_BUFFER;
    mesa_buffer.req.mip_levels[0] = 1;
    mesa_buffer.req.size_addr = (uint64_t)(uintptr_t)&mesa_buffer_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &mesa_buffer) == 0);
    assert(drm_test_last_surface_desc.format == SVGA3D_BUFFER &&
           drm_test_last_surface_desc.flags == mesa_buffer_flags &&
           drm_test_last_surface_desc.width == 32u * 1024u);
    struct drm_vmwgfx_surface *mesa_buffer_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)mesa_buffer.rep.sid);
    struct drm_vmwgfx_surface_metadata mesa_buffer_metadata;
    assert(mesa_buffer_record &&
           drm_vmwgfx_surface_get_metadata(mesa_buffer_record,
                                           &mesa_buffer_metadata) == 0);
    assert(mesa_buffer_metadata.desc.flags == mesa_buffer_flags);
    drm_vmwgfx_surface_put(mesa_buffer_record);
    struct drm_vmw_surface_arg mesa_buffer_unref = {
        .sid = mesa_buffer.rep.sid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &mesa_buffer_unref) == 0);
    drm_vmwgfx_process_retirements(1);

    struct drm_vmw_size alpha8_size = {
        .width = 32u, .height = 32u, .depth = 1u,
    };
    const uint32_t alpha8_flags = SVGA3D_SURFACE_HINT_TEXTURE |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE;
    union drm_vmw_surface_create_arg alpha8_surface = {0};
    alpha8_surface.req.flags = alpha8_flags;
    alpha8_surface.req.format = SVGA3D_ALPHA8;
    alpha8_surface.req.mip_levels[0] = 1;
    alpha8_surface.req.size_addr = (uint64_t)(uintptr_t)&alpha8_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &alpha8_surface) == 0);
    assert(drm_test_last_surface_desc.format == SVGA3D_ALPHA8 &&
           drm_test_last_surface_desc.flags == alpha8_flags &&
           drm_test_last_surface_desc.width == 32u &&
           drm_test_last_surface_desc.height == 32u);
    struct drm_vmwgfx_surface *alpha8_record =
        drm_vmwgfx_surface_lookup_get(owner.drm_file,
                                      (uint32_t)alpha8_surface.rep.sid);
    struct drm_vmwgfx_surface_metadata alpha8_metadata;
    assert(alpha8_record &&
           drm_vmwgfx_surface_get_metadata(alpha8_record,
                                           &alpha8_metadata) == 0);
    assert(alpha8_metadata.desc.format == SVGA3D_ALPHA8 &&
           alpha8_metadata.desc.flags == alpha8_flags &&
           alpha8_metadata.desc.width == 32u &&
           alpha8_metadata.desc.height == 32u &&
           alpha8_metadata.mip_levels[0] == 1u);
    drm_vmwgfx_surface_put(alpha8_record);
    union drm_vmw_surface_reference_arg alpha8_reference = {0};
    alpha8_reference.req.sid = alpha8_surface.rep.sid;
    alpha8_reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    struct drm_vmw_size alpha8_reply_size = {0};
    alpha8_reference.rep.size_addr =
        (uint64_t)(uintptr_t)&alpha8_reply_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_REF_SURFACE,
                      &alpha8_reference) == 0);
    assert(alpha8_reference.rep.format == SVGA3D_ALPHA8 &&
           alpha8_reference.rep.flags == alpha8_flags &&
           alpha8_reference.rep.mip_levels[0] == 1u &&
           alpha8_reply_size.width == 32u &&
           alpha8_reply_size.height == 32u &&
           alpha8_reply_size.depth == 1u);
    struct drm_vmw_surface_arg alpha8_unref = {
        .sid = alpha8_surface.rep.sid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &alpha8_unref) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &alpha8_unref) == 0);
    drm_vmwgfx_process_retirements(1);

    union drm_vmw_surface_create_arg surface = {0};
    surface.req.flags = SVGA3D_SURFACE_HINT_TEXTURE;
    surface.req.format = SVGA3D_A8R8G8B8;
    surface.req.mip_levels[0] = 2;
    surface.req.size_addr = (uint64_t)(uintptr_t)levels;
    surface.req.shareable = -5;
    surface.req.scanout = 2;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &surface) == 0);
    assert(drm_test_last_surface_desc.flags == SVGA3D_SURFACE_HINT_TEXTURE);
    int32_t surface_id = surface.rep.sid;
    assert(surface_id >= (int32_t)DRM_TEST_RESOURCE_HANDLE_MIN);
    assert(surface_id != context.cid);
    assert((uint32_t)surface_id != namespace_bo.handle);
    struct drm_vmwgfx_surface *typed_surface =
        drm_vmwgfx_surface_lookup_get(owner.drm_file, (uint32_t)surface_id);
    struct drm_vmwgfx_context *held_context =
        drm_vmwgfx_context_lookup_get(owner.drm_file, (uint32_t)context.cid);
    assert(typed_surface && held_context);
    assert(drm_vmwgfx_surface_backend_handle(typed_surface) !=
           SVGA_INVALID_HANDLE);
    assert(drm_vmwgfx_surface_backend_id(typed_surface) != (uint32_t)surface_id);
    struct drm_vmwgfx_surface_metadata surface_metadata;
    assert(drm_vmwgfx_surface_get_metadata(typed_surface,
                                           &surface_metadata) == 0);
    assert(surface_metadata.handle == (uint32_t)surface_id &&
           surface_metadata.desc.flags == SVGA3D_SURFACE_HINT_TEXTURE &&
           surface_metadata.desc.width == 64u &&
           surface_metadata.desc.height == 64u &&
           surface_metadata.shareable && surface_metadata.scanout);
    struct drm_vmwgfx_surface_subresource subresource;
    assert(drm_vmwgfx_surface_get_subresource(typed_surface, 0, 1,
                                              &subresource) == 0);
    assert(subresource.width == 32u && subresource.height == 32u &&
           subresource.depth == 1u);
    assert(drm_vmwgfx_surface_get_subresource(typed_surface, 1, 0,
                                              &subresource) == -LINUX_EINVAL);

    struct drm_vmw_surface_arg wrong_surface = {
        .sid = context.cid, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &wrong_surface) == -LINUX_EINVAL);

    struct drm_auth magic = {0};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_MAGIC, &magic) == 0);
    union drm_vmw_surface_reference_arg reference = {0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_PRIME;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EBADF);
    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id + 42;
    reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EINVAL);
    reference.req.sid = surface_id;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EACCES);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_AUTH_MAGIC, &magic) == 0);
    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    fail_copyout_address = (uint64_t)(uintptr_t)&reference;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    struct drm_vmw_surface_arg missing_client_ref = {
        .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &missing_client_ref) == -LINUX_EINVAL);

    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    struct drm_vmw_size base_size = {0};
    reference.rep.size_addr = (uint64_t)(uintptr_t)&base_size;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == 0);
    assert(reference.rep.flags == SVGA3D_SURFACE_HINT_TEXTURE &&
           reference.rep.format == SVGA3D_A8R8G8B8 &&
           reference.rep.mip_levels[0] == 2 &&
           reference.rep.scanout == 1 &&
           reference.rep.shareable == 1 &&
           reference.rep.size_addr == (uint64_t)(uintptr_t)&base_size);
    assert(base_size.width == 64u && base_size.height == 64u &&
           base_size.depth == 1u);
    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == 0); /* Each REF owns a count. */
    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_PRIME;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EBADF);
    reference = (union drm_vmw_surface_reference_arg){0};
    reference.req.sid = surface_id;
    reference.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    reference.rep.size_addr = 1;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_REF_SURFACE,
                      &reference) == -LINUX_EFAULT);

    wrong_context.cid = surface_id;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &wrong_context) == -LINUX_EINVAL);
    struct drm_vmw_surface_arg owner_surface_unref = {
        .sid = surface_id, .handle_type = DRM_VMW_HANDLE_PRIME,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &owner_surface_unref) == 0);
    struct drm_vmw_surface_arg client_surface_unref = {
        .sid = surface_id, .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &client_surface_unref) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &client_surface_unref) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &client_surface_unref) == -LINUX_EINVAL);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &context) == 0);

    unsigned context_destroys_before_held = drm_test_context_destroy_calls;
    unsigned surface_destroys_before_held = drm_test_surface_destroy_calls;
    drm_vmwgfx_process_retirements(2);
    assert(drm_test_context_destroy_calls == context_destroys_before_held);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_held);
    drm_vmwgfx_context_put(held_context);
    drm_vmwgfx_process_retirements(2);
    assert(drm_test_context_destroy_calls == context_destroys_before_held + 1u);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_held);
    drm_vmwgfx_surface_put(typed_surface);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_surface_destroy_calls == surface_destroys_before_held + 1u);

    struct drm_vmw_context_arg first_busy_context = {0};
    struct drm_vmw_context_arg second_busy_context = {0};
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &first_busy_context) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &second_busy_context) == 0);
    struct drm_vmwgfx_context *first_busy_record =
        drm_vmwgfx_context_lookup_get(owner.drm_file,
                                      (uint32_t)first_busy_context.cid);
    struct drm_vmwgfx_context *second_busy_record =
        drm_vmwgfx_context_lookup_get(owner.drm_file,
                                      (uint32_t)second_busy_context.cid);
    assert(first_busy_record && second_busy_record);
    svga_handle first_busy_backend =
        drm_vmwgfx_context_backend_handle(first_busy_record);
    svga_handle second_busy_backend =
        drm_vmwgfx_context_backend_handle(second_busy_record);
    assert(first_busy_backend != second_busy_backend);
    drm_vmwgfx_context_put(first_busy_record);
    drm_vmwgfx_context_put(second_busy_record);
    struct drm_vmw_context_arg first_busy_unref = {
        .cid = first_busy_context.cid,
    };
    struct drm_vmw_context_arg second_busy_unref = {
        .cid = second_busy_context.cid,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &first_busy_unref) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &second_busy_unref) == 0);
    unsigned attempt_start = drm_test_context_destroy_attempt_count;
    unsigned destroys_before_fairness = drm_test_context_destroy_calls;
    drm_test_context_destroy_busy = 1;
    drm_vmwgfx_process_retirements(1);
    drm_vmwgfx_process_retirements(1);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_context_destroy_attempt_count == attempt_start + 3u);
    assert(drm_test_context_destroy_attempts[attempt_start] ==
           first_busy_backend);
    assert(drm_test_context_destroy_attempts[attempt_start + 1u] ==
           second_busy_backend);
    assert(drm_test_context_destroy_attempts[attempt_start + 2u] ==
           first_busy_backend);
    assert(drm_test_context_destroy_calls == destroys_before_fairness + 3u);

    struct drm_vmw_context_arg close_context = {0};
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &close_context) == 0);
    unsigned destroys_before_close = drm_test_context_destroy_calls;

    /* Render REF follows Linux's require-existing per-file reference rule.
     * A render file cannot discover a foreign primary name, and two render
     * files cannot authorize one another through NULL == NULL master identity. */
    struct task render_task = make_task(904, true);
    struct task foreign_render_task = make_task(905, true);
    struct task_file render_file = {
        .node = {.first_cluster = DRM_TEST_RENDER_NODE_KIND},
    };
    struct task_file foreign_render_file = {
        .node = {.first_cluster = DRM_TEST_RENDER_NODE_KIND},
    };
    assert(drm_file_open(&render_task, &render_file) == 0);
    assert(drm_file_open(&foreign_render_task, &foreign_render_file) == 0);
    assert(!render_file.drm_file->master &&
           !foreign_render_file.drm_file->master);
    assert(!render_file.drm_file->authenticated &&
           !foreign_render_file.drm_file->authenticated);
    struct drm_vmw_size primary_render_size = {
        .width = 32, .height = 32, .depth = 1,
    };
    union drm_vmw_surface_create_arg primary_surface = {0};
    primary_surface.req.flags = 1u << 5;
    primary_surface.req.format = SVGA3D_A8R8G8B8;
    primary_surface.req.mip_levels[0] = 1;
    primary_surface.req.size_addr =
        (uint64_t)(uintptr_t)&primary_render_size;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &primary_surface) == 0);
    int32_t primary_render_sid = primary_surface.rep.sid;
    union drm_vmw_surface_reference_arg render_ref = {0};
    render_ref.req.sid = primary_render_sid;
    render_ref.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    assert(ioctl_call(&render_task, &render_file, DRM_IOCTL_VMW_REF_SURFACE,
                      &render_ref) == -LINUX_EACCES);
    struct drm_vmw_surface_arg primary_surface_unref = {
        .sid = primary_render_sid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &primary_surface_unref) == 0);
    assert(drm_vmwgfx_process_retirements(1) == 1u);

    struct drm_vmw_size render_size = {.width = 32, .height = 32, .depth = 1};
    union drm_vmw_surface_create_arg render_surface = {0};
    render_surface.req.flags = 1u << 5;
    render_surface.req.format = SVGA3D_A8R8G8B8;
    render_surface.req.mip_levels[0] = 1;
    render_surface.req.size_addr = (uint64_t)(uintptr_t)&render_size;
    assert(ioctl_call(&render_task, &render_file, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &render_surface) == 0);
    uint32_t render_sid = (uint32_t)render_surface.rep.sid;
    render_ref = (union drm_vmw_surface_reference_arg){0};
    render_ref.req.sid = (int32_t)render_sid;
    render_ref.req.handle_type = DRM_VMW_HANDLE_LEGACY;
    assert(ioctl_call(&foreign_render_task, &foreign_render_file,
                      DRM_IOCTL_VMW_REF_SURFACE, &render_ref) ==
           -LINUX_EACCES);
    assert(ioctl_call(&render_task, &render_file, DRM_IOCTL_VMW_REF_SURFACE,
                      &render_ref) == 0);
    struct drm_vmw_surface_arg render_unref = {
        .sid = (int32_t)render_sid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&render_task, &render_file, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &render_unref) == 0);
    assert(ioctl_call(&render_task, &render_file, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &render_unref) == 0);
    drm_file_close(&foreign_render_file);
    drm_file_close(&render_file);
    drm_vmwgfx_process_retirements(2);

    drm_file_close(&client);
    drm_file_close(&owner);
    assert(drm_test_context_destroy_calls == destroys_before_close);
    uint32_t idle_retired = drm_vmwgfx_process_retirements(2);
    assert(drm_test_context_destroy_calls == destroys_before_close + 1u);
    assert(idle_retired == 1u);
    assert(drm_test_context_create_calls == 6u);
    assert(drm_test_surface_create_calls == 10u);
    assert(drm_test_surface_destroy_calls == 10u);
    assert(allocations == 0);
    (void)foreign_task;
    (void)foreign;
}

static struct drm_test_borrowed *drm_test_borrowed_find(svga_handle handle)
{
    for (uint32_t i = 0; i < drm_test_borrowed_count; ++i)
        if (drm_test_borrowed[i].handle == handle)
            return &drm_test_borrowed[i];
    return NULL;
}

static void test_gmr_retirement_busy_does_not_reuse(
    struct task *task, struct task_file *file)
{
    assert(task && file && drm_vmwgfx_exec_work_count == 0u);

    /* Start from an empty, completed GMR cache so this case owns all entries
     * visited by the production retirement worker. */
    drm_test_ticks += 101u;
    for (uint32_t i = 0; i < 128u && drm_vmwgfx_attachment_count; ++i) {
        ++drm_test_ticks;
        drm_vmwgfx_execbuf_process_retirements(8u);
    }
    assert(drm_vmwgfx_attachment_count == 0u);
    assert(drm_test_borrowed_count == 0u);

    const uint32_t attachments_before = drm_vmwgfx_attachment_count;
    const uint64_t attachment_bytes_before = drm_vmwgfx_attachment_bytes;
    const uint64_t bo_bytes_before = drm_bo_bytes;
    const uint64_t active_pages_before = mm_pages_allocated - mm_pages_freed;
    const uint32_t owner_puts_before = drm_test_borrowed_owner_put_calls;
    const uint32_t detach_attempts_before = drm_test_borrowed_detach_attempts;
    const uint32_t detach_publications_before =
        drm_test_borrowed_detach_publications;
    const uint32_t detach_busy_before = drm_test_borrowed_detach_busy_returns;

    struct drm_vmw_context_arg context = {0};
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_CREATE_CONTEXT, &context) == 0);
    union drm_vmw_alloc_bo_arg bo = {0};
    bo.req.size = 4096u;
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_ALLOC_BO, &bo) == 0);
    struct drm_bo *bo_pin = drm_bo_lookup_handle_get(file->drm_file,
                                                      bo.rep.handle);
    assert(bo_pin && bo_pin->references == 2u);

    SVGA3dCmdBeginQuery begin = {
        .cid = (uint32_t)context.cid,
        .type = SVGA3D_QUERYTYPE_OCCLUSION,
    };
    SVGA3dCmdEndQuery end = {
        .cid = (uint32_t)context.cid,
        .type = SVGA3D_QUERYTYPE_OCCLUSION,
        .guestResult = {.gmrId = bo.rep.handle, .offset = 0},
    };
    uint32_t stream[4u + sizeof(begin) / sizeof(uint32_t) +
                    sizeof(end) / sizeof(uint32_t)] = {0};
    uint32_t stream_words = test_append_packet(stream,
        sizeof(stream) / sizeof(stream[0]), 0,
        SVGA_3D_CMD_BEGIN_QUERY, &begin, sizeof(begin));
    stream_words = test_append_packet(stream,
        sizeof(stream) / sizeof(stream[0]), stream_words,
        SVGA_3D_CMD_END_QUERY, &end, sizeof(end));
    struct drm_vmw_execbuf_arg exec = {
        .commands = (uint64_t)(uintptr_t)stream,
        .command_size = stream_words * sizeof(uint32_t),
        .version = 1u,
    };

    drm_test_last_retained_gmr = 0;
    const uint32_t submits_before = drm_test_batch_submit_calls;
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_EXECBUF, &exec) == 0);
    const svga_handle old_gmr = drm_test_last_retained_gmr;
    const uint32_t first_fence = drm_test_fence_issued;
    assert(old_gmr && drm_test_batch_submit_calls == submits_before + 1u);
    struct drm_test_borrowed *old_record = drm_test_borrowed_find(old_gmr);
    assert(old_record && old_record->owner);
    struct drm_vmwgfx_attachment *old_attachment = old_record->owner;
    assert(old_attachment->bo == bo_pin && old_attachment->ready &&
           old_attachment->references == 1u);

    /* Passing the real EXECBUF fence drops its attachment reference and
     * starts the cache-age interval in the production cleanup path. */
    drm_test_fence_passed = first_fence;
    drm_vmwgfx_execbuf_process_retirements(1u);
    assert(drm_vmwgfx_exec_work_count == 0u);
    assert(old_attachment->references == 0u && bo_pin->references == 3u);
    assert(drm_vmwgfx_attachment_count == attachments_before + 1u);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before + 4096u);

    /* The fake backend publishes GMR destruction once, returns EBUSY until
     * its explicit fence sequence passes, and keeps the owner callback held. */
    drm_test_borrowed_detach_fence_handle = old_gmr;
    drm_test_ticks += 101u;
    for (uint32_t i = 0; i < 8u &&
         drm_test_borrowed_detach_publications == detach_publications_before;
         ++i) {
        ++drm_test_ticks;
        drm_vmwgfx_execbuf_process_retirements(1u);
    }
    assert(drm_test_borrowed_detach_publications ==
           detach_publications_before + 1u);
    const uint32_t retirement_fence = old_record->retirement_fence_seq;
    assert(old_record->destruction_published && retirement_fence > first_fence);
    assert(drm_test_fence_passed < retirement_fence);
    assert(drm_test_borrowed_detach_attempts == detach_attempts_before + 1u);
    assert(drm_test_borrowed_detach_busy_returns == detach_busy_before + 1u);
    assert(drm_test_borrowed_owner_put_calls == owner_puts_before);
    assert(drm_vmwgfx_attachment_count == attachments_before + 1u);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before + 4096u);
    assert(bo_pin->references == 3u);

    /* Submitting against the same BO while its old GMR retires must create a
     * distinct mapping and must never retain the old backend handle. */
    drm_test_last_retained_gmr = 0;
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_EXECBUF, &exec) == 0);
    const svga_handle new_gmr = drm_test_last_retained_gmr;
    const uint32_t second_fence = drm_test_fence_issued;
    assert(drm_test_batch_submit_calls == submits_before + 2u);
    assert(new_gmr && new_gmr != old_gmr);
    assert(old_attachment->detaching);
    struct drm_test_borrowed *new_record = drm_test_borrowed_find(new_gmr);
    assert(new_record && new_record->owner && new_record != old_record);
    struct drm_vmwgfx_attachment *new_attachment = new_record->owner;
    assert(new_attachment != old_attachment && new_attachment->bo == bo_pin &&
           new_attachment->ready && new_attachment->references == 1u);
    assert(drm_vmwgfx_attachment_count == attachments_before + 2u);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before + 8192u);
    assert(drm_test_borrowed_count == 2u);
    assert(bo_pin->references == 5u);

    /* Closing the BO handle cannot release its pages while either mapping or
     * the new EXECBUF still owns it. */
    assert(ioctl_call(task, file, DRM_IOCTL_VMW_UNREF_BO,
        &(struct drm_vmw_handle_close_arg){.handle = bo.rep.handle}) == 0);
    assert(bo_pin->references == 4u);
    assert(drm_bo_bytes == bo_bytes_before + 4096u);
    assert(mm_pages_allocated - mm_pages_freed == active_pages_before + 1u);

    for (uint32_t i = 0; i < 2u; ++i) {
        ++drm_test_ticks;
        drm_vmwgfx_execbuf_process_retirements(8u);
    }
    assert(drm_test_borrowed_detach_attempts == detach_attempts_before + 3u);
    assert(drm_test_borrowed_detach_publications ==
           detach_publications_before + 1u);
    assert(drm_test_borrowed_detach_busy_returns == detach_busy_before + 3u);
    assert(drm_test_borrowed_owner_put_calls == owner_puts_before);
    assert(old_attachment->detaching && bo_pin->references == 4u);
    assert(drm_vmwgfx_attachment_count == attachments_before + 2u);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before + 8192u);

    /* Passing the backend retirement fence releases the old owner exactly
     * once; the newer mapping remains live until its own EXECBUF fence passes. */
    drm_test_fence_passed = retirement_fence;
    ++drm_test_ticks;
    drm_vmwgfx_execbuf_process_retirements(8u);
    assert(drm_test_borrowed_detach_attempts == detach_attempts_before + 4u);
    assert(drm_test_borrowed_detach_publications ==
           detach_publications_before + 1u);
    assert(drm_test_borrowed_detach_busy_returns == detach_busy_before + 3u);
    assert(drm_test_borrowed_owner_put_calls == owner_puts_before + 1u);
    assert(drm_test_borrowed_find(old_gmr) == NULL);
    struct drm_test_borrowed *surviving_record =
        drm_test_borrowed_find(new_gmr);
    assert(surviving_record && surviving_record->owner == new_attachment);
    assert(drm_vmwgfx_attachment_count == attachments_before + 1u);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before + 4096u);
    assert(bo_pin->references == 3u);

    drm_test_fence_passed = second_fence;
    ++drm_test_ticks;
    drm_vmwgfx_execbuf_process_retirements(1u);
    assert(drm_vmwgfx_exec_work_count == 0u);
    assert(new_attachment->references == 0u && bo_pin->references == 2u);

    /* Let the new cache entry reach normal idle retirement and verify its
     * callback restores all GMR, BO-byte, and physical-page accounting. */
    drm_test_borrowed_detach_fence_handle = 0;
    drm_test_ticks += 101u;
    for (uint32_t i = 0; i < 8u &&
         drm_vmwgfx_attachment_count > attachments_before; ++i) {
        ++drm_test_ticks;
        drm_vmwgfx_execbuf_process_retirements(1u);
    }
    assert(drm_vmwgfx_attachment_count == attachments_before);
    assert(drm_vmwgfx_attachment_bytes == attachment_bytes_before);
    assert(drm_test_borrowed_count == 0u);
    assert(drm_test_borrowed_owner_put_calls == owner_puts_before + 2u);
    assert(bo_pin->references == 1u);
    drm_bo_put(bo_pin);
    assert(drm_bo_bytes == bo_bytes_before);
    assert(mm_pages_allocated - mm_pages_freed == active_pages_before);

    assert(ioctl_call(task, file, DRM_IOCTL_VMW_UNREF_CONTEXT, &context) == 0);
    drm_vmwgfx_process_retirements(8u);
    drm_test_borrowed_detach_fence_handle = 0;
}

static void test_vgpu9_execbuf_v1_empty_fence_abi(void)
{
    struct task owner_task = make_task(900, false);
    struct task client_task = make_task(901, false);
    struct task_file owner = {0}, client = {0};
    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    /* Model the pinned open description held by the real ioctl syscall path. */
    client_task.syscall_file = &client;
    client_task.syscall_fd = 7;
    client_task.syscall_file_number = LINUX_SYS_IOCTL;
    assert(!client.drm_file->authenticated);

    struct drm_vmw_fence_rep reply = {0};
    struct drm_vmw_execbuf_arg exec = {
        .commands = UINT64_MAX, /* Empty EXECBUF must not dereference this. */
        .command_size = 0,
        .fence_rep = (uint64_t)(uintptr_t)&reply,
        .version = 1,
        .flags = 0,
        .context_handle = 0xdeadbeefu, /* Linux ignores this for v1. */
        .imported_fence_fd = 0x1234,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &exec) == 0);
    assert(reply.handle && reply.mask == 0 && reply.fd == -1 && !reply.error);
    assert(reply.seqno == drm_test_fence_issued &&
           reply.passed_seqno == drm_test_fence_passed);

    struct drm_vmw_fence_signaled_arg signaled = {
        .handle = reply.handle,
        .flags = 0x80000000u,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &signaled) == 0);
    assert(!signaled.signaled && signaled.signaled_flags == signaled.flags &&
           signaled.passed_seqno == drm_test_fence_passed);
    drm_test_fence_passed = reply.seqno;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &signaled) == 0);
    assert(signaled.signaled && signaled.signaled_flags == signaled.flags);

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
                      &(struct drm_vmw_fence_arg){.handle = reply.handle}) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
                      &(struct drm_vmw_fence_arg){.handle = reply.handle}) ==
           -LINUX_EINVAL);

    struct drm_vmw_execbuf_arg short_exec = {
        .commands = UINT64_MAX,
        .command_size = 0,
        .fence_rep = (uint64_t)(uintptr_t)&reply,
        .version = 1,
        .flags = 0,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF_SHORT,
                      &short_exec) == 0);
    assert(reply.handle && reply.mask == 0 && reply.fd == -1);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
                      &(struct drm_vmw_fence_arg){.handle = reply.handle}) == 0);

    /* A structurally valid packet with an unauthorized per-file context must
     * fail validation before any fence is committed. */
    SVGA3dCmdSetTransform invalid_transform = {
        .cid = 0x01000001u, .type = SVGA3D_TRANSFORM_WORLD,
    };
    uint32_t invalid_context_stream[2 + sizeof(invalid_transform) / sizeof(uint32_t)] = {
        SVGA_3D_CMD_SETTRANSFORM, sizeof(invalid_transform),
    };
    memcpy(&invalid_context_stream[2], &invalid_transform,
           sizeof(invalid_transform));
    uint32_t fence_count_before_invalid_stream = drm_test_fence_issued;
    uint32_t batch_submit_before_invalid_stream = drm_test_batch_submit_calls;
    uint32_t abort_count_before_invalid_stream = drm_test_batch_abort_calls;
    struct drm_vmw_execbuf_arg invalid_stream_exec = {
        .commands = (uint64_t)(uintptr_t)invalid_context_stream,
        .command_size = sizeof(invalid_context_stream),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &invalid_stream_exec) == -LINUX_EINVAL);
    assert(drm_test_fence_issued == fence_count_before_invalid_stream);
    assert(drm_test_batch_submit_calls == batch_submit_before_invalid_stream);
    assert(drm_test_batch_abort_calls == abort_count_before_invalid_stream);

    struct drm_vmw_context_arg context = {0};
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &context) == 0);
    struct drm_vmw_size exact_vertex_bytes = {
        .width = 16u, .height = 1u, .depth = 1u,
    };
    union drm_vmw_surface_create_arg exact_vertex_buffer = {0};
    exact_vertex_buffer.req.flags = SVGA3D_SURFACE_HINT_VERTEXBUFFER;
    exact_vertex_buffer.req.format = SVGA3D_BUFFER;
    exact_vertex_buffer.req.mip_levels[0] = 1;
    exact_vertex_buffer.req.size_addr =
        (uint64_t)(uintptr_t)&exact_vertex_bytes;
    assert(ioctl_call(&client_task, &client,
                      DRM_IOCTL_VMW_CREATE_SURFACE,
                      &exact_vertex_buffer) == 0);

    struct drm_vmw_size alpha8_texture_size = {
        .width = 32u, .height = 32u, .depth = 1u,
    };
    union drm_vmw_surface_create_arg alpha8_texture = {0};
    alpha8_texture.req.flags = SVGA3D_SURFACE_HINT_TEXTURE |
        DRM_TEST_SURFACE_BIND_SHADER_RESOURCE;
    alpha8_texture.req.format = SVGA3D_ALPHA8;
    alpha8_texture.req.mip_levels[0] = 1;
    alpha8_texture.req.size_addr =
        (uint64_t)(uintptr_t)&alpha8_texture_size;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_CREATE_SURFACE,
                      &alpha8_texture) == 0);
    union drm_vmw_alloc_bo_arg alpha8_dma_bo = {0};
    alpha8_dma_bo.req.size = 32u * 32u;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_ALLOC_BO,
                      &alpha8_dma_bo) == 0);

    /* One texture bind and both DMA directions exercise the production
     * format-byte table at the exact 1024-byte Alpha8 footprint. */
    SVGA3dCmdSetTextureState alpha8_texture_header = {.cid = context.cid};
    SVGA3dTextureState alpha8_texture_state = {
        .stage = 0,
        .name = SVGA3D_TS_BIND_TEXTURE,
        .value = (uint32_t)alpha8_texture.rep.sid,
    };
    uint8_t alpha8_texture_body[sizeof(alpha8_texture_header) +
                                sizeof(alpha8_texture_state)];
    __builtin_memcpy(alpha8_texture_body, &alpha8_texture_header,
                     sizeof(alpha8_texture_header));
    __builtin_memcpy(alpha8_texture_body + sizeof(alpha8_texture_header),
                     &alpha8_texture_state, sizeof(alpha8_texture_state));
    SVGA3dCmdSurfaceDMA alpha8_dma = {0};
    alpha8_dma.guest.ptr.gmrId = alpha8_dma_bo.rep.handle;
    alpha8_dma.guest.pitch = 32u;
    alpha8_dma.host.sid = (uint32_t)alpha8_texture.rep.sid;
    alpha8_dma.transfer = SVGA3D_WRITE_HOST_VRAM;
    SVGA3dCopyBox alpha8_dma_box = {.w = 32u, .h = 32u, .d = 1u};
    SVGA3dCmdSurfaceDMASuffix alpha8_dma_suffix = {
        .suffixSize = sizeof(alpha8_dma_suffix),
        .maximumOffset = 32u * 32u,
    };
    uint8_t alpha8_dma_body[sizeof(alpha8_dma) + sizeof(alpha8_dma_box) +
                            sizeof(alpha8_dma_suffix)] = {0};
    __builtin_memcpy(alpha8_dma_body, &alpha8_dma, sizeof(alpha8_dma));
    __builtin_memcpy(alpha8_dma_body + sizeof(alpha8_dma), &alpha8_dma_box,
                     sizeof(alpha8_dma_box));
    __builtin_memcpy(alpha8_dma_body + sizeof(alpha8_dma) +
                     sizeof(alpha8_dma_box), &alpha8_dma_suffix,
                     sizeof(alpha8_dma_suffix));
    uint32_t alpha8_dma_stream[128] = {0};
    uint32_t alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128, 0,
        SVGA_3D_CMD_SETTEXTURESTATE, alpha8_texture_body,
        sizeof(alpha8_texture_body));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128,
        alpha8_dma_dwords, SVGA_3D_CMD_SURFACE_DMA, alpha8_dma_body,
        sizeof(alpha8_dma_body));
    alpha8_dma.transfer = SVGA3D_READ_HOST_VRAM;
    __builtin_memcpy(alpha8_dma_body, &alpha8_dma, sizeof(alpha8_dma));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128,
        alpha8_dma_dwords, SVGA_3D_CMD_SURFACE_DMA, alpha8_dma_body,
        sizeof(alpha8_dma_body));
    struct drm_vmw_execbuf_arg alpha8_dma_exec = {
        .commands = (uint64_t)(uintptr_t)alpha8_dma_stream,
        .command_size = alpha8_dma_dwords * sizeof(uint32_t),
        .version = 1,
    };
    uint32_t alpha8_dma_submits = drm_test_batch_submit_calls;
    uint32_t alpha8_dma_appends = drm_test_batch_append_calls;
    uint32_t alpha8_dma_attachments = drm_test_borrowed_count;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &alpha8_dma_exec) == 0);
    assert(drm_test_batch_submit_calls == alpha8_dma_submits + 1u &&
           drm_test_batch_append_calls == alpha8_dma_appends + 3u &&
           drm_test_borrowed_count == alpha8_dma_attachments + 1u);
    uint32_t alpha8_dma_sequence = drm_test_fence_issued;

    /* An undersized row pitch, suffix range, or BO range must reject the
     * whole stream before adding a batch packet, borrowed GMR, or fence. */
    uint32_t alpha8_bad_pitch = 31u;
    __builtin_memcpy(alpha8_dma_body, &alpha8_dma, sizeof(alpha8_dma));
    __builtin_memcpy(&((SVGA3dCmdSurfaceDMA *)alpha8_dma_body)->guest.pitch,
                     &alpha8_bad_pitch, sizeof(alpha8_bad_pitch));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128, 0,
        SVGA_3D_CMD_SETTEXTURESTATE, alpha8_texture_body,
        sizeof(alpha8_texture_body));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128,
        alpha8_dma_dwords, SVGA_3D_CMD_SURFACE_DMA, alpha8_dma_body,
        sizeof(alpha8_dma_body));
    alpha8_dma_exec.command_size = alpha8_dma_dwords * sizeof(uint32_t);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &alpha8_dma_exec) == -LINUX_EINVAL);
    uint32_t alpha8_short_maximum = 1023u;
    __builtin_memcpy(alpha8_dma_body, &alpha8_dma, sizeof(alpha8_dma));
    __builtin_memcpy(alpha8_dma_body + sizeof(alpha8_dma) +
                     sizeof(alpha8_dma_box) +
                     __builtin_offsetof(SVGA3dCmdSurfaceDMASuffix,
                                        maximumOffset),
                     &alpha8_short_maximum, sizeof(alpha8_short_maximum));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128, 0,
        SVGA_3D_CMD_SETTEXTURESTATE, alpha8_texture_body,
        sizeof(alpha8_texture_body));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128,
        alpha8_dma_dwords, SVGA_3D_CMD_SURFACE_DMA, alpha8_dma_body,
        sizeof(alpha8_dma_body));
    alpha8_dma_exec.command_size = alpha8_dma_dwords * sizeof(uint32_t);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &alpha8_dma_exec) == -LINUX_EINVAL);
    alpha8_dma.guest.ptr.gmrId = alpha8_dma_bo.rep.handle;
    alpha8_dma.guest.ptr.offset = 4096u - 512u;
    alpha8_dma_suffix.maximumOffset = 32u * 32u;
    __builtin_memcpy(alpha8_dma_body, &alpha8_dma, sizeof(alpha8_dma));
    __builtin_memcpy(alpha8_dma_body + sizeof(alpha8_dma) +
                     sizeof(alpha8_dma_box), &alpha8_dma_suffix,
                     sizeof(alpha8_dma_suffix));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128, 0,
        SVGA_3D_CMD_SETTEXTURESTATE, alpha8_texture_body,
        sizeof(alpha8_texture_body));
    alpha8_dma_dwords = test_append_packet(alpha8_dma_stream, 128,
        alpha8_dma_dwords, SVGA_3D_CMD_SURFACE_DMA, alpha8_dma_body,
        sizeof(alpha8_dma_body));
    alpha8_dma_exec.command_size = alpha8_dma_dwords * sizeof(uint32_t);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &alpha8_dma_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == alpha8_dma_submits + 1u &&
           drm_test_batch_append_calls == alpha8_dma_appends + 3u &&
           drm_test_borrowed_count == alpha8_dma_attachments + 1u &&
           drm_test_fence_issued == alpha8_dma_sequence);

    struct drm_vmw_surface_arg alpha8_texture_unref = {
        .sid = alpha8_texture.rep.sid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    uint32_t alpha8_surface_destroys = drm_test_surface_destroy_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &alpha8_texture_unref) == 0);
    drm_vmwgfx_process_retirements(1);
    assert(drm_test_surface_destroy_calls == alpha8_surface_destroys);
    drm_test_fence_passed = alpha8_dma_sequence;
    drm_vmwgfx_execbuf_process_retirements(DRM_VMW_EXEC_WORK_MAX);
    drm_vmwgfx_process_retirements(DRM_VMW_EXEC_WORK_MAX);
    assert(drm_test_surface_destroy_calls == alpha8_surface_destroys + 1u &&
           drm_vmwgfx_exec_work_count == 0u &&
           drm_test_borrowed_count == alpha8_dma_attachments + 1u);
    struct drm_vmw_handle_close_arg alpha8_dma_bo_unref = {
        .handle = alpha8_dma_bo.rep.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_BO,
                      &alpha8_dma_bo_unref) == 0);

    struct drm_vmw_context_arg foreign_context = {0};
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_CREATE_CONTEXT,
                      &foreign_context) == 0);
    SVGA3dCmdSetTransform foreign_transform = {
        .cid = foreign_context.cid, .type = SVGA3D_TRANSFORM_WORLD,
    };
    uint32_t foreign_stream[2 + sizeof(foreign_transform) / sizeof(uint32_t)] = {
        SVGA_3D_CMD_SETTRANSFORM, sizeof(foreign_transform),
    };
    memcpy(&foreign_stream[2], &foreign_transform,
           sizeof(foreign_transform));
    uint32_t submits_before_foreign = drm_test_batch_submit_calls;
    struct drm_vmw_execbuf_arg foreign_exec = {
        .commands = (uint64_t)(uintptr_t)foreign_stream,
        .command_size = sizeof(foreign_stream),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &foreign_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == submits_before_foreign);

    SVGA3dCmdSetTransform transform = {
        .cid = context.cid, .type = SVGA3D_TRANSFORM_WORLD,
    };
    uint32_t valid_stream[2 + sizeof(transform) / sizeof(uint32_t)] = {
        SVGA_3D_CMD_SETTRANSFORM, sizeof(transform),
    };
    memcpy(&valid_stream[2], &transform, sizeof(transform));
    struct drm_vmw_execbuf_arg valid_exec = {
        .commands = (uint64_t)(uintptr_t)valid_stream,
        .command_size = sizeof(valid_stream),
        .version = 1,
        .context_handle = 0xdeadbeefu,
    };
    uint32_t submits_before_valid = drm_test_batch_submit_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &valid_exec) == 0);
    assert(drm_test_batch_submit_calls == submits_before_valid + 1u);
    assert(drm_test_batch_append_calls != 0);

    /* A final-submit failure must stay silent by default and copy the
     * backend's numeric snapshot before abort cleanup can overwrite it. */
    drm_test_diag_log_reset();
    drm_faildiag_configure(NULL);
    drm_test_batch_submit_failure = SVGA_EINVAL;
    drm_test_batch_abort_rewrites_failure = true;
    uint32_t submit_reason_calls_before = drm_test_batch_submit_calls;
    uint32_t submit_reason_aborts_before = drm_test_batch_abort_calls;
    uint32_t submit_reason_fences_before = drm_test_fence_issued;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &valid_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == submit_reason_calls_before + 1u);
    assert(drm_test_batch_abort_calls == submit_reason_aborts_before + 1u);
    assert(drm_test_fence_issued == submit_reason_fences_before);
    assert(drm_test_diag_log_calls == 0u);

    drm_test_diag_log_reset();
    drm_faildiag_configure("drm.faildiag=1");
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &valid_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == submit_reason_calls_before + 2u);
    assert(drm_test_batch_abort_calls == submit_reason_aborts_before + 2u);
    assert(drm_test_fence_issued == submit_reason_fences_before);
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "stage=exec.fifo_submit"));
    assert(strstr(drm_test_diag_log_buffer, "errno=-22"));
    char submit_reason_field[64];
    (void)snprintf(submit_reason_field, sizeof(submit_reason_field),
        "source=%llu", (unsigned long long)sizeof(valid_stream));
    assert(strstr(drm_test_diag_log_buffer, submit_reason_field));
    (void)snprintf(submit_reason_field, sizeof(submit_reason_field),
        "output=%llu", (unsigned long long)sizeof(valid_stream));
    assert(strstr(drm_test_diag_log_buffer, submit_reason_field));
    assert(strstr(drm_test_diag_log_buffer, "capacity=4096"));
    assert(strstr(drm_test_diag_log_buffer, "reason=5"));
    assert(strstr(drm_test_diag_log_buffer, "index=7"));
    (void)snprintf(submit_reason_field, sizeof(submit_reason_field),
        "detail0=%llu",
        (unsigned long long)SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID);
    assert(strstr(drm_test_diag_log_buffer, submit_reason_field));
    assert(strstr(drm_test_diag_log_buffer, "detail1=19"));
    assert(strstr(drm_test_diag_log_buffer, "detail2=20"));
    assert(!strstr(drm_test_diag_log_buffer, "work_count="));
    drm_test_batch_submit_failure = 0;
    drm_test_batch_abort_rewrites_failure = false;
    drm_faildiag_configure(NULL);

    /* An invalid field in a later packet must reject the whole stream before
     * FIFO publication, even though the earlier context/transform is valid. */
    SVGA3dCmdSetTransform invalid_enum_transform = {
        .cid = context.cid, .type = SVGA3D_TRANSFORM_MAX,
    };
    uint32_t invalid_enum_stream[2 *
        (2 + sizeof(transform) / sizeof(uint32_t))] = {0};
    memcpy(invalid_enum_stream, valid_stream, sizeof(valid_stream));
    uint32_t invalid_enum_offset = sizeof(valid_stream) / sizeof(uint32_t);
    invalid_enum_stream[invalid_enum_offset] = SVGA_3D_CMD_SETTRANSFORM;
    invalid_enum_stream[invalid_enum_offset + 1u] =
        sizeof(invalid_enum_transform);
    memcpy(&invalid_enum_stream[invalid_enum_offset + 2u],
           &invalid_enum_transform, sizeof(invalid_enum_transform));
    struct drm_vmw_execbuf_arg invalid_enum_exec = {
        .commands = (uint64_t)(uintptr_t)invalid_enum_stream,
        .command_size = sizeof(invalid_enum_stream),
        .version = 1,
    };
    uint32_t invalid_enum_submits = drm_test_batch_submit_calls;
    uint32_t invalid_enum_fences = drm_test_fence_issued;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &invalid_enum_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == invalid_enum_submits &&
           drm_test_fence_issued == invalid_enum_fences);

    /* Mesa's rangeHint.last is exclusive. A four-vertex range ending exactly
     * at a 16-byte buffer must be accepted without reading a fifth element. */
    SVGA3dCmdDrawPrimitives draw_header = {
        .cid = context.cid, .numVertexDecls = 1, .numRanges = 1,
    };
    SVGA3dVertexDecl draw_decl = {
        .identity = {
            .type = SVGA3D_DECLTYPE_FLOAT1,
            .method = SVGA3D_DECLMETHOD_DEFAULT,
            .usage = SVGA3D_DECLUSAGE_POSITION,
        },
        .array = {
            .surfaceId = (uint32_t)exact_vertex_buffer.rep.sid,
            .offset = 0, .stride = sizeof(float),
        },
        .rangeHint = {.first = 0, .last = 4},
    };
    SVGA3dPrimitiveRange draw_range = {
        .primType = SVGA3D_PRIMITIVE_TRIANGLELIST,
        .primitiveCount = 1,
        .indexArray = {.surfaceId = SVGA3D_INVALID_ID},
    };
    uint8_t draw_body[sizeof(draw_header) + sizeof(draw_decl) +
                      sizeof(draw_range)];
    __builtin_memcpy(draw_body, &draw_header, sizeof(draw_header));
    __builtin_memcpy(draw_body + sizeof(draw_header), &draw_decl,
                     sizeof(draw_decl));
    __builtin_memcpy(draw_body + sizeof(draw_header) + sizeof(draw_decl),
                     &draw_range, sizeof(draw_range));
    uint32_t draw_stream[2 + sizeof(draw_body) / sizeof(uint32_t)] = {0};
    uint32_t draw_dwords = test_append_packet(draw_stream,
        sizeof(draw_stream) / sizeof(draw_stream[0]), 0,
        SVGA_3D_CMD_DRAW_PRIMITIVES, draw_body, sizeof(draw_body));
    struct drm_vmw_execbuf_arg draw_exec = {
        .commands = (uint64_t)(uintptr_t)draw_stream,
        .command_size = draw_dwords * sizeof(uint32_t),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &draw_exec) == 0);

    /* The range hint is advisory. This draw fetches exactly three FLOAT1
     * vertices (12 bytes), while its conservative hint covers eight. */
    draw_decl.rangeHint = (SVGA3dArrayRangeHint){.first = 0, .last = 8};
    __builtin_memcpy(draw_body + sizeof(draw_header), &draw_decl,
                     sizeof(draw_decl));
    draw_dwords = test_append_packet(draw_stream,
        sizeof(draw_stream) / sizeof(draw_stream[0]), 0,
        SVGA_3D_CMD_DRAW_PRIMITIVES, draw_body, sizeof(draw_body));
    draw_exec.command_size = draw_dwords * sizeof(uint32_t);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &draw_exec) == 0);

    /* The advertised VGPU9 limit is four color attachments (plus depth and
     * stencil). COLOR4 is outside that negotiated profile. */
    SVGA3dCmdSetRenderTarget unsupported_target = {
        .cid = context.cid,
        .target = {.sid = SVGA3D_INVALID_ID},
        .type = SVGA3D_RT_COLOR4,
    };
    uint32_t unsupported_target_stream[2 +
        sizeof(unsupported_target) / sizeof(uint32_t)] = {0};
    uint32_t unsupported_target_dwords = test_append_packet(
        unsupported_target_stream,
        sizeof(unsupported_target_stream) / sizeof(uint32_t), 0,
        SVGA_3D_CMD_SETRENDERTARGET, &unsupported_target,
        sizeof(unsupported_target));
    struct drm_vmw_execbuf_arg unsupported_target_exec = {
        .commands = (uint64_t)(uintptr_t)unsupported_target_stream,
        .command_size = unsupported_target_dwords * sizeof(uint32_t),
        .version = 1,
    };
    uint32_t unsupported_target_submits = drm_test_batch_submit_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &unsupported_target_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == unsupported_target_submits);

    /* Mesa's exact 64 KiB input ceiling must fit as one immutable command
     * stream and one backend batch. Two variable render-state packets fill
     * the boundary exactly without relying on a test-only parser shortcut. */
    const uint32_t state_counts[2] = {4094u, 4095u};
    uint32_t *max_stream = malloc(DRM_VMW_EXECBUF_MAX_COMMAND_BYTES);
    assert(max_stream);
    uint32_t max_dwords = 0;
    uint32_t append_before_max = drm_test_batch_append_calls;
    uint32_t submit_before_max = drm_test_batch_submit_calls;
    for (unsigned packet = 0; packet < 2; ++packet) {
        uint32_t state_count = state_counts[packet];
        uint32_t body_bytes = sizeof(uint32_t) +
            state_count * sizeof(SVGA3dRenderState);
        uint8_t *body = calloc(1, body_bytes);
        assert(body);
        __builtin_memcpy(body, &context.cid, sizeof(context.cid));
        SVGA3dRenderState state = {
            .state = SVGA3D_RS_ZENABLE,
            .uintValue = 0,
        };
        for (uint32_t i = 0; i < state_count; ++i)
            __builtin_memcpy(body + sizeof(uint32_t) +
                i * sizeof(state), &state, sizeof(state));
        max_dwords = test_append_packet(max_stream,
            DRM_VMW_EXECBUF_MAX_COMMAND_BYTES / sizeof(uint32_t),
            max_dwords, SVGA_3D_CMD_SETRENDERSTATE, body, body_bytes);
        free(body);
    }
    assert(max_dwords * sizeof(uint32_t) ==
           DRM_VMW_EXECBUF_MAX_COMMAND_BYTES);
    struct drm_vmw_execbuf_arg max_exec = {
        .commands = (uint64_t)(uintptr_t)max_stream,
        .command_size = DRM_VMW_EXECBUF_MAX_COMMAND_BYTES,
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &max_exec) == 0);
    assert(drm_test_batch_append_calls == append_before_max + 2u &&
           drm_test_batch_submit_calls == submit_before_max + 1u);
    free(max_stream);

    uint32_t late_bad_stream[2 * (2 + sizeof(transform) / sizeof(uint32_t))];
    memcpy(late_bad_stream, valid_stream, sizeof(valid_stream));
    uint32_t bad_packet_offset = sizeof(valid_stream) / sizeof(uint32_t);
    late_bad_stream[bad_packet_offset] = SVGA_3D_CMD_SETTRANSFORM;
    late_bad_stream[bad_packet_offset + 1u] = sizeof(transform);
    transform.cid = 0xf0000000u;
    memcpy(&late_bad_stream[bad_packet_offset + 2u], &transform,
           sizeof(transform));
    struct drm_vmw_execbuf_arg late_bad_exec = {
        .commands = (uint64_t)(uintptr_t)late_bad_stream,
        .command_size = sizeof(late_bad_stream),
        .version = 1,
    };
    submits_before_valid = drm_test_batch_submit_calls;
    abort_count_before_invalid_stream = drm_test_batch_abort_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &late_bad_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == submits_before_valid);
    assert(drm_test_batch_abort_calls == abort_count_before_invalid_stream);

    SVGA3dCmdDefineShader shader_define = {
        .cid = context.cid, .shid = 42u, .type = SVGA3D_SHADERTYPE_VS,
    };
    uint32_t shader_code = 0x00000001u;
    uint8_t define_body[sizeof(shader_define) + sizeof(shader_code)];
    memcpy(define_body, &shader_define, sizeof(shader_define));
    memcpy(define_body + sizeof(shader_define), &shader_code,
           sizeof(shader_code));
    SVGA3dCmdSetShader shader_bind = {
        .cid = context.cid, .type = SVGA3D_SHADERTYPE_VS, .shid = 42u,
    };
    SVGA3dCmdSetShaderConst constants = {
        .cid = context.cid, .reg = 7u, .type = SVGA3D_SHADERTYPE_VS,
        .ctype = SVGA3D_CONST_TYPE_FLOAT,
        .values = {1u, 2u, 3u, 4u},
    };
    uint32_t constant_body[sizeof(constants) / sizeof(uint32_t) + 4u];
    memcpy(constant_body, &constants, sizeof(constants));
    constant_body[sizeof(constants) / sizeof(uint32_t) + 0u] = 5u;
    constant_body[sizeof(constants) / sizeof(uint32_t) + 1u] = 6u;
    constant_body[sizeof(constants) / sizeof(uint32_t) + 2u] = 7u;
    constant_body[sizeof(constants) / sizeof(uint32_t) + 3u] = 8u;
    SVGA3dCmdDestroyShader shader_destroy = {
        .cid = context.cid, .shid = 42u, .type = SVGA3D_SHADERTYPE_VS,
    };
    SVGA3dCmdSetTransform bad_tail_transform = {
        .cid = 0xf0000000u, .type = SVGA3D_TRANSFORM_WORLD,
    };
    uint32_t shader_bad_tail[32] = {0};
    uint32_t bad_tail_dwords = test_append_packet(shader_bad_tail, 32, 0,
        SVGA_3D_CMD_SHADER_DEFINE, define_body, sizeof(define_body));
    bad_tail_dwords = test_append_packet(shader_bad_tail, 32, bad_tail_dwords,
        SVGA_3D_CMD_SETTRANSFORM, &bad_tail_transform,
        sizeof(bad_tail_transform));
    uint32_t shader_submit_before_bad_tail = drm_test_batch_submit_calls;
    drm_test_stage_event_count = 0;
    struct drm_vmw_execbuf_arg shader_bad_exec = {
        .commands = (uint64_t)(uintptr_t)shader_bad_tail,
        .command_size = bad_tail_dwords * sizeof(uint32_t),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &shader_bad_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == shader_submit_before_bad_tail &&
           drm_test_stage_event_count == 0u);

    const uint32_t float_single_body[] = {
        context.cid, 9u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT,
        0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u,
    };
    const uint32_t int_single_body[] = {
        context.cid, 2u, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_INT,
        0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    };
    const uint32_t int_multi_body[] = {
        context.cid, 4u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_INT,
        0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
        0x13579bdfu, 0x2468ace0u, 0x0badf00du, 0xfeedc0deu,
    };
    const uint32_t bool_single_body[] = {
        context.cid, 6u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_BOOL,
        1u, 0u, 0u, 0u,
    };
    const uint32_t bool_multi_body[] = {
        context.cid, 8u, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_BOOL,
        0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u,
    };
    const struct {
        uint32_t reg, type, const_type, registers;
        uint32_t values[8];
    } expected_constants[] = {
        {7u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, 2u,
         {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}},
        {9u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT, 1u,
         {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u}},
        {2u, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_INT, 1u,
         {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u}},
        {4u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_INT, 2u,
         {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
          0x13579bdfu, 0x2468ace0u, 0x0badf00du, 0xfeedc0deu}},
        {6u, SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_BOOL, 1u,
         {1u, 0u, 0u, 0u}},
        {8u, SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_BOOL, 2u,
         {0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u}},
    };
    uint32_t shader_stream[128] = {0};
    uint32_t shader_dwords = 0;
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SHADER_DEFINE, define_body, sizeof(define_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER, &shader_bind, sizeof(shader_bind));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, constant_body,
        sizeof(constant_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, float_single_body,
        sizeof(float_single_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, int_single_body,
        sizeof(int_single_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, int_multi_body,
        sizeof(int_multi_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, bool_single_body,
        sizeof(bool_single_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SET_SHADER_CONST, bool_multi_body,
        sizeof(bool_multi_body));
    shader_dwords = test_append_packet(shader_stream, 128, shader_dwords,
        SVGA_3D_CMD_SHADER_DESTROY, &shader_destroy,
        sizeof(shader_destroy));
    drm_test_stage_event_count = 0;
    drm_test_shader_constant_capture_count = 0;
    struct drm_vmw_execbuf_arg shader_exec = {
        .commands = (uint64_t)(uintptr_t)shader_stream,
        .command_size = shader_dwords * sizeof(uint32_t),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &shader_exec) == 0);
    assert(drm_test_stage_event_count == 9u);
    assert(drm_test_stage_events[0].kind == DRM_TEST_STAGE_SHADER_DEFINE &&
           drm_test_stage_events[0].first == 42u &&
           drm_test_stage_events[0].second == sizeof(shader_code));
    assert(drm_test_stage_events[1].kind == DRM_TEST_STAGE_SHADER_BIND &&
           drm_test_stage_events[1].first == 42u);
    assert(drm_test_shader_constant_capture_count ==
           sizeof(expected_constants) / sizeof(expected_constants[0]));
    for (uint32_t i = 0; i < drm_test_shader_constant_capture_count; ++i) {
        const struct drm_test_shader_constant_capture *capture =
            &drm_test_shader_constant_captures[i];
        assert(capture->reg == expected_constants[i].reg &&
               capture->type == expected_constants[i].type &&
               capture->const_type == expected_constants[i].const_type &&
               capture->registers == expected_constants[i].registers);
        assert(memcmp(capture->values, expected_constants[i].values,
                      capture->registers * 4u * sizeof(uint32_t)) == 0);
        assert(drm_test_stage_events[i + 2u].kind ==
                   DRM_TEST_STAGE_SHADER_CONSTANTS &&
               drm_test_stage_events[i + 2u].first ==
                   expected_constants[i].reg &&
               drm_test_stage_events[i + 2u].second ==
                   expected_constants[i].registers);
    }
    assert(drm_test_stage_events[8].kind == DRM_TEST_STAGE_SHADER_DESTROY &&
           drm_test_stage_events[8].first == 42u);

    /* Query result storage is an authorized BO/GMR and stays reserved through
     * the real batch fence, even if its user handle closes while SYNCCPU is
     * parked. */
    union drm_vmw_alloc_bo_arg query_bo = {0};
    query_bo.req.size = 4096;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_ALLOC_BO,
                      &query_bo) == 0);
    struct drm_bo *query_bo_pin = drm_bo_lookup_handle_get(
        client.drm_file, query_bo.rep.handle);
    assert(query_bo_pin);
    SVGA3dCmdBeginQuery query_begin = {
        .cid = context.cid, .type = SVGA3D_QUERYTYPE_OCCLUSION,
    };
    SVGA3dCmdEndQuery query_end = {
        .cid = context.cid,
        .type = SVGA3D_QUERYTYPE_OCCLUSION,
        .guestResult = {.gmrId = query_bo.rep.handle, .offset = 0},
    };
    SVGA3dCmdEndQuery invalid_query_end = query_end;
    invalid_query_end.guestResult.offset = 4088u;
    uint32_t invalid_query_stream[4 +
        sizeof(query_begin) / sizeof(uint32_t) +
        sizeof(invalid_query_end) / sizeof(uint32_t)] = {
        SVGA_3D_CMD_BEGIN_QUERY, sizeof(query_begin),
    };
    memcpy(&invalid_query_stream[2], &query_begin, sizeof(query_begin));
    uint32_t invalid_end_header =
        2u + sizeof(query_begin) / sizeof(uint32_t);
    invalid_query_stream[invalid_end_header] = SVGA_3D_CMD_END_QUERY;
    invalid_query_stream[invalid_end_header + 1u] =
        sizeof(invalid_query_end);
    memcpy(&invalid_query_stream[invalid_end_header + 2u],
           &invalid_query_end, sizeof(invalid_query_end));
    uint32_t invalid_query_submit_before = drm_test_batch_submit_calls;
    uint32_t invalid_query_fence_before = drm_test_fence_issued;
    uint32_t invalid_query_attachment_before = drm_test_borrowed_count;
    struct drm_vmw_execbuf_arg invalid_query_exec = {
        .commands = (uint64_t)(uintptr_t)invalid_query_stream,
        .command_size = sizeof(invalid_query_stream),
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &invalid_query_exec) == -LINUX_EINVAL);
    assert(drm_test_batch_submit_calls == invalid_query_submit_before &&
           drm_test_fence_issued == invalid_query_fence_before &&
           drm_test_borrowed_count == invalid_query_attachment_before);
    uint32_t query_stream[4 +
        sizeof(query_begin) / sizeof(uint32_t) +
        sizeof(query_end) / sizeof(uint32_t)] = {
        SVGA_3D_CMD_BEGIN_QUERY, sizeof(query_begin),
    };
    memcpy(&query_stream[2], &query_begin, sizeof(query_begin));
    uint32_t end_header = 2 + sizeof(query_begin) / sizeof(uint32_t);
    query_stream[end_header] = SVGA_3D_CMD_END_QUERY;
    query_stream[end_header + 1u] = sizeof(query_end);
    memcpy(&query_stream[end_header + 2u], &query_end, sizeof(query_end));
    struct drm_vmw_fence_rep query_reply = {0};
    struct drm_vmw_execbuf_arg query_exec = {
        .commands = (uint64_t)(uintptr_t)query_stream,
        .command_size = sizeof(query_stream),
        .fence_rep = (uint64_t)(uintptr_t)&query_reply,
        .version = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &query_exec) == 0);
    assert(query_reply.seqno == drm_test_fence_issued &&
           drm_test_borrowed_count == invalid_query_attachment_before + 1u);

    struct drm_vmw_synccpu_arg sync = {
        .op = drm_vmw_synccpu_grab,
        .flags = drm_vmw_synccpu_read | drm_vmw_synccpu_dontblock,
        .handle = query_bo.rep.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == -LINUX_EBUSY);
    sync.flags = drm_vmw_synccpu_read;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == KERNEL_SYSCALL_BLOCKED);
    assert(client_task.drm_vmwgfx_continuation != NULL);

    /* A generation change alone is not a completion proof. Fill the global
     * work budget with the still-pending query plus empty jobs, then verify
     * that stale-fence retirement keeps both the BO reservation and budget
     * charged until an explicit old-generation quiescence proof arrives. */
    uint32_t query_generation = drm_test_svga_info.generation;
    uint32_t query_sequence = query_reply.seqno;
    drm_test_fence_passed = query_sequence - 1u;
    while (drm_vmwgfx_exec_work_count > 1u)
        drm_vmwgfx_execbuf_process_retirements(8);
    assert(drm_vmwgfx_exec_work_count == 1u &&
           drm_bo_gpu_busy(query_bo_pin));
    struct task reset_budget_task = make_task(905, false);
    while (drm_vmwgfx_exec_work_count < DRM_VMW_EXEC_WORK_MAX) {
        struct drm_vmw_execbuf_arg pending = {.version = 1};
        assert(ioctl_call(&reset_budget_task, &client, DRM_IOCTL_VMW_EXECBUF,
                          &pending) == 0);
    }
    uint32_t reset_last_sequence = drm_test_fence_issued;
    uint32_t reset_work_bytes = drm_vmwgfx_exec_work_bytes;
    struct drm_vmw_execbuf_arg over_reset_limit = {.version = 1};
    assert(ioctl_call(&reset_budget_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &over_reset_limit) == -LINUX_ENOMEM);

    ++drm_test_svga_info.generation;
    drm_test_fence_issued = 0;
    drm_test_fence_passed = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_bo_gpu_busy(query_bo_pin));
    drm_vmwgfx_execbuf_process_retirements(1);
    assert(drm_vmwgfx_exec_work_count == DRM_VMW_EXEC_WORK_MAX &&
           drm_vmwgfx_exec_work_bytes == reset_work_bytes &&
           drm_bo_gpu_busy(query_bo_pin));
    struct task stale_synccpu_probe = make_task(904, false);
    assert(ioctl_call(&stale_synccpu_probe, &client, DRM_IOCTL_VMW_SYNCCPU,
        &(struct drm_vmw_synccpu_arg){
            .op = drm_vmw_synccpu_grab,
            .flags = drm_vmw_synccpu_read | drm_vmw_synccpu_dontblock,
            .handle = query_bo.rep.handle,
        }) == -LINUX_EBUSY);
    assert(ioctl_call(&reset_budget_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &over_reset_limit) == -LINUX_ENOMEM);
    assert(drm_vmwgfx_exec_work_count == DRM_VMW_EXEC_WORK_MAX &&
           drm_vmwgfx_exec_work_bytes == reset_work_bytes);

    assert(ioctl_call(&owner_task, &client, DRM_IOCTL_VMW_UNREF_BO,
        &(struct drm_vmw_handle_close_arg){
            .handle = query_bo.rep.handle,
        }) == 0);
    sync.handle = UINT32_MAX; /* Internal retry must use its pinned snapshot. */
    drm_test_quiesced_generation = query_generation;
    drm_test_quiesced_seq = reset_last_sequence;
    while (drm_vmwgfx_exec_work_count)
        drm_vmwgfx_execbuf_process_retirements(8);
    assert(!drm_bo_gpu_busy(query_bo_pin));
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_SYNCCPU,
                      &sync) == 0);
    assert(drm_vmwgfx_exec_work_bytes == 0u);
    assert(client_task.drm_vmwgfx_continuation == NULL);
    drm_bo_put(query_bo_pin);

    /* A parked FENCE_WAIT owns an immutable argument snapshot. A changed
     * userspace handle is ignored by its internal retry; the cookie is copied
     * back before the fence is advanced. */
    struct drm_vmw_fence_rep wait_reply = {0};
    struct drm_vmw_execbuf_arg wait_exec = {
        .version = 1,
        .fence_rep = (uint64_t)(uintptr_t)&wait_reply,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &wait_exec) == 0);
    struct drm_vmw_fence_wait_arg wait = {
        .handle = wait_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
        .flags = 0x7fffffff,
        .pad64 = 0x7fffffff,
    };
    uint32_t wait_sequence = wait_reply.seqno;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &wait) == KERNEL_SYSCALL_BLOCKED);
    assert(wait.cookie_valid && wait.kernel_cookie == drm_test_ticks + 500u);
    wait.handle = UINT32_MAX;
    drm_test_fence_passed = wait_sequence;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &wait) == 0);
    assert(wait.handle == wait_reply.handle);

    /* Non-lazy waits request a normal scheduler decision on both the initial
     * block and each pending continuation retry. The retained snapshot and
     * success-only UNREF behavior remain unchanged. */
    const int64_t saved_clock_resolution = drm_test_clock_resolution_ns;
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_rep nonlazy_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = nonlazy_reply.seqno - 1u;
    struct drm_vmw_fence_wait_arg nonlazy_wait = {
        .handle = nonlazy_reply.handle,
        .timeout_us = 5000000,
        .lazy = 0,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    uint32_t nonlazy_sleeps_before = sleep_current_calls;
    uint32_t nonlazy_reschedules_before = reschedule_current_calls;
    reschedule_expected_task = &client_task;
    reschedule_expected_handle = nonlazy_reply.handle;
    reschedule_expected_lazy = 0;
    reschedule_expected_deadline = drm_test_ticks + 500u;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &nonlazy_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_task_continuation *nonlazy_continuation =
        client_task.drm_vmwgfx_continuation;
    assert(nonlazy_continuation &&
           nonlazy_continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
           nonlazy_continuation->snapshot.wait.lazy == 0 &&
           nonlazy_continuation->snapshot.wait.handle == nonlazy_reply.handle &&
           nonlazy_continuation->deadline == nonlazy_wait.kernel_cookie);
    assert(reschedule_current_calls == nonlazy_reschedules_before + 1u &&
           sleep_current_calls == nonlazy_sleeps_before);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &nonlazy_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(client_task.drm_vmwgfx_continuation == nonlazy_continuation &&
           nonlazy_continuation->snapshot.wait.lazy == 0 &&
           nonlazy_continuation->deadline == nonlazy_wait.kernel_cookie);
    assert(reschedule_current_calls == nonlazy_reschedules_before + 2u &&
           sleep_current_calls == nonlazy_sleeps_before);
    drm_test_fence_passed = nonlazy_reply.seqno;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &nonlazy_wait) == 0);
    assert(!client_task.drm_vmwgfx_continuation &&
           reschedule_current_calls == nonlazy_reschedules_before + 2u &&
           sleep_current_calls == nonlazy_sleeps_before);
    reschedule_expected_task = NULL;
    struct drm_vmw_fence_signaled_arg nonlazy_removed = {
        .handle = nonlazy_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &nonlazy_removed) == -LINUX_EINVAL);

    /* Lazy waits retain the one-tick sleep on both initial and resumed
     * pending attempts, without also requesting an immediate reschedule. */
    struct drm_vmw_fence_rep lazy_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = lazy_reply.seqno - 1u;
    struct drm_vmw_fence_wait_arg lazy_wait = {
        .handle = lazy_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    uint32_t lazy_sleeps_before = sleep_current_calls;
    uint32_t lazy_reschedules_before = reschedule_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &lazy_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_task_continuation *lazy_continuation =
        client_task.drm_vmwgfx_continuation;
    assert(lazy_continuation &&
           lazy_continuation->snapshot.wait.lazy == 1);
    assert(sleep_current_calls == lazy_sleeps_before + 1u &&
           sleep_current_deadline == drm_test_ticks + 1u &&
           reschedule_current_calls == lazy_reschedules_before);
    uint32_t lazy_suspends_before_retry = drm_test_suspend_calls;
    uint32_t lazy_unlocked_polls_before_retry =
        drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &lazy_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(client_task.drm_vmwgfx_continuation == lazy_continuation &&
           sleep_current_calls == lazy_sleeps_before + 2u &&
           sleep_current_deadline == drm_test_ticks + 1u &&
           reschedule_current_calls == lazy_reschedules_before &&
           drm_test_suspend_calls == lazy_suspends_before_retry &&
           drm_test_unlocked_fence_status_calls ==
               lazy_unlocked_polls_before_retry);
    drm_test_fence_passed = lazy_reply.seqno;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &lazy_wait) == 0);
    assert(!client_task.drm_vmwgfx_continuation &&
           sleep_current_calls == lazy_sleeps_before + 2u &&
           reschedule_current_calls == lazy_reschedules_before);
    struct drm_vmw_fence_signaled_arg lazy_removed = {
        .handle = lazy_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &lazy_removed) == -LINUX_EINVAL);

    /* A resumed non-lazy retry can complete in the bounded unlocked poll,
     * while retaining its original timeout cookie and success-only UNREF. */
    struct drm_vmw_fence_rep resumed_poll_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_poll_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_poll_wait = {
        .handle = resumed_poll_reply.handle,
        .timeout_us = 5000000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    reschedule_expected_handle = resumed_poll_reply.handle;
    reschedule_expected_lazy = 0;
    reschedule_expected_deadline = drm_test_ticks + 500u;
    reschedule_expected_task = &client_task;
    uint32_t resumed_poll_reschedules_before = reschedule_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_poll_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_task_continuation *resumed_poll_continuation =
        client_task.drm_vmwgfx_continuation;
    struct drm_vmwgfx_fence *resumed_poll_fence =
        resumed_poll_continuation
            ? resumed_poll_continuation->fence_object : NULL;
    uint64_t resumed_poll_deadline = resumed_poll_continuation
        ? resumed_poll_continuation->deadline : 0;
    assert(resumed_poll_continuation &&
           resumed_poll_continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
           resumed_poll_continuation->snapshot.wait.handle ==
               resumed_poll_reply.handle &&
           resumed_poll_continuation->deadline ==
               resumed_poll_wait.kernel_cookie &&
           reschedule_current_calls == resumed_poll_reschedules_before + 1u);

    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(9000, 0);
    drm_test_uptime_script[0] = 9000;
    drm_test_uptime_script[1] = 9000;
    drm_test_uptime_script[2] = 9010;
    drm_test_uptime_script_count = 3;
    resumed_poll_wait.handle = UINT32_MAX;
    uint32_t resumed_poll_status_before = drm_test_fence_status_calls;
    uint32_t resumed_poll_unlocked_before =
        drm_test_unlocked_fence_status_calls;
    uint32_t resumed_poll_suspend_before = drm_test_suspend_calls;
    uint32_t resumed_poll_resume_before = drm_test_resume_calls;
    uint32_t resumed_poll_sleep_before = sleep_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_poll_wait) == 0);
    assert(resumed_poll_wait.handle == resumed_poll_reply.handle &&
           resumed_poll_wait.cookie_valid &&
           resumed_poll_wait.kernel_cookie ==
               resumed_poll_deadline);
    assert(drm_test_fence_status_calls == resumed_poll_status_before + 2u &&
           drm_test_unlocked_fence_status_calls ==
               resumed_poll_unlocked_before + 1u &&
           drm_test_suspend_calls == resumed_poll_suspend_before + 1u &&
           drm_test_resume_calls == resumed_poll_resume_before + 1u &&
           sleep_current_calls == resumed_poll_sleep_before &&
           reschedule_current_calls == resumed_poll_reschedules_before + 1u &&
           !client_task.drm_vmwgfx_continuation &&
           resumed_poll_fence && resumed_poll_fence->references == 1u &&
           !resumed_poll_fence->user_linked &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    struct drm_vmw_fence_signaled_arg resumed_poll_removed = {
        .handle = resumed_poll_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &resumed_poll_removed) == -LINUX_EINVAL);
    reschedule_expected_task = NULL;

    /* A hard error found by a resumed poll releases the retained continuation
     * without applying the wait's success-only UNREF option. */
    struct drm_vmw_fence_rep resumed_error_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_error_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_error_wait = {
        .handle = resumed_error_reply.handle,
        .timeout_us = 5000000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_error_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_fence *resumed_error_fence =
        ((struct drm_vmwgfx_task_continuation *)
            client_task.drm_vmwgfx_continuation)->fence_object;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(SVGA_EIO, false);
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(10000, 0);
    drm_test_uptime_script[0] = 10000;
    drm_test_uptime_script[1] = 10000;
    drm_test_uptime_script[2] = 10010;
    drm_test_uptime_script_count = 3;
    uint32_t resumed_error_unlocked_before =
        drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_error_wait) == -LINUX_EIO);
    assert(drm_test_unlocked_fence_status_calls ==
               resumed_error_unlocked_before + 1u &&
           !client_task.drm_vmwgfx_continuation &&
           resumed_error_fence->references == 2u &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    struct drm_vmw_fence_signaled_arg resumed_error_signaled = {
        .handle = resumed_error_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &resumed_error_signaled) == 0 &&
           !resumed_error_signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){
            .handle = resumed_error_reply.handle}) == 0);
    assert(resumed_error_fence->references == 1u);

    /* Coarse clock and suspend refusal retain the existing locked-sample
     * fallback and never leave ownership or IRQ state changed. */
    struct drm_vmw_fence_rep resumed_fallback_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_fallback_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_fallback_wait = {
        .handle = resumed_fallback_reply.handle,
        .timeout_us = 5000000,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_fallback_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_fence *resumed_fallback_fence =
        ((struct drm_vmwgfx_task_continuation *)
            client_task.drm_vmwgfx_continuation)->fence_object;
    drm_test_clock_resolution_ns = 1000;
    drm_test_clock_getres_status = -1;
    uint32_t resumed_fallback_status_before = drm_test_fence_status_calls;
    uint32_t resumed_fallback_unlocked_before =
        drm_test_unlocked_fence_status_calls;
    uint32_t resumed_fallback_suspend_before = drm_test_suspend_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_fallback_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_fence_status_calls ==
               resumed_fallback_status_before + 1u &&
           drm_test_unlocked_fence_status_calls ==
               resumed_fallback_unlocked_before &&
           drm_test_suspend_calls == resumed_fallback_suspend_before &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_test_clock_getres_status = 0;
    drm_test_suspend_status = -35;
    resumed_fallback_status_before = drm_test_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_fallback_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_fence_status_calls ==
               resumed_fallback_status_before + 1u &&
           drm_test_unlocked_fence_status_calls ==
               resumed_fallback_unlocked_before &&
           drm_test_suspend_calls == resumed_fallback_suspend_before + 1u &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_test_suspend_status = 0;
    drm_vmwgfx_cancel_task_continuation(&client_task);
    drm_vmwgfx_execbuf_process_retirements(1);
    assert(resumed_fallback_fence->references == 2u);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){
            .handle = resumed_fallback_reply.handle}) == 0);
    assert(resumed_fallback_fence->references == 1u);

    /* With a stopped fine clock the resumed batch is bounded by its hard
     * sample cap and returns through the original non-lazy reschedule path. */
    struct drm_vmw_fence_rep resumed_cap_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_cap_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_cap_wait = {
        .handle = resumed_cap_reply.handle,
        .timeout_us = 5000000,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_cap_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_task_continuation *resumed_cap_continuation =
        client_task.drm_vmwgfx_continuation;
    struct drm_vmwgfx_fence *resumed_cap_fence =
        resumed_cap_continuation->fence_object;
    uint64_t resumed_cap_deadline = resumed_cap_wait.kernel_cookie;
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(11000, 0);
    uint32_t resumed_cap_unlocked_before =
        drm_test_unlocked_fence_status_calls;
    uint32_t resumed_cap_reschedules_before = reschedule_current_calls;
    reschedule_expected_task = &client_task;
    reschedule_expected_handle = resumed_cap_reply.handle;
    reschedule_expected_lazy = 0;
    reschedule_expected_deadline = resumed_cap_deadline;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_cap_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(client_task.drm_vmwgfx_continuation ==
               resumed_cap_continuation &&
           resumed_cap_continuation->deadline == resumed_cap_deadline &&
           drm_test_unlocked_fence_status_calls ==
               resumed_cap_unlocked_before + 8192u &&
           reschedule_current_calls == resumed_cap_reschedules_before + 1u &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    reschedule_expected_task = NULL;
    drm_vmwgfx_cancel_task_continuation(&client_task);
    drm_vmwgfx_execbuf_process_retirements(1);
    assert(resumed_cap_fence->references == 2u);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = resumed_cap_reply.handle}) == 0);
    assert(resumed_cap_fence->references == 1u);

    /* UNREF after a resumed poll is still tied to the retained object when
     * its numeric handle is closed and reused during the unlocked sample. */
    struct drm_vmw_fence_rep resumed_reuse_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_reuse_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_reuse_wait = {
        .handle = resumed_reuse_reply.handle,
        .timeout_us = 5000000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_reuse_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_fence *resumed_reuse_fence =
        ((struct drm_vmwgfx_task_continuation *)
            client_task.drm_vmwgfx_continuation)->fence_object;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(12000, 0);
    drm_test_uptime_script[0] = 12000;
    drm_test_uptime_script[1] = 12000;
    drm_test_uptime_script[2] = 12010;
    drm_test_uptime_script_count = 3;
    drm_test_replace_fence_file = client.drm_file;
    drm_test_replace_fence_handle = resumed_reuse_reply.handle;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_reuse_wait) == 0);
    assert(!drm_test_replace_fence_file && !drm_test_replace_fence_handle &&
           !client_task.drm_vmwgfx_continuation &&
           resumed_reuse_fence->references == 1u &&
           !resumed_reuse_fence->user_linked);
    struct drm_vmw_fence_signaled_arg resumed_reuse_signaled = {
        .handle = resumed_reuse_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &resumed_reuse_signaled) == 0 &&
           !resumed_reuse_signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){
            .handle = resumed_reuse_reply.handle}) == 0);

    /* If cancellation replaces the continuation while the status sample is
     * unlocked, the resumed poll must not release or rewrite that new object. */
    struct drm_vmw_fence_rep resumed_replace_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_replace_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_replace_wait = {
        .handle = resumed_replace_reply.handle,
        .timeout_us = 5000000,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_replace_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_fence *resumed_replace_fence =
        ((struct drm_vmwgfx_task_continuation *)
            client_task.drm_vmwgfx_continuation)->fence_object;
    struct drm_vmwgfx_task_continuation *replacement_continuation =
        kernel_malloc(sizeof(*replacement_continuation));
    assert(replacement_continuation);
    *replacement_continuation = (struct drm_vmwgfx_task_continuation){
        .kind = DRM_VMW_CONT_SYNCCPU,
    };
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(13000, 0);
    drm_test_uptime_script[0] = 13000;
    drm_test_uptime_script[1] = 13000;
    drm_test_uptime_script[2] = 13010;
    drm_test_uptime_script_count = 3;
    drm_test_replacement_continuation = replacement_continuation;
    drm_test_replace_continuation_task = &client_task;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_replace_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(!drm_test_replacement_continuation &&
           !drm_test_replace_continuation_task &&
           client_task.drm_vmwgfx_continuation ==
               replacement_continuation &&
           replacement_continuation->kind == DRM_VMW_CONT_SYNCCPU &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    drm_vmwgfx_execbuf_process_retirements(8);
    assert(resumed_replace_fence->references == 2u);
    struct drm_vmw_fence_signaled_arg resumed_replace_signaled = {
        .handle = resumed_replace_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &resumed_replace_signaled) == 0 &&
           !resumed_replace_signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){
            .handle = resumed_replace_reply.handle}) == 0);
    assert(resumed_replace_fence->references == 1u);

    /* A remote exit can change task state during suspension without freeing
     * the running task's resources; the resumed path leaves cleanup to the
     * normal scheduler boundary and releases only its local fence pin. */
    struct drm_vmw_fence_rep resumed_exit_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = resumed_exit_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg resumed_exit_wait = {
        .handle = resumed_exit_reply.handle,
        .timeout_us = 5000000,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_exit_wait) == KERNEL_SYSCALL_BLOCKED);
    struct drm_vmwgfx_task_continuation *resumed_exit_continuation =
        client_task.drm_vmwgfx_continuation;
    struct drm_vmwgfx_fence *resumed_exit_fence =
        resumed_exit_continuation->fence_object;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_clock_resolution_ns = 1000;
    drm_test_uptime_reset(14000, 0);
    drm_test_uptime_script[0] = 14000;
    drm_test_uptime_script[1] = 14000;
    drm_test_uptime_script[2] = 14010;
    drm_test_uptime_script_count = 3;
    enum task_state resumed_exit_prior_state = client_task.state;
    drm_test_exit_task_during_unlocked_poll = &client_task;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &resumed_exit_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(!drm_test_exit_task_during_unlocked_poll &&
           client_task.state == TASK_EXITED &&
           client_task.drm_vmwgfx_continuation ==
               resumed_exit_continuation &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    client_task.state = resumed_exit_prior_state;
    drm_vmwgfx_cancel_task_continuation(&client_task);
    drm_vmwgfx_execbuf_process_retirements(1);
    assert(resumed_exit_fence->references == 2u);
    struct drm_vmw_fence_signaled_arg resumed_exit_signaled = {
        .handle = resumed_exit_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &resumed_exit_signaled) == 0 &&
           !resumed_exit_signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){
            .handle = resumed_exit_reply.handle}) == 0);
    assert(resumed_exit_fence->references == 1u);

    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = saved_clock_resolution;

    /* A zero timeout is an immediate completion check. The timeout cookie
     * returned on EBUSY is reused by an explicit successful UNREF wait. */
    struct drm_vmw_fence_rep immediate_reply = {0};
    struct drm_vmw_execbuf_arg immediate_exec = {
        .version = 1,
        .fence_rep = (uint64_t)(uintptr_t)&immediate_reply,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &immediate_exec) == 0);
    struct drm_vmw_fence_wait_arg immediate_wait = {
        .handle = immediate_reply.handle,
        .timeout_us = 0,
    };
    uint32_t suspend_calls_before_zero = drm_test_suspend_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &immediate_wait) == -LINUX_EBUSY);
    assert(drm_test_suspend_calls == suspend_calls_before_zero);
    assert(immediate_wait.cookie_valid &&
           immediate_wait.kernel_cookie == drm_test_ticks);
    drm_test_fence_passed = immediate_reply.seqno;
    immediate_wait.wait_options = DRM_VMW_WAIT_OPTION_UNREF;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &immediate_wait) == 0);
    assert(drm_test_suspend_calls == suspend_calls_before_zero);
    struct drm_vmw_fence_signaled_arg removed_signaled = {
        .handle = immediate_reply.handle,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &removed_signaled) == -LINUX_EINVAL);

    /* Signal handoff cancels only the wait snapshot. A restarted ioctl reads
     * its fresh userspace arguments; the committed fence remains usable. */
    struct drm_vmw_fence_rep restart_reply = {0};
    struct drm_vmw_execbuf_arg restart_exec = {
        .version = 1,
        .fence_rep = (uint64_t)(uintptr_t)&restart_reply,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &restart_exec) == 0);
    struct drm_vmw_fence_wait_arg restart_wait = {
        .handle = restart_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &restart_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(client_task.drm_vmwgfx_continuation != NULL &&
           restart_wait.cookie_valid);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(client_task.drm_vmwgfx_continuation == NULL);
    drm_vmwgfx_execbuf_process_retirements(1);
    restart_wait.handle = UINT32_MAX;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &restart_wait) == -LINUX_EINVAL);
    restart_wait.handle = restart_reply.handle;
    drm_test_fence_passed = restart_reply.seqno;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &restart_wait) == 0);

    /* A conclusive in-flight status result wins even if the measured window
     * crosses 500 us. Reusing its numeric handle during the unlocked phase
     * must leave lookup_get's reference alive and protect the replacement
     * from the old wait's success-only UNREF. */
    struct drm_vmw_fence_rep fast_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = fast_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_clock_getres_status = 0;
    drm_test_clock_resolution_ns = 1000;
    drm_test_suspend_status = 0;
    drm_test_resume_status = 0;
    drm_test_uptime_reset(1000, 0);
    drm_test_uptime_script[0] = 1000;
    drm_test_uptime_script[1] = 1000;
    drm_test_uptime_script[2] = 1501;
    drm_test_uptime_script_count = 3;
    drm_test_replace_fence_file = client.drm_file;
    drm_test_replace_fence_handle = fast_reply.handle;
    struct drm_vmw_fence_wait_arg fast_wait = {
        .handle = fast_reply.handle,
        .timeout_us = 5000000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    uint32_t fast_sleeps_before = sleep_current_calls;
    uint32_t fast_suspends_before = drm_test_suspend_calls;
    uint32_t fast_resumes_before = drm_test_resume_calls;
    uint32_t fast_polls_before = drm_test_unlocked_fence_status_calls;
    uint32_t fast_clock_getres_before = drm_test_clock_getres_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &fast_wait) == 0);
    assert(sleep_current_calls == fast_sleeps_before);
    assert(drm_test_suspend_calls == fast_suspends_before + 1u &&
           drm_test_resume_calls == fast_resumes_before + 1u);
    assert(drm_test_clock_getres_calls == fast_clock_getres_before + 1u);
    assert(drm_test_unlocked_fence_status_calls == fast_polls_before + 1u);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled &&
           !client_task.drm_vmwgfx_continuation);
    assert(!drm_test_replace_fence_file);
    signaled.handle = fast_reply.handle;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &signaled) == 0 && !signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = fast_reply.handle}) == 0);

    /* A fence completing after a few 100-us samples but within the 500-us
     * window completes without entering the one-tick fallback. */
    struct drm_vmw_fence_rep midwindow_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = midwindow_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_uptime_reset(2500, 0);
    drm_test_uptime_script[0] = 2500;
    drm_test_uptime_script[1] = 2500;
    drm_test_uptime_script[2] = 2600;
    drm_test_uptime_script[3] = 2700;
    drm_test_uptime_script[4] = 2800;
    drm_test_uptime_script[5] = 2800;
    drm_test_uptime_script[6] = 2850;
    drm_test_uptime_script_count = 7;
    struct drm_vmw_fence_wait_arg midwindow_wait = {
        .handle = midwindow_reply.handle,
        .timeout_us = 5000000,
    };
    fast_sleeps_before = sleep_current_calls;
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &midwindow_wait) == 0);
    assert(sleep_current_calls == fast_sleeps_before &&
           drm_test_unlocked_fence_status_calls == fast_polls_before + 3u);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = midwindow_reply.handle}) == 0);

    /* SVGA lock contention is an inconclusive poll, so a subsequent pass in
     * the same bounded phase completes without the one-tick sleep. */
    struct drm_vmw_fence_rep contention_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = contention_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(SVGA_EBUSY, false);
    drm_test_fence_status_script_push(0, true);
    drm_test_uptime_reset(2000, 0);
    struct drm_vmw_fence_wait_arg contention_wait = {
        .handle = contention_reply.handle,
        .timeout_us = 5000000,
    };
    fast_sleeps_before = sleep_current_calls;
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &contention_wait) == 0);
    assert(sleep_current_calls == fast_sleeps_before &&
           drm_test_unlocked_fence_status_calls == fast_polls_before + 2u);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = contention_reply.handle}) == 0);

    /* A pending sample at the window boundary stops further polling while
     * retaining the original blocking continuation behavior. */
    struct drm_vmw_fence_rep budget_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = budget_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(0, false);
    drm_test_uptime_reset(4000, 0);
    drm_test_uptime_script[0] = 4000;
    drm_test_uptime_script[1] = 4000;
    drm_test_uptime_script[2] = 4501;
    drm_test_uptime_script_count = 3;
    struct drm_vmw_fence_wait_arg budget_wait = {
        .handle = budget_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    fast_sleeps_before = sleep_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &budget_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_unlocked_fence_status_calls == fast_polls_before + 1u &&
           sleep_current_calls == fast_sleeps_before + 1u &&
           client_task.drm_vmwgfx_continuation);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = budget_reply.handle}) == 0);

    /* Hard errors after an unlocked poll resume ownership and preserve the
     * user fence even when UNREF was requested. */
    struct drm_vmw_fence_rep error_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = error_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_fence_status_script_push(0, false);
    drm_test_fence_status_script_push(SVGA_EIO, false);
    drm_test_uptime_reset(3000, 0);
    struct drm_vmw_fence_wait_arg error_wait = {
        .handle = error_reply.handle,
        .timeout_us = 5000000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    fast_sleeps_before = sleep_current_calls;
    fast_resumes_before = drm_test_resume_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &error_wait) == -LINUX_EIO);
    assert(sleep_current_calls == fast_sleeps_before &&
           drm_test_resume_calls == fast_resumes_before + 1u);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled &&
           !client_task.drm_vmwgfx_continuation);
    signaled.handle = error_reply.handle;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_SIGNALED,
                      &signaled) == 0 && !signaled.signaled);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = error_reply.handle}) == 0);

    /* A backward fine-clock sample is bounded and falls back through the
     * retained continuation after reacquiring ownership. */
    struct drm_vmw_fence_rep clock_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = clock_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_uptime_reset(0, 0);
    drm_test_uptime_script[0] = 5000;
    drm_test_uptime_script[1] = 4999;
    drm_test_uptime_script_count = 2;
    struct drm_vmw_fence_wait_arg clock_wait = {
        .handle = clock_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    fast_sleeps_before = sleep_current_calls;
    fast_resumes_before = drm_test_resume_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &clock_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_unlocked_fence_status_calls <= fast_polls_before + 1u);
    assert(drm_test_resume_calls == fast_resumes_before + 1u &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    assert(sleep_current_calls == fast_sleeps_before + 1u &&
           client_task.drm_vmwgfx_continuation);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = clock_reply.handle}) == 0);

    /* A progressing clock reaches the 500-us window well before the sample
     * ceiling, even when status never passes. */
    struct drm_vmw_fence_rep capped_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = capped_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_uptime_reset(6000, 1);
    struct drm_vmw_fence_wait_arg capped_wait = {
        .handle = capped_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    fast_sleeps_before = sleep_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &capped_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_unlocked_fence_status_calls > fast_polls_before + 64u &&
           drm_test_unlocked_fence_status_calls < fast_polls_before + 8192u &&
           drm_test_uptime_value >= 6500u);
    assert(sleep_current_calls == fast_sleeps_before + 1u &&
           client_task.drm_vmwgfx_continuation &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = capped_reply.handle}) == 0);

    /* A stopped clock cannot keep the unlocked poll running past the hard
     * sample ceiling; fallback still retains the original continuation. */
    struct drm_vmw_fence_rep stopped_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = stopped_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_uptime_reset(7000, 0);
    struct drm_vmw_fence_wait_arg stopped_wait = {
        .handle = stopped_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    fast_sleeps_before = sleep_current_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &stopped_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_unlocked_fence_status_calls == fast_polls_before + 8192u &&
           sleep_current_calls == fast_sleeps_before + 1u &&
           client_task.drm_vmwgfx_continuation &&
           drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = stopped_reply.handle}) == 0);

    /* Coarse clocks and failed suspension skip polling and use the existing
     * continuation/sleep semantics with ownership and IRQ state untouched. */
    struct drm_vmw_fence_rep coarse_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = coarse_reply.seqno - 1u;
    drm_test_fence_status_script_reset();
    drm_test_clock_resolution_ns = 10000000;
    struct drm_vmw_fence_wait_arg coarse_wait = {
        .handle = coarse_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_suspends_before = drm_test_suspend_calls;
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &coarse_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_suspend_calls == fast_suspends_before &&
           drm_test_unlocked_fence_status_calls == fast_polls_before);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = coarse_reply.handle}) == 0);

    struct drm_vmw_fence_rep suspend_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = suspend_reply.seqno - 1u;
    drm_test_clock_resolution_ns = 1000;
    drm_test_suspend_status = -35;
    struct drm_vmw_fence_wait_arg suspend_wait = {
        .handle = suspend_reply.handle,
        .timeout_us = 5000000,
        .lazy = 1,
    };
    fast_suspends_before = drm_test_suspend_calls;
    fast_resumes_before = drm_test_resume_calls;
    fast_polls_before = drm_test_unlocked_fence_status_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &suspend_wait) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_suspend_calls == fast_suspends_before + 1u &&
           drm_test_resume_calls == fast_resumes_before &&
           drm_test_unlocked_fence_status_calls == fast_polls_before);
    assert(drm_test_execution_owned && !drm_test_interrupts_enabled);
    drm_vmwgfx_cancel_task_continuation(&client_task);
    drm_test_suspend_status = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = suspend_reply.handle}) == 0);

    /* Expired cookies do not suspend or enter the polling section. */
    struct drm_vmw_fence_rep expired_reply =
        drm_test_submit_wait_fence(&client_task, &client);
    drm_test_fence_passed = expired_reply.seqno - 1u;
    struct drm_vmw_fence_wait_arg expired_wait = {
        .handle = expired_reply.handle,
        .cookie_valid = 1,
        .kernel_cookie = drm_test_ticks - 1u,
    };
    fast_suspends_before = drm_test_suspend_calls;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_WAIT,
                      &expired_wait) == -LINUX_EBUSY);
    assert(drm_test_suspend_calls == fast_suspends_before);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_FENCE_UNREF,
        &(struct drm_vmw_fence_arg){.handle = expired_reply.handle}) == 0);

    drm_test_fence_status_script_reset();
    drm_test_uptime_reset(0, 0);
    drm_test_clock_resolution_ns = 1000;

    /* The reply-copy fault continuation never resubmits and returns EFAULT
     * only after its fence has passed, even though the bad reply page is now
     * repaired. */
    struct drm_vmw_fence_rep fault_reply = {0};
    struct drm_vmw_execbuf_arg fault_exec = {
        .version = 1,
        .fence_rep = (uint64_t)(uintptr_t)&fault_reply,
    };
    fail_copyout_address = (uint64_t)(uintptr_t)&fault_reply;
    uint32_t fault_issued_before = drm_test_fence_issued;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &fault_exec) == KERNEL_SYSCALL_BLOCKED);
    uint32_t fault_sequence = drm_test_fence_issued;
    assert(fault_sequence == fault_issued_before + 1u);
    fail_copyout_address = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &fault_exec) == KERNEL_SYSCALL_BLOCKED);
    assert(drm_test_fence_issued == fault_sequence);
    drm_test_fence_passed = fault_sequence;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                      &fault_exec) == -LINUX_EFAULT);
    assert(drm_test_fence_issued == fault_sequence);

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &context) == 0);
    struct drm_vmw_surface_arg exact_vertex_unref = {
        .sid = (uint32_t)exact_vertex_buffer.rep.sid,
        .handle_type = DRM_VMW_HANDLE_LEGACY,
    };
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_UNREF_SURFACE,
                      &exact_vertex_unref) == 0);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_VMW_UNREF_CONTEXT,
                      &foreign_context) == 0);

    exec.version = 2;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF, &exec) ==
           -LINUX_EINVAL);
    exec.version = 1;
    exec.flags = DRM_VMW_EXECBUF_FLAG_IMPORT_FENCE_FD;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF, &exec) ==
           -LINUX_EINVAL);
    exec.flags = 0;
    uint32_t too_short = _IOC(_IOC_WRITE, DRM_IOCTL_BASE,
        DRM_COMMAND_BASE + DRM_VMW_EXECBUF, 31);
    assert(ioctl_call(&client_task, &client, too_short, &exec) == -LINUX_EINVAL);

    drm_test_fence_passed = drm_test_fence_issued;
    for (unsigned i = 0; i < 4; ++i)
        drm_vmwgfx_execbuf_process_retirements(8);
    drm_test_fence_status_calls = 0;
    uint32_t first_fair_fence = drm_test_fence_issued + 1u;
    for (unsigned i = 0; i < 10; ++i) {
        struct drm_vmw_execbuf_arg no_reply = {.version = 1};
        assert(ioctl_call(&client_task, &client, DRM_IOCTL_VMW_EXECBUF,
                          &no_reply) == 0);
    }
    drm_vmwgfx_execbuf_process_retirements(8);
    assert(drm_test_fence_status_calls == 8u);
    for (unsigned i = 0; i < 8; ++i)
        assert(drm_test_fence_status_sequences[i] == first_fair_fence + i);
    drm_vmwgfx_execbuf_process_retirements(8);
    assert(drm_test_fence_status_calls == 16u);
    assert(drm_test_fence_status_sequences[8] == first_fair_fence + 8u);
    assert(drm_test_fence_status_sequences[9] == first_fair_fence + 9u);
    for (unsigned i = 0; i < 6; ++i)
        assert(drm_test_fence_status_sequences[10u + i] == first_fair_fence + i);
    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_execbuf_process_retirements(8);
    drm_vmwgfx_execbuf_process_retirements(8);
    drm_test_ticks += 101;
    for (unsigned i = 0; i < 4; ++i)
        drm_vmwgfx_process_retirements(8);

    /* Global in-flight accounting survives final OFD close and reopen. A
     * stalled host keeps all 32 jobs charged until their real fences pass. */
    assert(drm_vmwgfx_exec_work_count == 0u);
    struct task pressure_task = make_task(902, false);
    struct task_file pressure_file = {0};
    assert(drm_file_open(&pressure_task, &pressure_file) == 0);
    pressure_task.syscall_file = &pressure_file;
    pressure_task.syscall_fd = 8;
    pressure_task.syscall_file_number = LINUX_SYS_IOCTL;
    for (unsigned i = 0; i < DRM_VMW_EXEC_WORK_MAX; ++i) {
        struct drm_vmw_execbuf_arg pressure_exec = {.version = 1};
        assert(ioctl_call(&pressure_task, &pressure_file,
                          DRM_IOCTL_VMW_EXECBUF, &pressure_exec) == 0);
    }
    uint32_t pressure_fence_last = drm_test_fence_issued;
    struct drm_vmw_execbuf_arg over_limit_exec = {.version = 1};
    assert(ioctl_call(&pressure_task, &pressure_file,
                      DRM_IOCTL_VMW_EXECBUF, &over_limit_exec) ==
           -LINUX_ENOMEM);
    pressure_task.syscall_file = NULL;
    drm_file_close(&pressure_file);

    struct task reopened_task = make_task(903, false);
    struct task_file reopened_file = {0};
    assert(drm_file_open(&reopened_task, &reopened_file) == 0);
    reopened_task.syscall_file = &reopened_file;
    reopened_task.syscall_fd = 9;
    reopened_task.syscall_file_number = LINUX_SYS_IOCTL;
    struct drm_vmw_execbuf_arg reopened_over_limit = {.version = 1};
    assert(ioctl_call(&reopened_task, &reopened_file,
                      DRM_IOCTL_VMW_EXECBUF, &reopened_over_limit) ==
           -LINUX_ENOMEM);
    assert(drm_vmwgfx_exec_work_count == DRM_VMW_EXEC_WORK_MAX);
    drm_test_fence_passed = pressure_fence_last;
    for (unsigned i = 0; i < DRM_VMW_EXEC_WORK_MAX / 8u; ++i)
        drm_vmwgfx_execbuf_process_retirements(8);
    assert(drm_vmwgfx_exec_work_count == 0u);
    struct drm_vmw_execbuf_arg after_reap = {.version = 1};
    assert(ioctl_call(&reopened_task, &reopened_file,
                      DRM_IOCTL_VMW_EXECBUF, &after_reap) == 0);
    drm_test_fence_passed = drm_test_fence_issued;
    drm_vmwgfx_execbuf_process_retirements(8);
    reopened_task.syscall_file = NULL;
    drm_file_close(&reopened_file);

    test_gmr_retirement_busy_does_not_reuse(&owner_task, &owner);

    client_task.syscall_file = NULL;
    drm_file_close(&client);
    drm_file_close(&owner);
    assert(allocations == 0);
}

static int drm_test_fail_alloc_bo(struct task *task, struct task_file *task_file)
{
    union drm_vmw_alloc_bo_arg argument = {0};
    argument.req.size = 4096u;
    return ioctl_call(task, task_file, DRM_IOCTL_VMW_ALLOC_BO, &argument);
}

static void test_faildiag_real_bo_failures(void)
{
    struct task task = make_task(314, false);
    struct drm_file drm_file = {.is_render = true};
    struct task_file task_file = {.drm_file = &drm_file};
    const uint64_t original_bo_bytes = drm_bo_bytes;
    const unsigned original_allocations = allocations;
    const uint64_t original_pages_allocated = mm_pages_allocated;
    const uint64_t original_pages_freed = mm_pages_freed;
    assert(original_bo_bytes == 0u);

    drm_test_diag_log_reset();
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_test_diag_log_calls == 0u);
    drm_bo_bytes = original_bo_bytes;

    const char *near_matches[] = {
        "xdrm.faildiag=1",
        "drm.faildiag=10",
        "drm.faildiag=1x",
        "drm.faildiag=0",
    };
    for (unsigned i = 0; i < sizeof(near_matches) / sizeof(near_matches[0]); ++i) {
        drm_faildiag_configure(near_matches[i]);
        drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
        assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
        assert(drm_test_diag_log_calls == 0u);
        drm_bo_bytes = original_bo_bytes;
    }
    drm_faildiag_configure(NULL);
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_test_diag_log_calls == 0u);
    drm_bo_bytes = original_bo_bytes;

    drm_faildiag_configure("quiet drm.faildiag=1 console=ttyS0");
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_bo_bytes == DRM_BO_GLOBAL_BYTES);
    drm_bo_bytes = original_bo_bytes;
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "[drm:faildiag]"));
    assert(strstr(drm_test_diag_log_buffer, "pid=314"));
    assert(strstr(drm_test_diag_log_buffer, "handler=11/bo"));
    assert(strstr(drm_test_diag_log_buffer, "stage=bo.global_bytes"));
    assert(strstr(drm_test_diag_log_buffer, "errno=-12"));
    assert(strstr(drm_test_diag_log_buffer, "requested=4096"));
    assert(strstr(drm_test_diag_log_buffer, "aligned=4096"));
    assert(strstr(drm_test_diag_log_buffer, "retained=268435456"));
    assert(strstr(drm_test_diag_log_buffer, "pages=1"));
    assert(strstr(drm_test_diag_log_buffer, "free_kib=12345"));

    drm_test_diag_log_reset();
    drm_faildiag_configure("drm.faildiag=1");
    mm_allocation_fails = true;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    mm_allocation_fails = false;
    assert(drm_bo_bytes == original_bo_bytes);
    assert(allocations == original_allocations);
    assert(mm_pages_allocated == original_pages_allocated);
    assert(mm_pages_freed == original_pages_freed);
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "stage=bo.physical_pages"));
    assert(strstr(drm_test_diag_log_buffer, "retained=0"));

    drm_test_diag_log_reset();
    drm_faildiag_configure("drm.faildiag=1");
    struct drm_vmw_execbuf_arg execbuf = {.version = 1u};
    fail_kernel_malloc_after = 0;
    assert(ioctl_call(&task, &task_file, DRM_IOCTL_VMW_EXECBUF,
                      &execbuf) == -LINUX_ENOMEM);
    fail_kernel_malloc_after = -1;
    assert(drm_vmwgfx_exec_work_count == 0u);
    assert(drm_vmwgfx_exec_work_bytes == 0u);
    assert(allocations == original_allocations);
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "handler=16/execbuf"));
    assert(strstr(drm_test_diag_log_buffer, "stage=exec.work.heap"));
    assert(strstr(drm_test_diag_log_buffer, "errno=-12"));
    assert(strstr(drm_test_diag_log_buffer, "source=0"));
    assert(strstr(drm_test_diag_log_buffer, "work_count=0"));
    assert(strstr(drm_test_diag_log_buffer, "work_bytes=0"));

    drm_test_diag_log_reset();
    drm_faildiag_configure("drm.faildiag=1");
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    for (unsigned i = 0; i < 20u; ++i)
        assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    drm_bo_bytes = original_bo_bytes;
    assert(drm_test_diag_log_calls == 16u);
    assert(allocations == original_allocations);
    assert(mm_pages_allocated == original_pages_allocated);
    assert(mm_pages_freed == original_pages_freed);
    assert(drm_bo_bytes == original_bo_bytes);
    drm_faildiag_configure(NULL);
    drm_test_diag_log_reset();
}

int main(void)
{
    test_faildiag_real_bo_failures();
    test_registration_and_open_lifetime();
    test_render_open_never_takes_primary_master();
    test_render_ioctl_permission_matrix();
    test_vmwgfx_resource_contract();
    test_prime_surface_fd_lifetime();
    test_core_abi_and_validation();
    test_legacy_magic_and_empty_readiness();
    test_dumb_and_vmwgfx_bo_contract();
    test_kms_resources_properties_and_scanout();
    test_kms_surface_format_compatibility();
    test_kms_surface_present_pressure();
    test_vgpu9_execbuf_v1_empty_fence_abi();
    assert(mm_pages_freed == mm_pages_allocated);
    puts("drm-core: PASS");
    return 0;
}
