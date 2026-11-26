#!/usr/bin/env python3
"""Build the multiplier-speed benchmark (CFGR.MS0) for the loader; load and entry $0200.

usage: make.py [iterations [outdir]]      (default 65535, next to this script)

Writes mul_bench.bin, mul_bench.load (loader stream: addr(2) entry(2) len(2) + payload)
and mul_bench.lst.txt. Each phase runs `iterations` loops of 32 identical
operations from the instruction cache; the marker letters it writes with the
write syscall delimit the phases, so the PC app times the gaps between them:

  N  32 x NOP                 baseline (loop overhead + 1 cycle per op)
  M  32 x MULT R5             8x8 signed
  U  32 x ALT1 ; MULT R5      UMULT, 8x8 unsigned (prefix + opcode)
  F  32 x FMULT               16x16 signed, high word
  L  32 x ALT1 ; FMULT        LMULT, 16x16 signed, 32-bit result (prefix + opcode)
  E  end marker, then the program parks in a read-syscall loop

Run it twice (run.py PORT 10 and run.py PORT 10 ms0, which set the clock and MS0 in the
monitor) and compare. Net cost per operation in cycles = (T_phase - T_N) / (iterations x 32)
x clock + 1.
"""
import os, sys

BASE = 0x0200
K = 32                                    # operations per loop iteration
N = int(sys.argv[1]) if len(sys.argv) > 1 else 65535
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
assert 1 <= N <= 65535

out, labels = [], {}
def emit(b, text): out.append(("b", list(b), text))
def label(n): out.append(("l", n, n + ":"))
def branch(op, name, text): out.append(("br", (op, name), text))
def ibt(n, v): emit([0xA0 + n, v & 0xFF], "IBT R%d,#$%02X" % (n, v & 0xFF))
def iwt(n, v): emit([0xF0 + n, v & 0xFF, v >> 8], "IWT R%d,#$%04X" % (n, v))
def stop(): emit([0x00, 0x01], "STOP ; NOP (dummy byte after STOP)")
def putc(ch):
    ibt(0, ord(ch)); ibt(3, 4); stop()   # op = write

PHASES = [
    ("N", [0x01],       "NOP"),
    ("M", [0x85],       "MULT R5"),
    ("U", [0x3D, 0x85], "ALT1 ; MULT R5   (UMULT R5)"),
    ("F", [0x9F],       "FMULT"),
    ("L", [0x3D, 0x9F], "ALT1 ; FMULT     (LMULT)"),
]
label("start")
for tag, code, text in PHASES:
    putc(tag)
    iwt(0, 0x0123)                        # Sreg/Dreg operand
    ibt(5, 3)                             # 8-bit multiplier operand
    iwt(6, 0x4321)                        # 16-bit multiplier operand (FMULT/LMULT)
    iwt(2, N)                             # loop counter
    label("lp_" + tag)
    for _ in range(K): emit(code, text)
    emit([0xE2], "DEC R2")
    branch(0x08, "lp_" + tag, "BNE lp_" + tag)
    emit([0x01], "NOP ; branch delay slot")
putc("E")
label("wait")
ibt(3, 3)                            # op = read
stop()
branch(0x05, "wait", "BRA wait")
emit([0x01], "NOP ; branch delay slot")

addr = 0
for kind, p, t in out:
    if kind == "l": labels[p] = addr
    elif kind == "b": addr += len(p)
    else: addr += 2
blob, lst, addr = [], [], 0
for kind, p, t in out:
    if kind == "l":
        lst.append("            %s" % t); continue
    if kind == "b":
        bs = p
    else:
        op, name = p
        off = labels[name] - (addr + 2)
        assert -128 <= off <= 127, (name, off)
        bs = [op, off & 0xFF]
    lst.append("%04X  %-9s %s" % (BASE + addr, " ".join("%02X" % x for x in bs), t))
    blob += bs; addr += len(bs)

payload = bytes(blob)
hdr = bytes([BASE & 255, BASE >> 8, BASE & 255, BASE >> 8, len(payload) & 255, len(payload) >> 8])
os.makedirs(OUT, exist_ok=True)
open(os.path.join(OUT, "mul_bench.bin"), "wb").write(payload)
open(os.path.join(OUT, "mul_bench.load"), "wb").write(hdr + payload)
open(os.path.join(OUT, "mul_bench.lst.txt"), "w").write("\n".join(lst) + "\n")
print("mul_bench: %d bytes at $%04X, %d iterations x %d ops" % (len(payload), BASE, N, K))
