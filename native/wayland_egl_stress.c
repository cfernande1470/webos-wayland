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
#include <sched.h>
#include <errno.h>

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
#define STRESS_DEFAULT_TIMER_SLOTS 8
#define STRESS_MAX_TIMER_SLOTS 128

enum stress_pacing {
    STRESS_PACING_FRAME,
    STRESS_PACING_SWAP,
    STRESS_PACING_OFFSCREEN,
    STRESS_PACING_PBUFFER
};

enum stress_workload {
    STRESS_WORKLOAD_ALU,
    STRESS_WORKLOAD_SFU,
    STRESS_WORKLOAD_BANDWIDTH,
    STRESS_WORKLOAD_FILL,
    STRESS_WORKLOAD_OVERDRAW,
    STRESS_WORKLOAD_MULTIPASS,
    STRESS_WORKLOAD_BLUR,
    STRESS_WORKLOAD_DRAWS
};

enum stress_blend_mode {
    STRESS_BLEND_NONE,
    STRESS_BLEND_ALPHA,
    STRESS_BLEND_PREMULTIPLIED,
    STRESS_BLEND_ADDITIVE
};

struct stress_gpu_timer_slot {
    GLuint id;
    int active;
    double started_sec;
    unsigned long long sequence;
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
    int surface_backend;

    GLuint program;
    GLuint vbo;
    GLuint fbo;
    GLuint offscreen_texture;
    GLuint bandwidth_texture0;
    GLuint bandwidth_texture1;
    GLuint multipass_texture_a;
    GLuint multipass_texture_b;
    GLuint program_alt;

    GLint attr_pos;
    GLint uni_resolution;
    GLint uni_time;
    GLint uni_pointer;
    GLint uni_theme;
    GLint uni_texture0;
    GLint uni_texture1;
    GLint uni_layer;
    GLint uni_pass;

    int width;
    int height;
    int force_4k;
    int iterations;
    int requested_highp;
    int selected_highp;
    int offscreen_batch;
    int offscreen_since_sync;
    int timer_slots;
    int gpu_timer_requested;
    int gpu_timer_bits;
    int max_texture_size;
    int texture_size;
    int texture_samples;
    int texture_layout_atlas;
    int texture_compressed;
    int layers;
    int draws;
    int passes;
    int program_switches;
    int blur_taps;
    int batch;
    int blend_mode;
    char texture_format[16];
    char texture_format_effective[24];
    char texture_filter[16];
    char texture_pattern[16];
    char output_mode[16];
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
    unsigned long long measured_frames;
    unsigned long long measured_presented;
    int theme;

    int pointer_x;
    int pointer_y;

    double start_sec;
    double warmup_sec;
    double duration_sec;
    double last_report_sec;
    double *cpu_submit_samples;
    double *cpu_draw_samples;
    double *cpu_query_samples;
    double *cpu_swap_samples;
    double *cpu_frame_samples;
    double *callback_samples;
    double *gpu_samples;
    size_t cpu_submit_count;
    size_t cpu_draw_count;
    size_t cpu_query_count;
    size_t cpu_swap_count;
    size_t cpu_frame_count;
    size_t callback_count;
    size_t gpu_sample_count;
    struct stress_gpu_timer_slot *gpu_timers;
    unsigned long long timer_sequence;
    unsigned long long timer_ring_full;
    unsigned long long timer_waits;
    unsigned long long finish_count;
    unsigned long long timer_completed;
    unsigned long long timer_discarded;
    unsigned long long frame_backpressure;
    double last_callback_sec;
    int gpu_timer_supported;
    PFNGLGENQUERIESEXTPROC gen_queries;
    PFNGLDELETEQUERIESEXTPROC delete_queries;
    PFNGLBEGINQUERYEXTPROC begin_query;
    PFNGLENDQUERYEXTPROC end_query;
    PFNGLGETQUERYOBJECTIVEXTPROC get_query_objectiv;
    PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_objectui64v;
    PFNGLGETQUERYIVEXTPROC get_queryiv;
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
    const char *textures = (a->workload == STRESS_WORKLOAD_BANDWIDTH ||
                            a->workload == STRESS_WORKLOAD_MULTIPASS ||
                            a->workload == STRESS_WORKLOAD_BLUR)
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
            "    vec2 base = v_uv;\n"
            "    vec2 uv0 = fract(base * (1.0 + fi * 0.013) + vec2(t * 0.003, fi * 0.017));\n"
            "    vec2 uv1 = fract(base.yx * (1.0 + fi * 0.009) - vec2(fi * 0.011, t * 0.002));\n"
            "    #if STRESS_TEXTURE_PATTERN == 1\n"
            "    uv0 = fract(base * vec2(0.6180339, 0.7548776) * u_resolution.xy / 257.0 + fi * 0.031);\n"
            "    uv1 = fract(base.yx * vec2(0.5698403, 0.4385791) * u_resolution.xy / 251.0 - fi * 0.027);\n"
            "    #elif STRESS_TEXTURE_PATTERN == 2\n"
            "    vec2 h = fract(base * u_resolution.xy * vec2(0.7548776, 0.5698403) + fi * vec2(37.719, 19.193));\n"
            "    h = fract(h * (h + vec2(17.0, 59.4)));\n"
            "    uv0 = fract(base + h * 0.9375);\n"
            "    uv1 = fract(base.yx + h.yx * 0.8125);\n"
            "    #endif\n"
            "    for (int s = 0; s < STRESS_TEXTURE_SAMPLES; ++s) {\n"
            "      float fs = float(s);\n"
            "      vec4 tx0 = texture2D(u_texture0, fract(uv0 + fs * 0.013));\n"
            "      vec4 tx1 = texture2D(u_texture1, fract(uv1 + fs * 0.017));\n"
            "      acc += dot(tx0, tx1) + tx0.x * tx1.w;\n"
            "    }\n"
            "  }\n";
        break;
    case STRESS_WORKLOAD_FILL:
        loop =
            "  float keep = u_theme * 0.000001 + u_time * 0.000000001;\n"
            "  acc = keep;\n";
        break;
    case STRESS_WORKLOAD_OVERDRAW:
        loop =
            "  acc = 0.1 + u_layer * 0.013 + u_time * 0.000001;\n";
        break;
    case STRESS_WORKLOAD_MULTIPASS:
        loop =
            "  vec4 previous = texture2D(u_texture0, v_uv);\n"
            "  for (int i = 0; i < STRESS_ITERS; ++i) {\n"
            "    previous = previous * 0.985 + vec4(0.0031, 0.0047, 0.0023, 0.0);\n"
            "    acc += dot(previous.rgb, vec3(0.31, 0.47, 0.19));\n"
            "  }\n";
        break;
    case STRESS_WORKLOAD_BLUR:
        loop =
            "  vec4 blur_sum = vec4(0.0);\n"
            "  for (int i = 0; i < STRESS_BLUR_TAPS; ++i) {\n"
            "    float fi = float(i) - float(STRESS_BLUR_TAPS - 1) * 0.5;\n"
            "    vec2 direction = u_pass < 0.5 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);\n"
            "    blur_sum += texture2D(u_texture0, v_uv + direction * fi / u_resolution);\n"
            "  }\n"
            "  acc = dot(blur_sum.rgb, vec3(0.31, 0.47, 0.19));\n";
        break;
    case STRESS_WORKLOAD_DRAWS:
        loop =
            "  acc = 0.001 + u_layer * 0.000013;\n";
        break;
    }

    const char *final_output = a->workload == STRESS_WORKLOAD_FILL
        ? "  gl_FragColor = vec4(0.125 + acc, 0.25, 0.5, 1.0);\n"
        : (a->workload == STRESS_WORKLOAD_OVERDRAW || a->workload == STRESS_WORKLOAD_DRAWS)
        ? "  float layer_alpha = STRESS_BLEND_MODE == 0 ? 1.0 : 0.14;\n"
          "  vec3 layer_color = vec3(0.12 + fract(u_layer * 0.071), 0.25, 0.55);\n"
          "  gl_FragColor = vec4(STRESS_BLEND_MODE == 2 ? layer_color * layer_alpha : layer_color, layer_alpha);\n"
        : "  float grid = step(0.985, sin((uv.x + t * 0.03) * u_resolution.x * 0.16))\n"
          "             + step(0.985, sin((uv.y - t * 0.02) * u_resolution.y * 0.16));\n"
          "  float cursor = smoothstep(0.035, 0.0, distance(uv, mp));\n"
          "  vec3 av0 = vec3(0.0, 0.55, 1.0);\n"
          "  vec3 bv0 = vec3(1.0, 0.18, 0.72);\n"
          "  vec3 cv0 = vec3(0.35, 1.0, 0.28);\n"
          "  vec3 theme = mix(av0, bv0, step(0.5, mod(u_theme, 3.0)));\n"
          "  theme = mix(theme, cv0, step(1.5, mod(u_theme, 3.0)));\n"
          "  float stable = acc / (1.0 + abs(acc));\n"
          "  vec3 col = vec3(0.015, 0.02, 0.045) + theme * (0.25 + stable * 0.1);\n"
          "  col += vec3(grid) * 0.05 + cursor * vec3(1.0);\n"
          "  gl_FragColor = vec4(pow(abs(col), vec3(0.85)), 1.0);\n";

    char *fs = calloc(1, 16000);
    if (!fs) return 0;
    snprintf(fs, 16000,
        "%s\n#define STRESS_ITERS %d\n#define STRESS_TEXTURE_PATTERN %d\n#define STRESS_TEXTURE_SAMPLES %d\n#define STRESS_BLUR_TAPS %d\n#define STRESS_BLEND_MODE %d\n%s"
        "varying vec2 v_uv;\n"
        "uniform vec2 u_resolution;\n"
        "uniform float u_time;\n"
        "uniform vec2 u_pointer;\n"
        "uniform float u_theme;\n"
        "uniform float u_layer;\n"
        "uniform float u_pass;\n"
        "void main() {\n"
        "  vec2 uv = gl_FragCoord.xy / u_resolution.xy;\n"
        "  vec2 p = uv * 2.0 - 1.0;\n"
        "  p.x *= u_resolution.x / u_resolution.y;\n"
        "  vec2 mp = u_pointer / u_resolution.xy;\n"
        "  vec2 q = p;\n"
        "  float t = u_time;\n"
        "  float acc = 0.0;\n"
        "%s"
        "%s"
        "}\n",
        precision, a->iterations,
        strcmp(a->texture_pattern, "stride") == 0 ? 1 :
        strcmp(a->texture_pattern, "randomish") == 0 ? 2 : 0,
        a->texture_samples,
        a->blur_taps, a->blend_mode, textures, loop, final_output);

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

        EGLint required_surface_bit = a->pacing == STRESS_PACING_PBUFFER ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT;
        if (!(surface & required_surface_bit) || !(renderable & EGL_OPENGL_ES2_BIT) ||
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

static void apply_cpu_affinity(void) {
    const char *value = getenv("STRESS_CPU_AFFINITY");
    if (!value || !*value) return;
    char *end = NULL;
    long cpu = strtol(value, &end, 10);
    if (end == value || *end != '\0' || cpu < 0 || cpu >= CPU_SETSIZE) {
        fprintf(stderr, "STRESS_CPU_AFFINITY invalid=%s\n", value);
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int)cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "STRESS_CPU_AFFINITY cpu=%ld applied=0 errno=%d\n", cpu, errno);
    } else {
        fprintf(stderr, "STRESS_CPU_AFFINITY cpu=%ld applied=1\n", cpu);
    }
}

static int parse_resolution(struct app *a) {
    const char *preset = getenv("STRESS_RESOLUTION");
    if (preset) {
        if (strcmp(preset, "720p") == 0) {
            a->width = 1280; a->height = 720;
        } else if (strcmp(preset, "1080p") == 0) {
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

static void init_texture(struct app *a, GLuint texture, int size, int seed) {
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
    GLint min_filter = strcmp(a->texture_filter, "nearest") == 0 ? GL_NEAREST : GL_LINEAR;
    GLint mag_filter = strcmp(a->texture_filter, "nearest") == 0 ? GL_NEAREST : GL_LINEAR;
    if (strcmp(a->texture_filter, "trilinear") == 0) {
        min_filter = GL_LINEAR_MIPMAP_LINEAR;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (strcmp(a->texture_format, "rgb565") == 0) {
        uint16_t *rgb565 = malloc((size_t)size * (size_t)size * sizeof(*rgb565));
        if (rgb565) {
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    size_t p = ((size_t)y * (size_t)size + (size_t)x) * 4u;
                    rgb565[(size_t)y * (size_t)size + (size_t)x] =
                        (uint16_t)(((pixels[p] >> 3) << 11) |
                                   ((pixels[p + 1] >> 2) << 5) |
                                   (pixels[p + 2] >> 3));
                }
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, size, size, 0,
                         GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
            free(rgb565);
        }
    } else {
        if (strcmp(a->texture_format, "etc2") == 0 ||
            strcmp(a->texture_format, "astc") == 0) {
            fprintf(stderr, "STRESS_TEXTURE_FORMAT requested=%s asset=fallback_rgba8888\n",
                    a->texture_format);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    if (strcmp(a->texture_filter, "trilinear") == 0) glGenerateMipmap(GL_TEXTURE_2D);
    free(pixels);
}

static int init_offscreen_target(struct app *a) {
    if (a->pacing != STRESS_PACING_OFFSCREEN && a->pacing != STRESS_PACING_PBUFFER) return 0;

    glGenTextures(1, &a->offscreen_texture);
    glBindTexture(GL_TEXTURE_2D, a->offscreen_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, a->width, a->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    if (a->workload == STRESS_WORKLOAD_MULTIPASS || a->workload == STRESS_WORKLOAD_BLUR) {
        glGenTextures(1, &a->multipass_texture_b);
        glBindTexture(GL_TEXTURE_2D, a->multipass_texture_b);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, a->width, a->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        a->multipass_texture_a = a->offscreen_texture;
    }

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
    fprintf(stderr, "STRESS_FBO size=%dx%d status=complete backend=%s multipass=%d\n", a->width, a->height,
            a->pacing == STRESS_PACING_PBUFFER ?
                (a->surface_backend == 2 ? "surfaceless" : "pbuffer") : "window",
            a->multipass_texture_b ? 1 : 0);
    return 0;
}

static int init_bandwidth_textures(struct app *a) {
    if (a->workload != STRESS_WORKLOAD_BANDWIDTH) return 0;
    glGenTextures(1, &a->bandwidth_texture0);
    glGenTextures(1, &a->bandwidth_texture1);
    if (!a->bandwidth_texture0 || !a->bandwidth_texture1) return -1;
    init_texture(a, a->bandwidth_texture0, a->texture_size, 3);
    if (a->texture_layout_atlas) {
        a->bandwidth_texture1 = a->bandwidth_texture0;
    } else {
        init_texture(a, a->bandwidth_texture1, a->texture_size, 29);
    }
    fprintf(stderr, "STRESS_TEXTURES size=%dx%d count=%d working_set_mib=%.2f pattern=%s format=%s effective=%s filter=%s layout=%s samples=%d\n",
            a->texture_size, a->texture_size, a->texture_layout_atlas ? 1 : 2,
            a->texture_size * a->texture_size * (a->texture_layout_atlas ? 4.0 : 8.0) / (1024.0 * 1024.0),
            a->texture_pattern, a->texture_format, a->texture_format_effective,
            a->texture_filter,
            a->texture_layout_atlas ? "atlas" : "separate", a->texture_samples);
    return 0;
}

static int init_gpu_timer(struct app *a) {
    const char *requested = getenv("STRESS_GPU_TIMER");
    a->gpu_timer_requested = !requested || strcmp(requested, "off") != 0;
    if (!a->gpu_timer_requested) {
        fprintf(stderr, "STRESS_GPU_TIMER enabled=0 reason=disabled slots=%d\n", a->timer_slots);
        return 0;
    }
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!egl_extension_present(extensions, "GL_EXT_disjoint_timer_query")) {
        fprintf(stderr, "STRESS_GPU_TIMER enabled=0 reason=extension slots=%d\n", a->timer_slots);
        return 0;
    }

    a->gen_queries = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueriesEXT");
    a->delete_queries = (PFNGLDELETEQUERIESEXTPROC)eglGetProcAddress("glDeleteQueriesEXT");
    a->begin_query = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQueryEXT");
    a->end_query = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQueryEXT");
    a->get_query_objectiv = (PFNGLGETQUERYOBJECTIVEXTPROC)eglGetProcAddress("glGetQueryObjectivEXT");
    a->get_query_objectui64v = (PFNGLGETQUERYOBJECTUI64VEXTPROC)eglGetProcAddress("glGetQueryObjectui64vEXT");
    a->get_queryiv = (PFNGLGETQUERYIVEXTPROC)eglGetProcAddress("glGetQueryivEXT");
    if (!a->gen_queries || !a->delete_queries || !a->begin_query || !a->end_query ||
        !a->get_query_objectiv || !a->get_query_objectui64v || !a->get_queryiv) {
        fprintf(stderr, "STRESS_GPU_TIMER enabled=0 reason=entrypoint slots=%d\n", a->timer_slots);
        return 0;
    }

    a->gpu_timers = calloc((size_t)a->timer_slots, sizeof(*a->gpu_timers));
    if (!a->gpu_timers) return 0;
    GLuint *ids = calloc((size_t)a->timer_slots, sizeof(*ids));
    if (!ids) return 0;
    a->gen_queries(a->timer_slots, ids);
    for (int i = 0; i < a->timer_slots; i++) a->gpu_timers[i].id = ids[i];
    free(ids);
    GLint bits = 0;
    a->get_queryiv(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
    a->gpu_timer_bits = bits;
    GLboolean disjoint = GL_FALSE;
    glGetBooleanv(GL_GPU_DISJOINT_EXT, &disjoint);
    a->gpu_timer_supported = 1;
    fprintf(stderr, "STRESS_GPU_TIMER enabled=1 slots=%d counter_bits=%d initial_disjoint=%d\n",
            a->timer_slots, a->gpu_timer_bits, disjoint ? 1 : 0);
    return 1;
}

static void poll_gpu_timers(struct app *a) {
    if (!a->gpu_timer_supported) return;
    for (int i = 0; i < a->timer_slots; i++) {
        struct stress_gpu_timer_slot *slot = &a->gpu_timers[i];
        if (!slot->active) continue;
        GLint available = 0;
        a->get_query_objectiv(slot->id, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (!available) continue;
        GLuint64 nanoseconds = 0;
        a->get_query_objectui64v(slot->id, GL_QUERY_RESULT_EXT, &nanoseconds);
        GLboolean disjoint = GL_FALSE;
        glGetBooleanv(GL_GPU_DISJOINT_EXT, &disjoint);
        a->timer_completed++;
        if (!disjoint && slot->started_sec >= a->warmup_sec &&
            a->gpu_sample_count < STRESS_MAX_SAMPLES) {
            a->gpu_samples[a->gpu_sample_count++] = (double)nanoseconds / 1000000.0;
        } else if (disjoint) {
            a->timer_discarded++;
        }
        slot->active = 0;
    }
}

static int begin_gpu_timer(struct app *a) {
    if (!a->gpu_timer_supported) return -1;
    poll_gpu_timers(a);
    for (int i = 0; i < a->timer_slots; i++) {
        if (!a->gpu_timers[i].active) {
            a->begin_query(GL_TIME_ELAPSED_EXT, a->gpu_timers[i].id);
            a->gpu_timers[i].active = 1;
            a->gpu_timers[i].started_sec = now_sec();
            a->gpu_timers[i].sequence = ++a->timer_sequence;
            return i;
        }
    }
    a->timer_ring_full++;
    return -1;
}

static void end_gpu_timer(struct app *a, int slot) {
    if (slot >= 0) a->end_query(GL_TIME_ELAPSED_EXT);
}

static int throttle_offscreen(struct app *a) {
    if (a->pacing != STRESS_PACING_OFFSCREEN && a->pacing != STRESS_PACING_PBUFFER) return 0;
    if (a->gpu_timer_supported) {
        poll_gpu_timers(a);
        int slot_available = 0;
        for (int i = 0; i < a->timer_slots; i++) {
            if (!a->gpu_timers[i].active) {
                slot_available = 1;
                break;
            }
        }
        if (!slot_available) {
            a->timer_ring_full++;
            a->frame_backpressure++;
            struct stress_gpu_timer_slot *oldest = NULL;
            for (int i = 0; i < a->timer_slots; i++) {
                if (a->gpu_timers[i].active &&
                    (!oldest || a->gpu_timers[i].sequence < oldest->sequence)) {
                    oldest = &a->gpu_timers[i];
                }
            }
            if (oldest) {
                GLuint64 ignored = 0;
                a->get_query_objectui64v(oldest->id, GL_QUERY_RESULT_EXT, &ignored);
                a->timer_waits++;
                poll_gpu_timers(a);
            }
        }
    } else if (a->offscreen_since_sync >= a->offscreen_batch) {
        glFinish();
        a->finish_count++;
        a->frame_backpressure++;
        a->offscreen_since_sync = 0;
    }
    return 0;
}

static int init_egl(struct app *a) {
    if (a->pacing != STRESS_PACING_PBUFFER) {
        a->egl_window = wl_egl_window_create(a->surface, a->width, a->height);
        if (!a->egl_window) {
            fprintf(stderr, "ERROR wl_egl_window_create failed\n");
            return -1;
        }
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

    if (a->pacing == STRESS_PACING_PBUFFER) {
        const char *extensions = eglQueryString(a->egl_display, EGL_EXTENSIONS);
        int surfaceless = egl_extension_present(extensions, "EGL_KHR_surfaceless_context");
        if (surfaceless) {
            a->surface_backend = 2;
            a->egl_surface = EGL_NO_SURFACE;
            fprintf(stderr, "STRESS_BACKEND=surfaceless\n");
        } else {
            EGLint pbuffer_attrs[] = {
                EGL_WIDTH, a->width, EGL_HEIGHT, a->height, EGL_NONE
            };
            a->egl_surface = eglCreatePbufferSurface(
                a->egl_display, a->egl_config, pbuffer_attrs
            );
            a->surface_backend = 1;
            fprintf(stderr, "STRESS_BACKEND=pbuffer\n");
        }
    } else {
        a->egl_surface = eglCreateWindowSurface(
            a->egl_display,
            a->egl_config,
            (EGLNativeWindowType)a->egl_window,
            NULL
        );
        a->surface_backend = 0;
    }

    if (a->pacing != STRESS_PACING_PBUFFER && a->egl_surface == EGL_NO_SURFACE) {
        die_egl("eglCreateWindowSurface");
        return -1;
    }

    if (!eglMakeCurrent(a->egl_display,
                        a->egl_surface,
                        a->egl_surface,
                        a->egl_context)) {
        die_egl("eglMakeCurrent");
        return -1;
    }

    egl_log_capabilities(a->egl_display, "STRESS");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &a->max_texture_size);
    if (a->max_texture_size > 0 && a->texture_size > a->max_texture_size) {
        fprintf(stderr, "STRESS_TEXTURE_SIZE_CLAMP requested=%d max=%d\n",
                a->texture_size, a->max_texture_size);
        a->texture_size = a->max_texture_size;
    }
    fprintf(stderr, "STRESS_GL_LIMIT max_texture_size=%d\n", a->max_texture_size);

    int swap_interval = a->pacing == STRESS_PACING_FRAME ?
        parse_positive_env("STRESS_SWAP_INTERVAL", 1, 0, 4) : 0;
    if (a->pacing == STRESS_PACING_OFFSCREEN) swap_interval = 0;
    if (a->pacing != STRESS_PACING_PBUFFER && !eglSwapInterval(a->egl_display, swap_interval)) {
        die_egl("eglSwapInterval");
        return -1;
    }

    fprintf(stderr, "STRESS_SWAP_INTERVAL %d pacing=%s applied=%d\n", swap_interval,
            a->pacing_name, a->pacing == STRESS_PACING_PBUFFER ? 0 : 1);

    a->program = make_program(a);
    if (!a->program) return -1;

    a->attr_pos = glGetAttribLocation(a->program, "a_pos");
    a->uni_resolution = glGetUniformLocation(a->program, "u_resolution");
    a->uni_time = glGetUniformLocation(a->program, "u_time");
    a->uni_pointer = glGetUniformLocation(a->program, "u_pointer");
    a->uni_theme = glGetUniformLocation(a->program, "u_theme");
    a->uni_texture0 = glGetUniformLocation(a->program, "u_texture0");
    a->uni_texture1 = glGetUniformLocation(a->program, "u_texture1");
    a->uni_layer = glGetUniformLocation(a->program, "u_layer");
    a->uni_pass = glGetUniformLocation(a->program, "u_pass");

    if (a->attr_pos < 0 ||
        ((a->workload == STRESS_WORKLOAD_BANDWIDTH &&
          (a->uni_texture0 < 0 || a->uni_texture1 < 0)) ||
         ((a->workload == STRESS_WORKLOAD_MULTIPASS ||
           a->workload == STRESS_WORKLOAD_BLUR) && a->uni_texture0 < 0))) {
        fprintf(stderr, "ERROR required shader attribute or uniform is unavailable\n");
        return -1;
    }
    if (a->program_switches > 0) {
        a->program_alt = make_program(a);
        if (!a->program_alt) {
            fprintf(stderr, "ERROR alternate shader program unavailable\n");
            return -1;
        }
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
    a->cpu_draw_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->cpu_draw_samples));
    a->cpu_query_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->cpu_query_samples));
    a->cpu_swap_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->cpu_swap_samples));
    a->cpu_frame_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->cpu_frame_samples));
    a->callback_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->callback_samples));
    a->gpu_samples = calloc(STRESS_MAX_SAMPLES, sizeof(*a->gpu_samples));
    if (!a->cpu_submit_samples || !a->cpu_draw_samples || !a->cpu_query_samples ||
        !a->cpu_swap_samples || !a->cpu_frame_samples || !a->callback_samples ||
        !a->gpu_samples) {
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
static void record_metric(double *samples, size_t *count, double milliseconds,
                          double now, double warmup_sec);

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
    double callback_now = now_sec();

    if (a->last_callback_sec > 0.0) {
        record_metric(a->callback_samples, &a->callback_count,
                      (callback_now - a->last_callback_sec) * 1000.0,
                      callback_now, a->warmup_sec);
    }
    a->last_callback_sec = callback_now;

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

struct metric_stats {
    size_t count;
    double avg;
    double p50;
    double p95;
    double p99;
};

static struct metric_stats calculate_stats(const double *values, size_t count) {
    struct metric_stats stats = {0, 0.0, 0.0, 0.0, 0.0};
    stats.count = count;
    if (!values || count == 0) return stats;
    double *copy = malloc(count * sizeof(*copy));
    if (!copy) return stats;
    memcpy(copy, values, count * sizeof(*copy));
    stats.avg = average(copy, count);
    stats.p50 = percentile(copy, count, 0.50);
    stats.p95 = percentile(copy, count, 0.95);
    stats.p99 = percentile(copy, count, 0.99);
    free(copy);
    return stats;
}

static void log_metric_stats(const char *name, const struct metric_stats *stats) {
    fprintf(stderr, "STRESS_%s count=%zu avg=%.4f p50=%.4f p95=%.4f p99=%.4f\n",
            name, stats->count, stats->avg, stats->p50, stats->p95, stats->p99);
}

static void record_cpu_submit(struct app *a, double milliseconds, double now) {
    if (now < a->warmup_sec || a->cpu_submit_count >= STRESS_MAX_SAMPLES) return;
    a->cpu_submit_samples[a->cpu_submit_count++] = milliseconds;
}

static void record_metric(double *samples, size_t *count, double milliseconds,
                          double now, double warmup_sec) {
    if (now < warmup_sec || *count >= STRESS_MAX_SAMPLES) return;
    samples[(*count)++] = milliseconds;
}

static void record_frame_metrics(struct app *a, double frame_start, double frame_end,
                                 double draw_start, double draw_end,
                                 double query_milliseconds,
                                 double swap_start, double swap_end) {
    record_metric(a->cpu_frame_samples, &a->cpu_frame_count,
                  (frame_end - frame_start) * 1000.0, frame_end, a->warmup_sec);
    record_metric(a->cpu_draw_samples, &a->cpu_draw_count,
                  (draw_end - draw_start) * 1000.0, frame_end, a->warmup_sec);
    record_metric(a->cpu_query_samples, &a->cpu_query_count,
                  query_milliseconds, frame_end, a->warmup_sec);
    if (swap_end > swap_start) {
        record_metric(a->cpu_swap_samples, &a->cpu_swap_count,
                      (swap_end - swap_start) * 1000.0, frame_end, a->warmup_sec);
    }
}

static int benchmark_expired(struct app *a, double now) {
    return a->duration_sec > 0.0 && now - a->start_sec >= a->duration_sec;
}

static const char *blend_mode_name(int mode) {
    switch (mode) {
    case STRESS_BLEND_ALPHA: return "alpha";
    case STRESS_BLEND_PREMULTIPLIED: return "premultiplied";
    case STRESS_BLEND_ADDITIVE: return "additive";
    default: return "none";
    }
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
    struct metric_stats submit = calculate_stats(a->cpu_submit_samples, a->cpu_submit_count);
    struct metric_stats draw = calculate_stats(a->cpu_draw_samples, a->cpu_draw_count);
    struct metric_stats query = calculate_stats(a->cpu_query_samples, a->cpu_query_count);
    struct metric_stats swap = calculate_stats(a->cpu_swap_samples, a->cpu_swap_count);
    struct metric_stats frame = calculate_stats(a->cpu_frame_samples, a->cpu_frame_count);
    struct metric_stats callback = calculate_stats(a->callback_samples, a->callback_count);
    struct metric_stats gpu = calculate_stats(a->gpu_samples, a->gpu_sample_count);
    double pixels = (double)a->width * (double)a->height;
    double measured_elapsed = elapsed - (a->warmup_sec - a->start_sec);
    if (measured_elapsed < 0.0) measured_elapsed = 0.0;
    double measured_fps = measured_elapsed > 0.0 ?
        (double)a->measured_frames / measured_elapsed : 0.0;
    double mpixel_s = measured_fps * pixels / 1000000.0;
    double ns_pixel = measured_fps > 0.0 ? 1000000000.0 / (measured_fps * pixels) : 0.0;
    double estimated_flops = a->workload == STRESS_WORKLOAD_ALU ?
        measured_fps * pixels * (double)a->iterations * 23.0 / 1000000000.0 : 0.0;
    double texture_samples = a->workload == STRESS_WORKLOAD_BANDWIDTH ?
        measured_fps * pixels * (double)a->iterations *
        (double)a->texture_samples * 2.0 : 0.0;
    double gtexel_s = texture_samples / 1000000000.0;
    double approx_texture_bytes_s = texture_samples * 16.0;
    fprintf(stderr,
            "STRESS_SUMMARY workload=%s pacing=%s backend=%s precision=%s size=%dx%d iters=%d elapsed_s=%.3f frames=%llu presented=%llu measured_frames=%llu workload_fps=%.2f presented_fps=%.2f measured_fps=%.2f cpu_samples=%zu gpu_samples=%zu\n",
            a->workload_name, a->pacing_name,
            a->pacing == STRESS_PACING_PBUFFER ?
                (a->surface_backend == 2 ? "surfaceless" : "pbuffer") :
                (a->pacing == STRESS_PACING_OFFSCREEN ? "window-fbo" : "window"),
            a->selected_highp ? "highp" : "mediump", a->width, a->height,
            a->iterations, elapsed, a->frame, a->presented,
            a->measured_frames,
            elapsed > 0.0 ? (double)a->frame / elapsed : 0.0,
            elapsed > 0.0 ? (double)a->presented / elapsed : 0.0,
            measured_fps,
            a->cpu_submit_count, a->gpu_sample_count);
    log_metric_stats("CPU_SUBMIT_MS", &submit);
    log_metric_stats("CPU_DRAW_MS", &draw);
    log_metric_stats("CPU_QUERY_BEGIN_END_MS", &query);
    log_metric_stats("CPU_SWAP_MS", &swap);
    log_metric_stats("CPU_FRAME_TOTAL_MS", &frame);
    log_metric_stats("FRAME_CALLBACK_INTERVAL_MS", &callback);
    log_metric_stats("GPU_MS", &gpu);
    fprintf(stderr,
            "STRESS_COUNTERS timer_slots=%d timer_ring_full=%llu timer_waits=%llu finish=%llu query_completed=%llu query_discarded=%llu backpressure=%llu disjoint=%llu\n",
            a->timer_slots, a->timer_ring_full, a->timer_waits, a->finish_count,
            a->timer_completed, a->timer_discarded, a->frame_backpressure,
            a->timer_discarded);
    fprintf(stderr, "STRESS_THROUGHPUT mpixel_s=%.4f ns_pixel=%.4f estimated_alu_gflops=%.4f gtexel_s=%.4f approx_texture_bytes_s=%.0f\n",
            mpixel_s, ns_pixel, estimated_flops, gtexel_s, approx_texture_bytes_s);

    if (strcmp(a->output_mode, "jsonl") == 0 || strcmp(a->output_mode, "tsv") == 0) {
        if (strcmp(a->output_mode, "jsonl") == 0) {
            fprintf(stdout,
                "{\"timestamp\":%.3f,\"workload\":\"%s\",\"pacing\":\"%s\",\"backend\":\"%s\",\"resolution\":\"%dx%d\",\"width\":%d,\"height\":%d,\"precision\":\"%s\",\"iterations\":%d,\"layers\":%d,\"blend_mode\":\"%s\",\"passes\":%d,\"draw_calls\":%d,\"program_switches\":%d,\"batch\":%d,\"texture_size\":%d,\"texture_pattern\":\"%s\",\"texture_format\":\"%s\",\"texture_format_effective\":\"%s\",\"texture_filter\":\"%s\",\"texture_samples\":%d,\"texture_layout\":\"%s\",\"color_format\":\"%s\",\"context_priority\":\"%s\",\"gpu_timer\":%s,\"timer_slots\":%d,\"gpu_timer_bits\":%d,\"timer_waits\":%llu,\"backpressure\":%llu,\"frames\":%llu,\"presented\":%llu,\"measured_frames\":%llu,\"measured_presented\":%llu,\"workload_fps\":%.4f,\"presented_fps\":%.4f,\"gpu_avg_ms\":%.4f,\"gpu_p50_ms\":%.4f,\"gpu_p95_ms\":%.4f,\"gpu_p99_ms\":%.4f,\"cpu_draw_avg_ms\":%.4f,\"cpu_swap_avg_ms\":%.4f,\"cpu_total_avg_ms\":%.4f,\"mpixel_s\":%.4f,\"ns_pixel\":%.4f,\"timer_ring_full\":%llu,\"finish_count\":%llu,\"query_completed\":%llu,\"query_discarded\":%llu}\n",
                now, a->workload_name, a->pacing_name,
                a->pacing == STRESS_PACING_PBUFFER ? (a->surface_backend == 2 ? "surfaceless" : "pbuffer") :
                    (a->pacing == STRESS_PACING_OFFSCREEN ? "window-fbo" : "window"),
                a->width, a->height, a->width, a->height,
                a->selected_highp ? "highp" : "mediump", a->iterations,
                a->layers, blend_mode_name(a->blend_mode), a->passes, a->draws,
                a->program_switches, a->batch,
                a->texture_size, a->texture_pattern, a->texture_format,
                a->texture_format_effective,
                a->texture_filter, a->texture_samples,
                a->texture_layout_atlas ? "atlas" : "separate",
                a->color_mode, a->priority_mode,
                a->gpu_timer_supported ? "true" : "false", a->timer_slots,
                a->gpu_timer_bits, a->timer_waits, a->frame_backpressure,
                a->frame, a->presented, a->measured_frames, a->measured_presented, measured_fps,
                measured_elapsed > 0.0 ? (double)a->measured_presented / measured_elapsed : 0.0,
                gpu.avg, gpu.p50, gpu.p95, gpu.p99, draw.avg, swap.avg, frame.avg,
                mpixel_s, ns_pixel, a->timer_ring_full, a->finish_count,
                a->timer_completed, a->timer_discarded);
        } else {
            fprintf(stdout, "timestamp\tworkload\tpacing\tbackend\twidth\theight\tprecision\titers\tlayers\tblend_mode\tpasses\tdraw_calls\tprogram_switches\tbatch\ttexture_size\ttexture_pattern\ttexture_format\ttexture_format_effective\ttexture_filter\ttexture_samples\ttexture_layout\tgpu_timer\ttimer_slots\tgpu_timer_bits\ttimer_waits\tbackpressure\tframes\tpresented\tmeasured_frames\tmeasured_presented\tworkload_fps\tpresented_fps\tgpu_avg_ms\tgpu_p50_ms\tgpu_p95_ms\tgpu_p99_ms\tcpu_draw_avg_ms\tcpu_swap_avg_ms\tcpu_total_avg_ms\tmpixel_s\tns_pixel\ttimer_ring_full\tfinish_count\tquery_completed\tquery_discarded\n");
            fprintf(stdout, "%.3f\t%s\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%llu\t%llu\t%llu\t%llu\n",
                    now, a->workload_name, a->pacing_name,
                    a->pacing == STRESS_PACING_PBUFFER ? (a->surface_backend == 2 ? "surfaceless" : "pbuffer") :
                        (a->pacing == STRESS_PACING_OFFSCREEN ? "window-fbo" : "window"),
                    a->width, a->height, a->selected_highp ? "highp" : "mediump",
                    a->iterations, a->layers, blend_mode_name(a->blend_mode), a->passes,
                    a->draws, a->program_switches, a->batch, a->texture_size,
                    a->texture_pattern, a->texture_format, a->texture_format_effective,
                    a->texture_filter, a->texture_samples,
                    a->texture_layout_atlas ? "atlas" : "separate",
                    a->gpu_timer_supported, a->timer_slots, a->gpu_timer_bits,
                    a->timer_waits, a->frame_backpressure, a->frame,
                    a->presented, a->measured_frames, a->measured_presented,
                    measured_fps, measured_elapsed > 0.0 ? (double)a->measured_presented / measured_elapsed : 0.0,
                    gpu.avg, gpu.p50, gpu.p95, gpu.p99,
                    draw.avg, swap.avg, frame.avg, mpixel_s, ns_pixel,
                    a->timer_ring_full, a->finish_count, a->timer_completed,
                    a->timer_discarded);
        }
        fflush(stdout);
    }
}

static void render(struct app *a) {
    double frame_start = now_sec();
    double now = frame_start;
    if (!a->running || benchmark_expired(a, now)) {
        a->running = 0;
        return;
    }
    if (!a->render_visible &&
        (a->pacing == STRESS_PACING_FRAME || a->pacing == STRESS_PACING_SWAP)) return;

    double t = now;
    throttle_offscreen(a);
    if (a->pacing == STRESS_PACING_OFFSCREEN || a->pacing == STRESS_PACING_PBUFFER) {
        glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    int multipass = a->workload == STRESS_WORKLOAD_MULTIPASS ||
                    a->workload == STRESS_WORKLOAD_BLUR;
    int overdraw = a->workload == STRESS_WORKLOAD_OVERDRAW;
    int drawcalls = a->workload == STRESS_WORKLOAD_DRAWS;
    int pass_count = multipass ? a->passes : 1;
    int draw_count = overdraw ? a->layers : (drawcalls ? a->draws : 1);
    if (drawcalls && a->batch) draw_count = 1;

    glViewport(0, 0, drawcalls ? 1 : a->width, drawcalls ? 1 : a->height);
    glUseProgram(a->program);

    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glEnableVertexAttribArray((GLuint)a->attr_pos);
    glVertexAttribPointer((GLuint)a->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glUniform2f(a->uni_resolution, (GLfloat)a->width, (GLfloat)a->height);
    glUniform1f(a->uni_time, (GLfloat)t);
    glUniform2f(a->uni_pointer, (GLfloat)a->pointer_x, (GLfloat)a->pointer_y);
    glUniform1f(a->uni_theme, (GLfloat)a->theme);
    glUniform1f(a->uni_layer, 0.0f);
    glUniform1f(a->uni_pass, 0.0f);

    if (a->workload == STRESS_WORKLOAD_BANDWIDTH) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a->bandwidth_texture0);
        glUniform1i(a->uni_texture0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, a->bandwidth_texture1);
        glUniform1i(a->uni_texture1, 1);
    }

    if (multipass) {
        if (!a->fbo || !a->multipass_texture_b) {
            fprintf(stderr, "STRESS_MULTIPASS unavailable reason=offscreen_fbo_required\n");
            a->running = 0;
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, a->multipass_texture_a, 0);
        glClearColor(0.04f, 0.06f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (overdraw && a->blend_mode != STRESS_BLEND_NONE) {
        glEnable(GL_BLEND);
        if (a->blend_mode == STRESS_BLEND_ALPHA) {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else if (a->blend_mode == STRESS_BLEND_PREMULTIPLIED) {
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glBlendFunc(GL_ONE, GL_ONE);
        }
    } else {
        glDisable(GL_BLEND);
    }

    double query_start = now_sec();
    int timer_slot = begin_gpu_timer(a);
    double query_begin_end = now_sec();
    double draw_start = now_sec();
    GLuint source_texture = a->multipass_texture_a;
    GLuint target_texture = a->multipass_texture_b;
    for (int pass = 0; pass < pass_count; pass++) {
        if (multipass) {
            glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, target_texture, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, source_texture);
            glUniform1i(a->uni_texture0, 0);
            glUniform1f(a->uni_pass, (GLfloat)(pass & 1));
        }
        for (int draw = 0; draw < draw_count; draw++) {
            if (a->program_switches > 0 && draw < a->program_switches && a->program_alt) {
                glUseProgram((draw & 1) ? a->program_alt : a->program);
                glUniform2f(a->uni_resolution, (GLfloat)a->width, (GLfloat)a->height);
                glUniform1f(a->uni_time, (GLfloat)t);
                glUniform1f(a->uni_layer, (GLfloat)draw);
                glUniform1f(a->uni_pass, (GLfloat)(pass & 1));
            }
            glUniform1f(a->uni_layer, (GLfloat)draw);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (multipass) {
            GLuint swap = source_texture;
            source_texture = target_texture;
            target_texture = swap;
        }
    }
    double draw_end = now_sec();
    double query_end_start = now_sec();
    end_gpu_timer(a, timer_slot);
    double query_end = now_sec();

    glDisableVertexAttribArray((GLuint)a->attr_pos);
    glDisable(GL_BLEND);

    if (a->pacing == STRESS_PACING_FRAME) {
        if (a->frame_cb) wl_callback_destroy(a->frame_cb);
        a->frame_cb = wl_surface_frame(a->surface);
        wl_callback_add_listener(a->frame_cb, &frame_listener, a);
    }

    double swap_start = 0.0;
    double swap_end = 0.0;
    if (a->pacing == STRESS_PACING_FRAME || a->pacing == STRESS_PACING_SWAP) {
        swap_start = now_sec();
        if (!eglSwapBuffers(a->egl_display, a->egl_surface)) {
            die_egl("eglSwapBuffers");
            a->running = 0;
            return;
        }
        swap_end = now_sec();
        a->presented++;
    }

    a->frame++;
    if (query_end >= a->warmup_sec) {
        a->measured_frames++;
        if (swap_end > swap_start) a->measured_presented++;
    }
    if (a->pacing == STRESS_PACING_OFFSCREEN || a->pacing == STRESS_PACING_PBUFFER) {
        a->offscreen_since_sync++;
    }
    record_cpu_submit(a, (query_end - query_start) * 1000.0, query_end);
    record_frame_metrics(a, frame_start, now_sec(), draw_start, draw_end,
                         ((query_begin_end - query_start) +
                          (query_end - query_end_start)) * 1000.0,
                         swap_start, swap_end);
    poll_gpu_timers(a);
    log_progress(a, query_end);
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
    } else if (strcmp(pacing, "pbuffer") == 0 || strcmp(pacing, "surfaceless") == 0) {
        a->pacing = STRESS_PACING_PBUFFER;
        copy_mode(a->pacing_name, sizeof(a->pacing_name), "pbuffer", "pbuffer");
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
    } else if (strcmp(workload, "fill") == 0) {
        a->workload = STRESS_WORKLOAD_FILL;
        copy_mode(a->workload_name, sizeof(a->workload_name), "fill", "fill");
    } else if (strcmp(workload, "overdraw") == 0) {
        a->workload = STRESS_WORKLOAD_OVERDRAW;
        copy_mode(a->workload_name, sizeof(a->workload_name), "overdraw", "overdraw");
    } else if (strcmp(workload, "multipass") == 0) {
        a->workload = STRESS_WORKLOAD_MULTIPASS;
        copy_mode(a->workload_name, sizeof(a->workload_name), "multipass", "multipass");
    } else if (strcmp(workload, "blur") == 0) {
        a->workload = STRESS_WORKLOAD_BLUR;
        copy_mode(a->workload_name, sizeof(a->workload_name), "blur", "blur");
    } else if (strcmp(workload, "drawcalls") == 0) {
        a->workload = STRESS_WORKLOAD_DRAWS;
        copy_mode(a->workload_name, sizeof(a->workload_name), "drawcalls", "drawcalls");
    } else {
        fprintf(stderr, "STRESS_WORKLOAD_INVALID value=%s fallback=alu\n", workload);
        a->workload = STRESS_WORKLOAD_ALU;
        copy_mode(a->workload_name, sizeof(a->workload_name), "alu", "alu");
    }

    a->iterations = parse_positive_env("STRESS_ITERS", 1, 1, 64);
    a->layers = parse_positive_env("STRESS_LAYERS", 1, 1, 64);
    a->draws = parse_positive_env("STRESS_DRAWS", 1, 1, 2000);
    a->passes = parse_positive_env("STRESS_PASSES",
                                  a->workload == STRESS_WORKLOAD_BLUR ? 2 : 1,
                                  1, 16);
    a->program_switches = parse_positive_env("STRESS_PROGRAM_SWITCHES", 0, 0, 2000);
    a->texture_samples = parse_positive_env("STRESS_TEXTURE_SAMPLES", 1, 1, 64);
    a->blur_taps = parse_positive_env("STRESS_BLUR_TAPS", 5, 3, 9);
    a->batch = parse_positive_env("STRESS_BATCH", 0, 0, 1);
    a->offscreen_batch = parse_positive_env("STRESS_OFFSCREEN_BATCH", 64, 1, 4096);
    a->timer_slots = parse_positive_env("STRESS_TIMER_SLOTS",
                                        STRESS_DEFAULT_TIMER_SLOTS, 1,
                                        STRESS_MAX_TIMER_SLOTS);
    const char *pattern = getenv("STRESS_TEXTURE_PATTERN");
    if (!pattern || strcmp(pattern, "coherent") == 0 ||
        strcmp(pattern, "stride") == 0 || strcmp(pattern, "randomish") == 0) {
        copy_mode(a->texture_pattern, sizeof(a->texture_pattern), pattern, "coherent");
    } else {
        fprintf(stderr, "STRESS_TEXTURE_PATTERN_INVALID value=%s fallback=coherent\n", pattern);
        copy_mode(a->texture_pattern, sizeof(a->texture_pattern), "coherent", "coherent");
    }
    a->texture_size = parse_positive_env("STRESS_TEXTURE_SIZE", 256, 64, 8192);
    const char *texture_filter = getenv("STRESS_FILTER");
    if (!texture_filter || strcmp(texture_filter, "nearest") == 0 ||
        strcmp(texture_filter, "linear") == 0 || strcmp(texture_filter, "trilinear") == 0) {
        copy_mode(a->texture_filter, sizeof(a->texture_filter), texture_filter, "nearest");
    } else {
        fprintf(stderr, "STRESS_FILTER_INVALID value=%s fallback=nearest\n", texture_filter);
        copy_mode(a->texture_filter, sizeof(a->texture_filter), "nearest", "nearest");
    }
    const char *texture_format = getenv("STRESS_TEXTURE_FORMAT");
    if (!texture_format || strcmp(texture_format, "rgba8888") == 0 ||
        strcmp(texture_format, "rgb565") == 0 || strcmp(texture_format, "etc2") == 0 ||
        strcmp(texture_format, "astc") == 0) {
        copy_mode(a->texture_format, sizeof(a->texture_format), texture_format, "rgba8888");
    } else {
        fprintf(stderr, "STRESS_TEXTURE_FORMAT_INVALID value=%s fallback=rgba8888\n", texture_format);
        copy_mode(a->texture_format, sizeof(a->texture_format), "rgba8888", "rgba8888");
    }
    const char *texture_layout = getenv("STRESS_TEXTURE_LAYOUT");
    a->texture_layout_atlas = texture_layout && strcmp(texture_layout, "atlas") == 0;
    if (texture_layout && !a->texture_layout_atlas && strcmp(texture_layout, "separate") != 0) {
        fprintf(stderr, "STRESS_TEXTURE_LAYOUT_INVALID value=%s fallback=separate\n", texture_layout);
    }
    if (strcmp(a->texture_format, "etc2") == 0 ||
        strcmp(a->texture_format, "astc") == 0) {
        copy_mode(a->texture_format_effective, sizeof(a->texture_format_effective),
                  "rgba8888-fallback", "rgba8888-fallback");
    } else {
        copy_mode(a->texture_format_effective, sizeof(a->texture_format_effective),
                  a->texture_format, "rgba8888");
    }
    const char *blend = getenv("STRESS_BLEND");
    if (!blend || strcmp(blend, "none") == 0 || strcmp(blend, "off") == 0) {
        a->blend_mode = STRESS_BLEND_NONE;
    } else if (strcmp(blend, "alpha") == 0) {
        a->blend_mode = STRESS_BLEND_ALPHA;
    } else if (strcmp(blend, "premultiplied") == 0) {
        a->blend_mode = STRESS_BLEND_PREMULTIPLIED;
    } else if (strcmp(blend, "additive") == 0) {
        a->blend_mode = STRESS_BLEND_ADDITIVE;
    } else {
        fprintf(stderr, "STRESS_BLEND_INVALID value=%s fallback=none\n", blend);
        a->blend_mode = STRESS_BLEND_NONE;
    }
    const char *output = getenv("STRESS_OUTPUT");
    if (!output || strcmp(output, "human") == 0 || strcmp(output, "jsonl") == 0 ||
        strcmp(output, "tsv") == 0) {
        copy_mode(a->output_mode, sizeof(a->output_mode), output, "human");
    } else {
        fprintf(stderr, "STRESS_OUTPUT_INVALID value=%s fallback=human\n", output);
        copy_mode(a->output_mode, sizeof(a->output_mode), "human", "human");
    }
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
            "STRESS_CONFIG workload=%s pacing=%s iters=%d layers=%d draws=%d passes=%d precision=%s color=%s priority=%s size=%dx%d offscreen_batch=%d timer_slots=%d gpu_timer=%s texture_size=%d texture_pattern=%s texture_format=%s filter=%s samples=%d layout=%s blend=%d blur_taps=%d batch=%d output=%s\n",
            a->workload_name, a->pacing_name, a->iterations,
            a->layers, a->draws, a->passes,
            precision ? precision : "auto", a->color_mode, a->priority_mode,
            a->width, a->height, a->offscreen_batch, a->timer_slots,
            getenv("STRESS_GPU_TIMER") && strcmp(getenv("STRESS_GPU_TIMER"), "off") == 0 ? "off" : "on",
            a->texture_size, a->texture_pattern, a->texture_format,
            a->texture_filter, a->texture_samples,
            a->texture_layout_atlas ? "atlas" : "separate", a->blend_mode,
            a->blur_taps, a->batch, a->output_mode);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct app a;
    memset(&a, 0, sizeof(a));
    if (configure_benchmark(&a) < 0) return 2;
    apply_cpu_affinity();
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
    a.finish_count++;
    poll_gpu_timers(&a);
    for (int i = 0; i < 4 && a.gpu_timer_supported; i++) {
        glFinish();
        a.finish_count++;
        poll_gpu_timers(&a);
    }
    log_summary(&a);

    fprintf(stderr, "wayland_egl_stress exit\n");

    if (a.frame_cb) wl_callback_destroy(a.frame_cb);

    if (a.vbo) glDeleteBuffers(1, &a.vbo);
    if (a.bandwidth_texture0) glDeleteTextures(1, &a.bandwidth_texture0);
    if (a.bandwidth_texture1 && a.bandwidth_texture1 != a.bandwidth_texture0) {
        glDeleteTextures(1, &a.bandwidth_texture1);
    }
    if (a.fbo) glDeleteFramebuffers(1, &a.fbo);
    if (a.offscreen_texture) glDeleteTextures(1, &a.offscreen_texture);
    if (a.multipass_texture_b) glDeleteTextures(1, &a.multipass_texture_b);
    if (a.gpu_timer_supported && a.gpu_timers) {
        GLuint *ids = calloc((size_t)a.timer_slots, sizeof(*ids));
        if (ids) {
            for (int i = 0; i < a.timer_slots; i++) ids[i] = a.gpu_timers[i].id;
            a.delete_queries(a.timer_slots, ids);
            free(ids);
        }
    }
    free(a.cpu_submit_samples);
    free(a.cpu_draw_samples);
    free(a.cpu_query_samples);
    free(a.cpu_swap_samples);
    free(a.cpu_frame_samples);
    free(a.callback_samples);
    free(a.gpu_samples);
    free(a.gpu_timers);
    if (a.program_alt) glDeleteProgram(a.program_alt);
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
