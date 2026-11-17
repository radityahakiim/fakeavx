// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: out-of-line implementations of instructions with no cheap SSE
 * equivalent. Runs in user mode inside the helper blob. No libc, no writable
 * data, no relocations.
 */
#include "../core/fakeavx.h"

typedef union {
	u8 b[32];
	u32 d[8];
	u64 q[4];
	s32 sd[8];
	s64 sq[4];
	float f[8];
	double df[4];
} vec;

void *memcpy(void *d, const void *s, size_t n)
{
	u8 *dd = d;
	const u8 *ss = s;

	while (n--)
		*dd++ = *ss++;
	return d;
}

void *memset(void *d, int ch, size_t n)
{
	u8 *dd = d;

	while (n--)
		*dd++ = ch;
	return d;
}

static void get_reg(u8 *st, int r, vec *v)
{
	memcpy(v->b, st + FA_ST_LO + 16 * r, 16);
	memcpy(v->b + 16, st + FA_ST_HI + 16 * r, 16);
}

static void set_reg(u8 *st, int r, const vec *v, int L)
{
	memcpy(st + FA_ST_LO + 16 * r, v->b, 16);
	if (L)
		memcpy(st + FA_ST_HI + 16 * r, v->b + 16, 16);
	else
		memset(st + FA_ST_HI + 16 * r, 0, 16);
}

static u64 seg_base(const struct fa_insn *in)
{
	u64 base = 0;

	if (in->mem.seg == 0x64)	/* x86-64 TLS ABI: %fs:0 holds the fs base */
		__asm__("mov %%fs:0, %0" : "=r"(base));
	return base;
}

/* effective address; idx is the already sign-extended VSIB index or 0 */
static u8 *ea(const struct fa_insn *in, const u64 *gpr, int vsib, s64 idx)
{
	const struct fa_mem *m = &in->mem;
	u64 a;

	if (m->riprel) {
		a = in->rip + in->len + (s64)m->disp;
	} else {
		a = (s64)m->disp;
		if (m->has_base)
			a += gpr[m->base];
		if (vsib)
			a += (u64)idx << m->scale;
		else if (m->has_index)
			a += gpr[m->index] << m->scale;
	}
	if (m->addr32)
		a = (u32)a;
	return (u8 *)(a + seg_base(in));
}

static void get_rm(const struct fa_insn *in, const u64 *gpr, u8 *st, vec *v, int n)
{
	if (in->mod == 3)
		get_reg(st, in->rm, v);
	else
		memcpy(v->b, ea(in, gpr, 0, 0), n);
}

static void set_flags(u64 *flags, int zf, int cf)
{
	*flags = (*flags & ~0x8d5ULL) | (zf ? 0x40 : 0) | (cf ? 1 : 0);
}

static void do_gather(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int ds = in->W ? 8 : 4, is = (in->opcode & 1) ? 8 : 4;
	int vl = in->L ? 32 : 16;
	int n = vl / (ds > is ? ds : is), i;
	vec dst, mask, idx, res;

	get_reg(st, in->reg, &dst);
	get_reg(st, in->vvvv, &mask);
	get_reg(st, in->mem.index, &idx);
	memset(&res, 0, sizeof(res));
	memcpy(res.b, dst.b, n * ds);
	for (i = 0; i < n; i++) {
		s64 ix = is == 8 ? idx.sq[i] : idx.sd[i];
		int sel = ds == 8 ? (mask.sq[i] < 0) : (mask.sd[i] < 0);

		if (sel)
			memcpy(res.b + i * ds, ea(in, gpr, 1, ix), ds);
	}
	memset(&mask, 0, sizeof(mask));
	set_reg(st, in->vvvv, &mask, 0);
	set_reg(st, in->reg, &res, in->L);
}

static void do_maskmov(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int ds, n, i, store;
	vec mask, data, res;
	u8 *m = ea(in, gpr, 0, 0);

	if (in->opcode == 0x8c || in->opcode == 0x8e) {
		ds = in->W ? 8 : 4;
		store = in->opcode == 0x8e;
	} else {
		ds = (in->opcode & 1) ? 8 : 4;
		store = in->opcode >= 0x2e;
	}
	n = (in->L ? 32 : 16) / ds;
	get_reg(st, in->vvvv, &mask);
	get_reg(st, in->reg, &data);
	memset(&res, 0, sizeof(res));
	for (i = 0; i < n; i++) {
		int sel = ds == 8 ? (mask.sq[i] < 0) : (mask.sd[i] < 0);

		if (!sel)
			continue;
		if (store)
			memcpy(m + i * ds, data.b + i * ds, ds);
		else
			memcpy(res.b + i * ds, m + i * ds, ds);
	}
	if (!store)
		set_reg(st, in->reg, &res, in->L);
}

static void do_varshift(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int n = in->L ? 8 : 4, i;
	vec a, b, r;

	get_reg(st, in->vvvv, &a);
	get_rm(in, gpr, st, &b, in->L ? 32 : 16);
	memset(&r, 0, sizeof(r));
	if (in->W) {
		for (i = 0; i < n / 2; i++) {
			u64 cnt = b.q[i];

			if (in->opcode == 0x45)
				r.q[i] = cnt < 64 ? a.q[i] >> cnt : 0;
			else
				r.q[i] = cnt < 64 ? a.q[i] << cnt : 0;
		}
	} else {
		for (i = 0; i < n; i++) {
			u32 cnt = b.d[i];

			if (in->opcode == 0x45)
				r.d[i] = cnt < 32 ? a.d[i] >> cnt : 0;
			else if (in->opcode == 0x46)
				r.sd[i] = a.sd[i] >> (cnt < 32 ? cnt : 31);
			else
				r.d[i] = cnt < 32 ? a.d[i] << cnt : 0;
		}
	}
	set_reg(st, in->reg, &r, in->L);
}

static void do_permilvar(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int i;
	vec a, b, r;

	get_reg(st, in->vvvv, &a);
	get_rm(in, gpr, st, &b, in->L ? 32 : 16);
	memset(&r, 0, sizeof(r));
	if (in->opcode == 0x0c) {
		for (i = 0; i < (in->L ? 8 : 4); i++)
			r.d[i] = a.d[(i & 4) + (b.d[i] & 3)];
	} else {
		for (i = 0; i < (in->L ? 4 : 2); i++)
			r.q[i] = a.q[(i & 2) + ((b.q[i] >> 1) & 1)];
	}
	set_reg(st, in->reg, &r, in->L);
}

static void do_test(const struct fa_insn *in, u64 *gpr, u8 *st, u64 *flags)
{
	int n = in->L ? 4 : 2, i;
	u64 z = 0, c = 0, m;
	vec a, b;

	get_reg(st, in->reg, &a);
	get_rm(in, gpr, st, &b, in->L ? 32 : 16);
	if (in->opcode == 0x0e)
		m = 0x8000000080000000ULL;
	else if (in->opcode == 0x0f)
		m = 0x8000000000000000ULL;
	else
		m = ~0ULL;
	for (i = 0; i < n; i++) {
		z |= a.q[i] & b.q[i] & m;
		c |= ~a.q[i] & b.q[i] & m;
	}
	set_flags(flags, !z, !c);
}

static void do_permd(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int i;
	vec a, b, r;

	get_reg(st, in->vvvv, &a);
	get_rm(in, gpr, st, &b, 32);
	for (i = 0; i < 8; i++)
		r.d[i] = b.d[a.d[i] & 7];
	set_reg(st, in->reg, &r, 1);
}

static void do_pcmpgtq(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int i;
	vec a, b, r;

	get_reg(st, in->vvvv, &a);
	get_rm(in, gpr, st, &b, in->L ? 32 : 16);
	memset(&r, 0, sizeof(r));
	for (i = 0; i < (in->L ? 4 : 2); i++)
		r.sq[i] = a.sq[i] > b.sq[i] ? -1 : 0;
	set_reg(st, in->reg, &r, in->L);
}

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

static void do_cmp(const struct fa_insn *in, u64 *gpr, u8 *st)
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

static u32 crc32c_byte(u32 crc, u8 b)
{
	int k;

	crc ^= b;
	for (k = 0; k < 8; k++)
		crc = (crc >> 1) ^ (0x82f63b78 & -(crc & 1));
	return crc;
}

static void do_crc32(const struct fa_insn *in, u64 *gpr)
{
	int size = in->opcode == 0xf0 ? 1 : in->W ? 8 : in->opsize16 ? 2 : 4, i;
	u32 crc = (u32)gpr[in->reg];
	u64 src = 0;

	if (in->mod != 3)
		memcpy(&src, ea(in, gpr, 0, 0), size);
	else if (size == 1 && !in->rex && in->rm >= 4)
		src = gpr[in->rm - 4] >> 8;	/* ah, ch, dh, bh */
	else
		src = gpr[in->rm];
	for (i = 0; i < size; i++)
		crc = crc32c_byte(crc, (u8)(src >> (8 * i)));
	gpr[in->reg] = crc;
}

static void do_pcmpgtq_legacy(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	vec a, b;
	int i;

	get_reg(st, in->reg, &a);
	get_rm(in, gpr, st, &b, 16);
	for (i = 0; i < 2; i++)
		a.sq[i] = a.sq[i] > b.sq[i] ? -1 : 0;
	memcpy(st + FA_ST_LO + 16 * in->reg, a.b, 16);
}

static int str_elem(const vec *v, int i, int fmt)
{
	switch (fmt) {
	case 0: return v->b[i];
	case 1: return v->b[2 * i] | (v->b[2 * i + 1] << 8);
	case 2: return (s8)v->b[i];
	default: return (s16)(v->b[2 * i] | (v->b[2 * i + 1] << 8));
	}
}

static int str_len(const struct fa_insn *in, u64 reg, const vec *v, int fmt, int n)
{
	int i;

	if (in->opcode <= 0x61) {	/* explicit length: |eax| / |edx|, saturated */
		s64 l = in->W ? (s64)reg : (s64)(s32)reg;

		if (l < 0)
			l = -l;
		return (l < 0 || l > n) ? n : (int)l;
	}
	for (i = 0; i < n; i++)
		if (!str_elem(v, i, fmt))
			break;
	return i;
}

static void do_pcmpstr(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st)
{
	int fmt = in->imm & 3, n = (fmt & 1) ? 8 : 16;
	int agg = (in->imm >> 2) & 3, pol = (in->imm >> 4) & 3;
	u32 res = 0, all = (1u << n) - 1;
	int la, lb, i, j;
	vec a, b;

	get_reg(st, in->reg, &a);
	get_rm(in, gpr, st, &b, 16);
	la = str_len(in, gpr[0], &a, fmt, n);
	lb = str_len(in, gpr[2], &b, fmt, n);

	for (j = 0; j < n; j++) {
		int bj = str_elem(&b, j, fmt), bit = 0;

		switch (agg) {
		case 0:		/* equal any */
			for (i = 0; i < la && j < lb; i++)
				bit |= str_elem(&a, i, fmt) == bj;
			break;
		case 1:		/* ranges */
			for (i = 0; i + 1 < la && j < lb; i += 2)
				bit |= bj >= str_elem(&a, i, fmt) && bj <= str_elem(&a, i + 1, fmt);
			break;
		case 2:		/* equal each */
			if (j < la && j < lb)
				bit = str_elem(&a, j, fmt) == bj;
			else
				bit = j >= la && j >= lb;
			break;
		default:	/* equal ordered */
			bit = 1;
			for (i = 0; i < n - j && bit; i++) {
				if (i >= la)
					break;			/* past the needle: matches */
				if (j + i >= lb)
					bit = 0;		/* haystack ran out */
				else
					bit = str_elem(&a, i, fmt) == str_elem(&b, j + i, fmt);
			}
			break;
		}
		res |= (u32)bit << j;
	}
	if (pol == 1)
		res ^= all;
	else if (pol == 3)
		res ^= lb >= n ? all : (1u << lb) - 1;

	if (in->opcode & 1) {		/* index in ecx */
		int idx = n;

		if (res)
			idx = (in->imm & 0x40) ? 31 - __builtin_clz(res) : __builtin_ctz(res);
		gpr[1] = (u32)idx;
	} else {			/* mask in xmm0 */
		vec m;

		memset(&m, 0, sizeof(m));
		if (in->imm & 0x40) {
			for (i = 0; i < n; i++) {
				if (!(res & (1u << i)))
					continue;
				if (n == 16)
					m.b[i] = 0xff;
				else
					m.b[2 * i] = m.b[2 * i + 1] = 0xff;
			}
		} else {
			m.d[0] = res;
		}
		memcpy(st + FA_ST_LO, m.b, 16);
	}
	*flags = (*flags & ~0x8d5ULL) | (res ? 1 : 0) | (lb < n ? 0x40 : 0) |
		 (la < n ? 0x80 : 0) | ((res & 1) ? 0x800 : 0);
}

void fa_helper_c(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st)
{
	if (in->kind == FA_K_SSE42) {
		if (in->map == 3)
			do_pcmpstr(in, gpr, flags, st);
		else if (in->opcode == 0x37)
			do_pcmpgtq_legacy(in, gpr, st);
		else
			do_crc32(in, gpr);
		return;
	}
	if (in->map == 1 && in->opcode == 0xc2) {
		do_cmp(in, gpr, st);
		return;
	}
	if (in->map != 2)
		__builtin_trap();
	switch (in->opcode) {
	case 0x0c: case 0x0d:
		do_permilvar(in, gpr, st);
		break;
	case 0x0e: case 0x0f: case 0x17:
		do_test(in, gpr, st, flags);
		break;
	case 0x16: case 0x36:
		do_permd(in, gpr, st);
		break;
	case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x8c: case 0x8e:
		do_maskmov(in, gpr, st);
		break;
	case 0x37:
		do_pcmpgtq(in, gpr, st);
		break;
	case 0x45: case 0x46: case 0x47:
		do_varshift(in, gpr, st);
		break;
	case 0x90: case 0x91: case 0x92: case 0x93:
		do_gather(in, gpr, st);
		break;
	default:
		__builtin_trap();
	}
}
