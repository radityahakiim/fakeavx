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
| `tools/fakeavx-cpuid` | Lists, saves and restores the known `cpuid` sites; manual registration. |
| `scripts/` | Permanent installation: `install.sh`, `uninstall.sh`, systemd units. |
| `tools/fakeavx-census` | Reports which instruction-set extensions a binary uses. |
| `tests/difftest.c` | Differential test: every instruction form, native AVX vs. translation. Needs an AVX2 machine. |
| `tests/regtest.c` | Region test: whole test functions run as translated regions vs. a C reference. Needs an AVX2 machine. |
| `tests/intdec_check.py` | Checks the integer-instruction decoder against Capstone. |
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
| `autocpuid` | 1 | Find a binary's `cpuid` instructions automatically (below). |
| `execscan` | 1 | Examine every program before it starts (below); 0 waits for its first unsupported instruction instead. |
| `stat_*` | — | Read-only counters, e.g. `stat_slow` (first-time traps), `stat_fast` (repeat traps), `stat_regions`, `stat_patched`, `stat_cpuid`, `stat_scans_exec` / `stat_scans` (files examined at start / after a trap), `stat_unsupported`. |

`stat_fast` is the number to watch: every count is a full kernel round trip.
If it keeps climbing while a program runs, that program has hot instructions
that could not be patched (see limitations). Unsupported instructions are
logged with their bytes: `dmesg | grep fakeavx`.

## CPUID

Many programs ask CPUID before using an instruction set, and CPUID cannot be
trapped on a Core 2. The module therefore puts a probe on the `cpuid`
instructions of the binaries that matter and answers in the CPU's place.

This is automatic. Every program is examined before it executes its first
instruction (the module sits in the kernel's list of executable formats, ahead
of the ELF loader). The first time a given file is started, the module walks
its code once:

- If the code contains instructions this CPU lacks (AVX, POPCNT, SSE4.2,
  SSE4.1 on CPUs without it), its `cpuid` instructions are answered from that
  first run on, for every process running the file.
- If it contains none, the program keeps seeing the real CPU, and the file is
  remembered as not needing anything.

The walk costs a fraction of a second for Bun and about a second for Claude
Code (measured on a recent desktop; expect a few times that on a Core 2), once
per file. With the permanent installation the results survive reboots, so a
program is only examined again when it changes (an update).

Shared libraries are not examined at start. glibc asks CPUID inside `ld.so`
for every program, and answering there would make every program on the
system pick glibc's SSE4.2 string functions, which this CPU would have to
emulate. A library is handled when one of its own instructions traps.

What CPUID is made to report:

- SSE4.1, SSE4.2 and POPCNT: always.
- AVX, AVX2 and OSXSAVE: only to a binary that has been seen executing AVX
  instructions (it uses AVX whatever CPUID says), or with `FAKEAVX=avx2`. A
  program that can choose is faster on its own SSE code than on translated
  AVX, and AVX2 code paths in the wild usually also use BMI/FMA/LZCNT, which
  are not emulated.

Limits of the automatic way:

- A program that contains AVX/SSE4.2/POPCNT code only as an option, chosen at
  run time, is answered too, so it may now pick its SSE4.2/POPCNT paths
  instead of its plain ones. These still run, but SSE4.2 string instructions
  are emulated and slower. `FAKEAVX=0` makes such a program see the real CPU.
- A statically linked program contains glibc's AVX string functions, so it
  always counts as needing CPUID answered (for the reason above).
- A program that checks for AVX and refuses to run without it never executes
  an AVX instruction, so it is never recognised as an AVX program: start it
  with `FAKEAVX=avx2`, or mark it once with `fakeavx-cpuid avx /path/to/program`.
- A shared library that asks CPUID itself, before executing anything
  unsupported, gets the real answer on its first run. `fakeavx-cpuid add
  /path/to/library.so` covers it in advance.

`fakeavx-cpuid list` shows the files whose CPUID is answered (`list -a` also
the ones examined and found not to need it); see `fakeavx-cpuid` without
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
tests/guest/cpuid_test      # CPUID answers + SSE4.2, correct on the first run
tests/guest/cpuid_plain     # a program needing nothing: must see the real CPU
```

## What is implemented

- AVX and AVX2: all 128/256-bit moves, arithmetic, logic, compares (all 32
  predicates), shuffles, blends, conversions, shifts, broadcasts, permutes,
  insert/extract, masked moves, gathers, `vptest`/`vtestps`, `vzeroupper` —
  about 330 instruction forms, see `core/optable.c`.
- POPCNT (16/32/64-bit, exact flags).
- SSE4.2 in its legacy encoding: `pcmpgtq`, `crc32`, `pcmpestri`, `pcmpestrm`,
  `pcmpistri`, `pcmpistrm` (all control-byte variants). These go through the
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
| Differential test in SSSE3-host mode, and legacy SSE4.1 encodings | development machine | 0 mismatches |
| In-kernel `cpuid` scanner vs. objdump (5 binaries incl. Bun, Claude Code) | development machine | identical site lists |
| End-to-end on an SSSE3-only CPU model (`-cpu core2duo`) | QEMU | pass |
| Automatic mode, automatic CPUID discovery, cache save/restore | QEMU, both CPU models | pass |
| CPUID answered on a program's first run; a program needing nothing left alone | QEMU, both CPU models | pass |
| Bun and Claude Code (version before automatic mode) | ThinkPad T61 P8600 | reported working |
| `scripts/install.sh` (DKMS, systemd units) | — | **not run**: only the file layout was checked |
| Automatic mode, SSSE3 support | real hardware | **not yet run** |
