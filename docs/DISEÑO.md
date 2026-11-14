# RelevoJIT: diseño

Del 2026-11-14. Decisiones del usuario de ese día (`~/Projects/dashos`, `docs/diseño/S21-x86.md` en la rama `x86`):
el JIT 68k para x86-64 va en un proyecto aparte, MPL-2.0, como biblioteca; intérprete y JIT; los registros del 68k
en memoria y cargados en registros de x86 por bloque; la memoria del 68k en big-endian y el código en
little-endian, dando la vuelta a los bytes en cada acceso.

## Qué es

Una biblioteca en C que ejecuta código del Motorola 680x0 en x86-64. Quien la usa (DashOS, un emulador, un
sistema) le da la memoria, los registros y unas funciones para lo que no es memoria normal (los chips, las
llamadas a lo nativo). RelevoJIT no sabe nada del Amiga: no emula chips, ni carga ROM, ni arranca sola.

Tiene dos motores con la misma interfaz:

- **Intérprete.** Portable (C11, cualquier CPU), correcto y lento. Es la referencia del JIT y sirve donde la
  exactitud importa más que la velocidad.
- **JIT x86-64.** Traduce bloques de 68k a x86-64. Solo en x86-64 (System V y Microsoft x64).

## Sala limpia

- **Se puede leer:** los manuales de Motorola (M68000PRM, MC68020UM, MC68030UM, MC68040UM, MC68060UM, MC68881UM),
  Emu68 (MPL-2.0: se puede copiar con su aviso y sigue siendo MPL), y código con licencia permisiva (MIT, BSD,
  MPL) tras comprobar la cabecera, citándolo.
- **No se lee:** UAE, WinUAE, FS-UAE por dentro, Amiberry, E-UAE (GPL); Musashi (su licencia cambió con los años y no
  merece el riesgo); AROS; desensamblados del Kickstart. Nada de ROM en el repositorio.
- **Datos de prueba:** SingleStepTests/m68000 (MIT, sacadas del núcleo microcodificado de MAME). Se descargan al
  probar (`tests/datos/`, fuera de git); no se copian.

## La interfaz

Una cabecera, `include/relevo.h`. Esbozo (lo fija la tarea R1):

- `relevo_config`: modelo de CPU (68000, 68010, 68020, 68030, 68040, 68060), FPU (ninguna, 68881/68882, la del
  040/060), motor (intérprete o JIT) y las funciones del anfitrión.
- **Memoria.** El anfitrión da un puntero base y un tamaño: la dirección 68k `a` está en `base + a`, en
  big-endian. Una tabla de páginas de 64 KB marca cuáles son memoria normal y cuáles van a las funciones de lectura
  y escritura del anfitrión (los chips del Amiga en `0xDFF000`, los CIA). Así el camino rápido es un acceso directo,
  y el lento una llamada.
- **Excepciones y trampas.** La línea A y la línea F pueden ir a una función del anfitrión antes que al vector (las
  llamadas nativas de DashOS, como el parche 0002 de Emu68). Las demás excepciones van por el vector, como en un 68k.
- **Interrupciones.** `relevo_set_ipl(cpu, nivel)`: el anfitrión pone el nivel; la CPU lo atiende entre
  instrucciones (intérprete) o entre bloques (JIT).
- **Ejecutar.** `relevo_run(cpu, presupuesto)`: corre hasta gastar el presupuesto (instrucciones o ciclos), hasta
  `STOP`, o hasta que una función del anfitrión pida parar.
- **Registros.** Una estructura pública con D0-D7, A0-A7, PC, SR, USP, SSP (ISP y MSP), VBR, CACR, SFC, DFC y la FPU.
- **Caché.** `relevo_invalidate(cpu, dirección, tamaño)` para el código que se carga o se cambia; `CPUSH` y `CINV`
  la llaman solas.

Sin estado global: varias CPU en el mismo proceso, cada una en su hilo.

## El intérprete

- Una tabla de 65 536 entradas, una por código de operación, con la función que lo ejecuta. Se rellena al crear la
  CPU según el modelo (lo que no existe en ese modelo, a la excepción de instrucción ilegal o de línea F).
- Banderas calculadas al momento (es la referencia: nada de pereza).
- Ciclos del 68000 contados según el manual, para quien necesite exactitud de tiempo (la máquina Amiga entera de
  DashOS, S18). En el resto de modelos, aproximados.

## El JIT x86-64

- **Bloques.** Desde un PC hasta un salto, una excepción posible o un tope de instrucciones. Caché de traducciones
  por dirección 68k, con un resumen del código para detectar cambios, como Emu68.
- **Registros.** Los del 68k viven en una estructura en memoria. Un registro fijo de x86 apunta a ella y otro a la
  base de la memoria 68k. Dentro de un bloque, un asignador simple carga en registros de x86 los del 68k que se usan y
  guarda los cambiados al salir.
- **Memoria.** Acceso directo `[base + dirección]` con `MOVBE` (o `BSWAP`/`ROL` si la CPU no lo tiene, se elige al
  arrancar). Antes, una comprobación de la tabla de páginas para los accesos a los chips, que se puede quitar cuando
  el anfitrión garantiza que esa zona da un fallo de página (DashOS lo hará, S18).
- **Banderas.** N, Z, V y C son SF, ZF, OF y CF de x86 (en la resta, CF es el préstamo, como en el 68k). Se guardan
  solo cuando alguien las lee: análisis hacia atrás dentro del bloque, como Emu68. X va aparte.
- **Salida del bloque.** Encadenado directo al siguiente bloque cuando se conoce el destino; si no, vuelta al bucle,
  que mira interrupciones y busca en la caché.
- **Comprobación.** Cada instrucción se prueba en el intérprete y en el JIT con los mismos estados de entrada; tienen
  que salir iguales.

## Cómo se prueba

1. **SingleStepTests/m68000**: miles de casos por instrucción con estado antes y después (68000).
2. **Diferencial intérprete contra JIT**, con estados al azar, por instrucción.
3. **Banco contra el oráculo** (`tools/oraculo/`, R3a): programas generados con cientos de casos por instrucción,
   en FS-UAE con el Kickstart real (68020 y 68040), en Emu68 y en RelevoJIT; `make test` compara con lo guardado.
4. **Programas 68k** del banco de DashOS (`jit-host/pruebas68k`), contra Emu68 en `qemu-aarch64_be` y contra el
   oráculo de FS-UAE con el Kickstart real (`tools/emu_run.py` de Dash).

## Fases

| Fase | Qué | Hecho cuando |
|---|---|---|
| R0 | Repositorio, compilación (Makefile, C11, `-Wall -Werror`), esqueleto | `make` y `make test` pasan vacíos |
| R1 | `relevo.h`, estado de la CPU, memoria con páginas, bucle | Un `NOP` y un `RTS` a 0 terminan |
| R2 | Intérprete 68000 entero, excepciones y modo supervisor | SingleStepTests/m68000 pasa entero |
| R3 | 68010 a 68040 sin FPU: modos de dirección nuevos, campos de bits, MUL/DIV largos, CAS, CHK2, PACK, MOVE16, VBR | Pruebas propias por instrucción y programas de `pruebas68k` |
| R4 | FPU 68881/68882 y la del 040 | Programas de prueba contra el oráculo |
| R5 | Banco diferencial contra Emu68 y FS-UAE | Mismas salidas en los programas de `pruebas68k` |
| R6 | JIT x86-64: emisor, bloques, registros, memoria, banderas, encadenado | Diferencial contra el intérprete limpio |
| R7 | JIT de la FPU, rendimiento | Medido contra el intérprete |
| R8 | En DashOS: la caja Amiga en x86 | Tarea X7 de DashOS |

## Lengua

El código, sus comentarios y el README van en inglés, porque es para que lo use más gente. Este diseño y la
bitácora (`docs/BITACORA.md`) van en español.
