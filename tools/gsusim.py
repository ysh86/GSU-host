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
            elif 0x80<=op<=0x8F:    # MULT / UMULT (8x8)
                x=s.r[sr]&0xFF; y=s.r[op&15]&0xFF
                if not a1:
                    x=x-256 if x>127 else x; y=y-256 if y>127 else y
                v=(x*y)&0xFFFF; s.r[d]=v; s.z=v==0
            elif op==0x9F:          # FMULT (high word) / LMULT (R4 = low word, Dreg = high word)
                x=s.r[sr]; y=s.r[6]
                x=x-65536 if x>32767 else x; y=y-65536 if y>32767 else y
                p=(x*y)&0xFFFFFFFF
                if a1: s.r[4]=p&0xFFFF
                s.r[d]=p>>16; s.z=s.r[d]==0
            elif 0x98<=op<=0x9D and a1:
                newpbr=s.r[op&15]&0xFF; tgt=s.r[sr]
                slot=s.fetch(s.r[15]); assert slot==0x01, "slot %02X"%slot   # delay slot runs from old bank
                s.pbr=newpbr; s.r[15]=tgt
            else: raise Exception("unmodelled opcode %02X at %04X"%(op,pc))
            prefix_clear()

def host(g, rx, steps=200000, exits=None):
    """Fake firmware. The syscall number is R3 low: 0 nop, 1 exit, 2 status, 3 read,
    4 write. Exit is recorded in `exits` as (output position, R15) and then resumed,
    like the monitor's "g"."""
    out=[]; rx=list(rx)
    for _ in range(steps):
        g.run(); op=g.r[3]&0xFF
        if op==3:
            if not rx: return out,'rx-empty'
            g.r[0]=rx.pop(0)       # host writes R0 as a zero-extended 16-bit value
        elif op==4: out.append(g.r[0]&0xFF)
        elif op==2: g.r[0]=(1 if rx else 0)
        elif op==0: pass
        elif op==1:
            assert exits is not None, "unexpected exit"
            exits.append((len(out), g.r[15]))
        else: raise Exception("bad op")
    return out,'steps'

def boot(stream):
    """Firmware boot as in gsu_boot(): stage 1 in the cache with R1/R2/R4/R8 preset,
    the firmware serves the loader bytes first, then the PC stream follows."""
    assert len(BOOT)==16 and BOOT[15]==0x01
    g=GSU(BOOT)
    g.r[2]=len(LOADER); g.r[1]=0; g.r[4]=0; g.r[8]=0x70
    return g, LOADER+list(stream)

# --- loader (stage 1 pulls it into RAM at $0000, then it loads the payload)
payload=[0xA3,0x04, 0xA0,0x41, 0x00,0x01, 0x05,0xFA, 0x01]   # write 'A' forever
addr=0x0200; entry=0x0200
hdr=[addr&255,addr>>8, entry&255,entry>>8, len(payload)&255,len(payload)>>8]
g,feed=boot(hdr+payload)
out,why=host(g, feed)
assert bytes(g.ram[:len(LOADER)])==bytes(LOADER), "boot: loader not copied to RAM"
assert bytes(g.ram[addr:addr+len(payload)])==bytes(payload), "boot: payload RAM mismatch"
print("boot: loader ran from RAM, PBR=%02X, out=%r (%s)"%(g.pbr, bytes(out[:5]), why))
assert g.pbr==0x70 and out[:3]==[0x41]*3

# --- exit syscall (op 1): returns to the host with R15 = STOP+2, then resumes
prog=[0xA0,0x58, 0xA3,0x04, 0x00,0x01,      # 'X'
      0xA3,0x01, 0x00,0x01,                 # exit
      0xA0,0x59, 0xA3,0x04, 0x00,0x01,      # 'Y' (after "g")
      0xA3,0x03, 0x00,0x01]                 # read: stop the simulation
h=[0x00,0x02, 0x00,0x02, len(prog),0]
g,feed=boot(h+prog); ex=[]
out,why=host(g, feed, exits=ex)
assert bytes(out)==b"XY" and why=='rx-empty', (bytes(out),why)
assert ex==[(1, 0x0200+10)], ex     # exit after 'X'; R15 = address after STOP + dummy
print("exit: output %r, exit at output position %d, R15=%04X"%(bytes(out),ex[0][0],ex[0][1]))

# --- tools/exit_test.load: exit in a loop, every "g" returns to the monitor
et=list(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"exit_test.load"),"rb").read())
g,feed=boot(et); ex=[]
out,why=host(g, feed, steps=300, exits=ex)
assert why=='steps' and len(ex)>=3 and all(e==(0,0x0204) for e in ex), (why,ex[:3])
print("exit_test.load: %d exits at R15=%04X"%(len(ex),ex[0][1]))

# --- A10-A14 mapping used for the RAM window and the register window
def _def(n): return int(re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)"%n,_SRC).group(1),0)
AH_REGS,AH_RAM,PIN_A10=_def("AH_REGS"),_def("AH_RAM"),_def("PIN_A10")
assert PIN_A10==21 and (_def("SYS_NOP"),_def("SYS_EXIT"),_def("SYS_READ"),_def("SYS_WRITE"))==(0,1,3,4)
assert AH_REGS<<10 | 0 == 0x3000 and (AH_REGS<<10)+0x3FF < 0x3400, "register window"
for off in range(0x2000):                    # the formula of ram_addr_bits()
    a=(off&0x3FF) | ((AH_RAM|((off>>10)&7))<<10)
    assert a==0x6000+off, (off,hex(a))
print("address bits: registers at $%04X, RAM window $6000-$7FFF ok"%(AH_REGS<<10))

# --- tools/echo_demo.load: each received byte comes back +1 (loaded through the loader)
ed=list(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"echo_demo.load"),"rb").read())
g,feed=boot(ed+[0x10,0x20,0x30])
out,why=host(g, feed, steps=2000)
print("echo_demo.load:", [hex(x) for x in out], why)
assert out==[0x11,0x21,0x31]
# --- measurement program: nop syscall, loops forever
g=GSU(MEASURE); calls=0
for _ in range(1000):
    g.run(); assert g.r[3]&0xFF==0, "measure op"; calls+=1
print("measure: %d nop syscalls ok, R15 after STOP=%04X"%(calls,g.r[15]))
# --- STB/STW-then-STOP hang test payload (tools/hangtest), loaded at $0200 through the loader
import subprocess
_h=os.path.join(os.path.dirname(os.path.abspath(__file__)),"hangtest")
subprocess.check_call([sys.executable,"-I",os.path.join(_h,"make.py")],stdout=subprocess.DEVNULL)
ht=list(open(os.path.join(_h,"hang_test.load"),"rb").read())
g,feed=boot(ht)
out,why=host(g, feed, steps=500000)
assert why=='rx-empty', why
assert bytes(out)==b"AaBbCcGgHhDdEND\r\n", bytes(out)
print("hangtest: %r (%s)"%(bytes(out),why))
for t in "ABCGHD":
    g,feed=boot(list(open(os.path.join(_h,"hang_test_%s.load"%t),"rb").read()))
    out,why=host(g, feed, steps=500000)
    assert bytes(out)==(t+t.lower()+"END\r\n").encode(), (t,bytes(out))
print("hangtest single phases ok")
# --- multiplier benchmark payload (tools/multest): small iteration count, in a temp dir
import tempfile
with tempfile.TemporaryDirectory() as _t:
    subprocess.check_call([sys.executable,"-I",os.path.join(os.path.dirname(os.path.abspath(__file__)),"multest","make.py"),"3",_t],stdout=subprocess.DEVNULL)
    mb=list(open(os.path.join(_t,"mul_bench.load"),"rb").read())
g,feed=boot(mb)
out,why=host(g, feed, steps=2000)
assert bytes(out)==b"NMUFLE" and why=='rx-empty', (bytes(out),why)
print("mul_bench: %r (%s)"%(bytes(out),why))
# --- multest/run.py report(): synthetic timings built from known cycle counts
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),"multest"))
import run as _run
_e=65535*32/10.7e6; _t=0; _st=[]
for _c,_cyc in zip("NMUFL",(1,2,3,4,9)):
    _st.append((_c,_t)); _t+=0.4+(_cyc-1)*_e
_st.append(("E",_t))
_rep=_run.report(_st,10.7)
for _c,_cyc in zip("NMUFL",(1,2,3,4,9)):
    _line=[l for l in _rep.splitlines() if l.startswith(_run.NAMES[_c])][0]
    assert abs(float(_line.split()[-1])-_cyc)<0.01,(_line,_cyc)
print("multest report ok")
print("ALL OK")
