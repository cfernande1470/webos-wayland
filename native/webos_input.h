#ifndef WEBOS_INPUT_H
#define WEBOS_INPUT_H

#include <stdint.h>
#include <wayland-client.h>

#define WEBOS_INPUT_MAX_SEATS 8

struct webos_input_context;

struct webos_input_callbacks {
    void (*pointer_enter)(void *data, int x, int y);
    void (*pointer_leave)(void *data);
    void (*pointer_motion)(void *data, int x, int y);
    void (*pointer_button)(
        void *data, uint32_t button, uint32_t state, int x, int y
    );
    void (*keyboard_focus)(void *data, int focused);
    void (*keyboard_key)(void *data, uint32_t key, uint32_t state);
};

struct webos_input_seat {
    struct webos_input_context *context;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    uint32_t registry_name;
    uint32_t index;
    int pointer_x;
    int pointer_y;
    int pointer_focused;
    int keyboard_focused;
};

struct webos_input_context {
    struct webos_input_seat seats[WEBOS_INPUT_MAX_SEATS];
    struct webos_input_callbacks callbacks;
    void *data;
    uint32_t seat_count;
    uint32_t pointer_focus_count;
    uint32_t keyboard_focus_count;
    uint32_t last_key_time;
    uint32_t last_key;
    uint32_t last_key_state;
    uint32_t last_button_time;
    uint32_t last_button;
    uint32_t last_button_state;
    int have_last_key;
    int have_last_button;
};

void webos_input_context_init(
    struct webos_input_context *context,
    void *data,
    const struct webos_input_callbacks *callbacks
);

int webos_input_try_bind(
    struct webos_input_context *context,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
);

void webos_input_global_remove(struct webos_input_context *context, uint32_t name);
void webos_input_context_destroy(struct webos_input_context *context);

#endif
