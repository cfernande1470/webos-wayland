#ifndef EGL_DIAGNOSTICS_H
#define EGL_DIAGNOSTICS_H

#include <EGL/egl.h>

int egl_extension_present(const char *extensions, const char *name);
void egl_log_capabilities(EGLDisplay display, const char *label);

#endif
