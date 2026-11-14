# RelevoJIT: tareas y bitácora

Las reglas del bucle están en `~/Projects/dashos/.worktrees/x86/docs/X86_LOOP.md` (línea B). Diseño en
`docs/DISEÑO.md`.

## Tareas

- [ ] R0 **Esqueleto.** Makefile (C11, `-O2 -Wall -Wextra -Werror`, gcc y clang), `librelevo.a`, `make test` con un
  ejecutor de pruebas mínimo, `tools/datos.sh` que descarga SingleStepTests/m68000 a `tests/datos/` (comprobando su
  licencia MIT), y un lector de sus ficheros JSON.
- [ ] R1 **Interfaz y bucle.** `include/relevo.h` como en el diseño (configuración, memoria con páginas de 64 KB,
  funciones del anfitrión, registros, `relevo_run`, `relevo_set_ipl`, `relevo_invalidate`). Estado de la CPU, tabla de
  65 536 entradas, accesos big-endian, bucle del intérprete. Prueba: `NOP` y `RTS` a 0 terminan.
- [ ] R2 **Intérprete 68000.** Todas las instrucciones, modos de dirección, excepciones (grupos 0, 1 y 2, error de
  dirección y de bus), modo supervisor, `STOP`, `TRACE`, interrupciones, ciclos del manual. Hecho cuando
  SingleStepTests/m68000 pasa entero (registros, memoria y, si se puede, ciclos). Se puede repartir por grupos de
  instrucciones entre dos agentes.
- [ ] R3 **68010 a 68040 sin FPU.** VBR, `MOVEC`, `MOVES`, `RTD`; modos de dirección del 68020 (índice escalado,
  indirectos con memoria); campos de bits; `MULx.L`, `DIVx.L`; `CAS`, `CAS2`; `CHK2`, `CMP2`; `PACK`, `UNPK`;
  `EXTB.L`; `TRAPcc`; `MOVE16`; caché (`CACR`, `CPUSH`, `CINV`). Pruebas propias por instrucción, sacadas de los
  manuales.
- [ ] R4 **FPU.** 68881/68882 y la del 040 (las que faltan en el 040 van a la excepción, como en el real). Precisión
  extendida en software propio.
- [ ] R5 **Banco diferencial.** Los programas de `dashos/jit-host/pruebas68k` en RelevoJIT, en Emu68
  (`qemu-aarch64_be`) y en FS-UAE con el Kickstart real; las salidas, iguales.
- [ ] R6 **JIT x86-64.** Emisor, bloques, asignador de registros, memoria con `MOVBE`, banderas, encadenado, caché e
  invalidación. Diferencial contra el intérprete con estados al azar.
- [ ] R7 **JIT de la FPU y rendimiento.**

## Bloqueos

(ninguno)

## Bitácora

### 2026-11-14 — Creación

Repositorio, licencia MPL-2.0, README y diseño.
