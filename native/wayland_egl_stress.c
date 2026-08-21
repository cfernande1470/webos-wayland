#define _GNU_SOURCE
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "egl_diagnostics.h"
#include "webos_input.h"
#include "webos_shell.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef KEY_ESC
#define KEY_ESC 1
#define KEY_ENTER 28
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_BACK 158
#endif

#define STRESS_MAX_SAMPLES 8192
#define STRESS_TIMER_SLOTS 8

enum stress_pacing {
    STRESS_PACING_FRAME,
    STRESS_PACING_SWAP,
    STRESS_PACING_OFFSCREEN
};

enum stress_workload {
    STRESS_WORKLOAD_ALU,
    STRESS_WORKLOAD_SFU,
    STRESS_WORKLOAD_BANDWIDTH
};

struct stress_gpu_timer_slot {
    GLuint id;
    int active;
    double started_sec;
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_callback *frame_cb;
    struct webos_input_context input;
    struct webos_shell_context webos_shell;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;

    GLuint program;
    GLuint vbo;
    GLuint fbo;
    GLuint offscreen_texture;
    GLuint bandwidth_texture0;
    GLuint bandwidth_texture1;

    GLint attr_pos;
    GLint uni_resolution;
    GLint uni_time;
    GLint uni_pointer;
    GLint uni_theme;
    GLint uni_texture0;
    GLint uni_texture1;

    int width;
    int height;
    int force_4k;
    int iterations;
    int requested_highp;
    int selected_highp;
    int offscreen_batch;
    int offscreen_since_sync;
    enum stress_pacing pacing;
    enum stress_workload workload;
    char workload_name[16];
    char pacing_name[16];
    char color_mode[16];
    char priority_mode[16];
    int running;
    int render_visible;
    unsigned long long frame;
    unsigned long long presented;
    int theme;

    int pointer_x;
    int pointer_y;

    double start_sec;
    double warmup_sec;
    double duration_sec;
    double last_report_sec;
    double *cpu_submit_samples;
    double *gpu_samples;
    size_t cpu_submit_count;
    size_t gpu_sample_count;
    struct stress_gpu_timer_slot gpu_timers[STRESS_TIMER_SLOTS];
    int gpu_timer_supported;
    PFNGLGENQUERIESEXTPROC gen_queries;
    PFNGLDELETEQUERIESEXTPROC delete_queries;
    PFNGLBEGINQUERYEXTPROC begin_query;
    PFNGLENDQUERYEXTPROC end_query;
    PFNGLGETQUERYOBJECTIVEXTPROC get_query_objectiv;
    PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_objectui64v;
};

static double now_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void die_egl(const char *where) {
    fprintf(stderr, "EGL_ERROR %s err=0x%04x\n", where, eglGetError());
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        fprintf(stderr, "GL_SHADER_COMPILE_FAILED type=0x%x log=%s\n", type, log);
        glDeleteShader(sh);
        return 0;
    }

    return sh;
}

static GLuint make_program_once(struct app *a, int highp) {
    const char *vs =
        "attribute vec2 a_pos;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "  v_uv = a_pos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    const char *precision = highp ? "precision highp float;" : "precision mediump float;";
    const char *textures = a->workload == STRESS_WORKLOAD_BANDWIDTH
        ? "uniform sampler2D u_texture0;\nuniform sampler2D u_texture1;\n"
        : "";
    const char *loop = NULL;
    switch (a->workload) {
    case STRESS_WORKLOAD_ALU:
        loop =
            "  vec4 av = vec4(q, q.yx);\n"
            "  for (int i = 0; i < STRESS_ITERS; ++i) {\n"
            "    av = av * vec4(1.013, 0.997, 1.007, 0.991) + vec4(0.011, -0.007, 0.005, -0.003);\n"
            "    av = av * av.yzwx + av.zwxy * 0.17;\n"
            "    acc += dot(av, av.yzwx);\n"
            "  }\n";
        break;
    case STRESS_WORKLOAD_SFU:
        loop =
            "  for (int i = 0; i < STRESS_ITERS; ++i) {\n"
            "    float fi = float(i);\n"
            "    float sx = sin(q.x * (1.7 + fi * 0.021) + t * 0.91);\n"
            "    float cy = cos(q.y * (1.9 + fi * 0.017) - t * 0.73);\n"
            "    float root = sqrt(abs(sx * cy) + 0.001);\n"
            "    float inv = inversesqrt(abs(root) + 0.001);\n"
            "    acc += pow(abs(sx + cy) + 0.001, 1.37) * inv;\n"
            "    q = vec2(sx, cy) + q.yx * 0.72;\n"
            "  }\n";
        break;
    case STRESS_WORKLOAD_BANDWIDTH:
        loop =
            "  for (int i = 0; i < STRESS_ITERS; ++i) {\n"
            "    float fi = float(i);\n"
            "    vec2 uv0 = fract(v_uv * (1.0 + fi * 0.013) + vec2(t * 0.003, fi * 0.017));\n"
            "    vec2 uv1 = fract(v_uv.yx * (1.0 + fi * 0.009) - vec2(fi * 0.011, t * 0.002));\n"
            "    vec4 tx0 = texture2D(u_texture0, uv0);\n"
            "    vec4 tx1 = texture2D(u_texture1, uv1);\n"
            "    acc += dot(tx0, tx1) + tx0.x * tx1.w;\n"
            "  }\n";
        break;
    }

    char *fs = calloc(1, 16000);
    if (!fs) return 0;
    snprintf(fs, 16000,
        "%s\n#define STRESS_ITERS %d\n%s"
        "varying vec2 v_uv;\n"
        "uniform vec2 u_resolution;\n"
        "uniform float u_time;\n"
        "uniform vec2 u_pointer;\n"
        "uniform float u_theme;\n"
        "void main() {\n"
        "  vec2 uv = gl_FragCoord.xy / u_resolution.xy;\n"
        "  vec2 p = uv * 2.0 - 1.0;\n"
        "  p.x *= u_resolution.x / u_resolution.y;\n"
        "  vec2 mp = u_pointer / u_resolution.xy;\n"
        "  vec2 q = p;\n"
        "  float t = u_time;\n"
        "  float acc = 0.0;\n"
        "%s"
        "  float grid = step(0.985, sin((uv.x + t * 0.03) * u_resolution.x * 0.16))\n"
        "             + step(0.985, sin((uv.y - t * 0.02) * u_resolution.y * 0.16));\n"
        "  float cursor = smoothstep(0.035, 0.0, distance(uv, mp));\n"
        "  vec3 a = vec3(0.0, 0.55, 1.0);\n"
        "  vec3 b = vec3(1.0, 0.18, 0.72);\n"
        "  vec3 c = vec3(0.35, 1.0, 0.28);\n"
        "  vec3 theme = mix(a, b, step(0.5, mod(u_theme, 3.0)));\n"
        "  theme = mix(theme, c, step(1.5, mod(u_theme, 3.0)));\n"
        "  float stable = acc / (1.0 + abs(acc));\n"
        "  vec3 col = vec3(0.015, 0.02, 0.045) + theme * (0.25 + stable * 0.1);\n"
        "  col += vec3(grid) * 0.05 + cursor * vec3(1.0);\n"
        "  gl_FragColor = vec4(pow(abs(col), vec3(0.85)), 1.0);\n"
        "}\n",
        precision, a->iterations, textures, loop);

    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    free(fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);

    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);

    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "GL_PROGRAM_LINK_FAILED log=%s\n", log);
        glDeleteProgram(p);
        return 0;
    }

    return p;
}

static GLuint make_program(struct app *a) {
    if (a->requested_highp != 0) {
        GLuint program = make_program_once(a, 1);
        if (program) {
            a->selected_highp = 1;
            return program;
        }
        fprintf(stderr, "STRESS_PRECISION highp_compile_failed fallback=mediump\n");
    }

    a->selected_highp = 0;
    return make_program_once(a, 0);
}

static int choose_config(struct app *a) {
    EGLint count = 0;
    if (!eglGetConfigs(a->egl_display, NULL, 0, &count) || count < 1) {
        die_egl("eglGetConfigs(count)");
        return -1;
    }

    EGLConfig *configs = calloc((size_t)count, sizeof(*configs));
    if (!configs) {
        fprintf(stderr, "ERROR allocating EGL config list\n");
        return -1;
    }

    if (!eglGetConfigs(a->egl_display, configs, count, &count)) {
        free(configs);
        die_egl("eglGetConfigs(list)");
        return -1;
    }

    int best_score = -1000000;
    EGLConfig best = NULL;

    for (EGLint i = 0; i < count; i++) {
        EGLint surface = 0, renderable = 0;
        EGLint red = 0, green = 0, blue = 0, alpha = 0;
        EGLint depth = 0, stencil = 0, caveat = EGL_NONE;

        eglGetConfigAttrib(a->egl_display, configs[i], EGL_SURFACE_TYPE, &surface);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_RENDERABLE_TYPE, &renderable);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_RED_SIZE, &red);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_GREEN_SIZE, &green);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_BLUE_SIZE, &blue);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_ALPHA_SIZE, &alpha);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_STENCIL_SIZE, &stencil);
        eglGetConfigAttrib(a->egl_display, configs[i], EGL_CONFIG_CAVEAT, &caveat);

        if (!(surface & EGL_WINDOW_BIT) || !(renderable & EGL_OPENGL_ES2_BIT) ||
            caveat == EGL_SLOW_CONFIG) {
            continue;
        }

        if (strcmp(a->color_mode, "565") == 0) {
            if (red < 5 || green < 6 || blue < 5 || alpha != 0) continue;
        } else if (strcmp(a->color_mode, "rgb888") == 0) {
            if (red < 8 || green < 8 || blue < 8 || alpha != 0) continue;
        } else if (strcmp(a->color_mode, "8888") == 0) {
            if (red < 8 || green < 8 || blue < 8 || alpha < 8) continue;
        } else if (red < 8 || green < 8 || blue < 8) {
            continue;
        }

        int score = 0;
        if (strcmp(a->color_mode, "auto") == 0) score += alpha == 0 ? 10000 : 0;
        if (strcmp(a->color_mode, "565") == 0) {
            score += red == 5 && green == 6 && blue == 5 ? 10000 : 0;
        }
        score -= alpha * 100 + depth * 10 + stencil * 10;
        score -= (red - 8) + (green - 8) + (blue - 8);
        if (score > best_score) {
            best_score = score;
            best = configs[i];
        }
    }

    free(configs);
    if (!best) {
        fprintf(stderr, "ERROR no suitable EGL window config\n");
        return -1;
    }

    a->egl_config = best;

    EGLint id = 0, red = 0, green = 0, blue = 0, alpha = 0, depth = 0, stencil = 0;
    eglGetConfigAttrib(a->egl_display, best, EGL_CONFIG_ID, &id);
    eglGetConfigAttrib(a->egl_display, best, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(a->egl_display, best, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(a->egl_display, best, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(a->egl_display, best, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(a->egl_display, best, EGL_DEPTH_SIZE, &depth);
    eglGetConfigAttrib(a->egl_display, best, EGL_STENCIL_SIZE, &stencil);
    fprintf(stderr, "EGL_CONFIG id=%d rgba=%d/%d/%d/%d depth=%d stencil=%d\n",
            id, red, green, blue, alpha, depth, stencil);
    fprintf(stderr, "EGL_COLOR_MODE selected=%s\n", a->color_mode);
    return 0;
}

static void set_surface_opaque(struct app *a) {
    if (!a->compositor || !a->surface || a->width <= 0 || a->height <= 0) return;

    struct wl_region *region = wl_compositor_create_region(a->compositor);
    if (!region) return;

    wl_region_add(region, 0, 0, a->width, a->height);
    wl_surface_set_opaque_region(a->surface, region);
    wl_region_destroy(region);
}

static int parse_positive_env(const char *name, int fallback, int minimum, int maximum) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        fprintf(stderr, "STRESS_ENV_INVALID name=%s value=%s fallback=%d\n",
                name, value, fallback);
        return fallback;
    }
    return (int)parsed;
}

static void copy_mode(char *destination, size_t size, const char *value,
                      const char *fallback) {
    const char *selected = value && *value ? value : fallback;
    snprintf(destination, size, "%s", selected);
}

static int parse_resolution(struct app *a) {
    const char *preset = getenv("STRESS_RESOLUTION");
    if (preset) {
        if (strcmp(preset, "1080p") == 0) {
            a->width = 1920; a->height = 1080;
        } else if (strcmp(preset, "1440p") == 0) {
            a->width = 2560; a->height = 1440;
        } else if (strcmp(preset, "4k") == 0 || strcmp(preset, "2160p") == 0) {
            a->width = 3840; a->height = 2160;
        } else {
            fprintf(stderr, "STRESS_RESOLUTION_INVALID value=%s\n", preset);
            return -1;
        }
    }

    const char *width = getenv("STRESS_WIDTH");
    const char *height = getenv("STRESS_HEIGHT");
    if (width) a->width = parse_positive_env("STRESS_WIDTH", a->width, 16, 8192);
    if (height) a->height = parse_positive_env("STRESS_HEIGHT", a->height, 16, 8192);
    return 0;
}

static void init_texture(GLuint texture, int seed) {
    const int size = 256;
    unsigned char *pixels = malloc((size_t)size * (size_t)size * 4u);
    if (!pixels) return;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            size_t offset = ((size_t)y * (size_t)size + (size_t)x) * 4u;
            pixels[offset + 0] = (unsigned char)((x * 17 + y * 3 + seed) & 255);
            pixels[offset + 1] = (unsigned char)((x * 5 + y * 13 + seed * 7) & 255);
            pixels[offset + 2] = (unsigned char)((x * 11 + y * 19 + seed * 11) & 255);
            pixels[offset + 3] = 255;
        }
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}

static int init_offscreen_target(struct app *a) {
    if (a->pacing != STRESS_PACING_OFFSCREEN) return 0;

    glGenTextures(1, &a->offscreen_texture);
    glBindTexture(GL_TEXTURE_2D, a->offscreen_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, a->width, a->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &a->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, a->offscreen_texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "STRESS_FBO_FAILED status=0x%x\n", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }
    fprintf(stderr, "STRESS_FBO size=%dx%d status=complete\n", a->width, a->height);
    return 0;
}

static int init_bandwidth_textures(struct app *a) {
    if (a->workload != STRESS_WORKLOAD_BANDWIDTH) return 0;
    glGenTextures(1, &a->bandwidth_texture0);
    glGenTextures(1, &a->bandwidth_texture1);
    if (!a->bandwidth_texture0 || !a->bandwidth_texture1) return -1;
    init_texture(a->bandwidth_texture0, 3);
    init_texture(a->bandwidth_texture1, 29);
    fprintf(stderr, "STRESS_TEXTURES size=256x256 count=2\n");
    return 0;
}

static int init_gpu_timer(struct app *a) {
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!egl_extension_present(extensions, "GL_EXT_disjoint_timer_query")) {
        fprintf(stderr, "STRESS_GPU_TIMER available=0 reason=extension\n");
        return 0;
    }

    a->gen_queries = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueriesEXT");
    a->delete_queries = (PFNGLDELETEQUERIESEXTPROC)eglGetProcAddress("glDeleteQueriesEXT");
    a->begin_query = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQueryEXT");
    a->end_query = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQueryEXT");
    a->get_query_objectiv = (PFNGLGETQUERYOBJECTIVEXTPROC)eglGetProcAddress("glGetQueryObjectivEXT");
    a->get_query_objectui64v = (PFNGLGETQUERYOBJECTUI64VEXTPROC)eglGetProcAddress("glGetQueryObjectui64vEXT");
    if (!a->gen_queries || !a->delete_queries || !a->begin_query || !a->end_query ||
        !a->get_query_objectiv || !a->get_query_objectui64v) {
        fprintf(stderr, "STRESS_GPU_TIMER available=0 reason=entrypoint\n");
        return 0;
    }

    GLuint ids[STRESS_TIMER_SLOTS];
    a->gen_queries(STRESS_TIMER_SLOTS, ids);
    for (int i = 0; i < STRESS_TIMER_SLOTS; i++) a->gpu_timers[i].id = ids[i];
    a->gpu_timer_supported = 1;
    fprintf(stderr, "STRESS_GPU_TIMER available=1 slots=%d\n", STRESS_TIMER_SLOTS);
    return 1;
}

static void poll_gpu_timers(struct app *a) {
    if (!a->gpu_timer_supported) return;
    for (int i = 0; i < STRESS_TIMER_SLOTS; i++) {
        struct stress_gpu_timer_slot *slot = &a->gpu_timers[i];
        if (!slot->active) continue;
        GLint available = 0;
        a->get_query_objectiv(slot->id, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (!available) continue;
        GLuint64 nanoseconds = 0;
        a->get_query_objectui64v(slot->id, GL_QUERY_RESULT_EXT, &nanoseconds);
        GLboolean disjoint = GL_FALSE;
        glGetBooleanv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (!disjoint && slot->started_sec >= a->warmup_sec &&
            a->gpu_sample_count < STRESS_MAX_SAMPLES) {
            a->gpu_samples[a->gpu_sample_count++] = (double)nanoseconds / 1000000.0;
        } else if (disjoint) {
            fprintf(stderr, "STRESS_GPU_SAMPLE discarded=disjoint\n");
        }
        slot->active = 0;
    }
}

static int begin_gpu_timer(struct app *a) {
    if (!a->gpu_timer_supported) return -1;
    poll_gpu_timers(a);
    for (int i = 0; i < STRESS_TIMER_SLOTS; i++) {
        if (!a->gpu_timers[i].active) {
            a->begin_query(GL_TIME_ELAPSED_EXT, a->gpu_timers[i].id);
            a->gpu_timers[i].active = 1;
            a->gpu_timers[i].started_sec = now_sec();
            return i;
        }
    }
    return -1;
}

static void end_gpu_timer(struct app *a, int slot) {
    if (slot >= 0) a->end_query(GL_TIME_ELAPSED_EXT);
}

static int throttle_offscreen(struct app *a) {
    if (a->pacing != STRESS_PACING_OFFSCREEN) return 0;
    if (a->gpu_timer_supported) {
        poll_gpu_timers(a);
        int slot_available = 0;
        for (int i = 0; i < STRESS_TIMER_SLOTS; i++) {
            if (!a->gpu_timers[i].active) {
                slot_available = 1;
                break;
            }
        }
        if (!slot_available) {
            glFinish();
            poll_gpu_timers(a);
        }
    } else if (a->offscreen_since_sync >= a->offscreen_batch) {
        glFinish();
        a->offscreen_since_sync = 0;
    }
    return 0;
}

static int init_egl(struct app *a) {
    a->egl_window = wl_egl_window_create(a->surface, a->width, a->height);
    if (!a->egl_window) {
        fprintf(stderr, "ERROR wl_egl_window_create failed\n");
        return -1;
    }

    a->egl_display = eglGetDisplay((EGLNativeDisplayType)a->display);
    if (a->egl_display == EGL_NO_DISPLAY) {
        die_egl("eglGetDisplay");
        return -1;
    }

    EGLint major = 0;
    EGLint minor = 0;

    if (!eglInitialize(a->egl_display, &major, &minor)) {
        die_egl("eglInitialize");
        return -1;
    }

    fprintf(stderr, "EGL_VERSION %d.%d\n", major, minor);
    fprintf(stderr, "EGL_VENDOR %s\n", eglQueryString(a->egl_display, EGL_VENDOR));
    fprintf(stderr, "EGL_CLIENT_APIS %s\n", eglQueryString(a->egl_display, EGL_CLIENT_APIS));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        die_egl("eglBindAPI");
        return -1;
    }

    if (choose_config(a) < 0) {
        return -1;
    }

    EGLint ctx_attrs[8];
    int ctx_attr_count = 0;
    ctx_attrs[ctx_attr_count++] = EGL_CONTEXT_CLIENT_VERSION;
    ctx_attrs[ctx_attr_count++] = 2;
    const char *egl_extensions = eglQueryString(a->egl_display, EGL_EXTENSIONS);
    int priority_requested = strcmp(a->priority_mode, "high") == 0;
    int priority_applied = priority_requested &&
        egl_extension_present(egl_extensions, "EGL_IMG_context_priority");
    if (priority_requested && !priority_applied) {
        fprintf(stderr, "EGL_CONTEXT_PRIORITY requested=high applied=0 reason=extension\n");
    }
    if (priority_applied) {
        ctx_attrs[ctx_attr_count++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        ctx_attrs[ctx_attr_count++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    }
    ctx_attrs[ctx_attr_count++] = EGL_NONE;

    a->egl_context = eglCreateContext(
        a->egl_display, a->egl_config, EGL_NO_CONTEXT, ctx_attrs
    );
    if (a->egl_context == EGL_NO_CONTEXT) {
        die_egl("eglCreateContext");
        return -1;
    }
    if (priority_applied) {
        EGLint actual_priority = 0;
        if (eglQueryContext(a->egl_display, a->egl_context,
                            EGL_CONTEXT_PRIORITY_LEVEL_IMG, &actual_priority)) {
            fprintf(stderr, "EGL_CONTEXT_PRIORITY requested=high applied=1 actual=0x%x\n",
                    actual_priority);
        } else {
            fprintf(stderr, "EGL_CONTEXT_PRIORITY requested=high applied=1 actual=unknown\n");
        }
    } else if (!priority_requested) {
        fprintf(stderr, "EGL_CONTEXT_PRIORITY requested=default applied=0\n");
    }

    a->egl_surface = eglCreateWindowSurface(
        a->egl_display,
        a->egl_config,
        (EGLNativeWindowType)a->egl_window,
        NULL
    );

    if (a->egl_surface == EGL_NO_SURFACE) {
        die_egl("eglCreateWindowSurface");
        return -1;
    }

    if (!eglMakeCurrent(a->egl_display, a->egl_surface, a->egl_surface, a->egl_context)) {
        die_egl("eglMakeCurrent");
        return -1;
    }

    egl_log_capabilities(a->egl_display, "STRESS");

    int swap_interval = a->pacing == STRESS_PACING_FRAME ?
        parse_positive_env("STRESS_SWAP_INTERVAL", 1, 0, 4) : 0;
    if (a->pacing == STRESS_PACING_OFFSCREEN) swap_interval = 0;
    if (!eglSwapInterval(a->egl_display, swap_interval)) {
        die_egl("eglSwapInterval");
        return -1;
    }

    fprintf(stderr, "STRESS_SWAP_INTERVAL %d pacing=%s\n", swap_interval, a->pacing_name);

    a->program = make_program(a);
    if (!a->program) return -1;

    a->attr_pos = glGetAttribLocation(a->program, "a_pos");
    a->uni_resolution = glGetUniformLocation(a->program, "u_resolution");
    a->uni_time = glGetUniformLocation(a->program, "u_time");
    a->uni_pointer = glGetUniformLocation(a->program, "u_pointer");
    a->uni_theme = glGetUniformLocation(a->program, "u_theme");
    a->uni_texture0 = glGetUniformLocation(a->program, "u_texture0");
    a->uni_texture1 = glGetUniformLocation(a->program, "u_texture1");

    if (a->attr_pos < 0 || a->uni_resolution < 0 || a->uni_time < 0 ||
        a->uni_pointer < 0 || a->uni_theme < 0 ||
        (a->workload == STRESS_WORKLOAD_BANDWIDTH &&
         (a->uni_texture0 < 0 || a->uni_texture1 < 0))) {
        fprintf(stderr, "ERROR required shader attribute or uniform is unavailable\n");
        return -1;
    }

    const GLfloat verts[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f
    };

    glGenBuffers(1, &a->vbo);
    if (!a->vbo) {
        fprintf(stderr, "ERROR glGenBuffers failed\n");
        return -1;
    }
    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    a->cpu_submit_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->cpu_submit_samples));
    a->gpu_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->gpu_samples));
    if (!a->cpu_submit_samples || !a->gpu_samples) {
        fprintf(stderr, "ERROR allocating benchmark samples\n");
        return -1;
    }
    if (init_offscreen_target(a) < 0 || init_bandwidth_textures(a) < 0) return -1;
    init_gpu_timer(a);

    a->start_sec = now_sec();
    a->warmup_sec = a->start_sec + (double)parse_positive_env(
        "STRESS_WARMUP_MS", 1000, 0, 60000
    ) / 1000.0;
    a->duration_sec = (double)parse_positive_env(
        "STRESS_DURATION_MS", 10000, 0, 3600000
    ) / 1000.0;
    a->last_report_sec = a->start_sec;
    fprintf(stderr, "STRESS_DURATION duration_s=%.3f warmup_s=%.3f\n",
            a->duration_sec, a->warmup_sec - a->start_sec);

    return 0;
}

static void render(struct app *a);

static void webos_visibility_changed(void *data, int visible) {
    struct app *a = data;

    a->render_visible = visible;
    if (!visible && a->frame_cb) {
        wl_callback_destroy(a->frame_cb);
        a->frame_cb = NULL;
    } else if (visible && a->running && !a->frame_cb &&
               a->egl_context != EGL_NO_CONTEXT) {
        render(a);
    }
}

static void webos_close_requested(void *data) {
    struct app *a = data;
    a->running = 0;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    struct app *a = data;

    if (cb) wl_callback_destroy(cb);
    a->frame_cb = NULL;

    if (!a->running || !a->render_visible) return;

    render(a);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

static int compare_double(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double percentile(double *values, size_t count, double fraction) {
    if (count == 0) return 0.0;
    qsort(values, count, sizeof(*values), compare_double);
    size_t index = (size_t)((double)(count - 1) * fraction);
    return values[index];
}

static double average(const double *values, size_t count) {
    if (count == 0) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < count; i++) total += values[i];
    return total / (double)count;
}

static void record_cpu_submit(struct app *a, double milliseconds, double now) {
    if (now < a->warmup_sec || a->cpu_submit_count >= STRESS_MAX_SAMPLES) return;
    a->cpu_submit_samples[a->cpu_submit_count++] = milliseconds;
}

static int benchmark_expired(struct app *a, double now) {
    return a->duration_sec > 0.0 && now - a->start_sec >= a->duration_sec;
}

static void log_progress(struct app *a, double now) {
    if (now - a->last_report_sec < 2.0) return;
    double elapsed = now - a->start_sec;
    double workload_fps = elapsed > 0.0 ? (double)a->frame / elapsed : 0.0;
    double present_fps = elapsed > 0.0 ? (double)a->presented / elapsed : 0.0;
    fprintf(stderr,
            "STRESS_PROGRESS workload=%s pacing=%s size=%dx%d iters=%d frames=%llu presented=%llu workload_fps=%.2f presented_fps=%.2f gpu_samples=%zu\n",
            a->workload_name, a->pacing_name, a->width, a->height,
            a->iterations, a->frame, a->presented, workload_fps, present_fps,
            a->gpu_sample_count);
    a->last_report_sec = now;
}

static void log_summary(struct app *a) {
    double now = now_sec();
    double elapsed = now - a->start_sec;
    double *cpu_copy = NULL;
    double *gpu_copy = NULL;
    if (a->cpu_submit_count) {
        cpu_copy = malloc(a->cpu_submit_count * sizeof(*cpu_copy));
        if (cpu_copy) memcpy(cpu_copy, a->cpu_submit_samples,
                             a->cpu_submit_count * sizeof(*cpu_copy));
    }
    if (a->gpu_sample_count) {
        gpu_copy = malloc(a->gpu_sample_count * sizeof(*gpu_copy));
        if (gpu_copy) memcpy(gpu_copy, a->gpu_samples,
                             a->gpu_sample_count * sizeof(*gpu_copy));
    }
    fprintf(stderr,
            "STRESS_SUMMARY workload=%s pacing=%s precision=%s size=%dx%d iters=%d elapsed_s=%.3f frames=%llu presented=%llu workload_fps=%.2f presented_fps=%.2f cpu_samples=%zu gpu_samples=%zu\n",
            a->workload_name, a->pacing_name,
            a->selected_highp ? "highp" : "mediump", a->width, a->height,
            a->iterations, elapsed, a->frame, a->presented,
            elapsed > 0.0 ? (double)a->frame / elapsed : 0.0,
            elapsed > 0.0 ? (double)a->presented / elapsed : 0.0,
            a->cpu_submit_count, a->gpu_sample_count);
    if (cpu_copy) {
        fprintf(stderr, "STRESS_CPU_SUBMIT_MS avg=%.4f p50=%.4f p95=%.4f p99=%.4f\n",
                average(cpu_copy, a->cpu_submit_count),
                percentile(cpu_copy, a->cpu_submit_count, 0.5),
                percentile(cpu_copy, a->cpu_submit_count, 0.95),
                percentile(cpu_copy, a->cpu_submit_count, 0.99));
        free(cpu_copy);
    }
    if (gpu_copy) {
        fprintf(stderr, "STRESS_GPU_MS avg=%.4f p50=%.4f p95=%.4f p99=%.4f\n",
                average(gpu_copy, a->gpu_sample_count),
                percentile(gpu_copy, a->gpu_sample_count, 0.5),
                percentile(gpu_copy, a->gpu_sample_count, 0.95),
                percentile(gpu_copy, a->gpu_sample_count, 0.99));
        free(gpu_copy);
    }
}

static void render(struct app *a) {
    double now = now_sec();
    if (!a->running || benchmark_expired(a, now)) {
        a->running = 0;
        return;
    }
    if (!a->render_visible) return;

    double t = now;
    throttle_offscreen(a);
    if (a->pacing == STRESS_PACING_OFFSCREEN) {
        glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glViewport(0, 0, a->width, a->height);
    glUseProgram(a->program);

    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glEnableVertexAttribArray((GLuint)a->attr_pos);
    glVertexAttribPointer((GLuint)a->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glUniform2f(a->uni_resolution, (GLfloat)a->width, (GLfloat)a->height);
    glUniform1f(a->uni_time, (GLfloat)t);
    glUniform2f(a->uni_pointer, (GLfloat)a->pointer_x, (GLfloat)a->pointer_y);
    glUniform1f(a->uni_theme, (GLfloat)a->theme);

    if (a->workload == STRESS_WORKLOAD_BANDWIDTH) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a->bandwidth_texture0);
        glUniform1i(a->uni_texture0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, a->bandwidth_texture1);
        glUniform1i(a->uni_texture1, 1);
    }

    double submit_start = now_sec();
    int timer_slot = begin_gpu_timer(a);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    end_gpu_timer(a, timer_slot);
    double submit_end = now_sec();

    glDisableVertexAttribArray((GLuint)a->attr_pos);

    if (a->pacing == STRESS_PACING_FRAME) {
        if (a->frame_cb) wl_callback_destroy(a->frame_cb);
        a->frame_cb = wl_surface_frame(a->surface);
        wl_callback_add_listener(a->frame_cb, &frame_listener, a);
    }

    if (a->pacing != STRESS_PACING_OFFSCREEN) {
        if (!eglSwapBuffers(a->egl_display, a->egl_surface)) {
            die_egl("eglSwapBuffers");
            a->running = 0;
            return;
        }
        a->presented++;
    }

    a->frame++;
    if (a->pacing == STRESS_PACING_OFFSCREEN) a->offscreen_since_sync++;
    record_cpu_submit(a, (submit_end - submit_start) * 1000.0, submit_end);
    poll_gpu_timers(a);
    log_progress(a, submit_end);
}

static void shell_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
    (void)data;
    wl_shell_surface_pong(shell_surface, serial);
}

static void shell_configure(void *data, struct wl_shell_surface *shell_surface,
                            uint32_t edges, int32_t width, int32_t height) {
    (void)shell_surface;
    struct app *a = data;

    fprintf(stderr, "CONFIGURE edges=%u w=%d h=%d force4k=%d\n",
            edges, width, height, a->force_4k);

    if (!a->force_4k && width > 0 && height > 0) {
        a->width = width;
        a->height = height;
    }

    if (a->egl_window) {
        wl_egl_window_resize(a->egl_window, a->width, a->height, 0, 0);
    }

    set_surface_opaque(a);
    if (a->egl_context != EGL_NO_CONTEXT) {
        glViewport(0, 0, a->width, a->height);
    }
}

static void shell_popup_done(void *data, struct wl_shell_surface *shell_surface) {
    (void)data;
    (void)shell_surface;
}

static const struct wl_shell_surface_listener shell_listener = {
    .ping = shell_ping,
    .configure = shell_configure,
    .popup_done = shell_popup_done
};

static void input_pointer_enter(void *data, int x, int y) {
    struct app *a = data;
    a->pointer_x = x;
    a->pointer_y = y;
}

static void input_pointer_leave(void *data) {
    (void)data;
}

static void input_pointer_motion(void *data, int x, int y) {
    struct app *a = data;
    a->pointer_x = x;
    a->pointer_y = y;
}

static void input_pointer_button(
    void *data, uint32_t button, uint32_t state, int x, int y
) {
    struct app *a = data;
    a->pointer_x = x;
    a->pointer_y = y;

    if (button == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        a->theme++;
    }
}

static void input_keyboard_focus(void *data, int focused) {
    (void)data;
    fprintf(stderr, "KEYBOARD_FOCUS focused=%d\n", focused);
}

static void input_keyboard_key(void *data, uint32_t key, uint32_t state) {
    struct app *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    if (key == KEY_ESC || key == KEY_BACK) {
        a->running = 0;
        return;
    }

    if (key == KEY_ENTER) {
        a->theme++;
    } else if (key == KEY_LEFT) {
        a->theme += 1;
    } else if (key == KEY_RIGHT) {
        a->theme += 1;
    } else if (key == KEY_UP) {
        a->theme += 1;
    } else if (key == KEY_DOWN) {
        a->theme += 1;
    }
}

static const struct webos_input_callbacks input_callbacks = {
    .pointer_enter = input_pointer_enter,
    .pointer_leave = input_pointer_leave,
    .pointer_motion = input_pointer_motion,
    .pointer_button = input_pointer_button,
    .keyboard_focus = input_keyboard_focus,
    .keyboard_key = input_keyboard_key
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct app *a = data;

    fprintf(stderr, "GLOBAL %s v=%u id=%u\n", interface, version, name);

    if (webos_shell_try_bind(&a->webos_shell, registry, name, interface, version)) {
        return;
    }
    if (webos_input_try_bind(&a->input, registry, name, interface, version)) return;

    if (strcmp(interface, "wl_compositor") == 0) {
        a->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 3 ? version : 3
        );
    } else if (strcmp(interface, "wl_shell") == 0) {
        a->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    struct app *a = data;
    webos_input_global_remove(&a->input, name);
    webos_shell_global_remove(&a->webos_shell, name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove
};

static int configure_benchmark(struct app *a) {
    const char *pacing = getenv("STRESS_PACING");
    if (!pacing || strcmp(pacing, "frame") == 0) {
        a->pacing = STRESS_PACING_FRAME;
        copy_mode(a->pacing_name, sizeof(a->pacing_name), "frame", "frame");
    } else if (strcmp(pacing, "swap") == 0) {
        a->pacing = STRESS_PACING_SWAP;
        copy_mode(a->pacing_name, sizeof(a->pacing_name), "swap", "swap");
    } else if (strcmp(pacing, "offscreen") == 0) {
        a->pacing = STRESS_PACING_OFFSCREEN;
        copy_mode(a->pacing_name, sizeof(a->pacing_name), "offscreen", "offscreen");
    } else {
        fprintf(stderr, "STRESS_PACING_INVALID value=%s fallback=frame\n", pacing);
        a->pacing = STRESS_PACING_FRAME;
        copy_mode(a->pacing_name, sizeof(a->pacing_name), "frame", "frame");
    }

    const char *workload = getenv("STRESS_WORKLOAD");
    if (!workload || strcmp(workload, "alu") == 0) {
        a->workload = STRESS_WORKLOAD_ALU;
        copy_mode(a->workload_name, sizeof(a->workload_name), "alu", "alu");
    } else if (strcmp(workload, "sfu") == 0) {
        a->workload = STRESS_WORKLOAD_SFU;
        copy_mode(a->workload_name, sizeof(a->workload_name), "sfu", "sfu");
    } else if (strcmp(workload, "bandwidth") == 0) {
        a->workload = STRESS_WORKLOAD_BANDWIDTH;
        copy_mode(a->workload_name, sizeof(a->workload_name), "bandwidth", "bandwidth");
    } else {
        fprintf(stderr, "STRESS_WORKLOAD_INVALID value=%s fallback=alu\n", workload);
        a->workload = STRESS_WORKLOAD_ALU;
        copy_mode(a->workload_name, sizeof(a->workload_name), "alu", "alu");
    }

    a->iterations = parse_positive_env("STRESS_ITERS", 1, 1, 64);
    a->offscreen_batch = parse_positive_env("STRESS_OFFSCREEN_BATCH", 64, 1, 4096);
    const char *precision = getenv("STRESS_PRECISION");
    a->requested_highp = !precision || strcmp(precision, "mediump") != 0;
    if (precision && strcmp(precision, "highp") != 0 && strcmp(precision, "mediump") != 0 &&
        strcmp(precision, "auto") != 0) {
        fprintf(stderr, "STRESS_PRECISION_INVALID value=%s fallback=auto\n", precision);
        a->requested_highp = 1;
    }

    const char *color_mode = getenv("EGL_COLOR_MODE");
    if (!color_mode || strcmp(color_mode, "auto") == 0 || strcmp(color_mode, "8888") == 0 ||
        strcmp(color_mode, "rgb888") == 0 || strcmp(color_mode, "565") == 0) {
        copy_mode(a->color_mode, sizeof(a->color_mode), color_mode, "auto");
    } else {
        fprintf(stderr, "EGL_COLOR_MODE_INVALID value=%s fallback=auto\n", color_mode);
        copy_mode(a->color_mode, sizeof(a->color_mode), "auto", "auto");
    }

    const char *priority = getenv("EGL_CONTEXT_PRIORITY");
    if (!priority || strcmp(priority, "default") == 0 || strcmp(priority, "high") == 0) {
        copy_mode(a->priority_mode, sizeof(a->priority_mode), priority, "default");
    } else {
        fprintf(stderr, "EGL_CONTEXT_PRIORITY_INVALID value=%s fallback=default\n", priority);
        copy_mode(a->priority_mode, sizeof(a->priority_mode), "default", "default");
    }

    const char *force = getenv("STRESS_FORCE_4K");
    a->force_4k = force ? atoi(force) != 0 : 1;
    a->width = a->force_4k ? 3840 : 1920;
    a->height = a->force_4k ? 2160 : 1080;
    if (parse_resolution(a) < 0) return -1;
    fprintf(stderr,
            "STRESS_CONFIG workload=%s pacing=%s iters=%d precision=%s color=%s priority=%s size=%dx%d offscreen_batch=%d\n",
            a->workload_name, a->pacing_name, a->iterations,
            precision ? precision : "auto", a->color_mode, a->priority_mode,
            a->width, a->height, a->offscreen_batch);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct app a;
    memset(&a, 0, sizeof(a));
    if (configure_benchmark(&a) < 0) return 2;
    webos_input_context_init(&a.input, &a, &input_callbacks);
    webos_shell_context_init(
        &a.webos_shell, &a, webos_visibility_changed, webos_close_requested
    );

    a.running = 1;
    a.render_visible = 1;
    a.pointer_x = a.width / 2;
    a.pointer_y = a.height / 2;

    fprintf(stderr, "wayland_egl_stress start force4k=%d initial=%dx%d\n",
            a.force_4k, a.width, a.height);

    a.display = wl_display_connect(NULL);
    if (!a.display) {
        fprintf(stderr, "ERROR wl_display_connect failed\n");
        return 2;
    }

    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);

    if (!a.compositor || !a.shell) {
        fprintf(stderr, "ERROR missing compositor=%p shell=%p\n",
                (void*)a.compositor, (void*)a.shell);
        return 3;
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    if (!a.surface) {
        fprintf(stderr, "ERROR failed to create Wayland surface\n");
        return 4;
    }

    a.shell_surface = wl_shell_get_shell_surface(a.shell, a.surface);
    if (!a.shell_surface) {
        fprintf(stderr, "ERROR failed to create Wayland shell surface\n");
        return 4;
    }

    wl_shell_surface_add_listener(a.shell_surface, &shell_listener, &a);
    wl_shell_surface_set_title(a.shell_surface, "webOS Wayland EGL 4K Stress");
    wl_shell_surface_set_class(a.shell_surface, "org.webosbrew.wayland");
    wl_shell_surface_set_fullscreen(
        a.shell_surface,
        WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
        0,
        NULL
    );

    const char *app_id = getenv("APP_ID");
    webos_shell_attach(
        &a.webos_shell,
        a.surface,
        app_id ? app_id : "org.webosbrew.wayland"
    );

    set_surface_opaque(&a);

    if (init_egl(&a) < 0) {
        fprintf(stderr, "ERROR init_egl failed\n");
        return 4;
    }

    render(&a);

    if (a.pacing == STRESS_PACING_FRAME) {
        while (a.running) {
            int r = wl_display_dispatch(a.display);
            if (r < 0) {
                fprintf(stderr, "wl_display_dispatch returned %d\n", r);
                break;
            }
        }
    } else {
        while (a.running) {
            render(&a);
            wl_display_dispatch_pending(a.display);
            wl_display_flush(a.display);
        }
    }

    glFinish();
    poll_gpu_timers(&a);
    for (int i = 0; i < 4 && a.gpu_timer_supported; i++) {
        glFinish();
        poll_gpu_timers(&a);
    }
    log_summary(&a);

    fprintf(stderr, "wayland_egl_stress exit\n");

    if (a.frame_cb) wl_callback_destroy(a.frame_cb);

    if (a.vbo) glDeleteBuffers(1, &a.vbo);
    if (a.bandwidth_texture0) glDeleteTextures(1, &a.bandwidth_texture0);
    if (a.bandwidth_texture1) glDeleteTextures(1, &a.bandwidth_texture1);
    if (a.fbo) glDeleteFramebuffers(1, &a.fbo);
    if (a.offscreen_texture) glDeleteTextures(1, &a.offscreen_texture);
    if (a.gpu_timer_supported) {
        GLuint ids[STRESS_TIMER_SLOTS];
        for (int i = 0; i < STRESS_TIMER_SLOTS; i++) ids[i] = a.gpu_timers[i].id;
        a.delete_queries(STRESS_TIMER_SLOTS, ids);
    }
    free(a.cpu_submit_samples);
    free(a.gpu_samples);
    if (a.program) glDeleteProgram(a.program);

    if (a.egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(a.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (a.egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(a.egl_display, a.egl_surface);
        }

        if (a.egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(a.egl_display, a.egl_context);
        }

        eglTerminate(a.egl_display);
    }

    if (a.egl_window) wl_egl_window_destroy(a.egl_window);
    webos_input_context_destroy(&a.input);
    webos_shell_context_destroy(&a.webos_shell);
    if (a.shell_surface) wl_shell_surface_destroy(a.shell_surface);
    if (a.surface) wl_surface_destroy(a.surface);
    if (a.shell) wl_shell_destroy(a.shell);
    if (a.compositor) wl_compositor_destroy(a.compositor);
    if (a.registry) wl_registry_destroy(a.registry);
    if (a.display) wl_display_disconnect(a.display);

    return 0;
}
