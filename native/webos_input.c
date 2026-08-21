#include "webos_input.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define INPUT_DUPLICATE_WINDOW_MS 20u

static int recent_duplicate(
    uint32_t time,
    uint32_t previous_time,
    uint32_t value,
    uint32_t previous_value,
    uint32_t state,
    uint32_t previous_state,
    int have_previous
) {
    return have_previous && value == previous_value && state == previous_state &&
           time - previous_time <= INPUT_DUPLICATE_WINDOW_MS;
}

static void clear_pointer_focus(struct webos_input_seat *input_seat) {
    struct webos_input_context *context = input_seat->context;

    if (!input_seat->pointer_focused) return;
    input_seat->pointer_focused = 0;
    if (context->pointer_focus_count > 0) context->pointer_focus_count--;
    if (context->pointer_focus_count == 0 && context->callbacks.pointer_leave) {
        context->callbacks.pointer_leave(context->data);
    }
}

static void clear_keyboard_focus(struct webos_input_seat *input_seat) {
    struct webos_input_context *context = input_seat->context;

    if (!input_seat->keyboard_focused) return;
    input_seat->keyboard_focused = 0;
    if (context->keyboard_focus_count > 0) context->keyboard_focus_count--;
    if (context->keyboard_focus_count == 0 && context->callbacks.keyboard_focus) {
        context->callbacks.keyboard_focus(context->data, 0);
    }
}

static void destroy_pointer(struct webos_input_seat *input_seat) {
    if (!input_seat->pointer) return;

    if (wl_proxy_get_version((struct wl_proxy *)input_seat->pointer) >=
        WL_POINTER_RELEASE_SINCE_VERSION) {
        wl_pointer_release(input_seat->pointer);
    } else {
        wl_pointer_destroy(input_seat->pointer);
    }
    input_seat->pointer = NULL;
}

static void destroy_keyboard(struct webos_input_seat *input_seat) {
    if (!input_seat->keyboard) return;

    if (wl_proxy_get_version((struct wl_proxy *)input_seat->keyboard) >=
        WL_KEYBOARD_RELEASE_SINCE_VERSION) {
        wl_keyboard_release(input_seat->keyboard);
    } else {
        wl_keyboard_destroy(input_seat->keyboard);
    }
    input_seat->keyboard = NULL;
}

static void destroy_seat_proxy(struct webos_input_seat *input_seat) {
    if (!input_seat->seat) return;

    if (wl_proxy_get_version((struct wl_proxy *)input_seat->seat) >=
        WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(input_seat->seat);
    } else {
        wl_seat_destroy(input_seat->seat);
    }
    input_seat->seat = NULL;
}

static void pointer_enter(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t sx,
    wl_fixed_t sy
) {
    (void)pointer;
    (void)serial;
    (void)surface;
    struct webos_input_seat *input_seat = data;
    struct webos_input_context *context = input_seat->context;

    input_seat->pointer_x = wl_fixed_to_int(sx);
    input_seat->pointer_y = wl_fixed_to_int(sy);
    if (!input_seat->pointer_focused) {
        input_seat->pointer_focused = 1;
        context->pointer_focus_count++;
    }

    fprintf(stderr, "POINTER_ENTER seat=%u x=%d y=%d\n",
            input_seat->index, input_seat->pointer_x, input_seat->pointer_y);
    if (context->callbacks.pointer_enter) {
        context->callbacks.pointer_enter(
            context->data, input_seat->pointer_x, input_seat->pointer_y
        );
    }
}

static void pointer_leave(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    struct wl_surface *surface
) {
    (void)pointer;
    (void)serial;
    (void)surface;
    struct webos_input_seat *input_seat = data;

    clear_pointer_focus(input_seat);

    fprintf(stderr, "POINTER_LEAVE seat=%u\n", input_seat->index);
}

static void pointer_motion(
    void *data,
    struct wl_pointer *pointer,
    uint32_t time,
    wl_fixed_t sx,
    wl_fixed_t sy
) {
    (void)pointer;
    (void)time;
    struct webos_input_seat *input_seat = data;
    struct webos_input_context *context = input_seat->context;

    input_seat->pointer_x = wl_fixed_to_int(sx);
    input_seat->pointer_y = wl_fixed_to_int(sy);
    if (context->callbacks.pointer_motion) {
        context->callbacks.pointer_motion(
            context->data, input_seat->pointer_x, input_seat->pointer_y
        );
    }
}

static void pointer_button(
    void *data,
    struct wl_pointer *pointer,
    uint32_t serial,
    uint32_t time,
    uint32_t button,
    uint32_t state
) {
    (void)pointer;
    (void)serial;
    struct webos_input_seat *input_seat = data;
    struct webos_input_context *context = input_seat->context;

    fprintf(stderr, "POINTER_BUTTON seat=%u button=%u state=%u x=%d y=%d\n",
            input_seat->index, button, state,
            input_seat->pointer_x, input_seat->pointer_y);

    if (recent_duplicate(
            time, context->last_button_time,
            button, context->last_button,
            state, context->last_button_state,
            context->have_last_button)) {
        fprintf(stderr, "INPUT_DUPLICATE type=pointer_button seat=%u\n",
                input_seat->index);
        return;
    }

    context->last_button_time = time;
    context->last_button = button;
    context->last_button_state = state;
    context->have_last_button = 1;
    if (context->callbacks.pointer_button) {
        context->callbacks.pointer_button(
            context->data, button, state,
            input_seat->pointer_x, input_seat->pointer_y
        );
    }
}

static void pointer_axis(
    void *data,
    struct wl_pointer *pointer,
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value
) {
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

static void keyboard_keymap(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t format,
    int32_t fd,
    uint32_t size
) {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    if (fd >= 0) close(fd);
}

static void keyboard_enter(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    struct wl_surface *surface,
    struct wl_array *keys
) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    struct webos_input_seat *input_seat = data;
    struct webos_input_context *context = input_seat->context;

    if (!input_seat->keyboard_focused) {
        input_seat->keyboard_focused = 1;
        context->keyboard_focus_count++;
    }
    fprintf(stderr, "KEYBOARD_ENTER seat=%u\n", input_seat->index);
    if (context->keyboard_focus_count == 1 && context->callbacks.keyboard_focus) {
        context->callbacks.keyboard_focus(context->data, 1);
    }
}

static void keyboard_leave(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    struct wl_surface *surface
) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct webos_input_seat *input_seat = data;

    clear_keyboard_focus(input_seat);
    fprintf(stderr, "KEYBOARD_LEAVE seat=%u\n", input_seat->index);
}

static void keyboard_key(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    uint32_t time,
    uint32_t key,
    uint32_t state
) {
    (void)keyboard;
    (void)serial;
    struct webos_input_seat *input_seat = data;
    struct webos_input_context *context = input_seat->context;

    fprintf(stderr, "KEY seat=%u key=%u state=%u\n",
            input_seat->index, key, state);
    if (recent_duplicate(
            time, context->last_key_time,
            key, context->last_key,
            state, context->last_key_state,
            context->have_last_key)) {
        fprintf(stderr, "INPUT_DUPLICATE type=key seat=%u\n", input_seat->index);
        return;
    }

    context->last_key_time = time;
    context->last_key = key;
    context->last_key_state = state;
    context->have_last_key = 1;
    if (context->callbacks.keyboard_key) {
        context->callbacks.keyboard_key(context->data, key, state);
    }
}

static void keyboard_modifiers(
    void *data,
    struct wl_keyboard *keyboard,
    uint32_t serial,
    uint32_t mods_depressed,
    uint32_t mods_latched,
    uint32_t mods_locked,
    uint32_t group
) {
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
    struct webos_input_seat *input_seat = data;

    fprintf(stderr, "SEAT_CAPS index=%u caps=%u seat=%p\n",
            input_seat->index, caps, (void *)seat);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input_seat->pointer) {
        input_seat->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(input_seat->pointer, &pointer_listener, input_seat);
        fprintf(stderr, "POINTER_ATTACHED seat=%u ptr=%p\n",
                input_seat->index, (void *)input_seat->pointer);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER)) {
        clear_pointer_focus(input_seat);
        destroy_pointer(input_seat);
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input_seat->keyboard) {
        input_seat->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(input_seat->keyboard, &keyboard_listener, input_seat);
        fprintf(stderr, "KEYBOARD_ATTACHED seat=%u kbd=%p\n",
                input_seat->index, (void *)input_seat->keyboard);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        clear_keyboard_focus(input_seat);
        destroy_keyboard(input_seat);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)seat;
    const struct webos_input_seat *input_seat = data;
    fprintf(stderr, "SEAT_NAME index=%u name=%s\n",
            input_seat->index, name ? name : "(null)");
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name
};

static void destroy_input_seat(struct webos_input_seat *input_seat) {
    struct webos_input_context *context = input_seat->context;

    (void)context;
    clear_pointer_focus(input_seat);
    clear_keyboard_focus(input_seat);
    destroy_pointer(input_seat);
    destroy_keyboard(input_seat);
    destroy_seat_proxy(input_seat);
    memset(input_seat, 0, sizeof(*input_seat));
}

void webos_input_context_init(
    struct webos_input_context *context,
    void *data,
    const struct webos_input_callbacks *callbacks
) {
    memset(context, 0, sizeof(*context));
    context->data = data;
    if (callbacks) context->callbacks = *callbacks;
}

int webos_input_try_bind(
    struct webos_input_context *context,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
) {
    if (strcmp(interface, "wl_seat") != 0) return 0;

    struct webos_input_seat *input_seat = NULL;
    for (size_t i = 0; i < WEBOS_INPUT_MAX_SEATS; i++) {
        if (!context->seats[i].seat) {
            input_seat = &context->seats[i];
            break;
        }
    }
    if (!input_seat) {
        fprintf(stderr, "SEAT_SKIPPED id=%u reason=limit\n", name);
        return 1;
    }

    uint32_t bind_version = version < 3 ? version : 3;
    input_seat->context = context;
    input_seat->registry_name = name;
    input_seat->index = context->seat_count++;
    input_seat->seat = wl_registry_bind(
        registry, name, &wl_seat_interface, bind_version
    );
    if (!input_seat->seat) {
        fprintf(stderr, "SEAT_BIND_FAILED id=%u\n", name);
        memset(input_seat, 0, sizeof(*input_seat));
        return 1;
    }

    wl_seat_add_listener(input_seat->seat, &seat_listener, input_seat);
    fprintf(stderr, "BIND_SEAT index=%u id=%u proxy=%p version=%u\n",
            input_seat->index, name, (void *)input_seat->seat, bind_version);
    return 1;
}

void webos_input_global_remove(struct webos_input_context *context, uint32_t name) {
    for (size_t i = 0; i < WEBOS_INPUT_MAX_SEATS; i++) {
        if (context->seats[i].seat && context->seats[i].registry_name == name) {
            fprintf(stderr, "SEAT_REMOVED index=%u id=%u\n",
                    context->seats[i].index, name);
            destroy_input_seat(&context->seats[i]);
            return;
        }
    }
}

void webos_input_context_destroy(struct webos_input_context *context) {
    for (size_t i = 0; i < WEBOS_INPUT_MAX_SEATS; i++) {
        if (context->seats[i].seat) destroy_input_seat(&context->seats[i]);
    }
}
