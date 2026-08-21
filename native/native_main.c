#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <errno.h>

#ifndef APP_ID
#define APP_ID "org.webosbrew.wayland"
#endif

static void log_line(const char *msg) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/tmp/%s.native_main.log", APP_ID);

    FILE *f = fopen(path, "a");
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

    snprintf(msg, sizeof(msg), "exec failed %s: %s", path, strerror(errno));
    log_line(msg);
}

static int join_path(char *dest, size_t size, const char *dir, const char *name) {
    int written = snprintf(dest, size, "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= size) {
        log_line("binary path is too long");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char native_log[PATH_MAX];
    snprintf(native_log, sizeof(native_log), "/tmp/%s.native_main.log", APP_ID);
    unlink(native_log);
    log_line("native_main client-wrapper started");

    unsetenv("LD_PRELOAD");

    setenv("APP_ID", APP_ID, 1);
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    setenv("DISPLAY_ID", "0", 1);
    setenv("EGL_PLATFORM", "wayland", 1);
    setenv("STRESS_FORCE_4K", "1", 0);
    setenv("STRESS_SWAP_INTERVAL", "1", 0);

    char bin_dir[PATH_MAX];
    find_bin_dir(bin_dir, sizeof(bin_dir));

    char client_log[PATH_MAX];
    snprintf(client_log, sizeof(client_log), "/tmp/%s.client.log", APP_ID);

    int fd = open(client_log, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }

    char android_backend[PATH_MAX];
    char client[PATH_MAX];
    char egl[PATH_MAX];
    char shm[PATH_MAX];

    if (join_path(android_backend, sizeof(android_backend), bin_dir, "android_backend") < 0 ||
        join_path(client, sizeof(client), bin_dir, "client") < 0 ||
        join_path(egl, sizeof(egl), bin_dir, "wayland_egl") < 0 ||
        join_path(shm, sizeof(shm), bin_dir, "wayland_rect") < 0) {
        return 126;
    }

    if (strcmp(APP_ID, "org.webosbrew.android") == 0) {
        try_exec(android_backend);
    }

    try_exec(client);
    try_exec(egl);
    try_exec(shm);

    log_line("all exec attempts failed");
    return 127;
}
