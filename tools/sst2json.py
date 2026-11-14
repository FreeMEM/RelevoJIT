#!/usr/bin/env python3
# Convert one SingleStepTests/m68000 binary file (v1/*.json.bin) to JSON.
#
# Usage: sst2json.py INPUT.json.bin OUTPUT.json
#
# The binary layout is the one read by upstream's decode.py (MIT License,
# https://github.com/SingleStepTests/m68000). All integers are little-endian.
#
#   file:   u32 magic 0x1A3F5D71, u32 count, then `count` tests
#   test:   u32 size, u32 magic 0xABC12367, name, initial state, final state,
#           transactions
#   name:   u32 size, u32 magic 0x89ABCDEF, u32 length, UTF-8 bytes
#   state:  u32 size, u32 magic 0x01234567, 19 x u32 registers (d0-d7, a0-a6,
#           usp, ssp, sr, pc), 2 x u32 prefetch, u32 n, n x (u32 addr,
#           u16 word) memory words
#   trans:  u32 size, u32 magic 0x456789AB, u32 cycles, u32 n, n x (u8 kind,
#           u32 cycles [, u32 fc, u32 addr, u32 data, u32 uds, u32 lds])
#
# The output has the same shape as upstream's JSON: an array of objects with
# "name", "initial", "final", "transactions" and "length". Memory words become
# byte pairs [address, byte]. One test per line keeps the files small.
import json
import struct
import sys

REGS = ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7',
        'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'usp', 'ssp', 'sr', 'pc']
KINDS = {1: 'w', 2: 'r', 3: 't', 4: 're', 5: 'we'}


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, fmt):
        vals = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += struct.calcsize('<' + fmt)
        return vals

    def magic(self, want):
        _size, got = self.take('II')
        if got != want:
            raise ValueError('bad magic 0x%08X at offset %d' % (got, self.pos - 4))


def read_state(r):
    r.magic(0x01234567)
    st = dict(zip(REGS, r.take('19I')))
    st['prefetch'] = list(r.take('II'))
    (n,) = r.take('I')
    ram = []
    for _ in range(n):
        addr, word = r.take('IH')
        ram.append([addr, word >> 8])
        ram.append([addr | 1, word & 0xFF])
    st['ram'] = ram
    return st


def read_transactions(r):
    r.magic(0x456789AB)
    cycles, n = r.take('II')
    out = []
    for _ in range(n):
        kind, cyc = r.take('BI')
        if kind == 0:
            out.append(['n', cyc])
            continue
        fc, addr, data, uds, lds = r.take('5I')
        if kind not in KINDS:
            raise ValueError('bad transaction kind %d' % kind)
        size = '.w' if uds + lds == 2 else '.b'
        out.append([KINDS[kind], cyc, fc, addr, size, data, uds, lds])
    return out, cycles


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: sst2json.py INPUT.json.bin OUTPUT.json')
    with open(sys.argv[1], 'rb') as f:
        r = Reader(f.read())
    magic, count = r.take('II')
    if magic != 0x1A3F5D71:
        sys.exit('%s: not a SingleStepTests binary file' % sys.argv[1])
    with open(sys.argv[2], 'w') as out:
        out.write('[\n')
        for i in range(count):
            r.magic(0xABC12367)
            r.magic(0x89ABCDEF)
            (length,) = r.take('I')
            name = r.data[r.pos:r.pos + length].decode('utf-8')
            r.pos += length
            test = {'name': name, 'initial': read_state(r), 'final': read_state(r)}
            test['transactions'], test['length'] = read_transactions(r)
            out.write(json.dumps(test, separators=(',', ':')))
            out.write(',\n' if i + 1 < count else '\n')
        out.write(']\n')


if __name__ == '__main__':
    main()
