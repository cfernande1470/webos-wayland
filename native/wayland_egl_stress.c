#define _GNU_SOURCE
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "webos_shell.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef KEY_ESC
#define KEY_ESC 1
#define KEY_ENTER 28
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_BACK 158
#endif

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_callback *frame_cb;
    struct webos_shell_context webos_shell;

    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;

    GLuint program;
    GLuint vbo;

    GLint attr_pos;
    GLint uni_resolution;
    GLint uni_time;
    GLint uni_pointer;
    GLint uni_theme;

    int width;
    int height;
    int force_4k;
    int running;
    int render_visible;
    int frame;
    int theme;

    int pointer_x;
    int pointer_y;

    double start_sec;
    double last_sec;
    int last_frame;
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

static GLuint make_program(void) {
    const char *vs =
        "attribute vec2 a_pos;\n"
        "void main() {\n"
        "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";

    const char *fs =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "uniform vec2 u_resolution;\n"
        "uniform float u_time;\n"
        "uniform vec2 u_pointer;\n"
        "uniform float u_theme;\n"
        "\n"
        "void main() {\n"
        "  vec2 uv = gl_FragCoord.xy / u_resolution.xy;\n"
        "  vec2 p = uv * 2.0 - 1.0;\n"
        "  p.x *= u_resolution.x / u_resolution.y;\n"
        "\n"
        "  vec2 mp = u_pointer / u_resolution.xy;\n"
        "  vec2 q = p;\n"
        "  float t = u_time;\n"
        "  float acc = 0.0;\n"
        "  vec3 col = vec3(0.015, 0.02, 0.045);\n"
        "\n"
        "  for (int i = 0; i < 1; i++) {\n"
        "    float fi = float(i);\n"
        "    q = vec2(\n"
        "      sin(q.x * (1.7 + fi * 0.021) + t * 0.91 + fi * 0.13),\n"
        "      cos(q.y * (1.9 + fi * 0.017) - t * 0.73 + fi * 0.11)\n"
        "    ) + q.yx * 0.72;\n"
        "\n"
        "    float v = sin(q.x * 7.0 + q.y * 5.0 + t + fi * 0.03);\n"
        "    float ring = 0.018 / (0.02 + abs(length(q) - 0.45 - 0.18 * sin(t + fi)));\n"
        "    acc += v * 0.006 + ring * 0.0005;\n"
        "  }\n"
        "\n"
        "  float grid = step(0.985, sin((uv.x + t * 0.03) * u_resolution.x * 0.16))\n"
        "             + step(0.985, sin((uv.y - t * 0.02) * u_resolution.y * 0.16));\n"
        "\n"
        "  float d = distance(uv, mp);\n"
        "  float cursor = smoothstep(0.035, 0.0, d);\n"
        "\n"
        "  vec3 a = vec3(0.0, 0.55, 1.0);\n"
        "  vec3 b = vec3(1.0, 0.18, 0.72);\n"
        "  vec3 c = vec3(0.35, 1.0, 0.28);\n"
        "  vec3 theme = mix(a, b, step(0.5, mod(u_theme, 3.0)));\n"
        "  theme = mix(theme, c, step(1.5, mod(u_theme, 3.0)));\n"
        "\n"
        "  col += theme * (0.25 + acc);\n"
        "  col += vec3(grid) * 0.05;\n"
        "  col += cursor * vec3(1.0, 1.0, 1.0);\n"
        "  col = pow(abs(col), vec3(0.85));\n"
        "\n"
        "  gl_FragColor = vec4(col, 1.0);\n"
        "}\n";

    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
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
            red < 8 || green < 8 || blue < 8 || caveat == EGL_SLOW_CONFIG) {
            continue;
        }

        int score = (alpha == 0 ? 10000 : 0) - alpha * 100 - depth * 10 - stencil * 10;
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

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    a->egl_context = eglCreateContext(a->egl_display, a->egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (a->egl_context == EGL_NO_CONTEXT) {
        die_egl("eglCreateContext");
        return -1;
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

    const char *swap_env = getenv("STRESS_SWAP_INTERVAL");
    int swap_interval = swap_env ? atoi(swap_env) : 1;
    if (!eglSwapInterval(a->egl_display, swap_interval)) {
        die_egl("eglSwapInterval");
        return -1;
    }

    fprintf(stderr, "STRESS_SWAP_INTERVAL %d\n", swap_interval);
    fprintf(stderr, "GL_VENDOR %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION %s\n", glGetString(GL_VERSION));

    a->program = make_program();
    if (!a->program) return -1;

    a->attr_pos = glGetAttribLocation(a->program, "a_pos");
    a->uni_resolution = glGetUniformLocation(a->program, "u_resolution");
    a->uni_time = glGetUniformLocation(a->program, "u_time");
    a->uni_pointer = glGetUniformLocation(a->program, "u_pointer");
    a->uni_theme = glGetUniformLocation(a->program, "u_theme");

    if (a->attr_pos < 0 || a->uni_resolution < 0 || a->uni_time < 0 ||
        a->uni_pointer < 0 || a->uni_theme < 0) {
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

    a->start_sec = now_sec();
    a->last_sec = a->start_sec;
    a->last_frame = 0;

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

    a->frame++;
    render(a);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

static void log_stats(struct app *a) {
    double now = now_sec();

    double dt = now - a->last_sec;
    double total = now - a->start_sec;

    int frames = a->frame - a->last_frame;

    if (dt >= 2.0) {
        double fps = (double)frames / dt;
        double avg = total > 0.0 ? (double)a->frame / total : 0.0;

        fprintf(stderr,
                "STRESS_FRAME frame=%d size=%dx%d fps=%.2f avg=%.2f theme=%d ptr=%d,%d force4k=%d\n",
                a->frame, a->width, a->height, fps, avg,
                a->theme, a->pointer_x, a->pointer_y, a->force_4k);

        a->last_sec = now;
        a->last_frame = a->frame;
    }
}

static void render(struct app *a) {
    if (!a->running || !a->render_visible) return;

    double t = now_sec();

    glViewport(0, 0, a->width, a->height);
    glUseProgram(a->program);

    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glEnableVertexAttribArray((GLuint)a->attr_pos);
    glVertexAttribPointer((GLuint)a->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glUniform2f(a->uni_resolution, (GLfloat)a->width, (GLfloat)a->height);
    glUniform1f(a->uni_time, (GLfloat)t);
    glUniform2f(a->uni_pointer, (GLfloat)a->pointer_x, (GLfloat)a->pointer_y);
    glUniform1f(a->uni_theme, (GLfloat)a->theme);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray((GLuint)a->attr_pos);

    if (a->frame_cb) wl_callback_destroy(a->frame_cb);
    a->frame_cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(a->frame_cb, &frame_listener, a);

    if (!eglSwapBuffers(a->egl_display, a->egl_surface)) {
        die_egl("eglSwapBuffers");
        a->running = 0;
        return;
    }

    log_stats(a);
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

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    (void)surface;

    struct app *a = data;
    a->pointer_x = wl_fixed_to_int(sx);
    a->pointer_y = wl_fixed_to_int(sy);

    fprintf(stderr, "POINTER_ENTER x=%d y=%d\n", a->pointer_x, a->pointer_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;

    fprintf(stderr, "POINTER_LEAVE\n");
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)time;

    struct app *a = data;
    a->pointer_x = wl_fixed_to_int(sx);
    a->pointer_y = wl_fixed_to_int(sy);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;

    struct app *a = data;

    fprintf(stderr, "POINTER_BUTTON button=%u state=%u x=%d y=%d\n",
            button, state, a->pointer_x, a->pointer_y);

    if (button == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        a->theme++;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;

    if (fd >= 0) close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;

    fprintf(stderr, "KEYBOARD_ENTER\n");
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;

    fprintf(stderr, "KEYBOARD_LEAVE\n");
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;

    struct app *a = data;

    fprintf(stderr, "KEY key=%u state=%u\n", key, state);

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

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t mods_depressed, uint32_t mods_latched,
                               uint32_t mods_locked, uint32_t group) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct app *a = data;

    fprintf(stderr, "SEAT_CAPS caps=%u seat=%p\n", caps, (void*)seat);

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        struct wl_pointer *ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ptr, &pointer_listener, a);
        a->pointer = ptr;
        fprintf(stderr, "POINTER_ATTACHED seat=%p ptr=%p\n", (void*)seat, (void*)ptr);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        wl_pointer_release(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        struct wl_keyboard *kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(kbd, &keyboard_listener, a);
        a->keyboard = kbd;
        fprintf(stderr, "KEYBOARD_ATTACHED seat=%p kbd=%p\n", (void*)seat, (void*)kbd);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_release(a->keyboard);
        a->keyboard = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct app *a = data;

    fprintf(stderr, "GLOBAL %s v=%u id=%u\n", interface, version, name);

    if (webos_shell_try_bind(&a->webos_shell, registry, name, interface, version)) {
        return;
    }

    if (strcmp(interface, "wl_compositor") == 0) {
        a->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 3 ? version : 3
        );
    } else if (strcmp(interface, "wl_shell") == 0) {
        a->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0 && !a->seat) {
        struct wl_seat *seat = wl_registry_bind(
            registry, name, &wl_seat_interface, version < 3 ? version : 3
        );
        wl_seat_add_listener(seat, &seat_listener, a);
        a->seat = seat;
        fprintf(stderr, "BIND_SEAT id=%u proxy=%p version=%u\n", name, (void*)seat, version);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    struct app *a = data;
    webos_shell_global_remove(&a->webos_shell, name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove
};

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct app a;
    memset(&a, 0, sizeof(a));
    webos_shell_context_init(
        &a.webos_shell, &a, webos_visibility_changed, webos_close_requested
    );

    a.force_4k = getenv("STRESS_FORCE_4K") ? atoi(getenv("STRESS_FORCE_4K")) : 1;
    a.width = a.force_4k ? 3840 : 1920;
    a.height = a.force_4k ? 2160 : 1080;
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

    while (a.running) {
        int r = wl_display_dispatch(a.display);
        if (r < 0) {
            fprintf(stderr, "wl_display_dispatch returned %d\n", r);
            break;
        }
    }

    fprintf(stderr, "wayland_egl_stress exit\n");

    if (a.frame_cb) wl_callback_destroy(a.frame_cb);

    if (a.vbo) glDeleteBuffers(1, &a.vbo);
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
    if (a.pointer) wl_pointer_release(a.pointer);
    if (a.keyboard) wl_keyboard_release(a.keyboard);
    if (a.seat) wl_seat_release(a.seat);
    webos_shell_context_destroy(&a.webos_shell);
    if (a.shell_surface) wl_shell_surface_destroy(a.shell_surface);
    if (a.surface) wl_surface_destroy(a.surface);
    if (a.shell) wl_shell_destroy(a.shell);
    if (a.compositor) wl_compositor_destroy(a.compositor);
    if (a.registry) wl_registry_destroy(a.registry);
    if (a.display) wl_display_disconnect(a.display);

    return 0;
}
