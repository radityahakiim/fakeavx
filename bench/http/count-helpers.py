# Count fakeavx helper calls in a program, between its first and second
# SIGSTOP (server.ts / client.ts with STOP=1):
#
#   FAKEAVX_BLOB=helpers/blob.bin gdb -q -batch -x bench/http/count-helpers.py --args bun client.ts N
#
# Translated code reaches the helper blob through its entry points; a
# breakpoint on each, in every copy of the blob in the process (anonymous
# executable mappings starting with the blob's bytes), counts every call,
# including those made from loops that run inside fakeavx's code cache, where
# a breakpoint on the original instruction would miss them. The descriptor in
# rdi names the instruction. Output lines start with "COUNT:".
import collections, os
import gdb

blob = open(os.environ.get("FAKEAVX_BLOB", "helpers/blob.bin"), "rb").read()
ENTRIES = {0x00: "helper(full)", 0x08: "popcnt", 0x10: "crc32", 0x18: "helper(lean)"}
KINDS = {1: "vex", 2: "popcnt", 3: "xgetbv", 4: "sse4-legacy"}
counts = collections.Counter()


class Entry(gdb.Breakpoint):
    def __init__(self, addr, name):
        super().__init__("*%#x" % addr, internal=True)
        self.name = name

    def stop(self):
        if not self.name.startswith("helper"):
            counts[self.name] += 1
            return False
        rdi = int(gdb.parse_and_eval("$rdi")) & ((1 << 64) - 1)
        d = bytes(gdb.selected_inferior().read_memory(rdi, 16))   # struct fa_insn
        kind, mp, opc, imm = d[0], d[2], d[6], d[12]
        counts["%s map%d op %#04x" % (KINDS.get(kind, "?"), mp, opc)] += 1
        if mp == 3 and 0x60 <= opc <= 0x63:                        # (v)pcmpXstrX: operands
            rax = int(gdb.parse_and_eval("$rax")) & 0xffffffff
            rdx = int(gdb.parse_and_eval("$rdx")) & 0xffffffff
            counts["  imm %#04x eax %d edx %d" % (imm, rax, rdx)] += 1
        return False


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("handle SIGSTOP stop print nopass")
gdb.execute("handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint pass")
gdb.execute("run")
pid = gdb.selected_inferior().pid
copies = 0
for line in open("/proc/%d/maps" % pid):
    f = line.split()
    if f[1] != "r-xp" or f[4] != "0" or len(f) > 5:
        continue
    lo = int(f[0].split("-")[0], 16)
    try:
        head = bytes(gdb.selected_inferior().read_memory(lo, 32))
    except gdb.MemoryError:
        continue
    if head == blob[:32]:
        copies += 1
        for off, name in ENTRIES.items():
            Entry(lo + off, name)
print("COUNT: blob copies %d" % copies)
gdb.execute("continue")
for k, v in counts.most_common():
    print("COUNT: %8d  %s" % (v, k))
gdb.execute("kill")
