// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: out-of-line implementations of instructions with no cheap SSE
 * equivalent. Runs in user mode inside the helper blob. No libc, no writable
 * data, no relocations.
 */
#include "helper.h"

/* bit 0: less, bit 1: equal, bit 2: greater, bit 3: unordered */
static const u8 cmp_truth[16] = {
	0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
	0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
};

static int rel_f(float a, float b)
{
	return a < b ? 1 : a == b ? 2 : a > b ? 4 : 8;
}

static int rel_d(double a, double b)
{
	return a < b ? 1 : a == b ? 2 : a > b ? 4 : 8;
}

static __attribute__((noinline)) void do_cmp(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	u8 truth = cmp_truth[in->imm & 0xf];
	int dbl = in->pp & 1, scalar = in->pp >= 2;
	int L = in->L && !scalar, n, i;
	vec a, b, r;

	get_reg(st, in->vvvv, &a);
	memset(&b, 0, sizeof(b));
	get_rm(in, gpr, st, &b, scalar ? (dbl ? 8 : 4) : (L ? 32 : 16));
	r = a;
	if (dbl) {
		n = scalar ? 1 : (L ? 4 : 2);
		for (i = 0; i < n; i++)
			r.sq[i] = (truth & rel_d(a.df[i], b.df[i])) ? -1 : 0;
	} else {
		n = scalar ? 1 : (L ? 8 : 4);
		for (i = 0; i < n; i++)
			r.sd[i] = (truth & rel_f(a.f[i], b.f[i])) ? -1 : 0;
	}
	set_reg(st, in->reg, &r, L);
}

/* ---- SSE4.2, legacy encoding: the upper ymm halves are left alone ---- */


/*
 * ---- SSE4.1, for hosts that only have SSSE3 ----
 * Used for the legacy encodings (two operands, upper ymm halves untouched,
 * blendv mask in xmm0) and for the VEX encodings (three operands, 128 or
 * 256 bits). Floating-point exception flags are not reproduced.
 */
typedef union {
	u8 b[16];
	s8 sb[16];
	u16 w[8];
	s16 sw[8];
	u32 d[4];
	s32 sd[4];
	u64 q[2];
	s64 sq[2];
	float f[4];
	double df[2];
} lane;

static u32 get_mxcsr(void)
{
	u32 v;

	__asm__("stmxcsr %0" : "=m"(v));
	return v;
}

static int round_mode(int imm)
{
	return (imm & 4) ? (get_mxcsr() >> 13) & 3 : imm & 3;
}

/* mode: 0 nearest even, 1 down, 2 up, 3 truncate */
static float round_f(float x, int mode)
{
	u32 u, e;
	float t, r;

	memcpy(&u, &x, 4);
	e = (u >> 23) & 0xff;
	if (e == 0xff) {
		if (u & 0x7fffff)
			u |= 0x400000;	/* quiet a NaN */
		memcpy(&x, &u, 4);
		return x;
	}
	if (e >= 127 + 23)
		return x;
	t = (float)(s32)x;
	r = t;
	if (mode == 1) {
		if (t > x)
			r = t - 1.0f;
	} else if (mode == 2) {
		if (t < x)
			r = t + 1.0f;
	} else if (mode == 0) {
		float d = x - t;

		if (d > 0.5f || (d == 0.5f && ((s32)t & 1)))
			r = t + 1.0f;
		else if (d < -0.5f || (d == -0.5f && ((s32)t & 1)))
			r = t - 1.0f;
	}
	if (r == 0.0f) {
		u &= 0x80000000;
		memcpy(&r, &u, 4);
	}
	return r;
}

static double round_d(double x, int mode)
{
	u64 u, e;
	double t, r;

	memcpy(&u, &x, 8);
	e = (u >> 52) & 0x7ff;
	if (e == 0x7ff) {
		if (u & 0xfffffffffffffULL)
			u |= 0x8000000000000ULL;
		memcpy(&x, &u, 8);
		return x;
	}
	if (e >= 1023 + 52)
		return x;
	t = (double)(s64)x;
	r = t;
	if (mode == 1) {
		if (t > x)
			r = t - 1.0;
	} else if (mode == 2) {
		if (t < x)
			r = t + 1.0;
	} else if (mode == 0) {
		double d = x - t;

		if (d > 0.5 || (d == 0.5 && ((s64)t & 1)))
			r = t + 1.0;
		else if (d < -0.5 || (d == -0.5 && ((s64)t & 1)))
			r = t - 1.0;
	}
	if (r == 0.0) {
		u &= 0x8000000000000000ULL;
		memcpy(&r, &u, 8);
	}
	return r;
}

/*
 * Operand order decides which NaN payload survives, and the compiler is free
 * to swap the operands of a commutative operation: pin them down.
 */
static float mul_f(float a, float b) { __asm__("mulss %1, %0" : "+x"(a) : "x"(b)); return a; }
static float add_f(float a, float b) { __asm__("addss %1, %0" : "+x"(a) : "x"(b)); return a; }
static double mul_d(double a, double b) { __asm__("mulsd %1, %0" : "+x"(a) : "x"(b)); return a; }
static double add_d(double a, double b) { __asm__("addsd %1, %0" : "+x"(a) : "x"(b)); return a; }

#define EACH(n, expr) do { for (i = 0; i < (n); i++) expr; } while (0)

/* one 128-bit lane of a lane-wise operation */
static void lane_op(int map, int opc, const lane *a, const lane *b, int imm, lane *r)
{
	int i;

	*r = *a;
	if (map == 2) {
		switch (opc) {
		case 0x28: EACH(2, r->sq[i] = (s64)a->sd[2 * i] * b->sd[2 * i]); break;
		case 0x29: EACH(2, r->sq[i] = a->q[i] == b->q[i] ? -1 : 0); break;
		case 0x2b:
			EACH(4, r->w[i] = a->sd[i] < 0 ? 0 : a->sd[i] > 0xffff ? 0xffff : a->sd[i]);
			EACH(4, r->w[4 + i] = b->sd[i] < 0 ? 0 : b->sd[i] > 0xffff ? 0xffff : b->sd[i]);
			break;
		case 0x38: EACH(16, r->sb[i] = a->sb[i] < b->sb[i] ? a->sb[i] : b->sb[i]); break;
		case 0x39: EACH(4, r->sd[i] = a->sd[i] < b->sd[i] ? a->sd[i] : b->sd[i]); break;
		case 0x3a: EACH(8, r->w[i] = a->w[i] < b->w[i] ? a->w[i] : b->w[i]); break;
		case 0x3b: EACH(4, r->d[i] = a->d[i] < b->d[i] ? a->d[i] : b->d[i]); break;
		case 0x3c: EACH(16, r->sb[i] = a->sb[i] > b->sb[i] ? a->sb[i] : b->sb[i]); break;
		case 0x3d: EACH(4, r->sd[i] = a->sd[i] > b->sd[i] ? a->sd[i] : b->sd[i]); break;
		case 0x3e: EACH(8, r->w[i] = a->w[i] > b->w[i] ? a->w[i] : b->w[i]); break;
		case 0x3f: EACH(4, r->d[i] = a->d[i] > b->d[i] ? a->d[i] : b->d[i]); break;
		case 0x40: EACH(4, r->d[i] = a->d[i] * b->d[i]); break;
		default: __builtin_trap();
		}
		return;
	}
	switch (opc) {
	case 0x08: EACH(4, r->f[i] = round_f(b->f[i], round_mode(imm))); break;
	case 0x09: EACH(2, r->df[i] = round_d(b->df[i], round_mode(imm))); break;
	case 0x02:
	case 0x0c: EACH(4, r->d[i] = (imm >> i) & 1 ? b->d[i] : a->d[i]); break;
	case 0x0d: EACH(2, r->q[i] = (imm >> i) & 1 ? b->q[i] : a->q[i]); break;
	case 0x0e: EACH(8, r->w[i] = (imm >> i) & 1 ? b->w[i] : a->w[i]); break;
	case 0x40: {
		/*
		 * Real CPUs sum the products with a different operand order
		 * for every result element (measured: it decides which NaN
		 * payload comes out).
		 */
		float t[4], s[4];

		EACH(4, t[i] = (imm >> (4 + i)) & 1 ? mul_f(a->f[i], b->f[i]) : 0.0f);
		s[0] = add_f(add_f(t[1], t[0]), add_f(t[3], t[2]));
		s[1] = add_f(add_f(t[0], t[1]), add_f(t[2], t[3]));
		s[2] = add_f(add_f(t[3], t[2]), add_f(t[1], t[0]));
		s[3] = add_f(add_f(t[2], t[3]), add_f(t[0], t[1]));
		EACH(4, r->f[i] = (imm >> i) & 1 ? s[i] : 0.0f);
		break;
	}
	case 0x41: {
		double t0 = (imm & 0x10) ? mul_d(a->df[0], b->df[0]) : 0.0;
		double t1 = (imm & 0x20) ? mul_d(a->df[1], b->df[1]) : 0.0;

		r->df[0] = (imm & 1) ? add_d(t0, t1) : 0.0;
		r->df[1] = (imm & 2) ? add_d(t1, t0) : 0.0;
		break;
	}
	case 0x42: {
		int ao = (imm & 4) ? 4 : 0, bo = (imm & 3) * 4, k;

		for (i = 0; i < 8; i++) {
			unsigned s = 0;

			for (k = 0; k < 4; k++) {
				int d = a->b[ao + i + k] - b->b[bo + k];

				s += d < 0 ? -d : d;
			}
			r->w[i] = s;
		}
		break;
	}
	default: __builtin_trap();
	}
}

static void put_lanes(const struct fa_insn *in, u8 *st, const lane *r, int legacy)
{
	memcpy(st + FA_ST_LO + 16 * in->reg, r[0].b, 16);
	if (legacy)
		return;
	if (in->L)
		memcpy(st + FA_ST_HI + 16 * in->reg, r[1].b, 16);
	else
		memset(st + FA_ST_HI + 16 * in->reg, 0, 16);
}

static __attribute__((noinline)) void do_sse41(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st,
						    int legacy)
{
	int lanes = (!legacy && in->L) ? 2 : 1, A = legacy ? in->reg : in->vvvv;
	int map = in->map, opc = in->opcode, imm = in->imm, l, i;
	u8 *mem = in->mod != 3 ? ea(in, gpr, 0, 0) : NULL;
	vec va, vb;
	lane a[2], b[2], r[2];

	get_reg(st, A, &va);
	memcpy(a, va.b, 32);
	memset(b, 0, sizeof(b));
	if (!mem) {
		get_reg(st, in->rm, &vb);
		memcpy(b, vb.b, 32);
	}
	r[0] = a[0];
	r[1] = a[1];

	if (map == 2 && opc == 0x17) {		/* ptest */
		do_test(in, gpr, st, flags);
		return;
	}
	if (map == 3 && opc >= 0x14 && opc <= 0x17) {	/* pextrb/w/d/q, extractps: reg -> r/m */
		u64 v;
		int size;

		get_reg(st, in->reg, &va);
		if (opc == 0x14) {
			v = va.b[imm & 15];
			size = 1;
		} else if (opc == 0x15) {
			v = va.b[2 * (imm & 7)] | (va.b[2 * (imm & 7) + 1] << 8);
			size = 2;
		} else if (opc == 0x16 && in->W) {
			v = va.q[imm & 1];
			size = 8;
		} else {
			v = va.d[imm & 3];
			size = 4;
		}
		if (mem)
			memcpy(mem, &v, size);
		else
			gpr[in->rm] = v;
		return;
	}
	if (map == 3 && (opc == 0x20 || opc == 0x22)) {	/* pinsrb/d/q: gpr or memory source */
		int size = opc == 0x20 ? 1 : in->W ? 8 : 4;
		u64 v = 0;

		if (mem)
			memcpy(&v, mem, size);
		else
			v = gpr[in->rm];
		if (size == 1)
			r[0].b[imm & 15] = v;
		else if (size == 4)
			r[0].d[imm & 3] = v;
		else
			r[0].q[imm & 1] = v;
		put_lanes(in, st, r, legacy);
		return;
	}
	if (map == 3 && opc == 0x21) {		/* insertps */
		u32 v;

		if (mem)
			memcpy(&v, mem, 4);
		else
			v = b[0].d[imm >> 6];
		r[0].d[(imm >> 4) & 3] = v;
		for (i = 0; i < 4; i++)
			if (imm & (1 << i))
				r[0].d[i] = 0;
		put_lanes(in, st, r, legacy);
		return;
	}
	if (map == 3 && (opc == 0x0a || opc == 0x0b)) {	/* roundss / roundsd */
		if (mem)
			memcpy(b, mem, opc == 0x0a ? 4 : 8);
		if (opc == 0x0a)
			r[0].f[0] = round_f(b[0].f[0], round_mode(imm));
		else
			r[0].df[0] = round_d(b[0].df[0], round_mode(imm));
		put_lanes(in, st, r, legacy);
		return;
	}
	if (map == 2 && ((opc >= 0x20 && opc <= 0x25) || (opc >= 0x30 && opc <= 0x35))) {
		/* pmovsx / pmovzx: bw bd bq wd wq dq */
		static const u8 from[6] = { 1, 1, 1, 2, 2, 4 }, to[6] = { 2, 4, 8, 4, 8, 8 };
		int v = opc & 7, sx = opc < 0x30, n = 16 / to[v] * lanes;
		u8 src[32];
		u8 out[32];

		if (mem)
			memcpy(src, mem, n * from[v]);
		else
			memcpy(src, b, 16);
		for (i = 0; i < n; i++) {
			const u8 *e = src + i * from[v];
			u64 x = from[v] == 1 ? e[0] : from[v] == 2 ? *(const u16 *)e : *(const u32 *)e;
			u8 *o = out + i * to[v];

			if (sx)
				x = from[v] == 1 ? (u64)(s64)(s8)x : from[v] == 2 ? (u64)(s64)(s16)x
										: (u64)(s64)(s32)x;
			if (to[v] == 2)
				*(u16 *)o = x;
			else if (to[v] == 4)
				*(u32 *)o = x;
			else
				*(u64 *)o = x;
		}
		memcpy(r, out, 16 * lanes);
		put_lanes(in, st, r, legacy);
		return;
	}
	if (mem)
		memcpy(b, mem, 16 * lanes);
	if (map == 2 && opc == 0x2a) {		/* movntdqa */
		put_lanes(in, st, b, legacy);
		return;
	}
	if (map == 2 && opc == 0x41) {		/* phminposuw */
		int best = 0;

		for (i = 1; i < 8; i++)
			if (b[0].w[i] < b[0].w[best])
				best = i;
		memset(&r[0], 0, 16);
		r[0].w[0] = b[0].w[best];
		r[0].w[1] = best;
		put_lanes(in, st, r, legacy);
		return;
	}
	if (map == 2 && (opc == 0x10 || opc == 0x14 || opc == 0x15)) {
		/* legacy pblendvb / blendvps / blendvpd: mask is xmm0 */
		lane m;

		memcpy(m.b, st + FA_ST_LO, 16);
		if (opc == 0x10)
			EACH(16, r[0].b[i] = m.sb[i] < 0 ? b[0].b[i] : a[0].b[i]);
		else if (opc == 0x14)
			EACH(4, r[0].d[i] = m.sd[i] < 0 ? b[0].d[i] : a[0].d[i]);
		else
			EACH(2, r[0].q[i] = m.sq[i] < 0 ? b[0].q[i] : a[0].q[i]);
		put_lanes(in, st, r, legacy);
		return;
	}
	for (l = 0; l < lanes; l++) {
		int li = imm;

		if (map == 3 && (opc == 0x0c || opc == 0x02))
			li = (imm >> (4 * l)) & 0xf;
		else if (map == 3 && opc == 0x0d)
			li = (imm >> (2 * l)) & 3;
		else if (map == 3 && opc == 0x42)
			li = (imm >> (3 * l)) & 7;
		lane_op(map, opc, &a[l], &b[l], li, &r[l]);
	}
	put_lanes(in, st, r, legacy);
}

/*
 * Floating-point work in C sets MXCSR exception flags the emulated
 * instruction may not set (and the C compare uses comisd, which signals on
 * any NaN). Only do_cmp and do_sse41 (with round_f/round_d) contain such
 * instructions -- checked on the compiled blob -- so only they pay for
 * saving and restoring MXCSR (ldmxcsr costs ~50 cycles on Core 2). They are
 * noinline: their floating-point instructions all execute inside the call,
 * which the "memory" clobbers order between the save and the restore.
 */
static inline u32 mxcsr_save(void)
{
	u32 m;

	__asm__ volatile("stmxcsr %0" : "=m"(m) :: "memory");
	return m;
}

/* ldmxcsr only if something changed it: same result, and usually nothing did */
static inline void mxcsr_restore(u32 m)
{
	u32 now;

	__asm__ volatile("stmxcsr %0" : "=m"(now) :: "memory");
	if (now != m)
		__asm__ volatile("ldmxcsr %0" :: "m"(m) : "memory");
}

static void fp_cmp(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	u32 m = mxcsr_save();

	do_cmp(in, gpr, st);
	mxcsr_restore(m);
}

static void fp_sse41(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st, int legacy)
{
	u32 m = mxcsr_save();

	do_sse41(in, gpr, flags, st, legacy);
	mxcsr_restore(m);
}

/* everything, with all xmm registers saved (floating point needs them) */
void fa_helper_c(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st)
{
	if (fa_helper_lean(in)) {
		fa_helper_lean_c(in, gpr, flags, st);
		return;
	}
	if (in->kind == FA_K_SSE4)
		fp_sse41(in, gpr, flags, st, 1);
	else if (in->map == 1 && in->opcode == 0xc2)
		fp_cmp(in, gpr, st);
	else
		fp_sse41(in, gpr, flags, st, 0);	/* SSSE3-only host */
}
