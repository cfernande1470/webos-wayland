#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#define APP_ID "org.webosbrew.wayland"

static void log_line(const char *msg) {
    FILE *f = fopen("/tmp/org.webosbrew.wayland.native_main.log", "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] %s\n", (long)t, msg);
        fclose(f);
    }
}

static void find_bin_dir(char *bin_dir, size_t size) {
    char exe[PATH_MAX];

    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        snprintf(bin_dir, size, ".");
        return;
    }

    exe[n] = 0;

    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;

    snprintf(bin_dir, size, "%s", exe);
}

static void try_exec(const char *path) {
    char msg[PATH_MAX + 128];
    snprintf(msg, sizeof(msg), "try exec %s", path);
    log_line(msg);

    execl(path, path, NULL);

    snprintf(msg, sizeof(msg), "exec failed %s", path);
    log_line(msg);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    unlink("/tmp/org.webosbrew.wayland.native_main.log");
    log_line("native_main client-wrapper started");

    unsetenv("LD_PRELOAD");

    setenv("APP_ID", APP_ID, 1);
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    setenv("DISPLAY_ID", "0", 1);
    setenv("EGL_PLATFORM", "wayland", 1);

    char bin_dir[PATH_MAX];
    find_bin_dir(bin_dir, sizeof(bin_dir));

    int fd = open("/tmp/org.webosbrew.wayland.client.log",
                  O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }

    char client[PATH_MAX];
    char egl[PATH_MAX];
    char shm[PATH_MAX];

    snprintf(client, sizeof(client), "%s/client", bin_dir);
    snprintf(egl, sizeof(egl), "%s/wayland_egl", bin_dir);
    snprintf(shm, sizeof(shm), "%s/wayland_rect", bin_dir);

    try_exec(client);
    try_exec(egl);
    try_exec(shm);

    log_line("all exec attempts failed");
    return 127;
}
