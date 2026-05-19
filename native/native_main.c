#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#define APP_ID "org.webosbrew.wayland"

static volatile sig_atomic_t stopping = 0;
static pid_t child_pid = -1;
static char bin_dir[PATH_MAX];

static void log_line(const char *msg) {
    FILE *f = fopen("/tmp/org.webosbrew.wayland.native_main.log", "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] %s\n", (long)t, msg);
        fclose(f);
    }
}

static void on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

static void find_bin_dir(void) {
    char exe[PATH_MAX];

    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        strcpy(bin_dir, ".");
        return;
    }

    exe[n] = 0;

    char *slash = strrchr(exe, '/');
    if (slash) {
        *slash = 0;
    }

    snprintf(bin_dir, sizeof(bin_dir), "%s", exe);
}

static void stop_child(void) {
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        usleep(300000);
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, WNOHANG);
        child_pid = -1;
    }
}

static void launch_child(void) {
    if (child_pid > 0) {
        return;
    }

    char child_path[PATH_MAX];
    snprintf(child_path, sizeof(child_path), "%s/wayland_rect", bin_dir);

    child_pid = fork();

    if (child_pid < 0) {
        log_line("fork failed");
        return;
    }

    if (child_pid == 0) {
        unsetenv("LD_PRELOAD");
        unsetenv("LD_PRELOAD");
        setenv("APP_ID", APP_ID, 1);
        setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("DISPLAY_ID", "0", 1);

        int fd = open("/tmp/org.webosbrew.wayland.wayland_rect.log",
                      O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }

        execl(child_path, child_path, NULL);
        perror("execl wayland_rect");
        _exit(127);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "launched wayland_rect pid=%ld", (long)child_pid);
    log_line(buf);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    unlink("/tmp/org.webosbrew.wayland.native_main.log");

    log_line("native_main started");
    find_bin_dir();

    char buf[512];
    snprintf(buf, sizeof(buf), "bin_dir=%s", bin_dir);
    log_line(buf);

    launch_child();

    while (!stopping) {
        if (child_pid > 0) {
            int st = 0;
            pid_t r = waitpid(child_pid, &st, WNOHANG);

            if (r == child_pid) {
                snprintf(buf, sizeof(buf), "wayland_rect exited status=%d; relaunching", st);
                log_line(buf);
                child_pid = -1;
                sleep(1);
                launch_child();
            }
        }

        sleep(1);
    }

    log_line("native_main stopping");
    stop_child();
    log_line("native_main stopped");

    return 0;
}
