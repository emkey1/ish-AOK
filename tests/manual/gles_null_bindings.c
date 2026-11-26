// gles_null_bindings.c -- OpenGL ES on the GPU with a current Mesa: the cases
// zink hands the Vulkan driver a null binding for, checked against llvmpipe.
//
// Mesa 25.2 and later start zink only on a Vulkan device with robustness2's
// nullDescriptor, and use it: an attribute whose array is disabled, a sampler
// with no texture bound, are VK_NULL_HANDLE to the driver. AOK-Workstation's GPU
// (Venus on the MoltenVK fork) has offered the feature since build 559. This
// draws five scenes into an FBO with no window and prints the colour at the
// centre of each; the same lines must come out of software rendering.
//
// Not in the suite: it needs Mesa with its headers and the GPU drivers in the
// root (Alpine 3.24: apk add build-base mesa-dev mesa-dri-gallium mesa-egl
// mesa-gles mesa-vulkan-virtio vulkan-loader).
//
//     cc -O1 -o glnull gles_null_bindings.c -lEGL -lGLESv2
//     LIBGL_ALWAYS_SOFTWARE=1 MESA_SHADER_CACHE_DISABLE=true ./glnull > sw.txt
//     MESA_LOADER_DRIVER_OVERRIDE=zink ./glnull > gpu.txt
//     grep '^[1-5]' sw.txt > a; grep '^[1-5]' gpu.txt > b; cmp a b
//
// and gpu.txt's first line must name zink. (The software run is a check of
// the emulator in its own right: llvmpipe's JIT is where the arm64 guest's
// borrowed Apple M1 identity and its missing half-precision conversions
// showed -- tests/manual/arm64/cpu_identity.c and fcvt_half.c.)
//
//   1 a triangle strip with per-vertex colour from a buffer
//   2 the same shape with the colour attribute's array DISABLED (a constant
//     attribute: the slot has no buffer)
//   3 a shader that samples a texture unit with NO texture bound (GL: reads
//     opaque black) and adds a uniform colour
//   4 a textured quad
//   5 scene 1 again, after the others: nothing they did is left bound wrong
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof(log), NULL, log); printf("shader: %s\n", log); exit(2); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "col");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { printf("link failed\n"); exit(2); }
    return p;
}
static void report(const char *name) {
    unsigned char px[4] = {9, 9, 9, 9};
    glFinish();
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("%-28s %3u %3u %3u %3u  err=%#x\n", name, px[0], px[1], px[2], px[3], glGetError());
}

int main(void) {
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { printf("no EGL display\n"); return 2; }
    EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : NULL, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { printf("no context\n"); return 2; }
    printf("renderer: %s\n", (const char *) glGetString(GL_RENDERER));

    GLuint fbo, tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("fbo incomplete\n"); return 2; }
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, 64, 64);

    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const GLfloat cols[] = {0.2f, 0.4f, 0.6f, 1, 0.2f, 0.4f, 0.6f, 1, 0.2f, 0.4f, 0.6f, 1, 0.2f, 0.4f, 0.6f, 1};
    GLuint vb[2];
    glGenBuffers(2, vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb[0]); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vb[1]); glBufferData(GL_ARRAY_BUFFER, sizeof(cols), cols, GL_STATIC_DRAW);

    GLuint colour = program(
        "attribute vec2 pos; attribute vec4 col; varying vec4 v; void main() { v = col; gl_Position = vec4(pos, 0.0, 1.0); }",
        "precision mediump float; varying vec4 v; void main() { gl_FragColor = v; }");
    GLuint sampled = program(
        "attribute vec2 pos; varying vec2 uv; void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }",
        "precision mediump float; varying vec2 uv; uniform sampler2D t; uniform vec4 add; void main() { gl_FragColor = texture2D(t, uv) + add; }");

    for (int pass = 0; pass < 2; pass++) {
        // 1 / 5: colour from a buffer
        glClearColor(1, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(colour);
        glBindBuffer(GL_ARRAY_BUFFER, vb[0]); glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vb[1]); glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        report(pass == 0 ? "1 colour from a buffer" : "5 colour from a buffer again");
        if (pass == 1)
            break;

        // 2: the colour array disabled -- a constant attribute, no buffer in the slot
        glClear(GL_COLOR_BUFFER_BIT);
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 0.8f, 0.6f, 0.4f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        report("2 constant attribute");

        // 3: a sampler with no texture bound
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(sampled);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(glGetUniformLocation(sampled, "t"), 0);
        glUniform4f(glGetUniformLocation(sampled, "add"), 0.0f, 0.4f, 0.0f, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        report("3 unbound texture + green");

        // 4: a real texture
        GLuint t2; glGenTextures(1, &t2); glBindTexture(GL_TEXTURE_2D, t2);
        static unsigned char texels[4 * 4 * 4];
        for (int i = 0; i < 16; i++) { texels[i * 4] = 51; texels[i * 4 + 1] = 0; texels[i * 4 + 2] = 204; texels[i * 4 + 3] = 255; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        report("4 textured + green");
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return 0;
}
