# RelevoJIT

A Motorola 680x0 interpreter and x86-64 JIT, as a C library.

RelevoJIT runs 68k code on x86-64 machines. The 68k memory stays big-endian, as on the real CPU; the JIT swaps
bytes on every access (`MOVBE` where available). It knows nothing about any particular computer: the host gives it
memory, registers and callbacks for I/O and traps.

Two engines, one API:

- **Interpreter**: portable C11, accurate, the reference for the JIT.
- **JIT**: translates 68k blocks to x86-64.

Status: early work. The API, memory pages and the interpreter loop are in place; only `NOP`, `RTS` and the
illegal/line A/line F exceptions run so far.

It is the 68k engine of AlborOS on x86, in the same way Emu68 is on ARM.

## Building

```sh
make              # build/librelevo.a (and build/sst, the test runner)
make CC=clang     # any C11 compiler
make test         # unit tests, plus the NOP SingleStepTests if downloaded
make data         # download SingleStepTests/m68000 (MIT) into tests/datos/
make sst          # run every SingleStepTests file: passed/total per instruction
```

## Using it

The whole interface is `include/relevo.h`. In short:

```c
relevo_config cfg = {0};                /* zeroed: 68000, interpreter */
cfg.model = RELEVO_68020;
cfg.memory = ram;                       /* big-endian 68k memory */
cfg.memory_size = 16 << 20;             /* a multiple of 64 KB */
cfg.options = RELEVO_OPT_EXIT_ON_PC_ZERO;
relevo_cpu *cpu = relevo_create(&cfg);

relevo_map(cpu, 0xDF0000, 0x10000, RELEVO_PAGE_HOST);   /* I/O goes to cfg.host.read/write */

relevo_regs *r = relevo_get_regs(cpu);
r->pc = entry;
r->a[7] = stack;                        /* with a 0 return address pushed on it */
relevo_status st = relevo_run(cpu, 1000000);
```

With `RELEVO_OPT_EXIT_ON_PC_ZERO`, `relevo_run()` returns `RELEVO_EXIT_PC_ZERO` as soon as the PC reaches 0, so
the host can call 68k code by pushing 0 as its return address (as Emu68 does). The host can also take line A and
line F opcodes before they become exceptions (`cfg.host.line_a`, `cfg.host.line_f`).

## License

Mozilla Public License 2.0. See `LICENSE`.

## Clean room

RelevoJIT is written from the Motorola manuals and tests. GPL emulators (UAE and its descendants) are not read.
See `docs/DISEÑO.md` (Spanish).
