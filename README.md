# fakeavx

Run AVX / AVX2 / SSE4.2 / POPCNT binaries on a CPU that has none of them (target:
Penryn-class Core 2, e.g. ThinkPad T61 with a T9xxx/P8xxx/P9xxx), as a Linux
kernel module. Design rationale is in [DESIGN.md](DESIGN.md).

How it works in one paragraph: an unsupported instruction raises #UD. The
module catches that, translates the instruction **once** into SSE4.1-only code
in a per-process code cache, and resumes the program there. It then overwrites
the original instruction with a `jmp` to the translation, so that site never
traps again.

Translation works on **regions**: starting from the trapping instruction the
module follows direct branches and translates the surrounding code as a unit,
carrying the integer instructions in between along. A loop therefore runs
entirely inside the code cache. If the region contains a loop, XMM registers
its code never mentions are borrowed for its duration to hold the upper halves
of the YMM registers it does use, so a 256-bit operation becomes two
register-to-register SSE operations instead of a load, an operation and a
store. The kernel never emulates anything itself — all translated code
runs in user mode at native speed, and code that does not use AVX is not
touched at all.

## Layout

| Path | What |
|---|---|
| `core/` | Decoders, instruction table, AVX→SSE translator and region builder. Plain C, no OS dependencies. |
| `helpers/` | User-mode helper blob (gathers, masked moves, `pcmpXstrX`, `crc32`, constants for inline sequences). Mapped into each process. |
| `kmod/` | The kernel module. |
| `tools/fakeavx-cpuid` | Lists, saves and restores the known `cpuid` sites; manual registration. |
| `scripts/` | Permanent installation: `install.sh`, `uninstall.sh`, systemd units. |
| `tools/fakeavx-census` | Reports which instruction-set extensions a binary uses. |
| `tools/prof/` | `fakeavx-prof`: per-instruction profiler and bit-exact verifier (below). |
| `tests/difftest.c` | Differential test: every instruction form, native AVX vs. translation. Needs an AVX2 machine. |
| `tests/regtest.c` | Region test: whole test functions run as translated regions vs. a C reference. Needs an AVX2 machine. |
| `tests/intdec_check.py` | Checks the integer-instruction decoder against Capstone. |
| `tests/ssedec_check.py` | Checks the legacy SSE/MMX decoder (instructions regions copy as they are) against Capstone, including implicit register use. |
| `tests/divert_check.c` | Checks where `fa_find_divert()` decides an instruction starts against objdump's disassembly of a whole executable (`make -C tests divert-check BIN=...`). |
| `tests/scan_check.c` | The module's `cpuid` scan against a full instruction-by-instruction walk of whole ELF files (`make -C tests scan-check FILES=...`; part of `make check`). |
| `tests/fuzz.c` | ASan/UBSan fuzz of decoders, translator and region builder. |
| `tests/guest/` | End-to-end test program to run on the AVX-less machine. |
| `tests/qemu/run.sh` | Boots a kernel in QEMU with a Penryn CPU model and runs the end-to-end test. |

## Supported CPUs

Any x86-64 CPU with SSSE3 and without AVX: Core 2 Merom/Conroe/Kentsfield
(SSSE3 only) and Penryn/Wolfdale/Yorkfield (SSE4.1). On an SSSE3-only CPU
SSE4.1 is emulated as well, in C, so programs that lean on SSE4.1 run but
slowly; on Penryn SSE4.1 runs natively.

## Install

Requirements: kernel headers for the running kernel (`linux-lts-headers` on
Arch), gcc, make, binutils; `dkms` recommended. The kernel needs
`CONFIG_KPROBES`, `CONFIG_MMU_NOTIFIER` and `CONFIG_UPROBES` (all set in Arch
kernels).

Kernel versions: the module builds on Linux 6.1 to 7.4. `kmod/compat.h`
holds the API differences (the CPUID header, the MSR accessor names, the
uprobe registration and handler signatures), and
`tests/kernel-compat.sh` (`make kernel-compat`) compile-tests against
kernel.org sources of 6.1, 6.6, 6.12, 6.18, 7.2, 7.3 and 7.4-rc3 by default
(any versions can be given). Compiling is what that checks; the module has
been run on 6.18.

```sh
make                # helper blob + kernel module
sudo make install   # permanent: loaded at every boot
```

`make install` (`scripts/install.sh`) sets up:

- the module, through DKMS if it is installed (rebuilt automatically for new
  kernels), otherwise copied into `/lib/modules/$(uname -r)/updates` (re-run
  after a kernel update);
- `/etc/modules-load.d/fakeavx.conf` to load it at boot, and
  `/etc/modprobe.d/fakeavx.conf` for options;
- `fakeavx-cpuid` and `fakeavx-census` in `/usr/local/bin`;
- `fakeavx.service`, which restores the list of known `cpuid` sites at boot and
  saves it at shutdown, and `fakeavx-save.timer`, which also saves it hourly.
  The list lives in `/var/cache/fakeavx/sites`.

`sudo make uninstall` removes it all (`scripts/uninstall.sh --purge` also
deletes the cache and the options file). To try the module without installing:
`make load` / `make unload`.

## Use

Nothing to do: every process is handled. When a program executes an
instruction the CPU does not have, it is translated instead of killed.

Per program, through its environment:

| | |
|---|---|
| `FAKEAVX=0` | leave this program alone (it gets SIGILL as on a bare system) |
| `FAKEAVX=sse4` | never let CPUID report AVX to it |
| `FAKEAVX=avx2` | always let CPUID report AVX and AVX2 to it |

Module parameters (`/etc/modprobe.d/fakeavx.conf`, or
`/sys/module/fakeavx/parameters/`):

| Parameter | Default | Meaning |
|---|---|---|
| `policy` | 1 | 1: every process unless it has `FAKEAVX=0`. 0: only processes started with `FAKEAVX=1`. |
| `patch` | 1 | Overwrite translated instructions with a jump. 0 traps on every execution (slow; for debugging). |
| `divert` | 1 | When an instruction that could not be patched keeps trapping, patch the instruction before it instead (below). |
| `warm` | 1 | Translate all of a file's known sites at once, the first time a process traps in it (below). |
| `lazy` | 1 | Give the other AVX instructions of a region an entry only once they are entered directly (each such entry costs a trap then); 0 gives all of them one up front. |
| `evict` | 1 | When a 3-4 byte instruction's jump window is unusable, rewrite the first byte(s) of the next instruction (one the same region translates or carries) to move the window (below). |
| `autocpuid` | 1 | Find a binary's `cpuid` instructions automatically (below). |
| `stat_*` | — | Read-only counters, e.g. `stat_slow` (first-time traps), `stat_fast` (repeat traps), `stat_regions`, `stat_patched`, `stat_cpuid`, `stat_scans`, `stat_unsupported`, `stat_short_refused` (3-4 byte instructions left trapping because their stub window was unavailable), `stat_unpatched` (all instructions left trapping, for any reason), `stat_diverted` (of those, how many were then diverted at the instruction before), `stat_evicted` (short instructions patched by rewriting the next one; `stat_evicted_int`: of them, where the next one is an integer instruction), `stat_evict_traps` (direct jumps into such a rewritten instruction, each a trap), `stat_evict_undone` (evictions undone after 16 such jumps: integer instructions given their bytes back, and AVX instructions their own jump), `stat_lazy` (AVX instructions in regions left without an entry), `stat_warmed` (sites translated by warm-up), `stat_warm_stale` (warm-up sites dropped because their file changed), and where first executions spend their time: `stat_ns_slow` (all of the slow path), `stat_ns_scan` (looking for `cpuid` instructions, part of it), `stat_ns_warm`, `stat_ns_build` (building translations), `stat_ns_map`, `stat_ns_poke` (`stat_pokes` writes), `stat_ns_sync` (`stat_syncs` IPI rounds; `stat_syncs_skipped`: not needed, single-threaded process). |

`stat_fast` is the number to watch: every count is a full kernel round trip.
If it keeps climbing while a program runs, that program has hot instructions
that could not be patched (see limitations). Each instruction that should
have been patched but was not is logged with the program, file and offset,
its bytes and the reason (`dmesg | grep fakeavx`, rate limited), and the last
64 are kept in `/proc/fakeavx/unpatched`, with the number of times each has
trapped since, the stub window and the heap position where that matters.
Unsupported instructions are logged with their bytes too.

Once such an instruction has trapped 16 times, fakeavx tries to patch the
instruction before it instead: if that one is at least 5 bytes long and leads
straight into it, a jump there enters a translated copy that runs both, so
the traps stop for every path through it. The line in
`/proc/fakeavx/unpatched` then ends in `diverted N bytes before it`, or says
why not. This needs to know for certain where the previous instruction
starts, which x86 code does not say; fakeavx decodes forward from the three
nearest function starts (recognised by the int3 padding LLVM puts between
functions) and uses the answer only if all three agree. Checked against
objdump over Claude Code, Bun and Node: 23,670 answers, none wrong. In
Claude Code 2.1.326, 8 of the 10 instructions reported on the T61 qualify;
the other two follow a `call` or start a block, and still trap.

**Warm-up.** Every instruction a first trap translates is remembered with its
file and offset, and `fakeavx-cpuid save` (run by `fakeavx-save.timer` and at
shutdown) keeps the list in `/var/cache/fakeavx/sites` with the cpuid sites.
After a reboot it is restored, and the first time a process traps in such a
file, fakeavx translates and patches all of that file's sites at once instead
of one trap at a time (`fakeavx-cpuid list` shows how many each file has).
That usually happens before the program starts threads, and patching a
single-threaded process needs none of the cross-CPU synchronization (IPIs)
that each patch step otherwise waits for. Only offsets are stored, never
code, so a damaged or forged cache can cost time but cannot put code into a
process; the module also drops a file's list if the file is not exactly as
it was (inode, change time, size), since an offset that is no longer an
instruction start must never be patched. Measured in QEMU with
`fakeavx-prof` (960 sites): slow path 327 ms -> 152 ms, first-execution
traps 2,288 -> 1,329 (the rest are `xgetbv`, emulated on every execution by
design); emulator timings, so only the proportions mean anything.

## CPUID

Many programs ask CPUID before using an instruction set, and CPUID cannot be
trapped on a Core 2. The module therefore puts a probe on the `cpuid`
instructions of the binaries that matter and answers in the CPU's place.

This is automatic. The first time a binary executes an unsupported
instruction, the module looks through that file's code for its `cpuid`
instructions (once per file; 31 ms for Claude Code's 113 MB of code in a VM on a
recent CPU, more on a Core 2) and from then on
answers them — for every process running that file, now and later. With the
permanent installation the list survives reboots; a file that has changed
since (an update) is simply scanned again.

What CPUID is made to report:

- SSE4.1, SSE4.2 and POPCNT: always.
- AVX, AVX2 and OSXSAVE: only to a binary that has been seen executing AVX
  instructions (it uses AVX whatever CPUID says), or with `FAKEAVX=avx2`. A
  program that can choose is faster on its own SSE code than on translated
  AVX, and AVX2 code paths in the wild usually also use BMI/FMA/LZCNT, which
  are not emulated. AVX that a `FAKEAVX=avx2` process executes does not mark
  its file: that process was told AVX is there. (Before this rule, one such
  run of Claude Code marked it for good, and from then on its JavaScript JIT
  wrote AVX code, which traps on every execution: about 300,000 traps per
  start in a VM. `fakeavx-cpuid list` shows a mark as "reports AVX";
  `fakeavx-cpuid clear` forgets it along with everything else, which the
  module then learns again.)

Limits of the automatic way:

- A program that asks CPUID *before* executing anything unsupported, and gives
  up, fails on its very first run and works from the second. To avoid even
  that: `fakeavx-cpuid add /path/to/program`.
- A program that checks for AVX and refuses to run without it never executes
  an AVX instruction, so it is never recognised as an AVX program: start it
  with `FAKEAVX=avx2`, or mark it once with `fakeavx-cpuid avx /path/to/program`.
- Only the binary in which the instruction was hit is covered, not the shared
  libraries it loads (each is handled when one of *its* instructions traps).

`fakeavx-cpuid list` shows what is known; see `fakeavx-cpuid` without
arguments for the rest. `fakeavx-census FILE` reports which instruction-set
extensions a binary uses, to see in advance whether fakeavx can run it.

### Bun and Claude Code

Reported working on a ThinkPad T61 (P8600): Bun, and Claude Code (a
Bun-compiled executable). These are built for Nehalem — SSE4.2 and POPCNT in
the main code, AVX only in code selected at run time — which is what the
SSE4.2 emulation and the CPUID answers are for. A build that targets Haswell
throughout cannot work: `lzcnt`/`tzcnt` silently execute as different
instructions on a Core 2.

Things to expect: JIT-compiled JavaScript that uses POPCNT or SSE4.2 traps on
every execution (JIT memory is never patched), and these are non-PIE
executables, so keep an eye on `stat_fast`.

## Test programs

```sh
make guest
tests/guest/guest_test      # AVX2 + POPCNT workload, threads, fork
tests/guest/cpuid_test      # CPUID answers + SSE4.2 (run it twice the first time)
```

## Profiling: which instructions are slow

`fakeavx-prof` measures every supported instruction form (about 200, in
`tools/prof/prof_tests.h`) the way a hot loop in a real program runs it, and
shows what fakeavx turned it into, so that the expensive ones can be found
and reduced.

```sh
make prof
tools/prof/fakeavx-prof -s                     # all tests, slowest first
tools/prof/fakeavx-prof -v -f gather,vdpps     # details for some tests
tools/prof/fakeavx-prof -n                     # translations only, no timing (any machine)
tools/prof/fakeavx-prof -c > prof.csv          # for a spreadsheet
tools/prof/fakeavx-prof --check tools/prof/golden.txt   # bit-exact check against real AVX2
```

For precise numbers set the performance governor first
(`sudo cpupower frequency-set -g performance`) and close other programs. The
profiler pins itself to one CPU, interleaves every sample with an empty loop,
and reports the median of the differences and its spread (MAD). It reads
core cycles and retired instructions from perf counters when allowed
(`kernel.perf_event_paranoid` ≤ 2), and otherwise uses the TSC.

| Column | Meaning |
|---|---|
| `path` | `native` (this CPU has it), `inline` (translated to SSE), `helper` (calls C code in the helper blob: usually the slow ones), `kernel` (XGETBV, emulated on every execution by design), `TRAPS` (not patched, traps every time: a bug or a limitation worth reporting), `UNSUPP`, `FAULT` |
| `cyc/u`, `ns/u` | cost of one unit (usually one instruction) in a loop |
| `ins/u` | host instructions actually retired per unit, including inside helpers |
| `L:cov` | how much of the loop its region covers; `all` means the loop runs entirely in the code cache. Less than that means legacy SSE (or another instruction a region cannot carry) splits it, and each AVX instruction enters and leaves the cache on its own |
| `L:insns` | host instructions the translation runs per unit inside the loop |
| `first-us` | first execution: trap, translation and patching |
| `traps` | traps while measuring; should be 0 except for `kernel` |
| `check` | result of `--check` |

`-v` adds, per test, the instruction bytes, the decoding, the translation
alone and inside its loop region (registers borrowed, helper calls), the
module's counters for the first call and while measuring, and the timing
details; `-vv` adds every sample and a hex dump of the translation. `-D DIR`
writes the translations to files: disassemble them with
`objdump -D -b binary -m i386:x86-64 DIR/<test>.region.bin`.

The `ref_*` tests are native SSE instructions, for comparison; the `mix_*`
tests put an AVX instruction and a legacy SSE instruction in one loop, as in
programs that are mostly SSE. `--merom` / `--penryn` choose which CPU the
static analysis assumes when it runs on a machine that has AVX.

`tools/prof/golden.txt` holds the results of every test recorded on a CPU
with real AVX2 (`--record`); `--check` compares bit for bit, register by
register, and prints both values when they differ.

## What is implemented

- AVX and AVX2: all 128/256-bit moves, arithmetic, logic, compares (all 32
  predicates), shuffles, blends, conversions, shifts, broadcasts, permutes,
  insert/extract, masked moves, gathers, `vptest`/`vtestps`, `vzeroupper` —
  about 330 instruction forms, see `core/optable.c`.
- POPCNT (16/32/64-bit, exact flags).
- SSE4.2 in its legacy encoding: `pcmpgtq`, `crc32`, `pcmpestri`, `pcmpestrm`,
  `pcmpistri`, `pcmpistrm` (all control-byte variants), and the VEX.128 forms
  `vpcmpestri`, `vpcmpestrm`, `vpcmpistri`, `vpcmpistrm` that `-mavx`/`-mavx2`
  code uses. These go through the
  out-of-line helper, so they are correct but not fast, and they do not set
  floating-point exception flags in MXCSR.
- SSE4.1 (legacy and VEX encodings) on CPUs that only have SSSE3.
- CPUID answering, with automatic discovery of the `cpuid` instructions.
- `XGETBV` (reports AVX state as enabled) when the kernel is booted with
  `noxsave`.
- Per-thread state for the upper YMM halves, including new threads and `fork`.

## What is not implemented yet

- **FMA, F16C, BMI1/BMI2.** AVX/AVX2/POPCNT do not depend on them, so they were
  left out. Binaries built with `-march=haswell` or `-march=x86-64-v3` use them
  and will get SIGILL at the first such instruction (the module logs the
  bytes). `LZCNT`/`TZCNT` can never be supported by this approach: on old CPUs
  they silently execute as `BSR`/`BSF` instead of trapping.
- **Lazy upper-zero tracking** (DESIGN.md O6) and flag-liveness analysis (O7).
- Regions stop at `call`, `ret`, indirect jumps and any instruction the
  integer decoder does not positively recognise (legacy SSE, x87, string
  compares with unusual prefixes…). Code after such an instruction starts a new
  region when it next traps.
- AES-NI, PCLMUL, SSE4.2 string instructions, AVX-512.
- 32-bit processes.

## Known limitations

- **Non-PIE executables** have trouble with short (3–4 byte) instructions.
  Their jump takes its top displacement byte(s) from the next instruction;
  a VEX prefix (`c4`/`c5`) or any byte ≥ 0x80 there puts the stub 1–2 GB
  below the instruction, which for an executable loaded at a few MB does not
  exist. When the same region translates or carries the next instruction,
  `evict` rewrites its first byte(s) into an opcode that is invalid in
  64-bit mode and that moves the window above the heap: the region runs that
  instruction from the cache, and a jump straight to it traps into its
  record (after `fork()`, the child finds it through a record kept in the
  code cache). After 16 such jumps the eviction is undone where that helps,
  and the short instruction traps again: an integer instruction, which would
  not trap otherwise, gets its bytes back, and so does an AVX instruction
  that had a jump of its own (the eviction took it). An AVX instruction
  without one trapped on direct entry anyway and stays evicted. Otherwise an earlier integer instruction is patched instead
  where possible (`divert`). What is left — e.g. `vzeroupper` before `ret`,
  or an instruction right after a `call` — still traps once per execution.
  Watch `/proc/fakeavx/unpatched`; building with `-fPIE -pie` or
  `-static-pie` avoids it. Bun and Claude Code are such executables
  (loaded at 0x200000).
- **Signal handlers that run AVX code.** While a region runs, borrowed
  registers and upper halves live in per-thread scratch memory. A handler
  that itself executes translated AVX code reuses that memory and corrupts the
  interrupted region. Handlers that do not use AVX are fine.
- A handler inspecting the interrupted context while a region runs sees `rip`
  in the code cache and borrowed XMM registers holding other values.
- Programs that use the `%gs` segment base themselves (64-bit Wine) are
  detected and left alone.
- If a memory operand faults, the faulting `rip` reported to a signal handler
  points into the code cache, and a destination register may already be
  partly written. Resuming after the fault is correct.
- Debuggers show XMM registers only.
- Only read-only file-backed code is patched. JIT-generated code works but
  traps on every execution of an AVX instruction, and is translated one
  instruction at a time (no regions), because a JIT rewrites its code in
  place and only the trapping instruction's bytes are checked.
- Unloading the module makes every process that was using it die with SIGILL.

## Testing

```sh
make -C tests check      # on a machine with real AVX2
```

runs the region test, the differential test (each instruction form executed
natively and through the translation from identical random state, half of the
time with randomly borrowed registers; registers, flags, MXCSR and memory must
match bit for bit) and the fuzz test.

```sh
KERNEL=/path/to/bzImage QEMU=qemu-system-x86_64 tests/qemu/run.sh
```

boots a Penryn virtual machine, loads the module and runs `tests/guest`.
`REPEATS=n` scales the workload, `GUEST=path` runs a different static binary,
and extra arguments are passed to `insmod` (e.g. `./run.sh patch=0`).

## Test status

| Test | Where | Result |
|---|---|---|
| Differential (165,000 instruction instances incl. SSE4.2, plus far-stub mode) | AVX2 development machine | 0 mismatches |
| Region test (6 functions, with and without borrowed registers) | development machine | pass |
| Integer decoder vs. Capstone (685,000 accepted instructions) | development machine | 0 divergences |
| Fuzz of decoders, translator, region builder under ASan + UBSan | development machine | clean |
| End-to-end (threads, fork, XGETBV, rmmod), PIE and non-PIE, `patch=0` | QEMU 10 TCG, `-cpu Penryn`, 4 CPUs, Linux 6.18.39 | pass, no kernel warnings |
| End-to-end, per-instruction version (before regions) | ThinkPad T61, Arch 6.18.39-lts | pass, `stat_fast` 0, 209 ms |
| End-to-end, region version | ThinkPad T61 | pass, `stat_fast` 0, 183 ms, 6 of 11 regions borrowing |
| Differential test in SSSE3-host mode, and legacy SSE4.1 encodings | development machine | 0 mismatches |
| In-kernel `cpuid` scanner vs. objdump (5 binaries incl. Bun, Claude Code) | development machine | identical site lists |
| `cpuid` scan from the candidate bytes (`tests/scan_check.c`) vs. a full walk: 3,781 ELF files of a Linux system, Claude Code 2.1.328-330, Bun, Node; in the module vs. objdump on Claude Code 2.1.330 | development machine; QEMU TCG | identical (also with a 64-byte window); Claude Code 417 → 31 ms in a KVM guest |
| End-to-end on an SSSE3-only CPU model (`-cpu core2duo`) | QEMU | pass |
| Automatic mode, automatic CPUID discovery, cache save/restore | QEMU, both CPU models | pass |
| Bun and Claude Code (version before automatic mode) | ThinkPad T61 P8600 | reported working |
| Short patches no longer depend on where ASLR puts the heap (10 runs, heap 114-913 MB above the program) | QEMU, `-cpu Penryn` | all patched, `stat_short_refused` 0 (before: 6 of 10 trapping) |
| Legacy SSE4.1 inside regions runs natively; differential test with borrowed registers around it | development machine, QEMU | 1.18 M instances per mode, 0 mismatches |
| Unpatched instructions reported with program, file+offset and reason (`tests/guest/unpatched_test`) | QEMU | reported, result correct |
| Per-instruction trap counts in `/proc/fakeavx/unpatched`; diversion at the instruction before (`tests/guest/unpatched_test`, 100,000 calls) | QEMU, both CPU models | diverted after 16 traps, result correct; with `divert=0` 99,999 traps (test fails, as it should) |
| Diversion point vs. objdump (`tests/divert_check.c`): Claude Code 2.1.326, Bun, Node | development machine | 23,670 answers, 0 wrong; with only one function start, 2 wrong (hence three) |
| Legacy SSE/SSE2/SSE3/SSSE3/MMX carried inside regions: decoder vs. Capstone (393,000 accepted, 309,000 from real code incl. Bun and Claude Code); region test `mixsse` with borrowed registers | development machine | 0 divergences; pass (and fails when the register accounting is removed) |
| Inline `vpcmpgtq`, `vptest ymm`, `vtestps/pd`, variable shifts, and a table-driven `crc32` routine instead of the C helper | development machine (1.18 M differential instances, mutation-checked), QEMU both CPU models (`fakeavx-prof --check` 198/198) | 0 mismatches |
| `fakeavx-prof --check`: 198 instruction forms bit-exact against real AVX2 through the module | QEMU, `-cpu Penryn` and `-cpu core2duo` | 198/198 match, none trapping after translation |
| Eviction undone for an AVX instruction that had its own jump (`tests/evict_test`) | QEMU, both CPU models | 16 traps, then 0 in 2000 direct entries (previous module: 2000 and 2000) |
| Floating point only where MXCSR is saved (`helpers/check_fp.sh`, on every blob build) | development machine | passes; fails with either rule mutated |
| Shorter inline `vptest`/`vtestps/pd` (20 -> 13), variable shifts (46 -> 38, 18 -> 14), `vpermd`/`vpermps` (33 -> 25), `vpcmpgtq` (24 -> 22), `vpbroadcastb` (5 -> 3) | development machine (differential test, all modes; previous build's state dumps; a planted bug in each caught), QEMU both CPU models (`fakeavx-prof --check` 198/198, `vpermd` dumps under the module) | 0 mismatches, identical |
| `popcnt` inline (36 -> 23 instructions, no call) and legacy `pcmpgtq` inline (C helper -> 9 instructions) | development machine (differential test all modes incl. 24,000 `popcnt`/SSE4.2 cases, zero inputs for ZF; previous build's state dumps; 9 planted bugs caught), QEMU both CPU models (`fakeavx-prof --check` 198/198, `evict_test`, `unpatched_test`, end-to-end) | 0 mismatches, identical |
| JIT code rewritten in place (`tests/guest/jit_test`: the instruction after an AVX one, a second AVX instruction, a later immediate) | QEMU, both CPU models | pass; the previous module ran the old code in all 3 cases |
| Lazy entries (`lazy=1` vs `0`): `fakeavx-prof` cold and warmed up, Claude Code 2.1.330 cold, a program with 4,096 AVX functions | QEMU (TCG, KVM) | Same traps and regions in `fakeavx-prof` (Claude Code: 52 vs 51); pokes 26,316 → 6,020 (`fakeavx-prof`), 103,142 → 14,067 (4,096 functions); launch 76-82 → 52 ms cold, 46 → 24 ms warmed up |
| Warm-up (`tests/qemu/warm-test.sh`): cold run, same boot, and after save + module reload + restore; lists of changed files dropped; IPIs still issued once a process has threads | QEMU, both CPU models | 198/198 in every run; 960 / 984 sites warmed; wrong identity and touched file dropped; 24 IPI rounds with a second thread, results right |
| `scripts/install.sh` (DKMS, systemd units) | — | **not run**: only the file layout was checked |
| Automatic mode, SSSE3 support | real hardware | **not yet run** |
