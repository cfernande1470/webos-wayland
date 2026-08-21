# Fase 4 — GPU pipeline reverse engineering (A–E)

Estado: primera captura cuantitativa completada el 2026-08-22. Esta nota
separa explícitamente observaciones hechas en la TV de inferencias basadas en
código o artefactos locales. No se han modificado kernel, DTB, clocks,
voltajes, librerías del sistema ni servicios críticos.

## Plataforma observada

La TV responde como `aarch64`, con kernel `4.4.84-229.1.kavir.2`,
`/dev/mali0` (major 10, minor 60) y `/dev/ion`. No existe `/dev/dri`.
Sysfs reporta:

```text
Mali-G51 3 cores r1p1 0x7090
core mask 0x7
GPU frequency max/min 600000/528000 kHz
```

El DDK devuelto por kbase es `K:r9p0-01rel0(GPL)` y la negociación UK es
`11.0`, tanto desde un proceso nativo AArch64 freestanding como desde un
proceso ARM32 EABI dinámico.

## Stack observado: renderer nativo

El binario de prueba ARM32 carga esta cadena real:

```text
wayland_egl (ARM32, /lib/ld-linux.so.3)
  -> /usr/lib/libEGL.so.1.4
  -> /usr/lib/libGLESv2.so.2.0
  -> /usr/lib/libmali.so.0.1
  -> /usr/lib/libwayland-egl.so.0.1
  -> libwayland-client.so.0.3.0
  -> libwayland-webos-client.so.1.0.0
  -> /dev/mali0
```

El proceso tiene un único fd observado para `/dev/mali0`. `EGL_VENDOR` es
`ARM`, `GL_RENDERER` es `Mali-G51`, y el contexto anuncia
`EGL_ARM_implicit_external_sync`, `EGL_KHR_fence_sync` y
`EGL_KHR_surfaceless_context`. No anuncia `EGL_ANDROID_native_fence_sync` ni
`EGL_EXT_image_dma_buf_import`.

Hashes obtenidos directamente de la TV: `libmali.so.0.1` (18.159.656 bytes,
SHA-256 `6e76326f6342a5564864a0515958dc29de65d2ec51d125382586329f143063ca`),
`libEGL.so.1.4` (`9876028aafcb61bb8a6b3aa45c03cc5b4f21073ff0e8b65eee40beea954256d7`)
y `libGLESv2.so.2.0`
(`40bb263f2c2f61c5b51934289019c03b14cbd59c640935efdcc0af63520f4461`).

## Primer conteo nativo

Captura de aproximadamente 4,95 s, con el renderer a 1920×1080 y el frame
pacing normal de Wayland:

| evento | total | aproximación por frame |
|---|---:|---:|
| `JOB_SUBMIT` | 293 | 1,00 |
| `MEM_SYNC` | 1.465 | 5,00 |
| `MEM_QUERY` | 1.174 | 4,00 |
| `MEM_ALLOC` | 16 | sólo inicialización/churn no periódico |
| `MEM_IMPORT` | 2 | sólo inicialización observada |
| `poll` | 2.043 | 6,97 |
| `read` | 581 | 1,98 |
| ioctls kbase totales | 3.007 | 10,27 |

El conteo de `JOB_SUBMIT` es el primer dato directo de submissions por frame:
en este workload nativo estable es aproximadamente uno. El patrón de
sincronización sí merece investigación prioritaria: hay unas cinco llamadas
`MEM_SYNC` y cuatro `MEM_QUERY` por frame. Todavía no se puede afirmar si son
cache maintenance real, consultas de bookkeeping o efectos indirectos de
`eglSwapBuffers`; para eso hay que capturar argumentos validados y correlación
EGL/Wayland.

La captura tuvo 0 eventos descartados. El archivo binario ocupó
aproximadamente 360 KiB. Una muestra de CPU agregada sobre los threads dio
8 ticks en el segundo final tanto con tracing como sin tracing; es una
comprobación de que no hubo una perturbación grande, no una prueba formal de
que el overhead sea menor del 5 %.

## Hardware counters

La UAPI legacy funciona parcialmente:

```text
KBASE_IOCTL_HWCNT_READER_SETUP -> fd 4
reader HWVER                    -> 5
reader buffer size              -> 1536 bytes
reader API version              -> 1
```

El raw GPU ID de `GET_GPUPROPS` es `0x70901010`, con shader mask `0x7`, tiler
`0x1` y L2 `0x1`. Por el formato GPU_ID2 de r9p0 corresponde al modelo de
contadores `tSIx`, coherente con Mali-G51/Bifrost. Sin embargo, el `mmap` del
reader devuelve `-EPERM` tanto al pedir 1536 bytes (un buffer) como 3072 bytes
(dos buffers), en ambas ABI. Por tanto, los counters están
expuestos por ioctl pero todavía no hay una muestra interpretada. No se han
enviado jobs arbitrarios.

## Estado de los otros stacks

### Chromium

El artefacto `browser_shell` custom es ARM32 y sus `DT_NEEDED` incluyen
`libwayland-egl.so`, `libwayland-webos-client.so.1` y
`libwayland-client.so.0`. No había un proceso custom Chromium ejecutándose en
la TV durante esta captura. El `WebAppMgr` integrado sí estaba ejecutándose,
con `--in-process-gpu`, y tenía fd `/dev/mali0` y las mismas librerías EGL/Mali
del sistema. Eso es evidencia del Chromium de webOS, no todavía del artefacto
custom de este repositorio.

### Firefox

Los binarios preparados son ARM32. El proceso Firefox presente en la TV y su
socket process cargan `libmozwayland.so`, `libwayland-egl.so.0.1` y
`libwayland-client.so.0`, pero en la instantánea no cargan `libEGL`,
`libGLESv2` ni `libmali`, y no tienen fd `/dev/mali0`. La ruta de GPU Firefox
no está caracterizada todavía: primero hay que conseguir un workload WebGL o
compositor acelerado que demuestre que llega a EGL.

### Android/VirGL

No había proceso Android/VirGL ejecutándose en la TV durante esta captura. El
código local sí documenta una arquitectura AArch64 guest → `virpipe`/vtest →
servidor `virglrenderer` ARM32 → EGL/GLES/libMali ARM32, con un presenter
Wayland EGL separado que hace fullscreen sampling y `eglSwapBuffers`. Esta
parte queda marcada como diseño/código, no como medición de TV.

## Próximo experimento válido

1. Añadir un decoder de argumentos con validación contra `/proc/<pid>/maps`.
2. Correlacionar `JOB_SUBMIT`, `MEM_SYNC` y `MEM_QUERY` con `eglSwapBuffers` y
   callbacks Wayland.
3. Repetir la captura con el Chromium custom y con Android/VirGL en workloads
   controlados.
4. Resolver la semántica del `mmap` HWCNT en este kernel antes de interpretar
   counters.

No se proponen optimizaciones de producción en esta etapa.
