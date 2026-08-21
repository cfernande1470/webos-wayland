#include <stddef.h>
#include <stdint.h>

/*
 * Minimal, libc-free kbase r9p0 probe.
 *
 * This deliberately does not allocate GPU memory or submit jobs.  It only
 * opens a context, performs the public handshake/property queries, and tries
 * the legacy HWCNT reader setup.  All structures below are copied from the
 * public bifrost/r9p0 UAPI; do not silently substitute a newer header.
 */

#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC 02000000

#define IOC_NRBITS 8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14
#define IOC_DIRBITS 2
#define IOC_NRSHIFT 0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT (IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NONE 0U
#define IOC_WRITE 1U
#define IOC_READ 2U
#define IOC(dir, type, nr, size) \
    ((unsigned long)(((dir) << IOC_DIRSHIFT) | ((type) << IOC_TYPESHIFT) | \
                     ((nr) << IOC_NRSHIFT) | ((size) << IOC_SIZESHIFT)))
#define IO(type, nr) IOC(IOC_NONE, (type), (nr), 0)
#define IOR(type, nr, type_) IOC(IOC_READ, (type), (nr), sizeof(type_))
#define IOW(type, nr, type_) IOC(IOC_WRITE, (type), (nr), sizeof(type_))
#define IOWR(type, nr, type_) IOC(IOC_READ | IOC_WRITE, (type), (nr), sizeof(type_))

struct version_check { uint16_t major, minor; };
struct set_flags { uint32_t create_flags; };
struct gpu_props { uint64_t buffer; uint32_t size, flags; };
struct ddk_version { uint64_t version_buffer; uint32_t size, padding; };
struct hwcnt_setup {
    uint32_t buffer_count;
    uint32_t jm_bm;
    uint32_t shader_bm;
    uint32_t tiler_bm;
    uint32_t mmu_l2_bm;
};
struct hwcnt_metadata {
    uint64_t timestamp;
    uint32_t event_id;
    uint32_t buffer_idx;
};
struct pollfd_ { int32_t fd; int16_t events; int16_t revents; };

enum {
    KBASE_VERSION_CHECK = 0xc0048000UL,
    KBASE_SET_FLAGS = 0x40048001UL,
    KBASE_GET_GPUPROPS = 0x40108003UL,
    KBASE_HWCNT_SETUP = 0x40148008UL,
    KBASE_DDK_VERSION = 0x4010800dUL,
    HWCNT_READER = 0xbe,
};

#define HWCNT_GET_HWVER IOR(HWCNT_READER, 0x00, uint32_t)
#define HWCNT_GET_BUFFER_SIZE IOR(HWCNT_READER, 0x01, uint32_t)
#define HWCNT_DUMP IOW(HWCNT_READER, 0x10, uint32_t)
#define HWCNT_GET_BUFFER IOR(HWCNT_READER, 0x20, struct hwcnt_metadata)
#define HWCNT_PUT_BUFFER IOW(HWCNT_READER, 0x21, struct hwcnt_metadata)
#define HWCNT_SET_INTERVAL IOW(HWCNT_READER, 0x30, uint32_t)
#define HWCNT_ENABLE_EVENT IOW(HWCNT_READER, 0x40, uint32_t)
#define HWCNT_GET_API_VERSION IOW(HWCNT_READER, 0xff, uint32_t)

#define MAX_PROPS 16384U
#define MAX_HWCNT_MAP (16U * 1024U * 1024U)

#if defined(__aarch64__)
static long syscall6(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}
static long syscall3(long n, long a0, long a1, long a2)
{ return syscall6(n, a0, a1, a2, 0, 0, 0); }
static long syscall4(long n, long a0, long a1, long a2, long a3)
{ return syscall6(n, a0, a1, a2, a3, 0, 0); }
static long sys_open(const char *p)
{ return syscall4(56, AT_FDCWD, (long)p, O_RDWR | O_CLOEXEC, 0); }
#define SYS_IOCTL 29
#define SYS_CLOSE 57
#define SYS_MMAP 222
#define SYS_MUNMAP 215
#define SYS_POLL 7
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT 94
#define ARCH_NAME "aarch64"
#else
static long syscall6(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r4 __asm__("r4") = a4;
    register long r5 __asm__("r5") = a5;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3),
                     "r"(r4), "r"(r5), "r"(r7) : "memory");
    return r0;
}
static long syscall3(long n, long a0, long a1, long a2)
{ return syscall6(n, a0, a1, a2, 0, 0, 0); }
static long syscall4(long n, long a0, long a1, long a2, long a3)
{ return syscall6(n, a0, a1, a2, a3, 0, 0); }
static long sys_open(const char *p)
{ return syscall3(5, (long)p, O_RDWR | O_CLOEXEC, 0); }
#define SYS_IOCTL 54
#define SYS_CLOSE 6
#define SYS_MMAP 192
#define SYS_MUNMAP 91
#define SYS_POLL 168
#define SYS_READ 3
#define SYS_WRITE 4
#define SYS_EXIT 248
#define ARCH_NAME "arm32"
#endif

static long sys_ioctl(long fd, unsigned long request, void *arg)
{ return syscall3(SYS_IOCTL, fd, (long)request, (long)arg); }
static long sys_close(long fd) { return syscall3(SYS_CLOSE, fd, 0, 0); }
static long sys_read(long fd, void *buf, size_t len)
{ return syscall3(SYS_READ, fd, (long)buf, (long)len); }
static long sys_write(long fd, const void *buf, size_t len)
{ return syscall3(SYS_WRITE, fd, (long)buf, (long)len); }
static long sys_poll(struct pollfd_ *p, long n, long timeout)
{ return syscall3(SYS_POLL, (long)p, n, timeout); }
static long sys_mmap(size_t len, long fd)
{
    /* PROT_READ|PROT_WRITE, MAP_SHARED, offset 0. */
    return syscall6(SYS_MMAP, 0, (long)len, 3, 1, fd, 0);
}
static long sys_munmap(void *p, size_t len)
{ return syscall3(SYS_MUNMAP, (long)p, (long)len, 0); }
static void sys_exit(long code) { syscall3(SYS_EXIT, code, 0, 0); for (;;) {} }

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
static void put(const char *s) { sys_write(1, s, slen(s)); }
static void put_u64(uint64_t value, int hex)
{
    char b[24]; size_t n = sizeof(b); const char *digits = "0123456789abcdef";
    b[--n] = 0;
    do { b[--n] = digits[hex ? (value & 15) : (value % 10)]; value = hex ? value >> 4 : value / 10; }
    while (value);
    put(&b[n]);
}
static void put_i(long value)
{
    if (value < 0) { put("-"); value = -value; }
    put_u64((uint64_t)value, 0);
}
static void field_s(const char *name, const char *value)
{ put("\""); put(name); put("\":\""); put(value); put("\""); }
static void field_u(const char *name, uint64_t value)
{ put("\""); put(name); put("\":"); put_u64(value, 0); }
static void field_hex(const char *name, uint64_t value)
{ put("\""); put(name); put("\":\"0x"); put_u64(value, 1); put("\""); }
static void result_field(long result)
{
    put(",\"return\":"); put_i(result);
    if (result < 0) { put(",\"errno\":"); put_i(-result); }
}
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p)
{ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static const char *prop_name(uint32_t id)
{
    switch (id) {
    case 1: return "PRODUCT_ID";
    case 2: return "VERSION_STATUS";
    case 3: return "MINOR_REVISION";
    case 4: return "MAJOR_REVISION";
    case 5: return "GPU_SPEED_MHZ";
    case 6: return "GPU_FREQ_KHZ_MAX";
    case 7: return "GPU_FREQ_KHZ_MIN";
    case 8: return "LOG2_PROGRAM_COUNTER_SIZE";
    case 12: return "GPU_AVAILABLE_MEMORY_SIZE";
    case 13: return "L2_LOG2_LINE_SIZE";
    case 14: return "L2_LOG2_CACHE_SIZE";
    case 15: return "L2_NUM_L2_SLICES";
    case 16: return "TILER_BIN_SIZE_BYTES";
    case 17: return "TILER_MAX_ACTIVE_LEVELS";
    case 18: return "MAX_THREADS";
    case 19: return "MAX_WORKGROUP_SIZE";
    case 20: return "MAX_BARRIER_SIZE";
    case 21: return "MAX_REGISTERS";
    case 22: return "MAX_TASK_QUEUE";
    case 23: return "MAX_THREAD_GROUP_SPLIT";
    case 24: return "IMPL_TECH";
    case 25: return "RAW_SHADER_PRESENT";
    case 26: return "RAW_TILER_PRESENT";
    case 27: return "RAW_L2_PRESENT";
    case 28: return "RAW_STACK_PRESENT";
    case 31: return "RAW_MEM_FEATURES";
    case 32: return "RAW_MMU_FEATURES";
    case 33: return "RAW_AS_PRESENT";
    case 34: return "RAW_JS_PRESENT";
    case 51: return "RAW_TILER_FEATURES";
    case 55: return "RAW_GPU_ID";
    case 56: return "RAW_THREAD_MAX_THREADS";
    case 57: return "RAW_THREAD_MAX_WORKGROUP_SIZE";
    case 58: return "RAW_THREAD_MAX_BARRIER_SIZE";
    case 59: return "RAW_THREAD_FEATURES";
    case 60: return "RAW_COHERENCY_MODE";
    case 61: return "COHERENCY_NUM_GROUPS";
    case 62: return "COHERENCY_NUM_CORE_GROUPS";
    case 63: return "COHERENCY_COHERENCY";
    default: return "UNKNOWN";
    }
}

static void dump_props(const uint8_t *buf, uint32_t size)
{
    uint32_t off = 0, count = 0;
    while (off + 4 <= size && count++ < 256) {
        uint32_t key = rd32(buf + off), id = key >> 2, kind = key & 3, bytes = 1U << kind;
        uint64_t value = 0;
        off += 4;
        if (off + bytes > size) break;
        if (bytes == 1) value = buf[off];
        else if (bytes == 2) value = rd16(buf + off);
        else if (bytes == 4) value = rd32(buf + off);
        else value = rd64(buf + off);
        put("{\"event\":\"gpu_property\",\"id\":"); put_u64(id, 0);
        put(",\"name\":\""); put(prop_name(id)); put("\",\"value\":");
        if (bytes == 8 || id == 25 || id == 26 || id == 27 || id == 28)
            { put("\"0x"); put_u64(value, 1); put("\""); }
        else put_u64(value, 0);
        put(",\"value_bytes\":"); put_u64(bytes, 0); put("}\n");
        off += bytes;
    }
    put("{\"event\":\"gpu_properties_end\",\"parsed_bytes\":");
    put_u64(off, 0); put(",\"records\":"); put_u64(count, 0); put("}\n");
}

static void try_hwcnt(long mali_fd)
{
    struct hwcnt_setup setup = {2, 1, 1, 1, 1};
    long reader = sys_ioctl(mali_fd, KBASE_HWCNT_SETUP, &setup);
    put("{\"event\":\"hwcnt_setup\",\"request\":\"KBASE_IOCTL_HWCNT_READER_SETUP\"");
    result_field(reader); put(",\"buffer_count\":2,\"jm_mask\":1,\"shader_mask\":1,\"tiler_mask\":1,\"mmu_l2_mask\":1}\n");
    if (reader < 0) return;

    uint32_t hwver = 0, dump_size = 0, api = 0;
    long r_hwver = sys_ioctl(reader, HWCNT_GET_HWVER, &hwver);
    long r_size = sys_ioctl(reader, HWCNT_GET_BUFFER_SIZE, &dump_size);
    long r_api = sys_ioctl(reader, HWCNT_GET_API_VERSION, &api);
    put("{\"event\":\"hwcnt_reader\",\"fd\":"); put_u64((uint64_t)reader, 0);
    put(",\"hwver\":"); put_u64(hwver, 0); put(",\"buffer_size\":"); put_u64(dump_size, 0);
    put(",\"api_version\":"); put_u64(api, 0); put(",\"get_hwver_return\":"); put_i(r_hwver);
    put(",\"get_buffer_size_return\":"); put_i(r_size); put(",\"get_api_return\":"); put_i(r_api); put("}\n");

    if (r_size >= 0 && dump_size != 0 && dump_size <= MAX_HWCNT_MAP && dump_size * 2U <= MAX_HWCNT_MAP) {
        long one_buffer = sys_mmap(dump_size, reader);
        put("{\"event\":\"hwcnt_mmap_probe\",\"length\":"); put_u64(dump_size, 0);
        result_field(one_buffer); put("}\n");
        if (one_buffer > 0 && one_buffer != -1L)
            (void)sys_munmap((void *)one_buffer, dump_size);
        size_t map_len = (size_t)dump_size * 2U;
        long mapped = sys_mmap(map_len, reader);
        put("{\"event\":\"hwcnt_mmap\",\"length\":"); put_u64(map_len, 0);
        result_field(mapped); put("}\n");
        if (mapped > 0 && mapped != -1L) {
            uint32_t event = 0;
            long r_enable = sys_ioctl(reader, HWCNT_ENABLE_EVENT, &event);
            long r_dump = sys_ioctl(reader, HWCNT_DUMP, &event);
            struct pollfd_ p = {(int32_t)reader, 1, 0};
            long r_poll = sys_poll(&p, 1, 100);
            struct hwcnt_metadata meta = {0, 0, 0};
            long r_get = sys_ioctl(reader, HWCNT_GET_BUFFER, &meta);
            put("{\"event\":\"hwcnt_sample\",\"enable_return\":"); put_i(r_enable);
            put(",\"dump_return\":"); put_i(r_dump); put(",\"poll_return\":"); put_i(r_poll);
            put(",\"poll_revents\":"); put_u64((uint16_t)p.revents, 0); put(",\"get_buffer_return\":"); put_i(r_get);
            put(",\"timestamp\":"); put_u64(meta.timestamp, 0); put(",\"event_id\":"); put_u64(meta.event_id, 0);
            put(",\"buffer_idx\":"); put_u64(meta.buffer_idx, 0); put("}\n");
            if (r_get >= 0 && meta.buffer_idx < 2) {
                const uint8_t *raw = (const uint8_t *)mapped + ((size_t)meta.buffer_idx * dump_size);
                uint32_t bytes = dump_size < 128 ? dump_size : 128;
                put("{\"event\":\"hwcnt_raw_prefix\",\"bytes\":"); put_u64(bytes, 0); put(",\"hex\":\"");
                for (uint32_t i = 0; i < bytes; ++i) { const char *d = "0123456789abcdef"; char c[2] = {d[raw[i] >> 4], d[raw[i] & 15]}; sys_write(1, c, 2); }
                put("\"}\n");
                (void)sys_ioctl(reader, HWCNT_PUT_BUFFER, &meta);
            }
            (void)sys_munmap((void *)mapped, map_len);
        }
    }
    (void)sys_close(reader);
}

int main(void)
{
    uint8_t props[MAX_PROPS];
    char ddk[128];
    struct version_check version = {11, 0};
    struct set_flags flags = {0};
    struct gpu_props query = {0, 0, 0};
    struct ddk_version ddk_query = {(uint64_t)(uintptr_t)ddk, sizeof(ddk), 0};
    long fd = sys_open("/dev/mali0");
    if (fd < 0) {
        put("{\"event\":\"open\",\"arch\":\""); put(ARCH_NAME); put("\""); result_field(fd); put("}\n");
        sys_exit(2);
    }
    put("{\"event\":\"open\",\"arch\":\""); put(ARCH_NAME); put("\",\"fd\":"); put_u64(fd, 0); put("}\n");

    long r = sys_ioctl(fd, KBASE_VERSION_CHECK, &version);
    put("{\"event\":\"version_check\",\"requested_major\":11,\"requested_minor\":0,\"negotiated_major\":");
    put_u64(version.major, 0); put(",\"negotiated_minor\":"); put_u64(version.minor, 0); result_field(r); put("}\n");
    if (r < 0) { (void)sys_close(fd); sys_exit(3); }
    r = sys_ioctl(fd, KBASE_SET_FLAGS, &flags);
    put("{\"event\":\"set_flags\""); result_field(r); put("}\n");
    if (r < 0) { (void)sys_close(fd); sys_exit(4); }

    ddk[0] = 0;
    r = sys_ioctl(fd, KBASE_DDK_VERSION, &ddk_query);
    put("{\"event\":\"ddk_version\",\"return\":"); put_i(r); put(",\"text\":\"");
    for (size_t i = 0; i < sizeof(ddk) && ddk[i]; ++i) { char c[1] = {ddk[i]}; sys_write(1, c, 1); }
    put("\"}\n");

    r = sys_ioctl(fd, KBASE_GET_GPUPROPS, &query);
    put("{\"event\":\"gpu_properties_size\""); result_field(r); put("}\n");
    uint32_t size = (r > 0 && (uint64_t)r <= MAX_PROPS) ? (uint32_t)r : MAX_PROPS;
    query.buffer = (uint64_t)(uintptr_t)props; query.size = size; query.flags = 0;
    r = sys_ioctl(fd, KBASE_GET_GPUPROPS, &query);
    put("{\"event\":\"gpu_properties_read\",\"requested_bytes\":"); put_u64(size, 0); result_field(r); put("}\n");
    if (r > 0 && (uint64_t)r <= MAX_PROPS) dump_props(props, (uint32_t)r);

    try_hwcnt(fd);
    (void)sys_close(fd);
    put("{\"event\":\"close\"}\n");
    return 0;
}
