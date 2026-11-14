# Revisión del plan y la arquitectura (2026-11-15)

La pidió el usuario: priorizar compatibilidad, estabilidad y velocidad, por ese orden. Estado revisado: `main` tras
R7 (intérprete 68000 a 68040 con FPU, JIT x86-64 con encadenado; 101 pruebas, SingleStepTests entero con los dos
motores, banco contra FS-UAE).

## Lo que está bien y no se toca

- Dos motores con la misma interfaz, y el intérprete como referencia del JIT.
- Cada instrucción del JIT en dos fases (comprobar y luego hacer): un fallo sale con el estado intacto y lo resuelve
  el intérprete. Por eso las excepciones salen iguales en los dos motores.
- Sin estado global, páginas de 64 KB, código que se modifica detectado por páginas marcadas.
- La prueba diferencial al azar (`jitdiff`), que ya encontró fallos reales.

## Compatibilidad

1. **No ha corrido ningún programa de verdad** (grave). Todo lo probado son instrucciones sueltas o bloques al azar.
   Los fallos de un JIT salen en programas largos: combinaciones de banderas entre bloques, código que se
   modifica, pilas profundas. Falta un banco de programas reales compilados para 68k (Dhrystone, CoreMark, un
   compresor, un intérprete pequeño), sin sistema operativo, con salida comparada entre intérprete, JIT, Emu68 y
   FS-UAE. Y un modo "en paralelo": los dos motores ejecutan el mismo programa y se comparan en cada salida de bloque.
2. **El oráculo se equivoca.** FS-UAE falla en 7 casos enteros y en casi toda la FPU. Ahí manda el manual, pero sin
   segunda referencia. Opciones: un Amiga real con 68020/030 (el mismo programa del banco corre tal cual), y para la
   aritmética de la FPU, TestFloat de Berkeley (licencia BSD), que prueba el redondeo IEEE caso a caso.
3. **Transcendentales de la FPU** (seno, logaritmo...): usan la biblioteca matemática del anfitrión. El resultado
   cambia en el último bit según el sistema donde se compile, y no coincide con un 68881. Un programa da resultados
   distintos en Linux y en DashOS. Hay que escribirlas propias sobre `fpx`, deterministas.
4. **68060**: ejecuta como un 040 lo que el chip real deja al software. Los programas para 060 esperan esa trampa
   (68060.library). Marcarlo como experimental o hacerlo bien.
5. **MMU del 68030/040**: solo están los registros. No hay traducción de direcciones. Casi ningún programa de Amiga
   la usa (sí Enforcer, depuradores, Linux/68k). Hay que decidir si entra; si no, decirlo en el README.
6. Huecos pequeños: traza T0, `RTE` de un marco de fallo de bus que no reanuda la instrucción, `CALLM`/`RTM`.
7. **Tiempo exacto**: con el JIT los ciclos son una estimación y las interrupciones llegan hasta 64 instrucciones
   tarde. Vale para la caja Amiga (programas del sistema). Para la máquina entera con chips (S18), el intérprete.
   Dejarlo escrito como dos perfiles: "sistema" (JIT) y "exacto" (intérprete, 68000 con ciclos).

## Estabilidad

1. **No hay integración continua** y el repositorio es público. GitHub Actions con gcc y clang, ASan y UBSan, y
   Windows (que hoy está escrito y sin probar; con MSVC no detecta `MOVBE`).
2. **El contrato de `relevo_invalidate`** es una trampa: si el anfitrión escribe código y no avisa, el JIT ejecuta
   la traducción vieja y el fallo aparece lejos. Hace falta un modo de comprobación (resumen del código al entrar
   en cada bloque, lento) para cazarlo en desarrollo.
3. **Basura como código**: `jitdiff` genera sobre todo instrucciones válidas. Falta una tanda con bytes al azar
   puros (los dos motores iguales y sin caerse), y un objetivo de libFuzzer.
4. **Hilos**: `relevo_set_ipl` y la petición de parada se pueden llamar desde otro hilo, pero no hay prueba con
   ThreadSanitizer.
5. **Pruebas de aguante**: invalidaciones al azar mientras corre código encadenado, memoria de código llena, horas
   de ejecución de los programas reales.
6. **Interfaz**: falta número de versión en `relevo.h` y guardar y recuperar el estado entero de la CPU (lo pide
   "congelar un programa" de DashOS).

## Velocidad

Hoy (millones de instrucciones 68k por segundo): aritmética 3600, copia 1800, saltos 3100, coma flotante 76, página
del anfitrión 143. Son programas de juguete; hay que medir en los reales del punto 1 de compatibilidad.

1. **FPU con x87** (la ganancia mayor: 10 veces o más). El x87 del PC tiene el mismo formato extendido de 80 bits,
   los mismos modos de redondeo y control de precisión. La prueba de R4 ya dio 600 000 operaciones idénticas, en
   valor y banderas, entre `fpx` y el x87. El JIT puede emitir x87 para suma, resta, producto, división, raíz y
   comparación, y caer a `fpx` en los casos raros (NaN, desnormales). En R7 se descartó sin medirlo.
2. **Quitar la comprobación de página en cada acceso.** Reservar 4 GB de direcciones, proyectar ahí la RAM y la
   ROM, y dejar sin proyectar lo demás: un acceso a un chip da un fallo de página, y una tabla dice a qué salida del
   bloque volver. El diseño en dos fases ya lo permite. Dobla más o menos la velocidad del código que mueve
   memoria. Cambia la interfaz (la biblioteca reserva las direcciones) y pide al anfitrión entregar fallos (Linux:
   señal; DashOS: lo previsto en S18). Opcional: el modo con comprobaciones se queda.
3. **Registros entre bloques.** Hoy cada bloque carga sus registros al entrar y los guarda al salir, aunque esté
   encadenado. Fijar los más usados (A7, D0, D1, A0, A1) en registros de x86 para todos los bloques, o enlazar
   bloques que comparten reparto.
4. **Pila de retornos** para `BSR`/`RTS` (como Emu68), en vez de buscar el destino en la caché.
5. **Páginas del anfitrión**: llamar a la función del anfitrión desde el bloque, sin pasar por el intérprete.
6. Lo que sigue en el intérprete y corta bloques: desplazamientos con la cuenta en un registro, `MOVE` con SR y
   CCR, `RTE`, `TRAP`.

## Orden propuesto

| Paso | Qué | Prioridad |
|---|---|---|
| V1 | Integración continua (Linux y Windows) | Estabilidad |
| V2 | Banco de programas reales y modo en paralelo | Compatibilidad |
| V3 | Modo de comprobación de `relevo_invalidate`, basura como código, hilos, aguante | Estabilidad |
| V4 | Transcendentales propias; TestFloat | Compatibilidad |
| V5 | FPU con x87 | Velocidad |
| V6 | Memoria por fallos de página | Velocidad |
| V7 | Registros entre bloques y pila de retornos | Velocidad |
| V8 | 68060 de verdad, T0, estado guardable, versión de la interfaz | Compatibilidad |

La velocidad va después de V2 y V3 a propósito: V5 a V7 tocan el corazón del JIT, y sin programas reales ni
pruebas de aguante no se sabría si rompen algo.

## Para decidir el usuario

1. ¿Entra la MMU del 68030/040? Recomendado: no, de momento.
2. ¿El 68060 es objetivo o basta hasta el 68040? Recomendado: hasta el 040; el 060, experimental.
3. ¿Hay acceso a un Amiga real con 68020 o 68030 para la segunda referencia?
