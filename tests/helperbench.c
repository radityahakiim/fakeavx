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
 *   helperbench vexpair [N]      VEX vpcmpXstrX against their legacy encodings
 *   helperbench sdump "" [N]     pcmpXstrX, all control bytes: state dump for two builds
 *   helperbench mvexpair [N]     the same, executed under the loaded module
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

/*
 * vexpair: VEX.128 vpcmpXstrX against their legacy encodings, which go
 * through the same helper and are verified against hardware by difftest:
 * all four instructions x 256 control bytes x register/memory operand x
 * W=0/1 (explicit lengths), on string-like inputs. Results must be identical,
 * except that the VEX ...strm forms also zero ymm0[255:128].
 */
static int vexpair(long cases)
{
	static struct hb_state a, b;
	u8 *sl = code + 0x10000, *sv = code + 0x11000, ins[16];
	long n = 0, bad = 0;

	for (int op = 0x60; op <= 0x63; op++)
		for (int mem = 0; mem < 2; mem++)
			for (int w = 0; w < (op <= 0x61 ? 2 : 1); w++)
				for (int imm = 0; imm < 256; imm++) {
					struct hb_ent el = { "legacy", ins, 0 }, ev = { "vex", ins + 8, 0 };
					u8 modrm = mem ? 0x4e : 0xca;	/* xmm1, 16(%rsi) | xmm2 */
					int k = 0;

					/* legacy: 66 [48] 0f 3a op modrm [disp8] imm */
					ins[k++] = 0x66;
					if (w)
						ins[k++] = 0x48;
					ins[k++] = 0x0f; ins[k++] = 0x3a; ins[k++] = op; ins[k++] = modrm;
					if (mem)
						ins[k++] = 0x10;
					ins[k++] = imm;
					el.end = ins + k;
					/* VEX3: c4 e3 (W vvvv=1111 L=0 pp=01) op modrm [disp8] imm */
					k = 8;
					ins[k++] = 0xc4; ins[k++] = 0xe3; ins[k++] = (w << 7) | 0x79;
					ins[k++] = op; ins[k++] = modrm;
					if (mem)
						ins[k++] = 0x10;
					ins[k++] = imm;
					ev.end = ins + k;
					build(&el, 1, sl);
					build(&ev, 1, sv);
					for (long c = 0; c < cases; c++, n++) {
						fill(&a, "vexpair", c * 1000 + imm);
						for (int i = 0; i < 16; i++) {	/* short alphabets, zeros */
							a.xmm[1][i] = rnd() % 5;
							a.xmm[2][i] = rnd() % 5;
							a.mem[16 + i] = rnd() % 5;
						}
						a.gp[0] = (u64)(s64)((int)(rnd() % 41) - 20);
						a.gp[2] = (u64)(s64)((int)(rnd() % 41) - 20);
						if (c % 16 == 5)
							a.gp[0] = 0x80000000u;	/* |INT_MIN| saturation */
						if (c % 16 == 9)
							a.gp[2] = 0xffffffff00000003ull;	/* W decides */
						b = a;
						put_hi(&a);
						hb_once(&a, sl);
						get_hi(&a);
						put_hi(&b);
						hb_once(&b, sv);
						get_hi(&b);
						if (!(op & 1))
							memset(a.hi[0], 0, 16);	/* what VEX must do */
						if (memcmp(&a, &b, sizeof(a))) {
							if (bad++ < 5)
								printf("MISMATCH op %#x imm %#04x mem %d W %d case %ld\n",
								       op, imm, mem, w, c);
						}
					}
				}
	/* what the hardware rejects (#UD) the decoder must reject too */
	for (int op = 0x60; op <= 0x63; op++) {
		const u8 ok[] = { 0xc4, 0xe3, 0x79, op, 0xca, 0x0c };
		const u8 l1[] = { 0xc4, 0xe3, 0x7d, op, 0xca, 0x0c };		/* VEX.L = 1 */
		const u8 vv[] = { 0xc4, 0xe3, 0x71, op, 0xca, 0x0c };		/* vvvv = xmm1 */
		struct fa_insn in;

		if (fa_decode(ok, sizeof(ok), 0, &in) || in.len != 6) {
			printf("valid encoding of op %#x rejected\n", op);
			bad++;
		}
		if (!fa_decode(l1, sizeof(l1), 0, &in) || !fa_decode(vv, sizeof(vv), 0, &in)) {
			printf("invalid encoding of op %#x (L=1 or vvvv != 1111) accepted\n", op);
			bad++;
		}
	}
	printf("vexpair: %ld cases (4 instructions x 256 control bytes x reg/mem x W), %ld mismatches;"
	       " L=1 and vvvv!=1111 rejected\n", n, bad);
	return bad != 0;
}

/*
 * mvexpair: the same comparison, but both encodings executed natively, so
 * that the loaded fakeavx module decodes and translates them. The code goes
 * into an anonymous executable page (rewritten for every case; the module
 * notices changed bytes and translates again).
 */
static int mvexpair(long cases)
{
	static struct hb_state a, b;
	u8 *pl = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	u8 *pv = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	long n = 0, bad = 0;

	if (pl == MAP_FAILED || pv == MAP_FAILED)
		return 2;
	for (int op = 0x60; op <= 0x63; op++)
		for (int mem = 0; mem < 2; mem++)
			for (int w = 0; w < (op <= 0x61 ? 2 : 1); w++)
				for (int imm = 0; imm < 256; imm++) {
					u8 modrm = mem ? 0x4e : 0xca;
					int k = 0;

					pl[k++] = 0x66;
					if (w)
						pl[k++] = 0x48;
					pl[k++] = 0x0f; pl[k++] = 0x3a; pl[k++] = op; pl[k++] = modrm;
					if (mem)
						pl[k++] = 0x10;
					pl[k++] = imm;
					pl[k] = 0xc3;
					k = 0;
					pv[k++] = 0xc4; pv[k++] = 0xe3; pv[k++] = (w << 7) | 0x79;
					pv[k++] = op; pv[k++] = modrm;
					if (mem)
						pv[k++] = 0x10;
					pv[k++] = imm;
					pv[k] = 0xc3;
					for (long c = 0; c < cases; c++, n++) {
						fill(&a, "vexpair", c * 1000 + imm);
						for (int i = 0; i < 16; i++) {
							a.xmm[1][i] = rnd() % 5;
							a.xmm[2][i] = rnd() % 5;
							a.mem[16 + i] = rnd() % 5;
						}
						a.gp[0] = (u64)(s64)((int)(rnd() % 41) - 20);
						a.gp[2] = (u64)(s64)((int)(rnd() % 41) - 20);
						if (c % 16 == 5)
							a.gp[0] = 0x80000000u;
						if (c % 16 == 9)
							a.gp[2] = 0xffffffff00000003ull;
						b = a;
						hb_mrun(&a, pl);
						hb_mrun(&b, pv);
						if (!(op & 1))
							memset(a.hi[0], 0, 16);
						if (memcmp(&a, &b, sizeof(a)) && bad++ < 5)
							printf("MISMATCH op %#x imm %#04x mem %d W %d case %ld\n", op, imm, mem, w, c);
					}
				}
	printf("mvexpair (under the module): %ld cases, %ld mismatches\n", n, bad);
	return bad != 0;
}

/*
 * strcost: cycles per pcmpXstrX by aggregation mode, element size and
 * string length (both operands the same length; implicit forms get a
 * terminating zero there, explicit forms the length in eax/edx).
 */
static void strcost(void)
{
	static const char *agg[] = { "equal-any", "ranges", "equal-each", "equal-ordered" };
	static const char *opn[] = { "pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri" };
	u8 *stub = code + 0x10000, *empty = code + 0xf000, ins[16];
	struct hb_state *sp = placed_state();

	empty[0] = 0xc3;
	printf("cycles per instruction (VEX and legacy forms share the helper)\n");
	printf("%-10s %-13s %-5s %9s %9s %9s\n", "insn", "mode", "elem", "len 3", "len half", "full");
	for (int op = 0x60; op <= 0x63; op++)
		for (int a = 0; a < 4; a++)
			for (int fmt = 0; fmt < 2; fmt++) {
				int n = fmt ? 8 : 16;
				double c[3];

				for (int L = 0; L < 3; L++) {
					int len = L == 0 ? 3 : L == 1 ? n / 2 : n;
					u8 b[16];
					struct hb_ent e = { "s", ins, ins + 6 };

					/* 66 0f 3a op modrm(xmm1, xmm2) imm */
					ins[0] = 0x66; ins[1] = 0x0f; ins[2] = 0x3a; ins[3] = op; ins[4] = 0xca;
					ins[5] = (u8)(fmt | a << 2);
					build(&e, UNROLL, stub);
					memset(sp, 0, sizeof(*sp));
					for (int i = 0; i < 16; i++)
						b[i] = 'a' + i % 7;
					if (len < n) {		/* terminator for the implicit forms */
						b[fmt ? 2 * len : len] = 0;
						if (fmt)
							b[2 * len + 1] = 0;
					}
					memcpy(sp->xmm[1], b, 16);
					memcpy(sp->xmm[2], b, 16);
					sp->gp[0] = sp->gp[2] = len;
					sp->gp[6] = (u64)sp->mem;
					sp->flags = 0x202;
					sp->mxcsr = 0x1f80;
					put_hi(sp);
					{
						u64 d[9], tl, tm;

						hb_loop(sp, stub, 2000);
						for (int r = 0; r < 9; r++) {
							u64 t0 = rdtsc();

							hb_loop(sp, empty, 2000);
							tm = rdtsc();
							hb_loop(sp, stub, 2000);
							tl = rdtsc();
							d[r] = (tl - tm) - (tm - t0);
						}
						qsort(d, 9, sizeof(d[0]), cmp_u64);
						c[L] = (double)d[4] / (2000.0 * UNROLL);
					}
				}
				printf("%-10s %-13s %-5s %9.0f %9.0f %9.0f\n", opn[op - 0x60], agg[a],
				       fmt ? "word" : "byte", c[0], c[1], c[2]);
			}
}

/*
 * sdump: complete state after legacy pcmpXstrX for every instruction x
 * control byte x register/memory operand x W, on string-like inputs (short
 * alphabets with zeros, signed and wide values, lengths -20..20, INT_MIN,
 * W-dependent upper halves). Compare two builds' outputs (helper_oracle.sh).
 */
static void sdump(long cases)
{
	static struct hb_state a;
	u8 *stub = code + 0x10000, ins[16];

	for (int op = 0x60; op <= 0x63; op++)
		for (int mem = 0; mem < 2; mem++)
			for (int w = 0; w < (op <= 0x61 ? 2 : 1); w++)
				for (int imm = 0; imm < 256; imm++) {
					struct hb_ent e = { "s", ins, 0 };
					int k = 0;

					ins[k++] = 0x66;
					if (w)
						ins[k++] = 0x48;
					ins[k++] = 0x0f; ins[k++] = 0x3a; ins[k++] = op; ins[k++] = mem ? 0x4e : 0xca;
					if (mem)
						ins[k++] = 0x10;
					ins[k++] = imm;
					e.end = ins + k;
					build(&e, 1, stub);
					for (long c = 0; c < cases; c++) {
						int alpha = c % 4 == 0 ? 256 : c % 4 == 1 ? 5 : c % 4 == 2 ? 3 : 130;
						char name[48];

						fill(&a, "sdump", c * 1000 + imm);
						for (int i = 0; i < 16; i++) {
							/* alphabet 130: values around 0x7f/0x80, signed edges */
							int v1 = rnd() % alpha, v2 = rnd() % alpha, v3 = rnd() % alpha;

							a.xmm[1][i] = alpha == 130 ? 0x7f + v1 % 3 - (v1 & 0x80) : v1;
							a.xmm[2][i] = alpha == 130 ? 0x7f + v2 % 3 - (v2 & 0x80) : v2;
							a.mem[16 + i] = alpha == 130 ? 0x7f + v3 % 3 - (v3 & 0x80) : v3;
						}
						if (c % 7 == 3)		/* no terminator at all */
							for (int i = 0; i < 16; i++)
								a.xmm[1][i] |= 1, a.xmm[2][i] |= 1, a.mem[16 + i] |= 1;
						a.gp[0] = (u64)(s64)((int)(rnd() % 41) - 20);
						a.gp[2] = (u64)(s64)((int)(rnd() % 41) - 20);
						if (c % 16 == 5)
							a.gp[0] = 0x80000000u;
						if (c % 16 == 9)
							a.gp[2] = 0xffffffff00000003ull;
						put_hi(&a);
						hb_once(&a, stub);
						get_hi(&a);
						snprintf(name, sizeof(name), "op%x/m%d/w%d/imm%02x", op, mem, w, imm);
						print_state(name, c, &a);
					}
				}
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
		if (!strcmp(mode, "mvexpair"))
			return mvexpair(argc > 2 ? atol(argv[2]) : 50);
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
	if (!strcmp(mode, "sdump")) {
		sdump(argc > 3 ? atol(argv[3]) : 40);	/* sdump "" N, like the other modes */
		return 0;
	}
	if (!strcmp(mode, "strcost")) {
		strcost();
		return 0;
	}
	if (!strcmp(mode, "strhttp")) {	/* picohttpparser's form: pcmpestri $4/$0x14, la = range bytes, lb = 16 */
		u8 *stub = code + 0x10000, *empty = code + 0xf000, ins[8];
		struct hb_state *sp = placed_state();
		const char *req = "Host: example.com\r\nUser-Agent: bench/1.0\r\nAccept: */*\r\n";

		empty[0] = 0xc3;
		printf("%-6s %4s %s\n", "imm", "la", "cycles (lb = 16, HTTP header text)");
		for (int imm = 0x04; imm <= 0x14; imm += 0x10)
			for (int la = 2; la <= 16; la += la < 8 ? 2 : 8) {
				struct hb_ent e = { "h", ins, ins + 6 };
				u64 d[9];

				ins[0] = 0x66; ins[1] = 0x0f; ins[2] = 0x3a; ins[3] = 0x61; ins[4] = 0xca; ins[5] = imm;
				build(&e, UNROLL, stub);
				memset(sp, 0, sizeof(*sp));
				memcpy(sp->xmm[0], "\x00 \x7f\x7f\"\"\x3a\x3a\r\r\n\n,,;;", 16);	/* ranges */
				memcpy(sp->xmm[2], req, 16);
				sp->gp[0] = la;
				sp->gp[2] = 16;
				sp->gp[6] = (u64)sp->mem;
				sp->flags = 0x202;
				sp->mxcsr = 0x1f80;
				/* the instruction above names xmm1 as the ranges: copy them there */
				memcpy(sp->xmm[1], sp->xmm[0], 16);
				put_hi(sp);
				hb_loop(sp, stub, 2000);
				for (int r = 0; r < 9; r++) {
					u64 t0 = rdtsc(), tm, tl;

					hb_loop(sp, empty, 2000);
					tm = rdtsc();
					hb_loop(sp, stub, 2000);
					tl = rdtsc();
					d[r] = (tl - tm) - (tm - t0);
				}
				qsort(d, 9, sizeof(d[0]), cmp_u64);
				printf("%#-6x %4d %8.0f\n", imm, la, (double)d[4] / (2000.0 * UNROLL));
			}
		return 0;
	}
	if (!strcmp(mode, "vexpair"))
		return vexpair(argc > 2 ? atol(argv[2]) : 50);
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
