#define _GNU_SOURCE
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

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
    GLint uni_angle;
    GLint uni_offset;
    GLint uni_color;

    int width;
    int height;
    int running;
    int frame;
    int theme;

    int pointer_x;
    int pointer_y;
    int have_pointer;

    float offset_x;
    float offset_y;
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
    if (!v || !f) return 0;

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

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLint ncfg = 0;
    if (!eglChooseConfig(a->egl_display, cfg_attrs, &a->egl_config, 1, &ncfg) || ncfg < 1) {
        die_egl("eglChooseConfig");
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

    eglSwapInterval(a->egl_display, 1);

    fprintf(stderr, "GL_VENDOR %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION %s\n", glGetString(GL_VERSION));

    a->program = make_program();
    if (!a->program) return -1;

    a->attr_pos = glGetAttribLocation(a->program, "a_pos");
    a->uni_angle = glGetUniformLocation(a->program, "u_angle");
    a->uni_offset = glGetUniformLocation(a->program, "u_offset");
    a->uni_color = glGetUniformLocation(a->program, "u_color");

    const GLfloat verts[] = {
         0.0f,  0.75f,
        -0.75f, -0.55f,
         0.75f, -0.55f
    };

    glGenBuffers(1, &a->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, a->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glViewport(0, 0, a->width, a->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    return 0;
}

static void render(struct app *a);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    struct app *a = data;

    if (cb) wl_callback_destroy(cb);
    a->frame_cb = NULL;

    if (!a->running) return;

    a->frame++;
    render(a);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

static void render(struct app *a) {
    float t = (float)a->frame * 0.016f;

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
    a->have_pointer = 1;
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
    a->have_pointer = 1;
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

        if (a->width > 0 && a->height > 0) {
            a->offset_x = ((float)a->pointer_x / (float)a->width) * 2.0f - 1.0f;
            a->offset_y = 1.0f - ((float)a->pointer_y / (float)a->height) * 2.0f;
        }

        fprintf(stderr, "EGL_CLICK theme=%d offset=%.2f,%.2f\n",
                a->theme, a->offset_x, a->offset_y);
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

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        struct wl_pointer *ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ptr, &pointer_listener, a);
        a->pointer = ptr;
        fprintf(stderr, "POINTER_ATTACHED seat=%p ptr=%p\n", (void*)seat, (void*)ptr);
    }

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(kbd, &keyboard_listener, a);
        a->keyboard = kbd;
        fprintf(stderr, "KEYBOARD_ATTACHED seat=%p kbd=%p\n", (void*)seat, (void*)kbd);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct app *a = data;

    fprintf(stderr, "GLOBAL %s v=%u id=%u\n", interface, version, name);

    if (strcmp(interface, "wl_compositor") == 0) {
        a->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 3 ? version : 3
        );
    } else if (strcmp(interface, "wl_shell") == 0) {
        a->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        struct wl_seat *seat = wl_registry_bind(
            registry, name, &wl_seat_interface, version < 3 ? version : 3
        );
        wl_seat_add_listener(seat, &seat_listener, a);
        a->seat = seat;
        fprintf(stderr, "BIND_SEAT id=%u proxy=%p version=%u\n", name, (void*)seat, version);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
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

    a.width = 1920;
    a.height = 1080;
    a.running = 1;
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
    a.shell_surface = wl_shell_get_shell_surface(a.shell, a.surface);

    wl_shell_surface_add_listener(a.shell_surface, &shell_listener, &a);
    wl_shell_surface_set_title(a.shell_surface, "webOS Wayland EGL/GLES");
    wl_shell_surface_set_class(a.shell_surface, app_id ? app_id : "org.webosbrew.wayland");
    wl_shell_surface_set_fullscreen(
        a.shell_surface,
        WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
        0,
        NULL
    );

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
    if (a.display) wl_display_disconnect(a.display);

    return 0;
}
