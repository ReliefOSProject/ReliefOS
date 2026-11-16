/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

_Noreturn static void fail(const char *stage)
{
    fprintf(stderr,"VIRTGPU_GL_FAIL %s egl=%#x gl=%#x\n",stage,eglGetError(),glGetError());
    exit(1);
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

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);
    if(fd<0){perror("VIRTGPU_GL_FAIL open render node");return 1;}
    struct gbm_device *gbm=gbm_create_device(fd);if(!gbm)fail("gbm_create_device");
    EGLDisplay display=eglGetPlatformDisplay(0x31D7,gbm,NULL);
    EGLint major,minor;
    if(display==EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor))fail("eglInitialize");
    printf("VIRTGPU_EGL %d.%d vendor=%s\n",major,minor,eglQueryString(display,EGL_VENDOR));
    if(!eglBindAPI(EGL_OPENGL_ES_API))fail("eglBindAPI");
    EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;
    if(!eglChooseConfig(display,attributes,&config,1,&count) || count!=1)fail("eglChooseConfig");
    EGLint context_attributes[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attributes);
    if(context==EGL_NO_CONTEXT)fail("eglCreateContext");
    if(!strstr(eglQueryString(display,EGL_EXTENSIONS),"EGL_KHR_surfaceless_context") ||
       !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))fail("eglMakeCurrent");
    const char *renderer=(const char *)glGetString(GL_RENDERER);
    printf("VIRTGPU_GL renderer=%s version=%s\n",renderer,(const char *)glGetString(GL_VERSION));
    if(!renderer || !strstr(renderer,"virgl") || strstr(renderer,"llvmpipe"))fail("hardware renderer");
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
    GLuint vertex=shader(GL_VERTEX_SHADER,"attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}");
    GLuint fragment=shader(GL_FRAGMENT_SHADER,"precision mediump float; void main(){gl_FragColor=vec4(.25,.75,.5,1.);}");
    GLuint program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);
    glBindAttribLocation(program,0,"position");glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked)fail("program link");
    glUseProgram(program);
    const GLfloat positions[]={-1.f,-1.f,1.f,-1.f,0.f,1.f};
    GLuint vertices;glGenBuffers(1,&vertices);glBindBuffer(GL_ARRAY_BUFFER,vertices);
    glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);glEnableVertexAttribArray(0);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char inside[4],outside[4];
    glReadPixels(32,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,inside);
    glReadPixels(1,62,1,1,GL_RGBA,GL_UNSIGNED_BYTE,outside);
    if(abs((int)inside[0]-64)>1 || abs((int)inside[1]-191)>1 ||
       abs((int)inside[2]-128)>1 || inside[3]!=255 || outside[0] || outside[1] || outside[2])fail("triangle pixels");
    glFinish();double begin=monotonic();
    for(unsigned frame=0;frame<1000;++frame)glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();double elapsed=monotonic()-begin;
    printf("VIRTGPU_GL_BENCH draws=1000 seconds=%.6f draws_per_second=%.1f target=64x64 offscreen\n",elapsed,elapsed>0 ? 1000/elapsed : 0);
    glDeleteBuffers(1,&vertices);glDeleteProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);
    glDeleteFramebuffers(1,&framebuffer);glDeleteTextures(1,&texture);
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT) ||
       !eglDestroyContext(display,context) ||
       !eglTerminate(display))fail("EGL teardown");
    gbm_device_destroy(gbm);close(fd);
    puts("VIRTGPU_GL_PASS renderer=virgl readback=16 triangle=pass teardown=pass");
    return 0;
}
