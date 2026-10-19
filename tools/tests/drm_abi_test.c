#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>

#include <stddef.h>

/* These values and layouts are copied from the canonical Linux 6.12 UAPI
 * headers. Keep the literals here so a self-consistent local header typo is
 * caught by the fixture. */
_Static_assert(sizeof(struct drm_version) == 64, "drm_version ABI size");
_Static_assert(offsetof(struct drm_version, name_len) == 16, "drm_version name_len");
_Static_assert(offsetof(struct drm_version, name) == 24, "drm_version name");
_Static_assert(offsetof(struct drm_version, desc) == 56, "drm_version desc");
_Static_assert(sizeof(struct drm_unique) == 16, "drm_unique ABI size");
_Static_assert(sizeof(struct drm_set_version) == 16, "drm_set_version ABI size");
_Static_assert(sizeof(struct drm_get_cap) == 16, "drm_get_cap ABI size");
_Static_assert(sizeof(struct drm_set_client_cap) == 16, "drm_set_client_cap ABI size");
_Static_assert(DRM_IOCTL_VERSION == 0xc0406400u, "DRM_IOCTL_VERSION Linux x86-64 value");
_Static_assert(DRM_IOCTL_GET_UNIQUE == 0xc0106401u, "DRM_IOCTL_GET_UNIQUE value");
_Static_assert(DRM_IOCTL_GET_MAGIC == 0x80046402u, "DRM_IOCTL_GET_MAGIC value");
_Static_assert(DRM_IOCTL_SET_VERSION == 0xc0106407u, "DRM_IOCTL_SET_VERSION value");
_Static_assert(DRM_IOCTL_GET_CAP == 0xc010640cu, "DRM_IOCTL_GET_CAP value");
_Static_assert(DRM_IOCTL_SET_CLIENT_CAP == 0x4010640du, "DRM_IOCTL_SET_CLIENT_CAP value");
_Static_assert(DRM_IOCTL_AUTH_MAGIC == 0x40046411u, "DRM_IOCTL_AUTH_MAGIC value");
_Static_assert(DRM_IOCTL_SET_MASTER == 0x641eu, "DRM_IOCTL_SET_MASTER value");
_Static_assert(DRM_IOCTL_DROP_MASTER == 0x641fu, "DRM_IOCTL_DROP_MASTER value");
_Static_assert(sizeof(struct drm_mode_create_dumb) == 32, "create dumb ABI size");
_Static_assert(offsetof(struct drm_mode_create_dumb, handle) == 16, "create dumb handle");
_Static_assert(offsetof(struct drm_mode_create_dumb, size) == 24, "create dumb size");
_Static_assert(sizeof(struct drm_mode_map_dumb) == 16, "map dumb ABI size");
_Static_assert(sizeof(struct drm_mode_destroy_dumb) == 4, "destroy dumb ABI size");
_Static_assert(sizeof(struct drm_gem_close) == 8, "GEM close ABI size");
_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "modeinfo ABI size");
_Static_assert(sizeof(struct drm_mode_card_res) == 64, "resources ABI size");
_Static_assert(sizeof(struct drm_mode_crtc) == 104, "CRTC ABI size");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "connector ABI size");
_Static_assert(sizeof(struct drm_mode_get_encoder) == 20, "encoder ABI size");
_Static_assert(sizeof(struct drm_mode_obj_get_properties) == 32,
               "object properties ABI size");
_Static_assert(sizeof(struct drm_mode_fb_cmd) == 28, "legacy framebuffer ABI size");
_Static_assert(sizeof(struct drm_mode_fb_cmd2) == 104, "framebuffer2 ABI size");
_Static_assert(sizeof(struct drm_mode_fb_dirty_cmd) == 24, "dirtyfb ABI size");
_Static_assert(sizeof(union drm_vmw_alloc_bo_arg) == 24, "VMW alloc BO ABI size");
_Static_assert(offsetof(union drm_vmw_alloc_bo_arg, rep.cur_gmr_id) == 12,
               "VMW alloc current handle offset");
_Static_assert(sizeof(struct drm_vmw_handle_close_arg) == 8,
               "VMW unref BO ABI size");
_Static_assert(sizeof(struct drm_vmw_synccpu_arg) == 16,
               "VMW sync CPU ABI size");
_Static_assert(DRM_IOCTL_MODE_CREATE_DUMB == 0xc02064b2u,
               "create dumb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_MAP_DUMB == 0xc01064b3u,
               "map dumb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_DESTROY_DUMB == 0xc00464b4u,
               "destroy dumb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_GETRESOURCES == 0xc04064a0u,
               "resources ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_GETCRTC == 0xc06864a1u,
               "get CRTC ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_SETCRTC == 0xc06864a2u,
               "set CRTC ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_GETENCODER == 0xc01464a6u,
               "get encoder ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_GETCONNECTOR == 0xc05064a7u,
               "get connector ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_ADDFB == 0xc01c64aeu,
               "addfb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_RMFB == 0xc00464afu,
               "rmfb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_DIRTYFB == 0xc01864b1u,
               "dirtyfb ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_ADDFB2 == 0xc06864b8u,
               "addfb2 ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_MODE_OBJ_GETPROPERTIES == 0xc02064b9u,
               "object properties ioctl x86-64 encoding");
_Static_assert(DRM_IOCTL_GEM_CLOSE == 0x40086409u,
               "GEM close ioctl x86-64 encoding");
_Static_assert(DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO,
                        union drm_vmw_alloc_bo_arg) == 0xc0186441u,
               "VMW alloc BO ioctl x86-64 encoding");
_Static_assert(DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_DMABUF,
                       struct drm_vmw_handle_close_arg) == 0x40086442u,
               "VMW unref BO ioctl x86-64 encoding");
_Static_assert(DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_SYNCCPU,
                       struct drm_vmw_synccpu_arg) == 0x40106459u,
               "VMW sync CPU ioctl x86-64 encoding");

int main(void) { return 0; }
