# Reescritura en Rust

Decisión del usuario (2026-11-15): "reescribe si va a ser una ventaja". Valoración del coordinador: lo es si se hace
ahora (antes de la MMU, el 68060 y el generador para AArch64, que duplicarán el código) y comprobada contra la
versión en C.

## Por qué

- La MMU, el 68060 y el generador para AArch64 encajan mejor con los tipos y la propiedad de Rust. El fallo más
  delicado visto hasta ahora (una función del anfitrión que quita el bloque que corre) es de propiedad y vida.
- DashOS es Rust: la caja en x86 queda en Rust de punta a punta, sin libc ni capa de puertos.
- La comprobación sale casi gratis: la biblioteca en Rust exporta la misma interfaz C (`include/relevo.h`), así que
  las pruebas en C de hoy (SingleStepTests, el banco del oráculo, `jitdiff`, `bench`) enlazan contra ella sin
  cambios, y la versión en C sirve de oráculo.

## Reglas

- Crate `relevo`, `#![no_std]` con `alloc`; característica `std` para Linux y las pruebas. Exporta `extern "C"`
  con la ABI exacta de `relevo.h` (staticlib y cdylib).
- `unsafe` solo en módulos marcados (memoria de código, entrada al código generado, la ventana de memoria del 68k,
  la frontera C); el resto con `#![deny(unsafe_code)]`, como el kernel de DashOS.
- Sala limpia igual que en `DISEÑO.md`. La versión en C (propia) se puede leer y traducir.
- La versión en C queda en `c/` hasta que la de Rust la iguale en todo; entonces se retira.

## Pasos

| Paso | Qué | Hecho cuando | Estado |
|---|---|---|---|
| RR0 | Crate, compilación (Makefile llama a cargo), la interfaz C completa con stubs | Las pruebas en C enlazan contra la biblioteca de Rust | [x] 2026-11-15 |
| RR1 | Intérprete 68000, excepciones, ciclos | SingleStepTests 317 500/317 500 con ciclos | [x] 2026-11-15 |
| RR2 | 68010 a 68040 (sin MMU) y la FPU (`fpx`) | Pruebas propias y banco del oráculo iguales | [x] 2026-11-15 |
| RR3 | JIT x86-64 (emisor, bloques, encadenado, banderas perezosas, FPU) | `jitdiff`, `sst JIT=1` y `bench` sin bajar | [ ] |
| RR4 | Diferencial al azar C contra Rust; se retira la versión en C | Millones de casos iguales | [ ] |

Cómo va (RR0 a RR2, ver la bitácora del 2026-11-15): `make` compila el crate y enlaza contra él las pruebas en C;
`make c-test` (cualquier objetivo con `c-` delante) hace lo mismo con la versión en C de `c/`. Lo que la de Rust no
tiene aún (el JIT) se salta con aviso (`tests/skip_rust.c` y el `Makefile`). `make crdiff` ya compara las dos
bibliotecas con código al azar en todos los modelos (el diferencial de RR4, adelantado a RR2).

Después siguen V2 a V9 de `REVISION.md` (programas reales, aguante, x87, memoria por fallos de página, 68060,
MMU), ya en Rust.
