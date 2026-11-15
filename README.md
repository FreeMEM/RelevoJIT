# RelevoJIT

A Motorola 680x0 interpreter and x86-64 JIT, as a Rust crate with a C interface.

It was first written in C and then rewritten in Rust (`docs/REESCRITURA.md`, Spanish). The crate in `src/` exports
the same C interface, and the C tests in `tests/` link against it. The C version was retired once a long random
differential run found both equal; it is in the git history.

RelevoJIT runs 68k code on x86-64 machines. The 68k memory stays big-endian, as on the real CPU; the JIT swaps
bytes on every access (`MOVBE` where available). It knows nothing about any particular computer: the host gives it
memory, registers and callbacks for I/O and traps.

Two engines, one API:

- **Interpreter**: portable (`no_std`, any CPU), accurate, the reference for the JIT.
- **JIT**: translates 68k blocks to x86-64.

Status: the interpreter runs the 68000 to the 68060 (without MMU) and their FPUs; every exception, with the
68000's cycle counts. It passes SingleStepTests/m68000 entirely, cycles included, and the JIT gives the same
results. What comes next is in `docs/REVISION.md` (Spanish): real programs, the MMU, the 68060's own traps.

It is the 68k engine of DashOS on x86, in the same way Emu68 is on ARM.

## Building

```sh
make              # the library (cargo, target/release/librelevo.a) and the C tests linked against it, in build/
make CC=clang     # any C11 compiler for the tests
make test         # ABI check, unit tests, the oracle bench and the JIT against the interpreter
make data         # download SingleStepTests/m68000 (MIT) into tests/datos/
make sst          # run every SingleStepTests file: passed/total per instruction, and cycles
make sst JIT=1    # the same on the JIT engine
make jitdiff      # a long random differential run, JIT against interpreter
make sysdiff      # the same on the machine state: every model and FPU, SR, VBR, frames, host traps
make bench        # speed of the JIT against the interpreter
make oraculo      # regenerate the bench against FS-UAE with the real Kickstart (tools/oraculo)
cargo test        # the crate's own tests (and cargo clippy)
```

A C program links with `target/release/librelevo.a` and what the Rust standard library needs from the system (on
Linux `-lgcc_s -lutil -lrt -lpthread -ldl -lc`; see the Makefile). A Rust program uses the crate directly; without
its `std` feature it is `no_std` with `alloc`, and has no JIT.

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

`cfg.engine = RELEVO_ENGINE_JIT` selects the JIT (x86-64 hosts: Linux and other Unix systems; the Windows code
memory is written but not yet tried; `relevo_create()` returns NULL elsewhere). It gives the same results as the
interpreter: what it does not translate, and every access that is not plain RAM (or ROM, to read), runs in the
interpreter, called from the translated code for host pages. Host functions may call `relevo_invalidate()` and
`relevo_map()`. The FPU's common instructions run from translated code too, with the same software arithmetic. Writes
by the 68k code (or through `relevo_write()`) into code it has translated are caught; when the host changes code by
writing the memory block directly, it calls `relevo_invalidate()` for that range (as a 68040 program flushes its
caches).

With `cfg.fpu` the 68020 and 68030 get a 68881 or 68882 and the 68040 and 68060 their own FPU, in extended
precision computed in software (`src/fpx`), the transcendental functions included: the results are the same on
every host.

Interrupts come from the host with `relevo_set_ipl()`; `relevo_cycles()` gives the 68000 clock cycles spent so
far. Unmapped pages (`RELEVO_PAGE_NONE`) give the CPU a bus error.

## License

Mozilla Public License 2.0. See `LICENSE`.

## Clean room

RelevoJIT is written from the Motorola manuals and tests. GPL emulators (UAE and its descendants) are not read.
See `docs/DISEÑO.md` (Spanish).
