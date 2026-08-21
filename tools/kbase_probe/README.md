# kbase_probe

Probe freestanding para el ABI público Mali kbase bifrost/r9p0.

```sh
tools/kbase_probe/build.sh
```

Genera `build/tools/kbase_probe/kbase_probe.arm32` y
`kbase_probe.aarch64`. El binario sólo abre `/dev/mali0`, negocia UK 11.0,
consulta DDK/GPU properties y prueba la creación/cierre del HWCNT reader. No
reserva memoria GPU, no importa buffers y no envía `JOB_SUBMIT`.

La salida es JSONL para poder conservarla junto con las trazas. Los negativos
son retornos raw de syscall (`-errno`).
