#ifndef WEBOS_SHELL_H
#define WEBOS_SHELL_H

#include <stdint.h>
#include <wayland-client.h>

struct wl_webos_shell;
struct wl_webos_shell_surface;

typedef void (*webos_shell_visibility_handler)(void *data, int visible);
typedef void (*webos_shell_close_handler)(void *data);

struct webos_shell_context {
    struct wl_webos_shell *global;
    struct wl_webos_shell_surface *surface;
    void *data;
    webos_shell_visibility_handler visibility_handler;
    webos_shell_close_handler close_handler;
    uint32_t registry_name;
    uint32_t version;
    uint32_t state;
    int state_visible;
    int exposed_visible;
    int have_exposed;
    int effective_visible;
};

void webos_shell_context_init(
    struct webos_shell_context *context,
    void *data,
    webos_shell_visibility_handler visibility_handler,
    webos_shell_close_handler close_handler
);

int webos_shell_try_bind(
    struct webos_shell_context *context,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
);

int webos_shell_attach(
    struct webos_shell_context *context,
    struct wl_surface *surface,
    const char *app_id
);

void webos_shell_global_remove(struct webos_shell_context *context, uint32_t name);
void webos_shell_context_destroy(struct webos_shell_context *context);

#endif
