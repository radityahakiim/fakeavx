// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: the helper operations that need no floating point. Compiled with
 * -mgeneral-regs-only, so that no xmm register is touched: they are entered
 * through FA_BLOB_HELPER_LEAN, which saves none. The translator stores the
 * operand registers to the state block before the call and loads them back
 * after it (see do_helper() in core/translate.c).
 */
#include "helper.h"

#undef memcpy
#undef memset
/*
 * Out-of-line copies, for sizes not known at compile time. Words first:
 * byte-at-a-time copies of vector registers cost ~16 stores each, and the
 * entry stub then reloads the 16-byte slots, which a store of a smaller size
 * cannot forward to (a ~12-cycle stall per register on Core 2).
 */
void *memcpy(void *d, const void *s, size_t n)
{
	u8 *dd = d;
	const u8 *ss = s;

	for (; n >= 8; n -= 8, dd += 8, ss += 8)
		*(u64 *)dd = *(const u64 *)ss;
	while (n--)
		*dd++ = *ss++;
	return d;
}

void *memset(void *d, int ch, size_t n)
{
	u8 *dd = d;
	u64 w = 0x0101010101010101ULL * (u8)ch;

	for (; n >= 8; n -= 8, dd += 8)
		*(u64 *)dd = w;
	while (n--)
		*dd++ = ch;
	return d;
}

#define memcpy fa_cpy
#define memset fa_set

/*
 * The four 8-byte words of a result register into the state block (low half,
 * then upper half; a 128-bit result has zeros in out[2..3]).
 */
static inline void put_words(u8 *st, int r, const u64 *out)
{
	u64 *lo = (u64 *)(st + FA_ST_LO + 16 * r), *hi = (u64 *)(st + FA_ST_HI + 16 * r);

	lo[0] = out[0];
	lo[1] = out[1];
	hi[0] = out[2];
	hi[1] = out[3];
}

/*
 * Result words are built in registers from the gathered (or kept) elements
 * and stored once: see do_maskmov. Nothing is written before every selected
 * element has been loaded, as before.
 */
static void do_gather(const struct fa_insn *in, u64 *gpr, u8 *st)
{
	int ds = in->W ? 8 : 4, is = (in->opcode & 1) ? 8 : 4;
	int vl = in->L ? 32 : 16;
	int n = vl / (ds > is ? ds : is), i;
	u64 out[4] = { 0, 0, 0, 0 }, zero[4] = { 0, 0, 0, 0 };
	vec dst, mask, idx;

	get_reg(st, in->reg, &dst);
	get_reg(st, in->vvvv, &mask);
	get_reg(st, in->mem.index, &idx);
	for (i = 0; i < n; i++) {
		s64 ix = is == 8 ? idx.sq[i] : idx.sd[i];

		if (ds == 8) {
			out[i] = mask.sq[i] < 0 ? *(const u64 *)ea(in, gpr, 1, ix) : dst.q[i];
		} else {
			u64 e = mask.sd[i] < 0 ? *(const u32 *)ea(in, gpr, 1, ix) : dst.d[i];

			out[i / 2] |= e << (32 * (i & 1));
		}
	}
	put_words(st, in->vvvv, zero);		/* the mask is cleared */
	put_words(st, in->reg, out);
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
	if (!store) {
		/*
		 * Loads: build each 8-byte word of the result in a register and
		 * store it once, straight into the state block. Assembling it in
		 * memory from 4-byte pieces and copying it on would make every
		 * 8-byte read of it wait for two narrower stores (Core 2 cannot
		 * forward those).
		 */
		u64 out[4] = { 0, 0, 0, 0 };	/* written only once every load has succeeded */

		for (i = 0; i < n * ds / 8; i++) {
			u64 w = 0;

			if (ds == 8) {
				if (mask.sq[i] < 0)
					w = *(const u64 *)(m + 8 * i);
			} else {
				if (mask.sd[2 * i] < 0)
					w = *(const u32 *)(m + 8 * i);
				if (mask.sd[2 * i + 1] < 0)
					w |= (u64)*(const u32 *)(m + 8 * i + 4) << 32;
			}
			out[i] = w;
		}
		put_words(st, in->reg, out);
		return;
	}
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

void do_test(const struct fa_insn *in, u64 *gpr, u8 *st, u64 *flags)
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

/*
 * The 16 bytes of an operand as 16 bytes or 8 words, signed or not (imm[1:0]),
 * each element widened to an int once: the comparisons below then need no
 * per-element decoding (formerly a switch per element, up to 512 times).
 */
static int str_elems(const u8 *p, int fmt, s32 *e)
{
	int i;

	switch (fmt) {
	case 0:
		for (i = 0; i < 16; i++)
			e[i] = p[i];
		return 16;
	case 1:
		for (i = 0; i < 8; i++)
			e[i] = p[2 * i] | p[2 * i + 1] << 8;
		return 8;
	case 2:
		for (i = 0; i < 16; i++)
			e[i] = (s8)p[i];
		return 16;
	default:
		for (i = 0; i < 8; i++)
			e[i] = (s16)(p[2 * i] | p[2 * i + 1] << 8);
		return 8;
	}
}

static int str_len(const struct fa_insn *in, u64 reg, const s32 *e, int n)
{
	int i;

	if (in->opcode <= 0x61) {	/* explicit length: |eax| / |edx|, saturated */
		s64 l = in->W ? (s64)reg : (s64)(s32)reg;

		if (l < 0)
			l = -l;
		return (l < 0 || l > n) ? n : (int)l;
	}
	for (i = 0; i < n && e[i]; i++)	/* implicit: up to the first zero element */
		;
	return i;
}

/* index of the first zero byte in 16 bytes, or 16 */
static inline int zero_byte(const u8 *p)
{
	u64 w[2];
	int k;

	memcpy(w, p, 16);
	for (k = 0; k < 2; k++) {
		u64 z = (w[k] - 0x0101010101010101ULL) & ~w[k] & 0x8080808080808080ULL;

		if (z)	/* the lowest set bit marks the first zero byte exactly */
			return 8 * k + __builtin_ctzll(z) / 8;
	}
	return 16;
}

/*
 * IntRes1 for byte elements and equal any / ranges, straight from the bytes:
 * the set of matching values as a 256-bit map (signed bytes ^ 0x80, which
 * keeps their order), then 16 independent lookups. A range sets its bits in
 * at most four words, whatever its width.
 */
static u32 pcmpstr_bytes(const struct fa_insn *in, const u64 *gpr, const u8 *pa, const u8 *pb,
			 int fmt, int agg, int *pla, int *plb)
{
	u64 map[4] = { 0, 0, 0, 0 };
	u8 flip = fmt == 2 ? 0x80 : 0;
	u32 res = 0;
	int la, lb, i, j;

	if (in->opcode <= 0x61) {	/* explicit lengths, as str_len() */
		s64 l1 = in->W ? (s64)gpr[0] : (s64)(s32)gpr[0];
		s64 l2 = in->W ? (s64)gpr[2] : (s64)(s32)gpr[2];

		if (l1 < 0)
			l1 = -l1;
		if (l2 < 0)
			l2 = -l2;
		la = (l1 < 0 || l1 > 16) ? 16 : (int)l1;
		lb = (l2 < 0 || l2 > 16) ? 16 : (int)l2;
	} else {
		la = zero_byte(pa);
		lb = zero_byte(pb);
	}
	if (agg == 0) {
		for (i = 0; i < la; i++) {
			u8 v = pa[i] ^ flip;

			map[v >> 6] |= 1ULL << (v & 63);
		}
	} else {
		for (i = 0; i + 1 < la; i += 2) {
			int lo = pa[i] ^ flip, hi = pa[i + 1] ^ flip, w;

			if (lo > hi)
				continue;		/* matches nothing */
			for (w = lo >> 6; w <= hi >> 6; w++) {
				int l = w == lo >> 6 ? lo & 63 : 0, h = w == hi >> 6 ? hi & 63 : 63;

				map[w] |= (~0ULL >> (63 - h)) & (~0ULL << l);	/* bits l..h */
			}
		}
	}
#pragma GCC unroll 16
	for (j = 0; j < 16; j++) {
		u8 v = pb[j] ^ flip;

		res |= (u32)(map[v >> 6] >> (v & 63) & 1) << j;
	}
	*pla = la;
	*plb = lb;
	return lb >= 16 ? res : res & ((1u << lb) - 1);	/* only j < lb can match */
}

static void do_pcmpstr(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st)
{
	int fmt = in->imm & 3, n;
	int agg = (in->imm >> 2) & 3, pol = (in->imm >> 4) & 3;
	u32 res = 0, all;
	int la, lb, i, j;
	u8 pa[16], pb[16];
	s32 a[16], b[16];

	memcpy(pa, st + FA_ST_LO + 16 * in->reg, 16);
	if (in->mod == 3)
		memcpy(pb, st + FA_ST_LO + 16 * in->rm, 16);
	else
		memcpy(pb, ea(in, gpr, 0, 0), 16);
	if (!(fmt & 1) && agg <= 1) {
		/* bytes, equal any / ranges (what picohttpparser and most uses are) */
		res = pcmpstr_bytes(in, gpr, pa, pb, fmt, agg, &la, &lb);
		n = 16;
		all = 0xffff;
		goto out;
	}
	n = str_elems(pa, fmt, a);
	str_elems(pb, fmt, b);
	all = (1u << n) - 1;
	la = str_len(in, gpr[0], a, n);
	lb = str_len(in, gpr[2], b, n);

	/* IntRes1: bit j for element j of the second operand (only j < lb can be set
	 * by any and ranges) */
	switch (agg) {
	case 0:		/* equal any: b[j] is one of a[0..la) */
		for (i = 0; i < la; i++)
			for (j = 0; j < lb; j++)
				res |= (u32)(a[i] == b[j]) << j;
		break;
	case 1:		/* ranges: a[i] <= b[j] <= a[i+1] for a pair i */
		for (i = 0; i + 1 < la; i += 2) {
			s32 lo = a[i], hi = a[i + 1];

			for (j = 0; j < lb; j++)
				res |= (u32)((b[j] >= lo) & (b[j] <= hi)) << j;
		}
		break;
	case 2:		/* equal each */
		for (j = 0; j < n; j++)
			res |= (u32)(j < la && j < lb ? a[j] == b[j] : j >= la && j >= lb) << j;
		break;
	default:	/* equal ordered: the needle a[0..la) starts at b[j] */
		for (j = 0; j < n; j++) {
			int bit = 1;

			for (i = 0; i < n - j && bit; i++) {
				if (i >= la)
					break;			/* past the needle: matches */
				if (j + i >= lb)
					bit = 0;		/* haystack ran out */
				else
					bit = a[i] == b[j + i];
			}
			res |= (u32)bit << j;
		}
		break;
	}
out:
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

/* integer operations; fa_helper_lean() in fakeavx.h says which */
void fa_helper_lean_c(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st)
{
	if (in->kind == FA_K_VEX && in->map == 3) {	/* vpcmpXstrX (VEX.128) */
		do_pcmpstr(in, gpr, flags, st);
		/* a VEX.128 write zeroes the upper half; the legacy form keeps it */
		if (!(in->opcode & 1))
			memset(st + FA_ST_HI, 0, 16);
		return;
	}
	if (in->kind == FA_K_SSE4) {
		if (in->map == 3)
			do_pcmpstr(in, gpr, flags, st);
		else if (in->opcode == 0x37)
			do_pcmpgtq_legacy(in, gpr, st);
		else
			do_crc32(in, gpr);
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
