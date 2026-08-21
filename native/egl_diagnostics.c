#include "egl_diagnostics.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

int egl_extension_present(const char *extensions, const char *name) {
    if (!extensions || !name || !*name) return 0;

    size_t name_length = strlen(name);
    const char *cursor = extensions;
    while ((cursor = strstr(cursor, name)) != NULL) {
        const char before = cursor == extensions ? ' ' : cursor[-1];
        const char after = cursor[name_length];
        if ((before == ' ' || before == '\0') &&
            (after == ' ' || after == '\0')) return 1;
        cursor += name_length;
    }
    return 0;
}

static void log_extension_flag(const char *scope, const char *extensions,
                               const char *name) {
    fprintf(stderr, "%s_EXTENSION name=%s available=%d\n",
            scope, name, egl_extension_present(extensions, name));
}

void egl_log_capabilities(EGLDisplay display, const char *label) {
    const char *egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);

    fprintf(stderr, "%s_EGL_VERSION %s\n", label,
            eglQueryString(display, EGL_VERSION));
    fprintf(stderr, "%s_EGL_VENDOR %s\n", label,
            eglQueryString(display, EGL_VENDOR));
    fprintf(stderr, "%s_EGL_CLIENT_APIS %s\n", label,
            eglQueryString(display, EGL_CLIENT_APIS));
    fprintf(stderr, "%s_EGL_EXTENSIONS %s\n", label,
            egl_extensions ? egl_extensions : "(null)");
    fprintf(stderr, "%s_GL_VENDOR %s\n", label,
            (const char *)glGetString(GL_VENDOR));
    fprintf(stderr, "%s_GL_RENDERER %s\n", label,
            (const char *)glGetString(GL_RENDERER));
    fprintf(stderr, "%s_GL_VERSION %s\n", label,
            (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "%s_GL_EXTENSIONS %s\n", label,
            gl_extensions ? gl_extensions : "(null)");

    log_extension_flag("EGL", egl_extensions, "EGL_KHR_partial_update");
    log_extension_flag("EGL", egl_extensions, "EGL_KHR_swap_buffers_with_damage");
    log_extension_flag("EGL", egl_extensions, "EGL_EXT_swap_buffers_with_damage");
    log_extension_flag("EGL", egl_extensions, "EGL_EXT_buffer_age");
    log_extension_flag("EGL", egl_extensions, "EGL_IMG_context_priority");
    log_extension_flag("EGL", egl_extensions, "EGL_ARM_implicit_external_sync");
    log_extension_flag("GL", gl_extensions, "GL_EXT_disjoint_timer_query");
    log_extension_flag("GL", gl_extensions, "GL_EXT_multisampled_render_to_texture");
    log_extension_flag("GL", gl_extensions, "GL_EXT_shader_framebuffer_fetch");
}
