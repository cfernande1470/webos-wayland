# Kbase UAPI notes — TV r9p0 / UK 11.0

Fuente primaria de ABI de referencia: [bifrost/r9p0 kbase source tree](https://nest-open-source.googlesource.com/manifest_repos/mali-driver/+/0bde759bc5682bba7de558e308410bbc6d08d2d8/bifrost/r9p0/kernel/).
Las estructuras no se deben sustituir por headers modernos de CSF/Valhall.

## Identificación observada

| campo | valor |
|---|---|
| kernel | Linux 4.4.84 aarch64 |
| DDK | `K:r9p0-01rel0(GPL)` |
| UK | `11.0` |
| GPU sysfs | Mali-G51, 3 cores, r1p1, `0x7090` |
| raw GPU ID | `0x70901010` |
| shader/tiler/L2 present | `0x7 / 0x1 / 0x1` |
| `GET_GPUPROPS` payload | 706 bytes |

## UAPI verificada

| ioctl | nr | estructura/tamaño | resultado en TV |
|---|---:|---:|---|
| `VERSION_CHECK` | 0 | `u16 major, u16 minor` / 4 | `11.0`, OK |
| `SET_FLAGS` | 1 | `u32` / 4 | OK con flags 0 |
| `JOB_SUBMIT` | 2 | addr/u32 nr/u32 stride / 16 | observado desde tracer |
| `GET_GPUPROPS` | 3 | ptr/u32 size/u32 flags / 16 | OK, 706 bytes |
| `MEM_ALLOC` | 5 | union / 32 | observado |
| `MEM_QUERY` | 6 | union / 16 | observado |
| `MEM_FREE` | 7 | GPU VA / 8 | no observado en muestra |
| `HWCNT_READER_SETUP` | 8 | five `u32` / 20 | devuelve fd 4 |
| `GET_DDK_VERSION` | 13 | ptr/u32/padding / 16 | devuelve string r9p0 |
| `MEM_SYNC` | 15 | handle/user/size/type / 32 | observado |
| `MEM_COMMIT` | 20 | GPU VA/pages / 16 | observado una vez |
| `MEM_IMPORT` | 22 | union / 24 | observado dos veces |
| `MEM_PROFILE_ADD` | 27 | ptr/len/padding / 16 | observado 49 veces |

`JOB_SUBMIT` usa una tabla userspace de `base_jd_atom_v2`; su descriptor debe
decodificarse sólo después de validar `addr`, `nr_atoms` y `stride` contra las
regiones legibles del proceso. El tracer actual registra la dirección y el
tamaño de ioctl, pero no sigue punteros.

## GPU properties

El payload es una secuencia de pares little-endian:

```text
key = property_id << 2 | value_size_code
value_size_code: 0=u8, 1=u16, 2=u32, 3=u64
```

La captura confirma, entre otros:

```text
PRODUCT_ID                 0x7090
GPU_FREQ_KHZ_MAX           600000
GPU_FREQ_KHZ_MIN           528000
RAW_SHADER_PRESENT         0x7
RAW_TILER_PRESENT          0x1
RAW_L2_PRESENT             0x1
RAW_AS_PRESENT             0xff
RAW_JS_PRESENT             0x7
RAW_TILER_FEATURES         521
RAW_MEM_FEATURES           1
RAW_MMU_FEATURES           10273
```

## HWCNT reader

En r9p0, `HWCNT_READER_SETUP` devuelve un descriptor anónimo. El reader
reporta `HWVER=5`, `GET_BUFFER_SIZE=1536` y API 1. La familia secundaria usa
tipo ioctl `0xBE`; las estructuras de metadata son:

```c
struct kbase_hwcnt_reader_metadata {
    u64 timestamp;
    u32 event_id;
    u32 buffer_idx;
};
```

El probe intentó `GET_HWVER`, `GET_BUFFER_SIZE`, `GET_API_VERSION`, mmap de dos
buffers, enable de evento manual, dump manual y `poll`. El setup y las
consultas funcionan; el mmap devuelve `-EPERM` en AArch64 y ARM32. El probe
cierra siempre el reader fd y el contexto Mali. Queda por comprobar si el
fallo procede del tamaño no alineado del buffer (1536 × 2), de la política de
`remap_pfn_range` del kernel LG o de una diferencia entre el árbol público y
la integración vendor.

## 32/64-bit

El mismo ABI de campos funciona en ambos procesos probados porque los
punteros UAPI son `u64`. El probe usa estructuras de tamaño fijo y no depende
de `long`; las syscalls se implementan separadamente para AArch64 y ARM EABI.
La compatibilidad del kernel acepta ambos handshakes y ambos devuelven el
mismo payload de properties. Esto no demuestra que todas las estructuras de
job o importación sean idénticas en todas las rutas vendor: deben verificarse
por ioctl y retorno antes de decodificarse.

## Instrumentación pendiente

- Capturar el contenido de argumentos sólo tras validar rangos en
  `/proc/<pid>/maps`.
- Confirmar `MEM_IMPORT` type y origen (`ION`, UMM o buffer compartido).
- Resolver el mmap HWCNT y obtener primero dumps raw.
- Correlacionar atom tables con fences/event reads.
