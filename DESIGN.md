# fakeavx — fast AVX / AVX2 / POPCNT for Core 2 on Linux

Design document. See README.md for what is implemented and tested; §15 lists
where the implementation departs from the original plan below. Items marked
**[verify]** were assumptions at design time.

## 1. Goal

Run unmodified x86-64 Linux binaries that use AVX, AVX2 and POPCNT on Core 2
Duo/Quad CPUs (Merom/Conroe: up to SSSE3; Penryn/Wolfdale/Yorkfield: up to
SSE4.1), delivered as a loadable kernel module, and much faster than Intel SDE.

**Why it can beat SDE.** SDE is built on Pin: it JIT-translates *every*
instruction of the process, so even code that never touches AVX pays the
translation tax. fakeavx leaves the process running natively and only gets
involved where an unsupported instruction actually sits. Non-AVX code runs at
100% native speed; AVX code runs as native SSE code generated once.

**Non-goals (v1):** 32-bit processes, AVX-512, kernel-mode AVX, ptrace/gdb
showing YMM registers, `/proc/cpuinfo` cosmetics, bit-exact FMA at full speed.

## 2. Hardware facts that shape the design

| Fact | Consequence |
|---|---|
| In 64-bit mode every `C4`/`C5` (VEX) byte sequence raises #UD on a pre-AVX CPU | Every AVX/AVX2/BMI/FMA instruction is its own trap. Nothing has to be found ahead of time. |
| `F3 0F B8` (POPCNT) raises #UD on Core 2 **[verify]** | Same mechanism covers POPCNT. |
| `LZCNT`/`TZCNT` do **not** trap — they silently run as `BSR`/`BSF` | Cannot be fixed by trapping. We must never advertise LZCNT/BMI1 in CPUID (§9). |
| No YMM state: XCR0 bit 2 cannot be set; Merom has no XSAVE at all | The upper 128 bits of YMM0–15 need software storage per thread (§5). |
| No CPUID faulting (Ivy Bridge+ feature), CPUID does not trap in ring 3 | Feature detection must be spoofed some other way (§9). |
| No PCID, CPU is Meltdown-affected so PTI is on | A user→kernel→user round trip costs on the order of 1,000+ cycles. **Per-instruction trapping is hopeless for hot code.** |
| No FSGSBASE, but userspace `%gs` is unused on x86-64 Linux | `%gs` is free to be our per-thread state pointer (§5). |
| Host SIMD is 128-bit: SSE2/SSE3/SSSE3 (+SSE4.1 on Penryn) | One 256-bit op becomes two 128-bit ops; some ops need synthesized sequences, with two template tiers (SSSE3 / SSE4.1). |

## 3. Core idea: the kernel translates, it never emulates

The obvious design — catch #UD, emulate the instruction inside the kernel,
return — pays the full trap cost on every executed instruction and forces the
kernel to manipulate user FPU state (messy with lazy FPU restore).

fakeavx instead treats the #UD as a **JIT request**:

1. #UD arrives from user mode at address `A`.
2. Look up `A` in the per-process translation cache. On a miss, decode the
   guest code starting at `A`, and emit an equivalent SSE-only sequence into a
   per-process executable **code cache**.
3. Set `regs->ip` to the translated code and return to user mode.
4. If possible, patch a `jmp` over the original instruction so that this site
   never traps again (§7).

Consequences:

- The kernel module contains **no floating-point code** and never touches user
  XMM state. It is a decoder + code emitter + patcher.
- Memory operands are accessed by the translated code in user mode, so page
  faults, SIGSEGV addresses and `mprotect` semantics are naturally correct.
- Complex instructions (gathers, FMA, variable shifts) call into a **user-mode
  helper blob** mapped into the process, rather than trapping.

```
 user process                                   kernel (fakeavx.ko)
┌──────────────────────────────┐
│ original text                │   #UD (first time only)
│   ...                        │ ─────────────────────────► die notifier
│   vaddps ymm0,ymm1,ymm2  ◄───┼── patched to jmp ──┐          │ lookup / decode
│   ...                        │                    │          │ translate, emit
├──────────────────────────────┤                    │          │ patch site
│ code cache (R-X)             │ ◄──────────────────┘          │
│   translated traces          │ ◄─────── regs->ip = trace ────┘
│   helper blob (gather, fma…) │
│   translation hash table     │
├──────────────────────────────┤
│ per-thread state (%gs)       │  ymm high halves, spill slots, scratch
└──────────────────────────────┘
```

## 4. Kernel entry point

`exc_invalid_op` → `do_error_trap()` calls `notify_die(DIE_TRAP, …)` before
raising SIGILL, and treats `NOTIFY_STOP` as "handled" **[verify on target
kernel]**. `register_die_notifier()` is exported, so no IDT hooking, no
kprobes on `noinstr` code, no kernel patching.

Handler outline:

```
if (val != DIE_TRAP || trapnr != X86_TRAP_UD || !user_mode(regs)) return NOTIFY_DONE;
if (!task_opted_in(current) || !user_64bit_mode(regs))            return NOTIFY_DONE;
local_irq_enable();                    /* task context; we may fault/sleep */
ok = fakeavx_handle(regs);             /* lookup → translate → redirect   */
local_irq_disable();
return ok ? NOTIFY_STOP : NOTIFY_DONE; /* DONE → normal SIGILL            */
```

Processes are **opt-in** (launcher `fakeavx-run ./app` registers itself through
`/dev/fakeavx`, inherited over fork/exec), with a sysfs switch for global mode.
This keeps the blast radius small while the translator matures.

## 5. Guest state

Low 128 bits of each YMM live in the real XMM register. High halves live in a
per-thread **state block** addressed by `%gs`:

```
gs:0x000  hi[16]      16 × 16 bytes, 16-byte aligned   (ymm0..15 upper halves)
gs:0x100  zflag[16]   byte per register: "upper half is logically zero"
gs:0x110  spill slots (xmm + gpr scratch), helper scratch, flags save
gs:0x1xx  self pointer, tid, shadow-stack pointer for signals
```

The module allocates the block with `vm_mmap()` on first trap in a thread and
installs the base the same way `arch_prctl(ARCH_SET_GS)` does
(`thread.gsbase` + `MSR_KERNEL_GS_BASE`, selector 0).

- New thread (`CLONE_VM`): a fork hook (tracepoint or kprobe) allocates a fresh
  block and copies the parent's contents; plain `fork()` gets a COW copy for free.
- A process that sets its own GS base (64-bit Wine, a few runtimes) is detected
  and refused in v1.

**Principle: all per-process state lives in the process's own memory** — code
cache, translation hash table, state blocks. Fork duplicates it consistently
with the patched text, exec/exit destroy it, and the kernel keeps almost
nothing per mm. The kernel treats that memory as untrusted input; the worst a
process can do by corrupting it is redirect itself to its own addresses.

## 6. Translation

### 6.1 Decoder
Own table-driven VEX decoder (maps 0F / 0F38 / 0F3A, L, W, pp, vvvv, ModRM/SIB,
RIP-relative), plus enough of a legacy decoder to *relocate* ordinary integer
instructions inside a trace (length, RIP-relative fixups, branch targets).
Written as OS-agnostic C so the same code builds in userspace for testing (§11).

### 6.2 Lowering classes

| Class | Examples | Lowering |
|---|---|---|
| VEX.128 / scalar | `vaddsd`, `vmovaps xmm`, `vpshufb xmm` | Legacy SSE op (+ `movaps` to fix 3-operand form), mark upper half zero. ~1.0–1.3× native cost. **This is most of what `-mavx` code executes.** |
| 256-bit lane-wise | `vaddps`, `vpaddd`, `vpshufb`, `vpand`, `vpcmpeq*`, shifts | Same SSE op applied to low (register) and high half. |
| Cross-lane moves | `vextractf128`, `vinsertf128`, `vperm2f128`, `vpermq`, `vbroadcast*` | Mostly plain moves between halves — cheap, sometimes cheaper than native. |
| Needs synthesis | `vpcmpgtq` (no SSE4.2), SSE4.1 ops on Merom (`pmulld`, `pblendvb`, `ptest`, `pmovzx*`, `roundps`, `pminsd`…) | Template sequences; host tier chosen at module load. |
| Helper call | gathers, `vpsllv*/vpsrlv*/vpsrav*`, `vpermd/vpermps`, `vmaskmov*`, FMA, F16C, `pdep/pext` | Call into user-mode helper blob (plain C built with `-msse2`, embedded in the module). |
| State ops | `vzeroupper`, `vzeroall` | Set `zflag[]` (lazy), no data movement. |
| Flag producers | `vptest`, `vtestps`, `vucomiss`, `popcnt` | Sequences that end by producing exact EFLAGS. |
| POPCNT | `popcnt r, r/m` | Inline SWAR (or SSSE3 `pshufb` nibble table), ~15 cycles vs >1,000 for a trap. |

Three-operand aliasing (`dst == src2` for non-commutative ops), unaligned
memory operands (`vmovups`/VEX relaxes alignment → use `movups` loads, never
fold memory into an aligned-only SSE operand), and MXCSR exception behaviour are
handled in the templates.

## 7. Optimization techniques

In priority order — each is independently shippable.

**O1. Translation cache.** `rip → translation` hash table (in process memory).
A repeat trap costs trap + lookup + redirect, no decode. Baseline tier.

**O2. Site patching — eliminate the trap.** Overwrite the trapping instruction
with `jmp rel32` into the code cache; the trace ends with `jmp` back. Cost after
patching: two direct jumps. Rules:
- Only patch an instruction that is itself ≥5 bytes, and only overwrite bytes
  of that one instruction. Later instructions in the run stay intact, so a
  branch into the middle of the run still lands on a valid (trapping) original.
- Code cache regions are allocated within ±2 GB of each text mapping.
- Sites with a <5-byte head stay on O1 unless O3/O4 swallow them.
- **Cross-modification is safe for free:** the first byte (`C4`/`C5`) always
  traps on this CPU. Write the 4 displacement bytes first, then flip byte 0 to
  `E9`. A racing thread either takes #UD (handler redirects it) or sees the
  finished jump — the same idea as the kernel's int3 `text_poke_bp`, without IPIs.
- Text is written with `access_process_vm(FOLL_FORCE)` → private COW page, as
  ptrace/uprobes do.

**O3. Trace formation, not single instructions.** Translate forward from the
trap point through the whole basic block and across direct branches, relocating
the ordinary integer instructions in between (loop counters, pointer bumps,
`cmp/jcc`). A hot loop then runs *entirely* inside the code cache: one entry
per loop, not per iteration. Traces end at indirect branches, `call` (emitted
as push-original-return-address + `jmp`, keeping unwinders and C++ EH correct),
`ret`, `syscall`, or a size limit.

**O4. Trace chaining and lookahead.** When a trace exits to original address
`T`, `T` is a known instruction boundary. Scan forward from `T`; if VEX code
follows shortly, extend/link to a translation of `T` directly cache-to-cache.
This catches the short-head sites O2 could not patch.

**O5. High-half register caching (the big one for 256-bit loops).** Guest code
in a trace typically uses a handful of YMM registers. Host XMM registers the
trace never references are borrowed to hold the *upper halves* of the ones it
does: guest `ymm0` ↔ host (`xmm0`, `xmm9`). Borrowed registers are spilled to
`gs:` at trace entry and restored at exits (not at all if liveness shows they
are overwritten first). Inside the loop a 256-bit op is then exactly two
register-register SSE ops — no memory traffic, no store-forwarding latency in
dependency chains. Falls back to `gs:`-memory operands when no register is free.

**O6. Lazy upper-zero tracking.** VEX.128 writes and `vzeroupper` must zero
upper halves. Instead of storing zeros, set `zflag[reg]` (a `movb`, which does
not touch EFLAGS), deduplicated within a trace by static known-zero tracking.
Only a trace that *reads* a high half materializes it, via a flag-free check
(`jrcxz`) at trace entry. Scalar/128-bit AVX code — the common case — never
computes a high half at all.

**O7. EFLAGS liveness.** AVX arithmetic does not modify flags, so glue code is
restricted to flag-neutral instructions (`mov`, `lea`, `movb`, `jrcxz`). Where
a cheaper sequence needs `shl/or/add` (e.g. combining two `pmovmskb` results
for `vpmovmskb ymm`, POPCNT's SWAR), a forward scan proving the flags are dead
(next flag writer precedes any reader) allows it. Otherwise save/restore with
`lahf`/`seto`, never `pushf` (slow, and the red zone is live).

**O8. Inline helpers, no traps for hard instructions.** Gathers, variable
shifts, FMA etc. call the user-mode blob with a private calling convention
(operands passed via `gs:` slots, all guest registers preserved). FMA uses an
exact software path; an opt-in "fast-math FMA" (`mul`+`add`, not bit-exact)
can be offered per process.

**O9. Peepholes and idioms.** `vxorps r,r,r`/`vpxor` zero idiom → `pxor` + zflag;
`vpcmpeq r,r,r` ones idiom; `vmovaps` reg-reg → single `movaps` + `hi` move or
rename; `vextractf128 $1` + op + `vinsertf128` fusion; dead high-half
elimination when a later instruction in the trace fully overwrites the register.

**O10. Host-tier templates.** Chosen once at module load from real CPUID:
SSE4.1 forms on Penryn, SSSE3 synthesis on Merom.

**O11. Cheap slow path.** Per-CPU decode scratch, lock-free cache lookup,
translation done with IRQs on. Optionally document `pti=off` as a user choice
that roughly halves residual trap cost (with its security cost stated).

### Expected performance (estimates, to be measured)

| Code | fakeavx | Naive in-kernel emulation | SDE |
|---|---|---|---|
| Non-AVX code in the process | 1.0× (native) | 1.0× | several × slower |
| Scalar / VEX.128 code, patched | ~1.0–1.3× of SSE build | 100×+ | several × |
| 256-bit loops, reg-cached (O5) | ~2–3× of what a native 128-bit build costs | 100×+ | several × |
| Unpatchable cold sites (O1 only) | ~1,000+ cycles each | same | n/a |

## 8. Correctness hazards

- **Signals.** A handler interrupting a trace sees `rip` in the code cache
  (resuming there is fine). A handler that itself uses AVX would clobber the
  interrupted code's high halves, since the kernel signal frame has no YMM
  area. Fix: hook signal delivery/`rt_sigreturn` to push/pop the `gs:` state on
  a per-thread shadow stack and to map a cache `rip` back to the original
  instruction boundary (each guest instruction's fragment is restartable).
  Scheduled after the core works.
- **Faulting memory operands** inside a trace report a cache `rip` in
  `ucontext`; same remapping table solves it.
- **Self-inspecting / self-modifying code** (checksummed text, JITs rewriting
  code we translated): detect writes via page protection of translated source
  ranges is possible later; v1 documents it as unsupported.
- **Indirect jumps into the middle of a patched instruction**: not produced by
  compilers; accepted risk.
- **MXCSR/exception flags, NaN propagation, denormals**: lane-wise SSE ops
  match AVX semantics; synthesized sequences and helpers must be tested for it.
- **`vmaskmov`/gather fault suppression**: masked-off elements must not fault;
  helpers access only selected elements.

## 9. Making software *choose* AVX: CPUID and XGETBV

Binaries built with `-mavx2` / `-march=haswell` / `x86-64-v3` just work. Runtime
dispatchers (glibc ifuncs, OpenBLAS, ffmpeg, JITs) check CPUID and XGETBV first.

**XGETBV** (must report XCR0 with the YMM bit):
- Merom has no XSAVE → `xgetbv` raises #UD → we answer `XCR0 = 7` in the handler.
- Penryn has XSAVE; boot with `noxsave` so `CR4.OSXSAVE` stays clear and
  `xgetbv` raises #UD as well **[verify]**.

**CPUID** cannot be trapped on this hardware. Options, in order of preference:

1. **uprobes on known CPUID sites (recommended).** A userspace tool
   (`fakeavx-scan`) finds real `cpuid` instructions in an ELF file by
   disassembling from function starts (symbols / `.eh_frame`), caches results
   by build-id, and hands `(inode, offset)` lists to the module, which registers
   uprobes. The uprobe handler fills in spoofed leaf 1 / leaf 7 results for
   opted-in tasks and skips the instruction **[verify that a uprobe handler may
   advance `ip` on the target kernel]**. The kernel handles every mapping and
   every future `mmap` of that file. Covering `ld.so` + `libc` handles most
   software; the scanner covers the rest on demand.
2. **VT-x thin hypervisor.** CPUID exits unconditionally under VMX, so a
   minimal "host-as-guest" hypervisor spoofs it perfectly. Large extra
   subsystem, many Core 2 SKUs lack VT-x; kept as a possible later phase.
3. No spoofing — only statically-AVX binaries work.

Advertised: AVX, AVX2, POPCNT, OSXSAVE (+ FMA, F16C, BMI2, MOVBE once
implemented). **Never** advertised: LZCNT/ABM and BMI1 (because `lzcnt`/`tzcnt`
cannot trap), AVX-512, AES-NI/PCLMUL unless implemented. Note this means a
strict `x86-64-v3` check (which requires LZCNT+BMI1) will correctly fail.

## 10. Security and robustness

- The decoder parses attacker-controlled bytes in kernel context: bounded
  reads via `copy_from_user`, strict length limits, fuzzed in userspace first.
- Emitted code only ever runs in ring 3 with the process's own privileges.
- Code cache is mapped R-X; the kernel writes through `access_process_vm`.
- Per-process cache size cap; when full, flush and unpatch-or-retranslate.
- Anything undecodable → `NOTIFY_DONE` → the normal SIGILL. Never guess.
- Module unload: refuse while opted-in processes exist.

## 11. Testing strategy

- **Differential testing on an AVX2 machine (the dev box).** The translator
  core is plain C; a userspace harness runs each instruction natively and its
  SSE-only translation on random and edge-case inputs (NaNs, denormals, all
  mask patterns, aliasing combinations) and compares registers, memory, flags
  and MXCSR. This validates the bulk of the project with no kernel involved.
- **Decoder fuzzing** against a reference disassembler (Capstone/XED).
- **Kernel integration in QEMU TCG** with `-cpu core2duo` / `-cpu Penryn`,
  where VEX really raises #UD. (WSL2 on a modern host cannot exercise the trap
  path.)
- **Real hardware** last: Merom and Penryn, for errata, timing and benchmarks
  (glibc string functions, x264/ffmpeg, OpenBLAS, a `-march=haswell` SPEC-like set).

## 12. Milestones

| # | Deliverable | Techniques |
|---|---|---|
| M0 | Userspace decoder + translator core + differential harness; VEX.128 and lane-wise 256-bit subset; POPCNT | — |
| M1 | Kernel module: die notifier, opt-in, state block via `%gs`, code cache, single-instruction translation | O1, O10 |
| M2 | Site patching | O2 |
| M3 | Traces, chaining, flag liveness | O3, O4, O7 |
| M4 | High-half register caching, lazy zero tracking, peepholes | O5, O6, O9 |
| M5 | Helper blob: gathers, variable shifts, permutes, maskmov; then FMA/F16C/BMI2 | O8 |
| M6 | XGETBV + CPUID spoofing (scanner + uprobes) | §9 |
| M7 | Signal shadow stack, `rip` remapping, thread/fork hardening | §8 |

## 13. Proposed layout

```
core/      decoder, lowering templates, emitter, trace builder   (OS-agnostic C)
helpers/   user-mode helper blob (SSE2-only C/asm)
kmod/      die notifier, mm/cache management, patcher, uprobes, /dev/fakeavx
tools/     fakeavx-run (launcher), fakeavx-scan (CPUID site finder)
tests/     differential harness, decoder fuzzer, QEMU integration scripts
```

## 14. Open questions

1. Confirm the `notify_die` path and uprobe `ip`-skip on the kernel version we target (which one?).
2. Target CPUs: Merom + Penryn, or Penryn only? (Penryn-only drops the SSSE3 synthesis tier.)
3. Is FMA/BMI2/F16C in scope, or strictly AVX + AVX2 + POPCNT?
4. Is bit-exact FMA required, or is an opt-in fast mode acceptable?
5. Is booting with `noxsave` acceptable on Penryn machines?

## 15. Implementation notes (what changed while building it)

**Scope.** Penryn natively; SSSE3-only CPUs through C implementations of SSE4.1 (no synthesized templates). FMA, F16C
and BMI are out: AVX/AVX2/POPCNT do not depend on them. Implemented: O1, O2,
O3, O4 (within a region), O5, O8, O10. Not implemented: O6, O7, O9.

**Sleeping in the #UD handler (§4).** Confirmed on 6.18.39: `do_error_trap()`
calls `notify_die()` and honours `NOTIFY_STOP`. But the die chain is an atomic
notifier chain walked under `rcu_read_lock()`, and translation has to sleep
(user memory, `mmap`). `task_work_add()` is not exported to modules. The
handler therefore has two paths:

- *fast path*, fully atomic: look up the translation, compare the guest bytes
  with `copy_from_user_nofault()`, set `regs->ip`.
- *slow path*: drop out of the RCU section, enable interrupts, do the work,
  re-enter, and always return `NOTIFY_STOP`. That is sound only because
  `notifier_call_chain()` does not look at the chain again after a STOP, and
  because an in-flight counter keeps the module from unloading meanwhile. If
  the slow path decides the instruction is not ours, it records that and
  returns without changing `rip`; the instruction traps again and the fast
  path lets the normal SIGILL happen.

**Per-process state (§5).** The translation table lives in the kernel (an
xarray per mm, torn down from an mmu notifier), not in process memory; a
forked child simply re-translates on demand. Each entry stores the guest bytes
it was made from, so stale entries (unmapped library, rewritten JIT code) are
detected on the fast path. State blocks come from a per-mm pool so that a
kprobe on `wake_up_new_task` can hand a new thread its own block from atomic
context; a kprobe on `do_exit` returns it.

**Patching instructions shorter than 5 bytes (extends O2).** Most register-only
AVX instructions are 4 bytes and `vzeroupper` is 3, so the plain 5-byte jump
left most hot sites trapping (measured: 138 of 314 sites patched, 1.3 million
repeat traps in a short run). The fix: write only the instruction's own bytes
(`E9` plus 2 or 3 displacement bytes) and place the stub so that the remaining
displacement bytes *equal the bytes already in memory after the instruction*.
The next instruction is untouched and remains a valid jump target. This pins
the stub to a 16 MB window (4-byte instruction) or a 64 KB window (3-byte),
which the module maps on demand.

Two consequences:

- The borrowed bytes must never change afterwards. Runs are therefore patched
  back to front (the successor reaches its final form first), and before any
  site is patched the module checks, from memory, whether a short patch just
  before it borrows its first bytes. Reading this from memory rather than
  remembering it keeps it correct across `fork()`.
- The window is usually below the instruction (the next byte is typically
  `C5`, `C4` or, once patched, `E9` — all negative displacements). Non-PIE
  executables at low addresses have no room there. Fixing that needs real
  traces (O3) so the jump can live in any earlier 5-byte instruction.

**Regions (O3/O4), `core/region.c`.** Instead of a linear trace the builder
collects a small control-flow region: from the trapping address it decodes
forward, queues the targets of direct branches (within 4 KB), and lays the
blocks out one after another — at most 96 instructions. Branches whose target
is in the region are linked inside the cache; all others go to an exit stub
that jumps to the original address. Integer instructions are only carried
along if `core/intdec.c` recognises them exactly (a whitelist, checked against
Capstone); anything else ends the block there. `call`, `ret` and indirect jumps
always end a block, so return addresses stay the original ones.

Each AVX instruction of the region gets an entry stub and, if possible, a
patch, as before; several regions may cover the same instruction, the first
one wins.

**Borrowed registers (O5).** If the region has a backward edge, XMM registers
that none of its instructions name are borrowed: two as scratch (so
translations need no spill/restore pair inside the loop), the rest to hold
upper halves, most-used first. Entry stubs save the borrowed registers to the
state block and load the upper halves; exit stubs write them back. Inside,
the translator is told through `struct fa_regmap` and operates on the
registers directly. Helper calls write the affected upper halves back to
memory around the call. Regions without a loop borrow nothing, since the
entry/exit cost would not be recovered.

**Patching integer instructions.** When an AVX instruction cannot be patched
(a short instruction with no usable stub window — the non-PIE case), the first
integer instruction of at least 5 bytes earlier in the same block is patched
to enter the region instead, using the same three-step protocol with `C4` as
the temporary trapping byte.

**CPUID (§9).** Implemented with uprobes: confirmed on 6.18.39 that a uprobe
handler runs in sleepable context and that changing `regs->ip` makes the
kernel skip the probed instruction. Sites are found by the module itself, once
per file, by walking its executable ELF segments with `core/lendec.c` (a length-only decoder;
the kernel's `insn_decode()` is not exported). Its site lists are identical to
objdump's on the binaries tried. A file is walked at the first of two moments:
(1) when it is exec'd: the module registers a `linux_binfmt` at the head of
the format list whose `load_binary` examines the file and returns `-ENOEXEC`,
so the ELF loader runs next. This is the only exported hook that runs in
sleepable context before the new program's first instruction (kprobes and the
exec tracepoint cannot sleep, and `task_work_add` is not exported); uprobes
registered there are installed when the loader maps the file. A file is
answered only if its code contains an instruction the host lacks, otherwise
it is recorded as `clean`. Only main executables: answering `ld.so`'s CPUID
would move every process onto emulated SSE4.2 string functions. (2) when a
handled process traps in a file not yet answered (shared libraries,
`execscan=0`). File entries are keyed by device, inode number and ctime and
pin the inode only once uprobes are attached, so the entry kept for every
program ever run does not keep deleted files alive across package updates.
`/proc/fakeavx/sites` exports the state in
the same line format the `cpuid_site` parameter accepts, which is all the
persistence there is: a systemd unit saves it and writes it back at boot.
AVX is reported only to files seen executing VEX-encoded instructions.

**Policy.** Every process is handled by default (`policy=1`); `FAKEAVX=0` in
the environment opts a program out.

**SSSE3-only hosts.** The helper blob is built without SSE4.1 and contains C
implementations of the SSE4.1 operations. They serve two purposes: the legacy
encodings (which raise #UD on Merom) and, when the host lacks SSE4.1, the
VEX-encoded forms whose templates would have used an SSE4.1 instruction. One
detail found by differential testing: `dpps` sums its products with a
different operand order for each result element, visible only in which NaN
payload survives.

**SSE4.2.** Added because real "no AVX" builds (Bun's baseline build) target
Nehalem. The legacy-encoded `pcmpgtq`, `crc32` and `pcmpXstrX` all raise #UD
on Penryn, so they fit the existing mechanism; they are implemented in the
helper blob.

**Precision of faults.** Stubs are resume-correct but not precise: a faulting
memory operand may leave a destination register or the first half of a 256-bit
store already written.
