#!/usr/bin/env python3
"""Build the STB-then-STOP hang test for the loader (load and entry at $0200).

Writes next to this script:
  hang_test.bin       the payload (assembled for $70:0200)
  hang_test.load      loader stream: addr(2) entry(2) len(2) + payload, little endian
  hang_test.lst.txt   listing (address, bytes, source)

Each phase runs 10000 iterations of a RAM-write instruction followed by STOP
(the STOP is the NOP syscall, R1 = 3, so the host services it and resumes):
  A: STB (R3); STOP            -> 'A' ... 'a'
  B: STW (R3); STOP            -> 'B' ... 'b'
  C: STB (R3); NOP; STOP       -> 'C' ... 'c'
  G: STB (R3); NOP x2; STOP    -> 'G' ... 'g'
  H: STB (R3); NOP x3; STOP    -> 'H' ... 'h'
  D: STB (R3); NOP x4; STOP    -> 'D' ... 'd'   (control: several instructions between)
A phase that completes prints its lowercase letter. Then it prints END and
then parks in a read-syscall loop (reload or reset to run again).
If the GSU hangs, the host prints "TIMEOUT: GSU still running" and no
lowercase letter for that phase.
"""
import os, sys, subprocess

# usage: make.py            -> hang_test.* (all phases) and hang_test_A..D.* (one phase each)
#        make.py BCD        -> hang_test_BCD.* (only those phases)
if len(sys.argv) == 1:
    for sel in ("", "A", "B", "C", "G", "H", "D"):
        subprocess.check_call([sys.executable, "-I", os.path.abspath(__file__)] + ([sel] if sel else ["ABCGHD"]))
    sys.exit(0)
SEL = sys.argv[1]
SUFFIX = "" if SEL == "ABCGHD" else "_" + SEL

BASE = 0x0200
SCRATCH = 0x0400          # RAM address written by STB/STW (above the program)
N = 10000

out = []       # (kind, payload, text)
labels = {}

def emit(b, text): out.append(("b", list(b), text))
def label(n): out.append(("l", n, n + ":"))
def branch(op, name, text): out.append(("br", (op, name), text))

def ibt(n, v): emit([0xA0 + n, v & 0xFF], "IBT R%d,#$%02X" % (n, v & 0xFF))
def iwt(n, v): emit([0xF0 + n, v & 0xFF, v >> 8], "IWT R%d,#$%04X" % (n, v))
def stop(): emit([0x00, 0x01], "STOP ; NOP (dummy byte after STOP)")
def nop(t="NOP"): emit([0x01], t)
def putc(ch, name):
    ibt(0, ord(ch) if isinstance(ch, str) else ch)
    ibt(1, 1)
    stop()

def phase(tag, upper, body_bytes, body_text, nops):
    label("ph_" + tag)
    putc(upper, tag)
    ibt(1, 3)                         # op = NOP syscall
    iwt(2, N)                         # iteration count
    iwt(3, SCRATCH)                   # RAM address
    ibt(0, 0x55)                      # data
    label("lp_" + tag)
    emit(body_bytes, body_text)       # the RAM write
    for _ in range(nops): nop()
    stop()                            # STOP right after (nops == 0 means immediately)
    emit([0xE2], "DEC R2")
    branch(0x08, "lp_" + tag, "BNE lp_" + tag)
    nop("NOP ; branch delay slot")
    putc(upper.lower(), tag)

label("start")
for tag, up, body, text, nops in [
    ("A", "A", [0x3D, 0x33], "ALT1 ; STB (R3)", 0),
    ("B", "B", [0x33],       "STW (R3)",        0),
    ("C", "C", [0x3D, 0x33], "ALT1 ; STB (R3)", 1),
    ("G", "G", [0x3D, 0x33], "ALT1 ; STB (R3)", 2),
    ("H", "H", [0x3D, 0x33], "ALT1 ; STB (R3)", 3),
    ("D", "D", [0x3D, 0x33], "ALT1 ; STB (R3)", 4),
]:
    if tag in SEL: phase(tag, up, body, text, nops)
for ch in "END\r\n":
    putc(ch, "")
label("wait")
ibt(1, 0)                             # read syscall: wait for a key (keeps the GSU parked)
stop()
branch(0x05, "wait", "BRA wait")
nop("NOP ; branch delay slot")

# resolve
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
        off = labels[name] - (addr + 2)         # base = byte after the operand
        assert -128 <= off <= 127, (name, off)
        bs = [op, off & 0xFF]
    lst.append("%04X  %-9s %s" % (BASE + addr, " ".join("%02X" % x for x in bs), t))
    blob += bs; addr += len(bs)

here = os.path.dirname(os.path.abspath(__file__))
payload = bytes(blob)
hdr = bytes([BASE & 255, BASE >> 8, BASE & 255, BASE >> 8, len(payload) & 255, len(payload) >> 8])
open(os.path.join(here, "hang_test%s.bin" % SUFFIX), "wb").write(payload)
open(os.path.join(here, "hang_test%s.load" % SUFFIX), "wb").write(hdr + payload)
open(os.path.join(here, "hang_test%s.lst.txt" % SUFFIX), "w").write("\n".join(lst) + "\n")
print("hang_test%s: %d bytes at $%04X" % (SUFFIX, len(payload), BASE))
