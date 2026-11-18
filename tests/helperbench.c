// SPDX-License-Identifier: GPL-2.0
/*
 * helperbench: run fakeavx translations of helper-path instructions in an
 * ordinary process (no kernel module), with the real translator and the real
 * helper blob, to time them and to compare two builds.
 *
 *   helperbench time [filter]    cycles per instruction (TSC), like fakeavx-prof
 *   helperbench rdump|rtime      the same for loops built as regions (hb_loops)
 *   helperbench dump [filter]    complete state after each instruction, for
 *                                many random inputs: run it from two builds and
 *                                cmp the outputs (old blob = oracle)
 *   helperbench mdump|mrdump|mrtime   the same instructions and loops run
 *                                natively, so that the loaded fakeavx module
 *                                translates them (compare with dump / rdump)
 *
 * Environment: HB_NOSSE41=1 translate as for an SSSE3-only host; HB_MEMOFF
 * page offset of the timing modes' memory buffer (default 0x800);
 * HB_BODYOFF shift of a region body (alignment experiments); HB_DUMPREG=file
 * write the region body there; HB_UNBUF=1 unbuffered output.
 *
 * Works on any x86-64 CPU (only the translated SSE code runs). Needs %gs:
 * set with arch_prctl(ARCH_SET_GS), as the module does.
 */
#define _GNU_SOURCE
#include <asm/prctl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../core/fakeavx.h"
#include "../helpers/blob.h"

struct hb_state {
	u8 xmm[16][16];		/* 0 */
	u64 gp[16];		/* 256 */
	u64 flags;		/* 384 */
	u32 mxcsr;		/* 392 */
	u32 pad;
	u8 hi[16][16];		/* not read by the runner: copied to/from %gs */
	u8 mem[512] __attribute__((aligned(64)));
} __attribute__((aligned(64)));

_Static_assert(__builtin_offsetof(struct hb_state, gp) == 256, "layout");
_Static_assert(__builtin_offsetof(struct hb_state, flags) == 384, "layout");
_Static_assert(__builtin_offsetof(struct hb_state, mxcsr) == 392, "layout");
_Static_assert(__builtin_offsetof(struct hb_state, hi) == 400, "layout");	/* hb_mrun */

struct hb_ent { const char *name; const u8 *start, *end; };
extern const struct hb_ent hb_table[], hb_table_end[];
extern const struct hb_ent hb_loops[], hb_loops_end[];	/* region mode */
void hb_once(struct hb_state *s, void *stub);
void hb_loop(struct hb_state *s, void *stub, u64 n);
void hb_mrun(struct hb_state *s, void *code);

#define UNROLL 8
static u8 *code, *blob, *st;
static int no_sse41;	/* HB_NOSSE41=1: translate as for an SSSE3-only host (Merom) */

static u64 xs = 0x2545f4914f6cdd1dull;
static u64 rnd(void)
{
	xs ^= xs << 13;
	xs ^= xs >> 7;
	xs ^= xs << 17;
	return xs;
}

static inline u64 rdtsc(void)
{
	u32 lo, hi;

	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
	return (u64)hi << 32 | lo;
}

/* translate the unit `copies` times into a stub ending in ret; returns its address */
static u8 *build(const struct hb_ent *e, int copies, u8 *out)
{
	u8 *p = out;

	for (int k = 0; k < copies; k++) {
		u32 off = 0, len = e->end - e->start;

		while (off < len) {
			struct fa_insn in;
			struct fa_env env = { 0 };
			int n;

			if (fa_decode(e->start + off, len - off, (u64)(e->start + off), &in) < 0) {
				fprintf(stderr, "%s: cannot decode\n", e->name);
				exit(2);
			}
			env.out = p;
			env.cap = FA_MAX_STUB * 2;
			env.out_addr = (u64)p;
			env.blob_addr = (u64)blob;
			env.no_jmp = 1;
			env.no_sse41 = no_sse41;
			n = fa_translate(&in, &env);
			if (n <= 0) {
				fprintf(stderr, "%s: cannot translate\n", e->name);
				exit(2);
			}
			p += n;
			off += in.len;
		}
	}
	*p++ = 0xc3;
	return out;
}

/* deterministic inputs: mostly random, some special values, valid gather indices */
static void fill(struct hb_state *s, const char *name, long k)
{
	static const u32 sp[] = { 0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000,
				  0x7fa00001, 0x00000001, 0x3f800000, 0xbf800000, 0xffffffff };

	xs = 0x9e3779b97f4a7c15ull ^ ((u64)k * 0x100000001b3ull);
	for (const char *c = name; *c; c++)
		xs = (xs ^ (u8)*c) * 0x100000001b3ull | 1;
	memset(s, 0, sizeof(*s));
	for (int r = 0; r < 16; r++)
		for (int i = 0; i < 4; i++) {
			u64 v = rnd();
			u32 d = (v >> 8) % 4 == 0 ? sp[(v >> 16) % 10] : (u32)(v >> 32);

			memcpy(s->xmm[r] + 4 * i, &d, 4);
			v = rnd();
			d = (v >> 8) % 4 == 0 ? sp[(v >> 16) % 10] : (u32)(v >> 32);
			memcpy(s->hi[r] + 4 * i, &d, 4);
		}
	for (int i = 0; i < 8; i++) {	/* ymm6: dwords 7,0,5,0,3,0,1,0 */
		u32 v = i & 1 ? 0 : 7 - i;

		memcpy(i < 4 ? s->xmm[6] + 4 * i : s->hi[6] + 4 * (i - 4), &v, 4);
	}
	if (k % 4 == 1) {		/* string ops: shared prefixes, zero bytes */
		memcpy(s->xmm[2], s->xmm[1], 8);
		s->xmm[1][(rnd() & 15)] = 0;
	}
	if (k % 5 == 2)			/* equal operands */
		memcpy(s->xmm[2], s->xmm[1], 16), memcpy(s->hi[2], s->hi[1], 16);
	for (int i = 0; i < 512; i += 8) {
		u64 v = rnd();

		memcpy(s->mem + i, &v, 8);
	}
	for (int i = 0; i < 16; i++)
		s->gp[i] = rnd();
	s->gp[0] = k % 3 == 0 ? rnd() % 20 : s->gp[0];	/* pcmpestr lengths */
	s->gp[2] = k % 3 == 0 ? (u64)-(s64)(rnd() % 20) : s->gp[2];
	s->gp[6] = (u64)s->mem;
	s->flags = 0x202 | (rnd() & 0x8d5) | (k % 7 == 3 ? 0x400 : 0);	/* DF too */
	s->mxcsr = 0x1f80;
}

static void put_hi(const struct hb_state *s)
{
	for (int r = 0; r < 16; r++)
		memcpy(st + FA_ST_HI + 16 * r, s->hi[r], 16);
}

static void get_hi(struct hb_state *s)
{
	for (int r = 0; r < 16; r++)
		memcpy(s->hi[r], st + FA_ST_HI + 16 * r, 16);
}

static void hex(const u8 *p, int n)
{
	for (int i = n - 1; i >= 0; i--)
		printf("%02x", p[i]);
}

static int match(const char *name, const char *filter)
{
	return !filter || strstr(name, filter);
}

static void print_state(const char *name, long k, struct hb_state *s)
{
	s->gp[6] -= (u64)s->mem;	/* address independent */
	printf("%s %ld", name, k);
	for (int r = 0; r < 16; r++) {
		printf(" ");
		hex(s->hi[r], 16);
		hex(s->xmm[r], 16);
	}
	for (int r = 0; r < 16; r++)
		if (r != 4 && r < 13)
			printf(" %016llx", (unsigned long long)s->gp[r]);
	printf(" f%03llx m%04x ", (unsigned long long)(s->flags & 0xcd5), s->mxcsr);
	hex(s->mem, 512);
	printf("\n");
}

/*
 * Module modes: the same instructions (mdump) and loops (mrdump), but
 * executed natively in this process, so that the loaded fakeavx module
 * translates them; output in the same format as dump / rdump.
 */
static void mdump(const struct hb_ent *t0, const struct hb_ent *t1, const char *filter, long cases,
		  int loops)
{
	static struct hb_state s;

	for (const struct hb_ent *e = t0; e < t1; e++) {
		if (!match(e->name, filter))
			continue;
		for (long k = 0; k < cases; k++) {
			fill(&s, e->name, k);
			if (loops)
				s.gp[12] = 1 + k % 50;
			hb_mrun(&s, (void *)e->start);
			print_state(e->name, k, &s);
		}
	}
}

/*
 * A state whose memory buffer starts at page offset HB_MEMOFF (default
 * 0x800). The state block's busy part is at page offsets 0x000-0x3ff, and a
 * load at the same page offset as a recent store to it waits (4K aliasing),
 * so where a static variable happens to land could decide a measurement.
 */
static struct hb_state *placed_state(void)
{
	static u8 *page;
	long off = getenv("HB_MEMOFF") ? strtol(getenv("HB_MEMOFF"), NULL, 0) : 0x800;

	if (!page)
		page = aligned_alloc(4096, 3 * 4096);
	return (struct hb_state *)(page + 4096 + off - __builtin_offsetof(struct hb_state, mem));
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

/* cycles per loop iteration under the module (compare: rtime) */
static void mrtiming(const char *filter)
{
	struct hb_state *sp = placed_state();
#define s (*sp)

	printf("%-16s %10s\n", "loop", "cyc/iter");
	for (const struct hb_ent *e = hb_loops; e < hb_loops_end; e++) {
		u64 d[9];

		if (!match(e->name, filter))
			continue;
		for (int r = 0; r < 10; r++) {
			u64 t0;

			fill(&s, "time", 0);
			s.gp[0] = 5; s.gp[2] = 7;	/* as rtime */
			s.gp[12] = 20000;
			t0 = rdtsc();
			hb_mrun(&s, (void *)e->start);
			if (r)			/* the first run translates */
				d[r - 1] = rdtsc() - t0;
		}
		qsort(d, 9, sizeof(d[0]), cmp_u64);
		printf("%-16s %10.2f\n", e->name, (double)d[4] / 20000);
	}
#undef s
}

static void dump(const char *filter, long cases)
{
	static struct hb_state s;
	u8 *stub = code + 0x10000;

	for (const struct hb_ent *e = hb_table; e < hb_table_end; e++) {
		if (!match(e->name, filter))
			continue;
		build(e, 1, stub);
		for (long k = 0; k < cases; k++) {
			fill(&s, e->name, k);
			put_hi(&s);
			hb_once(&s, stub);
			get_hi(&s);
			s.gp[6] -= (u64)s.mem;	/* address independent */
			printf("%s %ld", e->name, k);
			for (int r = 0; r < 16; r++) {
				printf(" ");
				hex(s.hi[r], 16);
				hex(s.xmm[r], 16);
			}
			for (int r = 0; r < 16; r++)
				if (r != 4 && r < 13)
					printf(" %016llx", (unsigned long long)s.gp[r]);
			printf(" f%03llx m%04x ", (unsigned long long)(s.flags & 0xcd5), s.mxcsr);
			hex(s.mem, 512);
			printf("\n");
		}
	}
}

/* ---- region mode: a loop translated as one region, as the module does ---- */

static struct fa_rgn rgn;

static int rd_mem(void *ctx, u64 addr, u8 *buf, int n)
{
	(void)ctx;
	memcpy(buf, (const void *)(uintptr_t)addr, n);
	return n;
}

/* build the loop's region at code+0x20000; returns the address to call */
static u8 *build_region(const struct hb_ent *e)
{
	static const struct { u8 max_items, no_map; } tries[] = { { 0, 0 }, { 0, 1 }, { 1, 1 } };
	u8 *body = code + 0x20000 + (getenv("HB_BODYOFF") ? atoi(getenv("HB_BODYOFF")) : 0);	/* placement experiments */
	u8 *entry = code + 0x1e000, *thunk = code + 0x1f000;
	int ret = -1;

	thunk[0] = 0xc3;		/* every exit: back to hb_once / hb_loop */
	for (unsigned i = 0; i < 3 && ret < 0; i++) {	/* the module's fallbacks */
		memset(&rgn, 0, __builtin_offsetof(struct fa_rgn, len));
		rgn.read = rd_mem;
		rgn.entry_ip = (u64)e->start;
		rgn.out = body;
		rgn.cap = 0x10000;
		rgn.out_addr = (u64)body;
		rgn.blob_addr = (u64)blob;
		rgn.exit_thunk = (u64)thunk;
		rgn.max_items = tries[i].max_items;
		rgn.no_map = tries[i].no_map;
		rgn.no_sse41 = no_sse41;
		ret = fa_region_build(&rgn);
	}
	if (ret < 0 || rgn.it[0].kind != FA_R_OURS) {
		fprintf(stderr, "%s: no region\n", e->name);
		exit(2);
	}
	if (getenv("HB_DUMPREG")) {	/* the region body, for objdump -b binary */
		FILE *f = fopen(getenv("HB_DUMPREG"), "wb");

		if (f) {
			fwrite(body, 1, rgn.len, f);
			fclose(f);
		}
	}
	if (!rgn.has_map)
		return body + rgn.it[0].off;
	fa_emit_entry(entry, (u64)entry, &rgn.map, (u64)body + rgn.it[0].off);
	return entry;
}

/* the loop must not leave the region before its end: count instructions it holds */
static int region_items(const struct hb_ent *e)
{
	int n = 0;

	for (u32 i = 0; i < rgn.n; i++)
		n += rgn.it[i].ip >= (u64)e->start && rgn.it[i].ip < (u64)e->end;
	return n;
}

static void rdump(const char *filter, long cases)
{
	static struct hb_state s;

	for (const struct hb_ent *e = hb_loops; e < hb_loops_end; e++) {
		u8 *fn;

		if (!match(e->name, filter))
			continue;
		fn = build_region(e);
		for (long k = 0; k < cases; k++) {
			fill(&s, e->name, k);
			s.gp[12] = 1 + k % 50;	/* iterations */
			put_hi(&s);
			hb_once(&s, fn);
			get_hi(&s);
			s.gp[6] -= (u64)s.mem;
			printf("%s %ld", e->name, k);
			for (int r = 0; r < 16; r++) {
				printf(" ");
				hex(s.hi[r], 16);
				hex(s.xmm[r], 16);
			}
			for (int r = 0; r < 16; r++)
				if (r != 4 && r < 13)
					printf(" %016llx", (unsigned long long)s.gp[r]);
			printf(" f%03llx m%04x ", (unsigned long long)(s.flags & 0xcd5), s.mxcsr);
			hex(s.mem, 512);
			printf("\n");
		}
	}
}


static double cost(u8 *stub, u8 *empty, u64 n)
{
	/*
	 * The memory operand buffer at page offset HB_MEMOFF (default 0x800, as
	 * in fakeavx-prof): the state block's busy part is at 0x000-0x3ff, and a
	 * load at the same page offset as a recent store to it stalls (4K
	 * aliasing), so placement alone could decide a result.
	 */
	static u8 *page;
	static struct hb_state *sp;
	long off = getenv("HB_MEMOFF") ? strtol(getenv("HB_MEMOFF"), NULL, 0) : 0x800;
	u64 d[15];

	if (!page)
		page = aligned_alloc(4096, 3 * 4096);
	sp = (struct hb_state *)(page + 4096 + off - __builtin_offsetof(struct hb_state, mem));
#define s (*sp)

	/* the same inputs as fakeavx-prof's benchmarks (init_bench_state) */
	memset(&s, 0, sizeof(s));
	for (int r = 0; r < 16; r++)
		for (int i = 0; i < 8; i++) {
			float f = 1.0f + (r * 8 + i) / 128.0f;

			memcpy(i < 4 ? s.xmm[r] + 4 * i : s.hi[r] + 4 * (i - 4), &f, 4);
		}
	for (int i = 0; i < 8; i++) {	/* ymm5 mask, ymm3 blend selector, ymm6 indices */
		u32 m = i & 1 ? 0 : 0xffffffff, x = i & 1 ? 0 : 7 - i;

		memcpy(i < 4 ? s.xmm[5] + 4 * i : s.hi[5] + 4 * (i - 4), &m, 4);
		memcpy(i < 4 ? s.xmm[3] + 4 * i : s.hi[3] + 4 * (i - 4), &m, 4);
		memcpy(i < 4 ? s.xmm[6] + 4 * i : s.hi[6] + 4 * (i - 4), &x, 4);
	}
	for (int i = 0; i < 512 / 4; i++) {
		float f = 1.5f + i / 256.0f;

		memcpy(s.mem + 4 * i, &f, 4);
	}
	s.gp[0] = 0x123456789abcull;	/* rax */
	s.gp[1] = 0xdeadbeefcafef00dull;	/* rcx */
	s.gp[2] = 5;			/* rdx */
	s.gp[6] = (u64)s.mem;		/* rsi */
	s.gp[8] = 1; s.gp[9] = 2; s.gp[10] = 3; s.gp[11] = 4;
	s.flags = 0x202;
	s.mxcsr = 0x1f80;
	put_hi(&s);
	hb_loop(&s, stub, n);		/* warm */
	for (int r = 0; r < 15; r++) {
		u64 t0, t1, t2;

		t0 = rdtsc();
		hb_loop(&s, empty, n);
		t1 = rdtsc();
		hb_loop(&s, stub, n);
		t2 = rdtsc();
		d[r] = (t2 - t1) - (t1 - t0);
	}
	qsort(d, 15, sizeof(d[0]), cmp_u64);
	return (double)d[7] / ((double)n * UNROLL);
#undef s
}

/* cycles per loop iteration; and what the region looks like */
static void rtiming(const char *filter)
{
	struct hb_state *sp = placed_state();
#define s (*sp)

	printf("%-16s %10s %6s %8s %9s\n", "loop", "cyc/iter", "items", "borrowed", "covered");
	for (const struct hb_ent *e = hb_loops; e < hb_loops_end; e++) {
		u64 d[9];
		u8 *fn;
		int nin = 0;

		if (!match(e->name, filter))
			continue;
		fn = build_region(e);
		for (const u8 *p = e->start; p < e->end; p += fa_insn_len(p, e->end - p))
			nin++;
		fill(&s, "time", 0);
		s.gp[0] = 5; s.gp[2] = 7;	/* pcmpestr lengths */
		for (int r = 0; r < 9; r++) {
			u64 t0;

			s.gp[12] = 20000;
			put_hi(&s);
			t0 = rdtsc();
			hb_once(&s, fn);
			d[r] = rdtsc() - t0;
		}
		qsort(d, 9, sizeof(d[0]), cmp_u64);
		printf("%-16s %10.2f %6u %8d %5d/%-3d\n", e->name, (double)d[4] / 20000, rgn.n,
		       __builtin_popcount(rgn.has_map ? rgn.map.reserved : 0), region_items(e), nin);
	}
#undef s
}

static void timing(const char *filter)
{
	u8 *stub = code + 0x10000, *empty = code + 0xf000;

	empty[0] = 0xc3;
	printf("%-22s %10s\n", "instruction", "cycles");
	for (const struct hb_ent *e = hb_table; e < hb_table_end; e++) {
		if (!match(e->name, filter))
			continue;
		build(e, UNROLL, stub);
		printf("%-22s %10.2f\n", e->name, cost(stub, empty, 20000));
	}
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "time", *filter = argc > 2 ? argv[2] : NULL;
	cpu_set_t set;
	u64 v;

	no_sse41 = getenv("HB_NOSSE41") != NULL;
	code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	st = mmap(NULL, FA_ST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED || st == MAP_FAILED)
		return 1;
	blob = code;
	memcpy(blob, fa_blob, sizeof(fa_blob));
	v = (u64)st;
	memcpy(st + FA_ST_SELF, &v, 8);
	v = FA_MAGIC;
	memcpy(st + FA_ST_MAGIC, &v, 8);
	if (mode[0] == 'm') {		/* the module handles this process: no %gs of our own */
		setvbuf(stdout, NULL, _IOFBF, 1 << 20);
		if (!strcmp(mode, "mdump"))
			mdump(hb_table, hb_table_end, filter, argc > 3 ? atol(argv[3]) : 2000, 0);
		else if (!strcmp(mode, "mrtime"))
			mrtiming(filter);
		else
			mdump(hb_loops, hb_loops_end, filter, argc > 3 ? atol(argv[3]) : 2000, 1);
		return 0;
	}
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, st)) {
		perror("arch_prctl");
		return 1;
	}
	CPU_ZERO(&set);
	CPU_SET(sched_getcpu(), &set);
	sched_setaffinity(0, sizeof(set), &set);
	setvbuf(stdout, NULL, getenv("HB_UNBUF") ? _IONBF : _IOFBF, 1 << 20);
	if (!strcmp(mode, "dump"))
		dump(filter, argc > 3 ? atol(argv[3]) : 2000);
	else if (!strcmp(mode, "rdump"))
		rdump(filter, argc > 3 ? atol(argv[3]) : 2000);
	else if (!strcmp(mode, "rtime"))
		rtiming(filter);
	else
		timing(filter);
	return 0;
}
