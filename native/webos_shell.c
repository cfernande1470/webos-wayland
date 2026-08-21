#include "webos_shell.h"

#include <wayland-webos-shell-client-protocol.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void update_visibility(struct webos_shell_context *context) {
    int visible = context->state_visible &&
                  (!context->have_exposed || context->exposed_visible);

    if (visible == context->effective_visible) return;

    context->effective_visible = visible;
    fprintf(stderr, "WEBOS_SHELL_VISIBILITY visible=%d\n", visible);
    if (context->visibility_handler) {
        context->visibility_handler(context->data, visible);
    }
}

static void handle_state_changed(
    void *data,
    struct wl_webos_shell_surface *surface,
    uint32_t state
) {
    (void)surface;
    struct webos_shell_context *context = data;

    context->state = state;
    context->state_visible = state != WL_WEBOS_SHELL_SURFACE_STATE_MINIMIZED;
    fprintf(stderr, "WEBOS_SHELL_STATE state=%u\n", state);
    update_visibility(context);
}

static void handle_position_changed(
    void *data,
    struct wl_webos_shell_surface *surface,
    int32_t x,
    int32_t y
) {
    (void)data;
    (void)surface;
    fprintf(stderr, "WEBOS_SHELL_POSITION x=%d y=%d\n", x, y);
}

static void handle_close(void *data, struct wl_webos_shell_surface *surface) {
    (void)surface;
    struct webos_shell_context *context = data;

    fprintf(stderr, "WEBOS_SHELL_CLOSE\n");
    if (context->close_handler) context->close_handler(context->data);
}

static void handle_exposed(
    void *data,
    struct wl_webos_shell_surface *surface,
    struct wl_array *rectangles
) {
    (void)surface;
    struct webos_shell_context *context = data;
    const int32_t *values = rectangles ? rectangles->data : NULL;
    size_t count = rectangles ? rectangles->size / sizeof(*values) : 0;
    size_t rectangle_count = 0;
    int visible = 0;

    for (size_t i = 0; values && i < count;) {
        if (values[i] == -1) break;
        if (count - i < 4) {
            fprintf(stderr, "WEBOS_SHELL_EXPOSED malformed_values=%zu\n", count);
            break;
        }

        if (values[i + 2] > 0 && values[i + 3] > 0) visible = 1;
        rectangle_count++;
        i += 4;
    }

    context->have_exposed = 1;
    context->exposed_visible = visible;
    fprintf(stderr, "WEBOS_SHELL_EXPOSED rectangles=%zu visible=%d\n",
            rectangle_count, visible);
    update_visibility(context);
}

static void handle_state_about_to_change(
    void *data,
    struct wl_webos_shell_surface *surface,
    uint32_t state
) {
    (void)data;
    (void)surface;
    fprintf(stderr, "WEBOS_SHELL_STATE_PENDING state=%u\n", state);
}

static void handle_addon_status_changed(
    void *data,
    struct wl_webos_shell_surface *surface,
    uint32_t status
) {
    (void)data;
    (void)surface;
    fprintf(stderr, "WEBOS_SHELL_ADDON status=%u\n", status);
}

static const struct wl_webos_shell_surface_listener webos_surface_listener = {
    .state_changed = handle_state_changed,
    .position_changed = handle_position_changed,
    .close = handle_close,
    .exposed = handle_exposed,
    .state_about_to_change = handle_state_about_to_change,
    .addon_status_changed = handle_addon_status_changed
};

void webos_shell_context_init(
    struct webos_shell_context *context,
    void *data,
    webos_shell_visibility_handler visibility_handler,
    webos_shell_close_handler close_handler
) {
    memset(context, 0, sizeof(*context));
    context->data = data;
    context->visibility_handler = visibility_handler;
    context->close_handler = close_handler;
    context->state = WL_WEBOS_SHELL_SURFACE_STATE_DEFAULT;
    context->state_visible = 1;
    context->exposed_visible = 1;
    context->effective_visible = 1;
}

int webos_shell_try_bind(
    struct webos_shell_context *context,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
) {
    if (strcmp(interface, "wl_webos_shell") != 0) return 0;
    if (context->global) return 1;

    uint32_t bind_version = version < 2 ? version : 2;
    if (bind_version < 1) return 1;

    context->global = wl_registry_bind(
        registry, name, &wl_webos_shell_interface, bind_version
    );
    if (!context->global) {
        fprintf(stderr, "WEBOS_SHELL_BIND_FAILED advertised=%u\n", version);
        return 1;
    }

    context->registry_name = name;
    context->version = bind_version;
    fprintf(stderr, "WEBOS_SHELL_BOUND advertised=%u bound=%u id=%u\n",
            version, bind_version, name);
    return 1;
}

int webos_shell_attach(
    struct webos_shell_context *context,
    struct wl_surface *surface,
    const char *app_id
) {
    if (!context->global) {
        fprintf(stderr, "WEBOS_SHELL_FALLBACK reason=global-unavailable\n");
        return 0;
    }

    context->surface = wl_webos_shell_get_shell_surface(context->global, surface);
    if (!context->surface) {
        fprintf(stderr, "WEBOS_SHELL_FALLBACK reason=surface-creation-failed\n");
        return 0;
    }

    if (wl_webos_shell_surface_add_listener(
            context->surface, &webos_surface_listener, context) < 0) {
        fprintf(stderr, "WEBOS_SHELL_FALLBACK reason=listener-failed\n");
        wl_webos_shell_surface_destroy(context->surface);
        context->surface = NULL;
        return 0;
    }

    wl_webos_shell_surface_set_property(context->surface, "appId", app_id);
    wl_webos_shell_surface_set_property(context->surface, "displayAffinity", "0");
    wl_webos_shell_surface_set_key_mask(
        context->surface,
        (uint32_t)WL_WEBOS_SHELL_SURFACE_WEBOS_KEY_DEFAULT |
        (uint32_t)WL_WEBOS_SHELL_SURFACE_WEBOS_KEY_BACK |
        (uint32_t)WL_WEBOS_SHELL_SURFACE_WEBOS_KEY_EXIT
    );
    wl_webos_shell_surface_set_state(
        context->surface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN
    );

    fprintf(stderr, "WEBOS_SHELL_ATTACHED version=%u app_id=%s\n",
            context->version, app_id);
    return 1;
}

void webos_shell_global_remove(struct webos_shell_context *context, uint32_t name) {
    if (!context->global || context->registry_name != name) return;
    fprintf(stderr, "WEBOS_SHELL_GLOBAL_REMOVED id=%u\n", name);
}

void webos_shell_context_destroy(struct webos_shell_context *context) {
    if (context->surface) {
        wl_webos_shell_surface_destroy(context->surface);
        context->surface = NULL;
    }
    if (context->global) {
        wl_webos_shell_destroy(context->global);
        context->global = NULL;
    }
}
