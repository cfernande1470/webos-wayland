#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * Low-noise /dev/mali0 tracer.
 *
 * Events are appended to a fixed-size in-process array and written once at
 * process exit.  No stdio, malloc, or logging is used on the hot path.  The
 * initial version intentionally records ioctl identity/size/argument address
 * but does not dereference arbitrary ioctl pointers; a later decoder can use
 * validated snapshots from a process-specific ABI.
 */

#define TRACE_MAGIC "MALITR01"
#define TRACE_VERSION 1U
#define TRACE_CAPACITY 65536U
#define MAX_TRACKED_FDS 4096
#define KBASE_HWCNT_SETUP 0x40148008UL

enum event_type {
    EVENT_OPEN = 1,
    EVENT_OPENAT = 2,
    EVENT_CLOSE = 3,
    EVENT_IOCTL = 4,
    EVENT_MMAP = 5,
    EVENT_MUNMAP = 6,
    EVENT_POLL = 7,
    EVENT_PPOLL = 8,
    EVENT_READ = 9,
    EVENT_WRITE = 10,
};

struct trace_header {
    char magic[8];
    uint32_t version;
    uint32_t event_size;
    uint32_t capacity;
    uint32_t pid;
    uint64_t dropped;
};

struct trace_event {
    uint64_t timestamp_ns;
    uint64_t arg;
    uint64_t aux0;
    uint64_t aux1;
    int64_t result;
    int32_t fd;
    int32_t tid;
    int32_t error;
    uint16_t type;
    uint16_t request_nr;
    uint16_t request_size;
    uint16_t flags;
};

static struct trace_event events[TRACE_CAPACITY];
static volatile uint32_t event_count;
static volatile uint64_t dropped;
static volatile unsigned char tracked[MAX_TRACKED_FDS];
static int trace_fd = -1;
static int resolving;

static int (*real_open_fn)(const char *, int, ...);
static int (*real_open64_fn)(const char *, int, ...);
static int (*real_openat_fn)(int, const char *, int, ...);
static int (*real_openat64_fn)(int, const char *, int, ...);
static int (*real_close_fn)(int);
static int (*real_ioctl_fn)(int, unsigned long, ...);
static void *(*real_mmap_fn)(void *, size_t, int, int, int, off_t);
static int (*real_munmap_fn)(void *, size_t);
static int (*real_poll_fn)(struct pollfd *, nfds_t, int);
static int (*real_ppoll_fn)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
static ssize_t (*real_read_fn)(int, void *, size_t);
static ssize_t (*real_write_fn)(int, const void *, size_t);

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int tid(void) { return (int)syscall(SYS_gettid); }

static void resolve_symbols(void)
{
    if (real_open_fn || resolving)
        return;
    resolving = 1;
    real_open_fn = dlsym(RTLD_NEXT, "open");
    real_open64_fn = dlsym(RTLD_NEXT, "open64");
    real_openat_fn = dlsym(RTLD_NEXT, "openat");
    real_openat64_fn = dlsym(RTLD_NEXT, "openat64");
    real_close_fn = dlsym(RTLD_NEXT, "close");
    real_ioctl_fn = dlsym(RTLD_NEXT, "ioctl");
    real_mmap_fn = dlsym(RTLD_NEXT, "mmap");
    real_munmap_fn = dlsym(RTLD_NEXT, "munmap");
    real_poll_fn = dlsym(RTLD_NEXT, "poll");
    real_ppoll_fn = dlsym(RTLD_NEXT, "ppoll");
    real_read_fn = dlsym(RTLD_NEXT, "read");
    real_write_fn = dlsym(RTLD_NEXT, "write");
    resolving = 0;
}

static int is_mali_path(const char *path)
{ return path && strcmp(path, "/dev/mali0") == 0; }

static void mark_fd(int fd)
{ if (fd >= 0 && fd < MAX_TRACKED_FDS) tracked[fd] = 1; }
static int is_tracked(int fd)
{ return fd >= 0 && fd < MAX_TRACKED_FDS && tracked[fd]; }
static void unmark_fd(int fd)
{ if (fd >= 0 && fd < MAX_TRACKED_FDS) tracked[fd] = 0; }

static void write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) {
        long n = syscall(SYS_write, fd, p, len);
        if (n <= 0) return;
        p += n;
        len -= (size_t)n;
    }
}

static void record_event(uint16_t type, int fd, unsigned long request,
                         uint64_t arg, uint64_t aux0, uint64_t aux1,
                         int64_t result, int error, uint16_t flags)
{
    if (!is_tracked(fd) && type != EVENT_OPEN && type != EVENT_OPENAT)
        return;
    uint32_t slot = __sync_fetch_and_add(&event_count, 1U);
    if (slot >= TRACE_CAPACITY) {
        __sync_fetch_and_add(&dropped, 1U);
        return;
    }
    struct trace_event *e = &events[slot];
    e->timestamp_ns = now_ns();
    e->arg = arg;
    e->aux0 = aux0;
    e->aux1 = aux1;
    e->result = result;
    e->fd = fd;
    e->tid = tid();
    e->error = error;
    e->type = type;
    e->request_nr = (uint16_t)((request >> 0) & 0xffU);
    e->request_size = (uint16_t)((request >> 16) & 0x3fffU);
    e->flags = flags;
}

static void open_trace_file(void)
{
    const char *path = getenv("MALI_TRACE_OUT");
    char fallback[64];
    if (!path || !*path) {
        snprintf(fallback, sizeof(fallback), "/tmp/mali_trace.%ld.bin", (long)getpid());
        path = fallback;
    }
    trace_fd = (int)syscall(SYS_openat, AT_FDCWD, path,
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (trace_fd < 0) return;
    struct trace_header header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, TRACE_MAGIC, 8);
    header.version = TRACE_VERSION;
    header.event_size = sizeof(struct trace_event);
    header.capacity = TRACE_CAPACITY;
    header.pid = (uint32_t)getpid();
    write_all(trace_fd, &header, sizeof(header));
}

static void flush_trace_file(void)
{
    if (trace_fd < 0) return;
    uint32_t count = event_count;
    if (count > TRACE_CAPACITY) count = TRACE_CAPACITY;
    struct trace_header header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, TRACE_MAGIC, 8);
    header.version = TRACE_VERSION;
    header.event_size = sizeof(struct trace_event);
    header.capacity = TRACE_CAPACITY;
    header.pid = (uint32_t)getpid();
    header.dropped = dropped + (event_count > TRACE_CAPACITY ? event_count - TRACE_CAPACITY : 0);
    syscall(SYS_lseek, trace_fd, 0, SEEK_SET);
    write_all(trace_fd, &header, sizeof(header));
    write_all(trace_fd, events, (size_t)count * sizeof(events[0]));
    syscall(SYS_close, trace_fd);
    trace_fd = -1;
}

static void trace_signal(int signo)
{
    (void)signo;
    flush_trace_file();
    syscall(SYS_exit_group, 128 + signo);
    for (;;) {}
}

__attribute__((constructor)) static void mali_trace_init(void)
{
    resolve_symbols();
    open_trace_file();
    signal(SIGTERM, trace_signal);
    signal(SIGINT, trace_signal);
}

__attribute__((destructor)) static void mali_trace_fini(void)
{ flush_trace_file(); }

int open(const char *path, int flags, ...)
{
    if (!real_open_fn) resolve_symbols();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_open_fn ? (flags & O_CREAT ? real_open_fn(path, flags, mode) : real_open_fn(path, flags)) : -1;
    if (is_mali_path(path)) { mark_fd(fd); record_event(EVENT_OPEN, fd, 0, 0, flags, 0, fd, errno, 0); }
    return fd;
}

int open64(const char *path, int flags, ...)
{
    if (!real_open64_fn) resolve_symbols();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_open64_fn ? (flags & O_CREAT ? real_open64_fn(path, flags, mode) : real_open64_fn(path, flags)) : -1;
    if (is_mali_path(path)) { mark_fd(fd); record_event(EVENT_OPEN, fd, 0, 0, flags, 64, fd, errno, 0); }
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    if (!real_openat_fn) resolve_symbols();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_openat_fn ? (flags & O_CREAT ? real_openat_fn(dirfd, path, flags, mode) : real_openat_fn(dirfd, path, flags)) : -1;
    if (is_mali_path(path)) { mark_fd(fd); record_event(EVENT_OPENAT, fd, 0, 0, (uint64_t)(uint32_t)dirfd, flags, fd, errno, 0); }
    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    if (!real_openat64_fn) resolve_symbols();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int fd = real_openat64_fn ? (flags & O_CREAT ? real_openat64_fn(dirfd, path, flags, mode) : real_openat64_fn(dirfd, path, flags)) : -1;
    if (is_mali_path(path)) { mark_fd(fd); record_event(EVENT_OPENAT, fd, 0, 0, (uint64_t)(uint32_t)dirfd, flags, fd, errno, 64); }
    return fd;
}

int close(int fd)
{
    if (!real_close_fn) resolve_symbols();
    int tracked_fd = is_tracked(fd);
    int r = real_close_fn ? real_close_fn(fd) : -1;
    if (tracked_fd) { record_event(EVENT_CLOSE, fd, 0, 0, 0, 0, r, r < 0 ? errno : 0, 0); unmark_fd(fd); }
    return r;
}

int ioctl(int fd, unsigned long request, ...)
{
    if (!real_ioctl_fn) resolve_symbols();
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    int r = real_ioctl_fn ? real_ioctl_fn(fd, request, arg) : -1;
    int err = r < 0 ? errno : 0;
    if (is_tracked(fd)) {
        record_event(EVENT_IOCTL, fd, request, (uint64_t)(uintptr_t)arg,
                     (uint64_t)((request >> 8) & 0xffU), (uint64_t)((request >> 30) & 3U),
                     r, err, 0);
        if (request == KBASE_HWCNT_SETUP && r >= 0) mark_fd(r);
    }
    return r;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (!real_mmap_fn) resolve_symbols();
    void *p = real_mmap_fn ? real_mmap_fn(addr, length, prot, flags, fd, offset) : MAP_FAILED;
    if (is_tracked(fd)) record_event(EVENT_MMAP, fd, 0, (uint64_t)(uintptr_t)p, length, (uint64_t)offset, (int64_t)(intptr_t)p, p == MAP_FAILED ? errno : 0, prot | ((uint16_t)flags << 8));
    return p;
}

int munmap(void *addr, size_t length)
{
    if (!real_munmap_fn) resolve_symbols();
    int r = real_munmap_fn ? real_munmap_fn(addr, length) : -1;
    if (trace_fd >= 0) record_event(EVENT_MUNMAP, -1, 0, (uint64_t)(uintptr_t)addr, length, 0, r, r < 0 ? errno : 0, 0);
    return r;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!real_poll_fn) resolve_symbols();
    int relevant = 0;
    for (nfds_t i = 0; i < nfds; ++i) relevant |= is_tracked(fds[i].fd);
    int r = real_poll_fn ? real_poll_fn(fds, nfds, timeout) : -1;
    if (relevant) record_event(EVENT_POLL, fds[0].fd, 0, nfds, timeout, 0, r, r < 0 ? errno : 0, 0);
    return r;
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *mask)
{
    if (!real_ppoll_fn) resolve_symbols();
    int relevant = 0;
    for (nfds_t i = 0; i < nfds; ++i) relevant |= is_tracked(fds[i].fd);
    int r = real_ppoll_fn ? real_ppoll_fn(fds, nfds, timeout, mask) : -1;
    if (relevant) record_event(EVENT_PPOLL, fds[0].fd, 0, nfds, timeout ? (uint64_t)timeout->tv_nsec : 0, 0, r, r < 0 ? errno : 0, 0);
    return r;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (!real_read_fn) resolve_symbols();
    ssize_t r = real_read_fn ? real_read_fn(fd, buf, count) : -1;
    if (is_tracked(fd)) record_event(EVENT_READ, fd, 0, (uint64_t)(uintptr_t)buf, count, 0, r, r < 0 ? errno : 0, 0);
    return r;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (!real_write_fn) resolve_symbols();
    ssize_t r = real_write_fn ? real_write_fn(fd, buf, count) : -1;
    if (is_tracked(fd)) record_event(EVENT_WRITE, fd, 0, (uint64_t)(uintptr_t)buf, count, 0, r, r < 0 ? errno : 0, 0);
    return r;
}
