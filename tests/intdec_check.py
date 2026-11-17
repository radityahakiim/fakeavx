#!/usr/bin/env python3
"""Check core/intdec.c against Capstone: whatever it accepts must have the
same length, branch target and rip-relative target, and must not touch
vector registers or %gs.  Usage: intdec_check.py libintdec.so [elf files...]"""
import ctypes, random, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
from capstone import x86 as X

class FaInt(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_ubyte), ("len", ctypes.c_ubyte), ("cc", ctypes.c_ubyte),
                ("disp_off", ctypes.c_ubyte), ("target", ctypes.c_uint64)]

lib = ctypes.CDLL(sys.argv[1])
lib.fa_decode_int.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.POINTER(FaInt)]
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
IP = 0x555555550000
stats = dict(accepted=0, rejected=0, bad=0, cs_invalid=0)

def check(buf):
    o = FaInt()
    if lib.fa_decode_int(bytes(buf), len(buf), IP, ctypes.byref(o)) != 0:
        stats["rejected"] += 1; return
    ins = next(md.disasm(bytes(buf), IP, 1), None)
    def bad(why):
        stats["bad"] += 1
        if stats["bad"] <= 25:
            print("BAD", why, bytes(buf[:o.len + 2]).hex(), ins.mnemonic + " " + ins.op_str if ins else "(invalid)")
    if ins is None:
        stats["cs_invalid"] += 1
        return bad("capstone rejects")
    stats["accepted"] += 1
    if ins.size != o.len: return bad("length %d vs %d" % (o.len, ins.size))
    isbr = ins.group(CS_GRP_JUMP)
    if ins.group(CS_GRP_CALL) or ins.group(CS_GRP_RET): return bad("call/ret accepted")
    if isbr:
        if o.kind == 0: return bad("branch as plain")
        if ins.operands[0].type != X.X86_OP_IMM: return bad("indirect branch")
        if ins.operands[0].imm & (2**64 - 1) != o.target: return bad("branch target")
        if (o.kind == 2) != (ins.id == X.X86_INS_JMP): return bad("jmp/jcc kind")
    elif o.kind != 0: return bad("plain as branch")
    rip = None
    for op in ins.operands:
        if op.type == X.X86_OP_REG:
            n = ins.reg_name(op.reg)
            if n.startswith(("xmm", "ymm", "zmm", "mm", "st", "k")): return bad("vector/fpu reg " + n)
        if op.type == X.X86_OP_MEM:
            if op.mem.segment == X.X86_REG_GS: return bad("gs")
            if op.mem.base == X.X86_REG_RIP: rip = (ins.address + ins.size + op.mem.disp) & (2**64 - 1)
            if op.mem.base == X.X86_REG_EIP: return bad("eip-relative")
    if (rip is not None) != (o.disp_off != 0): return bad("rip-relative detection")
    if rip is not None and rip != o.target: return bad("rip target")
    if ins.id in (X.X86_INS_SYSCALL, X.X86_INS_INT3, X.X86_INS_HLT, X.X86_INS_UD2): return bad("special")

random.seed(1)
for _ in range(400000):
    check(bytearray(random.getrandbits(8) for _ in range(15)))
# real code: every instruction start capstone finds in the given ELF files' text
import subprocess
for f in sys.argv[2:]:
    out = subprocess.run(["objdump", "-h", f], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) > 6 and p[1] == ".text":
            size, off = int(p[2], 16), int(p[5], 16)
            data = open(f, "rb").read()[off:off + min(size, 1500000)]
            for ins in md.disasm(data, IP):
                o = ins.address - IP
                check(bytearray(data[o:o + 15].ljust(15, b"\x90")))
print(stats)
sys.exit(1 if stats["bad"] else 0)
