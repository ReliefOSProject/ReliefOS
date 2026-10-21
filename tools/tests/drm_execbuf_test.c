#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/errno.h>
#include "../../kernel/reliefnt/drivers/video/svga/svga_protocol.h"
#include "../../kernel/reliefnt/drivers/video/drm/vmwgfx/execbuf_parser.c"

static uint32_t append_packet(uint8_t *dst, uint32_t at, uint32_t id,
                              const void *payload, uint32_t payload_bytes)
{
    uint32_t header[2] = {id, payload_bytes};
    memcpy(dst + at, header, sizeof(header));
    if (payload_bytes) memcpy(dst + at + sizeof(header), payload,
                              payload_bytes);
    return at + (uint32_t)sizeof(header) + payload_bytes;
}

static int count_packets(uint32_t command, const uint8_t *body,
                         uint32_t body_bytes, uint32_t offset, void *context)
{
    (void)command;
    (void)body;
    (void)body_bytes;
    (void)offset;
    ++*(uint32_t *)context;
    return 0;
}

static void test_walker_does_not_visit_prefix_of_malformed_stream(void)
{
    uint8_t stream[64] = {0};
    SVGA3dRenderState fill_mode = {
        .state = SVGA3D_RS_FILLMODE,
        .uintValue = SVGA3D_FILLMODE_FILL,
    };
    uint32_t render_body[1 + sizeof(fill_mode) / sizeof(uint32_t)] = {7};
    memcpy(&render_body[1], &fill_mode, sizeof(fill_mode));
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE,
                                  render_body, sizeof(render_body));
    uint32_t malformed[2] = {SVGA_3D_CMD_SETTRANSFORM, 3};
    memcpy(stream + used, malformed, sizeof(malformed));
    used += sizeof(malformed);
    uint32_t visited = 0, packets = UINT32_MAX;
    assert(drm_vmwgfx_walk_stream(stream, used, &packets, count_packets,
                                  &visited) == -LINUX_EINVAL);
    assert(visited == 0 && packets == 0);
}

static void test_inline_constants_accept_one_or_more_vectors(void)
{
    uint8_t stream[128] = {0};
    uint32_t body[16] = {0};
    body[0] = 7; /* cid */
    body[1] = 4; /* first register */
    body[2] = SVGA3D_SHADERTYPE_VS;
    body[3] = SVGA3D_CONST_TYPE_FLOAT;
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SET_SHADER_CONST,
                                  body, 8u * sizeof(uint32_t));
    used = append_packet(stream, used, SVGA_3D_CMD_SET_SHADER_CONST,
                         body, sizeof(body));
    uint32_t packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
    assert(packets == 2);
}

static void test_late_malformed_packet_rejects_the_whole_stream(void)
{
    uint8_t stream[96] = {0};
    SVGA3dCmdSetZRange range = {.cid = 7, .zRange = {0.0f, 1.0f}};
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SETZRANGE,
                                  &range, sizeof(range));
    uint32_t malformed[2] = {SVGA_3D_CMD_SETTRANSFORM, 3};
    memcpy(stream + used, malformed, sizeof(malformed));
    used += sizeof(malformed);
    uint32_t packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
}

static void test_unknown_and_direct_lifecycle_commands_are_rejected(void)
{
    uint8_t stream[32] = {0};
    uint32_t body = 7;
    uint32_t packets = UINT32_MAX;
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_CONTEXT_DEFINE,
                                  &body, sizeof(body));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
    used = append_packet(stream, 0, SVGA_3D_CMD_BASE + 999u,
                         &body, sizeof(body));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
}

static void test_empty_and_oversized_stream_bounds(void)
{
    uint32_t packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(NULL, 0, &packets) == 0);
    assert(packets == 0);
    uint8_t oversized[65540] = {0};
    assert(drm_vmwgfx_validate_stream(oversized, sizeof(oversized), &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);
    assert(drm_vmwgfx_validate_stream(NULL, 4, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
    uint32_t valid = 0x12345678u;
    assert(drm_vmwgfx_validate_stream(&valid, sizeof(valid), &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);
}

static void test_variable_packet_sizes_and_shader_code_are_bounded(void)
{
    uint8_t stream[128] = {0};
    uint32_t packets = UINT32_MAX;
    uint32_t words[4] = {7, 0, SVGA3D_SHADERTYPE_VS, 0};
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SHADER_DEFINE,
                                  words, 3u * sizeof(uint32_t));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
    used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE, words,
                         sizeof(uint32_t) + sizeof(uint32_t));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
}

static void test_surface_dma_requires_linux_suffix_and_copybox(void)
{
    uint8_t stream[160] = {0};
    uint8_t body[sizeof(SVGA3dCmdSurfaceDMA) + sizeof(SVGA3dCopyBox) +
                 sizeof(SVGA3dCmdSurfaceDMASuffix)] = {0};
    SVGA3dCmdSurfaceDMA dma = {0};
    dma.transfer = SVGA3D_WRITE_HOST_VRAM;
    SVGA3dCopyBox box = {.w = 1, .h = 1, .d = 1};
    SVGA3dCmdSurfaceDMASuffix suffix = {
        .suffixSize = sizeof(SVGA3dCmdSurfaceDMASuffix),
        .maximumOffset = 4,
    };
    memcpy(body, &dma, sizeof(dma));
    memcpy(body + sizeof(dma), &box, sizeof(box));
    memcpy(body + sizeof(dma) + sizeof(box), &suffix, sizeof(suffix));

    uint32_t packets = UINT32_MAX;
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SURFACE_DMA,
                                  body, sizeof(body));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
    assert(packets == 1);

    used = append_packet(stream, 0, SVGA_3D_CMD_SURFACE_DMA, body,
                         sizeof(dma) + sizeof(box));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);
}

static void test_protocol_resource_fields_are_bounded_without_float_filters(void)
{
    uint8_t stream[128] = {0};
    uint32_t packets = UINT32_MAX;
    SVGA3dCmdSetRenderTarget target = {
        .cid = 1, .type = SVGA3D_RT_MAX,
    };
    uint32_t used = append_packet(stream, 0,
        SVGA_3D_CMD_SETRENDERTARGET, &target, sizeof(target));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);

    uint32_t texture_body[4] = {
        1, 32, SVGA3D_TS_BIND_TEXTURE, 1,
    };
    used = append_packet(stream, 0, SVGA_3D_CMD_SETTEXTURESTATE,
                         texture_body, sizeof(texture_body));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == -LINUX_EINVAL);
    assert(packets == 0);

    uint8_t draw_body[sizeof(SVGA3dCmdDrawPrimitives) +
                      sizeof(SVGA3dVertexDecl) +
                      sizeof(SVGA3dPrimitiveRange)] = {0};
    SVGA3dCmdDrawPrimitives draw = {.cid = 1, .numVertexDecls = 1,
                                     .numRanges = 1};
    SVGA3dVertexDecl decl = {0};
    decl.identity.type = SVGA3D_DECLTYPE_FLOAT3;
    decl.identity.usage = SVGA3D_DECLUSAGE_POSITION;
    decl.rangeHint = (SVGA3dArrayRangeHint){.first = 0, .last = 3};
    SVGA3dPrimitiveRange range = {
        .primType = SVGA3D_PRIMITIVE_TRIANGLELIST,
        .primitiveCount = 1,
        .indexArray = {.surfaceId = SVGA3D_INVALID_ID},
        .indexBias = -3,
    };
    memcpy(draw_body, &draw, sizeof(draw));
    memcpy(draw_body + sizeof(draw), &decl, sizeof(decl));
    memcpy(draw_body + sizeof(draw) + sizeof(decl), &range, sizeof(range));
    used = append_packet(stream, 0, SVGA_3D_CMD_DRAW_PRIMITIVES,
                         draw_body, sizeof(draw_body));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
    assert(packets == 1);
}

static void test_state_enums_and_values_are_checked_by_field(void)
{
    uint8_t stream[sizeof(SVGA3dCmdSetLightData) + 16] = {0};
    uint32_t packets = UINT32_MAX;
    SVGA3dRenderState render_state = {0};
    uint32_t render_body[1 + sizeof(render_state) / sizeof(uint32_t)] = {1};
    SVGA3dTextureState texture_state = {0};
    uint32_t texture_body[1 + sizeof(texture_state) / sizeof(uint32_t)] = {1};

    SVGA3dCmdSetTransform transform = {
        .cid = 1, .type = SVGA3D_TRANSFORM_MAX,
    };
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SETTRANSFORM,
                                  &transform, sizeof(transform));
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    SVGA3dCmdSurfaceStretchBlt stretch = {
        .mode = SVGA3D_STRETCH_BLT_MAX,
    };
    used = append_packet(stream, 0, SVGA_3D_CMD_SURFACE_STRETCHBLT,
                         &stretch, sizeof(stretch));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    SVGA3dCmdSetClipPlane clip_plane = {
        .index = SVGA3D_MAX_CLIP_PLANES,
    };
    used = append_packet(stream, 0, SVGA_3D_CMD_SETCLIPPLANE,
                         &clip_plane, sizeof(clip_plane));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    render_state = (SVGA3dRenderState){
        .state = SVGA3D_RS_ZENABLE,
        .uintValue = 2,
    };
    memcpy(&render_body[1], &render_state, sizeof(render_state));
    used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE,
                         render_body, sizeof(render_body));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    texture_state = (SVGA3dTextureState){
        .stage = 0, .name = SVGA3D_TS_COLORARG1,
        .value = SVGA3D_TA_TEXTURE | (1u << 8),
    };
    memcpy(&texture_body[1], &texture_state, sizeof(texture_state));
    used = append_packet(stream, 0, SVGA_3D_CMD_SETTEXTURESTATE,
                         texture_body, sizeof(texture_body));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    render_state = (SVGA3dRenderState){
        .state = SVGA3D_RS_FOGMODE,
        .uintValue = UINT32_MAX,
    };
    memcpy(&render_body[1], &render_state, sizeof(render_state));
    used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE,
                         render_body, sizeof(render_body));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    SVGA3dCmdSetLightEnabled light_enabled = {
        .cid = 1, .index = 0, .enabled = 2,
    };
    used = append_packet(stream, 0, SVGA_3D_CMD_SETLIGHTENABLED,
                         &light_enabled, sizeof(light_enabled));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    SVGA3dCmdSetMaterial material = {.cid = 1, .face = SVGA3D_FACE_MAX};
    used = append_packet(stream, 0, SVGA_3D_CMD_SETMATERIAL,
                         &material, sizeof(material));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    texture_state = (SVGA3dTextureState){
        .stage = 0, .name = SVGA3D_TS_ADDRESSU,
        .value = SVGA3D_TEX_ADDRESS_MAX,
    };
    memcpy(&texture_body[1], &texture_state, sizeof(texture_state));
    used = append_packet(stream, 0, SVGA_3D_CMD_SETTEXTURESTATE,
                         texture_body, sizeof(texture_body));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    SVGA3dCmdSetLightData light = {
        .cid = 1, .index = 0,
        .data = {.type = SVGA3D_LIGHTTYPE_MAX},
    };
    used = append_packet(stream, 0, SVGA_3D_CMD_SETLIGHTDATA,
                         &light, sizeof(light));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
           -LINUX_EINVAL);
    assert(packets == 0);

    /* Float-valued states carry protocol float bits verbatim.  The field
     * validator must not turn enum checking into a blanket NaN/Inf filter. */
    render_state = (SVGA3dRenderState){
        .state = SVGA3D_RS_FOGSTART,
        .uintValue = 0x7fc00000u,
    };
    memcpy(&render_body[1], &render_state, sizeof(render_state));
    used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE,
                         render_body, sizeof(render_body));
    packets = UINT32_MAX;
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
    assert(packets == 1);
}

static uint32_t append_fillmode_state(uint8_t *stream, uint32_t at,
                                      uint32_t value)
{
    SVGA3dRenderState state = {
        .state = SVGA3D_RS_FILLMODE,
        .uintValue = value,
    };
    uint32_t body[1 + sizeof(state) / sizeof(uint32_t)] = {7};
    memcpy(&body[1], &state, sizeof(state));
    return append_packet(stream, at, SVGA_3D_CMD_SETRENDERSTATE,
                         body, sizeof(body));
}

static void test_mesa_compact_fillmode_and_valid_explicit_faces_are_accepted(void)
{
    const uint32_t modes[] = {
        SVGA3D_FILLMODE_POINT,
        SVGA3D_FILLMODE_LINE,
        SVGA3D_FILLMODE_FILL,
    };
    const uint32_t faces[] = {
        SVGA3D_FACE_NONE,
        SVGA3D_FACE_FRONT,
        SVGA3D_FACE_BACK,
        SVGA3D_FACE_FRONT_BACK,
    };
    uint8_t stream[32] = {0};
    uint32_t packets = UINT32_MAX;

    for (uint32_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        uint32_t used = append_fillmode_state(stream, 0, modes[i]);
        packets = UINT32_MAX;
        assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
        assert(packets == 1);
    }

    for (uint32_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        for (uint32_t j = 0; j < sizeof(faces) / sizeof(faces[0]); ++j) {
            uint32_t value = (faces[j] << 16) | modes[i];
            uint32_t used = append_fillmode_state(stream, 0, value);
            packets = UINT32_MAX;
            assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
            assert(packets == 1);
        }
    }
}

static void test_fillmode_keeps_invalid_mode_and_face_domains_rejected(void)
{
    const uint32_t invalid_values[] = {
        0u, /* invalid mode and compact face */
        SVGA3D_FILLMODE_MAX,
        (SVGA3D_FACE_NONE << 16), /* valid face, invalid mode */
        (SVGA3D_FACE_MAX << 16) | SVGA3D_FILLMODE_FILL,
        (UINT16_MAX << 16) | SVGA3D_FILLMODE_FILL,
    };
    uint8_t stream[32] = {0};
    uint32_t packets = UINT32_MAX;

    for (uint32_t i = 0;
         i < sizeof(invalid_values) / sizeof(invalid_values[0]); ++i) {
        uint32_t used = append_fillmode_state(stream, 0, invalid_values[i]);
        packets = UINT32_MAX;
        assert(drm_vmwgfx_validate_stream(stream, used, &packets) ==
               -LINUX_EINVAL);
        assert(packets == 0);
    }
}

static void test_captured_mesa_render_state_sequence_is_accepted(void)
{
    const SVGA3dRenderState states[] = {
        {.state = SVGA3D_RS_COLORWRITEENABLE, .uintValue = 15u},
        {.state = SVGA3D_RS_BLENDENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_BLENDCOLOR, .uintValue = 0u},
        {.state = SVGA3D_RS_STENCILENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_STENCILENABLE2SIDED, .uintValue = 0u},
        {.state = SVGA3D_RS_ZENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_ALPHATESTENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_STENCILREF, .uintValue = 0u},
        {.state = SVGA3D_RS_SHADEMODE, .uintValue = 2u},
        {.state = SVGA3D_RS_FILLMODE, .uintValue = 3u},
        {.state = SVGA3D_RS_CULLMODE, .uintValue = 1u},
        {.state = SVGA3D_RS_SCISSORTESTENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_MULTISAMPLEANTIALIAS, .uintValue = 0u},
        {.state = SVGA3D_RS_LASTPIXEL, .uintValue = 0u},
        {.state = SVGA3D_RS_POINTSIZE, .uintValue = 0x3f800000u},
        {.state = SVGA3D_RS_POINTSIZEMIN, .uintValue = 0x3f800000u},
        {.state = SVGA3D_RS_POINTSIZEMAX, .uintValue = 0x42800000u},
        {.state = SVGA3D_RS_POINTSPRITEENABLE, .uintValue = 0u},
        {.state = SVGA3D_RS_SLOPESCALEDEPTHBIAS, .uintValue = 0u},
        {.state = SVGA3D_RS_DEPTHBIAS, .uintValue = 0u},
        {.state = SVGA3D_RS_OUTPUTGAMMA, .uintValue = 0x3f800000u},
        {.state = SVGA3D_RS_CLIPPLANEENABLE, .uintValue = 0u},
    };
    uint32_t body[1 + 2 * (sizeof(states) / sizeof(states[0]))] = {16777216u};
    uint8_t stream[256] = {0};
    uint32_t packets = UINT32_MAX;
    memcpy(&body[1], states, sizeof(states));
    uint32_t used = append_packet(stream, 0, SVGA_3D_CMD_SETRENDERSTATE,
                                  body, sizeof(body));

    assert(sizeof(states) / sizeof(states[0]) == 22u);
    assert(drm_vmwgfx_validate_stream(stream, used, &packets) == 0);
    assert(packets == 1);
}

int main(void)
{
    test_walker_does_not_visit_prefix_of_malformed_stream();
    test_inline_constants_accept_one_or_more_vectors();
    test_late_malformed_packet_rejects_the_whole_stream();
    test_unknown_and_direct_lifecycle_commands_are_rejected();
    test_empty_and_oversized_stream_bounds();
    test_variable_packet_sizes_and_shader_code_are_bounded();
    test_surface_dma_requires_linux_suffix_and_copybox();
    test_protocol_resource_fields_are_bounded_without_float_filters();
    test_state_enums_and_values_are_checked_by_field();
    test_mesa_compact_fillmode_and_valid_explicit_faces_are_accepted();
    test_fillmode_keeps_invalid_mode_and_face_domains_rejected();
    test_captured_mesa_render_state_sequence_is_accepted();
    puts("drm-execbuf: PASS");
    return 0;
}
