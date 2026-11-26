#!/usr/bin/env python3
"""Run mul_bench.load on the board and report the multiplier timings.

usage: run.py PORT [10|21] [ms0]   (needs pyserial). Run it right after a reset: it sends
the monitor commands "c 0|1" (clock, default 10 = 10.7 MHz) and "m 0|1" ("ms0" sets the
high-speed multiplier), then "l", waits for the READY line, sends the benchmark and
times the marker letters. The clock for the cycle counts is the one selected.
Not run on hardware yet; report() is checked with synthetic data in check.sh.
"""
import os, re, sys, time

ITER, K = 65535, 32
NAMES = {"N": "NOP (baseline)", "M": "MULT", "U": "UMULT (ALT1+MULT)", "F": "FMULT", "L": "LMULT (ALT1+FMULT)"}

def report(stamps, mhz, iters=ITER, k=K):
    """stamps: list of (letter, seconds) in order N M U F L E. Returns text."""
    t = {a: b2 - b for (a, b), (_, b2) in zip(stamps, stamps[1:])}
    base = t["N"]
    lines = ["%-20s %8s %12s" % ("phase", "time s", "cycles/op")]
    if min(t[c] for c in "NMUFL") < 0.05:
        lines.append("WARNING: a phase took under 50 ms; the timing is not valid")
    for c in "NMUFL":
        cyc = (t[c] - base) / (iters * k) * mhz * 1e6 + 1
        lines.append("%-20s %8.3f %12.2f" % (NAMES[c], t[c], cyc))
    return "\n".join(lines)

def main():
    import serial                                    # pyserial
    port = sys.argv[1]
    clk21 = len(sys.argv) > 2 and sys.argv[2] == "21"
    ms0 = "ms0" in sys.argv[2:]
    mhz = 21.477 if clk21 else 10.738
    data = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mul_bench.load"), "rb").read()
    ser = serial.Serial(port, 115200, timeout=0.1)
    ser.write(b"m 0\rc %d\rm %d\ri\rl\r" % (clk21, ms0))   # monitor: clock, multiplier, banner, load
    buf = b""
    while b"READY\r\n" not in buf:
        buf += ser.read(256)
    banner = buf.decode(errors="replace")
    print(banner.strip().splitlines()[-2:])
    print("clock used for the cycle counts: %.3f MHz, MS0=%d" % (mhz, ms0))
    ser.write(data)
    ser.timeout = 30
    stamps = []
    while not stamps or stamps[-1][0] != "E":
        # one byte per read: read(n) waits for n bytes or the timeout, which would
        # put several letters into one time slot and quantize every phase time
        c = ser.read(1)
        if not c:
            sys.exit("timeout: no marker for 30 s (did the program start?)")
        if chr(c[0]) in "NMUFLE":
            stamps.append((chr(c[0]), time.monotonic()))
    seq = "".join(c for c, _ in stamps)
    if seq != "NMUFLE":
        print("markers received: %s (expected NMUFLE); raw times: %s" % (seq, [(c, round(t, 3)) for c, t in stamps]))
        sys.exit("a marker is missing or repeated, so the phases cannot be timed")
    print(report(stamps, mhz))

if __name__ == "__main__":
    main()
