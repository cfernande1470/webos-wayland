#!/usr/bin/env python3
"""Decode the fixed binary format emitted by libmali_trace.so."""

import collections
import json
import struct
import sys

HEADER = struct.Struct("<8sIIIIQ")
EVENT = struct.Struct("<QQQQqiiiHHHH4x")
TYPES = {
    1: "open",
    2: "openat",
    3: "close",
    4: "ioctl",
    5: "mmap",
    6: "munmap",
    7: "poll",
    8: "ppoll",
    9: "read",
    10: "write",
}
IOCTL_NAMES = {
    (0x80, 0): "VERSION_CHECK",
    (0x80, 1): "SET_FLAGS",
    (0x80, 2): "JOB_SUBMIT",
    (0x80, 3): "GET_GPUPROPS",
    (0x80, 5): "MEM_ALLOC",
    (0x80, 6): "MEM_QUERY",
    (0x80, 7): "MEM_FREE",
    (0x80, 8): "HWCNT_READER_SETUP",
    (0x80, 9): "HWCNT_ENABLE",
    (0x80, 10): "HWCNT_DUMP",
    (0x80, 11): "HWCNT_CLEAR",
    (0x80, 12): "DISJOINT_QUERY",
    (0x80, 13): "GET_DDK_VERSION",
    (0x80, 15): "MEM_SYNC",
    (0x80, 16): "MEM_FIND_CPU_OFFSET",
    (0x80, 20): "MEM_COMMIT",
    (0x80, 21): "MEM_ALIAS",
    (0x80, 22): "MEM_IMPORT",
    (0x80, 23): "MEM_FLAGS_CHANGE",
    (0x80, 24): "STREAM_CREATE",
    (0x80, 25): "FENCE_VALIDATE",
    (0x80, 26): "GET_PROFILING_CONTROLS",
    (0x80, 27): "MEM_PROFILE_ADD",
    (0x80, 28): "SOFT_EVENT_UPDATE",
    (0xBE, 0): "HWCNT_GET_HWVER",
    (0xBE, 1): "HWCNT_GET_BUFFER_SIZE",
    (0xBE, 16): "HWCNT_DUMP_READER",
    (0xBE, 32): "HWCNT_GET_BUFFER",
    (0xBE, 33): "HWCNT_PUT_BUFFER",
    (0xBE, 48): "HWCNT_SET_INTERVAL",
    (0xBE, 64): "HWCNT_ENABLE_EVENT",
    (0xBE, 255): "HWCNT_GET_API_VERSION",
}


def main(path):
    data = open(path, "rb").read()
    magic, version, event_size, capacity, pid, dropped = HEADER.unpack_from(data)
    if magic != b"MALITR01" or event_size < EVENT.size:
        raise SystemExit(f"invalid trace header: {magic!r}, event_size={event_size}")
    count = min((len(data) - HEADER.size) // event_size, capacity)
    type_counts = collections.Counter()
    ioctl_counts = collections.Counter()
    ioctl_errors = collections.Counter()
    fd_counts = collections.Counter()
    thread_counts = collections.Counter()
    first = None
    last = None
    for i in range(count):
        off = HEADER.size + i * event_size
        raw = data[off : off + EVENT.size]
        ts, arg, aux0, aux1, result, fd, tid, error, typ, nr, size, flags = EVENT.unpack(raw)
        type_name = TYPES.get(typ, f"type_{typ}")
        type_counts[type_name] += 1
        fd_counts[fd] += 1
        thread_counts[tid] += 1
        first = ts if first is None else min(first, ts)
        last = ts if last is None else max(last, ts)
        if typ == 4:
            key = (aux0 & 0xFF, nr, size)
            name = IOCTL_NAMES.get(key, f"type0x{aux0 & 0xFF:02x}/nr{nr}/size{size}")
            ioctl_counts[name] += 1
            if error:
                ioctl_errors[name] += 1
    summary = {
        "file": path,
        "pid": pid,
        "version": version,
        "events": count,
        "dropped": dropped,
        "duration_s": (last - first) / 1e9 if first is not None else 0.0,
        "events_by_type": dict(type_counts),
        "ioctls": dict(ioctl_counts),
        "ioctl_errors": dict(ioctl_errors),
        "events_by_fd": dict(fd_counts),
        "events_by_thread": dict(thread_counts),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} TRACE.bin")
    main(sys.argv[1])
