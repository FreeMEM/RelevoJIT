# RelevoJIT

A Motorola 680x0 interpreter and x86-64 JIT, as a C library.

RelevoJIT runs 68k code on x86-64 machines. The 68k memory stays big-endian, as on the real CPU; the JIT swaps
bytes on every access (`MOVBE` where available). It knows nothing about any particular computer: the host gives it
memory, registers and callbacks for I/O and traps.

Two engines, one API:

- **Interpreter**: portable C11, accurate, the reference for the JIT.
- **JIT**: translates 68k blocks to x86-64.

Status: early work. Nothing usable yet.

It is the 68k engine of AlborOS on x86, in the same way Emu68 is on ARM.

## License

Mozilla Public License 2.0. See `LICENSE`.

## Clean room

RelevoJIT is written from the Motorola manuals and tests. GPL emulators (UAE and its descendants) are not read.
See `docs/DISEÑO.md` (Spanish).
