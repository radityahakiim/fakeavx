/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fakeavx helper blob: definitions shared by helper.c (floating point, called
 * with all xmm registers saved) and helper_int.c (integer only, compiled
 * with -mgeneral-regs-only, called without saving any xmm register).
 */
#ifndef FA_HELPER_H
#define FA_HELPER_H
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

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int ch, size_t n);

/*
 * Everything below copies through these: constant sizes become plain moves
 * (-fno-builtin does not apply to __builtin_*), and the element sizes that
 * occur at run time (4, 8, 16, 32: vector elements and halves) a single
 * access each. A single access per element is also what the hardware does:
 * a masked store whose element crosses into an unmapped page faults without
 * having written part of it.
 */
static inline __attribute__((always_inline)) void *fa_cpy(void *d, const void *s, size_t n)
{
	if (__builtin_constant_p(n))
		return __builtin_memcpy(d, s, n);
	switch (n) {
	case 4: __builtin_memcpy(d, s, 4); return d;
	case 8: __builtin_memcpy(d, s, 8); return d;
	case 16: __builtin_memcpy(d, s, 16); return d;
	case 32: __builtin_memcpy(d, s, 32); return d;
	}
	return memcpy(d, s, n);
}

static inline __attribute__((always_inline)) void *fa_set(void *d, int ch, size_t n)
{
	if (__builtin_constant_p(n))
		return __builtin_memset(d, ch, n);
	return memset(d, ch, n);
}

#define memcpy fa_cpy
#define memset fa_set

static inline void get_reg(u8 *st, int r, vec *v)
{
	memcpy(v->b, st + FA_ST_LO + 16 * r, 16);
	memcpy(v->b + 16, st + FA_ST_HI + 16 * r, 16);
}

static inline void set_reg(u8 *st, int r, const vec *v, int L)
{
	memcpy(st + FA_ST_LO + 16 * r, v->b, 16);
	if (L)
		memcpy(st + FA_ST_HI + 16 * r, v->b + 16, 16);
	else
		memset(st + FA_ST_HI + 16 * r, 0, 16);
}

static inline u64 seg_base(const struct fa_insn *in)
{
	u64 base = 0;

	if (in->mem.seg == 0x64)	/* x86-64 TLS ABI: %fs:0 holds the fs base */
		__asm__("mov %%fs:0, %0" : "=r"(base));
	return base;
}

/* effective address; idx is the already sign-extended VSIB index or 0 */
static inline u8 *ea(const struct fa_insn *in, const u64 *gpr, int vsib, s64 idx)
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

static inline void get_rm(const struct fa_insn *in, const u64 *gpr, u8 *st, vec *v, int n)
{
	if (in->mod == 3)
		get_reg(st, in->rm, v);
	else
		memcpy(v->b, ea(in, gpr, 0, 0), n);
}

static inline void set_flags(u64 *flags, int zf, int cf)
{
	*flags = (*flags & ~0x8d5ULL) | (zf ? 0x40 : 0) | (cf ? 1 : 0);
}

/* helper_int.c */
void fa_helper_lean_c(const struct fa_insn *in, u64 *gpr, u64 *flags, u8 *st);
void do_test(const struct fa_insn *in, u64 *gpr, u8 *st, u64 *flags);	/* also ptest on SSSE3 hosts */
#endif
