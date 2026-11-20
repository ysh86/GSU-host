# Minimal GSU subset simulator + fake host. Reads the programs from ../gsu_host.c. Opcodes per the SNES Super FX table
# (yoshisisland-disassembly wiki). Semantics assumed: STOP -> R15 = addr+2,
# byte after a branch/jump is executed first, branch base = byte after operand.
import sys

import re, os
_SRC=open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","gsu_host.c")).read()
def _arr(name):
    m=re.search(name+r"\[\d+\][^=]*=\s*\{(.*?)\};",_SRC,re.S)
    body=re.sub(r"//.*","",m.group(1))
    return [int(x,16) for x in re.findall(r"0x[0-9A-Fa-f]{2}",body)]
# Single source of truth: the byte arrays in gsu_host.c.
LOADER=_arr("loader_prog")
DEMO=_arr("demo_prog")
MEASURE=_arr("measure_prog")
BOOT=_arr("boot_prog")

class GSU:
    def __init__(s, cache):
        s.r=[0]*16; s.cache=bytearray(512); s.cache[:len(cache)]=bytes(cache)
        s.ram=bytearray(65536); s.pbr=0; s.go=False; s.z=False
    def fetch(s, a):
        if s.pbr==0: return s.cache[a & 0x1FF]
        assert s.pbr==0x70, "bad PBR %x"%s.pbr
        return s.ram[a]
    def run(s, maxsteps=100000):
        s.go=True; n=0
        d=0; sr=0; a1=a2=False
        while s.go:
            n+=1; assert n<maxsteps, "runaway"
            pc=s.r[15]; op=s.fetch(pc); s.r[15]=(pc+1)&0xFFFF
            def imm():
                v=s.fetch(s.r[15]); s.r[15]=(s.r[15]+1)&0xFFFF; return v
            def prefix_clear():
                nonlocal d,sr,a1,a2; d=0; sr=0; a1=a2=False
            if op==0x3D: a1=True; continue
            if op==0x3E: a2=True; continue
            if 0x10<=op<=0x1F: d=op&15; continue
            if 0xB0<=op<=0xBF: sr=op&15; continue
            if op==0x00:
                s.go=False; s.r[15]=(pc+2)&0xFFFF; return 'STOP'
            elif op==0x01: pass
            elif op in (0x05,0x08,0x09):
                off=imm(); off=off-256 if off>127 else off
                base=s.r[15]
                take = op==0x05 or (op==0x08 and not s.z) or (op==0x09 and s.z)
                # delay slot: execute next byte first
                slot=s.fetch(s.r[15]); s.r[15]=(s.r[15]+1)&0xFFFF
                assert slot==0x01, "only NOP slot modelled"
                if take: s.r[15]=(base+off)&0xFFFF
            elif 0xA0<=op<=0xAF: v=imm(); s.r[op&15]=v|0xFF00 if v&0x80 else v   # IBT sign-extends
            elif 0xF0<=op<=0xFF and not a1 and not a2: lo=imm(); hi=imm(); s.r[op&15]=lo|(hi<<8)   # IWT
            elif 0x30<=op<=0x3B and not a1 and not a2:   # STW (Rn)
                v=s.r[sr]; a=s.r[op&15]; s.ram[a]=v&0xFF; s.ram[(a+1)&0xFFFF]=v>>8
            elif 0xD0<=op<=0xDE: v=(s.r[op&15]+1)&0xFFFF; s.r[op&15]=v; s.z=v==0
            elif 0xE0<=op<=0xEE: v=(s.r[op&15]-1)&0xFFFF; s.r[op&15]=v; s.z=v==0
            elif op==0x9E: v=s.r[sr]&0xFF; s.r[d]=v; s.z=v==0
            elif op==0x4D: v=s.r[sr]; v=((v<<8)|(v>>8))&0xFFFF; s.r[d]=v; s.z=v==0
            elif 0xC1<=op<=0xCF and not a1 and not a2: v=s.r[sr]|s.r[op&15]; s.r[d]=v; s.z=v==0
            elif 0x71<=op<=0x7F and a2: v=s.r[sr]&(op&15); s.r[d]=v; s.z=v==0
            elif 0x30<=op<=0x3B and a1: s.ram[s.r[op&15]]=s.r[sr]&0xFF
            elif 0x98<=op<=0x9D and a1:
                newpbr=s.r[op&15]&0xFF; tgt=s.r[sr]
                slot=s.fetch(s.r[15]); assert slot==0x01, "slot %02X"%slot   # delay slot runs from old bank
                s.pbr=newpbr; s.r[15]=tgt
            else: raise Exception("unmodelled opcode %02X at %04X"%(op,pc))
            prefix_clear()

def host(g, rx, steps=200000):
    out=[]; rx=list(rx)
    for _ in range(steps):
        g.run(); op=g.r[1]&0xFF
        if op==0: 
            if not rx: return out,'rx-empty'
            g.r[0]=rx.pop(0)       # host writes R0 as a zero-extended 16-bit value
        elif op==1: out.append(g.r[0]&0xFF)
        elif op==2: g.r[0]=(1 if rx else 0)
        elif op==3: pass
        else: raise Exception("bad op")
    return out,'steps'

# --- loader test
payload=[0xA1,0x01, 0xA0,0x41, 0x00,0x01, 0x05,0xFA, 0x01]   # write 'A' forever
addr=0x0100; entry=0x0100
hdr=[addr&255,addr>>8, entry&255,entry>>8, len(payload)&255,len(payload)>>8]
g=GSU(LOADER)
out,why=host(g, hdr+payload)
assert bytes(g.ram[addr:addr+len(payload)])==bytes(payload), "RAM mismatch"
print("loader: RAM ok, PBR=%02X, out=%r (%s)"%(g.pbr, bytes(out[:5]), why))
assert g.pbr==0x70 and out[:3]==[0x41]*3 or why=='steps'

# --- two-stage boot (MC1): 15-byte stage 1 in line 0 pulls loader_prog into RAM,
# LJMPs to $70:0000, then the normal loader runs from RAM
assert len(BOOT)==16 and BOOT[15]==0x01
g=GSU(BOOT)
g.r[2]=len(LOADER); g.r[3]=0; g.r[4]=0; g.r[8]=0x70
out,why=host(g, LOADER+hdr+payload)
assert bytes(g.ram[:len(LOADER)])==bytes(LOADER), "boot: loader not copied to RAM"
assert bytes(g.ram[addr:addr+len(payload)])==bytes(payload), "boot: payload RAM mismatch"
print("boot: stage 2 loader ran from RAM, PBR=%02X, out=%r (%s)"%(g.pbr, bytes(out[:5]), why))
assert g.pbr==0x70 and out[:3]==[0x41]*3
# --- demo test
g=GSU(DEMO)
out,why=host(g,[0x10,0x20,0x30])
print("demo:", [hex(x) for x in out], why)
assert out==[0x11,0x21,0x31]
# --- measurement program: nop syscall, loops forever
g=GSU(MEASURE); calls=0
for _ in range(1000):
    g.run(); assert g.r[1]&0xFF==3, "measure op"; calls+=1
print("measure: %d nop syscalls ok, R15 after STOP=%04X"%(calls,g.r[15]))
# --- STB/STW-then-STOP hang test payload (tools/hangtest), loaded at $0100 through the loader
import subprocess
_h=os.path.join(os.path.dirname(os.path.abspath(__file__)),"hangtest")
subprocess.check_call([sys.executable,"-I",os.path.join(_h,"make.py")],stdout=subprocess.DEVNULL)
ht=list(open(os.path.join(_h,"hang_test.load"),"rb").read())
g=GSU(LOADER)
out,why=host(g, ht, steps=500000)
assert why=='rx-empty', why
assert bytes(out)==b"AaBbCcGgHhDdEND\r\n", bytes(out)
print("hangtest: %r (%s)"%(bytes(out),why))
for t in "ABCGHD":
    g=GSU(LOADER)
    out,why=host(g, list(open(os.path.join(_h,"hang_test_%s.load"%t),"rb").read()), steps=500000)
    assert bytes(out)==(t+t.lower()+"END\r\n").encode(), (t,bytes(out))
print("hangtest single phases ok")
print("ALL OK")
