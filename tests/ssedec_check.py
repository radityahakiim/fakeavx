#!/usr/bin/env python3
"""Check core/ssedec.c against Capstone.

Whatever fa_decode_sse() accepts must be a valid SSE/SSE2/SSE3/SSSE3/MMX
instruction (not SSE4 or later), of the same length, with the same
rip-relative target, never using %gs, and every xmm register Capstone says it
reads or writes - explicitly or implicitly - must be in the reported mask.
Also counts the legacy SSE instructions in real code that it does not accept.

  gcc -shared -fPIC -O1 -o libssedec.so ../core/ssedec.c
  ssedec_check.py ./libssedec.so [elf files...]
"""
import ctypes, random, subprocess, sys
from collections import Counter
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone import x86 as X


class FaInt(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_ubyte), ("len", ctypes.c_ubyte), ("cc", ctypes.c_ubyte),
                ("disp_off", ctypes.c_ubyte), ("target", ctypes.c_uint64)]


lib = ctypes.CDLL(sys.argv[1])
lib.fa_decode_sse.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint64,
                              ctypes.POINTER(FaInt), ctypes.POINTER(ctypes.c_uint16)]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
IP = 0x555555550000
ALLOWED = {"sse1", "sse2", "sse3", "ssse3", "mmx"}
LATER = {"sse41", "sse42", "aes", "pclmul", "sha", "avx", "avx2", "fma", "f16c", "avx512",
         "sse4a", "3dnow", "xop", "fma4", "adx", "bmi", "bmi2"}
SSE4_NAMES = {
    "pblendvb", "blendvps", "blendvpd", "ptest", "pmuldq", "pcmpeqq", "movntdqa", "packusdw",
    "pminsb", "pminsd", "pminuw", "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud", "pmulld",
    "phminposuw", "roundps", "roundpd", "roundss", "roundsd", "blendps", "blendpd", "pblendw",
    "pextrb", "pextrd", "pextrq", "extractps", "pinsrb", "pinsrd", "pinsrq", "insertps",
    "dpps", "dppd", "mpsadbw", "pcmpgtq", "pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm",
    "crc32", "popcnt", "lzcnt", "tzcnt",
}
LATER_PREFIXES = ("pmovsx", "pmovzx", "aes", "sha", "pclmul", "v", "extrq", "insertq")
SSE_PREFIXES = ("prefetch", "p", "mov", "cvt", "add", "sub", "mul", "div", "sqrt", "rsqrt", "rcp", "min",
                "max", "and", "or", "xor", "cmp", "ucomis", "comis", "unpck", "shuf", "hadd",
                "hsub", "lddqu", "maskmov", "pshuf", "palignr")
stats = Counter()
missed = Counter()


def groups(ins):
    return {ins.group_name(g) for g in ins.groups}


def check(buf, real=False):
    o = FaInt()
    m = ctypes.c_uint16(0)
    ins = next(md.disasm(bytes(buf), IP, 1), None)
    if lib.fa_decode_sse(bytes(buf), len(buf), IP, ctypes.byref(o), ctypes.byref(m)) != 0:
        stats["rejected"] += 1
        if real and ins is not None and groups(ins) & ALLOWED and not groups(ins) & LATER:
            missed[ins.mnemonic] += 1
        return

    def bad(why):
        stats["bad"] += 1
        if stats["bad"] <= 30:
            print("BAD", why, bytes(buf[:o.len + 1]).hex(),
                  (ins.mnemonic + " " + ins.op_str) if ins else "(invalid)", sorted(groups(ins)) if ins else "")

    if ins is None:
        return bad("capstone rejects")
    stats["accepted"] += 1
    stats["real accepted" if real else "random accepted"] += 1
    g = groups(ins)
    if ins.size != o.len:
        return bad("length %d vs %d" % (o.len, ins.size))
    # Capstone leaves the groups empty for many SSSE3/MMX forms, so the
    # later extensions are also recognised by name.
    if g & LATER or ins.mnemonic in SSE4_NAMES or ins.mnemonic.startswith(LATER_PREFIXES):
        return bad("SSE4 or later accepted")
    if not g & ALLOWED and not ins.mnemonic.startswith(SSE_PREFIXES):
        return bad("not an SSE/MMX instruction")
    if ins.mnemonic in ("emms", "ldmxcsr", "stmxcsr", "fxsave", "fxrstor", "lfence", "mfence",
                        "sfence", "clflush"):
        return bad("special")
    rip = None
    for op in ins.operands:
        if op.type == X.X86_OP_MEM:
            if op.mem.segment == X.X86_REG_GS:
                return bad("gs")
            if op.mem.base == X.X86_REG_RIP:
                rip = (ins.address + ins.size + op.mem.disp) & (2**64 - 1)
            if op.mem.base == X.X86_REG_EIP:
                return bad("eip-relative")
        if op.type == X.X86_OP_REG:
            n = ins.reg_name(op.reg)
            if n.startswith(("ymm", "zmm", "k", "st")):
                return bad("register " + n)
    if (rip is not None) != (o.disp_off != 0):
        return bad("rip-relative detection")
    if rip is not None and rip != o.target:
        return bad("rip target")
    rd, wr = ins.regs_access()
    for r in list(rd) + list(wr):
        n = ins.reg_name(r)
        if n.startswith("xmm") and not (m.value >> int(n[3:])) & 1:
            return bad("xmm accounting: uses %s, mask %#06x" % (n, m.value))


random.seed(2)
PFX = [b"", b"\x66", b"\xf2", b"\xf3", b"\x64", b"\x66\x2e", b"\x65", b"\x67", b"\xf0", b"\x66\xf3"]
for _ in range(600000):
    b = bytearray(random.choice(PFX))
    if random.random() < 0.5:
        b.append(0x40 | random.getrandbits(4))
    b.append(0x0f)
    r = random.random()
    if r < 0.15:
        b += bytes([0x38, random.getrandbits(8)])
    elif r < 0.2:
        b += bytes([0x3a, random.getrandbits(8)])
    else:
        b.append(random.getrandbits(8))
    b += bytes(random.getrandbits(8) for _ in range(15 - len(b)))
    check(b[:15])
for _ in range(200000):
    check(bytearray(random.getrandbits(8) for _ in range(15)))

for f in sys.argv[2:]:
    out = subprocess.run(["objdump", "-h", f], capture_output=True, text=True).stdout
    data = open(f, "rb").read()
    for line in out.splitlines():
        p = line.split()
        if len(p) > 6 and p[1] == ".text":
            size, off = int(p[2], 16), int(p[5], 16)
            text = data[off:off + min(size, 3000000)]
            for ins in md.disasm(text, IP):
                o = ins.address - IP
                check(bytearray(text[o:o + 15].ljust(15, b"\x90")), real=True)
print(dict(stats))
if missed:
    print("legacy SSE/MMX in real code not accepted:", missed.most_common(15))
sys.exit(1 if stats["bad"] else 0)
