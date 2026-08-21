#define _GNU_SOURCE
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "webos_input.h"
#include "webos_shell.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    GLint attr_pos;
    GLint uni_angle;
    GLint uni_offset;
    GLint uni_color;

    int width;
    int height;
    int running;
    int render_visible;
    int frame;
    int theme;

    int pointer_x;
    int pointer_y;
    int have_pointer;

    float offset_x;
    float offset_y;

    uint32_t first_frame_time;
    float animation_time;
    int have_frame_time;
};

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
        char log[2048];
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
        "uniform float u_angle;\n"
        "uniform vec2 u_offset;\n"
        "void main() {\n"
        "  float c = cos(u_angle);\n"
        "  float s = sin(u_angle);\n"
        "  mat2 r = mat2(c, -s, s, c);\n"
        "  vec2 p = r * a_pos * 0.55 + u_offset;\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";

    const char *fs =
        "precision mediump float;\n"
        "uniform vec3 u_color;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(u_color, 1.0);\n"
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
        char log[2048];
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

    a->egl_context = eglCreateContext(
        a->egl_display,
        a->egl_config,
        EGL_NO_CONTEXT,
        ctx_attrs
    );

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

    if (!eglSwapInterval(a->egl_display, 1)) {
        die_egl("eglSwapInterval");
        return -1;
    }

    fprintf(stderr, "GL_VENDOR %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION %s\n", glGetString(GL_VERSION));

    a->program = make_program();
    if (!a->program) return -1;

    a->attr_pos = glGetAttribLocation(a->program, "a_pos");
    a->uni_angle = glGetUniformLocation(a->program, "u_angle");
    a->uni_offset = glGetUniformLocation(a->program, "u_offset");
    a->uni_color = glGetUniformLocation(a->program, "u_color");

    if (a->attr_pos < 0 || a->uni_angle < 0 || a->uni_offset < 0 || a->uni_color < 0) {
        fprintf(stderr, "ERROR required shader attribute or uniform is unavailable\n");
        return -1;
    }

    const GLfloat verts[] = {
         0.0f,  0.75f,
        -0.75f, -0.55f,
         0.75f, -0.55f
    };

    glGenBuffers(1, &a->vbo);
    if (!a->vbo) {
        fprintf(stderr, "ERROR glGenBuffers failed\n");
        return -1;
    }
    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glViewport(0, 0, a->width, a->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

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
    struct app *a = data;

    if (cb) wl_callback_destroy(cb);
    a->frame_cb = NULL;

    if (!a->running || !a->render_visible) return;

    if (!a->have_frame_time) {
        a->first_frame_time = time;
        a->have_frame_time = 1;
    }
    a->animation_time = (float)(time - a->first_frame_time) * 0.001f;
    a->frame++;
    render(a);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

static void render(struct app *a) {
    if (!a->running || !a->render_visible) return;

    float t = a->animation_time;

    float bg_r = 0.02f;
    float bg_g = 0.03f;
    float bg_b = 0.08f;

    float cr = 0.2f;
    float cg = 0.8f;
    float cb = 1.0f;

    if ((a->theme % 3) == 1) {
        bg_r = 0.08f; bg_g = 0.02f; bg_b = 0.06f;
        cr = 1.0f; cg = 0.25f; cb = 0.75f;
    } else if ((a->theme % 3) == 2) {
        bg_r = 0.02f; bg_g = 0.07f; bg_b = 0.025f;
        cr = 0.3f; cg = 1.0f; cb = 0.35f;
    }

    glViewport(0, 0, a->width, a->height);
    glClearColor(bg_r, bg_g, bg_b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(a->program);
    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);

    glEnableVertexAttribArray((GLuint)a->attr_pos);
    glVertexAttribPointer((GLuint)a->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glUniform1f(a->uni_angle, t);
    glUniform2f(a->uni_offset, a->offset_x, a->offset_y);
    glUniform3f(a->uni_color, cr, cg, cb);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray((GLuint)a->attr_pos);

    if (a->frame_cb) wl_callback_destroy(a->frame_cb);
    a->frame_cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(a->frame_cb, &frame_listener, a);

    if (!eglSwapBuffers(a->egl_display, a->egl_surface)) {
        die_egl("eglSwapBuffers");
        a->running = 0;
    }

    if ((a->frame % 60) == 0) {
        fprintf(stderr, "EGL_FRAME %d theme=%d offset=%.2f,%.2f ptr=%d,%d\n",
                a->frame, a->theme, a->offset_x, a->offset_y,
                a->pointer_x, a->pointer_y);
    }
}

static void shell_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
    (void)data;
    wl_shell_surface_pong(shell_surface, serial);
}

static void shell_configure(void *data, struct wl_shell_surface *shell_surface,
                            uint32_t edges, int32_t width, int32_t height) {
    (void)shell_surface;
    struct app *a = data;

    fprintf(stderr, "CONFIGURE edges=%u w=%d h=%d\n", edges, width, height);

    if (width > 0 && height > 0) {
        a->width = width;
        a->height = height;

        if (a->egl_window) {
            wl_egl_window_resize(a->egl_window, width, height, 0, 0);
        }

        set_surface_opaque(a);
        if (a->egl_context != EGL_NO_CONTEXT) {
            glViewport(0, 0, a->width, a->height);
        }
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
    a->have_pointer = 1;
    a->pointer_x = x;
    a->pointer_y = y;
}

static void input_pointer_leave(void *data) {
    struct app *a = data;
    a->have_pointer = 0;
}

static void input_pointer_motion(void *data, int x, int y) {
    struct app *a = data;
    a->have_pointer = 1;
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

        if (a->width > 0 && a->height > 0) {
            a->offset_x = ((float)x / (float)a->width) * 2.0f - 1.0f;
            a->offset_y = 1.0f - ((float)y / (float)a->height) * 2.0f;
        }

        fprintf(stderr, "EGL_CLICK theme=%d offset=%.2f,%.2f\n",
                a->theme, a->offset_x, a->offset_y);
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
        fprintf(stderr, "EXIT_KEY key=%u\n", key);
        a->running = 0;
        return;
    }

    if (key == KEY_ENTER) {
        a->theme++;
    } else if (key == KEY_LEFT) {
        a->offset_x -= 0.08f;
    } else if (key == KEY_RIGHT) {
        a->offset_x += 0.08f;
    } else if (key == KEY_UP) {
        a->offset_y += 0.08f;
    } else if (key == KEY_DOWN) {
        a->offset_y -= 0.08f;
    }

    if (a->offset_x < -0.85f) a->offset_x = -0.85f;
    if (a->offset_x >  0.85f) a->offset_x =  0.85f;
    if (a->offset_y < -0.85f) a->offset_y = -0.85f;
    if (a->offset_y >  0.85f) a->offset_y =  0.85f;
    fprintf(stderr, "EGL_KEY_ACTION key=%u theme=%d offset=%.2f,%.2f\n",
            key, a->theme, a->offset_x, a->offset_y);
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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct app a;
    memset(&a, 0, sizeof(a));
    webos_input_context_init(&a.input, &a, &input_callbacks);
    webos_shell_context_init(
        &a.webos_shell, &a, webos_visibility_changed, webos_close_requested
    );

    a.width = 1920;
    a.height = 1080;
    a.running = 1;
    a.render_visible = 1;
    a.offset_x = 0.0f;
    a.offset_y = 0.0f;
    a.pointer_x = 960;
    a.pointer_y = 540;

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *wld = getenv("WAYLAND_DISPLAY");
    const char *app_id = getenv("APP_ID");

    fprintf(stderr, "wayland_egl start XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s APP_ID=%s\n",
            xdg ? xdg : "(null)",
            wld ? wld : "(null)",
            app_id ? app_id : "(null)");

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
    wl_shell_surface_set_title(a.shell_surface, "webOS Wayland EGL/GLES");
    wl_shell_surface_set_class(a.shell_surface, app_id ? app_id : "org.webosbrew.wayland");
    wl_shell_surface_set_fullscreen(
        a.shell_surface,
        WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
        0,
        NULL
    );

    webos_shell_attach(
        &a.webos_shell,
        a.surface,
        app_id ? app_id : "org.webosbrew.wayland"
    );

    set_surface_opaque(&a);

    if (init_egl(&a) < 0) {
        fprintf(stderr, "ERROR init_egl failed\n");
        return 5;
    }

    render(&a);

    while (a.running) {
        int r = wl_display_dispatch(a.display);
        if (r < 0) {
            fprintf(stderr, "wl_display_dispatch returned %d\n", r);
            break;
        }
    }

    fprintf(stderr, "wayland_egl exit\n");

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
