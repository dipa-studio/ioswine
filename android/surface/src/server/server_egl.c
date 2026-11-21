/*
 * EXPERIMENTAL, compile-time OFF (IOSWINE_USE_EGL=0 by default).
 *
 * EGL texture-upload presentation path for ioswine-surface.
 *
 * What it does: creates an EGL window surface on the ANativeWindow,
 * uploads the damage rects of each client frame into a GL texture with
 * glTexSubImage2D, and draws a fullscreen textured quad (letterboxed
 * via the viewport) each PRESENT.
 *
 * What it does NOT do: zero-copy. The client memfd cannot be wrapped in
 * an AHardwareBuffer through any public NDK API (there is no
 * "import fd into AHardwareBuffer" entry point), so the pixels still
 * cross the CPU->GPU boundary once per PRESENT via glTexSubImage2D.
 * The win over the primary ANativeWindow_lock path is therefore small:
 * the BGRA->RGBA swizzle moves from our CPU loop into the GPU (the
 * texture is uploaded as GL_BGRA_EXT / GL_UNSIGNED_BYTE and sampled
 * as-is), and the window lock/unlock round-trip is replaced by
 * eglSwapBuffers. The cost is an EGL context, a shader, and a second
 * code path to maintain, plus the risk that the Adreno driver's
 * BGRA texture upload is slower than a plain memcpy on some devices.
 *
 * Status: written but NOT compiled and NOT tested on device (no NDK in
 * this environment at authoring time). Enable only by setting
 * IOSWINE_USE_EGL=1 in CMakeLists.txt and verifying on the phone that
 * (a) it links, (b) frames appear, (c) FPS/latency beat the lock path
 * measured with the test client. Until then the lock path is the only
 * supported one.
 *
 * If this path is enabled, server.c must call isws_egl_init(window)
 * on APP_CMD_INIT_WINDOW (replacing setBuffersGeometry), isws_egl_present()
 * instead of present_frame(), and isws_egl_destroy() on TERM_WINDOW.
 * Those call sites are intentionally NOT wired in server.c yet.
 */
#if IOSWINE_USE_EGL

#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"

#define LOG_TAG "ioswine-surface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;
static GLuint g_program = 0;
static GLuint g_texture = 0;
static GLuint g_vbo = 0;
static int32_t g_tex_w = 0, g_tex_h = 0;

static const char *k_vs =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";

static const char *k_fs =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("egl: shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

int isws_egl_init(ANativeWindow *window, int32_t w, int32_t h) {
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) return -1;
    EGLint major, minor;
    if (!eglInitialize(g_display, &major, &minor)) return -1;

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_display, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) return -1;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_context = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_context == EGL_NO_CONTEXT) return -1;
    g_surface = eglCreateWindowSurface(g_display, cfg, window, NULL);
    if (g_surface == EGL_NO_SURFACE) return -1;
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return -1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_vs);
    if (!vs) return -1;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_fs);
    if (!fs) {
        glDeleteShader(vs);
        return -1;
    }
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) return -1;

    glGenTextures(1, &g_texture);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenBuffers(1, &g_vbo);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    LOGI("egl: initialised (%d.%d) window %dx%d", major, minor, w, h);
    return 0;
}

void isws_egl_destroy(void) {
    if (g_display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_program) glDeleteProgram(g_program);
    if (g_texture) glDeleteTextures(1, &g_texture);
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
    if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
    if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    g_surface = EGL_NO_SURFACE;
    g_context = EGL_NO_CONTEXT;
    g_program = 0;
    g_texture = 0;
    g_vbo = 0;
    g_tex_w = g_tex_h = 0;
}

/* Upload damage rects and draw. src points at the selected buffer,
 * stride_px is the client stride in pixels. map is the letterbox mapping.
 * Returns 0 on success. */
int isws_egl_present(const uint8_t *src, uint32_t stride_px,
                     const isws_info_t *map, const isws_rect_t *rects,
                     size_t nrects, int32_t win_w, int32_t win_h) {
    if (g_display == EGL_NO_DISPLAY) return -1;
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return -1;

    glBindTexture(GL_TEXTURE_2D, g_texture);
    if (g_tex_w != (int32_t)map->client_w || g_tex_h != (int32_t)map->client_h) {
        /* (Re)allocate the texture, BGRA upload, no swizzle needed:
         * GL_BGRA_EXT keeps the client's byte order; the sampler
         * returns the channels in the right places. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)map->client_w,
                     (GLsizei)map->client_h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                     NULL);
        g_tex_w = (int32_t)map->client_w;
        g_tex_h = (int32_t)map->client_h;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)stride_px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (size_t i = 0; i < nrects; i++) {
        const uint8_t *row0 = src + (size_t)rects[i].y * stride_px * 4u +
                              (size_t)rects[i].x * 4u;
        glTexSubImage2D(GL_TEXTURE_2D, 0, rects[i].x, rects[i].y,
                        (GLsizei)rects[i].w, (GLsizei)rects[i].h,
                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, row0);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Letterbox: viewport covers only the client frame rect. */
    GLint vx = (GLint)map->offset_x;
    GLint vy = (GLint)(win_h - (map->offset_y + map->client_h * map->scale));
    GLint vw = (GLint)(map->client_w * map->scale);
    GLint vh = (GLint)(map->client_h * map->scale);
    glViewport(0, 0, win_w, win_h);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vx, vy, vw, vh);

    /* Fullscreen quad in the (letterboxed) viewport, UVs flipped in Y
     * because GL texture origin is bottom-left and the client frame is
     * top-left. */
    static const float verts[] = {
        /* x, y, u, v */
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glUseProgram(g_program);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    GLint a_pos = glGetAttribLocation(g_program, "a_pos");
    GLint a_uv = glGetAttribLocation(g_program, "a_uv");
    glEnableVertexAttribArray((GLuint)a_pos);
    glEnableVertexAttribArray((GLuint)a_uv);
    glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (const void *)0);
    glVertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (const void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)a_pos);
    glDisableVertexAttribArray((GLuint)a_uv);
    eglSwapBuffers(g_display, g_surface);
    return 0;
}

#endif /* IOSWINE_USE_EGL */
