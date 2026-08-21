# mali_trace

`libmali_trace.so` es un `LD_PRELOAD` ARM32 que registra en un buffer fijo las
operaciones relacionadas con `/dev/mali0` y los reader fds HWCNT derivados.

```sh
tools/mali_trace/build.sh
MALI_TRACE_OUT=/tmp/native.bin \
  LD_PRELOAD=/tmp/libmali_trace.so ./renderer
tools/mali_trace/decode.py /tmp/native.bin
```

El formato es binario little-endian (`MALITR01`). No escribe por evento ni
dereferencia punteros de ioctl; registra fd, request nr/tamaño/dirección,
resultado, errno, thread ID y timestamp monotonic raw. El buffer admite 65.536
eventos y el header indica drops. `SIGTERM`/`SIGINT` hacen un flush controlado
para que los benchmarks acotados con `timeout` no pierdan la traza.

La primera versión cubre `open/open64/openat/openat64`, `close`, `ioctl`,
`mmap`, `munmap`, `poll`, `ppoll`, `read` y `write`. La decodificación de
argumentos userspace y de atom tables queda deliberadamente pendiente de un
validador de rangos por proceso.
