#!/usr/bin/env bun
/*
 * fakeavx benchmark, run with Bun:
 *
 *   bun bench/fakeavx-bench.ts [--quick] [--filter NAME,...] [--json FILE]
 *                              [--no-builtins] [--no-kernels]
 *
 * 1. Native kernels (bench/kernels.c, loaded with bun:ffi), each built three
 *    times: core2 (native on a Core 2: the reference), sse42 (POPCNT, CRC32,
 *    PCMPGTQ, PCMPISTRM: translated by fakeavx) and avx2 (256-bit code:
 *    auto-vectorized loops, gathers, masked loads, permutes, variable shifts).
 *    For every kernel: time per element in each build, speed relative to the
 *    native build, the first call (trap + translation + patching), traps
 *    while measuring, and a bit-exact check of the result against core2.
 *
 * 2. Bun's own native code (crc32, hashing, SHA, UTF-8, base64, JSON, gzip,
 *    string search, sort), in child processes with FAKEAVX=sse4 (CPUID never
 *    reports AVX: SSE code paths) and FAKEAVX=avx2 (AVX/AVX2 reported: the
 *    libraries' AVX2 paths, translated). Results must be identical across
 *    modes; times show what the AVX2 paths cost or gain under translation.
 *
 * Needs gcc (the kernels are compiled on first use into bench/build/), and
 * the fakeavx module loaded on a CPU without AVX2 (on a CPU with AVX2 it
 * measures the native instructions).
 */
import { dlopen, FFIType, ptr } from "bun:ffi";
import { existsSync, mkdirSync, readFileSync, realpathSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const opt = {
  quick: args.includes("--quick"),
  // separate processes per build: helper calls write to the stack, whose
  // ASLR-random page offset can alias with a kernel's data (4K aliasing on
  // Core 2), so a single process can be far off in either direction
  runs: args.includes("--runs") ? Number(args[args.indexOf("--runs") + 1]) : args.includes("--quick") ? 1 : 3,
  json: args.includes("--json") ? args[args.indexOf("--json") + 1] : undefined,
  filter: args.includes("--filter") ? args[args.indexOf("--filter") + 1].split(",") : undefined,
  kernels: !args.includes("--no-kernels"),
  builtins: !args.includes("--no-builtins"),
  child: args.includes("--builtins-child"), // internal: run built-ins, print JSON
  // internal: run one build's kernels, one JSON line per kernel
  kchild: args.includes("--kernel-child") ? args[args.indexOf("--kernel-child") + 1] : undefined,
  skip: args.includes("--skip") ? args[args.indexOf("--skip") + 1].split(",") : [],
};
const match = (name: string) => !opt.filter || opt.filter.some((f) => name.includes(f));
const SAMPLE_MS = opt.quick ? 8 : 25;
const REPS = opt.quick ? 3 : 7;

// ---------------------------------------------------------------- utilities

const median = (v: number[]) => {
  const s = [...v].sort((a, b) => a - b);
  return s.length & 1 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
};

/* time fn: calibrate calls per sample to SAMPLE_MS, REPS samples, median ns per call */
function measure(fn: () => unknown): { ns: number; spread: number; calls: number } {
  let calls = 1;
  for (;;) {
    const t0 = Bun.nanoseconds();
    for (let i = 0; i < calls; i++) fn();
    const dt = (Bun.nanoseconds() - t0) / 1e6;
    if (dt >= SAMPLE_MS || calls >= 1 << 24) break;
    calls = dt < 0.5 ? calls * 8 : Math.max(calls + 1, Math.ceil((calls * SAMPLE_MS) / dt));
  }
  const per: number[] = [];
  for (let r = 0; r < REPS; r++) {
    const t0 = Bun.nanoseconds();
    for (let i = 0; i < calls; i++) fn();
    per.push((Bun.nanoseconds() - t0) / calls);
  }
  const m = median(per);
  return { ns: m, spread: (Math.max(...per) - Math.min(...per)) / m, calls };
}

const MOD = "/sys/module/fakeavx/parameters";
const moduleLoaded = existsSync(MOD);
const STATS = ["slow", "fast", "translated", "regions", "regions_borrowing", "patched", "unsupported", "unpatched"];
function stats(): Record<string, number> {
  const r: Record<string, number> = {};
  for (const s of STATS) {
    try {
      r[s] = Number(readFileSync(`${MOD}/stat_${s}`, "utf8"));
    } catch {
      r[s] = 0;
    }
  }
  return r;
}
const delta = (a: Record<string, number>, b: Record<string, number>) =>
  Object.fromEntries(STATS.map((s) => [s, b[s] - a[s]]));

function cpuInfo() {
  const txt = readFileSync("/proc/cpuinfo", "utf8");
  const model = /model name\s*:\s*(.*)/.exec(txt)?.[1] ?? "?";
  const flags = new Set((/^flags\s*:\s*(.*)$/m.exec(txt)?.[1] ?? "").split(" "));
  return { model, flags };
}

/* deterministic data */
function rng(seed: number) {
  let x = BigInt(seed) | 1n;
  return () => {
    x ^= (x << 13n) & 0xffffffffffffffffn;
    x ^= x >> 7n;
    x ^= (x << 17n) & 0xffffffffffffffffn;
    return Number(x & 0xffffffffn) >>> 0;
  };
}
function fillU32(a: Uint32Array, seed: number) {
  const r = rng(seed);
  for (let i = 0; i < a.length; i++) a[i] = r();
  return a;
}

const pad = (s: unknown, n: number) => String(s).padEnd(n);
const lpad = (s: unknown, n: number) => String(s).padStart(n);
const fmtNs = (ns: number) => (ns >= 100 ? ns.toFixed(0) : ns >= 10 ? ns.toFixed(1) : ns.toFixed(2));

// ------------------------------------------------------------ built-ins (child)

type BuiltinResult = { name: string; unit: string; size: number; ns: number; spread: number; digest: string;
  procSpread?: number };

function builtins(): BuiltinResult[] {
  const out: BuiltinResult[] = [];
  const MB = opt.quick ? 1 << 18 : 1 << 20;
  const bytes = new Uint8Array(MB);
  fillU32(new Uint32Array(bytes.buffer), 7);
  const ascii = Buffer.alloc(MB);
  for (let i = 0; i < MB; i++) ascii[i] = 97 + (bytes[i] % 26);
  ascii.set(Buffer.from("needle-in-a-haystack"), MB - 64);
  const text = ascii.toString("latin1");
  const mixed = "héllo wörld — ☃ ✓ 日本語 ".repeat(MB / 64);
  const mixedBytes = new TextEncoder().encode(mixed);
  const b64 = Buffer.from(bytes).toString("base64");
  const obj = Array.from({ length: opt.quick ? 2000 : 8000 }, (_, i) => ({
    id: i, name: `item ${i} ✓`, v: i * 1.5, tags: ["a", "b", String(i % 7)], ok: i % 3 === 0,
  }));
  const json = JSON.stringify(obj);
  const gz = Bun.gzipSync(ascii);
  const floats = new Float64Array(opt.quick ? 50000 : 200000);
  { const r = rng(3); for (let i = 0; i < floats.length; i++) floats[i] = r() / 4294967296 - 0.5; }
  const enc = new TextEncoder(), dec = new TextDecoder();
  const h = (v: unknown) => {
    if (typeof v === "string") return Bun.hash(v).toString(16);
    if (v instanceof Uint8Array || v instanceof ArrayBuffer) return Bun.hash(v as Uint8Array).toString(16);
    return String(v);
  };

  const cases: [string, string, number, () => unknown][] = [
    ["Bun.hash.crc32", "MB/s", MB, () => Bun.hash.crc32(bytes)],
    ["Bun.hash (wyhash)", "MB/s", MB, () => Bun.hash(bytes)],
    ["sha256 (CryptoHasher)", "MB/s", MB, () => new Bun.CryptoHasher("sha256").update(bytes).digest("hex")],
    ["sha1 (CryptoHasher)", "MB/s", MB, () => new Bun.CryptoHasher("sha1").update(bytes).digest("hex")],
    ["md5 (CryptoHasher)", "MB/s", MB, () => new Bun.CryptoHasher("md5").update(bytes).digest("hex")],
    ["TextEncoder.encode (UTF-8)", "MB/s", mixedBytes.length, () => enc.encode(mixed)],
    ["TextDecoder.decode (UTF-8)", "MB/s", mixedBytes.length, () => dec.decode(mixedBytes)],
    ["Buffer base64 encode", "MB/s", MB, () => Buffer.from(bytes.buffer).toString("base64")],
    ["Buffer base64 decode", "MB/s", MB, () => Buffer.from(b64, "base64")],
    ["Buffer.indexOf (20-byte needle)", "MB/s", MB, () => ascii.indexOf("needle-in-a-haystack")],
    ["String.indexOf", "MB/s", MB, () => text.indexOf("needle-in-a-haystack")],
    ["JSON.parse", "MB/s", json.length, () => JSON.parse(json).length],
    ["JSON.stringify", "MB/s", json.length, () => JSON.stringify(obj)],
    ["gzipSync (level 6)", "MB/s", MB, () => Bun.gzipSync(ascii)],
    ["gunzipSync", "MB/s", MB, () => Bun.gunzipSync(gz)],
    ["Float64Array.sort", "Melem/s", floats.length, () => floats.slice().sort()[floats.length >> 1]],
    ["JS float math loop", "Melem/s", 1 << 16, () => {
      let s = 0;
      for (let i = 0; i < 1 << 16; i++) s += Math.sqrt(i) * Math.sin(i * 1e-3);
      return s;
    }],
    ["JS Math.clz32 loop", "Melem/s", 1 << 16, () => {
      let s = 0;
      for (let i = 0; i < 1 << 16; i++) s += Math.clz32(i * 2654435761);
      return s;
    }],
  ];
  for (const [name, unit, size, fn] of cases) {
    if (!match(name)) continue;
    const digest = h(fn());
    const m = measure(fn);
    out.push({ name, unit, size, ns: m.ns, spread: m.spread, digest });
  }
  return out;
}


// ------------------------------------------------------------------ kernels

const here = import.meta.dir;
const buildDir = join(here, "build");
const src = join(here, "kernels.c");
const VARIANTS = [
  { name: "core2", flags: ["-march=core2"] },
  { name: "sse42", flags: ["-march=core2", "-msse4.2", "-mpopcnt"] },
  { name: "avx2", flags: ["-march=core2", "-mavx2", "-msse4.2", "-mpopcnt"] },
] as const;
// fakeavx implements none of these; keep the compiler from using them
const NO_EXT = ["-mno-fma", "-mno-f16c", "-mno-bmi", "-mno-bmi2", "-mno-lzcnt", "-mno-avx512f"];

function buildKernels() {
  mkdirSync(buildDir, { recursive: true });
  for (const v of VARIANTS) {
    const so = join(buildDir, `kernels_${v.name}.so`);
    if (existsSync(so) && statSync(so).mtimeMs > statSync(src).mtimeMs) continue;
    const cmd = ["gcc", "-O3", "-fPIC", "-shared", "-fvisibility=hidden", ...v.flags, ...NO_EXT, "-o", so, src];
    const r = Bun.spawnSync(cmd, { stderr: "pipe" });
    if (r.exitCode !== 0) throw new Error(`${cmd.join(" ")}\n${r.stderr.toString()}`);
  }
}

/* children run pinned to the last CPU when taskset is available */
const TASKSET = Bun.which("taskset");
const pin = (cmd: string[]) => {
  const cpus = (readFileSync("/proc/cpuinfo", "utf8").match(/^processor/gm) ?? []).length;
  return TASKSET && cpus > 1 ? [TASKSET, "-c", String(cpus - 1), ...cmd] : cmd;
};

const SIG = { args: [FFIType.ptr, FFIType.ptr, FFIType.ptr, FFIType.u64, FFIType.u64], returns: FFIType.u64 };
const KERNELS = [
  "f32_saxpy", "f64_muladd", "f32_sqrt_div", "f32_select", "f32_matmul64",
  "i32_muladd", "i16_dot", "u8_absdiff_sum", "i32_varshift", "i64_cmpgt_count",
  "u8_count_byte", "u8_pshufb_popcnt", "u64_popcount", "crc32c", "sse42_strset_count",
  "i32_gather_sum", "f64_gather_sum", "i32_maskload_sum", "i32_permute", "u8_first_high",
] as const;
type KName = (typeof KERNELS)[number];
/* the buffer a kernel writes (its result), for those that return 0 */
const OUT: Partial<Record<KName, "a" | "b" | "c">> = {"f32_saxpy": "b", "f64_muladd": "c", "f32_sqrt_div": "c", "f32_select": "c", "f32_matmul64": "c", "i32_muladd": "c", "i32_varshift": "c", "i32_permute": "c"};

/* inputs per kernel: fresh buffers each time (several kernels update their output) */
function inputs(k: KName) {
  const N = opt.quick ? 1 << 14 : 1 << 16; // elements; 64K x 4-8 bytes stays in L2
  const u32 = (seed: number, n = N) => fillU32(new Uint32Array(n), seed);
  const f32 = (seed: number, n = N) => {
    const a = new Float32Array(n), r = rng(seed);
    for (let i = 0; i < n; i++) a[i] = (r() / 4294967296 - 0.5) * 64;
    return a;
  };
  const f64 = (seed: number, n = N) => {
    const a = new Float64Array(n), r = rng(seed);
    for (let i = 0; i < n; i++) a[i] = (r() / 4294967296 - 0.5) * 64;
    return a;
  };
  const bytes = (seed: number, n: number) => new Uint8Array(u32(seed, n / 4).buffer);
  const z = new Uint8Array(16);
  switch (k) {
    case "f32_saxpy": return { a: f32(1), b: f32(2), c: z, n: N, p: 23, elems: N };
    case "f64_muladd": return { a: f64(1), b: f64(2), c: f64(3), n: N, p: 0, elems: N };
    case "f32_sqrt_div": return { a: f32(1), b: f32(2).map(Math.abs), c: new Float32Array(N), n: N, p: 0, elems: N };
    case "f32_select": return { a: f32(1), b: f32(2), c: new Float32Array(N), n: N, p: 0, elems: N };
    case "f32_matmul64": return { a: f32(1, 4096), b: f32(2, 4096), c: new Float32Array(4096), n: 4, p: 0, elems: 4 * 64 * 64 * 64 };
    case "i32_muladd": return { a: u32(1), b: u32(2), c: u32(3), n: N, p: 0, elems: N };
    case "i16_dot": return { a: new Int16Array(u32(1).buffer), b: new Int16Array(u32(2).buffer), c: z, n: 2 * N, p: 0, elems: 2 * N };
    case "u8_absdiff_sum": return { a: bytes(1, 4 * N), b: bytes(2, 4 * N), c: z, n: 4 * N, p: 0, elems: 4 * N };
    case "i32_varshift": return { a: u32(1), b: u32(2), c: new Uint32Array(N), n: N, p: 0, elems: N };
    case "i64_cmpgt_count": return { a: u32(1, 2 * N), b: u32(2, 2 * N), c: z, n: N, p: 0, elems: N };
    case "u8_count_byte": return { a: bytes(1, 4 * N), b: z, c: z, n: 4 * N, p: 0x41, elems: 4 * N };
    case "u8_pshufb_popcnt": return { a: bytes(1, 4 * N), b: z, c: z, n: 4 * N, p: 0, elems: 4 * N };
    case "u64_popcount": return { a: u32(1, 2 * N), b: z, c: z, n: N, p: 0, elems: N };
    case "crc32c": return { a: bytes(1, 4 * N), b: z, c: z, n: 4 * N, p: 0, elems: 4 * N };
    case "sse42_strset_count": {
      const t = bytes(1, 4 * N).map((x) => 32 + (x % 95)); // printable, no zero bytes
      return { a: t, b: new TextEncoder().encode("aeiouAEIOU.,;:!?"), c: z, n: 4 * N, p: 0, elems: 4 * N };
    }
    case "i32_gather_sum": return { a: u32(1, 4096), b: u32(2).map((x) => x & 4095), c: z, n: N, p: 0, elems: N };
    case "f64_gather_sum": return { a: f64(1, 4096), b: u32(2).map((x) => x & 4095), c: z, n: N, p: 0, elems: N };
    case "i32_maskload_sum": return { a: u32(1), b: z, c: z, n: N, p: 5, elems: N };
    case "i32_permute": return { a: u32(1), b: z, c: new Uint32Array(N), n: N, p: 0x01234567 ^ 0x52143670, elems: N };
    case "u8_first_high": return { a: bytes(1, 4 * N).map((x) => x & 0x7f | (x < 4 ? 0x80 : 0)), b: z, c: z, n: 4 * N, p: 0, elems: 4 * N };
  }
}

type KResult = {
  kernel: string; variant: string; nsPerElem: number; spread: number; firstCallUs: number;
  trapsPerCall: number; checksum: string; ok: boolean | null; translated: number;
  procSpread?: number; runs?: number; nsPerElemMedian?: number;
};

/*
 * One build's kernels, in a child process: a kernel that executes an
 * instruction fakeavx does not handle gets SIGILL, which must not end the
 * whole benchmark. Prints {"start":k} before each kernel and its result
 * after, so the parent knows which kernel a dead child was in.
 */
function kernelChild(variant: string) {
  const syms = Object.fromEntries([...KERNELS, "fa_build", "fa_cpuid"].map((k) => [k, SIG]));
  const lib = dlopen(join(buildDir, `kernels_${variant}.so`), syms as any).symbols as any;
  for (const k of KERNELS) {
    if (!match(k) || opt.skip.includes(k)) continue;
    console.log(JSON.stringify({ start: k }));
    const f = lib[k];
    // correctness: fresh inputs, one call (the parent compares with core2)
    const inp = inputs(k)!;
    const s0 = stats();
    const t0 = Bun.nanoseconds();
    const ret = f(ptr(inp.a), ptr(inp.b), ptr(inp.c), inp.n, inp.p) as bigint;
    const firstUs = (Bun.nanoseconds() - t0) / 1e3;
    // kernels that write an array return 0: their result is the array
    const out = OUT[k] ? (inp as any)[OUT[k]!] : undefined;
    const cs = ret.toString(16) + (out ? ":" + Bun.hash(new Uint8Array(out.buffer, out.byteOffset, out.byteLength)).toString(16) : "");
    const s1 = stats();
    // speed: separate buffers (some kernels accumulate into theirs)
    const run = inputs(k)!;
    const pa = ptr(run.a), pb = ptr(run.b), pc = ptr(run.c);
    const call = () => f(pa, pb, pc, run.n, run.p);
    call();
    const s2 = stats();
    const m = measure(call);
    const traps = delta(s2, stats());
    const r: KResult = {
      kernel: k, variant, nsPerElem: m.ns / run.elems, spread: m.spread, firstCallUs: firstUs,
      trapsPerCall: (traps.fast + traps.slow) / (m.calls * REPS), checksum: cs, ok: null,
      translated: delta(s0, s1).translated,
    };
    console.log(JSON.stringify({ result: r }));
  }
  console.log(JSON.stringify({ cpuid: Number(lib.fa_cpuid(null, null, null, 0, 0)) }));
}

type KFail = { kernel: string; variant: string; why: string };

function runKernels(canRun: (v: string) => boolean) {
  const results: KResult[] = [], failed: KFail[] = [];
  const cpuid: Record<string, number> = {};
  for (const v of VARIANTS) {
    if (!canRun(v.name)) continue;
    const killed = new Set<string>();
    for (let run = 0; run < opt.runs; run++) {
    const skip: string[] = [...killed];
    for (;;) {	// restart after a kernel that killed the child
      const r = Bun.spawnSync(pin([process.execPath, import.meta.path, "--kernel-child", v.name,
        ...(opt.quick ? ["--quick"] : []), ...(opt.filter ? ["--filter", opt.filter.join(",")] : []),
        ...(skip.length ? ["--skip", skip.join(",")] : [])]), { stdout: "pipe", stderr: "pipe" });
      let current: string | undefined;
      for (const line of r.stdout.toString().split("\n")) {
        if (!line.startsWith("{")) continue;
        const o = JSON.parse(line);
        if (o.start) current = o.start;
        if (o.result) { results.push(o.result); skip.push(o.result.kernel); current = undefined; }
        if (o.cpuid !== undefined) cpuid[v.name] = o.cpuid;
      }
      if (r.exitCode === 0) break;
      if (!current) {	// died outside a kernel: give up on this build
        failed.push({ kernel: "(startup)", variant: v.name, why: `exit ${r.exitCode} ${r.signalCode ?? ""}` });
        break;
      }
      const msg = /Illegal instruction at address (0x[0-9A-F]+)/i.exec(r.stderr.toString());
      if (!killed.has(current))
        failed.push({ kernel: current, variant: v.name,
          why: (r.signalCode ?? `exit ${r.exitCode}`) + (msg ? ` at ${msg[1]}` : "") });
      killed.add(current);
      skip.push(current);
    }
    }
  }
  const ref = Object.fromEntries(results.filter((x) => x.variant === "core2").map((x) => [x.kernel, x.checksum]));
  for (const x of results) x.ok = ref[x.kernel] === undefined ? null : x.checksum === ref[x.kernel];
  // one entry per kernel and build: median over the processes; every run's
  // result must match (ok), and procSpread = (max - min) / median
  const merged: KResult[] = [];
  for (const k of KERNELS)
    for (const v of VARIANTS) {
      const all = results.filter((x) => x.kernel === k && x.variant === v.name);
      if (!all.length) continue;
      // best over the processes: other load on the machine only ever adds
      // time; the median is kept for the JSON report
      const t = all.map((x) => x.nsPerElem), best = Math.min(...t), m = median(t);
      merged.push({ ...all[0], nsPerElem: best, nsPerElemMedian: m, procSpread: (Math.max(...t) - best) / best,
        runs: all.length,
        ok: all.every((x) => x.ok !== false) ? all[0].ok : false,
        trapsPerCall: median(all.map((x) => x.trapsPerCall)), firstCallUs: median(all.map((x) => x.firstCallUs)) });
    }
  return { results: merged, cpuid, failed, raw: results };
}

// -------------------------------------------------------------------- main

if (opt.child) {
  console.log(JSON.stringify(builtins()));
  process.exit(0);
}
if (opt.kchild) {
  kernelChild(opt.kchild);
  process.exit(0);
}

const { model, flags } = cpuInfo();
const hostAvx2 = flags.has("avx2");
const header = {
  cpu: model,
  features: ["ssse3", "sse4_1", "sse4_2", "popcnt", "avx", "avx2"].filter((f) => flags.has(f)).join(" "),
  bun: `${Bun.version} (${Bun.revision.slice(0, 9)})`,
  fakeavx: moduleLoaded
    ? `loaded (policy=${readFileSync(`${MOD}/policy`, "utf8").trim()} patch=${readFileSync(`${MOD}/patch`, "utf8").trim()})`
    : "not loaded",
  env: process.env.FAKEAVX ?? "(unset)",
  governor: (() => { try { return readFileSync("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "utf8").trim(); } catch { return "?"; } })(),
  load: readFileSync("/proc/loadavg", "utf8").split(" ").slice(0, 3).join(" "),
};
console.log("fakeavx-bench");
for (const [k, v] of Object.entries(header)) console.log(`  ${pad(k, 9)}${v}`);
console.log(`  timing   median of ${REPS} samples x ${SAMPLE_MS} ms${opt.quick ? " (--quick)" : ""}`);
if (!moduleLoaded && !hostAvx2) console.log("  NOTE     no AVX2 and no fakeavx: only the core2 kernels can run");
if (header.governor !== "performance" && header.governor !== "?")
  console.log(`  NOTE     cpufreq governor "${header.governor}": for steadier numbers, ` +
    "sudo cpupower frequency-set -g performance");

const report: any = { header, kernels: null, builtins: null };
let failures = 0;

if (opt.kernels) {
  buildKernels();
  const canRun = (v: string) => v === "core2" || moduleLoaded || (v === "sse42" ? flags.has("sse4_2") : hostAvx2);
  const before = stats();
  const { results, cpuid, failed, raw } = runKernels(canRun);
  const after = stats();
  report.kernels = { results, cpuid, failed, raw, module: delta(before, after) };
  failures += failed.length;

  console.log(`\n1. Native kernels via bun:ffi (ns per element, best of ${opt.runs} process(es) per build;` +
    " x = time / native core2 build)");
  console.log(`   ${pad("kernel", 20)}${lpad("core2", 8)}${lpad("sse42", 9)}${lpad("x", 6)}${lpad("avx2", 9)}${lpad("x", 6)}${lpad("1st call", 10)}${lpad("traps", 7)}${lpad("spread", 8)}  check`);
  for (const k of KERNELS) {
    const r = (v: string) => results.find((x) => x.kernel === k && x.variant === v);
    const c = r("core2");
    if (!c) continue;
    const cell = (x?: KResult) => x ? lpad(fmtNs(x.nsPerElem), 9) + lpad((x.nsPerElem / c.nsPerElem).toFixed(2), 6) : lpad("-", 9) + lpad("", 6);
    const tr = Math.max(r("sse42")?.trapsPerCall ?? 0, r("avx2")?.trapsPerCall ?? 0);
    const first = Math.max(r("sse42")?.firstCallUs ?? 0, r("avx2")?.firstCallUs ?? 0);
    const bad = results.filter((x) => x.kernel === k && x.ok === false);
    const died = failed.filter((x) => x.kernel === k);
    failures += bad.length;
    console.log(`   ${pad(k, 20)}${lpad(fmtNs(c.nsPerElem), 8)}${cell(r("sse42"))}${cell(r("avx2"))}` +
      `${lpad(first.toFixed(0) + " us", 10)}${lpad(tr >= 0.05 ? tr.toFixed(1) : "0", 7)}` +
      `${lpad(Math.round(100 * Math.max(...results.filter((x) => x.kernel === k).map((x) => x.procSpread ?? 0))) + "%", 8)}  ` +
      (died.length ? `KILLED (${died.map((x) => `${x.variant}: ${x.why}`).join("; ")})`
        : bad.length ? `FAIL (${bad.map((x) => x.variant).join(",")})` : "ok"));
  }
  const bits = (b: number) => ["sse4.2", "popcnt", "avx", "avx2", "osxsave"].filter((_, i) => b >> i & 1).join(" ") || "-";
  console.log("   CPUID as seen by each kernel file (answered by fakeavx once the file has trapped):");
  for (const [v, b] of Object.entries(cpuid)) console.log(`     ${pad(v, 6)} ${bits(b)}`);
  const d = report.kernels.module;
  console.log(`   module, whole section (all processes): translated +${d.translated}, regions +${d.regions}` +
    ` (+${d.regions_borrowing} borrowing), patched +${d.patched}, slow traps +${d.slow}, fast traps +${d.fast},` +
    ` unsupported +${d.unsupported}`);
  console.log("   traps = traps per kernel call while measuring (global counter: background processes add a little)");
  console.log("   spread = (slowest - fastest process) / fastest, worst of the three builds");
}

if (opt.builtins) {
  const modes = moduleLoaded ? ["sse4", "avx2"] : ["(native)"];
  const runs: Record<string, BuiltinResult[]> = {};
  for (const mode of modes) {
    const env = { ...process.env, ...(moduleLoaded ? { FAKEAVX: mode } : {}) };
    const childArgs = [process.execPath, import.meta.path, "--builtins-child", ...(opt.quick ? ["--quick"] : []),
      ...(opt.filter ? ["--filter", opt.filter.join(",")] : [])];
    const s0 = stats();
    const all: BuiltinResult[][] = [];
    for (let run = 0; run < opt.runs; run++) {
      const r = Bun.spawnSync(pin(childArgs), { env, stdout: "pipe", stderr: "pipe" });
      if (r.exitCode !== 0) {
        console.log(`\n   built-ins with FAKEAVX=${mode}: child failed (exit ${r.exitCode}, signal ${r.signalCode})\n${r.stderr}`);
        failures++;
        break;
      }
      all.push(JSON.parse(r.stdout.toString().trim().split("\n").pop()!));
    }
    const s1 = stats();
    if (!all.length) continue;
    // best time over the processes; the digest must be the same in every run
    runs[mode] = all[0].map((x) => {
      const same = all.map((a) => a.find((y) => y.name === x.name)!);
      const t = same.map((y) => y.ns);
      return { ...x, ns: Math.min(...t), procSpread: (Math.max(...t) - Math.min(...t)) / Math.min(...t),
        digest: same.every((y) => y.digest === x.digest) ? x.digest : "differs between runs" };
    });
    (runs[mode] as any).module = delta(s0, s1);
  }
  report.builtins = runs;
  console.log(`\n2. Bun built-ins (throughput, best of ${opt.runs} process(es) per mode; FAKEAVX=${modes.join(" / ")})`);
  const ms = Object.keys(runs);
  console.log(`   ${pad("operation", 34)}${ms.map((m) => lpad(m, 12)).join("")}${ms.length > 1 ? lpad("avx2/sse4", 11) : ""}${lpad("spread", 8)}  same  unit`);
  const names = runs[ms[0]]?.map((x) => x.name) ?? [];
  for (const name of names) {
    const row = ms.map((m) => runs[m].find((x) => x.name === name));
    const tp = (x?: BuiltinResult) => x ? (x.unit === "MB/s" ? x.size / x.ns * 1e3 : x.size / x.ns * 1e3) : NaN;
    const same = row.every((x) => x && x.digest === row[0]!.digest);
    if (!same) failures++;
    const ratio = ms.length > 1 && row[0] && row[1] ? (tp(row[1]) / tp(row[0])).toFixed(2) : "";
    const spread = Math.max(...row.map((x: any) => x?.procSpread ?? 0));
    console.log(`   ${pad(name, 34)}${row.map((x) => lpad(x ? tp(x).toFixed(1) : "-", 12)).join("")}` +
      `${ms.length > 1 ? lpad(ratio, 11) : ""}${lpad(Math.round(100 * spread) + "%", 8)}  ${same ? "yes " : "NO  "}  ${row[0]?.unit}`);
  }
  for (const m of ms) {
    const d = (runs[m] as any).module;
    console.log(`   module during FAKEAVX=${m}: translated +${d.translated}, slow traps +${d.slow}, fast traps +${d.fast}, unsupported +${d.unsupported}, unpatched +${d.unpatched}`);
  }
  console.log("   avx2/sse4 > 1: the AVX2 code path (translated) is faster than the SSE path");
  // did Bun take its AVX2 paths at all? fakeavx lists files seen executing VEX code
  if (moduleLoaded) {
    let sites = "";
    try { sites = readFileSync("/proc/fakeavx/sites", "utf8"); } catch {}
    const bun = realpathSync(process.execPath);
    const ran = sites.split("\n").includes(`avx ${bun}`);
    const xsave = /\bxsave\b/.test(readFileSync("/proc/cpuinfo", "utf8"));
    report.bunRanAvx = ran;
    console.log(ran ? "   Bun executed AVX code under FAKEAVX=avx2: the avx2 column measures translated AVX2 paths"
      : "   Bun never executed AVX code (no \"avx\" entry for it in /proc/fakeavx/sites): both columns are its" +
        " SSE paths." + (xsave ? "\n   Likely cause: XSAVE is enabled, so XGETBV is not intercepted and reports no" +
          " YMM state;\n   libraries that check it (simdutf, BoringSSL) stay on SSE. Boot with \"noxsave\" to" +
          " measure their AVX2 paths." : ""));
  }
}

console.log(`\n${failures ? `${failures} CHECK FAILURE(S)` : "all results identical across builds and modes"}`);
if (opt.json) writeFileSync(opt.json, JSON.stringify(report, null, 2));
process.exit(failures ? 1 : 0);
