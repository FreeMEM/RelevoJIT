#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The bench against the oracle: 68k programs that run many cases of an
instruction and record the result of each one, for FS-UAE with the real
Kickstart (the oracle), Emu68 (jit-host) and RelevoJIT.

    oraculo.py gen [PROG...]              generate tests/oraculo/PROG.bin
    oraculo.py fsuae --cpu 68020 [PROG...]
                                          run them in FS-UAE, write
                                          tests/oraculo/PROG.68020.txt
    oraculo.py emu68 [PROG...]            run them in Emu68 (jit-host, a
                                          68040), compare with the 68040 oracle;
                                          without PROG, those that take no
                                          exception (jit-host dies on one)
    oraculo.py relevo --cpu 68020 [PROG...]
                                          RelevoJIT's lines (build/bench/), compared
                                          with the oracle
    oraculo.py list                       the programs

Without PROG, every program. `make oraculo` does gen, fsuae for 68020 and
68040 and emu68; `make test` runs build/oraculo, which runs each .bin in
RelevoJIT and compares with the .txt files (no emulator needed).

How a program works
-------------------
Each program ("core") is a flat binary that runs at $140000 in supervisor
mode with interrupts masked. It uses only 68000 instructions plus MOVEC (so it
runs on RelevoJIT before the 68020 addressing modes exist). Fixed addresses,
the same everywhere, so that every value is comparable:

    $140000  code, then the data of the cases
    $15C000  variables
    $15C800  REC: the record being built (132 bytes):
               D0-D7 A0-A7 (64), SR (2), vector (2, $FFFF: none),
               BUF (32: memory the cases use), FRAME (32: the top of the
               last exception frame)
    $15E000  user stack, $15F000 supervisor stack (ISP), $15F800 the
             epilogue's stack
    $160000  the records, one after another; $15C008 points past the last

Every exception goes through the core's own vector table (VBR) to a handler
that copies 32 bytes of the frame to FRAME and goes on at the end of the
case, with the frame still on the stack. A case:

    lea STACK,a7 / lea data,a0 / jsr prologue   BUF, D0-D7, A0-A6
    A7 and SR from the data (in the order that keeps A7 right)
    <the code under test>
  after:
    move.w sr,REC_SR / move.l a7,REC_A7 / lea STACK2,a7 / jsr epilogue

Cases that end in user mode have an ILLEGAL before `after`: its exception
brings the CPU back to supervisor mode.

In FS-UAE the core runs inside tools/oraculo/amiga.s, which takes the memory
with AllocAbs, calls the core through Supervisor() and prints the records as
hex. jit-host (Emu68) loads it at $140000 and dumps $140000-$16FFFF. The
RelevoJIT runner is tests/oraculo.c.

The text files have one line per case: the index, the registers, SR, the
vector, BUF and FRAME in hex, and after " ; " what the case runs. A '?' in
the oracle is a digit not compared (undefined on the real CPU).
"""

import argparse
import os
import random
import resource
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
TESTS = ROOT / "tests" / "oraculo"
BUILD = ROOT / "build" / "bench"
VASM = os.environ.get("VASM", "/opt/amiga/bin/vasmm68k_mot")
EMU_RUN = Path(os.environ.get("DASH_EMU_RUN",
                              Path.home() / "Projects/FreeMEMLang/tools/emu_run.py"))
JIT_HOST = Path(os.environ.get("JIT_HOST",
                               Path.home() / "Projects/dashos/jit-host/build/jit-host"))

CORE = 0x140000
VARS = 0x15C000
RESPTR = VARS + 0x08
REC = VARS + 0x800
REC_A7 = REC + 60
REC_SR = REC + 64
REC_VEC = REC + 66
BUF = REC + 68
FRAME = REC + 100
REC_SIZE = 132
USTACK = 0x15E000
MSTACK = 0x15E800
STACK = 0x15F000
STACK2 = 0x15F800
RESULTS = 0x160000
MAX_CASES = 0x10000 // REC_SIZE

MODELS = ("68020", "68040")

# ---- Values ---------------------------------------------------------------

EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
        0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80, 0xFFFF8000, 0x8001, 0x81]


def value(rng):
    """A 32-bit value: an edge value half the time."""
    if rng.random() < 0.5:
        v = rng.choice(EDGE)
        if rng.random() < 0.3:
            v ^= rng.getrandbits(32) & rng.choice([0xFF, 0xFF00, 0xFFFF0000])
        return v & 0xFFFFFFFF
    return rng.getrandbits(32)


SIZES = {"b": 1, "w": 2, "l": 4}


class Case:
    """One case: the initial state, the code and what not to compare."""

    def __init__(self, rng, code, desc=None):
        self.d = [value(rng) for _ in range(8)]
        self.a = [value(rng) & ~1 for _ in range(7)]
        self.a7 = STACK
        self.sr = 0x2700 | rng.getrandbits(5)
        self.buf = bytes(rng.getrandbits(8) for _ in range(32))
        self.code = code if isinstance(code, list) else [code]
        self.desc = desc or " / ".join(l.split(";")[-1].strip() if l.startswith("dc.w") else l
                                       for l in self.code)
        self.user = False       # ends in user mode: an ILLEGAL brings it back
        self.ignore = {}        # model or "all" -> [(field, lo, hi) or ("sr", mask)]

    def skip(self, what, models=("all",)):
        for m in models:
            self.ignore.setdefault(m, []).append(what)


def pointer(rng, size, mode, disp=0):
    """A value for An such that mode's access of `size` bytes falls in BUF."""
    off = rng.randrange(0, 33 - size)
    if mode == "-(An)":
        return BUF + off + size
    return (BUF + off - disp) & 0xFFFFFFFF


def ea_operand(rng, case, mode, size, avoid=()):
    """Assembler text for an operand in `mode`, setting up its registers in
    `case`. Returns (text, register used as pointer or None)."""
    regs = [r for r in range(7) if r not in avoid]
    n = rng.choice(regs)
    if mode == "Dn":
        return f"d{rng.randrange(8)}", None
    if mode == "An":
        return f"a{n}", None
    if mode == "#":
        return f"#${value(rng) & ((1 << (8 * size)) - 1):x}", None
    if mode == "(An)":
        case.a[n] = pointer(rng, size, mode)
        return f"(a{n})", n
    if mode == "(An)+":
        case.a[n] = pointer(rng, size, mode)
        return f"(a{n})+", n
    if mode == "-(An)":
        case.a[n] = pointer(rng, size, mode)
        return f"-(a{n})", n
    if mode == "d16(An)":
        disp = rng.randrange(-300, 300)
        case.a[n] = pointer(rng, size, mode, disp)
        return f"{disp}(a{n})", n
    if mode == "d8(An,Xn)":
        x = rng.randrange(8)
        disp = rng.randrange(-128, 128)
        xv = rng.randrange(-200, 200)
        long_index = rng.random() < 0.5
        case.d[x] = (xv & 0xFFFFFFFF) if long_index else \
            ((case.d[x] & 0xFFFF0000) | (xv & 0xFFFF))
        case.a[n] = pointer(rng, size, "(An)", disp + xv)
        return f"{disp}(a{n},d{x}.{'l' if long_index else 'w'})", n
    if mode == "abs":
        return f"${BUF + rng.randrange(0, 33 - size):x}.l", None
    raise ValueError(mode)


MEM_MODES = ["(An)", "(An)+", "-(An)", "d16(An)", "d8(An,Xn)", "abs"]

# ---- Programs ---------------------------------------------------------------
#
# Every case keeps the interrupt mask at 7, also in user mode: in FS-UAE the
# Amiga's interrupts would come in.


def prog_arith(rng):
    """ADD, SUB, CMP, AND, OR, EOR and their forms, NEG, NOT, ADDX/SUBX."""
    cases = []
    for _ in range(250):
        op = rng.choice(["add", "sub", "cmp", "and", "or", "eor", "adda", "suba", "cmpa",
                         "addq", "subq", "addi", "subi", "cmpi", "andi", "ori", "eori",
                         "addx", "subx", "neg", "negx", "not"])
        sz = rng.choice("bwl")
        size = SIZES[sz]
        case = Case(rng, [])
        if op in ("adda", "suba", "cmpa"):
            sz = rng.choice("wl")
            src, _ = ea_operand(rng, case, rng.choice(["Dn", "An", "#"] + MEM_MODES), SIZES[sz])
            text = f"{op}.{sz} {src},a{rng.randrange(7)}"
        elif op in ("addq", "subq"):
            mode = rng.choice(["Dn", "An"] + MEM_MODES[:5])
            if mode == "An" and sz == "b":
                sz = "w"
            dst, _ = ea_operand(rng, case, mode, SIZES[sz])
            text = f"{op}.{sz} #{rng.randrange(1, 9)},{dst}"
        elif op in ("addi", "subi", "cmpi", "andi", "ori", "eori"):
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), size)
            text = f"{op}.{sz} #${value(rng) & ((1 << (8 * size)) - 1):x},{dst}"
        elif op in ("addx", "subx"):
            if rng.random() < 0.5:
                text = f"{op}.{sz} d{rng.randrange(8)},d{rng.randrange(8)}"
            else:
                x, y = rng.sample(range(7), 2)
                case.a[x] = pointer(rng, size, "-(An)")
                case.a[y] = pointer(rng, size, "-(An)")
                text = f"{op}.{sz} -(a{x}),-(a{y})"
        elif op in ("neg", "negx", "not"):
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), size)
            text = f"{op}.{sz} {dst}"
        elif op == "eor":
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), size)
            text = f"eor.{sz} d{rng.randrange(8)},{dst}"
        elif op == "cmp" or rng.random() < 0.5:
            modes = ["Dn", "#"] + MEM_MODES + (["An"] if sz != "b" and op in ("add", "sub", "cmp") else [])
            src, _ = ea_operand(rng, case, rng.choice(modes), size)
            text = f"{op}.{sz} {src},d{rng.randrange(8)}"
        else:
            dst, _ = ea_operand(rng, case, rng.choice(MEM_MODES), size)
            text = f"{op}.{sz} d{rng.randrange(8)},{dst}"
        case.code = [text]
        case.desc = text
        cases.append(case)
    return cases


def prog_move(rng):
    """MOVE, MOVEA, MOVEQ, CLR, TST, EXT, SWAP, EXG, LEA, PEA, LINK/UNLK."""
    cases = []
    for _ in range(250):
        op = rng.choice(["move", "move", "move", "movea", "moveq", "clr", "tst", "ext",
                         "swap", "exg", "lea", "pea", "link", "movem"])
        sz = rng.choice("bwl")
        size = SIZES[sz]
        case = Case(rng, [])
        if op == "move":
            src, p = ea_operand(rng, case, rng.choice(["Dn", "#"] + MEM_MODES + (["An"] if sz != "b" else [])),
                                size)
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), size,
                                avoid=() if p is None else (p,))
            text = f"move.{sz} {src},{dst}"
        elif op == "movea":
            sz = rng.choice("wl")
            src, _ = ea_operand(rng, case, rng.choice(["Dn", "An", "#"] + MEM_MODES), SIZES[sz])
            text = f"movea.{sz} {src},a{rng.randrange(7)}"
        elif op == "moveq":
            text = f"moveq #{rng.randrange(-128, 128)},d{rng.randrange(8)}"
        elif op in ("clr", "tst"):
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), size)
            text = f"{op}.{sz} {dst}"
        elif op == "ext":
            text = f"ext.{rng.choice('wl')} d{rng.randrange(8)}"
        elif op == "swap":
            text = f"swap d{rng.randrange(8)}"
        elif op == "exg":
            text = f"exg {rng.choice('da')}{rng.randrange(7)},{rng.choice('da')}{rng.randrange(7)}"
        elif op == "lea":
            src, _ = ea_operand(rng, case, rng.choice(["(An)", "d16(An)", "d8(An,Xn)", "abs"]), 4)
            text = f"lea {src},a{rng.randrange(7)}"
        elif op == "pea":
            src, _ = ea_operand(rng, case, rng.choice(["(An)", "d16(An)", "d8(An,Xn)", "abs"]), 4)
            case.a7 = BUF + 4 * rng.randrange(1, 9)
            text = f"pea {src}"
        elif op == "link":
            n = rng.randrange(7)
            case.a7 = BUF + 4 * rng.randrange(1, 9)
            text = [f"link a{n},#{rng.choice([0, -4, -8, 8, 12, -100, 100])}"]
            if rng.random() < 0.5:
                text.append(f"unlk a{n}")
            case.code = text
            case.desc = " / ".join(text)
            cases.append(case)
            continue
        else:   # movem
            sz = rng.choice("wl")
            regs = sorted(rng.sample(range(15), rng.randrange(1, 6)))
            rl = "/".join(f"{'da'[r // 8]}{r % 8}" for r in regs)
            n = rng.choice([r for r in range(7) if 8 + r not in regs])
            span = len(regs) * SIZES[sz]
            if rng.random() < 0.5:
                case.a[n] = BUF + rng.randrange(0, 33 - span)
                text = f"movem.{sz} {rl},(a{n})" if rng.random() < 0.5 else f"movem.{sz} (a{n}),{rl}"
            else:
                case.a[n] = BUF + span + rng.randrange(0, 33 - span)
                text = f"movem.{sz} {rl},-(a{n})"
        case.code = [text]
        case.desc = text
        cases.append(case)
    return cases


CONDS = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]


def prog_branch(rng):
    """Bcc (.S .W .L), BRA, BSR, DBcc, Scc."""
    cases = []
    k = 0
    for cc in CONDS:
        for _ in range(4):
            k += 1
            for sz in "swl":
                if cc in ("t", "f"):
                    continue
                case = Case(rng, [])
                case.code = [f"b{cc}.{sz} .t{k}{sz}", "addq.w #1,a6", f".t{k}{sz}:"]
                case.desc = f"b{cc}.{sz}"
                cases.append(case)
    for sz in "swl":
        for op in ("bra", "bsr"):
            case = Case(rng, [])
            case.a7 = BUF + 16
            case.code = [f"{op}.{sz} .t{k}{sz}{op}", "addq.w #1,a6", f".t{k}{sz}{op}:"]
            case.desc = f"{op}.{sz}"
            cases.append(case)
    for cc in CONDS:
        for _ in range(4):
            k += 1
            case = Case(rng, [])
            n = rng.randrange(6)
            case.d[n] = rng.choice([0, 1, 2, 0xFFFF, 0x10000, 0x12340000, value(rng)])
            case.code = [f"db{cc} d{n},.t{k}", "addq.w #1,a6", f".t{k}:"]
            case.desc = f"db{cc}"
            cases.append(case)
    for cc in CONDS:
        for _ in range(3):
            case = Case(rng, [])
            dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), 1)
            case.code = [f"s{cc} {dst}"]
            case.desc = f"s{cc} {dst}"
            cases.append(case)
    return cases


# MOVEC control register codes (MC68020UM, MC68040UM)
CREGS = {"sfc": 0x000, "dfc": 0x001, "cacr": 0x002, "tc": 0x003, "itt0": 0x004, "itt1": 0x005,
         "dtt0": 0x006, "dtt1": 0x007, "usp": 0x800, "vbr": 0x801, "caar": 0x802, "msp": 0x803,
         "isp": 0x804, "mmusr": 0x805, "urp": 0x806, "srp": 0x807}


def movec(to_creg, creg, rn):
    """MOVEC as words (the assembler may not know every register for the
    chosen CPU). rn: 0-7 data, 8-15 address."""
    code = CREGS[creg] if isinstance(creg, str) else creg
    name = creg if isinstance(creg, str) else f"${creg:03x}"
    r = f"{'da'[rn // 8]}{rn % 8}"
    text = f"movec {r},{name}" if to_creg else f"movec {name},{r}"
    return f"dc.w ${0x4E7B if to_creg else 0x4E7A:04x},${rn << 12 | code:04x} ; {text}"


def prog_m68010(rng):
    """What the 68010 added: MOVEC, MOVES, RTD, MOVE from CCR, MOVE from SR
    (privileged), and MOVEC/MOVES/MOVE from SR in user mode."""
    cases = []
    # MOVEC: write, read back
    for creg in ["sfc", "dfc", "usp", "cacr", "caar", "msp", "tc", "itt0", "itt1",
                 "dtt0", "dtt1", "mmusr", "urp", "srp"]:
        for v in [0, 0xFFFFFFFF, None, None, None]:
            case = Case(rng, [])
            v = value(rng) if v is None else v
            if creg == "tc":
                v &= 0x7FFF           # never turn on the MMU
            if creg in ("itt0", "itt1", "dtt0", "dtt1"):
                v &= ~0x8000          # nor the transparent translation
            if creg == "cacr":
                v &= 0x7FFF7FFF if rng.random() < 0.5 else 0xFFFFFFFF
            case.d[1] = v
            case.code = [movec(True, creg, 1), movec(False, creg, 2)]
            case.desc = f"movec {creg} ${v:08x}"
            cases.append(case)
    # VBR: set and read back, then our table again
    for v in [0, 0x1000, 0x12345678, 0xFFFFFFFF]:
        case = Case(rng, [])
        case.d[1] = v
        case.code = [movec(False, "vbr", 3), movec(True, "vbr", 1), movec(False, "vbr", 2),
                     movec(True, "vbr", 3)]
        case.desc = f"movec vbr ${v:08x}"
        cases.append(case)
    # ISP and MSP with M clear: ISP is A7
    for creg in ["isp", "msp"]:
        case = Case(rng, [])
        case.d[1] = BUF + 8
        case.code = [movec(False, creg, 4), movec(True, creg, 1), movec(False, creg, 2),
                     movec(False, "isp", 3)]
        case.desc = f"movec {creg} (A7)"
        cases.append(case)
    # MOVEC from/to A registers
    case = Case(rng, [])
    case.a[3] = 0x89ABCDEF
    case.code = [movec(True, "usp", 11), movec(False, "usp", 12)]
    case.desc = "movec a3,usp / movec usp,a4"
    cases.append(case)
    # registers that do not exist (illegal), and MOVEC in user mode
    for code in [0x008, 0x00F, 0x808, 0x80F, 0x100, 0xFFF]:
        for direction in (False, True):
            case = Case(rng, [])
            case.code = [movec(direction, code, 1)]
            case.desc = f"movec {'to' if direction else 'from'} ${code:03x}"
            cases.append(case)
    for creg in ["vbr", "sfc", "usp"]:
        case = Case(rng, [])
        case.sr = 0x0700 | rng.getrandbits(5)
        case.a7 = USTACK - 16
        case.user = True
        case.code = [movec(False, creg, 1)]
        case.desc = f"movec {creg},d1 (user)"
        cases.append(case)
    # MOVES
    for _ in range(40):
        case = Case(rng, [])
        sz = rng.choice("bwl")
        size = SIZES[sz]
        mode = rng.choice(MEM_MODES)
        rn = rng.randrange(16)
        if rn >= 8:
            avoid = (rn - 8,)
        else:
            avoid = ()
        ea, _ = ea_operand(rng, case, mode, size, avoid=avoid)
        r = f"{'da'[rn // 8]}{rn % 8}"
        fc = rng.choice([1, 2, 5, 6])
        case.d[7] = fc
        pre = [movec(True, "sfc", 7), movec(True, "dfc", 7)]
        if rng.random() < 0.5:
            text = f"moves.{sz} {ea},{r}"
        else:
            text = f"moves.{sz} {r},{ea}"
        case.code = pre + [text]
        case.desc = text
        cases.append(case)
    for text in ["moves.l (a0),d1", "moves.w d1,(a0)"]:
        case = Case(rng, [])
        case.a[0] = BUF + 4
        case.sr = 0x0700 | rng.getrandbits(5)
        case.a7 = USTACK - 16
        case.user = True
        case.code = [text]
        case.desc = text + " (user)"
        cases.append(case)
    # RTD
    for disp in [0, 4, 8, -4, -8, 12, 2, 0x7FFE, -0x8000]:
        case = Case(rng, [])
        case.a7 = BUF + 16
        k = len(cases)
        case.code = [f"bsr.w .s{k}", f"bra.w .e{k}", f".s{k}:", f"rtd #{disp}", f".e{k}:"]
        case.desc = f"rtd #{disp}"
        cases.append(case)
    # MOVE from CCR, MOVE from SR
    for _ in range(24):
        case = Case(rng, [])
        dst, _ = ea_operand(rng, case, rng.choice(["Dn"] + MEM_MODES), 2)
        case.sr = 0x2700 | rng.getrandbits(5)
        op = rng.choice(["move.w ccr", "move.w sr"])
        case.code = [f"{op},{dst}"]
        case.desc = case.code[0]
        cases.append(case)
    for op in ["move.w ccr,d1", "move.w sr,d1", "move.w ccr,(a0)", "move.w sr,(a0)"]:
        for _ in range(2):
            case = Case(rng, [])
            case.a[0] = BUF + 2
            case.sr = 0x0700 | rng.getrandbits(5)
            case.a7 = USTACK - 16
            case.user = True
            case.code = [op]
            case.desc = op + " (user)"
            cases.append(case)
    return cases


def frame_case(rng, code, desc, user=False, a7=None):
    case = Case(rng, code, desc)
    case.user = user
    if user:
        case.sr = 0x0700 | rng.getrandbits(5)
        case.a7 = USTACK - 16
    elif a7 is not None:
        case.a7 = a7
    return case


def prog_frames(rng):
    """Exceptions and their frames; RTE with every frame format."""
    cases = []
    for n in range(16):
        cases.append(frame_case(rng, [f"trap #{n}"], f"trap #{n}"))
    cases.append(frame_case(rng, [f"trap #3"], "trap #3 (user)", user=True))
    cases.append(frame_case(rng, ["illegal"], "illegal"))
    cases.append(frame_case(rng, ["dc.w $a123"], "line a"))
    cases.append(frame_case(rng, ["dc.w $fe00"], "line f $fe00"))
    cases.append(frame_case(rng, ["dc.w $fc00"], "line f $fc00"))
    cases.append(frame_case(rng, ["dc.w $a000"], "line a (user)", user=True))
    for text in ["move.w #$2000,sr", "stop #$2700", "reset", "rte", "move.l usp,a0",
                 "ori.w #$700,sr", "andi.w #$f8ff,sr", "eori.w #$0001,sr"]:
        cases.append(frame_case(rng, [text], text + " (user)", user=True))
    # format 2 on the 68020 and later
    for _ in range(4):
        case = frame_case(rng, ["chk.w d1,d0"], "chk.w")
        case.d[0] = rng.choice([0xFFFF, 0x8000, 0x0100])
        case.d[1] = 0x00FF
        cases.append(case)
        case = frame_case(rng, ["chk.w d1,d0"], "chk.w (user)", user=True)
        case.d[0] = 0xFFFF
        cases.append(case)
    for _ in range(3):
        case = frame_case(rng, ["trapv"], "trapv")
        case.sr |= 2
        cases.append(case)
    for op in ["divu.w d1,d0", "divs.w d1,d0", "divu.w #0,d2"]:
        case = frame_case(rng, [op], op + " by zero")
        case.d[1] = 0
        cases.append(case)
    case = frame_case(rng, ["divu.w (a0),d0"], "divu.w (a0) by zero")
    case.a[0] = BUF
    case.buf = bytes(2) + case.buf[2:]
    cases.append(case)
    # trace
    for code in [["nop"], ["moveq #5,d0"], ["add.l d1,d2"], ["bra.s .tr0", "nop", ".tr0:"],
                 ["move.w (a0),d3"]]:
        case = frame_case(rng, code, "trace " + " / ".join(c for c in code if not c.startswith(".")))
        case.sr |= 0x8000
        case.a[0] = BUF
        case.code = [c.replace(".tr0", f".tr{len(cases)}") for c in code]
        cases.append(case)
    case = frame_case(rng, ["nop"], "trace nop (user)", user=True)
    case.sr |= 0x8000
    cases.append(case)
    # address error: a jump to an odd address
    for code in ["jmp (a0)", "jsr (a0)", "bra.w *+$101"]:
        case = frame_case(rng, [code], code + " odd")
        case.a[0] = BUF + 5
        case.skip(("f", 8, 15), ("68020",))
        case.skip(("f", 20, 31), ("68020",))
        cases.append(case)
    case = frame_case(rng, ["jmp (a0)"], "jmp (a0) odd (user)", user=True)
    case.a[0] = BUF + 9
    case.skip(("f", 8, 15), ("68020",))
    case.skip(("f", 20, 31), ("68020",))
    cases.append(case)
    # RTE with each format, built by hand on the supervisor stack; it returns
    # to .r<k>, and then on to the end of the case
    # formats where FS-UAE does not do what the manuals say (it takes $4, $7,
    # $8, $9, $A, $B on both models and not $3 on the 68040)
    rte_differs = {"68020": (4, 7, 8), "68040": (3, 8, 9, 0xA, 0xB)}

    def rte(fmt, words, sr=0x2700, extra=None, desc=None, user=False):
        k = len(cases)
        sr |= rng.getrandbits(5)
        code = []
        for w in reversed(words):
            code.append(f"move.w #${w & 0xFFFF:x},-(a7)")
        code += [f"move.w #${fmt << 12 | 0x10:x},-(a7)", f"move.l #.r{k},-(a7)",
                 f"move.w #${sr:x},-(a7)", "rte", "nop", f".r{k}:"] + (extra or [])
        case = frame_case(rng, code, desc or f"rte format ${fmt:x}")
        if user:
            case.user = True
        for model, fmts in rte_differs.items():
            if fmt in fmts:
                case.skip(("x",), (model,))
                case.desc += f" [FS-UAE {model}: not as the manual]"
        cases.append(case)
    rte(0, [])
    rte(0, [], sr=0x0700, desc="rte format $0 to user", user=True)
    rte(2, [0x1234, 0x5678])
    rte(3, [0x1234, 0x5678])
    rte(4, [0x1234, 0x5678, 0x9ABC, 0xDEF0])
    rte(7, [0] * 26)
    rte(8, [0] * 25)
    rte(9, [0x1234, 0x5678, 0, 0, 0, 0])
    rte(0xA, [0] * 12)
    rte(0xB, [0] * 42)
    for fmt in [5, 6, 0xC, 0xD, 0xE, 0xF]:
        rte(fmt, [0] * 4)
    # format 1: a throwaway frame on the ISP whose SR sets M; RTE goes on with
    # the frame on the MSP
    k = len(cases)
    code = [f"lea ${MSTACK:x},a5", "move.w #$0010,-(a5)", f"move.l #.r{k},-(a5)",
            "move.w #$2704,-(a5)", movec(True, "msp", 13),
            "move.w #$1010,-(a7)", "move.l #$12345678,-(a7)", "move.w #$3700,-(a7)",
            "rte", "nop", f".r{k}:", movec(False, "msp", 12)]
    cases.append(frame_case(rng, code, "rte format $1 then $0 on the MSP"))
    return cases


def bounds(rng, size):
    """A lower and an upper bound of `size` bytes for CMP2/CHK2."""
    m = (1 << (8 * size)) - 1
    lo, hi = value(rng) & m, value(rng) & m
    if rng.random() < 0.6:
        lo, hi = min(lo, hi), max(lo, hi)
    return lo, hi


def prog_m68020(rng):
    """The 68020 instructions of R3b, with the flags the manuals leave
    undefined: CHK.W/CHK.L, CMP2/CHK2, DIVx.W/DIVx.L (overflow, by zero),
    MULx.L, EXTB, CAS/CAS2, PACK/UNPK, TRAPcc, bit fields, the new
    addressing modes."""
    cases = []

    def add(code, desc=None, setup=None):
        case = Case(rng, code if isinstance(code, list) else [code], desc)
        if setup:
            setup(case)
        cases.append(case)
        return case

    small = lambda: rng.choice([0, 1, 2, 5, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, value(rng)])
    for sz in "wl":
        for _ in range(20):
            def st(c):
                c.d[0] = small() if rng.random() < 0.5 else value(rng)
                c.d[1] = small() if rng.random() < 0.5 else value(rng)
            add(f"chk.{sz} d1,d0", setup=st)
    for sz in "bwl":
        size = SIZES[sz]
        for op in ("cmp2", "chk2"):
            for _ in range(10):
                def st(c, size=size, sz=sz):
                    lo, hi = bounds(rng, size)
                    b = bytearray(c.buf)
                    b[0:2 * size] = lo.to_bytes(size, "big") + hi.to_bytes(size, "big")
                    c.buf = bytes(b)
                    c.a[0] = BUF
                    r = rng.choice([lo, hi, lo - 1, hi + 1, value(rng), (lo + hi) // 2])
                    c.d[1] = r & 0xFFFFFFFF
                    c.a[2] = value(rng) if rng.random() < 0.5 else sext32(r, size)
                    # FS-UAE compares signed; the manuals (and RelevoJIT) take
                    # bounds that are in order either signed or unsigned
                    on_a = "a2" in c.code[0]
                    s = 4 if on_a else size
                    lo_, hi_ = (sext32(lo, size), sext32(hi, size)) if on_a else (lo, hi)
                    rv = (c.a[2] if on_a else c.d[1]) & ((1 << (8 * s)) - 1)
                    m = (1 << (8 * s)) - 1
                    inside_manual = ((rv - lo_) & m) <= ((hi_ - lo_) & m)
                    sx = lambda v: v - (1 << (8 * s)) if v >> (8 * s - 1) else v
                    inside_signed = sx(lo_) <= sx(rv) <= sx(hi_)
                    if inside_manual != inside_signed:
                        c.skip(("x",))
                        c.desc += " [FS-UAE: signed compare]"
                    if c.code[0].startswith("chk2"):
                        # FS-UAE stacks the CHK2's own address as the PC
                        c.skip(("f", 2, 5))
                        c.skip(("f", 8, 11), ("68040",))
                reg = rng.choice(["d1", "a2"])
                add(f"{op}.{sz} (a0),{reg}", setup=st)
    for op in ("divu", "divs"):
        for _ in range(12):
            def st(c):
                c.d[0] = value(rng)
                c.d[1] = rng.choice([0, 1, 2, 0xFFFF, 0x8000, value(rng) & 0xFFFF, value(rng)])
            add(f"{op}.w d1,d0", setup=st)
        for form in ("d1,d2", "d1,d3:d2", "d1,d2:d2"):
            for _ in range(10):
                def st(c, op=op, form=form):
                    c.d[1] = rng.choice([0, 1, 2, 0xFFFFFFFF, 0x80000000, 0x10000, value(rng)])
                    c.d[2] = value(rng)
                    c.d[3] = rng.choice([0, 0xFFFFFFFF, 1, value(rng), value(rng) & 0xFF])
                    divl_skips(c, op == "divs", form)
                add(f"{op}.l {form}", setup=st)
        for _ in range(8):
            def st(c, op=op):
                c.d[1] = rng.choice([0, 1, 7, 0xFFFFFFFF, value(rng)])
                c.d[2] = value(rng)
                c.d[3] = value(rng)
                divl_skips(c, op == "divs", "d1,d2")
            add(f"{op}l.l d1,d3:d2", setup=st)
    for op in ("mulu", "muls"):
        for form in ("d1,d2", "d1,d3:d2", "d1,d2:d2"):
            for _ in range(6):
                add(f"{op}.l {form}")
    for _ in range(6):
        add(f"extb.l d{rng.randrange(8)}")
    for sz in "bwl":
        for _ in range(8):
            def st(c, sz=sz):
                c.a[0] = BUF + rng.randrange(0, 29)
                if rng.random() < 0.5:   # equal: the update happens
                    mem = int.from_bytes(c.buf[c.a[0] - BUF:c.a[0] - BUF + SIZES[sz]], "big")
                    c.d[1] = (c.d[1] & ~((1 << (8 * SIZES[sz])) - 1) | mem) & 0xFFFFFFFF
            add(f"cas.{sz} d1,d2,(a0)", setup=st)
    for sz in "wl":
        for _ in range(6):
            def st(c, sz=sz):
                n = SIZES[sz]
                c.a[0] = BUF + rng.randrange(0, 33 - n)
                c.a[1] = BUF + rng.randrange(0, 33 - n)
                for reg, ptr in ((1, c.a[0]), (2, c.a[1])):
                    if rng.random() < 0.6:
                        mem = int.from_bytes(c.buf[ptr - BUF:ptr - BUF + n], "big")
                        c.d[reg] = (c.d[reg] & ~((1 << (8 * n)) - 1) | mem) & 0xFFFFFFFF
            case = add(f"cas2.{sz} d1:d2,d3:d4,(a0):(a1)", setup=st)
            if sz == "w":   # FS-UAE loads the high word of Du into Dc too
                case.skip(("d", 1, "????...."))
                case.skip(("d", 2, "????...."))
    for _ in range(5):
        add(f"pack d1,d2,#${rng.getrandbits(16):x}")
        add(f"unpk d1,d2,#${rng.getrandbits(16):x}")
    for _ in range(3):
        def st(c):
            c.a[0] = BUF + 8
            c.a[1] = BUF + 20
        add(f"pack -(a0),-(a1),#${rng.getrandbits(16):x}", setup=st)
        add(f"unpk -(a0),-(a1),#${rng.getrandbits(16):x}", setup=st)
    for cc in CONDS:
        for text in (f"trap{cc}", f"trap{cc}.w #$1234", f"trap{cc}.l #$12345678"):
            case = add(text)
            # FS-UAE stacks the TRAPcc's own address as the PC; the manuals,
            # the next instruction's
            case.skip(("f", 2, 5))
            case.skip(("f", 8, 11), ("68040",))
    # bit fields
    for op in ("bftst", "bfextu", "bfexts", "bfchg", "bfclr", "bfset", "bfffo", "bfins"):
        for _ in range(10):
            off, width = rng.randrange(32), rng.randrange(1, 33)
            fld = rng.choice([f"{{{off}:{width % 32}}}", "{d3:d4}"])
            def st(c):
                c.a[0] = BUF + 8
                c.d[3] = rng.choice([rng.randrange(32), rng.randrange(-40, 80) & 0xFFFFFFFF])
                c.d[4] = rng.randrange(0, 40)
            dst = rng.choice(["d1", "(a0)"])
            if op in ("bftst", "bfchg", "bfclr", "bfset"):
                text = f"{op} {dst}{fld}"
            elif op == "bfins":
                text = f"bfins d2,{dst}{fld}"
            else:
                text = f"{op} {dst}{fld},d2"
            add(text, setup=st)
    # addressing modes: LEA (only the address) and MOVE through them
    for _ in range(30):
        n, x = rng.randrange(7), rng.randrange(8)
        scale = rng.choice([1, 2, 4, 8])
        xs = rng.choice(["w", "l"])
        bd = rng.choice([0, rng.randrange(-128, 128), rng.randrange(-0x8000, 0x8000),
                         rng.getrandbits(32)])
        mode = rng.choice([f"({bd},a{n},d{x}.{xs}*{scale})", f"({bd},a{n})",
                           f"({bd},d{x}.{xs}*{scale})", f"({bd}.l,za{n},d{x}.{xs}*{scale})"])
        add(f"lea {mode},a{rng.randrange(7)}")
    for _ in range(40):
        def st(c):
            c.a[0] = BUF + 4
            c.d[1] = rng.randrange(0, 4)
            b = bytearray(c.buf)
            b[0:4] = (BUF + 8).to_bytes(4, "big")    # a pointer in BUF[0]
            c.buf = bytes(b)
        scale = rng.choice([1, 2, 4])
        od = rng.randrange(0, 8)
        mode = rng.choice([f"(-4,a0,d1.l*{scale})", f"([-4,a0],d1.l*{scale},{od})",
                           f"([-4,a0],{od})", "([-4,a0])",
                           f"([-4.l,a0],d1.w*{scale},{od}.l)"])
        sz = rng.choice("bwl")
        if rng.random() < 0.5:
            text = f"move.{sz} {mode},d2"
        else:
            text = f"move.{sz} d2,{mode}"
        add(text, setup=st)
    return cases


def divl_skips(c, signed, form):
    """DIVx.L cases where FS-UAE and RelevoJIT part: on overflow FS-UAE sets
    Z by a rule not found yet; a signed 64-bit division whose quotient does
    not fit in 32 bits gives no overflow and a quotient of 0 there."""
    s = c.d[1]
    if s == 0:
        return
    signed32 = lambda v: v - (1 << 32) if v >> 31 else v
    if form == "d1,d2":
        n = signed32(c.d[2]) if signed else c.d[2]
    else:
        hi = c.d[2] if form == "d1,d2:d2" else c.d[3]
        n = hi << 32 | c.d[2]
        if signed and n >> 63:
            n -= 1 << 64
    d = signed32(s) if signed else s
    if signed:
        q = abs(n) // abs(d)
        q = -q if (n < 0) != (d < 0) else q
        over = not -0x80000000 <= q <= 0x7FFFFFFF
        if form != "d1,d2" and abs(q) >= 1 << 32:
            c.skip(("x",))
            c.desc += " [FS-UAE: no overflow, quotient 0]"
            return
    else:
        over = n // d > 0xFFFFFFFF
    if over:
        c.skip(("sr", 0xF))
        c.desc += " [FS-UAE: Z]"


def sext32(v, size):
    m = 1 << (8 * size - 1)
    v &= (1 << (8 * size)) - 1
    return ((v ^ m) - m) & 0xFFFFFFFF


# The programs Emu68 can run in jit-host: under qemu-user, its exceptions
# and its writes to SR stop the process.
EMU68_PROGRAMS = ["arith", "move", "branch"]

PROGRAMS = {
    "arith": prog_arith,
    "move": prog_move,
    "branch": prog_branch,
    "m68010": prog_m68010,
    "frames": prog_frames,
    "m68020": prog_m68020,
}

# ---- Generating ---------------------------------------------------------------

CORE_HEAD = f"""\
; Generated by tools/oraculo/oraculo.py: do not edit.
    org ${CORE:x}
VARS        equ ${VARS:x}
SAVED_SP    equ VARS+$00
SAVED_VBR   equ VARS+$04
RESPTR      equ VARS+$08
CONT        equ VARS+$0C
SAVED_USP   equ VARS+$10
SAVED_CACR  equ VARS+$14
SAVED_SFC   equ VARS+$18
SAVED_DFC   equ VARS+$1C
SAVED_MSP   equ VARS+$20
HSAVE       equ VARS+$28
DONE        equ VARS+$3C
REC         equ ${REC:x}
REC_A7      equ ${REC_A7:x}
REC_SR      equ ${REC_SR:x}
REC_VEC     equ ${REC_VEC:x}
BUF         equ ${BUF:x}
FRAME       equ ${FRAME:x}
USTACK      equ ${USTACK:x}
STACK       equ ${STACK:x}
STACK2      equ ${STACK2:x}
RESULTS     equ ${RESULTS:x}

start:
    movem.l d0-d7/a0-a6,-(a7)
    move.l  a7,SAVED_SP
    movec   vbr,d0
    move.l  d0,SAVED_VBR
    move.l  usp,a0
    move.l  a0,SAVED_USP
    movec   cacr,d0
    move.l  d0,SAVED_CACR
    movec   sfc,d0
    move.l  d0,SAVED_SFC
    movec   dfc,d0
    move.l  d0,SAVED_DFC
    movec   msp,d0
    move.l  d0,SAVED_MSP
    lea     vectab,a0
    movec   a0,vbr
    move.l  #RESULTS,RESPTR
    clr.l   DONE
    jmp     cases

; a0 = the case's data. Fills BUF, clears FRAME, loads D0-D7/A0-A6.
prologue:
    move.l  68(a0),CONT
    move.w  #$FFFF,REC_VEC
    lea     72(a0),a1
    lea     BUF,a2
    moveq   #7,d0
.b: move.l  (a1)+,(a2)+
    dbra    d0,.b
    lea     FRAME,a2
    moveq   #7,d0
.f: clr.l   (a2)+
    dbra    d0,.f
    lea     USTACK,a1
    move.l  a1,usp
    movem.l (a0),d0-d7/a0-a6
    rts

; Called with REC_SR and REC_A7 written: the rest of the record, then the
; record to the results.
epilogue:
    movem.l d0-d7/a0-a6,REC
    move.w  sr,d0                   ; back to $2700 (a write to SR only
    and.w   #$FF00,d0               ; if needed: in jit-host, Emu68 dies
    cmp.w   #$2700,d0               ; on one)
    beq.s   .s
    move.w  #$2700,sr
.s:
    movea.l RESPTR,a1
    lea     REC,a0
    moveq   #{REC_SIZE // 4 - 1},d0
.c: move.l  (a0)+,(a1)+
    dbra    d0,.c
    move.l  a1,RESPTR
    rts

; Every vector comes here (through its stub, which writes its number):
; copies the top of the frame and goes on at the end of the case.
handler:
    movem.l d0/a0-a1,HSAVE
    movea.l a7,a0
    lea     FRAME,a1
    moveq   #7,d0
.h: move.l  (a0)+,(a1)+
    dbra    d0,.h
    movem.l HSAVE,d0/a0-a1
    move.l  CONT,-(a7)
    rts

finish:                             ; (SR is $27xx: the epilogue saw to it)
    movea.l SAVED_SP,a7
    move.l  SAVED_MSP,d0
    movec   d0,msp
    move.l  SAVED_DFC,d0
    movec   d0,dfc
    move.l  SAVED_SFC,d0
    movec   d0,sfc
    move.l  SAVED_CACR,d0
    movec   d0,cacr
    movea.l SAVED_USP,a0
    move.l  a0,usp
    move.l  SAVED_VBR,d0
    movec   d0,vbr
    move.l  #$444F4E45,DONE
    movem.l (a7)+,d0-d7/a0-a6
    rts
"""


def fix_labels(cases):
    """Local labels in the case code (".t1:", "bra .t1") get the case's prefix."""
    import re
    for k, c in enumerate(cases):
        c.code = [re.sub(r"(?<![\w$])\.([a-z]+\d*[a-z]*)", lambda m: f"L{k}_{m.group(1)}", line)
                  if not line.startswith("dc.w") else line for line in c.code]


def generate(name):
    rng = random.Random(f"relevo-oraculo-{name}")
    cases = PROGRAMS[name](rng)
    if len(cases) > MAX_CASES:
        sys.exit(f"{name}: {len(cases)} cases, at most {MAX_CASES}")
    fix_labels(cases)
    BUILD.mkdir(parents=True, exist_ok=True)
    TESTS.mkdir(parents=True, exist_ok=True)
    src = BUILD / f"{name}.s"
    src.write_text(core_source(cases))
    binf = TESTS / f"{name}.bin"
    r = subprocess.run([VASM, "-Fbin", "-m68040", "-no-opt", "-quiet", "-nowarn=62",
                        "-o", str(binf), str(src)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"vasm failed on {src}:\n{r.stdout}{r.stderr}")
    if binf.stat().st_size > 0x1C000:
        sys.exit(f"{name}: the program is too big ({binf.stat().st_size} bytes)")
    meta = BUILD / f"{name}.cases"
    with meta.open("w") as f:
        for c in cases:
            f.write(repr((c.desc, c.ignore)) + "\n")
    print(f"{name}: {len(cases)} cases, {binf.stat().st_size} bytes")
    return cases


def core_source(cases):
    out = [CORE_HEAD]
    for v in range(256):
        out.append(f"stub{v}:\n    move.w #{v},REC_VEC\n    bra.w handler")
    out.append("    cnop 0,4\nvectab:")
    for v in range(0, 256, 8):
        out.append("    dc.l " + ",".join(f"stub{i}" for i in range(v, v + 8)))
    out.append("cases:")
    for k, c in enumerate(cases):
        out.append(f"; case {k}: {c.desc}")
        out.append(f"case{k}:")
        out.append("    lea STACK,a7")
        out.append(f"    lea data{k},a0")
        out.append("    jsr prologue")
        if c.sr & 0xFF00 == 0x2700:     # SR is $27xx already
            out.append(f"    movea.l data{k}+60,a7")
            out.append(f"    move.w data{k}+64,ccr")
        elif c.sr & 0x2000:
            out.append(f"    movea.l data{k}+60,a7")
            out.append(f"    move.w data{k}+64,sr")
        else:
            out.append(f"    move.w data{k}+64,sr")
            out.append(f"    movea.l data{k}+60,a7")
        for line in c.code:
            out.append(line if line.endswith(":") else "    " + line)
        if c.user or c.sr & 0x2000 == 0:
            out.append("    illegal")
        out.append(f"after{k}:")
        out.append("    move.w sr,REC_SR")
        out.append("    move.l a7,REC_A7")
        out.append("    lea STACK2,a7")
        out.append("    jsr epilogue")
    out.append("    jmp finish")
    out.append("    cnop 0,4")
    for k, c in enumerate(cases):
        regs = c.d + c.a + [c.a7]
        out.append(f"data{k}:")
        out.append("    dc.l " + ",".join(f"${r & 0xFFFFFFFF:x}" for r in regs))
        out.append(f"    dc.w ${c.sr:x},0")
        out.append(f"    dc.l after{k}")
        out.append("    dc.b " + ",".join(f"${b:x}" for b in c.buf))
    return "\n".join(out) + "\n"


def load_meta(name):
    import ast
    meta = BUILD / f"{name}.cases"
    if not meta.exists():
        generate(name)
    return [ast.literal_eval(l) for l in meta.read_text().splitlines()]


# ---- Records and text ---------------------------------------------------------

def record_line(i, rec, desc, ignore=()):
    d = struct.unpack(">8I", rec[0:32])
    a = struct.unpack(">8I", rec[32:64])
    sr, vec = struct.unpack(">HH", rec[64:68])
    buf, frame = rec[68:100], rec[100:132]
    fields = {
        "d": [f"{x:08x}" for x in d],
        "a": [f"{x:08x}" for x in a],
        "sr": f"{sr:04x}",
        "v": f"{vec:04x}",
        "b": buf.hex(),
        "f": frame.hex() if vec != 0xFFFF else "-",   # (no exception: FRAME is 0)
    }
    if any(what[0] == "x" for what in ignore):
        return f"{i:04d} x ; {desc}"     # not compared at all
    for what in ignore:
        if what[0] in ("b", "f") and fields[what[0]] != "-":
            _, lo, hi = what
            s = list(fields[what[0]])
            for j in range(2 * lo, 2 * hi + 2):
                s[j] = "?"
            fields[what[0]] = "".join(s)
        elif what[0] == "sr":
            mask = what[1]
            s = list(fields["sr"])
            for j in range(4):
                if (mask >> (12 - 4 * j)) & 15:
                    s[j] = "?"
            fields["sr"] = "".join(s)
        elif what[0] in ("d", "a"):
            pat = what[2] if len(what) > 2 else "????????"
            fields[what[0]][what[1]] = "".join("?" if p == "?" else ch
                                               for p, ch in zip(pat, fields[what[0]][what[1]]))
    return (f"{i:04d} d {' '.join(fields['d'])} a {' '.join(fields['a'])} sr {fields['sr']} "
            f"v {fields['v']} b {fields['b']} f {fields['f']} ; {desc}")


def records_to_text(name, data, model, masks=True):
    meta = load_meta(name)
    n = len(data) // REC_SIZE
    lines = []
    for i in range(n):
        desc, ignore = meta[i] if i < len(meta) else ("?", {})
        ig = list(ignore.get("all", [])) + list(ignore.get(model, [])) if masks else []
        lines.append(record_line(i, data[i * REC_SIZE:(i + 1) * REC_SIZE], desc, ig))
    if n != len(meta):
        lines.append(f"# {n} records of {len(meta)} cases")
    return "\n".join(lines) + "\n"


def compare_text(expected, got):
    """Lines that differ, '?' in `expected` matching anything."""
    diffs = []
    e_lines = [l for l in expected.splitlines() if not l.startswith("#")]
    g_lines = [l for l in got.splitlines() if not l.startswith("#")]
    for el, gl in zip(e_lines, g_lines):
        ef, gf = el.split(" ; ")[0], gl.split(" ; ")[0]
        if ef.endswith(" x"):
            continue
        if len(ef) != len(gf) or any(a != b and a != "?" for a, b in zip(ef, gf)):
            diffs.append((el, gl))
    if len(e_lines) != len(g_lines):
        diffs.append((f"{len(e_lines)} lines", f"{len(g_lines)} lines"))
    return diffs


def show_diffs(diffs, limit=20):
    names = ["d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
             "a7", "sr", "v", "b", "f"]
    by_insn = {}
    for el, gl in diffs:
        desc = el.split(" ; ")[-1]
        by_insn.setdefault(desc.split()[0] if desc else "?", []).append((el, gl))
    for insn, lst in sorted(by_insn.items()):
        print(f"  {insn}: {len(lst)} different")
    for el, gl in diffs[:limit]:
        et = [t for t in el.split(" ; ")[0].split(" ") if t not in ("d", "a", "sr", "v", "b", "f")]
        gt = [t for t in gl.split(" ; ")[0].split(" ") if t not in ("d", "a", "sr", "v", "b", "f")]
        what = [f"{names[j - 1] if 0 < j <= len(names) else j}: {e} / {g}"
                for j, (e, g) in enumerate(zip(et, gt))
                if len(e) != len(g) or any(x != y and x != "?" for x, y in zip(e, g))]
        print(f"  {el.split(' ; ')[0][:4]} {el.split(' ; ')[-1]}: " + "; ".join(what))


# ---- Running ------------------------------------------------------------------

def wrapper(name):
    """The AmigaOS executable around NAME.bin."""
    exe = BUILD / f"{name}.amiga"
    binf = TESTS / f"{name}.bin"
    src = BUILD / f"{name}.amiga.s"
    src.write_text((HERE / "amiga.s").read_text().replace("incbin  CORE_FILE", f'incbin  "{binf}"'))
    r = subprocess.run([VASM, "-Fhunkexe", "-m68020", "-no-opt", "-quiet", "-nosym",
                        "-o", str(exe), str(src)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"vasm failed on amiga.s:\n{r.stdout}{r.stderr}")
    return exe


def parse_hex_output(text):
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if l.startswith("ORACLE "):
            n = int(l.split()[1], 16)
            data = bytes.fromhex("".join(lines[i + 1:]).strip())
            return data[:n]
        if l.startswith("ORACLE-ERROR"):
            raise RuntimeError(l)
    raise RuntimeError("no ORACLE output: " + text[:200])


def run_fsuae(names, cpu):
    sys.path.insert(0, str(EMU_RUN.parent))
    import emu_run
    kick = os.environ.get("DASH_KICKSTART")
    if not kick:
        sys.exit("DASH_KICKSTART is not set: FS-UAE would boot AROS, which is no oracle")
    exes = [wrapper(n) for n in names]
    extra = "" if cpu == "68020" else f"cpu = {cpu}\nfpu = {cpu}\n"
    results = emu_run.run_programs([str(e) for e in exes], timeout=float(os.environ.get("ORACULO_TIMEOUT", "240")), kickstart=kick,
                                   extra_config=extra)
    for name, r in zip(names, results):
        if r.output is None or r.timed_out:
            print(f"{name}: no output from FS-UAE ({'timeout' if r.timed_out else r.rc})")
            continue
        try:
            data = parse_hex_output(r.output)
        except RuntimeError as e:
            print(f"{name}: {e}")
            continue
        out = TESTS / f"{name}.{cpu}.txt"
        out.write_text(records_to_text(name, data, cpu))
        # the same without '?', and the raw records, for looking into
        (BUILD / f"{name}.{cpu}.raw.txt").write_text(records_to_text(name, data, cpu, masks=False))
        (BUILD / f"{name}.{cpu}.rec").write_bytes(data)
        print(f"{name}: {len(data) // REC_SIZE} records -> {out.relative_to(ROOT)}")


def run_emu68(names):
    if not JIT_HOST.exists():
        sys.exit(f"{JIT_HOST} not found (make -C ~/Projects/dashos/jit-host)")
    BUILD.mkdir(parents=True, exist_ok=True)
    status = 0
    for name in names:
        dump = BUILD / f"{name}.emu68.mem"
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run(["qemu-aarch64_be", str(JIT_HOST), str(TESTS / f"{name}.bin"),
                                str(Path(tmp) / "vol"), str(dump)],
                               capture_output=True, text=True, timeout=600, cwd=tmp,
                               preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if not dump.exists():
            print(f"{name}: jit-host wrote no memory dump\n{r.stdout[-2000:]}")
            status = 1
            continue
        mem = dump.read_bytes()
        ptr = struct.unpack_from(">I", mem, RESPTR - CORE)[0]
        if not RESULTS <= ptr <= RESULTS + 0x10000:
            print(f"{name}: bad result pointer {ptr:#x} in Emu68")
            status = 1
            continue
        data = mem[RESULTS - CORE:ptr - CORE]
        text = records_to_text(name, data, "68040")
        (BUILD / f"{name}.emu68.txt").write_text(text)
        oracle = TESTS / f"{name}.68040.txt"
        if not oracle.exists():
            print(f"{name}: {len(data) // REC_SIZE} records (no 68040 oracle to compare)")
            continue
        diffs = compare_text(oracle.read_text(), text)
        total = len([l for l in oracle.read_text().splitlines() if not l.startswith("#")])
        print(f"{name}: Emu68 {total - len(diffs)}/{total} as the 68040 oracle")
        if diffs:
            show_diffs(diffs, 10)
    return status


def run_relevo(names, cpu, limit):
    exe = ROOT / "build" / "oraculo"
    status = 0
    for name in names:
        r = subprocess.run([str(exe), "-d", cpu, str(TESTS / f"{name}.bin")],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"{name}: {r.stderr}")
            status = 1
            continue
        meta = load_meta(name)
        lines = [f"{l} ; {meta[i][0] if i < len(meta) else '?'}"
                 for i, l in enumerate(r.stdout.splitlines())]
        text = "\n".join(lines) + "\n"
        (BUILD / f"{name}.relevo.{cpu}.txt").write_text(text)
        oracle = TESTS / f"{name}.{cpu}.txt"
        if not oracle.exists() or not oracle.read_text().strip():
            print(f"{name}: {len(lines)} records (no {cpu} oracle)")
            continue
        diffs = compare_text(oracle.read_text(), text)
        total = len([l for l in oracle.read_text().splitlines() if not l.startswith("#")])
        print(f"{name}: RelevoJIT {total - len(diffs)}/{total} as the {cpu} oracle")
        if diffs:
            status = 1
            show_diffs(diffs, limit)
    return status


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["gen", "fsuae", "emu68", "relevo", "list"])
    p.add_argument("programs", nargs="*")
    p.add_argument("--cpu", choices=MODELS, default="68020")
    p.add_argument("-v", type=int, default=20, help="differences shown")
    args = p.parse_args()
    names = args.programs or list(PROGRAMS)
    for n in names:
        if n not in PROGRAMS:
            sys.exit(f"unknown program {n}; there are: {' '.join(PROGRAMS)}")
    if args.command == "list":
        for n, f in PROGRAMS.items():
            print(f"{n:8} {f.__doc__.splitlines()[0]}")
    elif args.command == "gen":
        for n in names:
            generate(n)
    elif args.command == "fsuae":
        run_fsuae(names, args.cpu)
    elif args.command == "emu68":
        return run_emu68(args.programs or EMU68_PROGRAMS)
    elif args.command == "relevo":
        return run_relevo(names, args.cpu, args.v)
    return 0


if __name__ == "__main__":
    sys.exit(main())
