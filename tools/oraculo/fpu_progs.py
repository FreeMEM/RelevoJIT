# SPDX-License-Identifier: MPL-2.0
"""FPU programs of the oracle bench (see oraculo.py), task R4.

Each case loads its FP registers with FMOVEM (the bits as they are), sets
FPCR, clears FPSR and FPIAR, runs the code under test and then, also after
an exception, records:

    BUF+0   FP0 (extended, 12 bytes)
    BUF+12  FPSR
    BUF+16  FPIAR
    BUF+20  12 bytes: a memory operand or result (the case's choice)

FS-UAE runs them as a 68020 with a 68882 and as a 68040. Its FPU computes in
double precision (its softfloat mode, uae_fpu_softfloat, gives extended
results but loses FPSR and FPCR, see the bitácora), so the cases keep to
values whose results are exact in double precision: there FS-UAE and an
extended FPU agree. What FS-UAE does not do as the manuals say is masked
('?') or not compared, case by case, with a note in the description:

- it never takes FPU exceptions (no trap cases here; the unit tests have
  them), and runs on the 68040 what the 68040 leaves to software (line F
  in RelevoJIT, as in the real chip);
- the accrued byte gets UNFL with every inexact result, FINT, FSCALE, FMOD
  and FREM raise INEX2 when exact, FMOVE to an integer or packed decimal
  never; FMOD/FREM leave the quotient byte; FSCALE is not exact;
- FPIAR does not change with FMOVE to memory or to Dn;
- it writes infinities and NaNs with a wrong exponent (FMOVEM), and loses
  the N bit of -0;
- FTRAPcc is line F on the 68020 (68882), and some hang it: they are in
  a program of their own, run as a 68040 only;
- FMOVE to FPCR keeps bits 3-0 (the manuals have them as 0).

The precision of the rest (rounding, flags, extended results) is checked
against the x87 in tests/test_fpu.c.
"""

import struct
from fractions import Fraction

VARS = 0x15C000
SCRATCH = VARS + 0x400          # 0x100 bytes for FMOVEM and FSAVE
BUF = None                      # set by oraculo.py

# ---- Values -----------------------------------------------------------------


def x80(sign, exp, mant):
    """The 12 bytes of an extended value, as a hex string."""
    return f"{(sign << 15) | exp:04x}0000{mant:016x}"


def ext_of(q):
    """An exact Fraction (or int) -> extended hex (rounded to nearest)."""
    q = Fraction(q)
    if q == 0:
        return x80(0, 0, 0)
    sign = 1 if q < 0 else 0
    q = abs(q)
    e = q.numerator.bit_length() - q.denominator.bit_length()
    if q < Fraction(2) ** e:
        e -= 1
    m = q / Fraction(2) ** (e - 63)
    mi = m.numerator // m.denominator
    rem = m - mi
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and mi & 1):
        mi += 1
    if mi >> 64:
        mi >>= 1
        e += 1
    return x80(sign, e + 16383, mi)


def single_of(q):
    return struct.pack(">f", float(q)).hex()


def double_of(q):
    return struct.pack(">d", float(q)).hex()


def decimal_digits(q):
    """An exact decimal Fraction -> (sign, digit string, exponent of the
    first digit)."""
    sign = 1 if q < 0 else 0
    q = abs(q)
    k = 0
    while (q * 10 ** k).denominator != 1:
        k += 1
    n = int(q * 10 ** k)
    s = str(n)
    return sign, s, len(s) - 1 - k


def packed_of(q):
    """An exact decimal of up to 17 digits -> packed decimal hex."""
    q = Fraction(q)
    if q == 0:
        return "0" * 24
    sign, s, e10 = decimal_digits(q)
    s = s.rstrip("0") or "0"
    assert len(s) <= 17, q
    s = s.ljust(17, "0")
    ae = abs(e10)
    w0 = (sign << 31) | ((e10 < 0) << 30) | (int(str(ae % 1000).zfill(3), 16) << 16) | int(s[0])
    return f"{w0:08x}" + s[1:17]


def nice(rng, mbits=20, erange=16, sign=True):
    """m * 2^e, |m| < 2^mbits: few significant bits, exact anywhere."""
    m = rng.randrange(1, 1 << mbits)
    e = rng.randrange(-erange, erange + 1)
    v = Fraction(m) * Fraction(2) ** e
    return -v if sign and rng.random() < 0.4 else v


def nice_int(rng, bits=15):
    v = rng.randrange(1, 1 << bits)
    return -v if rng.random() < 0.4 else v


def nice_decimal(rng):
    """A value exact in binary with a short decimal form."""
    m = rng.randrange(1, 100000)
    e = rng.randrange(-6, 12)
    v = Fraction(m) * Fraction(2) ** e if e < 0 else Fraction(m * 2 ** e)
    return -v if rng.random() < 0.4 else v


# ---- Cases --------------------------------------------------------------------


def dc(hexstr):
    words = [hexstr[i:i + 4] for i in range(0, len(hexstr), 4)]
    return "dc.w " + ",".join(f"${w}" for w in words)


def inline(label, hexstr):
    """Data in the code, skipped over with a branch: label it .LABEL."""
    return ["bra.w .s" + label, f".{label}:", dc(hexstr), f".s{label}:"]


def fpu_case(Case, rng, code, desc, fpcr=0, regs=(), mem=None):
    """A case: FP registers from extended hex strings (regs[i] -> FPi, None
    leaves it), FPCR, FPSR and FPIAR 0, then `code`. `mem` (hex) goes to
    BUF+20."""
    pre = []
    for i, h in enumerate(regs):
        if h is None:
            continue
        pre += inline(f"r{i}", h)
        pre.append(f"fmovem.x .r{i}(pc),fp{i}")
    pre += [f"fmove.l #${fpcr:x},fpcr", "fmove.l #0,fpsr", "fmove.l #0,fpiar"]
    c = Case(rng, pre + list(code), desc)
    if mem is not None:
        b = bytearray(c.buf)
        raw = bytes.fromhex(mem)
        b[20:20 + len(raw)] = raw
        c.buf = bytes(b)
    c.post = [f"fmovem.x fp0,${BUF:x}.l",
              f"fmove.l fpsr,${BUF + 12:x}.l",
              f"fmove.l fpiar,${BUF + 16:x}.l"]
    return c


FMT_SIZE = {"b": 1, "w": 2, "l": 4, "s": 4, "d": 8, "x": 12, "p": 12}


def operand(rng, case, fmt, value_hex, modes):
    """Assembler text for a source operand of format fmt with the bytes
    value_hex, in one of `modes`; sets up registers and BUF+20."""
    size = FMT_SIZE[fmt]
    mode = rng.choice(modes)
    n = rng.randrange(1, 7)
    data = bytes.fromhex(value_hex)
    if mode == "Dn":
        d = rng.randrange(8)
        v = int(value_hex, 16)
        case.d[d] = (case.d[d] & ~((1 << (8 * size)) - 1) & 0xFFFFFFFF) | v
        return f"d{d}"
    if mode == "#":
        if fmt in "bw":
            v = int(value_hex, 16)
            if v >> (8 * size - 1):
                v -= 1 << (8 * size)
            return f"#{v}"
        return f"#${value_hex}"
    b = bytearray(case.buf)
    b[20:20 + size] = data
    case.buf = bytes(b)
    addr = BUF + 20
    if mode == "(An)":
        case.a[n] = addr
        return f"(a{n})"
    if mode == "(An)+":
        case.a[n] = addr
        return f"(a{n})+"
    if mode == "-(An)":
        case.a[n] = addr + size
        return f"-(a{n})"
    if mode == "d16(An)":
        disp = rng.randrange(-300, 300)
        case.a[n] = addr - disp
        return f"{disp}(a{n})"
    if mode == "d8(An,Xn)":
        x = rng.choice([r for r in range(8) if r != 0])
        disp = rng.randrange(-100, 100)
        xv = rng.randrange(-100, 100)
        case.d[x] = xv & 0xFFFFFFFF
        case.a[n] = addr - disp - xv
        return f"{disp}(a{n},d{x}.l)"
    if mode == "abs":
        return f"${addr:x}.l"
    raise ValueError(mode)


MEM_SRC = ["(An)", "(An)+", "-(An)", "d16(An)", "d8(An,Xn)", "abs"]


def encode(fmt, q):
    """A value of format fmt as hex (it must be exact in it)."""
    if fmt in "bwl":
        bits = 8 * FMT_SIZE[fmt]
        return f"{int(q) & ((1 << bits) - 1):0{bits // 4}x}"
    if fmt == "s":
        return single_of(q)
    if fmt == "d":
        return double_of(q)
    if fmt == "x":
        return ext_of(q)
    return packed_of(q)


def source(rng, case, q, fmts="bwlsdxp", reg_ok=True):
    """A source operand for the value q (exact in every format chosen):
    an FP register (FP1-FP7, set in `regs`), or <ea> in some format."""
    fmt = rng.choice(fmts)
    modes = MEM_SRC + ["#"] + (["Dn"] if fmt in "bwls" else [])
    return fmt, operand(rng, case, fmt, encode(fmt, q), modes)


def fits(q, mbits=24, emin=-126, emax=127):
    """Whether q has at most mbits significant bits and is in range."""
    if q == 0:
        return True
    q = abs(Fraction(q))
    e = q.numerator.bit_length() - q.denominator.bit_length()
    if q < Fraction(2) ** e:
        e -= 1
    if not emin <= e <= emax:
        return False
    m = q / Fraction(2) ** (e - mbits + 1)
    return m.denominator == 1


def is_dec(q, digits=15):
    if q == 0:
        return True
    _, s, _ = decimal_digits(Fraction(q))
    return len(s.rstrip("0")) <= digits


def fmts_for(q):
    """The formats that hold q exactly."""
    f = ""
    if Fraction(q).denominator == 1:
        if -128 <= q < 128:
            f += "b"
        if -32768 <= q < 32768:
            f += "w"
        if -2 ** 31 <= q < 2 ** 31:
            f += "l"
    if fits(q, 24):
        f += "s"
    if fits(q, 53, -1022, 1023):
        f += "d"
    f += "x"
    if is_dec(q):
        f += "p"
    return f


# ---- Programs ---------------------------------------------------------------------

FPCRS = [0x00, 0x10, 0x20, 0x30, 0x40, 0x80, 0x50, 0xA0]


BASE040 = {"fsadd": "fadd", "fdsub": "fsub", "fsmul": "fmul", "fddiv": "fdiv",
           "fssqrt": "fsqrt", "fdabs": "fabs", "fsneg": "fneg", "fdmove": "fmove"}
SOFT040 = {"fint", "fintrz", "fgetexp", "fgetman", "fscale", "fmod", "frem", "fsglmul",
           "fsgldiv"}


def prog_fpu(Case, rng):
    """FPU arithmetic: FADD, FSUB, FMUL, FDIV, FSQRT, FABS, FNEG, FINT,
    FINTRZ, FGETEXP, FGETMAN, FSCALE, FMOD, FREM, FSGLMUL, FSGLDIV, FMOVE,
    the 68040's FSxxx/FDxxx, every source format and mode."""
    cases = []
    ops = ["fadd", "fsub", "fmul", "fdiv", "fsqrt", "fabs", "fneg", "fint",
           "fintrz", "fgetexp", "fgetman", "fscale", "fmod", "frem", "fsglmul",
           "fsgldiv", "fmove"] + list(BASE040)
    for k in range(400):
        op = ops[k % len(ops)]
        base = BASE040.get(op, op)
        fpcr = rng.choice(FPCRS)
        if base == "fint":
            fpcr &= ~0x30                   # FS-UAE: always to nearest
        # few significant bits: the results are exact in double precision,
        # and in single when the precision or the instruction asks for it
        small = (fpcr & 0xC0) or op in ("fsadd", "fsmul", "fssqrt", "fsneg")
        mb = 10 if small else 20
        a = nice(rng, mb, 6 if small else 12)
        if base in ("fadd", "fsub"):
            b = nice(rng, mb, 6 if small else 12)
            if (base == "fadd" and a == -b) or (base == "fsub" and a == b):
                b += 1
        elif base in ("fmul", "fsglmul"):
            a, b = nice(rng, 11, 10), nice(rng, 11, 10)
        elif base in ("fdiv", "fsgldiv"):
            q, b = nice(rng, 11, 8), nice(rng, 11, 8)
            a = q * b
        elif base == "fsqrt":
            r = nice(rng, 12, 8, sign=False)
            b = r * r
        elif base in ("fint", "fintrz"):
            b = nice(rng, 20, 6)
            if base == "fint" and (b * 2).denominator == 1 and (b * 2).numerator % 2:
                b += Fraction(1, 8)         # no ties: FS-UAE rounds them away
        elif base == "fscale":
            b = Fraction(nice_int(rng, 5))
        elif base in ("fmod", "frem"):
            a, b = Fraction(nice_int(rng, 12)), Fraction(nice_int(rng, 6))
        else:
            b = nice(rng, mb, 12)
        dst = rng.choice([0, 0, 0, rng.randrange(1, 8)])
        regs = [ext_of(nice(rng))] + [None] * 7
        regs[dst] = ext_of(a)
        rr = rng.random() < 0.15
        if rr:
            src_reg = rng.choice([i for i in range(8) if i != dst])
            regs[src_reg] = ext_of(b)
        case = fpu_case(Case, rng, [], "", fpcr=fpcr, regs=regs)
        if rr:
            text = f"{op}.x fp{src_reg},fp{dst}"
        else:
            fmt, src = source(rng, case, b, fmts_for(b))
            text = f"{op}.{fmt} {src},fp{dst}"
        case.code.append(text)
        if dst:
            case.code.append(f"fmovem.x fp{dst},${BUF + 20:x}.l")
        case.desc = f"{text} ; {float(a)!r} {float(b)!r} fpcr ${fpcr:02x}"
        if base in ("fmod", "frem"):
            case.skip(("b", 13, 15))
            case.desc += " [FS-UAE: INEX2 when exact, no quotient byte]"
        if base in ("fint", "fintrz"):
            case.skip(("b", 14, 15))
            case.desc += " [FS-UAE: INEX2 wrong]"
        if base == "fscale":
            case.skip(("x",))
            case.desc += " [FS-UAE: FSCALE inexact]"
        if op in BASE040:
            case.skip(("b", 15, 15))
            case.desc += " [FS-UAE: accrued UNFL]"
        if op in SOFT040:
            case.skip(("x",), ("68040",))
            case.desc += " [68040: line F, FS-UAE runs it]"
        if op in BASE040:
            case.skip(("x",), ("68020",))
            case.desc += " [68882: line F, FS-UAE runs it]"
        cases.append(case)
    return cases


def prog_fpumove(Case, rng):
    """FMOVE to memory and Dn in every format (with k-factors), FMOVEM of
    data and control registers, FMOVECR, FSAVE/FRESTORE."""
    cases = []
    out_modes = ["(An)", "(An)+", "-(An)", "d16(An)", "d8(An,Xn)", "abs"]
    for k in range(210):
        fmt = "bwlsdxp"[k % 7]
        fpcr = rng.choice([0x00, 0x10, 0x20, 0x30]) if fmt in "bwl" else 0
        if fmt in "bwl":
            bits = 8 * FMT_SIZE[fmt]
            v = Fraction(rng.randrange(-(1 << (bits - 1)), 1 << (bits - 1)))
            if rng.random() < 0.5:
                v += Fraction(rng.choice([1, 3, 5, 7]), 8) * rng.choice([1, -1])
                if fpcr == 0x00 and (v * 2).denominator == 1:
                    v += Fraction(1, 16)
        elif fmt == "p":
            v = nice_decimal(rng)
        else:
            v = nice(rng, 20 if fmt == "s" else 40, 30)
        src_reg = rng.randrange(8)
        regs = [ext_of(nice(rng))] + [None] * 7
        regs[src_reg] = ext_of(v)
        case = fpu_case(Case, rng, [], "", fpcr=fpcr, regs=regs)
        size = FMT_SIZE[fmt]
        kf = ""
        if fmt == "p":
            _, s, _ = decimal_digits(abs(v))
            nd = len(s.rstrip("0"))
            kk = rng.choice([nd, nd + 1, 17, max(1, nd - 1)])
            if kk < nd:     # rounding: not on a tie (FS-UAE)
                if s[kk] == "5" and s[kk + 1:].strip("0") == "":
                    kk = nd
            kdyn = rng.random() < 0.5
            kf = f"{{#{kk}}}"
        if fmt in "bwls" and rng.random() < 0.3:
            d = rng.randrange(8)
            text = f"fmove.{fmt} fp{src_reg},d{d}"
        else:
            mode = rng.choice(out_modes)
            n = rng.randrange(1, 7)
            addr = BUF + 20
            if mode == "(An)":
                case.a[n] = addr
                ea = f"(a{n})"
            elif mode == "(An)+":
                case.a[n] = addr
                ea = f"(a{n})+"
            elif mode == "-(An)":
                case.a[n] = addr + size
                ea = f"-(a{n})"
            elif mode == "d16(An)":
                disp = rng.randrange(-200, 200)
                case.a[n] = addr - disp
                ea = f"{disp}(a{n})"
            elif mode == "d8(An,Xn)":
                x = rng.choice([r for r in range(1, 8) if r != n])
                disp = rng.randrange(-100, 100)
                xv = rng.randrange(-100, 100)
                case.d[x] = xv & 0xFFFFFFFF
                case.a[n] = addr - disp - xv
                ea = f"{disp}(a{n},d{x}.l)"
            else:
                ea = f"${addr:x}.l"
            if fmt == "p" and kdyn:
                used = {x} if mode == "d8(An,Xn)" else set()
                d = rng.choice([r for r in range(8) if r not in used])
                case.d[d] = kk & 0x7F | (case.d[d] & ~0x7F & 0xFFFFFFFF)
                kf = f"{{d{d}}}"
                if mode in ("(An)+", "-(An)"):
                    case.skip(("x",))
                    case.desc_extra = " [FS-UAE: FMOVE.P {Dn} leaves An and misses -(An)]"
            text = f"fmove.{fmt} fp{src_reg},{ea}{kf}"
        case.code += [text]
        case.desc = f"{text} ; {float(v)!r} fpcr ${fpcr:02x}" + getattr(case, "desc_extra", "")
        case.skip(("b", 16, 19))
        case.desc += " [FS-UAE: FPIAR]"
        if (fmt in "bwl" and v.denominator != 1) or fmt == "p":
            case.skip(("b", 14, 15))
            case.desc += " [FS-UAE: no INEX2]"
        cases.append(case)

    # FMOVE in from every format and mode, into any register
    for k in range(60):
        v = nice(rng, 12, 8)
        if rng.random() < 0.5:
            v = Fraction(nice_int(rng, 7))
        fmt = rng.choice(fmts_for(v))
        dst = rng.randrange(8)
        case = fpu_case(Case, rng, [], "", regs=[ext_of(nice(rng))])
        src = operand(rng, case, fmt, encode(fmt, v),
                      MEM_SRC + ["#"] + (["Dn"] if fmt in "bwls" else []))
        case.code += [f"fmove.{fmt} {src},fp{dst}"]
        if dst:
            case.code.append(f"fmovem.x fp{dst},${BUF + 20:x}.l")
        case.desc = f"fmove.{fmt} {src},fp{dst} ; {float(v)!r}"
        cases.append(case)

    # FMOVEM.X: static and dynamic lists, predecrement, postincrement, control
    for k in range(40):
        lst = rng.sample(range(8), rng.randrange(1, 4))
        regs = [ext_of(nice(rng)) if i in lst or i == 0 else None for i in range(8)]
        lst.sort()
        names = "/".join(f"fp{i}" for i in lst)
        case = fpu_case(Case, rng, [], "", regs=regs)
        form = k % 4
        span = 12 * len(lst)
        if form == 0:
            case.code += [f"lea ${SCRATCH + span:x}.l,a1", f"fmovem.x {names},-(a1)"]
            text = f"fmovem.x {names},-(a1)"
        elif form == 1:
            mask = sum(1 << i for i in lst)                 # predecrement: bit i = FPi
            case.d[2] = mask
            case.code += [f"lea ${SCRATCH + span:x}.l,a1", "fmovem.x d2,-(a1)"]
            text = f"fmovem.x d2,-(a1) ; d2 ${mask:02x}"
        elif form == 2:
            case.code += [f"lea ${SCRATCH:x}.l,a1", f"fmovem.x {names},8(a1)"]
            text = f"fmovem.x {names},8(a1)"
        else:
            mask = sum(0x80 >> i for i in lst)
            case.d[2] = mask
            case.code += [f"lea ${SCRATCH:x}.l,a1", "fmovem.x d2,(a1)"]
            text = f"fmovem.x d2,(a1) ; d2 ${mask:02x}"
        # back into other registers, postincrement, and into D0-D7
        case.code += [f"lea ${SCRATCH:x}.l,a2"]
        if form == 2:
            case.code += ["addq.l #8,a2"]
        case.code += [f"fmovem.x (a2)+,fp0-fp{len(lst) - 1}",
                      f"movem.l ${SCRATCH:x}.l,d0-d7"]
        case.desc = text
        cases.append(case)

    # control registers: FMOVE and FMOVEM.L
    for k in range(40):
        fpcr = rng.randrange(0, 0x10000) & 0xFFF0
        fpsr = rng.getrandbits(32) & 0x0FFFFFF8
        fpiar = rng.getrandbits(32)
        case = fpu_case(Case, rng, [], "")
        form = k % 5
        if form == 0:
            case.d[3], case.d[4], case.a[5] = fpcr, fpsr, fpiar
            case.code += ["fmove.l d3,fpcr", "fmove.l a5,fpiar", "fmove.l fpcr,d0",
                          "fmove.l fpiar,d1", "fmove.l #0,fpcr"]
            text = "fmove.l dn,fpcr / a5,fpiar"
        elif form == 1:
            case.code += [f"fmove.l #${fpcr:x},fpcr", f"fmove.l #${fpiar:x},fpiar",
                          "fmove.l fpcr,d0", "fmove.l fpiar,d1", "fmove.l #0,fpcr"]
            text = "fmove.l #imm,fpcr/fpiar"
        elif form == 2:
            case.code += [f"move.l #${fpcr:x},-(a7)", f"move.l #${fpiar:x},-(a7)",
                          "fmovem.l (a7)+,fpiar", "fmovem.l (a7)+,fpcr",
                          "fmovem.l fpcr/fpiar,-(a7)", "move.l (a7)+,d0", "move.l (a7)+,d1",
                          "fmove.l #0,fpcr"]
            text = "fmovem.l (a7)+ / fpcr/fpiar,-(a7)"
        elif form == 3:
            case.a[1] = BUF + 20
            case.code += [f"fmove.l #${fpcr:x},fpcr", f"fmove.l #${fpiar:x},fpiar",
                          "fmovem.l fpcr/fpiar,(a1)", "fmove.l #0,fpcr", "fmove.l #0,fpiar",
                          "fmovem.l (a1),fpcr/fpiar", "fmove.l fpcr,d0", "fmove.l fpiar,d1",
                          "fmove.l #0,fpcr"]
            text = "fmovem.l fpcr/fpiar,(a1) and back"
        else:
            case.code += [f"fmove.l #${fpsr & 0x0F000000:x},fpsr", "fmove.l fpsr,d0",
                          "fmove.l #0,fpsr"]
            text = "fmove.l #cc,fpsr [FS-UAE: reads other condition codes]"
            case.skip(("d", 0))
        case.desc = text
        cases.append(case)
    # FPCR bits 3-0: FS-UAE keeps them
    case = fpu_case(Case, rng, ["fmove.l #$ffff,fpcr", "fmove.l fpcr,d0", "fmove.l #0,fpcr"],
                    "fmove.l #$ffff,fpcr [FS-UAE: keeps bits 3-0]")
    case.skip(("d", 0))
    cases.append(case)

    # FMOVECR: the constants exact in double precision, and the reserved ones
    for off in [0x0F, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x01, 0x0A, 0x10, 0x2F, 0x40, 0x7F]:
        case = fpu_case(Case, rng, [f"fmovecr #${off:x},fp0"], f"fmovecr #${off:02x}",
                        regs=[ext_of(1)])
        case.skip(("x",), ("68040",))
        case.desc += " [68040: line F, FS-UAE runs it]"
        if off in (0x01, 0x0A, 0x10, 0x2F, 0x40, 0x7F):
            case.skip(("b", 16, 19))
            case.desc += " [FS-UAE: FPIAR before line F]"
        cases.append(case)

    # FSAVE: null after FRESTORE of a null frame, idle after an instruction
    for k in range(4):
        code = [f"clr.l ${SCRATCH:x}.l", f"frestore ${SCRATCH:x}.l"]
        if k & 1:
            code.append("ftst.x fp1")
        code += [f"lea ${SCRATCH + 0x80:x}.l,a1", "fsave -(a1)", "move.l (a1),d0",
                 "move.l a1,d1", f"sub.l #${SCRATCH + 0x80:x},d1"]
        if k & 2:
            code += ["frestore (a1)+", "move.l a1,d2"]
        case = fpu_case(Case, rng, code, f"fsave {'idle' if k & 1 else 'null'}"
                        + (" frestore" if k & 2 else ""), regs=[ext_of(3), ext_of(2)])
        case.skip(("b", 0, 11))
        case.desc += " [FS-UAE: writes NaN registers wrong]"
        if not k & 1:
            case.skip(("d", 0))
            case.skip(("b", 12, 15))
            case.desc += " [FS-UAE: null frame $00380000, FPSR Z]"
        if k & 2:
            case.skip(("b", 16, 19))
            case.desc += " [FS-UAE: FRESTORE sets FPIAR]"
        cases.append(case)
    return cases


PREDICATES = ["f", "eq", "ogt", "oge", "olt", "ole", "ogl", "or", "un", "ueq", "ugt", "uge",
              "ult", "ule", "ne", "t", "sf", "seq", "gt", "ge", "lt", "le", "gl", "gle",
              "ngle", "ngl", "nle", "nlt", "nge", "ngt", "sne", "st"]


def prog_fpucc(Case, rng):
    """FCMP, FTST and the conditional instructions: FBcc (.W, .L), FScc,
    FDBcc, every predicate."""
    return cc_cases(Case, rng, 352, ["fbw", "fbl", "fs", "fsmem", "fdb"])


def prog_fputrap(Case, rng):
    """FTRAPcc, every predicate (68040 only: FS-UAE's 68882 hangs on some)."""
    return cc_cases(Case, rng, 160, ["ftrap", "ftrapw", "ftrapl"])


def cc_cases(Case, rng, n, forms):
    cases = []
    for k in range(n):
        pred = PREDICATES[k % 32]
        a = nice(rng, 8, 4)
        rel = rng.choice(["lt", "eq", "gt"])
        b = a if rel == "eq" else (a - nice(rng, 8, 4, sign=False) if rel == "gt"
                                   else a + nice(rng, 8, 4, sign=False))
        if rng.random() < 0.15:
            a, b = Fraction(0), Fraction(0)
        regs = [ext_of(a), ext_of(b)]
        form = rng.choice(forms)
        test = rng.choice(["fcmp.x fp1,fp0", "fcmp.x fp1,fp0", "ftst.x fp0"])
        case = fpu_case(Case, rng, [], "", regs=regs)
        code = [test]
        if form in ("fbw", "fbl"):
            sz = "w" if form == "fbw" else "l"
            code += ["moveq #0,d0", f"fb{pred}.{sz} .t", "moveq #1,d0", ".t:"]
            text = f"fb{pred}.{sz}"
        elif form == "fs":
            d = rng.randrange(8)
            code += [f"fs{pred} d{d}"]
            text = f"fs{pred} d{d}"
        elif form == "fsmem":
            case.a[2] = BUF + 21
            code += [f"fs{pred} (a2)+"]
            text = f"fs{pred} (a2)+ [FS-UAE: A2 not incremented]"
            case.skip(("a", 2))
        elif form == "fdb":
            case.d[3] = (case.d[3] & 0xFFFF0000) | rng.choice([0, 1, 3])
            code += ["moveq #0,d0", ".l:", "addq.l #1,d0", f"fdb{pred} d3,.l"]
            text = f"fdb{pred} d3"
        else:
            op = {"ftrap": f"ftrap{pred}", "ftrapw": f"ftrap{pred}.w #$1234",
                  "ftrapl": f"ftrap{pred}.l #$12345678"}[form]
            code += [op]
            text = op
        case.code += code
        case.desc = f"{text} ; {test} {float(a)!r} {float(b)!r}"
        cases.append(case)
    return cases


TRANS = ["fsin", "fcos", "ftan", "fasin", "facos", "fatan", "fsinh", "fcosh", "ftanh",
         "fatanh", "fetox", "fetoxm1", "ftwotox", "ftentox", "flogn", "flognp1", "flog10",
         "flog2", "fsincos"]


MATH = {"fsin": "sin", "fcos": "cos", "ftan": "tan", "fasin": "asin", "facos": "acos",
        "fatan": "atan", "fsinh": "sinh", "fcosh": "cosh", "ftanh": "tanh", "fatanh": "atanh",
        "fetox": "exp", "fetoxm1": "expm1", "flogn": "log", "flognp1": "log1p",
        "flog10": "log10", "flog2": "log2", "fsincos": "sin"}


def near48(op, x):
    """Whether op(x) has bits 48-52 of its mantissa all 0 or all 1 (more or
    less): there FS-UAE's double and an extended result may differ above
    bit 48."""
    import math
    try:
        if op == "ftwotox":
            r = 2.0 ** float(x)
        elif op == "ftentox":
            r = 10.0 ** float(x)
        else:
            r = getattr(math, MATH[op])(float(x))
    except (ValueError, OverflowError):
        return True
    if r == 0 or math.isinf(r):
        return True
    m = int(math.frexp(abs(r))[0] * 2 ** 53)
    low = m & 31
    return low <= 2 or low >= 29


def prog_fputrans(Case, rng):
    """The transcendental functions of the 68881 (line F on the 68040)."""
    cases = []
    for k in range(342):
        op = TRANS[k % len(TRANS)]
        if op in ("fasin", "facos", "fatanh"):
            v = Fraction(rng.randrange(-999, 1000), 1000)
        elif op in ("flogn", "flog10", "flog2"):
            v = nice(rng, 16, 10, sign=False)
        elif op == "flognp1":
            v = Fraction(rng.randrange(-900, 10000), 1000)
        elif op in ("fetox", "fetoxm1", "ftwotox", "ftentox", "fsinh", "fcosh"):
            v = Fraction(rng.randrange(-4000, 4000), 1000)
        else:
            v = Fraction(rng.randrange(-8000, 8000), 1000)
        fmt = rng.choice(["x", "d"])
        case = fpu_case(Case, rng, [], "", regs=[ext_of(1), None, ext_of(2)])
        dv = Fraction(float(v))             # exact in double
        src = operand(rng, case, fmt, encode(fmt, dv), MEM_SRC + ["#"])
        if op == "fsincos":
            case.code += [f"fsincos.{fmt} {src},fp2:fp0", f"fmovem.x fp2,${BUF + 20:x}.l"]
            case.skip(("b", 30, 31))
        else:
            case.code += [f"{op}.{fmt} {src},fp0"]
        # FS-UAE computes in double: compare 48 bits of the mantissa (all
        # of it near a carry into bit 47), and not the accrued byte (it adds
        # UNFL)
        case.skip(("b", 4 if near48(op, dv) else 10, 11))
        case.skip(("b", 15, 15))
        if op == "fsincos" and near48("fcos", dv):
            case.skip(("b", 24, 31))
        case.desc = f"{op}.{fmt} {float(dv)!r} [FS-UAE: 48 bits]"
        case.skip(("x",), ("68040",))
        case.desc += " [68040: line F, FS-UAE runs it]"
        cases.append(case)
    return cases


def programs(Case):
    def bind(f):
        def g(rng):
            return f(Case, rng)
        g.__doc__ = f.__doc__
        return g
    return {"fpu": bind(prog_fpu), "fpumove": bind(prog_fpumove),
            "fpucc": bind(prog_fpucc), "fputrap": bind(prog_fputrap),
            "fputrans": bind(prog_fputrans)}


# Programs run on the 68040 only (FS-UAE's 68882 hangs on FTRAPcc).
ONLY_68040 = {"fputrap"}
