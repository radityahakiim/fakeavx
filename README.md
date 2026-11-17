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
| `helpers/` | User-mode helper blob (gathers, variable shifts, permutes, masked moves, POPCNT). Mapped into each process. |
| `kmod/` | The kernel module. |
| `tools/fakeavx-cpuid` | Makes CPUID report the emulated features to one program. |
| `tools/fakeavx-census` | Reports which instruction-set extensions a binary uses. |
| `tests/difftest.c` | Differential test: every instruction form, native AVX vs. translation. Needs an AVX2 machine. |
| `tests/regtest.c` | Region test: whole test functions run as translated regions vs. a C reference. Needs an AVX2 machine. |
| `tests/intdec_check.py` | Checks the integer-instruction decoder against Capstone. |
| `tests/fuzz.c` | ASan/UBSan fuzz of decoders, translator and region builder. |
| `tests/guest/` | End-to-end test program to run on the AVX-less machine. |
| `tests/qemu/run.sh` | Boots a kernel in QEMU with a Penryn CPU model and runs the end-to-end test. |

## Build and install (on the T61)

Requirements: kernel headers for the running kernel (`linux-lts-headers`),
gcc, make, binutils. The kernel needs `CONFIG_KPROBES` and
`CONFIG_MMU_NOTIFIER` (both set in Arch kernels).

```sh
make -C helpers          # helper blob -> helpers/blob.h
make -C kmod             # -> kmod/fakeavx.ko
sudo insmod kmod/fakeavx.ko
```

Then opt a program in through its environment (inherited by its children):

```sh
FAKEAVX=1 ./program-built-with-avx2
```

Module parameters (`insmod fakeavx.ko name=value`, or
`/sys/module/fakeavx/parameters/`):

| Parameter | Default | Meaning |
|---|---|---|
| `policy` | 0 | 0: only processes with `FAKEAVX=1` in their environment. 1: every process. |
| `patch` | 1 | Overwrite translated instructions with a jump. Set to 0 to trap on every execution (slow; for debugging). |
| `stat_*` | — | Read-only counters: `stat_slow` (first-time traps), `stat_fast` (repeat traps), `stat_translated`, `stat_regions`, `stat_regions_borrowing`, `stat_patched`, `stat_patched_short`, `stat_patched_int`, `stat_cpuid`, `stat_unsupported`. |
| `cpuid_site`, `cpuid_clear` | — | Write-only; used by `tools/fakeavx-cpuid`. |

`stat_fast` is the number to watch: every count is a full kernel round trip.
If it keeps climbing while a program runs, that program has hot instructions
that could not be patched (see limitations).

## CPUID

Many programs ask CPUID before using an instruction set, and CPUID cannot be
trapped on a Core 2. `tools/fakeavx-cpuid` finds the `cpuid` instructions of
one binary (with `objdump`; cached under `~/.cache/fakeavx`) and registers
them with the module, which then answers in the CPU's place for opted-in
processes running that binary:

```sh
tools/fakeavx-cpuid add /path/to/program     # once per module load; needs gawk, binutils
FAKEAVX=1    /path/to/program                # CPUID reports SSE4.2 + POPCNT
FAKEAVX=avx2 /path/to/program                # ... and AVX, AVX2, OSXSAVE
```

Use `FAKEAVX=1` unless the program really refuses to run without AVX: a
program that can choose is faster on its own SSE code than on translated AVX,
and AVX2 code paths in the wild usually also use BMI/FMA/LZCNT, which are not
emulated. Only the registered binary is affected; shared libraries (glibc
included) keep seeing the real CPUID unless you register them too. The kernel
needs `CONFIG_UPROBES`.

### Bun

Bun's regular x64 build targets Haswell (AVX2 + BMI + LZCNT) and cannot work:
`lzcnt`/`tzcnt` silently execute as different instructions on a Core 2. Use a
build that needs only SSE4.2 (`bun-linux-x64-baseline`; the 1.4.4 release
binary examined during development is of that kind: SSE4.2 and POPCNT in its
main code, AVX only behind run-time checks). It checks CPUID at start-up and
exits with "this CPU is missing SSE4.2 support" otherwise, so:

```sh
tools/fakeavx-cpuid add "$(readlink -f "$(command -v bun)")"
FAKEAVX=1 bun --version
```

The same goes for programs compiled with Bun into a single executable, such
as Claude Code: register the program's own binary
(`tools/fakeavx-cpuid add "$(readlink -f "$(command -v claude)")"`). Claude
Code 2.1.326 for linux-x64 was examined statically and has the same profile:
29 million instructions, SSE4.2 and POPCNT in the main code, AVX and BMI only
in run-time selected code.

`tools/fakeavx-census FILE` prints such a profile for any binary, to see in
advance whether fakeavx can run it.

This has **not been tested with Bun itself** beyond seeing that start-up check
fail without CPUID interception. Things to expect: JIT-compiled JavaScript
that uses POPCNT or SSE4.2 traps on every execution (JIT memory is never
patched), and Bun is a non-PIE executable, so check `stat_fast` and
`dmesg | grep fakeavx` (unsupported instructions are logged with their bytes).

To build the end-to-end test and run it on the machine itself:

```sh
make -C tests/guest
FAKEAVX=1 tests/guest/guest_test
```

## What is implemented

- AVX and AVX2: all 128/256-bit moves, arithmetic, logic, compares (all 32
  predicates), shuffles, blends, conversions, shifts, broadcasts, permutes,
  insert/extract, masked moves, gathers, `vptest`/`vtestps`, `vzeroupper` —
  about 330 instruction forms, see `core/optable.c`.
- POPCNT (16/32/64-bit, exact flags).
- SSE4.2 in its legacy encoding: `pcmpgtq`, `crc32`, `pcmpestri`, `pcmpestrm`,
  `pcmpistri`, `pcmpistrm` (all control-byte variants). These go through the
  out-of-line helper, so they are correct but not fast.
- CPUID interception for chosen binaries (see below).
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

- **Non-PIE executables** cannot have their short (3–4 byte) instructions
  patched: the stub would have to sit about 0.4–1 GB below the instruction,
  which for an executable loaded at 0x400000 does not exist. Regions mostly
  hide this — the loop runs in the cache, and where possible an earlier
  integer instruction is patched instead — but an unpatchable instruction at
  the head of a frequently called function still traps once per call. Watch
  `stat_fast`; building with `-fPIE -pie` or `-static-pie` avoids it.
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
  traps on every execution of an AVX instruction.
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
| CPUID interception + SSE4.2 (`tests/guest/cpuid_test`, both modes) | QEMU Penryn VM | pass |
| CPUID interception + SSE4.2 | ThinkPad T61 | **not yet run** |
| Bun | — | **not run** |
