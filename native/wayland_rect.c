#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifndef WL_SHM_FORMAT_XRGB8888
#define WL_SHM_FORMAT_XRGB8888 1
#endif

#ifndef KEY_ESC
#define KEY_ESC 1
#define KEY_ENTER 28
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_BACK 158
#endif

#define NUM_BUFS 3

struct shm_buf {
    struct wl_buffer *wlbuf;
    void *data;
    size_t size;
    int width;
    int height;
    int busy;
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_shell *shell;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_callback *frame_cb;

    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    struct shm_buf bufs[NUM_BUFS];

    int width;
    int height;
    int running;
    int frame;

    int theme;
    int android_mode;
    int android_ready;
    int win_x;
    int win_y;
    int pointer_x;
    int pointer_y;
    int have_pointer;

    int dragging;
    int drag_dx;
    int drag_dy;
};

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int path_executable(const char *path) {
    return path && access(path, X_OK) == 0;
}

static int android_sidecar_ready(void) {
    return access("/media/internal/android-usb/android-sidecar/run/system_server.pid", R_OK) == 0 ||
           access("/media/internal/android-usb/android-sidecar/run/zygote64.pid", R_OK) == 0;
}

static void launch_android_sidecar_once(const char *app_id) {
    if (!app_id || strcmp(app_id, "org.webosbrew.android") != 0) {
        return;
    }

    const char *usb = getenv("USB");
    const char *side = getenv("SIDE");
    const char *launcher = NULL;
    char launcher_path[512];
    char log_path[512];

    if (!usb || !*usb) usb = "/media/internal/android-usb";
    if (!side || !*side) side = "/media/internal/android-usb/android-sidecar";

    snprintf(launcher_path, sizeof(launcher_path), "%s/bin/try-zygote-start-system-server-v2.sh", side);
    if (path_executable(launcher_path)) {
        launcher = launcher_path;
    } else {
        snprintf(launcher_path, sizeof(launcher_path), "%s/bin/restart.sh", side);
        if (path_executable(launcher_path)) {
            launcher = launcher_path;
        }
    }

    if (!launcher) {
        fprintf(stderr, "ANDROID_SIDEcar launcher missing usb=%s side=%s\n", usb, side);
        return;
    }

    snprintf(log_path, sizeof(log_path), "/tmp/%s.android_backend.launch.log", app_id);

    signal(SIGCHLD, SIG_IGN);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ANDROID_SIDEcar fork failed: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        int fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        setsid();
        setenv("USB", usb, 1);
        setenv("SIDE", side, 1);
        setenv("FORMAT_USB", "0", 1);

        fprintf(stderr, "ANDROID_SIDEcar exec launcher=%s USB=%s SIDE=%s\n", launcher, usb, side);
        execl("/bin/sh", "sh", launcher, (char *)NULL);
        fprintf(stderr, "ANDROID_SIDEcar exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    fprintf(stderr, "ANDROID_SIDEcar launch pid=%ld launcher=%s\n", (long)pid, launcher);
}

static void rect(uint32_t *p, int sw, int sh, int x, int y, int w, int h, uint32_t c);
static void outline(uint32_t *p, int sw, int sh, int x, int y, int w, int h, int t, uint32_t c);

static void draw_android_robot(uint32_t *p, int w, int h, int cx, int cy, int scale, uint32_t green, uint32_t dark, uint32_t white) {
    int bw = 140 * scale / 100;
    int bh = 180 * scale / 100;
    int hx = cx - bw / 2;
    int hy = cy - bh / 2;
    int head_w = 110 * scale / 100;
    int head_h = 80 * scale / 100;
    int body_w = 140 * scale / 100;
    int body_h = 110 * scale / 100;

    rect(p, w, h, hx + 24, hy - 54, 6, 34, green);
    rect(p, w, h, hx + bw - 30, hy - 54, 6, 34, green);
    rect(p, w, h, hx, hy, bw, bh, dark);
    outline(p, w, h, hx, hy, bw, bh, 6, green);

    rect(p, w, h, cx - head_w / 2, hy - 28, head_w, head_h, green);
    outline(p, w, h, cx - head_w / 2, hy - 28, head_w, head_h, 5, white);

    rect(p, w, h, cx - body_w / 2, hy + 60, body_w, body_h, green);
    outline(p, w, h, cx - body_w / 2, hy + 60, body_w, body_h, 5, white);

    rect(p, w, h, cx - 44 * scale / 100, hy - 6, 18 * scale / 100, 18 * scale / 100, dark);
    rect(p, w, h, cx + 26 * scale / 100, hy - 6, 18 * scale / 100, 18 * scale / 100, dark);
    rect(p, w, h, cx - 36 * scale / 100, hy + 44 * scale / 100, 74 * scale / 100, 10 * scale / 100, dark);

    rect(p, w, h, cx - 110 * scale / 100, hy + 76, 10 * scale / 100, 84 * scale / 100, green);
    rect(p, w, h, cx + 100 * scale / 100, hy + 76, 10 * scale / 100, 84 * scale / 100, green);
    rect(p, w, h, cx - 108 * scale / 100, hy + 146, 28 * scale / 100, 12 * scale / 100, white);
    rect(p, w, h, cx + 80 * scale / 100, hy + 146, 28 * scale / 100, 12 * scale / 100, white);
}

static int create_tmpfile(size_t size) {
    char name[256];
    snprintf(name, sizeof(name), "/tmp/webos-wayland-%ld-%d-XXXXXX",
             (long)getpid(), rand());

    int fd = mkstemp(name);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }

    unlink(name);

    if (size > INT32_MAX || ftruncate(fd, (off_t)size) < 0) {
        if (size > INT32_MAX) errno = EFBIG;
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct shm_buf *b = data;
    b->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release
};

static int init_buffer(struct app *a, struct shm_buf *b) {
    if (a->width <= 0 || a->height <= 0 || a->width > INT32_MAX / 4) {
        errno = EINVAL;
        return -1;
    }

    int stride = a->width * 4;
    size_t size = (size_t)stride * (size_t)a->height;

    if (size > INT32_MAX) {
        errno = EFBIG;
        return -1;
    }

    int fd = create_tmpfile(size);
    if (fd < 0) return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    struct wl_buffer *wlbuf = wl_shm_pool_create_buffer(
        pool, 0, a->width, a->height, stride, WL_SHM_FORMAT_XRGB8888
    );

    wl_shm_pool_destroy(pool);
    close(fd);

    if (!wlbuf) {
        munmap(data, size);
        return -1;
    }

    memset(b, 0, sizeof(*b));
    b->wlbuf = wlbuf;
    b->data = data;
    b->size = size;
    b->width = a->width;
    b->height = a->height;
    b->busy = 0;

    wl_buffer_add_listener(wlbuf, &buffer_listener, b);
    return 0;
}

static void destroy_buffers(struct app *a) {
    for (int i = 0; i < NUM_BUFS; i++) {
        struct shm_buf *b = &a->bufs[i];
        if (b->wlbuf) wl_buffer_destroy(b->wlbuf);
        if (b->data && b->data != MAP_FAILED) munmap(b->data, b->size);
        memset(b, 0, sizeof(*b));
    }
}

static void set_surface_opaque(struct app *a) {
    if (!a->compositor || !a->surface || a->width <= 0 || a->height <= 0) return;

    struct wl_region *region = wl_compositor_create_region(a->compositor);
    if (!region) return;

    wl_region_add(region, 0, 0, a->width, a->height);
    wl_surface_set_opaque_region(a->surface, region);
    wl_region_destroy(region);
}

static struct shm_buf *next_buffer(struct app *a) {
    for (int i = 0; i < NUM_BUFS; i++) {
        struct shm_buf *b = &a->bufs[i];

        if (!b->wlbuf || b->width != a->width || b->height != a->height) {
            if (b->wlbuf) wl_buffer_destroy(b->wlbuf);
            if (b->data && b->data != MAP_FAILED) munmap(b->data, b->size);
            memset(b, 0, sizeof(*b));

            if (init_buffer(a, b) < 0) return NULL;
        }

        if (!b->busy) return b;
    }

    return NULL;
}

static void rect(uint32_t *p, int sw, int sh, int x, int y, int w, int h, uint32_t c) {
    int x0 = clampi(x, 0, sw);
    int y0 = clampi(y, 0, sh);
    int x1 = clampi(x + w, 0, sw);
    int y1 = clampi(y + h, 0, sh);

    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = p + yy * sw;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
}

static void outline(uint32_t *p, int sw, int sh, int x, int y, int w, int h, int t, uint32_t c) {
    rect(p, sw, sh, x, y, w, t, c);
    rect(p, sw, sh, x, y + h - t, w, t, c);
    rect(p, sw, sh, x, y, t, h, c);
    rect(p, sw, sh, x + w - t, y, t, h, c);
}

static void paint(struct app *a, struct shm_buf *b) {
    uint32_t *p = b->data;
    int w = a->width;
    int h = a->height;
    int f = a->frame;

    uint32_t bg1;
    uint32_t bg2;
    uint32_t accent;
    uint32_t panel;
    uint32_t white = rgb(235, 235, 235);

    if (a->android_mode) {
        a->android_ready = android_sidecar_ready();

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                p[y * w + x] = rgb(5, 8, 12);
            }
        }

        uint32_t green = rgb(105, 230, 120);
        uint32_t dark = rgb(18, 24, 30);
        uint32_t android_white = rgb(242, 242, 242);
        uint32_t blue = rgb(72, 138, 255);

        rect(p, w, h, 0, 0, w, 88, rgb(9, 12, 18));
        rect(p, w, h, 0, 86, w, 3, green);

        int card_w = 1020;
        int card_h = 420;
        int cx = clampi((w - card_w) / 2, 60, w - card_w - 60);
        int cy = clampi((h - card_h) / 2, 120, h - card_h - 120);
        rect(p, w, h, cx, cy, card_w, card_h, rgb(16, 20, 28));
        outline(p, w, h, cx, cy, card_w, card_h, 4, green);

        draw_android_robot(p, w, h, cx + 190, cy + 210, 150, green, dark, android_white);

        rect(p, w, h, cx + 360, cy + 82, 520, 42, green);
        rect(p, w, h, cx + 360, cy + 82, a->android_ready ? 520 : (a->frame * 18) % 520, 42, blue);
        rect(p, w, h, cx + 360, cy + 150, 520, 18, rgb(54, 60, 68));
        rect(p, w, h, cx + 360, cy + 184, 430, 18, rgb(54, 60, 68));
        rect(p, w, h, cx + 360, cy + 218, 470, 18, rgb(54, 60, 68));
        rect(p, w, h, cx + 360, cy + 266, 360, 54, green);
        rect(p, w, h, cx + 360, cy + 266, a->android_ready ? 360 : (a->frame * 9) % 360, 54, blue);

        rect(p, w, h, cx + 360, cy + 354, 520, 6, rgb(54, 60, 68));
        rect(p, w, h, cx + 360, cy + 354, a->android_ready ? 520 : (a->frame * 13) % 520, 6, green);

        if (a->have_pointer) {
            int px = clampi(a->pointer_x, 0, w - 1);
            int py = clampi(a->pointer_y, 0, h - 1);
            rect(p, w, h, px - 10, py - 2, 20, 4, white);
            rect(p, w, h, px - 2, py - 10, 4, 20, white);
        }

        outline(p, w, h, 0, 0, w, h, 4, green);
        return;
    }

    if (a->theme % 3 == 0) {
        bg1 = rgb(10, 18, 36);
        bg2 = rgb(20, 36, 72);
        accent = rgb(0, 180, 255);
        panel = rgb(34, 54, 92);
    } else if (a->theme % 3 == 1) {
        bg1 = rgb(28, 12, 24);
        bg2 = rgb(74, 24, 58);
        accent = rgb(255, 80, 190);
        panel = rgb(86, 36, 76);
    } else {
        bg1 = rgb(12, 26, 16);
        bg2 = rgb(32, 70, 42);
        accent = rgb(120, 255, 120);
        panel = rgb(36, 82, 50);
    }

    for (int y = 0; y < h; y++) {
        uint8_t mix = (uint8_t)((y * 80) / (h ? h : 1));
        uint8_t r = ((bg1 >> 16) & 0xff) + mix / 3;
        uint8_t g = ((bg1 >> 8) & 0xff) + mix / 4;
        uint8_t bl = (bg1 & 0xff) + mix / 2;

        uint32_t c = rgb(r, g, bl);
        for (int x = 0; x < w; x++) {
            p[y * w + x] = c;
        }
    }

    int grid = 96;
    for (int x = -(f % grid); x < w; x += grid) {
        rect(p, w, h, x, 0, 2, h, bg2);
    }
    for (int y = -(f % grid); y < h; y += grid) {
        rect(p, w, h, 0, y, w, 2, bg2);
    }

    rect(p, w, h, 0, 0, w, 72, rgb(8, 8, 12));
    rect(p, w, h, 0, 70, w, 2, accent);

    int meter_w = (f * 7) % (w - 80);
    rect(p, w, h, 40, 26, w - 80, 18, rgb(38, 38, 44));
    rect(p, w, h, 40, 26, meter_w, 18, accent);

    int dock_h = 96;
    rect(p, w, h, 0, h - dock_h, w, dock_h, rgb(8, 10, 14));
    for (int i = 0; i < 8; i++) {
        int x = 42 + i * 104;
        int pulse = ((f + i * 8) % 50);
        uint32_t c = (i == (a->theme % 8)) ? accent : panel;
        rect(p, w, h, x, h - 72 - (pulse / 8), 72, 48 + (pulse / 8), c);
        outline(p, w, h, x, h - 72 - (pulse / 8), 72, 48 + (pulse / 8), 2, white);
    }

    int wx = clampi(a->win_x, 20, w - 520);
    int wy = clampi(a->win_y, 90, h - 360);
    a->win_x = wx;
    a->win_y = wy;

    rect(p, w, h, wx + 14, wy + 18, 500, 300, rgb(0, 0, 0));
    rect(p, w, h, wx, wy, 500, 300, panel);
    uint32_t titlebar = a->dragging ? accent : rgb(16, 18, 26);
    rect(p, w, h, wx, wy, 500, 42, titlebar);
    rect(p, w, h, wx + 14, wy + 12, 18, 18, rgb(255, 80, 80));
    rect(p, w, h, wx + 40, wy + 12, 18, 18, rgb(255, 210, 80));
    rect(p, w, h, wx + 66, wy + 12, 18, 18, rgb(80, 255, 130));
    outline(p, w, h, wx, wy, 500, 300, 3, accent);

    for (int i = 0; i < 10; i++) {
        int bar = 30 + ((f * (i + 3)) % 190);
        uint32_t c = (i % 2) ? accent : white;
        rect(p, w, h, wx + 32 + i * 42, wy + 250 - bar, 26, bar, c);
    }

    int orb_x = wx + 250 + (((f * 5) % 220) - 110);
    int orb_y = wy + 144 + (((f * 3) % 120) - 60);
    rect(p, w, h, orb_x - 28, orb_y - 28, 56, 56, accent);
    outline(p, w, h, orb_x - 36, orb_y - 36, 72, 72, 3, white);

    if (a->have_pointer) {
        int px = clampi(a->pointer_x, 0, w - 1);
        int py = clampi(a->pointer_y, 0, h - 1);
        rect(p, w, h, px - 18, py - 2, 36, 4, white);
        rect(p, w, h, px - 2, py - 18, 4, 36, white);
        rect(p, w, h, px - 8, py - 8, 16, 16, accent);
    }

    outline(p, w, h, 0, 0, w, h, 5, accent);
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
    struct shm_buf *b = next_buffer(a);
    if (!b) {
        fprintf(stderr, "NO_FREE_BUFFER frame=%d\n", a->frame);
        wl_display_flush(a->display);
        return;
    }

    paint(a, b);
    b->busy = 1;

    wl_surface_attach(a->surface, b->wlbuf, 0, 0);
    wl_surface_damage(a->surface, 0, 0, a->width, a->height);

    if (a->frame_cb) wl_callback_destroy(a->frame_cb);
    a->frame_cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(a->frame_cb, &frame_listener, a);

    wl_surface_commit(a->surface);
    wl_display_flush(a->display);

    if ((a->frame % 60) == 0) {
        fprintf(stderr, "FRAME %d theme=%d win=%d,%d ptr=%d,%d\n",
                a->frame, a->theme, a->win_x, a->win_y,
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

    if (width > 0 && height > 0 && (width != a->width || height != a->height)) {
        a->width = width;
        a->height = height;
        destroy_buffers(a);
        set_surface_opaque(a);
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

    if (a->dragging) {
        a->win_x = a->pointer_x - a->drag_dx;
        a->win_y = a->pointer_y - a->drag_dy;
    }

    if ((a->frame % 30) == 0) {
        fprintf(stderr, "POINTER_MOTION x=%d y=%d dragging=%d\n",
                a->pointer_x, a->pointer_y, a->dragging);
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;

    struct app *a = data;

    int px = a->pointer_x;
    int py = a->pointer_y;
    int wx = a->win_x;
    int wy = a->win_y;

    fprintf(stderr, "POINTER_BUTTON button=%u state=%u x=%d y=%d\n",
            button, state, px, py);

    if (button != 272) {
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (px >= wx + 12 && px <= wx + 34 && py >= wy + 10 && py <= wy + 34) {
            fprintf(stderr, "UI_CLOSE\n");
            a->running = 0;
            wl_display_disconnect(a->display);
            exit(0);
        }

        if (px >= wx + 38 && px <= wx + 62 && py >= wy + 10 && py <= wy + 34) {
            a->theme++;
            fprintf(stderr, "UI_THEME_YELLOW theme=%d\n", a->theme);
            return;
        }

        if (px >= wx + 64 && px <= wx + 88 && py >= wy + 10 && py <= wy + 34) {
            a->theme++;
            fprintf(stderr, "UI_THEME_GREEN theme=%d\n", a->theme);
            return;
        }

        if (px >= wx && px <= wx + 500 && py >= wy && py <= wy + 42) {
            a->dragging = 1;
            a->drag_dx = px - wx;
            a->drag_dy = py - wy;
            fprintf(stderr, "DRAG_START dx=%d dy=%d\n", a->drag_dx, a->drag_dy);
            return;
        }

        a->theme++;
        fprintf(stderr, "POINTER_CLICK theme=%d\n", a->theme);
    } else {
        if (a->dragging) {
            fprintf(stderr, "DRAG_END win=%d,%d\n", a->win_x, a->win_y);
        }
        a->dragging = 0;
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
        fprintf(stderr, "EXIT_KEY\n");
        a->running = 0;
        wl_display_disconnect(a->display);
        exit(0);
    } else if (key == KEY_ENTER) {
        a->theme++;
    } else if (key == KEY_LEFT) {
        a->win_x -= 45;
    } else if (key == KEY_RIGHT) {
        a->win_x += 45;
    } else if (key == KEY_UP) {
        a->win_y -= 45;
    } else if (key == KEY_DOWN) {
        a->win_y += 45;
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

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    fprintf(stderr, "KEYBOARD_REPEAT rate=%d delay=%d\n", rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct app *a = data;

    fprintf(stderr, "SEAT_CAPS caps=%u\n", caps);

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

    if (strcmp(interface, "wl_compositor") == 0) {
        a->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 3 ? version : 3
        );
    } else if (strcmp(interface, "wl_shm") == 0) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_shell") == 0) {
        a->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0 && !a->seat) {
        struct wl_seat *seat = wl_registry_bind(
            registry, name, &wl_seat_interface, version < 3 ? version : 3
        );
        fprintf(stderr, "BIND_SEAT id=%u proxy=%p version=%u\n", name, (void*)seat, version);
        wl_seat_add_listener(seat, &seat_listener, a);
        a->seat = seat;
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

    srand((unsigned)time(NULL));

    struct app a;
    memset(&a, 0, sizeof(a));

    a.width = 1920;
    a.height = 1080;
    a.running = 1;
    a.win_x = 180;
    a.win_y = 170;
    a.pointer_x = 960;
    a.pointer_y = 540;

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *wld = getenv("WAYLAND_DISPLAY");
    const char *app_id = getenv("APP_ID");
    if (!app_id) app_id = "org.webosbrew.wayland";
    a.android_mode = strcmp(app_id, "org.webosbrew.android") == 0;

    fprintf(stderr, "wayland_rect_v3 start XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s APP_ID=%s\n",
            xdg ? xdg : "(null)", wld ? wld : "(null)", app_id);

    launch_android_sidecar_once(app_id);

    a.display = wl_display_connect(NULL);
    if (!a.display) {
        fprintf(stderr, "ERROR: wl_display_connect failed\n");
        return 2;
    }

    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);

    if (!a.compositor || !a.shm || !a.shell) {
        fprintf(stderr, "ERROR: missing compositor=%p shm=%p shell=%p\n",
                (void*)a.compositor, (void*)a.shm, (void*)a.shell);
        return 3;
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    if (!a.surface) {
        fprintf(stderr, "ERROR: failed to create Wayland surface\n");
        return 4;
    }

    a.shell_surface = wl_shell_get_shell_surface(a.shell, a.surface);
    if (!a.shell_surface) {
        fprintf(stderr, "ERROR: failed to create Wayland shell surface\n");
        return 4;
    }

    wl_shell_surface_add_listener(a.shell_surface, &shell_listener, &a);
    wl_shell_surface_set_title(a.shell_surface, "webOS Wayland Native Lab v3");
    wl_shell_surface_set_class(a.shell_surface, app_id);
    wl_shell_surface_set_fullscreen(
        a.shell_surface,
        WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
        0,
        NULL
    );

    set_surface_opaque(&a);

    render(&a);

    while (a.running) {
        int r = wl_display_dispatch(a.display);
        if (r < 0) {
            fprintf(stderr, "wl_display_dispatch returned %d\n", r);
            break;
        }
    }

    if (a.frame_cb) wl_callback_destroy(a.frame_cb);
    destroy_buffers(&a);
    if (a.pointer) wl_pointer_release(a.pointer);
    if (a.keyboard) wl_keyboard_release(a.keyboard);
    if (a.seat) wl_seat_release(a.seat);
    if (a.shell_surface) wl_shell_surface_destroy(a.shell_surface);
    if (a.surface) wl_surface_destroy(a.surface);
    if (a.shell) wl_shell_destroy(a.shell);
    if (a.shm) wl_shm_destroy(a.shm);
    if (a.compositor) wl_compositor_destroy(a.compositor);
    if (a.registry) wl_registry_destroy(a.registry);
    wl_display_disconnect(a.display);

    fprintf(stderr, "wayland_rect_v3 exit\n");
    return 0;
}
