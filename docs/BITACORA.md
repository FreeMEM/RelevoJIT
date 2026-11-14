# RelevoJIT: tareas y bitácora

Las reglas del bucle están en `~/Projects/dashos/.worktrees/x86/docs/X86_LOOP.md` (línea B). Diseño en
`docs/DISEÑO.md`.

## Tareas

- [x] R0 **Esqueleto.** Makefile (C11, `-O2 -Wall -Wextra -Werror`, gcc y clang), `librelevo.a`, `make test` con un
  ejecutor de pruebas mínimo, `tools/datos.sh` que descarga SingleStepTests/m68000 a `tests/datos/` (comprobando su
  licencia MIT), y un lector de sus ficheros JSON.
- [x] R1 **Interfaz y bucle.** `include/relevo.h` como en el diseño (configuración, memoria con páginas de 64 KB,
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

### 2026-11-14 — R0 y R1

Hecho:

- `Makefile`: C11 con `-Wall -Wextra -Werror -pedantic -O2`, `build/librelevo.a`. Compila con gcc y con clang
  (`make CC=clang`); si cambia el compilador, se recompila todo.
- `make test`: ejecutor propio (`tests/check.h`, `tests/main.c`, lista en `tests/tests.h`), 22 pruebas.
- `tools/datos.sh`: clona SingleStepTests/m68000 en `tests/datos/m68000-src`, comprueba que `LICENSE` sigue siendo
  MIT y pasa cada fichero a JSON en `tests/datos/m68000/`. Los ficheros vienen en un formato binario propio
  (`*.json.bin`), no en JSON; los convierte `tools/sst2json.py`, escrito a partir del formato que lee su
  `decode.py` (MIT). Una prueba por línea: 356 MB en total.
- Lector JSON propio (`tests/json.c`): analizador de tirar, sin memoria dinámica. Encima, `tests/sst_load.c` lee los
  casos y `tests/sst_run.c` los ejecuta: carga el estado, ejecuta una instrucción y compara registros y memoria.
  `build/sst` informa de pasados/total por instrucción.
- `include/relevo.h`: configuración (modelo, FPU, motor, memoria, máscara de direcciones, opciones), páginas de 64 KB
  (RAM, ROM, anfitrión, nada), funciones del anfitrión (lectura, escritura, línea A, línea F), registros públicos,
  `relevo_create/destroy/map/reset/run/set_sr/set_ipl/request_stop/invalidate/read/write`. Sin estado global.
- Intérprete: tabla de 65 536 manejadores por CPU, lo no hecho va a instrucción ilegal, línea A o línea F. Accesos
  big-endian directos o por el anfitrión; los que cruzan página se parten en bytes. `NOP`, `RTS` y las excepciones
  de grupo 1 y 2 con marco corto (68000 y formato 0 del 68010 en adelante).
- Opción `RELEVO_OPT_EXIT_ON_PC_ZERO`: `relevo_run` vuelve cuando el PC llega a 0, como Emu68. Apagada por defecto.

Comprobado:

- `make test` pasa con gcc 16 y clang 23, y con ASan y UBSan.
- SingleStepTests: NOP, ILLEGAL_LINEA e ILLEGAL_LINEF pasan enteros (2500 de 2500 cada uno). En total 8763 de
  317 500. RTS pasa 1263: los que fallan saltan a una dirección impar (error de dirección, R2).

No hecho (R2):

- Error de bus y de dirección, interrupciones (`relevo_set_ipl` solo guarda el nivel), traza, `STOP`, doble fallo.
- Las páginas sin nada dan 0 al leer y no guardan al escribir; serán error de bus.
- El presupuesto cuenta instrucciones, no ciclos. Los ciclos y las transacciones del bus de las pruebas no se
  comparan aún (el lector se los salta, salvo el total de ciclos).
