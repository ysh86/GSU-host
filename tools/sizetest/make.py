#!/usr/bin/env python3
"""Programs of different sizes for the loader: N NOPs, then an exit in a loop.

size_N.load is loaded and entered at $0200 and exits at R15 = $0200 + N + 4
(`EXIT R15=....`). It shows how far the loaded code runs: if code beyond some
address is fetched wrongly (as 00 = STOP), the run stops there with `BAD OP`
instead of the expected EXIT. Usage: make.py [outdir]
"""
import os, struct, sys

BASE = 0x0200
TAIL = bytes([0xA3, 0x01, 0x00, 0x01, 0x05, 0xFA, 0x01])   # IBT R3,#1; STOP; NOP; BRA back; NOP

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
for n in (32, 56, 58, 64, 80, 112, 139):          # total = N + 7 bytes (139 + 7 = 146)
    payload = bytes([0x01]) * n + TAIL
    with open(os.path.join(out, "size_%d.load" % (n + len(TAIL))), "wb") as f:
        f.write(struct.pack("<HHH", BASE, BASE, len(payload)) + payload)
    print("size_%d.load: %d bytes, exit at R15=%04X" % (n + len(TAIL), len(payload), BASE + n + 4))
