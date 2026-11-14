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
  Todas las instrucciones, modos de dirección, excepciones (grupos 0, 1 y 2, error de
  dirección y de bus), modo supervisor, `STOP`, `TRACE`, interrupciones, ciclos del manual. Hecho cuando
  SingleStepTests/m68000 pasa entero (registros, memoria y, si se puede, ciclos). Se puede repartir por grupos de
  instrucciones entre dos agentes.
- [x] R3 **68010 a 68040 sin FPU.** En dos partes a la vez, en worktrees aparte:
  - [x] R3a (2026-11-14): el banco contra el oráculo (programas 68020 generados que corren en FS-UAE con el Kickstart real y en
    Emu68 por `jit-host`, y comparan registros y memoria con RelevoJIT), y lo del 68010: VBR, `MOVEC`, `MOVES`, `RTD`,
    marcos de excepción del 68010 en adelante (formatos 0, 2, 9, A, B del 020/030; 0, 2, 3, 4, 7 del 040).
  - [x] R3b (2026-11-14): modos de dirección del 68020 (índice escalado, base y desplazamientos largos, indirectos con memoria),
    `MULx.L`, `DIVx.L`, `EXTB.L`, `CHK.L`, `CHK2`, `CMP2`, `CAS`, `CAS2`, `TRAPcc`, `PACK`, `UNPK`, campos de bits,
    `Bcc.L`, `LINK.L`, `MOVE16` y caché (`CACR`, `CPUSH`, `CINV`).
- [~] R4 **FPU.** 68881/68882 y la del 040 (las que faltan en el 040 van a la excepción, como en el real). Precisión
  extendida en software propio.
- [ ] R5 **Banco diferencial.** Los programas de `dashos/jit-host/pruebas68k` en RelevoJIT, en Emu68
  (`qemu-aarch64_be`) y en FS-UAE con el Kickstart real; las salidas, iguales.
- [~] R6 **JIT x86-64.** En partes:
  - [x] R6a (2026-11-14): la base. Emisor x86-64, bloques y caché de traducciones, registros del 68k en memoria con asignador por
    bloque, memoria con `MOVBE` (o `BSWAP`), banderas solo cuando alguien las lee, salida al bucle con
    interrupciones. Las instrucciones que no traduce caen al intérprete (una a una, dentro del bloque o cortándolo).
    Traduce de verdad un primer grupo: `MOVE`, `MOVEQ`, `LEA`, `ADD`, `SUB`, `CMP`, `AND`, `OR`, `EOR`, `TST`, `Bcc`,
    `DBcc`, `BRA`, `BSR`, `JSR`, `RTS`. Diferencial contra el intérprete con estados al azar, y SingleStepTests y el
    banco del oráculo también en modo JIT.
  - [ ] R6b: el resto de instrucciones enteras, encadenado de bloques, invalidación por escritura en código.
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

### 2026-11-14 — R3b

Hecho:

- Modos de dirección del 68020 en `ea.c`: índice escalado (×1, 2, 4, 8) en la palabra corta, y la palabra de
  extensión completa (supresión de base y de índice, desplazamientos de base y exteriores de 16 y 32 bits,
  indirectos con memoria pre y post indexados), también relativos al PC. Las codificaciones reservadas dan
  instrucción ilegal. El 68000 y el 68010 siguen leyendo la palabra corta como antes (sin escala).
- Grupo nuevo `020` (una línea en `OPS_GROUPS`), solo del 68020 en adelante:
  - `ops_020.c`: `MULU.L`/`MULS.L` (32 y 64 bits), `DIVU.L`/`DIVS.L`/`DIVUL.L`/`DIVSL.L` (32 y 64 bits),
    `EXTB.L`, `CHK.L`, `CHK2`, `CMP2`, `CAS`, `CAS2`, `TRAPcc` (sin operando, .W y .L), `PACK`, `UNPK`, `LINK.L`,
    `CMPI` con operando relativo al PC y `TST` con `An` (.W y .L), relativo al PC e inmediato.
  - `ops_020_bf.c`: `BFTST`, `BFEXTU`, `BFEXTS`, `BFCHG`, `BFCLR`, `BFSET`, `BFFFO`, `BFINS`, en registro (con la
    vuelta del bit 0 al 31) y en memoria (desplazamiento con signo, hasta 5 bytes).
  - `ops_020_cache.c` (68040 en adelante): `MOVE16` (las cinco formas), `CINV` y `CPUSH` (privilegiadas; la de
    instrucciones llama a `relevo_invalidate` por línea, página de 8 KB o todo).
- `src/cache.c` y `cache.h`: `cache_write_cacr()` guarda los bits de CACR que tiene cada modelo (020, 030, 040,
  060) y, en el 020 y el 030, C y CE invalidan como `CINV`. Es para el `MOVEC` de R3a, que solo tiene que llamarla.
- `Bcc.L`, `BRA.L` y `BSR.L` ya estaban (R2a); ahora tienen prueba.
- 23 pruebas nuevas en `tests/test_020.c`, sacadas de los manuales. Las codificaciones, comprobadas con vasm.

Comprobado:

- `make test` (59 pruebas) y `make sst` (317 500 de 317 500, con ciclos) con gcc 16 y clang 23, también con ASan y
  UBSan.
- `table_no_conflicts` pasa en todos los modelos con el grupo nuevo.

Decisiones donde el manual dice "indefinido" (sin comprobar contra un 68020 real; el banco de R3a lo dirá):

- `DIVx.L` que desborda: V a 1, C a 0, N a 1 y Z a 0, como `DIVx.W` en el 68000. Los registros no cambian.
- `MULx.L` de 64 bits con Dh = Dl: queda la mitad alta. `DIVx.L` con Dr = Dq: queda el cociente.
- `CHK.L`: Z, V y C como `CHK.W` en el 68000 (Z si Dn es 0; V y C a 0).
- `CMP2`/`CHK2`: dentro si Rn - inferior <= superior - inferior, sin signo (vale para límites con y sin signo, como
  Emu68). N y V no cambian.
- `CAS2` que falla con Dc1 = Dc2: queda el primer operando. `MOVE16 (Ax)+,(Ay)+` con Ax = Ay: sube 32.
- `PACK`/`UNPK` con `-(A7)`: baja 2 por byte, como los demás accesos de byte a la pila.
- El bit 3 de la palabra de extensión completa se ignora.

No hecho:

- Ciclos del 020 en adelante: solo aproximados (los accesos cuentan como en el 68000 y cada instrucción suma un
  poco).
- Marcos: `CHK`, `CHK2`, `TRAPcc`, `TRAPV` y la división por cero llaman a `cpu_exception()` con el PC siguiente, y
  hoy sale el formato 0. En el 020+ deberían ser formato 2 con la dirección de la instrucción (`c->insn_pc`): es de
  R3a, en `exception.c`.
- 68060: `MULx.L`/`DIVx.L` de 64 bits, `CHK2`, `CMP2` y `CAS2` se ejecutan como en el 040, sin la excepción de
  instrucción no implementada.
- `CALLM`/`RTM` (solo 68020) y las instrucciones de la MMU no están.
- No hay prueba de que `CINV`/`CPUSH` llamen de verdad a `relevo_invalidate`: el intérprete no guarda nada que se
  pueda mirar. Se verá con el JIT (R6).

Ficheros comunes tocados: `src/interp/ea.c` (modos nuevos, aparte del camino del 68000), `src/interp/interp.h` (una
línea en `OPS_GROUPS`), `Makefile` (`tests/test_020.c` en la lista) y `tests/tests.h`.

### 2026-11-14 — R3a

Hecho:

- El banco contra el oráculo, en `tools/oraculo/`. `oraculo.py gen` genera programas 68k con vasm: cada uno ejecuta
  cientos de casos de un grupo de instrucciones, con registros, banderas y memoria al azar (y valores de borde), y
  guarda tras cada caso D0-D7, A0-A7, SR, el vector de la excepción si la hubo, 32 bytes de memoria y 32 del marco.
  Seis programas: `arith`, `move`, `branch` (lo del 68000, en modo 68020), `m68010`, `frames` (excepciones y `RTE`
  con cada formato) y `m68020` (lo de R3b).
- Elección: el mismo binario plano corre en los tres sitios, en $140000, en modo supervisor y con las interrupciones
  enmascaradas; datos y resultados en direcciones fijas ($15C000-$16FFFF), así que todo se compara tal cual. Usa
  solo instrucciones del 68000 y `MOVEC`, y su propia tabla de vectores (VBR). En FS-UAE lo envuelve
  `tools/oraculo/amiga.s` (AllocAbs, Supervisor, la salida en hex por el Shell); en RelevoJIT, `tests/oraculo.c`;
  en Emu68, `jit-host` tal cual (carga en $140000 y vuelca la memoria). Así no hace falta imitar AmigaOS.
- `make test` ejecuta `build/oraculo` sobre `tests/oraculo/*.txt` (la salida de FS-UAE como 68020 y como 68040,
  texto) con los `.bin` de al lado. `make oraculo` lo regenera todo (FS-UAE con `DASH_KICKSTART`; Emu68 si hay un
  `jit-host` reciente, `JIT_HOST=`). Un `?` en el oráculo no se compara; una línea `NNNN x`, el caso entero.
- `src/interp/ops_010.c`, grupo nuevo `010` (desde el 68010): `MOVEC` con los registros de cada modelo hasta el 060
  (los que faltan dan instrucción ilegal; CACR por `cache_write_cacr`), `MOVES` (acceso normal: no hay códigos de
  función en el bus), `RTD`, `MOVE CCR,<ea>`. `MOVE SR,<ea>` privilegiada desde el 68010. Registros nuevos en
  `relevo_regs`: TC, ITT0/1, DTT0/1, MMUSR, URP, SRP, BUSCR, PCR.
- Marcos (`exception.c`): formato 2 en el 020+ para división por cero, `CHK`/`CHK2`, `TRAPV`/`TRAPcc` y traza;
  errores de bus y de dirección con formato 8 (68010), B (020/030), 2 o 7 (040); trama de descarte (formato 1)
  cuando llega una interrupción con M puesto. `RTE` lee el formato y quita lo que toca en cada modelo; un formato
  que el modelo no tiene es error de formato (vector 14). Tras un error de bus, `RTE` repite la instrucción entera.
- 7 pruebas unitarias nuevas (`tests/test_010.c`): lo que el oráculo no alcanza (el 68010, errores de bus,
  interrupción con M, error de formato).

Comprobado contra el oráculo (FS-UAE 3.2.35, Kickstart 40.68, A1200 con 68EC020 y con 68040):

- 1525 casos por modelo; RelevoJIT da lo mismo en todos los que se comparan. Emu68 (`jit-host`) da lo mismo que el
  68040 en `arith`, `move` y `branch`.
- Lo que el oráculo decidió, donde los manuales dicen "indefinido" (ahora RelevoJIT hace lo mismo):
  - `CHK.W`/`CHK.L` (020+): Z, V y C no cambian; N a 0 si Dn > límite, si no a 1 si Dn < 0. Cambia la decisión de R3b.
  - División por cero: en el 020, `DIVU.W` pone V, N del signo del dividendo y Z al revés; `DIVS.W` solo Z. En el
    040, `DIVU.W` borra N, Z, V y C; `DIVS.W` solo C. `DIVx.L`: nada cambia.
  - `MULx.L`: N y Z del producto de 64 bits, también en 32 bits con desborde. Con Dh = Dl queda la parte baja
    (R3b dejaba la alta).
  - `DIVx.L` con desborde: N a 1, como decidió R3b; Z sale a veces a 1 y no he dado con la regla (no se compara).
  - Formato 2 en el 040: la dirección es la de la instrucción siguiente (en el 020, la de la propia). Error de
    dirección en el 040: la dirección con el bit 0 a cero. Error de formato: se quitan antes las 4 primeras palabras
    del marco malo. MMUSR guarda los 32 bits.
- Lo que FS-UAE hace contra lo que dicen los manuales (RelevoJIT sigue los manuales; esos casos no se comparan y
  llevan una nota en su línea):
  - `TRAPcc` y `CHK2` apilan como PC su propia dirección, no la siguiente.
  - `CMP2`/`CHK2` comparan siempre con signo (los manuales admiten límites sin signo).
  - `CAS2.W` que falla pone en la mitad alta de Dc la de Du.
  - `DIVS.L` de 64 bits con cociente de más de 32 bits: sin desborde y cociente 0.
  - `RTE` acepta los formatos 4, 7, 8, 9, A y B en el 020 y en el 040, y no el 3 en el 040.
  - Error de dirección del 020: el marco B lleva la palabra de estado en +8, no en +10 (no se comparan +8 a +15).

No hecho:

- Errores de bus contra el oráculo: un Amiga no los da. Solo pruebas unitarias.
- Emu68 en `jit-host` muere (bajo `qemu-aarch64_be`) al escribir SR o al tomar una excepción: solo corre los tres
  programas sin excepciones. El `jit-host` compilado en `dashos` es anterior a U7 (no carga programas): hay que
  recompilarlo (`make -C ~/Projects/dashos/jit-host`) o dar otro con `JIT_HOST=`.
- `RTE` de un marco de fallo no termina el ciclo de bus: repite la instrucción. Traza T0 (cambio de flujo), sin
  hacer. El 68030 y el 060 no están en el oráculo (FS-UAE con `cpu = 68030` se puede añadir a `MODELS`).
- Los textos del oráculo ocupan unos 800 KB; los `.bin`, 270 KB.

Decisión del coordinador (2026-11-14): en las 7 diferencias con FS-UAE manda el manual. FS-UAE no es exacto en
estos casos raros (`DIVS.L` sin desborde con cociente de 64 bits lo delata). Quedan pendientes de un 68020 real.

Cómo usar el banco (para R3b y lo que venga):

- Un programa es una función `prog_<nombre>(rng)` en `tools/oraculo/oraculo.py` que devuelve casos (`Case`: código
  en sintaxis de vasm, registros, SR, BUF); se apunta en `PROGRAMS`. Como mucho 496 casos por programa.
- `oraculo.py gen NOMBRE`, `oraculo.py fsuae --cpu 68020 NOMBRE` (y `--cpu 68040`), `make`, y
  `oraculo.py relevo NOMBRE` (o `build/oraculo -v 20 tests/oraculo/NOMBRE.68020.txt`) enseña las diferencias por
  instrucción y campo. `build/bench/NOMBRE.CPU.raw.txt` es la salida de FS-UAE sin máscaras.
- Lo que el manual deja indefinido se toma del oráculo. Si el oráculo contradice al manual, `case.skip(...)` y una
  nota en `case.desc`.

Ficheros comunes tocados: `src/exception.c` (marcos), `src/interp/interp.h` (una línea en `OPS_GROUPS`),
`src/interp/ops_control.c` (`RTE`, `CHK.W` del 020+), `src/interp/ops_system.c` (`MOVE SR` privilegiada),
`src/interp/ops_muldiv.c` y `ops_020.c`/`ops_020.h` (banderas del oráculo), `src/cpu.h` y `src/memory.c` (tamaño del
acceso que falla), `include/relevo.h` (registros de la MMU), `Makefile`, `tests/tests.h`, `tests/test_020.c`
(pruebas de R3b con las banderas nuevas).

### 2026-11-14 — R6a

Hecho:

- Motor JIT (`RELEVO_ENGINE_JIT`), en `src/jit/`. `relevo_create()` lo acepta en x86-64; en otra CPU devuelve NULL.
  Sin estado global: cada CPU tiene su memoria de código, su caché y su trampolín.
- `codemem.c`: memoria ejecutable con W^X. En Linux, la misma memoria (un memfd) mapeada dos veces, RW para escribir
  y RX para ejecutar; si no se puede, un solo mapeo que pasa de RX a RW con `mprotect` mientras se escribe. Para
  otros sistemas hay que dar esas cuatro funciones (Windows: `VirtualAlloc`); el trampolín ya sabe la convención de
  llamada de Microsoft x64.
- `decode.c`: decodificador propio del JIT. Solo acepta lo que traduce, y nada que la tabla del intérprete dé por
  ilegal en ese modelo. Los accesos a direcciones fijas (absolutas, relativas al PC) se comprueban al traducir.
- `x86_64/emit.h`: emisor x86-64 propio (REX, ModRM, SIB, `MOVBE`, saltos con parches).
- `x86_64/translate.c`: RBX apunta a la CPU, R15 a la memoria 68k, R14 lleva SR. Nueve registros de x86 (RSI, RDI,
  RBP, R8-R13) para los del 68k, por orden de uso; el bloque se corta antes de necesitar un décimo. Se cargan al
  entrar y se guardan los cambiados al salir.
- Memoria: `[R15 + dirección]` con `MOVBE` si la CPU lo tiene (CPUID), si no `BSWAP`, `ROL` y `XCHG CL,CH` (que no
  tocan las banderas al escribir). Antes de cada acceso, en este orden: dirección impar en 68000/010, tipo de página
  (RAM, o ROM para leer), que no cruce de página y, al escribir, que no caiga en el código del propio bloque.
- Cada instrucción se emite en dos fases: direcciones y comprobaciones, y luego los efectos. Si una comprobación
  falla, el bloque sale con el estado de antes de esa instrucción ("bail") y el intérprete la ejecuta. Así los errores
  de dirección y de bus, las páginas del anfitrión y los marcos del 68000 salen del intérprete, iguales. Los bloques
  nunca llaman a C ni hacen `longjmp`.
- Banderas: N, Z, V y C de SF, ZF, OF y CF; X es C cuando toca. Análisis hacia atrás en el bloque: solo se guardan en
  R14 las que alguien lee después (otra instrucción, la salida del bloque o una instrucción que puede salir al
  intérprete). Las condiciones se evalúan con una tabla de 16 bits y `BT`.
- Bloque: hasta un salto o rama, una instrucción no traducida (el bloque se corta; esa la hace el intérprete), 64
  instrucciones, el presupuesto o el décimo registro. Un bloque que acaba en una rama a su propio inicio da la
  vuelta dentro de él mientras quede presupuesto y nadie pida atención (`relevo_set_ipl`, `relevo_request_stop`).
- Caché (`jit.c`): por dirección 68k, con un resumen (hash de 64 bits) del código, que se mira cada vez que se entra
  en el bloque, salvo si todo está en ROM. Las direcciones que no se traducen también se guardan, para no
  decodificarlas cada vez. `relevo_invalidate()` quita lo que toque el rango; `relevo_map()` lo vacía todo. Si se
  llena la memoria de código, se vacía todo.
- El bucle entre bloques hace lo mismo que el del intérprete: traza, interrupciones, `STOP`, petición de parada,
  presupuesto exacto y salida por PC 0. Con T1 puesto, todo va al intérprete. Al salir de un bloque deja en IR el
  último código de operación (el marco de un error al leer el siguiente lo lleva).
- Traduce `MOVE`, `MOVEA`, `MOVEQ`, `LEA`, `ADD`, `ADDA`, `ADDQ`, `SUB`, `SUBA`, `SUBQ`, `CMP`, `CMPA`, `AND`, `OR`,
  `EOR`, `ADDI`, `SUBI`, `CMPI`, `ANDI`, `ORI`, `EORI`, `TST`, `CLR`, `Bcc`, `BRA`, `BSR`, `DBcc`, `JSR`, `JMP`,
  `RTS` y `NOP`, con todos los modos de dirección del 68000. Del 020: la escala del índice y `Bcc.L`; la palabra de
  extensión completa va al intérprete.
- `cpu.c`: el paso del intérprete sale a `cpu_interp_step()`, que usan los dos bucles. Ciclos con el JIT: una
  estimación (4 por palabra y por acceso).

Pruebas nuevas:

- `build/jitdiff` (`tests/jitdiff.c`): diferencial contra el intérprete. Instrucciones al azar (las que se traducen,
  con operandos al azar, y algunas que no), registros, banderas y datos al azar, páginas de ROM, del anfitrión y vacías,
  direcciones impares, modo usuario, traza. Compara registros, SR, PC, instrucciones contadas, estado de salida, los
  accesos al anfitrión y la memoria (ventanas en cada caso, los 16 MB cada 256). `-k 1` instrucción a instrucción;
  `-k N` bloques de hasta N; `-x` con `BSWAP` en vez de `MOVBE`.
- `make test` corre además `build/oraculo -j`, `jitdiff` (68000 y 68020, con `MOVBE` y con `BSWAP`) y la NOP de
  SingleStepTests con `-j`. `make sst JIT=1` corre SingleStepTests con el JIT. `make jitdiff` es la tanda larga.
- 10 pruebas unitarias (`tests/test_jit.c`): bucle en un bloque, presupuesto exacto, código que cambia, ROM con
  `relevo_invalidate`, código que se cambia a sí mismo dentro del bloque, página del anfitrión dentro de un bloque,
  error de dirección, interrupción y parada, llamadas.

Comprobado:

- `make test` (76 pruebas, el oráculo con los dos motores, `jitdiff`) con gcc 16 y clang 23, también con ASan y UBSan.
  `make sst`: 317 500 de 317 500 con el intérprete (ciclos incluidos) y con el JIT.
- `make jitdiff`: 1,8 millones de casos (68000, 68010, 68020 y 68040), todos iguales. Antes, otros 3,9 millones con
  tres semillas.
- El camino de `mprotect` (sin memfd), a mano: pruebas y `jitdiff` iguales.
- Lo que encontró `jitdiff` y se arregló: IR viejo en el marco del 68000 tras un bloque, y código que se escribe a sí
  mismo dentro del bloque.

Velocidad (`make bench`, gcc -O2, millones de instrucciones 68k por segundo, 68020):

| Programa | Intérprete | JIT | |
|---|---|---|---|
| Bucle aritmético (`ADD`, `EOR`, `SUBQ`, `BNE`) | 124 | 1963 | 16x |
| Copia de memoria (`MOVE.L (A0)+,(A1)+`, `DBRA`) | 118 | 894 | 7,6x |
| Bytes con `CMP`, `Bcc`, `BSR` y `RTS` | 134 | 292 | 2,2x |

El último sale al bucle en cada rama: ahí está la ganancia del encadenado (R6b).

Para R6b:

- El resto de instrucciones enteras (`MOVEM`, desplazamientos, bits, `EXT`, `SWAP`, `Scc`, `NEG`, `NOT`, `ADDX`,
  `SUBX`, `MULx`, `DIVx`, `LINK`, `UNLK`, `PEA`, `EXG`...) y la palabra de extensión completa del 020.
- Encadenado de bloques (hoy solo la vuelta al propio inicio), con la misma mirada a `attention` y al presupuesto.
- Invalidación por escritura en código: hoy hay hash al entrar y la comprobación del propio bloque; falta evitar el
  hash en cada entrada (marcar páginas con código y comprobarlas al escribir).
- Banderas: dejarlas en EFLAGS entre instrucciones y juntar `CMP` + `Bcc`; la conversión a CCR es larga.
- Accesos al anfitrión desde el bloque (una llamada) en vez de salir al intérprete; comprobación de página más
  barata (o quitarla cuando el anfitrión dé fallo de página, S18).
- Las interrupciones se atienden entre bloques (hasta 64 instrucciones tarde, o una vuelta del bucle interno).
- Falta `codemem` para Windows.

Ficheros comunes tocados: `Makefile` (fuentes de `src/jit/`, `jitdiff`, `bench`, objetivos nuevos), `include/relevo.h`
(comentarios del motor JIT, de los ciclos y de `relevo_run`), `src/cpu.h` (campos `engine`, `jit`, `attention`;
`cpu_between_needed()` y `cpu_interp_step()`), `src/cpu.c` (el paso del intérprete aparte, crear y destruir el JIT,
`relevo_map`, `relevo_invalidate`, `attention`), `tests/tests.h`, `tests/test_api.c` (la prueba que esperaba que
no hubiera JIT), `tests/sst.c`, `tests/sst_run.c` y `.h` (`-j`), `tests/oraculo.c` (`-j`) y `README.md`. No se
tocaron `interp.h`, `table.c` ni `exception.c`.
