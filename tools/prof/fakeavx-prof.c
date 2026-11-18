// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx-prof - per-instruction profiler and verifier for fakeavx.
 *
 * For each instruction form in prof_tests.h it reports:
 *   - what fakeavx turns it into (static: the translator is linked in):
 *     inline SSE code or a call to a C helper, host instructions and bytes,
 *     both alone and inside a loop region with borrowed registers;
 *   - what it costs at run time on this machine: core cycles and retired
 *     instructions per instruction (perf counters, user mode) and wall time
 *     (TSC, including any kernel time), from many repetitions, as median of
 *     paired differences against an empty loop with its spread;
 *   - the one-time cost of the first execution (trap + translation + patch)
 *     and whether it keeps trapping afterwards (module counters);
 *   - optionally, whether the results are bit-exact against values recorded
 *     on a CPU with real AVX2 (--record there, --check here).
 *
 * Run "fakeavx-prof -h" for the options.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/perf_event.h>
#include <math.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../../core/fakeavx.h"
#include "prof.h"

/* ------------------------------------------------------------------ */
/* the tests                                                           */

struct test {
	const char *name;
	int flags;
	const char *text;
};

static const struct test tests[] = {
	{ "empty", 0, "" },
#define T(n, f, ...) { #n, f, #__VA_ARGS__ },
#include "prof_tests.h"
#undef T
};
#define NTESTS ((int)(sizeof(tests) / sizeof(tests[0])))

extern const struct prof_ent prof_table[], prof_table_end[];

typedef void (*bench_fn)(uint64_t iters, struct prof_state *st);
typedef void (*check_fn)(struct prof_state *st);

/* ------------------------------------------------------------------ */
/* options                                                             */

static int opt_verbose, opt_static_only, opt_csv, opt_sort, opt_reps = 15;
static double opt_ms = 1.0;
static const char *opt_filter, *opt_dump, *opt_record, *opt_check;
static int opt_cpu = -1, opt_target = -1;	/* -1 auto, 0 penryn, 1 merom */

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(2);
}

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
	return (uint64_t)hi << 32 | lo;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
	double *t = malloc(n * sizeof(*t)), m;

	memcpy(t, v, n * sizeof(*t));
	qsort(t, n, sizeof(*t), cmp_d);
	m = n & 1 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
	free(t);
	return m;
}

/* median absolute deviation, scaled to estimate a standard deviation */
static double mad(double *v, int n, double med)
{
	double *t = malloc(n * sizeof(*t)), m;

	for (int i = 0; i < n; i++)
		t[i] = fabs(v[i] - med);
	m = median(t, n) * 1.4826;
	free(t);
	return m;
}

static uint64_t xs_state;
static uint64_t rnd(void)
{
	xs_state ^= xs_state << 13;
	xs_state ^= xs_state >> 7;
	xs_state ^= xs_state << 17;
	return xs_state;
}

static uint64_t fnv(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ull;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 0x100000001b3ull;
	return h | 1;
}

/* ------------------------------------------------------------------ */
/* host description                                                    */

static struct {
	char model[128];
	int avx, avx2, sse41, sse42, popcnt, xsave, constant_tsc;
	char governor[32];
} host;

static void read_host(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[8192];

	strcpy(host.model, "?");
	if (f) {
		int got_flags = 0;

		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, ':');

			if (!v)
				continue;
			v += 2;
			if (!strncmp(line, "model name", 10) && host.model[0] == '?') {
				snprintf(host.model, sizeof(host.model), "%s", v);
				host.model[strcspn(host.model, "\n")] = 0;
			} else if (!strncmp(line, "flags", 5) && !got_flags) {
				char *tok, *save;

				got_flags = 1;
				for (tok = strtok_r(v, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save)) {
					host.avx |= !strcmp(tok, "avx");
					host.avx2 |= !strcmp(tok, "avx2");
					host.sse41 |= !strcmp(tok, "sse4_1");
					host.sse42 |= !strcmp(tok, "sse4_2");
					host.popcnt |= !strcmp(tok, "popcnt");
					host.xsave |= !strcmp(tok, "xsave");
					host.constant_tsc |= !strcmp(tok, "constant_tsc");
				}
			}
		}
		fclose(f);
	}
	f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
	if (f) {
		if (fgets(host.governor, sizeof(host.governor), f))
			host.governor[strcspn(host.governor, "\n")] = 0;
		fclose(f);
	}
}

/* the CPU the static analysis describes */
static struct { int sse41, sse42, popcnt, xsave; const char *name; } tgt;

static void pick_target(void)
{
	int t = opt_target;

	if (t < 0 && !host.avx) {	/* a real AVX-less machine: describe it */
		tgt.sse41 = host.sse41;
		tgt.sse42 = host.sse42;
		tgt.popcnt = host.popcnt;
		tgt.xsave = host.xsave;
		tgt.name = "this CPU";
		return;
	}
	if (t < 0)
		t = 0;
	tgt.sse41 = t == 0;
	tgt.sse42 = tgt.popcnt = tgt.xsave = 0;
	tgt.name = t == 0 ? "Penryn (assumed)" : "Merom/Kentsfield, SSSE3 only (assumed)";
}

/* ------------------------------------------------------------------ */
/* module counters                                                     */

static const char *const stat_names[] = {
	"slow", "fast", "translated", "patched", "patched_short", "patched_int",
	"regions", "regions_borrowing", "unsupported", "short_refused", "unpatched",
};
#define NSTAT ((int)(sizeof(stat_names) / sizeof(stat_names[0])))
enum { S_SLOW, S_FAST, S_TRANSLATED, S_PATCHED, S_PSHORT, S_PINT, S_REGIONS, S_RBORROW, S_UNSUP, S_SREFUSED, S_UNPATCHED };

static int stat_fd[NSTAT];
static int module_loaded;

static void open_stats(void)
{
	char path[128];

	for (int i = 0; i < NSTAT; i++) {
		snprintf(path, sizeof(path), "/sys/module/fakeavx/parameters/stat_%s", stat_names[i]);
		stat_fd[i] = open(path, O_RDONLY);
		if (stat_fd[i] >= 0)
			module_loaded = 1;
	}
}

static void read_stats(long long *v)
{
	char buf[32];

	for (int i = 0; i < NSTAT; i++) {
		ssize_t n = stat_fd[i] >= 0 ? pread(stat_fd[i], buf, sizeof(buf) - 1, 0) : -1;

		if (n > 0) {
			buf[n] = 0;
			v[i] = atoll(buf);
		} else {
			v[i] = 0;
		}
	}
}

static int read_param(const char *name, char *out, int len)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/module/fakeavx/parameters/%s", name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, len, f))
		out[0] = 0;
	out[strcspn(out, "\n")] = 0;
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* counters                                                            */

static int perf_fd = -1, perf_fd2 = -1;

static long perf_open(uint64_t config, int group)
{
	struct perf_event_attr a;

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.type = PERF_TYPE_HARDWARE;
	a.config = config;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	a.read_format = PERF_FORMAT_GROUP;
	return syscall(SYS_perf_event_open, &a, 0, -1, group, 0);
}

static void open_perf(void)
{
	perf_fd = perf_open(PERF_COUNT_HW_CPU_CYCLES, -1);
	if (perf_fd < 0)
		return;
	perf_fd2 = perf_open(PERF_COUNT_HW_INSTRUCTIONS, perf_fd);
	if (perf_fd2 < 0) {
		close(perf_fd);
		perf_fd = -1;
	}
}

struct sample {
	double cyc, ins, tsc;
};

static inline void perf_read(uint64_t *cyc, uint64_t *ins)
{
	struct { uint64_t nr, v[2]; } r;

	if (perf_fd < 0 || read(perf_fd, &r, sizeof(r)) != sizeof(r)) {
		*cyc = *ins = 0;
		return;
	}
	*cyc = r.v[0];
	*ins = r.v[1];
}

/*
 * Benign inputs for the benchmarks. Placed so that the memory operand
 * buffer starts at page offset 0x800: fakeavx's per-thread state block is
 * page aligned and its busy part (upper halves, spill slots) is at offsets
 * 0x000-0x3ff. A load at the same page offset as a recent store to it is
 * held back by Core 2's 4K-aliasing check, which costs ~9 cycles; where a
 * static variable happens to land must not decide the results.
 */
static struct prof_state *bst;
#define BST_PAGE_OFF	0x800

static struct sample run_bench(bench_fn fn, uint64_t iters)
{
	uint64_t c0, i0, c1, i1, t0, t1;
	struct sample s;

	perf_read(&c0, &i0);
	t0 = rdtsc();
	fn(iters, bst);
	t1 = rdtsc();
	perf_read(&c1, &i1);
	s.cyc = c1 - c0;
	s.ins = i1 - i0;
	s.tsc = t1 - t0;
	return s;
}

static double tsc_hz;

static void calibrate_tsc(void)
{
	double s0 = now_s(), s1;
	uint64_t t0 = rdtsc(), t1;

	do
		s1 = now_s();
	while (s1 - s0 < 0.2);
	t1 = rdtsc();
	tsc_hz = (t1 - t0) / (s1 - s0);
}

/* ------------------------------------------------------------------ */
/* faults: an unsupported instruction must not end the run             */

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_sig;

static void on_fault(int sig)
{
	fault_sig = sig;
	siglongjmp(fault_jmp, 1);
}

static void catch_faults(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_fault;
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* inputs                                                              */

static void fill_index(uint8_t *y)
{
	for (int i = 0; i < 8; i++) {
		uint32_t v = i & 1 ? 0 : (uint32_t)(7 - i);	/* 7,0,5,0,3,0,1,0 */

		memcpy(y + 4 * i, &v, 4);
	}
}

static void init_bench_state(void)
{
	uint8_t *page = aligned_alloc(4096, 2 * 4096);

	if (!page)
		die("out of memory\n");
	bst = (struct prof_state *)(page + BST_PAGE_OFF - ST_MEM);
	memset(bst, 0, sizeof(*bst));
	for (int r = 0; r < 16; r++)
		for (int i = 0; i < 8; i++) {
			float f = 1.0f + (r * 8 + i) / 128.0f;

			memcpy(&bst->ymm[r][4 * i], &f, 4);
		}
	for (int i = 0; i < 8; i++) {	/* ymm5 mask, ymm3 blend selector */
		uint32_t m = i & 1 ? 0 : 0xffffffffu;

		memcpy(&bst->ymm[5][4 * i], &m, 4);
		memcpy(&bst->ymm[3][4 * i], &m, 4);
	}
	fill_index(bst->ymm[6]);
	for (int i = 0; i < ST_MEMSZ / 4; i++) {
		float f = 1.5f + i / 256.0f;

		memcpy(&bst->mem[4 * i], &f, 4);
	}
	bst->gp[0] = 0x123456789abcull;
	bst->gp[1] = 0xdeadbeefcafef00dull;
	bst->gp[2] = 5;
	bst->gp[3] = 1;
	bst->gp[4] = 2;
	bst->gp[5] = 3;
	bst->gp[6] = 4;
	bst->flags = 0x202;
}

/* deterministic, test-specific, with special floating-point values */
static void fill_mixed(uint8_t *p, int n)
{
	static const uint32_t fs[] = {
		0x00000000, 0x80000000, 0x3fc00000, 0xbfc00000, 0x7f800000, 0xff800000,
		0x7fc00000, 0x7fa00001, 0xffc12345, 0x00000001, 0x007fffff, 0x4b000001,
		0x3f000000, 0xcf000000, 0x4f000000, 0x3effffff,
	};
	static const uint64_t ds[] = {
		0, 0x8000000000000000ull, 0x3ff8000000000000ull, 0xbff8000000000000ull,
		0x7ff0000000000000ull, 0x7ff8000000000000ull, 0x7ff4000000000001ull,
		0x0000000000000001ull, 0x43e0000000000000ull, 0xc1e0000000000000ull,
		0x3fe0000000000000ull, 0x4330000000000001ull,
	};

	for (int i = 0; i < n; i += 8) {
		uint64_t q = rnd(), r = rnd();
		uint32_t a, b;

		switch (r % 6) {
		case 0: case 1:
			break;
		case 2:		/* two floats: specials or moderate values */
			a = r & 0x100 ? fs[(r >> 9) % 16] : 0;
			b = r & 0x200 ? fs[(r >> 13) % 16] : 0;
			if (!a) {
				float f = (float)((int64_t)(q % 200001) - 100000) / 64.0f;
				memcpy(&a, &f, 4);
			}
			if (!b) {
				float f = (float)((int64_t)((q >> 20) % 2001) - 1000) / 8.0f;
				memcpy(&b, &f, 4);
			}
			q = (uint64_t)b << 32 | a;
			break;
		case 3:		/* a double */
			if (r & 0x100) {
				q = ds[(r >> 9) % 12];
			} else {
				double d = (double)((int64_t)(q % 2000001) - 1000000) / 1024.0;
				memcpy(&q, &d, 8);
			}
			break;
		case 4:		/* small integers */
			q &= 0x00ff00ff00ff00ffull;
			break;
		case 5:		/* sign-heavy */
			q |= 0x8080808080808080ull & rnd();
			break;
		}
		memcpy(p + i, &q, n - i < 8 ? n - i : 8);
	}
}

static void init_check_state(struct prof_state *st, const char *name)
{
	memset(st, 0, sizeof(*st));
	xs_state = fnv(name);
	for (int r = 0; r < 16; r++)
		fill_mixed(st->ymm[r], 32);
	fill_index(st->ymm[6]);
	fill_mixed(st->mem, ST_MEMSZ);
	for (int i = 0; i < NGP; i++)
		st->gp[i] = rnd();
	st->flags = 0x202 | (rnd() & 0x8d5);
}

/* ------------------------------------------------------------------ */
/* static analysis: what fakeavx makes of it                           */

static const char *const shape_names[] = {
	"none", "rvm", "rm", "shift-imm", "load", "store", "pass", "movss", "movmsk",
	"zero", "cvt-widen", "cvt-narrow", "bcast", "pmovx", "ptest", "perm2",
	"insert128", "extract128", "blendv", "permilpd", "permq", "helper",
};

/*
 * Calls into the helper blob (generic C helper, or the POPCNT or CRC32 routine):
 * every "call rel32" whose target is one of the blob's entry points. p is
 * the code as it would sit at guest address addr.
 */
static int count_helpers(const u8 *p, int len, u64 addr, u64 blob)
{
	int n = 0, i = 0;

	while (i < len) {
		int l = fa_insn_len(p + i, len - i);

		if (l <= 0)
			break;
		if (p[i] == 0xe8 && l == 5) {
			s32 rel;
			u64 to;

			memcpy(&rel, p + i + 1, 4);
			to = addr + i + 5 + (s64)rel;
			if (to == blob + FA_BLOB_HELPER || to == blob + FA_BLOB_POPCNT ||
			    to == blob + FA_BLOB_CRC32 || to == blob + FA_BLOB_HELPER_LEAN)
				n++;
		}
		i += l;
	}
	return n;
}

/* host instructions in translated code; skips data jumped over by "jmp short" */
static int count_insns(const u8 *p, int len, int stop_at_jmp)
{
	int n = 0, i = 0;

	while (i < len) {
		int l = fa_insn_len(p + i, len - i);

		if (l <= 0)
			break;
		n++;
		if (p[i] == 0xeb && (s8)p[i + 1] > 0) {
			i += 2 + (s8)p[i + 1];
			continue;
		}
		if (stop_at_jmp && p[i] == 0xe9)
			break;
		i += l;
	}
	return n;
}

/* does this decoded instruction trap on the target CPU? */
static int traps_on_target(const struct fa_insn *in)
{
	switch (in->kind) {
	case FA_K_VEX:
		return 1;
	case FA_K_POPCNT:
		return !tgt.popcnt;
	case FA_K_XGETBV:
		return !tgt.xsave;
	case FA_K_SSE4:
		if ((in->map == 2 && (in->opcode == 0x37 || in->opcode >= 0xf0)) ||
		    (in->map == 3 && in->opcode >= 0x60 && in->opcode <= 0x63))
			return !tgt.sse42;
		return !tgt.sse41;
	}
	return 0;
}

static const char *kind_name(int k)
{
	return k == FA_K_VEX ? "vex" : k == FA_K_POPCNT ? "popcnt" : k == FA_K_XGETBV ? "xgetbv" :
	       k == FA_K_SSE4 ? "sse4-legacy" : "?";
}

#define BUF_ADDR_SPACE	0x10000

static u8 tr_buf[FA_MAX_STUB * 4] __attribute__((aligned(64)));
static u8 rg_buf[16384] __attribute__((aligned(64)));
static struct fa_rgn rgn;

static int rd_self(void *ctx, u64 addr, u8 *buf, int n)
{
	(void)ctx;
	memcpy(buf, (const void *)(uintptr_t)addr, n);
	return n;
}

struct analysis {
	int k;			/* instructions in the unit */
	int trapping;		/* of which trap on the target */
	int unsupported;	/* trap but cannot be decoded/translated */
	int kernel;		/* emulated by the kernel on every execution (XGETBV) */
	int helpers;		/* helper calls per unit (single translation) */
	int single_insns, single_bytes;	/* translation outside a loop */
	int rg_ok, rg_items, rg_map, rg_borrowed, rg_body;
	int loop_total, loop_covered;	/* loop instructions, and those in the region */
	double loop_insns, loop_bytes, loop_helpers;	/* per unit, inside the loop region */
	char desc[160];		/* kinds and shapes */
	u8 single[FA_MAX_STUB * 4];
	int single_len;
	u8 orig[64];		/* the unit's bytes before fakeavx patched anything */
	int orig_len;
};

static void analyse(int t, struct analysis *a)
{
	const struct prof_ent *e = &prof_table[t];
	const u8 *p = (const u8 *)(uintptr_t)e->unit;
	int len = e->unit_end - e->unit, off = 0;

	memset(a, 0, sizeof(*a));
	a->orig_len = len < (int)sizeof(a->orig) ? len : (int)sizeof(a->orig);
	memcpy(a->orig, p, a->orig_len);
	while (off < len) {
		struct fa_insn in;
		struct fa_env env;
		int l = fa_insn_len(p + off, len - off), tl;
		char d[40];

		if (l <= 0)
			break;
		a->k++;
		if (fa_decode(p + off, l, e->unit + off, &in) < 0 || in.len != (u32)l ||
		    !traps_on_target(&in)) {
			if (fa_decode(p + off, l, e->unit + off, &in) >= 0 && in.len == (u32)l &&
			    !traps_on_target(&in))
				snprintf(d, sizeof(d), "%s(native here)", kind_name(in.kind));
			else
				snprintf(d, sizeof(d), "native");
			goto next;
		}
		a->trapping++;
		if (in.kind == FA_K_XGETBV) {
			a->kernel++;
			snprintf(d, sizeof(d), "xgetbv(kernel, every time)");
			goto next;
		}
		memset(&env, 0, sizeof(env));
		env.out = tr_buf;
		env.cap = sizeof(tr_buf);
		env.out_addr = (u64)(uintptr_t)tr_buf;
		env.blob_addr = (u64)(uintptr_t)tr_buf + BUF_ADDR_SPACE;
		env.no_sse41 = !tgt.sse41;
		tl = fa_translate(&in, &env);
		if (tl <= 0) {
			a->unsupported++;
			snprintf(d, sizeof(d), "%s:UNSUPPORTED", kind_name(in.kind));
			goto next;
		}
		if (a->single_len + tl <= (int)sizeof(a->single)) {
			memcpy(a->single + a->single_len, tr_buf, tl);
			a->single_len += tl;
		}
		a->single_bytes += tl;
		a->single_insns += count_insns(tr_buf, tl, 1);
		a->helpers += count_helpers(tr_buf, tl, env.out_addr, env.blob_addr);
		if (in.kind == FA_K_VEX) {
			const struct fa_op *op = fa_lookup(in.map, in.pp, in.opcode);
			int sh = op ? op->shape : 0;

			snprintf(d, sizeof(d), "vex.%s%s", sh < (int)(sizeof(shape_names) / sizeof(*shape_names)) ?
				 shape_names[sh] : "?", in.L ? ".256" : ".128");
		} else {
			snprintf(d, sizeof(d), "%s", kind_name(in.kind));
		}
next:
		snprintf(a->desc + strlen(a->desc), sizeof(a->desc) - strlen(a->desc), "%s%s",
			 a->desc[0] ? " + " : "", d);
		off += l;
	}

	/* the loop, as the module would build its region (same fallbacks) */
	if (a->trapping) {
		static const struct { u8 max_items, no_map; } tries[] = { { 0, 0 }, { 0, 1 }, { 1, 1 } };
		int ret = -1, order[FA_RG_ITEMS], n;
		u64 lo = e->loop, hi = e->loop + (u64)PROF_UNROLL * len;

		for (unsigned i = 0; i < 3 && ret < 0; i++) {
			memset(&rgn, 0, offsetof(struct fa_rgn, len));
			rgn.read = rd_self;
			rgn.entry_ip = e->loop;	/* where the loop's first trap happens */
			rgn.out = rg_buf;
			rgn.cap = sizeof(rg_buf);
			rgn.out_addr = (u64)(uintptr_t)rg_buf;
			rgn.blob_addr = (u64)(uintptr_t)rg_buf + BUF_ADDR_SPACE;
			rgn.max_items = tries[i].max_items;
			rgn.no_map = tries[i].no_map;
			rgn.no_sse41 = !tgt.sse41;
			ret = fa_region_build(&rgn);
		}
		if (ret < 0 || rgn.it[0].kind != FA_R_OURS)
			return;
		a->rg_ok = 1;
		a->rg_items = rgn.n;
		a->rg_map = rgn.has_map;
		a->rg_borrowed = __builtin_popcount(rgn.map.reserved);
		a->rg_body = rgn.len;
		/* items sorted by their place in the body, to know each one's length */
		n = rgn.n;
		for (int i = 0; i < n; i++)
			order[i] = i;
		for (int i = 1; i < n; i++)
			for (int j = i; j > 0 && rgn.it[order[j - 1]].off > rgn.it[order[j]].off; j--) {
				int x = order[j];

				order[j] = order[j - 1];
				order[j - 1] = x;
			}
		a->loop_total = PROF_UNROLL * a->k + 2;	/* + dec, jnz */
		for (int i = 0; i < n; i++) {
			const struct fa_ritem *it = &rgn.it[order[i]];
			u32 end = i + 1 < n ? rgn.it[order[i + 1]].off : rgn.len;

			if (it->ip >= lo && it->ip < e->loop_end)
				a->loop_covered++;
			if (it->ip < lo || it->ip >= hi || end <= it->off)
				continue;
			a->loop_bytes += end - it->off;
			a->loop_insns += count_insns(rg_buf + it->off, end - it->off, 0);
			a->loop_helpers += count_helpers(rg_buf + it->off, end - it->off,
							rgn.out_addr + it->off, rgn.blob_addr);
		}
		a->loop_bytes /= PROF_UNROLL;
		a->loop_insns /= PROF_UNROLL;
		a->loop_helpers /= PROF_UNROLL;
	}
}

static void hexdump(const char *pfx, const u8 *p, int n)
{
	for (int i = 0; i < n; i += 16) {
		printf("%s%04x:", pfx, i);
		for (int j = i; j < n && j < i + 16; j++)
			printf(" %02x", p[j]);
		printf("\n");
	}
}

static void dump_files(int t, const struct analysis *a)
{
	char path[512];
	FILE *f;

	mkdir(opt_dump, 0755);
	snprintf(path, sizeof(path), "%s/%s.single.bin", opt_dump, tests[t].name);
	if (a->single_len && (f = fopen(path, "wb"))) {
		fwrite(a->single, 1, a->single_len, f);
		fclose(f);
	}
	if (a->rg_ok) {
		/* analyse() left this test's region in rg_buf */
		snprintf(path, sizeof(path), "%s/%s.region.bin", opt_dump, tests[t].name);
		if ((f = fopen(path, "wb"))) {
			fwrite(rg_buf, 1, rgn.len, f);
			fclose(f);
		}
	}
}

/* ------------------------------------------------------------------ */
/* golden values                                                       */

/* components of a check result: 16 ymm, 7 gp, flags, 16 memory chunks */
#define NCOMP (16 + NGP + 1 + ST_MEMSZ / 32)

static int comp(const struct prof_state *st, int out, int c, u8 *v)
{
	if (c < 16) {
		memcpy(v, out ? st->oymm[c] : st->ymm[c], 32);
		return 32;
	}
	if (c < 16 + NGP) {
		memcpy(v, out ? &st->ogp[c - 16] : &st->gp[c - 16], 8);
		return 8;
	}
	if (c == 16 + NGP) {
		uint64_t f = (out ? st->oflags : st->flags) & 0x8d5;

		memcpy(v, &f, 8);
		return 8;
	}
	memcpy(v, st->mem + 32 * (c - 17 - NGP), 32);	/* memory is updated in place */
	return 32;
}

static const char *const gp_names[NGP] = { "rax", "rcx", "rdx", "r8", "r9", "r10", "r11" };

static const char *comp_name(int c, char *buf)
{
	if (c < 16)
		sprintf(buf, "ymm%d", c);
	else if (c < 16 + NGP)
		sprintf(buf, "%s", gp_names[c - 16]);
	else if (c == 16 + NGP)
		sprintf(buf, "flags");
	else
		sprintf(buf, "mem+%d", 32 * (c - 17 - NGP));
	return buf;
}

static void to_hex(char *s, const u8 *v, int n)
{
	/* most significant byte first, as registers are usually written */
	for (int i = n - 1; i >= 0; i--)
		s += sprintf(s, "%02x", v[i]);
}

static int run_check(int t, struct prof_state *st)
{
	init_check_state(st, tests[t].name);
	fault_sig = 0;
	if (sigsetjmp(fault_jmp, 1))
		return fault_sig;
	((check_fn)(uintptr_t)prof_table[t].check)(st);
	return 0;
}

static void record(void)
{
	static struct prof_state st;
	FILE *f;
	int n = 0;

	if (!host.avx2 || module_loaded)
		die("--record needs a CPU with real AVX2 (and fakeavx not loaded)\n");
	f = fopen(opt_record, "w");
	if (!f)
		die("%s: %s\n", opt_record, strerror(errno));
	fprintf(f, "# fakeavx-prof golden values, recorded on: %s\n", host.model);
	for (int t = 1; t < NTESTS; t++) {
		u8 in[32], out[32];
		char hex[80];

		if (tests[t].flags & NOCHK)
			continue;
		if (run_check(t, &st))
			die("%s faulted natively\n", tests[t].name);
		fprintf(f, "T %s\n", tests[t].name);
		/* memory chunks: compare against a fresh copy of the input */
		{
			static struct prof_state in_st;

			init_check_state(&in_st, tests[t].name);
			for (int c = 0; c < NCOMP; c++) {
				int len = comp(&in_st, 0, c, in);

				comp(&st, 1, c, out);
				if (!memcmp(in, out, len))
					continue;
				to_hex(hex, out, len);
				fprintf(f, "  %d %s\n", c, hex);
			}
		}
		n++;
	}
	fclose(f);
	printf("recorded %d tests to %s\n", n, opt_record);
}

struct golden {
	char name[64];
	int nc;
	int c[NCOMP];
	u8 v[NCOMP][32];
};

static struct golden *gold;
static int ngold;

static void load_golden(void)
{
	FILE *f = fopen(opt_check, "r");
	char line[256];

	if (!f)
		die("%s: %s\n", opt_check, strerror(errno));
	while (fgets(line, sizeof(line), f)) {
		char hex[128];
		int c;

		if (!strncmp(line, "T ", 2)) {
			gold = realloc(gold, (ngold + 1) * sizeof(*gold));
			memset(&gold[ngold], 0, sizeof(*gold));
			sscanf(line + 2, "%63s", gold[ngold].name);
			ngold++;
		} else if (ngold && sscanf(line, " %d %127s", &c, hex) == 2 && c >= 0 && c < NCOMP) {
			struct golden *g = &gold[ngold - 1];
			int len = strlen(hex) / 2;

			g->c[g->nc] = c;
			for (int i = 0; i < len && i < 32; i++) {
				unsigned b;

				sscanf(hex + 2 * (len - 1 - i), "%2x", &b);
				g->v[g->nc][i] = b;
			}
			g->nc++;
		}
	}
	fclose(f);
}

/* returns -1: no golden value, 0: match, >0 mismatching components */
static int check_one(int t, char *msg, int msglen)
{
	static struct prof_state st, in_st;
	struct golden *g = NULL;
	int bad = 0, sig;

	msg[0] = 0;
	for (int i = 0; i < ngold; i++)
		if (!strcmp(gold[i].name, tests[t].name))
			g = &gold[i];
	if (!g)
		return -1;
	sig = run_check(t, &st);
	if (sig) {
		snprintf(msg, msglen, "    killed by signal %d (%s)\n", sig, strsignal(sig));
		return NCOMP;
	}
	init_check_state(&in_st, tests[t].name);
	for (int c = 0; c < NCOMP; c++) {
		u8 want[32], got[32];
		int len = comp(&in_st, 0, c, want), j;
		char nm[16], h1[80], h2[80];

		for (j = 0; j < g->nc; j++)
			if (g->c[j] == c)
				memcpy(want, g->v[j], len);
		comp(&st, 1, c, got);
		if (!memcmp(want, got, len))
			continue;
		bad++;
		to_hex(h1, got, len);
		to_hex(h2, want, len);
		if ((int)strlen(msg) < msglen - 200)
			snprintf(msg + strlen(msg), msglen - strlen(msg),
				 "    %-6s got  %s\n    %-6s want %s\n", comp_name(c, nm), h1, "", h2);
	}
	return bad;
}

/* ------------------------------------------------------------------ */
/* measurement                                                         */

struct result {
	int sig;			/* faulted */
	double first_us;		/* first call: trap + translation + patch */
	long long first[NSTAT];		/* module counter deltas, first call */
	long long steady[NSTAT];	/* module counter deltas while measuring (all processes) */
	long long loop_traps;		/* traps per call that scale with iterations: per n iters */
	long long call_traps;		/* traps per call of the bench function (prologue) */
	uint64_t iters;
	double cyc, cyc_sd, ins, ns, ns_sd, cyc_min;
	struct sample *s, *e;		/* raw samples, test and empty */
};

static uint64_t empty_iters_done;

static void measure(int t, struct result *r)
{
	bench_fn fn = (bench_fn)(uintptr_t)prof_table[t].bench;
	bench_fn empty = (bench_fn)(uintptr_t)prof_table[0].bench;
	long long s0[NSTAT], s1[NSTAT];
	double *dc, *di, *dt;
	uint64_t n, t0, t1, target = tsc_hz * opt_ms / 1000;

	memset(r, 0, sizeof(*r));
	fault_sig = 0;
	if (sigsetjmp(fault_jmp, 1)) {
		r->sig = fault_sig;
		return;
	}
	if (!empty_iters_done) {	/* the overhead loop gets translated once, too */
		empty(1, bst);
		empty_iters_done = 1;
	}

	/* first call */
	read_stats(s0);
	t0 = rdtsc();
	fn(1, bst);
	t1 = rdtsc();
	read_stats(s1);
	r->first_us = (t1 - t0) / tsc_hz * 1e6;
	for (int i = 0; i < NSTAT; i++)
		r->first[i] = s1[i] - s0[i];

	/* iterations per sample: enough for opt_ms of work */
	for (n = 16; n < (1ull << 26); n *= 2) {
		struct sample s = run_bench(fn, n);

		if (s.tsc >= target)
			break;
	}
	r->iters = n;
	run_bench(fn, n);	/* warm */
	run_bench(empty, n);

	/*
	 * Traps of this process. The module's counters are global, and other
	 * processes using fakeavx (a shell, an editor) add to them at any
	 * time, so: traps of a 1-iteration call (the bench prologue) and
	 * those of an n-iteration call; the difference is what the loop
	 * itself costs in traps. Other processes can only add, so the
	 * minimum over a few trials is the best estimate.
	 */
	r->loop_traps = r->call_traps = LLONG_MAX;
	for (int k = 0; k < 5; k++) {
		long long a[NSTAT], b[NSTAT], c1, cn;

		read_stats(a);
		run_bench(fn, 1);
		read_stats(b);
		c1 = b[S_FAST] - a[S_FAST] + b[S_SLOW] - a[S_SLOW];
		read_stats(a);
		run_bench(fn, n);
		read_stats(b);
		cn = b[S_FAST] - a[S_FAST] + b[S_SLOW] - a[S_SLOW];
		if (c1 < r->call_traps)
			r->call_traps = c1;
		if (cn - c1 < r->loop_traps)
			r->loop_traps = cn - c1 < 0 ? 0 : cn - c1;
	}

	r->s = calloc(opt_reps, sizeof(*r->s));
	r->e = calloc(opt_reps, sizeof(*r->e));
	dc = calloc(opt_reps, sizeof(double));
	di = calloc(opt_reps, sizeof(double));
	dt = calloc(opt_reps, sizeof(double));
	read_stats(s0);
	for (int i = 0; i < opt_reps; i++) {	/* interleaved, so drift cancels */
		r->e[i] = run_bench(empty, n);
		r->s[i] = run_bench(fn, n);
	}
	read_stats(s1);
	for (int i = 0; i < NSTAT; i++)
		r->steady[i] = s1[i] - s0[i];

	for (int i = 0; i < opt_reps; i++) {
		double units = (double)n * PROF_UNROLL;

		dc[i] = (r->s[i].cyc - r->e[i].cyc) / units;
		di[i] = (r->s[i].ins - r->e[i].ins) / units;
		dt[i] = (r->s[i].tsc - r->e[i].tsc) / units / tsc_hz * 1e9;
	}
	r->cyc = median(dc, opt_reps);
	r->cyc_sd = mad(dc, opt_reps, r->cyc);
	r->ins = median(di, opt_reps);
	r->ns = median(dt, opt_reps);
	r->ns_sd = mad(dt, opt_reps, r->ns);
	{
		double ms = INFINITY, me = INFINITY;

		for (int i = 0; i < opt_reps; i++) {	/* TSC ticks without perf */
			double cs = perf_fd >= 0 ? r->s[i].cyc : r->s[i].tsc;
			double ce = perf_fd >= 0 ? r->e[i].cyc : r->e[i].tsc;

			if (cs < ms)
				ms = cs;
			if (ce < me)
				me = ce;
		}
		r->cyc_min = (ms - me) / ((double)n * PROF_UNROLL);
	}
	free(dc);
	free(di);
	free(dt);
}

/* ------------------------------------------------------------------ */
/* report                                                              */

struct row {
	int t;
	struct analysis a;
	struct result r;
	int chk;			/* -2 not checked, -1 no golden, 0 ok, >0 bad */
	char chkmsg[2048];
};

/*
 * Traps per n-iteration call that come from the loop, as opposed to the
 * bench function's prologue: should be 0 once the loop is translated.
 * Below one per 16 iterations it is background noise from other processes.
 */
static long long steady_traps(const struct result *r)
{
	return r->loop_traps * 16 >= (long long)r->iters && r->loop_traps > 8 ? r->loop_traps : 0;
}

static const char *path_of(const struct row *w)
{
	if (w->r.sig)
		return "FAULT";
	if (!w->a.trapping)
		return "native";
	if (w->a.unsupported)
		return "UNSUPP";
	if (w->a.kernel)
		return "kernel";
	if (steady_traps(&w->r) > 0 && !w->a.kernel)
		return "TRAPS";
	if (w->a.helpers || w->a.loop_helpers > 0)
		return "helper";
	return "inline";
}

static int matches(const char *name)
{
	char buf[512], *tok, *save;

	if (!opt_filter)
		return 1;
	snprintf(buf, sizeof(buf), "%s", opt_filter);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		if (strstr(name, tok))
			return 1;
	return 0;
}

static int sort_key(const void *x, const void *y)
{
	const struct row *a = x, *b = y;
	double ka = opt_static_only ? a->a.loop_insns : (perf_fd >= 0 ? a->r.cyc : a->r.ns);
	double kb = opt_static_only ? b->a.loop_insns : (perf_fd >= 0 ? b->r.cyc : b->r.ns);

	return ka < kb ? 1 : ka > kb ? -1 : 0;
}

/*
 * Where the heap landed relative to the program (ASLR): fakeavx cannot put
 * short-patch stubs near the heap, so this can decide whether 3-4 byte
 * instructions get patched or keep trapping.
 */
static void print_layout(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	unsigned long lo, hi, text = 0, text_end = 0, heap = 0, heap_end = 0;
	char line[512];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
			continue;
		if (!text && strstr(line, "r-xp") && (uintptr_t)print_layout >= lo &&
		    (uintptr_t)print_layout < hi) {
			text = lo;
			text_end = hi;
		}
		if (strstr(line, "[heap]")) {
			heap = lo;
			heap_end = hi;
		}
	}
	fclose(f);
	if (text && heap)
		printf("  layout     program text %#lx; heap starts %lu MB after its end (%lu KB)\n",
		       text, (heap - text_end) >> 20, (heap_end - heap) >> 10);
}

static void print_header(void)
{
	char pol[32] = "?", buf[64];

	printf("fakeavx-prof\n");
	printf("  cpu        %s\n", host.model);
	printf("  features   avx=%d avx2=%d sse4.1=%d sse4.2=%d popcnt=%d xsave=%d constant_tsc=%d\n",
	       host.avx, host.avx2, host.sse41, host.sse42, host.popcnt, host.xsave, host.constant_tsc);
	if (module_loaded) {
		read_param("policy", pol, sizeof(pol));
		read_param("patch", buf, sizeof(buf));
		printf("  fakeavx    loaded (policy=%s patch=%s)\n", pol, buf);
	} else {
		printf("  fakeavx    not loaded%s\n", host.avx ? " (timings are native AVX)" : "");
	}
	printf("  analysis   translations shown as for: %s\n", tgt.name);
	print_layout();
	if (!opt_static_only) {
		printf("  timing     TSC %.3f GHz; %s\n"
		       "             %d samples x %.2f ms per test; median of paired differences against\n"
		       "             an empty loop; +- is the MAD as %% of the median\n",
		       tsc_hz / 1e9, perf_fd >= 0 ? "cycles and instructions from perf (user mode)"
						  : "no perf counters: cycles are TSC ticks",
		       opt_reps, opt_ms);
		if (perf_fd < 0)
			printf("             (perf: needs kernel.perf_event_paranoid <= 2 and a PMU)\n");
		if (!module_loaded && host.avx)
			printf("             path/L: columns describe the target above, timings this CPU\n");
		if (host.governor[0] && strcmp(host.governor, "performance"))
			printf("  WARNING    cpufreq governor is \"%s\": for stable numbers use\n"
			       "             \"sudo cpupower frequency-set -g performance\"\n", host.governor);
		if (!host.constant_tsc)
			printf("  WARNING    no constant_tsc: ns figures follow the clock speed\n");
	}
	printf("\n");
}

static void print_columns(void)
{
	if (opt_csv) {
		printf("test,units,path,cycles,cycles_mad_pct,cycles_min,ns,instructions,first_call_us,"
		       "steady_traps,single_insns,single_bytes,loop_insns,loop_bytes,helpers,region_items,"
		       "region_borrowed,check,decode\n");
		return;
	}
	if (opt_static_only)
		printf("%-32s %2s %-7s %8s %8s %6s %7s %6s %6s  %s\n", "test", "k", "path",
		       "1:insns", "1:bytes", "L:cov", "L:insns", "L:hlp", "L:regs", "translation");
	else
		printf("%-32s %2s %-7s %8s %6s %8s %8s %6s %8s %9s %6s %6s\n", "test", "k", "path",
		       "cyc/u", "+-%", "ns/u", "ins/u", "L:cov", "L:insns", "first-us", "traps", "check");
}

/* how much of the loop its region covers: "all" means the loop runs in the cache */
static const char *cov_str(const struct analysis *a)
{
	static char buf[16];

	if (!a->trapping)
		return "-";
	if (!a->rg_ok)
		return "none";
	if (a->loop_covered >= a->loop_total)
		return "all";
	snprintf(buf, sizeof(buf), "%d%%", 100 * a->loop_covered / a->loop_total);
	return buf;
}

static void print_row(struct row *w)
{
	const struct analysis *a = &w->a;
	const struct result *r = &w->r;
	const char *chk = w->chk == -2 ? "-" : w->chk == -1 ? "n/a" : w->chk ? "FAIL" : "ok";
	double val = perf_fd >= 0 ? r->cyc : r->ns, sd = perf_fd >= 0 ? r->cyc_sd : r->ns_sd;
	double pct = val ? 100 * sd / fabs(val) : 0;
	char ins[16];

	if (perf_fd >= 0)
		snprintf(ins, sizeof(ins), "%.1f", r->ins);
	else
		strcpy(ins, "-");

	if (opt_csv) {
		printf("%s,%d,%s,%.3f,%.2f,%.3f,%.3f,%.3f,%.2f,%lld,%d,%d,%.2f,%.2f,%.2f,%d,%d,%s,\"%s\"\n",
		       tests[w->t].name, a->k, path_of(w), perf_fd >= 0 ? r->cyc : r->ns * tsc_hz / 1e9,
		       pct, r->cyc_min, r->ns, r->ins, r->first_us, steady_traps(r),
		       a->single_insns, a->single_bytes, a->loop_insns, a->loop_bytes,
		       a->loop_helpers, a->rg_items, a->rg_borrowed, chk, a->desc);
		return;
	}
	if (opt_static_only) {
		printf("%-32s %2d %-7s %8d %8d %6s %7.1f %6.2f %6d  %s\n", tests[w->t].name, a->k,
		       path_of(w), a->single_insns, a->single_bytes, cov_str(a), a->loop_insns,
		       a->loop_helpers, a->rg_borrowed, a->desc);
	} else if (r->sig) {
		printf("%-32s %2d %-7s  killed by %s  [%s]\n", tests[w->t].name, a->k, path_of(w),
		       strsignal(r->sig), a->desc);
	} else {
		printf("%-32s %2d %-7s %8.2f %6.1f %8.2f %8s %6s %8.1f %9.1f %6lld %6s\n",
		       tests[w->t].name, a->k, path_of(w),
		       perf_fd >= 0 ? r->cyc : r->ns * tsc_hz / 1e9, pct, r->ns,
		       ins, cov_str(a), a->loop_insns, r->first_us,
		       steady_traps(r), chk);
	}
	if (opt_verbose && !opt_csv) {
		printf("    instruction  %s\n", tests[w->t].text);
		printf("    bytes       ");
		for (int i = 0; i < a->orig_len; i++)
			printf(" %02x", a->orig[i]);
		printf("\n    decoded      %s\n", a->desc);
		if (a->trapping)
			printf("    alone        %d host instructions, %d bytes, %d helper call(s)\n",
			       a->single_insns, a->single_bytes, a->helpers);
		if (a->rg_ok) {
			printf("    in its loop  %.1f host instructions, %.1f bytes, %.2f helper calls per unit;"
			       " region of %d items, %s (%d host registers borrowed)\n",
			       a->loop_insns, a->loop_bytes, a->loop_helpers, a->rg_items,
			       a->rg_map ? "upper halves cached in registers" : "upper halves in memory",
			       a->rg_borrowed);
			if (a->loop_covered < a->loop_total)
				printf("    region       covers %d of the loop's %d instructions: the rest runs"
				       " natively and\n                 each AVX instruction there enters and"
				       " leaves the cache by itself\n", a->loop_covered, a->loop_total);
		}
		else if (a->trapping)
			printf("    in its loop  no region could be built\n");
		if (!opt_static_only && !r->sig) {
			printf("    timing       %llu iterations x %d per sample; cycles median %.3f, min %.3f,"
			       " MAD %.3f; ns %.3f +- %.3f\n",
			       (unsigned long long)r->iters, PROF_UNROLL, r->cyc, r->cyc_min, r->cyc_sd,
			       r->ns, r->ns_sd);
			if (module_loaded) {
				printf("    first call   %.1f us; module:", r->first_us);
				for (int i = 0; i < NSTAT; i++)
					if (r->first[i])
						printf(" %s+%lld", stat_names[i], r->first[i]);
				printf("\n    steady       %lld traps per %llu-iteration call from the loop, %lld from"
				       " the bench prologue%s\n", r->loop_traps, (unsigned long long)r->iters,
				       r->call_traps, !steady_traps(r) ? "" : a->kernel ?
				       "  <- emulated in the kernel on every execution" :
				       "  <- traps on every execution: not patched");
				printf("    module       all processes while measuring:");
				for (int i = 0; i < NSTAT; i++)
					if (r->steady[i])
						printf(" %s+%lld", stat_names[i], r->steady[i]);
				printf("\n");
			}
			if (opt_verbose > 1) {
				printf("    samples      (test - empty, per unit)\n");
				for (int i = 0; i < opt_reps; i++)
					printf("      %2d  cyc %10.3f  ins %10.3f  tsc %10.3f\n", i,
					       (r->s[i].cyc - r->e[i].cyc) / (r->iters * (double)PROF_UNROLL),
					       (r->s[i].ins - r->e[i].ins) / (r->iters * (double)PROF_UNROLL),
					       (r->s[i].tsc - r->e[i].tsc) / (r->iters * (double)PROF_UNROLL));
			}
		}
		if (opt_verbose > 1 && a->single_len)
			hexdump("    tr ", a->single, a->single_len);
		if (w->chk > 0)
			printf("%s", w->chkmsg);
		printf("\n");
	} else if (w->chk > 0 && !opt_csv) {
		printf("%s", w->chkmsg);
	}
}

static void usage(void)
{
	printf("usage: fakeavx-prof [options]\n"
	       "  -f, --filter A,B    only tests whose name contains A or B (e.g. -f gather,vdpps,lg_)\n"
	       "  -l, --list          list the tests and exit\n"
	       "  -v, --verbose       details per test: decoding, translation, timing, module counters\n"
	       "                      (-vv: also every sample and a hex dump of the translation)\n"
	       "  -s, --sort          slowest first\n"
	       "  -c, --csv           CSV output\n"
	       "  -n, --static        only show what fakeavx generates (no timing; runs anywhere)\n"
	       "  -r, --reps N        samples per test (default 15)\n"
	       "  -t, --time MS       length of one sample in ms (default 1)\n"
	       "  -p, --cpu N         pin to CPU N (default: the current one)\n"
	       "      --merom         analyse as for a CPU without SSE4.1\n"
	       "      --penryn        analyse as for Penryn (default on CPUs with AVX)\n"
	       "  -D, --dump DIR      write the translations to DIR/<test>.{single,region}.bin\n"
	       "      --record FILE   record reference results (on a CPU with real AVX2)\n"
	       "      --check FILE    compare results with recorded ones\n"
	       "\n"
	       "Columns: k = instructions in the test unit; path = native / inline (translated to\n"
	       "SSE) / helper (calls C code) / kernel (emulated in the kernel every time, by\n"
	       "design: XGETBV) / TRAPS (not patched: traps every time) / UNSUPP / FAULT; cyc/u, ns/u, ins/u = cost per unit; L:cov = how much of the loop its\n"
	       "region covers (all = the whole loop runs in the code cache); L:insns = host\n"
	       "instructions the region runs per unit; first-us = first execution\n"
	       "(trap + translation + patching); traps = traps while measuring (should be 0).\n");
}

int main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "filter", 1, 0, 'f' }, { "list", 0, 0, 'l' }, { "verbose", 0, 0, 'v' },
		{ "sort", 0, 0, 's' }, { "csv", 0, 0, 'c' }, { "static", 0, 0, 'n' },
		{ "reps", 1, 0, 'r' }, { "time", 1, 0, 't' }, { "cpu", 1, 0, 'p' },
		{ "dump", 1, 0, 'D' }, { "merom", 0, 0, 'M' }, { "penryn", 0, 0, 'P' },
		{ "record", 1, 0, 'R' }, { "check", 1, 0, 'C' }, { "help", 0, 0, 'h' },
		{ 0 },
	};
	struct row *rows;
	int c, nrows = 0, list = 0, nfail = 0, nchecked = 0;

	while ((c = getopt_long(argc, argv, "f:lvscnr:t:p:D:h", lo, NULL)) != -1) {
		switch (c) {
		case 'f': opt_filter = optarg; break;
		case 'l': list = 1; break;
		case 'v': opt_verbose++; break;
		case 's': opt_sort = 1; break;
		case 'c': opt_csv = 1; break;
		case 'n': opt_static_only = 1; break;
		case 'r': opt_reps = atoi(optarg); break;
		case 't': opt_ms = atof(optarg); break;
		case 'p': opt_cpu = atoi(optarg); break;
		case 'D': opt_dump = optarg; break;
		case 'M': opt_target = 1; break;
		case 'P': opt_target = 0; break;
		case 'R': opt_record = optarg; break;
		case 'C': opt_check = optarg; break;
		default: usage(); return c == 'h' ? 0 : 2;
		}
	}
	if (opt_reps < 3)
		opt_reps = 3;
	if ((size_t)(prof_table_end - prof_table) != (size_t)NTESTS)
		die("internal: %d tests but %d table entries\n", NTESTS, (int)(prof_table_end - prof_table));
	if (list) {
		for (int t = 1; t < NTESTS; t++)
			if (matches(tests[t].name))
				printf("%-32s %s\n", tests[t].name, tests[t].text);
		return 0;
	}

	read_host();
	pick_target();
	open_stats();
	catch_faults();
	if (!host.avx && !module_loaded && !opt_static_only)
		die("this CPU has no AVX and the fakeavx module is not loaded: the tests would just\n"
		    "crash. Load it, or use -n for the static analysis only.\n");

	if (opt_record) {
		record();
		return 0;
	}
	if (opt_check)
		load_golden();

	if (!opt_static_only) {
		cpu_set_t set;
		int cpu = opt_cpu >= 0 ? opt_cpu : sched_getcpu();
		struct sched_param sp = { .sched_priority = 1 };

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			fprintf(stderr, "warning: cannot pin to CPU %d\n", cpu);
		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			setpriority(PRIO_PROCESS, 0, -10);	/* best effort without root */
		/*
		 * Lock what is mapped now, but not MCL_FUTURE: that would make
		 * fakeavx's own mappings in this process (a 4 MB code cache,
		 * state blocks) count against RLIMIT_MEMLOCK, so its vm_mmap()
		 * fails and the first AVX instruction ends in SIGILL.
		 */
		if (mlockall(MCL_CURRENT))
			fprintf(stderr, "warning: mlockall: %s (timings may include page faults)\n",
				strerror(errno));
		open_perf();
		calibrate_tsc();
	}
	init_bench_state();
	if (!opt_csv)
		print_header();

	rows = calloc(NTESTS, sizeof(*rows));
	if (!opt_sort)
		print_columns();
	for (int t = 1; t < NTESTS; t++) {
		struct row *w;

		if (!matches(tests[t].name))
			continue;
		w = &rows[nrows++];
		w->t = t;
		w->chk = -2;
		analyse(t, &w->a);
		if (opt_dump)
			dump_files(t, &w->a);
		if (opt_check && !(tests[t].flags & NOCHK)) {
			w->chk = check_one(t, w->chkmsg, sizeof(w->chkmsg));
			if (w->chk >= 0)
				nchecked++;
			if (w->chk > 0)
				nfail++;
		}
		if (!opt_static_only)
			measure(t, &w->r);
		if (!opt_sort) {
			print_row(w);
			fflush(stdout);
		} else if (!opt_csv) {
			fprintf(stderr, "\r%d/%d %-40s", nrows, NTESTS - 1, tests[t].name);
		}
	}
	if (opt_sort) {
		if (!opt_csv)
			fprintf(stderr, "\r%60s\r", "");
		qsort(rows, nrows, sizeof(*rows), sort_key);
		print_columns();
		for (int i = 0; i < nrows; i++)
			print_row(&rows[i]);
	}

	if (!opt_csv) {
		int ntraps = 0, nfault = 0;

		for (int i = 0; i < nrows; i++) {
			ntraps += steady_traps(&rows[i].r) > 0 && !rows[i].a.kernel;
			nfault += rows[i].r.sig != 0;
		}
		printf("\n%d tests", nrows);
		if (opt_check)
			printf("; check: %d compared, %d FAILED", nchecked, nfail);
		if (!opt_static_only)
			printf("; %d still trapping after translation, %d faulted", ntraps, nfault);
		printf("\n");
		if (opt_dump)
			printf("translations written to %s/; disassemble with\n"
			       "  objdump -D -b binary -m i386:x86-64 %s/<test>.region.bin\n",
			       opt_dump, opt_dump);
	}
	return nfail ? 1 : 0;
}
