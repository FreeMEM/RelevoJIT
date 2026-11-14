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
- [x] R2 **Intérprete 68000.** Se hace en partes: R2a (base: modos de dirección, excepciones, interrupciones, traza, `STOP`, movimiento y saltos) y después R2b (aritmética y lógica) y R2c (desplazamientos, bits, BCD, MUL/DIV) a la vez.
  - [x] R2a: base, excepciones y movimiento y saltos (2026-11-14).
  - [x] R2b: aritmética y lógica, en `src/interp/ops_arith.c` (2026-11-14).
  - [x] R2c: desplazamientos, bits, BCD, MUL/DIV, en `ops_bits.c` y `ops_muldiv.c` (2026-11-14). Con todo junto, SingleStepTests/m68000 pasa 317 500 de 317 500, con ciclos.
  - [ ] R2c: desplazamientos, bits, BCD, MUL/DIV, en `src/interp/ops_bits.c`. Todas las instrucciones, modos de dirección, excepciones (grupos 0, 1 y 2, error de
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

### 2026-11-14 — R2a

Hecho:

- Modos de dirección del 68000 en `src/interp/ea.c`: `ea_decode`, `ea_read`, `ea_write`. Las palabras de extensión
  se leen al decodificar. `(An)+` y `-(An)` cambian el registro en el momento en que lo hace el 68000.
- Accesos de la CPU con fallos (`cpu_read*`/`cpu_write*` en `src/cpu.h`): error de dirección en palabra o largo
  impar (68000 y 68010) y error de bus en página sin nada. `relevo_read`/`relevo_write` no fallan.
- Excepciones en `src/exception.c`. Grupo 0 con marco largo de 14 bytes: la palabra de estado lleva los bits altos
  de IR, como en los datos. Salen de la instrucción con `longjmp` a `relevo_run`. Fallo dentro de un fallo:
  `RELEVO_HALTED` hasta `relevo_reset`. Grupos 1 y 2, traza, interrupciones con autovector (el nivel 7 por flanco),
  `STOP` que espera una interrupción, `RESET` con una función nueva del anfitrión (`host.reset`).
- La traza queda pendiente y se toma antes de la instrucción siguiente, como en las pruebas. Ilegal, privilegio,
  línea A/F y los errores de grupo 0 la cancelan.
- Instrucciones: `ops_control.c` (Bcc, BRA, BSR, DBcc, Scc, JMP, JSR, RTS, RTR, RTE, TRAP, TRAPV, CHK, NOP),
  `ops_move.c` (MOVE, MOVEA, MOVEQ, MOVEM, MOVEP, LEA, PEA, EXG, SWAP, LINK, UNLK, CLR, TST, EXT, TAS) y
  `ops_system.c` (MOVE con SR, CCR y USP, ANDI/ORI/EORI a CCR y SR, STOP, RESET).
- Ciclos del 68000: `relevo_cycles()`. Cada acceso al bus cuenta 4 (cada palabra de la instrucción también) y cada
  instrucción suma sus ciclos internos. Así los errores de dirección salen bien solos.
- `build/sst` compara los ciclos aparte (columna "cyc", `-c N` enseña los que no cuadran). Limpia la pila entre
  pruebas y la memoria entera entre ficheros: antes una instrucción que faltaba ensuciaba las pruebas siguientes.
- 13 pruebas unitarias nuevas (`tests/test_exceptions.c`): interrupciones, NMI, `STOP`, traza, error de bus, doble
  fallo, privilegio, `RESET`, `TRAP`/`RTE`, ciclos y que la tabla no tenga dos manejadores para un código.

Lo que dicen los datos y no los manuales (comprobado contra SingleStepTests):

- El PC del marco de error de dirección depende de hasta dónde llegó la instrucción. Las reglas están en `ea.c` y
  en MOVE (`ops_move.c`).
- MOVE.W a `-(An)` apila en IR la palabra siguiente a la instrucción. MOVE.L que falla al escribir deja N y Z a
  medias según el modo. CHK pone Z si Dn es 0 y borra V y C.
- `LINK A7` guarda el SP de antes del push. `DBcc` no escribe el contador si el salto falla. `JSR` no apila si el
  destino es impar; `BSR` sí, y apila el destino como PC.
- `TRAP` con la traza puesta no saca la traza dentro de la prueba: queda pendiente.

Resultado de `make sst` para la lista de R2a: los 53 ficheros pasan 2500 de 2500, y con los ciclos exactos también
2500 de 2500. Son MOVE.b, MOVE.w, MOVE.l, MOVE.q, MOVEA.w, MOVEA.l, MOVEM.w, MOVEM.l, MOVEP.w, MOVEP.l, LEA, PEA,
EXG, SWAP, LINK, UNLINK, MOVEfromSR, MOVEtoSR, MOVEtoCCR, MOVEfromUSP, MOVEtoUSP, ANDItoCCR, ANDItoSR, ORItoCCR,
ORItoSR, EORItoCCR, EORItoSR, CLR.b/w/l, TST.b/w/l, EXT.w/l, Bcc, BSR, DBcc, Scc, JMP, JSR, RTS, RTR, RTE, TRAP,
TRAPV, CHK, NOP, ILLEGAL_LINEA, ILLEGAL_LINEF, TAS, STOP y RESET. En total 132 500 de 317 500; el resto es R2b y
R2c.

Comprobado: `make test` (35 pruebas) y `make sst` con gcc 16 y clang 23, también con ASan y UBSan.

Para R2b y R2c:

- Cada uno tiene ya su fichero y su entrada en la tabla: R2b `src/interp/ops_arith.c` (`ops_arith_init`), R2c
  `src/interp/ops_bits.c` (`ops_bits_init`). No hace falta tocar `table.c` ni `interp.h`. Si queréis más ficheros,
  el Makefile los coge solos; los llamáis desde vuestro `_init` con una cabecera propia.
- Se registra con `table_fill(tb, base, bits_libres, clase_EA, manejador)`. Las clases (`EA_DATA_ALT`...) están
  en `interp.h`. La prueba `table_no_conflicts` falla si dos grupos dan el mismo código.
- Para leer y escribir operandos: `ea_decode_op` + `ea_read` + `ea_write` (lectura, cambio, escritura). Los errores
  de dirección y su PC salen solos en los casos vistos. Banderas: `set_nz`, `set_ccr_bits`. Ciclos: `cycles(c, n)`
  con lo que el manual da de más sobre 4 por palabra y por acceso.

Falta:

- R2b y R2c.
- Marcos de grupo 0 y RTE del 68010 en adelante (R3). En el 68020+ los datos impares no fallan, pero el marco de
  instrucción impar es aún el del 68000.
- Interrupciones solo con autovector (sin ciclo IACK del anfitrión ni interrupción espuria).
- El presupuesto de `relevo_run` sigue contando instrucciones; los ciclos ya están para usarlos.

### 2026-11-14 — R2b

Hecho:

- `src/interp/ops_arith.c`: ADD, ADDA, ADDI, ADDQ, ADDX, SUB, SUBA, SUBI, SUBQ, SUBX, CMP, CMPA, CMPI, CMPM, AND,
  ANDI, OR, ORI, EOR, EORI, NEG, NEGX y NOT. Banderas con dos funciones (`add_flags`, `sub_flags`); en ADDX, SUBX y
  NEGX la Z solo se borra.
- Ciclos: los del manual menos 4 por palabra y por acceso. Lo interno que queda: 2 en .L desde memoria, 4 desde
  registro o inmediato (CMP y CMPA, siempre 2); ADDA y SUBA .W 4; ADDQ y SUBQ a An o a Dn.L 4; NEG, NEGX y NOT a
  Dn.L 2; ADDX y SUBX en memoria, 2 al empezar.
- No se tocó ningún fichero común (`table.c`, `interp.h`, `ea.c`).

Lo que dicen los datos y no los manuales (comprobado contra SingleStepTests):

- CMPM: si falla una lectura, el PC apilado va una palabra más allá. Ay ya ha avanzado 2 (en .W y en .L); Ax no
  ha avanzado.
- ADDX y SUBX con `-(An)`: el PC apilado también va una palabra más allá. En .L se lee primero la palabra baja, en
  An-2, y An no cambia hasta tener las dos; en .B y .W An baja antes de leer. Solo el primer `-(An)` cuesta 2
  ciclos. Al escribir .L, primero la palabra baja.
- Los demás casos de error de dirección salen solos con `ea_read`/`ea_write` de R2a.

Resultado de `make sst` para la lista de R2b: los 39 ficheros pasan 2500 de 2500, también con los ciclos. Son ADD,
SUB, CMP, AND, OR, EOR, NEG, NEGX, NOT, ADDX y SUBX en .b, .w y .l, y ADDA, SUBA y CMPA en .w y .l. ADDI, ADDQ,
SUBI, SUBQ, CMPI, CMPM, ANDI, ORI y EORI van dentro de esos ficheros. Las 53 de R2a siguen pasando enteras. En total
230 000 de 317 500; el resto es R2c.

Comprobado: `make test` (35 pruebas) y la lista de R2b con gcc 16 y clang 23, también con ASan y UBSan.

Falta:

- R2c.
- CMPI con operandos relativos al PC (68020 en adelante, R3).
### 2026-11-14 — R2c

Hecho:

- `src/interp/ops_bits.c`: ASL, ASR, LSL, LSR, ROL, ROR, ROXL y ROXR en registro (cuenta inmediata o en Dn, módulo
  64) y en memoria (palabra, de uno en uno); BTST, BCHG, BCLR y BSET con Dn y con inmediato; ABCD, SBCD y NBCD.
- `src/interp/ops_muldiv.c`: MULU, MULS, DIVU y DIVS, con la excepción de división por cero. Cabecera privada
  `ops_bits.h`; `ops_bits_init` llama a `ops_muldiv_init`.
- Ciclos: los de las tablas del manual. Los de MULx salen de los unos (MULU) o de los cambios entre bits vecinos
  (MULS) del operando. Los de DIVx, del bucle de restas del 68000 bit a bit: DIVU 76 a 136, 10 si desborda; DIVS
  según los signos y los ceros del cociente.
- Prueba unitaria nueva `divide_by_zero`: vector 5, PC de la instrucción siguiente, C a 0, Dn intacto y 38 ciclos
  más los del operando.

Lo que dicen los datos y no los manuales:

- BCD: N es el bit 7 del resultado. ABCD pone V si la corrección enciende el bit 7; SBCD y NBCD, si lo apaga. ABCD
  da acarreo si la suma corregida en la cifra baja pasa de 0x9F. SBCD resta 0x60 si la resta binaria se pasa de
  cero, pero C y X salen de la resta ya corregida en la cifra baja (difieren con cifras no BCD).
- DIVU y DIVS que desbordan: N a 1, Z y C a 0, V a 1; Dn no cambia.
- BTST Dn,#imm tarda 10 ciclos, no los 8 de la tabla.
- -(Ay),-(Ax) de ABCD y SBCD: 18 ciclos, un solo paso interno de 2 para los dos decrementos.

Resultado de `make sst` (con ciclos): los 35 ficheros pasan 2500 de 2500, y con los ciclos exactos también 2500 de
2500. Son ASL, ASR, LSL, LSR, ROL, ROR, ROXL y ROXR en .b, .w y .l; BTST, BSET, BCLR, BCHG; ABCD, SBCD, NBCD; MULU,
MULS, DIVU, DIVS. Los de R2a siguen enteros. En total 220 000 de 317 500 (88 de 127 ficheros); lo que falta es R2b.

Comprobado: `make test` (36 pruebas) y `make sst` con gcc 16 y clang 23, también con ASan y UBSan.

Falta:

- Los datos no traen ninguna división por cero. Solo la cubre la prueba unitaria; N, Z y V quedan como estaban
  (el manual los da por indefinidos) y no está comprobado contra un 68000 real.
- No hay errores de bus en los datos de estas instrucciones; los errores de dirección (MULx, DIVx y desplazamientos
  en memoria) sí, y pasan.
