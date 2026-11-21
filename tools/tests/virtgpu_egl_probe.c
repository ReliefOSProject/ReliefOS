/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <drm/vmwgfx_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <stddef.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

static const char *marker = "VIRTGPU";

_Noreturn static void fail(const char *stage)
{
    fprintf(stderr,"%s_GL_FAIL %s egl=%#x gl=%#x\n",marker,stage,eglGetError(),glGetError());
    exit(1);
}

static uint64_t vmw_param(int fd, uint32_t param)
{
    struct drm_vmw_getparam_arg arg = {.param = param};
    if (ioctl(fd, DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM,
                           struct drm_vmw_getparam_arg), &arg) == -1) {
        fprintf(stderr, "SVGA3D_GL_FAIL GET_PARAM %u\n", param);
        perror("ioctl");
        exit(1);
    }
    return arg.value;
}

static bool check_svga(int fd, const char *mode)
{
    char name[32] = {0};
    struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    if (ioctl(fd, DRM_IOCTL_VERSION, &version) == -1 || strcmp(name, "vmwgfx"))
        fail("vmwgfx DRM identity");

    uint64_t device = vmw_param(fd, DRM_VMW_PARAM_DEVICE_ID);
    uint64_t caps = vmw_param(fd, DRM_VMW_PARAM_HW_CAPS);
    uint64_t dx = vmw_param(fd, DRM_VMW_PARAM_DX);
    uint64_t sm41 = vmw_param(fd, DRM_VMW_PARAM_SM4_1);
    uint64_t sm5 = vmw_param(fd, DRM_VMW_PARAM_SM5);
    uint64_t gl43 = vmw_param(fd, DRM_VMW_PARAM_GL43);
    /* SVGA_CAP_GBOBJECTS from VMware's SVGA II register specification. */
    int gb = (caps & 0x08000000U) != 0;

    printf("SVGA3D_MODE requested=%s device=%#llx abi=%d.%d.%d gb=%d dx=%u sm41=%u sm5=%u gl43=%u\n",
           mode, (unsigned long long)device, version.version_major, version.version_minor,
           version.version_patchlevel, gb, dx != 0, sm41 != 0, sm5 != 0, gl43 != 0);
    if (device != 0x0405 || !vmw_param(fd, DRM_VMW_PARAM_3D) ||
        (!strcmp(mode, "gb") && !gb) || (!strcmp(mode, "dx") && (!gb || !dx)))
        fail("requested SVGA II capability");
    if (!strcmp(mode, "legacy") && gb)
        fail("legacy test requires SVGA II with guest-backed objects disabled on the device");
    if ((sm41 && !dx) || (sm5 && !sm41) || (gl43 && !sm5))
        fail("inconsistent DX capability");
    return !strcmp(mode, "dx") && gl43;
}

static void wait_vmw_fence(int fd, const struct drm_vmw_fence_rep *reply)
{
    struct drm_vmw_fence_wait_arg wait = {
        .handle = reply->handle,
        .timeout_us = 3000000,
        .flags = DRM_VMW_FENCE_FLAG_EXEC,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    if (reply->error || reply->mask || !reply->handle ||
        ioctl(fd, DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_FENCE_WAIT,
                           struct drm_vmw_fence_wait_arg), &wait) != 0)
        fail("vmwgfx fence reply/wait");
}

static void check_vmw_execbuf(int fd)
{
    struct execbuf_v1 {
        uint64_t commands;
        uint32_t command_size, throttle_us;
        uint64_t fence_rep;
        uint32_t version, flags;
    };
    _Static_assert(sizeof(struct execbuf_v1) == 32 &&
                   sizeof(struct execbuf_v1) == offsetof(struct drm_vmw_execbuf_arg, context_handle),
                   "32-byte execbuf v1 prefix");
    _Static_assert(sizeof(struct drm_vmw_execbuf_arg) == 40, "40-byte execbuf v2");
    const unsigned long execbuf_v2 = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_EXECBUF,
                                            struct drm_vmw_execbuf_arg);
    struct drm_vmw_fence_rep reply = {.fd = -1, .error = -EFAULT};
    struct execbuf_v1 v1 = {.version = 1, .fence_rep = (uintptr_t)&reply};

    if (ioctl(fd, DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_EXECBUF, struct execbuf_v1), &v1) != 0 || reply.fd != -1)
        fail("vmwgfx execbuf v1");
    wait_vmw_fence(fd, &reply);

    reply = (struct drm_vmw_fence_rep){.fd = -1, .error = -EFAULT};
    struct drm_vmw_execbuf_arg v2 = {
        .version = 2,
        .context_handle = UINT32_MAX,
        .flags = DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD,
        .fence_rep = (uintptr_t)&reply,
    };
    if (ioctl(fd, execbuf_v2, &v2) != 0 || reply.fd < 0 || fcntl(reply.fd, F_GETFD) != FD_CLOEXEC)
        fail("vmwgfx execbuf v2 export");
    v2.flags = DRM_VMW_EXECBUF_FLAG_IMPORT_FENCE_FD;
    v2.fence_rep = 0;
    v2.imported_fence_fd = reply.fd;
    if (ioctl(fd, execbuf_v2, &v2) != 0)
        fail("vmwgfx execbuf v2 import");
    struct pollfd completion = {.fd = reply.fd, .events = POLLIN};
    if (poll(&completion, 1, 3000) != 1 || !(completion.revents & POLLIN))
        fail("vmwgfx sync_file poll");
    struct sync_fence_info fence = {0};
    struct sync_file_info info = {.num_fences = 1, .sync_fence_info = (uintptr_t)&fence};
    if (ioctl(reply.fd, SYNC_IOC_FILE_INFO, &info) != 0) {
        perror("SYNC_IOC_FILE_INFO");
        fail("vmwgfx sync_file info");
    }
    printf("SVGA3D_SYNC_FILE status=%d fences=%u fence_status=%d driver=%.32s timeline=%.32s\n",
           info.status, info.num_fences, fence.status, fence.driver_name, fence.obj_name);
    /* Newer Linux dma-fence detaches driver ops on signal; implementations
     * with immutable names may instead retain the producing timeline. */
    bool named = !strncmp(fence.driver_name, "vmwgfx", sizeof(fence.driver_name)) &&
                 !strncmp(fence.obj_name, "svga", sizeof(fence.obj_name));
    bool detached = !strncmp(fence.driver_name, "detached-driver", sizeof(fence.driver_name)) &&
                    !strncmp(fence.obj_name, "signaled-timeline", sizeof(fence.obj_name));
    if (info.status != 1 || info.num_fences != 1 || fence.status != 1 ||
        !fence.timestamp_ns || (!named && !detached))
        fail("vmwgfx sync_file completion info");
    wait_vmw_fence(fd, &reply);
    if (close(reply.fd) != 0)
        fail("vmwgfx sync_file close");

    static const struct drm_vmw_fence_rep read_only = {.fd = -1, .error = -EFAULT};
    v2.flags = DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD;
    v2.fence_rep = (uintptr_t)&read_only;
    if (ioctl(fd, execbuf_v2, &v2) != -1 || errno != EFAULT)
        fail("vmwgfx read-only fence reply");
    puts("SVGA3D_EXECBUF_PASS v1=32 v2=40 sync_file=pass reply_fault=EFAULT");
}

/** @brief Compile an actual shader and retain its diagnostic log on failure.
 * @param type GLES shader stage. @param source NUL-terminated source.
 * @return Compiled owned object; failure terminates this disposable probe.
 */
static GLuint shader(GLenum type,const char *source)
{
    GLuint object=glCreateShader(type);glShaderSource(object,1,&source,NULL);
    glCompileShader(object);GLint success=0;glGetShaderiv(object,GL_COMPILE_STATUS,&success);
    if(!success){char log[1024];glGetShaderInfoLog(object,sizeof(log),NULL,log);fprintf(stderr,"%s\n",log);fail("shader compile");}
    return object;
}

/** @brief Read POSIX monotonic seconds for an observational rendering measurement.
 * @return Fractional seconds; failure terminates this probe.
 */
static double monotonic(void)
{
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))fail("clock_gettime");
    return (double)now.tv_sec+(double)now.tv_nsec/1e9;
}

static unsigned benchmark_draws(void)
{
    const char *value = getenv("GPU_PROBE_DRAWS");
    if (!value)
        return 1000;
    char *end;
    unsigned long count = strtoul(value, &end, 10);
    if (end == value || *end || !count || count > 1000000)
        fail("GPU_PROBE_DRAWS must be in [1, 1000000]");
    return count;
}

static void check_occlusion(bool core)
{
    PFNGLGENQUERIESEXTPROC gen = glGenQueries;
    PFNGLBEGINQUERYEXTPROC begin = glBeginQuery;
    PFNGLENDQUERYEXTPROC end = glEndQuery;
    PFNGLGETQUERYOBJECTUIVEXTPROC result = glGetQueryObjectuiv;
    PFNGLDELETEQUERIESEXTPROC destroy = glDeleteQueries;
    if (!core) {
        const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "GL_EXT_occlusion_query_boolean"))
            fail("occlusion query capability");
        gen = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueriesEXT");
        begin = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQueryEXT");
        end = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQueryEXT");
        result = (PFNGLGETQUERYOBJECTUIVEXTPROC)eglGetProcAddress("glGetQueryObjectuivEXT");
        destroy = (PFNGLDELETEQUERIESEXTPROC)eglGetProcAddress("glDeleteQueriesEXT");
        if (!gen || !begin || !end || !result || !destroy)
            fail("occlusion query entry points");
    }

    GLuint queries[2];
    gen(2, queries);
    glEnable(GL_SCISSOR_TEST);
    for (unsigned i = 0; i < 2; ++i) {
        glScissor(i ? 1 : 32, i ? 62 : 24, 1, 1);
        begin(GL_ANY_SAMPLES_PASSED, queries[i]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        end(GL_ANY_SAMPLES_PASSED);
    }
    glDisable(GL_SCISSOR_TEST);
    GLuint inside = 0, outside = 1;
    result(queries[0], GL_QUERY_RESULT, &inside);
    result(queries[1], GL_QUERY_RESULT, &outside);
    destroy(2, queries);
    if (glGetError() != GL_NO_ERROR || !inside || outside)
        fail("occlusion result");
    printf("%s_GL_QUERY inside=%u outside=%u\n", marker, inside, outside);
}

static void check_texture_update(GLuint texture)
{
    unsigned char upload[16 * 16 * 4], readback[sizeof(upload)] = {0};
    for (unsigned i = 0; i < sizeof(upload); ++i)
        upload[i] = (i * 31u) ^ (i >> 2);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 3, 5, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, upload);
    glReadPixels(3, 5, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    if (glGetError() != GL_NO_ERROR || memcmp(upload, readback, sizeof(upload)))
        fail("texture subimage upload/readback");
    printf("%s_GL_TEXTURE bytes=%zu readback=pass\n", marker, sizeof(upload));
}

static void check_dx_draw(GLuint resolve)
{
    const GLsizei size = 64;
    GLuint target, attachments[2], indirect;
    glGenFramebuffers(1, &target);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glGenRenderbuffers(2, attachments);
    glBindRenderbuffer(GL_RENDERBUFFER, attachments[0]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 2, GL_RGBA8, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, attachments[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, attachments[1]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 2, GL_DEPTH_COMPONENT24, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, attachments[1]);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fail("multisample color/depth framebuffer");

    const GLuint draw[] = {3, 1, 0, 0};
    glGenBuffers(1, &indirect);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(draw), draw, GL_STATIC_DRAW);
    glEnable(GL_DEPTH_TEST);
    glClearDepthf(0.25f);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (unsigned pass = 0; pass < 2; ++pass) {
        /* z=0 maps to 0.5 (occluded) or 0.125 (passes GL_LESS). */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glDepthRangef(0.f, pass ? 0.25f : 1.f);
        glDrawArraysIndirect(GL_TRIANGLES, NULL);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
        glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve);
        unsigned char pixel[4] = {0};
        glReadPixels(32, 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const unsigned char expected[2][4] = {{0, 0, 0, 255}, {64, 191, 128, 255}};
        GLenum error = glGetError();
        for (unsigned channel = 0; channel < 4; ++channel) {
            if (error != GL_NO_ERROR || abs(pixel[channel] - expected[pass][channel]) > 1) {
                fprintf(stderr, "pass=%u pixel=%u,%u,%u,%u error=%#x\n",
                        pass, pixel[0], pixel[1], pixel[2], pixel[3], error);
                fail(pass ? "indirect draw multisample resolve" : "indirect draw depth occlusion");
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    }
    glDepthRangef(0.f, 1.f);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve);
    glDeleteFramebuffers(1, &target);
    glDeleteRenderbuffers(2, attachments);
    glDeleteBuffers(1, &indirect);
    printf("%s_GL_DX_DRAW indirect=pass depth=pass multisample_resolve=pass\n", marker);
}

static void check_compute(void)
{
    const GLuint initial[] = {10, 20, 30, 40};
    const GLuint expected[] = {27, 54, 81, 108};
    GLuint compute = shader(GL_COMPUTE_SHADER,
        "#version 310 es\n"
        "layout(local_size_x=4) in;\n"
        "layout(std430, binding=0) buffer Result { highp uint words[]; };\n"
        "void main() { highp uint i=gl_GlobalInvocationID.x; words[i]+=(i+1u)*17u; }\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, compute);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
        fail("compute program link");
    glUseProgram(program);

    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(initial), initial, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const void *data = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(expected), GL_MAP_READ_BIT);
    if (!data || memcmp(data, expected, sizeof(expected)) || !glUnmapBuffer(GL_SHADER_STORAGE_BUFFER) ||
        glGetError() != GL_NO_ERROR)
        fail("compute storage readback");

    glDeleteBuffers(1, &buffer);
    glDeleteProgram(program);
    glDeleteShader(compute);
    printf("%s_GL_COMPUTE words=4 readback=pass\n", marker);
}

int main(int argc, char **argv)
{
    const char *mode = argc == 2 ? argv[1] : NULL;
    if (argc != 1 && (!mode || (strcmp(mode, "legacy") && strcmp(mode, "gb") && strcmp(mode, "dx")))) {
        fprintf(stderr, "usage: %s [legacy|gb|dx]\n", argv[0]);
        return 2;
    }
    if (mode)
        marker = "SVGA3D";
    setvbuf(stdout,NULL,_IONBF,0);
    int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s_GL_FAIL open render node\n", marker);
        perror("/dev/dri/renderD128");
        return 1;
    }
    bool require_compute = mode && check_svga(fd, mode);
    struct gbm_device *gbm=gbm_create_device(fd);if(!gbm)fail("gbm_create_device");
    EGLDisplay display=eglGetPlatformDisplay(0x31D7,gbm,NULL);
    EGLint major,minor;
    if(display==EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor))fail("eglInitialize");
    printf("%s_EGL %d.%d vendor=%s\n",marker,major,minor,eglQueryString(display,EGL_VENDOR));
    if(!eglBindAPI(EGL_OPENGL_ES_API))fail("eglBindAPI");
    EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,
        require_compute ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;
    if(!eglChooseConfig(display,attributes,&config,1,&count) || count!=1)fail("eglChooseConfig");
    EGLint context_attributes[]={EGL_CONTEXT_MAJOR_VERSION,require_compute ? 3 : 2,
        EGL_CONTEXT_MINOR_VERSION,require_compute ? 1 : 0,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attributes);
    if(context==EGL_NO_CONTEXT)fail("eglCreateContext");
    if(!strstr(eglQueryString(display,EGL_EXTENSIONS),"EGL_KHR_surfaceless_context") ||
       !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))fail("eglMakeCurrent");
    const char *renderer=(const char *)glGetString(GL_RENDERER);
    const char *version=(const char *)glGetString(GL_VERSION);
    printf("%s_GL renderer=%s version=%s\n",marker,renderer ? renderer : "(null)",
           version ? version : "(null)");
    if (!renderer || (mode ? strncmp(renderer, "SVGA3D;", 7) != 0 : !strstr(renderer, "virgl")) ||
        strstr(renderer, "llvmpipe"))
        fail("hardware renderer");
    unsigned es_major, es_minor;
    if (!version || sscanf(version, "OpenGL ES %u.%u", &es_major, &es_minor) != 2)
        fail("OpenGL ES version");
    bool compute = es_major > 3 || (es_major == 3 && es_minor >= 1);
    if (require_compute && !compute)
        fail("OpenGL ES 3.1 capability");
    GLuint texture,framebuffer;
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)fail("framebuffer");
    glViewport(0,0,64,64);
    for(unsigned round=0;round<16;++round) {
        float red=(round&1) ? 1.f : 0.f;
        glClearColor(red,.5f,.25f,1.f);glClear(GL_COLOR_BUFFER_BIT);
        unsigned char pixel[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        glFinish();
        if(glGetError()!=GL_NO_ERROR || abs((int)pixel[0]-(red ? 255 : 0))>1 ||
            abs((int)pixel[1]-128)>1 || abs((int)pixel[2]-64)>1 || pixel[3]!=255) {
            fprintf(stderr,"pixel=%u,%u,%u,%u round=%u\n",pixel[0],pixel[1],pixel[2],pixel[3],round);
            fail("pixel readback");
        }
    }
    printf("%s_GL_CLEAR readback=16 pass\n", marker);
    GLuint vertex=shader(GL_VERTEX_SHADER,"attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}");
    GLuint fragment=shader(GL_FRAGMENT_SHADER,"precision mediump float; void main(){gl_FragColor=vec4(.25,.75,.5,1.);}");
    GLuint program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);
    glBindAttribLocation(program,0,"position");glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked)fail("program link");
    glUseProgram(program);
    const GLfloat positions[]={-1.f,-1.f,1.f,-1.f,0.f,1.f};
    GLuint vao = 0;
    if (require_compute) {
        /* GLES requires an explicit VAO for indirect draws. */
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
    }
    GLuint vertices;glGenBuffers(1,&vertices);glBindBuffer(GL_ARRAY_BUFFER,vertices);
    glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);glEnableVertexAttribArray(0);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char inside[4]={0},outside[4]={0};
    glReadPixels(32,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,inside);
    glReadPixels(1,62,1,1,GL_RGBA,GL_UNSIGNED_BYTE,outside);
    if(glGetError()!=GL_NO_ERROR || abs((int)inside[0]-64)>1 || abs((int)inside[1]-191)>1 ||
       abs((int)inside[2]-128)>1 || inside[3]!=255 || outside[0] || outside[1] || outside[2]) {
        fprintf(stderr, "triangle inside=%u,%u,%u,%u outside=%u,%u,%u,%u\n",
                inside[0], inside[1], inside[2], inside[3],
                outside[0], outside[1], outside[2], outside[3]);
        fail("triangle pixels");
    }
    check_occlusion(es_major >= 3);
    unsigned draws = benchmark_draws();
    glFinish();double begin=monotonic();
    for (unsigned draw = 0; draw < draws; ++draw)
        glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();double elapsed=monotonic()-begin;
    if (glGetError() != GL_NO_ERROR)
        fail("batched draws");
    printf("%s_GL_BENCH draws=%u seconds=%.6f draws_per_second=%.1f target=64x64 offscreen\n",
           marker, draws, elapsed, elapsed > 0 ? draws / elapsed : 0);
    check_texture_update(texture);
    if (require_compute)
        check_dx_draw(framebuffer);
    if (require_compute || (!mode && compute))
        check_compute();
    if (vao)
        glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1,&vertices);glDeleteProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);
    glDeleteFramebuffers(1,&framebuffer);glDeleteTextures(1,&texture);
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT) ||
       !eglDestroyContext(display,context) ||
       !eglTerminate(display))fail("EGL teardown");
    gbm_device_destroy(gbm);
    if (mode)
        check_vmw_execbuf(fd);
    close(fd);
    printf("%s_GL_PASS renderer=%s readback=16 triangle=pass teardown=pass\n",
           marker,mode ? "svga" : "virgl");
    return 0;
}
