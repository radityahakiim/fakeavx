// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: translate one VEX-encoded instruction (or POPCNT) into an
 * SSE4.1-only stub.
 *
 * Guest state mapping:
 *   ymmN[127:0]   -> host xmmN
 *   ymmN[255:128] -> %gs:FA_ST_HI + 16*N
 *
 * Rules for emitted code:
 *   - EFLAGS must not change unless the guest instruction changes them, so
 *     glue code only uses mov / lea / SSE moves.
 *   - Legacy SSE packed operations fault on unaligned memory operands while
 *     their VEX forms do not, so packed memory operands are loaded with
 *     movups first. Operands in the %gs state block are always aligned.
 */
#include "fakeavx.h"

enum { O_REG, O_MEM, O_GS };

struct opnd {
	u8 type;
	u8 reg;
	s32 off;	/* O_MEM: displacement adjustment, O_GS: offset */
};

struct ctx {
	const struct fa_insn *in;
	const struct fa_env *env;
	const struct fa_op *op;
	u8 *p;
	u32 len, cap;
	int err;
	struct fa_mem mem;	/* memory operand, possibly rewritten */
	u16 used;		/* xmm registers referenced by the guest insn */
	u8 nx, xs[3];		/* xmm scratch registers spilled by this stub */
	u8 npool;		/* scratch registers taken from the region's pool */
	u8 ng, gs[2];		/* gpr scratch registers in use */
	u16 gused;
	/* legacy op to emit */
	u8 pfx, map, opc, rexw;
	int imm_lo, imm_hi;	/* -1: none */
};

#define REG(r)	((struct opnd){ O_REG, (r), 0 })
#define MEM(a)	((struct opnd){ O_MEM, 0, (a) })
#define GS(o)	((struct opnd){ O_GS, 0, (o) })
#define HI(r)	hi_opnd(c, (r))

static const u8 pp_prefix[4] = { 0, 0x66, 0xf3, 0xf2 };

/* host register caching the upper half of guest register r, or -1 */
static int hireg(struct ctx *c, int r)
{
	return c->env->map ? c->env->map->hi[r] : -1;
}

static struct opnd hi_opnd(struct ctx *c, int r)
{
	int h = hireg(c, r);

	if (h >= 0)
		return REG(h);
	return GS(FA_ST_HI + 16 * r);
}

static void put(struct ctx *c, u8 b)
{
	if (c->len >= c->cap) {
		c->err = 1;
		return;
	}
	c->p[c->len++] = b;
}

static void put32(struct ctx *c, u32 v)
{
	put(c, v);
	put(c, v >> 8);
	put(c, v >> 16);
	put(c, v >> 24);
}

static void put64(struct ctx *c, u64 v)
{
	put32(c, (u32)v);
	put32(c, (u32)(v >> 32));
}

static u64 cur_addr(struct ctx *c)
{
	return c->env->out_addr + c->len;
}

static void emit_modrm(struct ctx *c, int reg, const struct opnd *o, int imm_len)
{
	const struct fa_mem *m = &c->mem;
	s64 disp;

	reg &= 7;
	if (o->type == O_REG) {
		put(c, 0xc0 | (reg << 3) | (o->reg & 7));
		return;
	}
	if (o->type == O_GS) {
		put(c, (reg << 3) | 4);
		put(c, 0x25);
		put32(c, o->off);
		return;
	}
	if (m->riprel) {
		u64 target = c->in->rip + c->in->len + (s64)m->disp + o->off;
		s64 rel;

		put(c, (reg << 3) | 5);
		rel = (s64)(target - (cur_addr(c) + 4 + imm_len));
		if (rel != (s32)rel)
			c->err = 1;
		put32(c, (u32)rel);
		return;
	}
	disp = (s64)m->disp + o->off;
	if (disp != (s32)disp)
		c->err = 1;
	if (!m->has_base) {
		put(c, (reg << 3) | 4);
		put(c, (m->scale << 6) | ((m->has_index ? m->index & 7 : 4) << 3) | 5);
		put32(c, (u32)disp);
	} else {
		int sib = m->has_index || (m->base & 7) == 4;
		int mod;

		if (disp == 0 && (m->base & 7) != 5)
			mod = 0;
		else if (disp == (s8)disp)
			mod = 1;
		else
			mod = 2;
		put(c, (mod << 6) | (reg << 3) | (sib ? 4 : (m->base & 7)));
		if (sib)
			put(c, (m->scale << 6) |
			       ((m->has_index ? m->index & 7 : 4) << 3) | (m->base & 7));
		if (mod == 1)
			put(c, (u8)disp);
		else if (mod == 2)
			put32(c, (u32)disp);
	}
}

/* map: 0 = one-byte opcode, 1 = 0F, 2 = 0F38, 3 = 0F3A */
static void emit(struct ctx *c, int pfx, int map, int opc, int reg,
		 struct opnd o, int rexw, int imm)
{
	u8 rex = 0x40;

	if (o.type == O_MEM) {
		if (c->mem.seg)
			put(c, c->mem.seg);
		if (c->mem.addr32)
			put(c, 0x67);
		if (!c->mem.riprel) {
			if (c->mem.has_index && (c->mem.index & 8))
				rex |= 2;
			if (c->mem.has_base && (c->mem.base & 8))
				rex |= 1;
		}
	} else if (o.type == O_GS) {
		put(c, 0x65);
	} else if (o.reg & 8) {
		rex |= 1;
	}
	if (pfx)
		put(c, pfx);
	if (rexw)
		rex |= 8;
	if (reg & 8)
		rex |= 4;
	if (rex != 0x40)
		put(c, rex);
	if (map >= 1)
		put(c, 0x0f);
	if (map == 2)
		put(c, 0x38);
	else if (map == 3)
		put(c, 0x3a);
	put(c, opc);
	emit_modrm(c, reg, &o, imm >= 0 ? 1 : 0);
	if (imm >= 0)
		put(c, (u8)imm);
}

/* ---- small building blocks ---- */

static void mov_rr(struct ctx *c, int d, int s)
{
	if (d != s)
		emit(c, 0, 1, 0x28, d, REG(s), 0, -1);		/* movaps */
}

/* gs is a state block slot or, for a cached upper half, a register */
static void load_gs(struct ctx *c, int r, struct opnd gs)
{
	if (gs.type == O_REG && gs.reg == r)
		return;
	emit(c, 0, 1, 0x28, r, gs, 0, -1);
}

static void store_gs(struct ctx *c, int r, struct opnd gs)
{
	if (gs.type == O_REG && gs.reg == r)
		return;
	emit(c, 0, 1, 0x29, r, gs, 0, -1);
}

static void loadu(struct ctx *c, int r, int adj)
{
	emit(c, 0, 1, 0x10, r, MEM(adj), 0, -1);		/* movups */
}

static void storeu(struct ctx *c, int r, int adj)
{
	emit(c, 0, 1, 0x11, r, MEM(adj), 0, -1);
}

static void pxor_self(struct ctx *c, int r)
{
	emit(c, 0x66, 1, 0xef, r, REG(r), 0, -1);
}

/* mov qword %gs:off, 0 (twice): flag-neutral zeroing of an upper half */
static void zero_hi(struct ctx *c, int r)
{
	int i, h = hireg(c, r);

	if (h >= 0) {
		pxor_self(c, h);
		return;
	}

	for (i = 0; i < 2; i++) {
		put(c, 0x65); put(c, 0x48); put(c, 0xc7); put(c, 0x04); put(c, 0x25);
		put32(c, FA_ST_HI + 16 * r + 8 * i);
		put32(c, 0);
	}
}

static int xscratch(struct ctx *c)
{
	int r;

	if (c->env->map && c->npool < c->env->map->nscratch)
		return c->env->map->scratch[c->npool++];
	for (r = 0; r < 16; r++) {
		if (c->used & (1 << r))
			continue;
		if (c->nx >= 3)
			break;
		c->used |= 1 << r;
		store_gs(c, r, GS(FA_ST_XTMP + 16 * c->nx));
		c->xs[c->nx++] = r;
		return r;
	}
	c->err = 1;
	return 0;
}

/* gpr <-> gs slot */
static void gpr_gs(struct ctx *c, int store, int r, int slot)
{
	emit(c, 0, 0, store ? 0x89 : 0x8b, r, GS(FA_ST_GTMP + 8 * slot), 1, -1);
}

static int gscratch(struct ctx *c, u16 avoid)
{
	int r;

	avoid |= c->gused | (1 << 4);	/* never rsp */
	for (r = 0; r < 16; r++) {
		if (avoid & (1 << r))
			continue;
		if (c->ng >= 2)
			break;
		c->gused |= 1 << r;
		gpr_gs(c, 1, r, 2 + c->ng);
		c->gs[c->ng++] = r;
		return r;
	}
	c->err = 1;
	return 0;
}

static void emit_jmp_abs(struct ctx *c, u64 target)
{
	s64 rel = (s64)(target - (cur_addr(c) + 5));

	if (rel == (s32)rel) {
		put(c, 0xe9);
		put32(c, (u32)rel);
	} else {
		put(c, 0xff); put(c, 0x25); put32(c, 0);
		put64(c, target);
	}
}

static void emit_call_blob(struct ctx *c, u32 entry)
{
	s64 rel = (s64)(c->env->blob_addr + entry - (cur_addr(c) + 5));

	if (rel != (s32)rel)
		c->err = 1;
	put(c, 0xe8);
	put32(c, (u32)rel);
}

static void finish(struct ctx *c)
{
	while (c->nx) {
		c->nx--;
		load_gs(c, c->xs[c->nx], GS(FA_ST_XTMP + 16 * c->nx));
	}
	while (c->ng) {
		c->ng--;
		gpr_gs(c, 0, c->gs[c->ng], 2 + c->ng);
	}
	if (!c->env->no_jmp)
		emit_jmp_abs(c, c->in->rip + c->in->len);
}

/* the guest operation, as a legacy SSE instruction */
static void op(struct ctx *c, int reg, struct opnd o, int imm)
{
	emit(c, c->pfx, c->map, c->opc, reg, o, c->rexw, imm);
}

static int is_mem(struct ctx *c)
{
	return c->in->mod != 3;
}

/* ---- shapes ---- */

static void do_rvm(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	u16 fl = c->op->flags;
	int D = in->reg, A = in->vvvv, B = in->rm;
	int mem = is_mem(c);
	int bgpr = !!(fl & FA_F_BGPR);
	int breg = !mem && !bgpr;	/* src2 is an xmm register */
	int fold = !!(fl & FA_F_FOLD);
	int L = in->L && !(fl & FA_F_SCALAR);
	int Tb = -1;
	struct opnd b;

	if (mem && !fold)
		Tb = xscratch(c);

	if (L) {
		int X;
		struct opnd bh;

		if (hireg(c, D) >= 0 && !(breg && D == B && D != A))
			X = hireg(c, D);	/* compute in place */
		else
			X = (D != A && !(breg && D == B)) ? D : xscratch(c);
		load_gs(c, X, HI(A));
		if (mem) {
			if (fl & FA_F_BSAME)
				emit(c, 0xf3, 1, 0x6f, Tb, MEM(0), 0, -1);	/* movdqu */
			else
				loadu(c, Tb, 16);
			bh = REG(Tb);
		} else {
			bh = (fl & FA_F_BSAME) ? REG(B) : HI(B);
		}
		op(c, X, bh, c->imm_hi);
		store_gs(c, X, HI(D));
	}

	if (mem && !fold) {
		if (!(L && (fl & FA_F_BSAME)))
			loadu(c, Tb, 0);
		b = REG(Tb);
	} else if (mem) {
		b = MEM(0);
	} else {
		b = REG(B);
	}

	if (D == A) {
		op(c, D, b, c->imm_lo);
	} else if (breg && B == D) {
		if (fl & FA_F_COMM) {
			op(c, D, REG(A), c->imm_lo);
		} else {
			int T = xscratch(c);

			mov_rr(c, T, A);
			op(c, T, b, c->imm_lo);
			mov_rr(c, D, T);
		}
	} else {
		mov_rr(c, D, A);
		op(c, D, b, c->imm_lo);
	}
	if (!L)
		zero_hi(c, D);
}

/* dst = op(src2, src1): packed compares with swapped predicates */
static void do_rvm_swapped(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, A = in->vvvv, B = in->rm;
	int mem = is_mem(c);

	if (in->L) {
		int X;

		if (hireg(c, D) >= 0 && (D != A || (!mem && D == B)))
			X = hireg(c, D);
		else
			X = (D != A && !(!mem && D == B)) ? D : xscratch(c);
		if (mem)
			loadu(c, X, 16);
		else
			load_gs(c, X, HI(B));
		op(c, X, HI(A), c->imm_hi);
		store_gs(c, X, HI(D));
	}
	if (mem) {
		int T = (D != A) ? D : xscratch(c);

		loadu(c, T, 0);
		op(c, T, REG(A), c->imm_lo);
		mov_rr(c, D, T);
	} else if (D == B) {
		op(c, D, REG(A), c->imm_lo);
	} else if (D == A) {
		int T = xscratch(c);

		mov_rr(c, T, B);
		op(c, T, REG(A), c->imm_lo);
		mov_rr(c, D, T);
	} else {
		mov_rr(c, D, B);
		op(c, D, REG(A), c->imm_lo);
	}
	if (!in->L)
		zero_hi(c, D);
}

static void do_rm(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, B = in->rm;
	int mem = is_mem(c);
	int fold = !!(c->op->flags & FA_F_FOLD);

	if (in->L) {
		int X = hireg(c, D);

		if (X < 0)
			X = (!mem && B == D) ? xscratch(c) : D;
		if (mem) {
			loadu(c, X, 16);
			op(c, X, REG(X), c->imm_hi);
		} else {
			op(c, X, HI(B), c->imm_hi);
		}
		store_gs(c, X, HI(D));
	}
	if (!mem) {
		op(c, D, REG(B), c->imm_lo);
	} else if (fold) {
		op(c, D, MEM(0), c->imm_lo);
	} else {
		loadu(c, D, 0);
		op(c, D, REG(D), c->imm_lo);
	}
	if (!in->L)
		zero_hi(c, D);
}

static void do_shifti(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->vvvv, B = in->rm, ext = in->reg & 7;

	if (in->L) {
		int X = hireg(c, D);

		if (X < 0)
			X = (D == B) ? xscratch(c) : D;
		load_gs(c, X, HI(B));
		op(c, ext, REG(X), in->imm);
		store_gs(c, X, HI(D));
	}
	mov_rr(c, D, B);
	op(c, ext, REG(D), in->imm);
	if (!in->L)
		zero_hi(c, D);
}

/* full-width register-to-register move, both halves */
static void move_reg(struct ctx *c, int D, int S, int L)
{
	if (L) {
		if (D == S)
			return;
		if (hireg(c, D) >= 0) {
			load_gs(c, hireg(c, D), HI(S));
		} else if (hireg(c, S) >= 0) {
			store_gs(c, hireg(c, S), HI(D));
		} else {
			load_gs(c, D, HI(S));
			store_gs(c, D, HI(D));
		}
		mov_rr(c, D, S);
	} else {
		mov_rr(c, D, S);
		zero_hi(c, D);
	}
}

static void do_load(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg;

	if (!is_mem(c)) {
		move_reg(c, D, in->rm, in->L);
		return;
	}
	if (in->L) {
		if (hireg(c, D) >= 0) {
			op(c, hireg(c, D), MEM(16), -1);
		} else {
			op(c, D, MEM(16), -1);
			store_gs(c, D, HI(D));
		}
		op(c, D, MEM(0), -1);
	} else {
		op(c, D, MEM(0), -1);
		zero_hi(c, D);
	}
}

static void do_store(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int S = in->reg;

	if (!is_mem(c)) {
		move_reg(c, in->rm, S, in->L);
		return;
	}
	op(c, S, MEM(0), -1);
	if (in->L) {
		int T = hireg(c, S);

		if (T < 0) {
			T = xscratch(c);
			load_gs(c, T, HI(S));
		}
		op(c, T, MEM(16), -1);
	}
}

static void do_pass(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	u16 fl = c->op->flags;

	if (in->map == 1 && in->opcode == 0xae) {
		int ext = in->reg & 7;

		if (ext != 2 && ext != 3) {
			c->err = 1;
			return;
		}
	}
	op(c, in->reg, is_mem(c) ? MEM(0) : REG(in->rm), in->has_imm ? in->imm : -1);
	if (fl & FA_F_ZHI)
		zero_hi(c, in->reg);
	if ((fl & FA_F_ZHIRM) && !is_mem(c))
		zero_hi(c, in->rm);
}

static void do_movss(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D, A = in->vvvv, B;

	if (is_mem(c)) {
		op(c, in->reg, MEM(0), -1);
		if (in->opcode == 0x10)
			zero_hi(c, in->reg);
		return;
	}
	if (in->opcode == 0x10) {
		D = in->reg;
		B = in->rm;
	} else {
		D = in->rm;
		B = in->reg;
	}
	/* legacy "movss x, y" (opcode 10): x[low] = y[low] */
	if (D == A) {
		emit(c, c->pfx, 1, 0x10, D, REG(B), 0, -1);
	} else if (D == B) {
		int T = xscratch(c);

		mov_rr(c, T, A);
		emit(c, c->pfx, 1, 0x10, T, REG(B), 0, -1);
		mov_rr(c, D, T);
	} else {
		mov_rr(c, D, A);
		emit(c, c->pfx, 1, 0x10, D, REG(B), 0, -1);
	}
	zero_hi(c, D);
}

static void emit_lea(struct ctx *c, int dst, int has_base, int base, int index,
		     int scale)
{
	struct fa_mem save = c->mem;

	memset(&c->mem, 0, sizeof(c->mem));
	c->mem.has_base = has_base;
	c->mem.base = base;
	c->mem.has_index = 1;
	c->mem.index = index;
	c->mem.scale = scale;
	emit(c, 0, 0, 0x8d, dst, MEM(0), 1, -1);
	c->mem = save;
}

static void do_movmsk(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int R = in->reg, B = in->rm, T, g, i;

	if (!in->L) {
		op(c, R, REG(B), -1);
		return;
	}
	T = hireg(c, B);
	if (T < 0) {
		T = xscratch(c);
		load_gs(c, T, HI(B));
	}
	g = gscratch(c, 1 << R);
	op(c, g, REG(T), -1);
	op(c, R, REG(B), -1);
	/* R |= g << k, without touching EFLAGS */
	if (in->opcode == 0xd7) {		/* k = 16 */
		for (i = 0; i < 5; i++)
			emit_lea(c, g, 0, 0, g, 3);
		emit_lea(c, R, 1, R, g, 1);
	} else if (in->pp == 0) {		/* ps: k = 4 */
		emit_lea(c, g, 1, g, g, 0);
		emit_lea(c, R, 1, R, g, 3);
	} else {				/* pd: k = 2 */
		emit_lea(c, R, 1, R, g, 2);
	}
}

static void do_zero(struct ctx *c)
{
	int i, T;

	if (c->in->L) {
		/* vzeroall names every register, so a region has nothing to borrow */
		if (c->env->map && c->env->map->reserved) {
			c->err = 1;
			return;
		}
		pxor_self(c, 0);
		for (i = 0; i < 16; i++)
			store_gs(c, 0, HI(i));
		for (i = 1; i < 16; i++)
			pxor_self(c, i);
		return;
	}
	T = xscratch(c);
	pxor_self(c, T);
	for (i = 0; i < 16; i++) {
		if (hireg(c, i) >= 0)
			pxor_self(c, hireg(c, i));
		else
			store_gs(c, T, HI(i));
	}
}

static void do_cvt_widen(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, B = in->rm;

	if (!in->L) {
		op(c, D, is_mem(c) ? MEM(0) : REG(B), -1);
		zero_hi(c, D);
		return;
	}
	if (is_mem(c)) {
		int X = hireg(c, D) >= 0 ? hireg(c, D) : D;

		op(c, X, MEM(8), -1);
		store_gs(c, X, HI(D));
		op(c, D, MEM(0), -1);
	} else {
		int X = hireg(c, D);

		if (X < 0)
			X = (D == B) ? xscratch(c) : D;
		emit(c, 0, 1, 0x12, X, REG(B), 0, -1);		/* movhlps */
		op(c, X, REG(X), -1);
		store_gs(c, X, HI(D));
		op(c, D, REG(B), -1);
	}
}

static void do_cvt_narrow(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, B = in->rm, T;

	if (!in->L) {
		if (is_mem(c)) {
			loadu(c, D, 0);
			op(c, D, REG(D), -1);
		} else {
			op(c, D, REG(B), -1);
		}
		zero_hi(c, D);
		return;
	}
	T = xscratch(c);
	if (is_mem(c)) {
		loadu(c, T, 16);
		op(c, T, REG(T), -1);
		loadu(c, D, 0);
		op(c, D, REG(D), -1);
	} else {
		op(c, T, HI(B), -1);
		op(c, D, REG(B), -1);
	}
	emit(c, 0, 1, 0x16, D, REG(T), 0, -1);			/* movlhps */
	zero_hi(c, D);
}

static void do_bcast(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, o = in->opcode;

	if (o == 0x1a || o == 0x5a) {
		loadu(c, D, 0);
		store_gs(c, D, HI(D));
		return;
	}
	if (is_mem(c)) {
		switch (o) {
		case 0x18: case 0x58:
			emit(c, 0xf3, 1, 0x10, D, MEM(0), 0, -1);	/* movss */
			break;
		case 0x19: case 0x59:
			emit(c, 0xf3, 1, 0x7e, D, MEM(0), 0, -1);	/* movq */
			break;
		case 0x78:
			if (c->env->no_sse41) {
				int g = gscratch(c, 0);

				emit(c, 0, 1, 0xb6, g, MEM(0), 0, -1);		/* movzx g, byte */
				emit(c, 0x66, 1, 0x6e, D, REG(g), 0, -1);	/* movd */
			} else {
				emit(c, 0x66, 3, 0x20, D, MEM(0), 0, 0);	/* pinsrb */
			}
			break;
		default:
			emit(c, 0x66, 1, 0xc4, D, MEM(0), 0, 0);	/* pinsrw */
			break;
		}
	} else {
		mov_rr(c, D, in->rm);
	}
	switch (o) {
	case 0x18: case 0x58:
		emit(c, 0x66, 1, 0x70, D, REG(D), 0, 0);		/* pshufd */
		break;
	case 0x19: case 0x59:
		emit(c, 0x66, 1, 0x6c, D, REG(D), 0, -1);		/* punpcklqdq */
		break;
	default:
		if (o == 0x78)
			emit(c, 0x66, 1, 0x60, D, REG(D), 0, -1);	/* punpcklbw */
		emit(c, 0xf2, 1, 0x70, D, REG(D), 0, 0);		/* pshuflw */
		emit(c, 0x66, 1, 0x6c, D, REG(D), 0, -1);
		break;
	}
	if (in->L)
		store_gs(c, D, HI(D));
	else
		zero_hi(c, D);
}

static void do_pmovx(struct ctx *c)
{
	static const u8 kk[6] = { 8, 4, 2, 8, 4, 8 };
	const struct fa_insn *in = c->in;
	int D = in->reg, B = in->rm;
	int k = kk[in->opcode & 7];

	if ((in->opcode & 7) > 5) {
		c->err = 1;
		return;
	}
	if (!in->L) {
		op(c, D, is_mem(c) ? MEM(0) : REG(B), -1);
		zero_hi(c, D);
		return;
	}
	if (is_mem(c)) {
		int X = hireg(c, D) >= 0 ? hireg(c, D) : D;

		op(c, X, MEM(k), -1);
		store_gs(c, X, HI(D));
		op(c, D, MEM(0), -1);
	} else {
		int X = hireg(c, D);

		if (X < 0)
			X = (D == B) ? xscratch(c) : D;
		mov_rr(c, X, B);
		emit(c, 0x66, 1, 0x73, 3, REG(X), 0, k);		/* psrldq */
		op(c, X, REG(X), -1);
		store_gs(c, X, HI(D));
		op(c, D, REG(B), -1);
	}
}

static void do_perm2(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, A = in->vvvv, B = in->rm;
	int T = xscratch(c), half;

	for (half = 1; half >= 0; half--) {
		int sel = (in->imm >> (4 * half)) & 0xf;
		int X = half ? T : D;

		if (sel & 8) {
			pxor_self(c, X);
			continue;
		}
		switch (sel & 3) {
		case 0:
			mov_rr(c, X, A);
			break;
		case 1:
			load_gs(c, X, HI(A));
			break;
		case 2:
			if (is_mem(c))
				loadu(c, X, 0);
			else
				mov_rr(c, X, B);
			break;
		case 3:
			if (is_mem(c))
				loadu(c, X, 16);
			else
				load_gs(c, X, HI(B));
			break;
		}
	}
	store_gs(c, T, HI(D));
}

static void do_insert128(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, A = in->vvvv, B = in->rm;
	int mem = is_mem(c);

	if (in->imm & 1) {
		if (!mem) {
			store_gs(c, B, HI(D));
		} else {
			int X = hireg(c, D);

			if (X < 0)
				X = (D != A) ? D : xscratch(c);
			loadu(c, X, 0);
			store_gs(c, X, HI(D));
		}
		mov_rr(c, D, A);
		return;
	}
	if (D != A) {
		if (hireg(c, D) >= 0) {
			load_gs(c, hireg(c, D), HI(A));
		} else if (hireg(c, A) >= 0) {
			store_gs(c, hireg(c, A), HI(D));
		} else {
			int X = (!mem && D == B) ? xscratch(c) : D;

			load_gs(c, X, HI(A));
			store_gs(c, X, HI(D));
		}
	}
	if (mem)
		loadu(c, D, 0);
	else
		mov_rr(c, D, B);
}

static void do_extract128(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int S = in->reg, R = in->rm;

	if (in->imm & 1) {
		if (is_mem(c)) {
			int T = hireg(c, S);

			if (T < 0) {
				T = xscratch(c);
				load_gs(c, T, HI(S));
			}
			storeu(c, T, 0);
		} else {
			load_gs(c, R, HI(S));
			zero_hi(c, R);
		}
	} else if (is_mem(c)) {
		storeu(c, S, 0);
	} else {
		mov_rr(c, R, S);
		zero_hi(c, R);
	}
}

static void do_blendv(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, A = in->vvvv, B = in->rm, M = in->imm >> 4;
	int X = xscratch(c), Y = xscratch(c), half;

	for (half = in->L ? 1 : 0; half >= 0; half--) {
		struct opnd m = half ? HI(M) : REG(M);
		struct opnd a = half ? HI(A) : REG(A);

		/* X = element-wise all-ones where the mask sign bit is set */
		switch (in->opcode) {
		case 0x4a:
			emit(c, 0, 1, 0x28, X, m, 0, -1);
			emit(c, 0x66, 1, 0x72, 4, REG(X), 0, 31);	/* psrad */
			break;
		case 0x4b:
			emit(c, 0x66, 1, 0x70, X, m, 0, 0xf5);		/* pshufd */
			emit(c, 0x66, 1, 0x72, 4, REG(X), 0, 31);
			break;
		default:
			pxor_self(c, X);
			emit(c, 0x66, 1, 0x64, X, m, 0, -1);		/* pcmpgtb */
			break;
		}
		if (is_mem(c))
			loadu(c, Y, half ? 16 : 0);
		else if (half)
			load_gs(c, Y, HI(B));
		else
			mov_rr(c, Y, B);
		emit(c, 0x66, 1, 0xef, Y, a, 0, -1);			/* pxor */
		emit(c, 0x66, 1, 0xdb, Y, REG(X), 0, -1);		/* pand */
		emit(c, 0x66, 1, 0xef, Y, a, 0, -1);
		if (half)
			store_gs(c, Y, HI(D));
		else
			mov_rr(c, D, Y);
	}
	if (!in->L)
		zero_hi(c, D);
}

static void do_permilpd(struct ctx *c)
{
	int lo = c->in->imm & 3, hi = (c->in->imm >> 2) & 3;

	c->pfx = 0x66;
	c->map = 1;
	c->opc = 0x70;	/* pshufd */
	c->imm_lo = ((lo & 1) ? 0x0e : 0x04) | ((lo & 2) ? 0xe0 : 0x40);
	c->imm_hi = ((hi & 1) ? 0x0e : 0x04) | ((hi & 2) ? 0xe0 : 0x40);
	do_rm(c);
}

static void do_permq(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, B = in->rm, T = xscratch(c), i;
	struct opnd q[4];

	if (is_mem(c)) {
		for (i = 0; i < 4; i++)
			q[i] = MEM(8 * i);
	} else {
		int hoff = FA_ST_HI + 16 * B;

		/* the four source quadwords have to be addressable in memory */
		store_gs(c, B, GS(FA_ST_XTMP + 48));
		if (hireg(c, B) >= 0) {
			hoff = FA_ST_XTMP + 64;
			store_gs(c, hireg(c, B), GS(hoff));
		}
		q[0] = GS(FA_ST_XTMP + 48);
		q[1] = GS(FA_ST_XTMP + 56);
		q[2] = GS(hoff);
		q[3] = GS(hoff + 8);
	}
	emit(c, 0xf3, 1, 0x7e, T, q[(in->imm >> 4) & 3], 0, -1);	/* movq */
	emit(c, 0, 1, 0x16, T, q[(in->imm >> 6) & 3], 0, -1);		/* movhps */
	emit(c, 0xf3, 1, 0x7e, D, q[in->imm & 3], 0, -1);
	emit(c, 0, 1, 0x16, D, q[(in->imm >> 2) & 3], 0, -1);
	store_gs(c, T, HI(D));
}

static void do_ptest(struct ctx *c)
{
	const struct fa_insn *in = c->in;

	if (is_mem(c)) {
		int T = xscratch(c);

		loadu(c, T, 0);
		op(c, in->reg, REG(T), -1);
	} else {
		op(c, in->reg, REG(in->rm), -1);
	}
}

static void do_helper(struct ctx *c)
{
	u32 fix, i, skip = 0;
	const u8 *d = (const u8 *)c->in;
	u8 ops[4] = { c->in->reg, c->in->vvvv, c->in->reg, c->in->reg };
	u16 sync = 0;

	/* the helper works on the in-memory state: write cached upper halves back */
	if (c->in->mod == 3)
		ops[2] = c->in->rm;
	else if (c->in->mem.has_index)
		ops[3] = c->in->mem.index;	/* VSIB; harmless otherwise */
	for (i = 0; i < 4; i++)
		if (hireg(c, ops[i]) >= 0)
			sync |= 1 << ops[i];
	for (i = 0; i < 16; i++)
		if (sync & (1 << i))
			store_gs(c, hireg(c, i), GS(FA_ST_HI + 16 * i));

	/* lea rsp,[rsp-128]; push rdi; lea rdi,[rip+desc] */
	put(c, 0x48); put(c, 0x8d); put(c, 0x64); put(c, 0x24); put(c, 0x80);
	put(c, 0x57);
	put(c, 0x48); put(c, 0x8d); put(c, 0x3d);
	fix = c->len;
	put32(c, 0);
	emit_call_blob(c, FA_BLOB_HELPER);
	put(c, 0x5f);
	/* lea rsp,[rsp+128] */
	put(c, 0x48); put(c, 0x8d); put(c, 0xa4); put(c, 0x24); put32(c, 128);
	for (i = 0; i < 16; i++)
		if (sync & (1 << i))
			load_gs(c, hireg(c, i), GS(FA_ST_HI + 16 * i));
	if (c->env->no_jmp) {
		put(c, 0xeb);		/* jmp short over the descriptor */
		skip = c->len;
		put(c, 0);
	} else {
		emit_jmp_abs(c, c->in->rip + c->in->len);
	}
	while (c->len & 7)
		put(c, 0xcc);
	if (c->err)
		return;
	i = c->len - (fix + 4);
	memcpy(c->p + fix, &i, 4);
	for (i = 0; i < sizeof(struct fa_insn); i++)
		put(c, d[i]);
	if (skip && !c->err)
		c->p[skip] = c->len - (skip + 1);
}

static void do_popcnt(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg;
	struct opnd src = is_mem(c) ? MEM(0) : REG(in->rm);
	int save = (D != 0) || in->opsize16;

	if (save)
		gpr_gs(c, 1, 0, 0);
	if (in->opsize16)
		emit(c, 0, 1, 0xb7, 0, src, 0, -1);		/* movzx eax, r/m16 */
	else
		emit(c, 0, 0, 0x8b, 0, src, in->W, -1);		/* mov (r|e)ax, r/m */
	put(c, 0x48); put(c, 0x8d); put(c, 0x64); put(c, 0x24); put(c, 0x80);
	emit_call_blob(c, FA_BLOB_POPCNT);
	put(c, 0x48); put(c, 0x8d); put(c, 0xa4); put(c, 0x24); put32(c, 128);
	if (D != 0) {
		/* mov D, (r|e)ax ; restore rax */
		emit(c, in->opsize16 ? 0x66 : 0, 0, 0x89, 0, REG(D), in->W, -1);
		gpr_gs(c, 0, 0, 0);
	} else if (in->opsize16) {
		/* merge the 16-bit result into the saved rax */
		emit(c, 0x66, 0, 0x89, 0, GS(FA_ST_GTMP), 0, -1);
		gpr_gs(c, 0, 0, 0);
	}
}

/*
 * Legacy crc32 (SSE4.2): eax = crc, edx = data, ecx = byte count, then the
 * table-driven routine in the helper blob, instead of the generic C helper.
 * rax, rcx and rdx are kept in gpr spill slots 0, 1 and 3 (slot 2 may hold a
 * scratch register for an unreachable rip-relative operand) and are read
 * from there once they have been overwritten.
 */
static void do_crc32(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int D = in->reg, size = in->opcode == 0xf0 ? 1 : in->W ? 8 : in->opsize16 ? 2 : 4;
	struct opnd src = is_mem(c) ? MEM(0) : REG(in->rm);
	static const s8 slot[3] = { 0, 1, 3 };
	int r;

	if (c->ng > 1) {
		c->err = 1;
		return;
	}
	for (r = 0; r < 3; r++)
		gpr_gs(c, 1, r, slot[r]);
	/* data, zero-extended, into rdx: nothing has been overwritten yet */
	if (size == 1) {
		if (in->rex && !is_mem(c) && in->rm >= 4 && in->rm < 8)
			put(c, 0x40);		/* spl..dil, not ah..bh */
		emit(c, 0, 1, 0xb6, 2, src, 0, -1);	/* movzx edx, r/m8 */
	} else if (size == 2) {
		emit(c, 0, 1, 0xb7, 2, src, 0, -1);	/* movzx edx, r/m16 */
	} else {
		emit(c, 0, 0, 0x8b, 2, src, size == 8, -1);	/* mov (e|r)dx, r/m */
	}
	/* crc into eax (rdx is no longer the guest's) */
	emit(c, 0, 0, 0x8b, 0, D == 2 ? GS(FA_ST_GTMP + 8 * slot[2]) : REG(D), 0, -1);
	put(c, 0xb9);				/* mov ecx, size */
	put32(c, size);
	put(c, 0x48); put(c, 0x8d); put(c, 0x64); put(c, 0x24); put(c, 0x80);
	emit_call_blob(c, FA_BLOB_CRC32);
	put(c, 0x48); put(c, 0x8d); put(c, 0xa4); put(c, 0x24); put32(c, 128);
	/* result: 32 bits, zero-extended into the destination */
	if (D != 0)
		emit(c, 0, 0, 0x89, 0, REG(D), 0, -1);	/* mov D32, eax */
	for (r = 0; r < 3; r++)
		if (r != D)
			gpr_gs(c, 0, r, slot[r]);
}

/* Rewrite an unreachable RIP-relative operand as [scratch gpr]. */
static void fix_riprel(struct ctx *c, u16 avoid)
{
	const struct fa_insn *in = c->in;
	u64 target = in->rip + in->len + (s64)in->mem.disp;
	s64 d = (s64)(target - c->env->out_addr);
	int g;

	if (in->mod == 3 || !in->mem.riprel)
		return;
	if (d > -0x7ff00000LL && d < 0x7ff00000LL)
		return;
	if (in->mem.addr32) {
		c->err = 1;
		return;
	}
	g = gscratch(c, avoid);
	put(c, 0x48 | (g >> 3));		/* movabs g, target */
	put(c, 0xb8 | (g & 7));
	put64(c, target);
	c->mem.riprel = 0;
	c->mem.has_base = 1;
	c->mem.base = g;
	c->mem.has_index = 0;
	c->mem.disp = 0;
}

/*
 * A legacy SSE4.1 instruction on a CPU that has SSE4.1. It only reaches the
 * translator when it sits inside a region (on its own it never traps), and
 * it runs as it is: it reads and writes only the low halves, which live in
 * the host registers of the same number. Only a RIP-relative operand that
 * the code cache cannot reach needs rewriting.
 */
static void do_native(struct ctx *c)
{
	const struct fa_insn *in = c->in;

	fix_riprel(c, 0);
	emit(c, 0x66, in->map, in->opcode, in->reg, is_mem(c) ? MEM(0) : REG(in->rm), in->W,
	     in->has_imm ? in->imm : -1);
}

/*
 * Inline replacements for instructions that used to go through the C helper.
 * A helper call saves and restores every register, the flags and MXCSR, and
 * decodes the instruction again: hundreds of cycles. These sequences work a
 * 128-bit lane at a time, with at most three scratch registers, and use only
 * SSE operations, which leave EFLAGS alone (ptest sets exactly the flags the
 * guest instruction sets).
 */

/* lane l (0: bits 127:0, 1: bits 255:128) of guest register r into host register T */
static void lane_reg(struct ctx *c, int T, int r, int l)
{
	if (l)
		load_gs(c, T, HI(r));
	else
		mov_rr(c, T, r);
}

/* lane l of the rm operand, register or (unaligned) memory, into T */
static void lane_rm(struct ctx *c, int T, int l)
{
	if (is_mem(c))
		loadu(c, T, 16 * l);
	else
		lane_reg(c, T, c->in->rm, l);
}

static struct opnd lane_opnd(struct ctx *c, int r, int l)
{
	return l ? HI(r) : REG(r);
}

/* 66 0F op */
static void sse2(struct ctx *c, int opc, int d, struct opnd s, int imm)
{
	emit(c, 0x66, 1, opc, d, s, 0, imm);
}

/* write a lane result held in T */
static void lane_out(struct ctx *c, int T, int l)
{
	if (l)
		store_gs(c, T, HI(c->in->reg));
	else
		mov_rr(c, c->in->reg, T);
}

/*
 * vpcmpgtq: a > b (signed) is the sign of (b & ~a) | (~(a ^ b) & (b - a)),
 * exact even where b - a overflows; then spread the sign over the qword.
 */
static void do_pcmpgtq(struct ctx *c)
{
	int A = c->in->vvvv, l, T1 = xscratch(c), T2 = xscratch(c), T3 = xscratch(c);

	for (l = c->in->L; l >= 0; l--) {
		struct opnd a = lane_opnd(c, A, l);

		lane_rm(c, T1, l);			/* b */
		mov_rr(c, T3, T1);			/* b */
		sse2(c, 0xfb, T1, a, -1);		/* psubq: b - a */
		lane_reg(c, T2, A, l);
		sse2(c, 0xef, T2, REG(T3), -1);		/* pxor: a ^ b */
		sse2(c, 0xdf, T2, REG(T1), -1);		/* pandn: ~(a ^ b) & (b - a) */
		lane_reg(c, T1, A, l);
		sse2(c, 0xdf, T1, REG(T3), -1);		/* pandn: ~a & b */
		sse2(c, 0xeb, T1, REG(T2), -1);		/* por */
		sse2(c, 0x72, 4, REG(T1), 31);		/* psrad $31 */
		sse2(c, 0x70, T1, REG(T1), 0xf5);	/* pshufd: high dword to both */
		lane_out(c, T1, l);
	}
	if (!c->in->L)
		zero_hi(c, c->in->reg);
}

/*
 * vptest (256-bit), vtestps, vtestpd: ZF = (src1 & src2) == 0 and
 * CF = (~src1 & src2) == 0 over all lanes (only the sign bits for vtest).
 * Both are folded into one register, z in the low qword and c in the high
 * one, and a single ptest against "low qword set" yields both flags.
 */
static void do_vtest(struct ctx *c, int bits)	/* 0: all, 32 / 64: sign bits */
{
	int S = c->in->reg, l, T1 = xscratch(c), T2 = xscratch(c), T3 = xscratch(c);

	for (l = c->in->L; l >= 0; l--) {
		struct opnd a = lane_opnd(c, S, l);
		int and = l == c->in->L ? T1 : T3;

		lane_rm(c, and, l);
		sse2(c, 0xdb, and, a, -1);		/* pand: src1 & src2 */
		if (and != T1)
			sse2(c, 0xeb, T1, REG(T3), -1);
		lane_rm(c, T3, l);
		sse2(c, 0xeb, T3, a, -1);		/* por */
		sse2(c, 0xef, T3, a, -1);		/* pxor: (s1 | s2) ^ s1 = ~s1 & s2 */
		if (l == c->in->L)
			mov_rr(c, T2, T3);
		else
			sse2(c, 0xeb, T2, REG(T3), -1);
	}
	if (bits == 32) {
		sse2(c, 0x72, 2, REG(T1), 31);		/* psrld $31 */
		sse2(c, 0x72, 2, REG(T2), 31);
	} else if (bits == 64) {
		sse2(c, 0x73, 2, REG(T1), 63);		/* psrlq $63 */
		sse2(c, 0x73, 2, REG(T2), 63);
	}
	mov_rr(c, T3, T1);
	sse2(c, 0x6c, T3, REG(T2), -1);			/* punpcklqdq: [z.lo, c.lo] */
	sse2(c, 0x6d, T1, REG(T2), -1);			/* punpckhqdq: [z.hi, c.hi] */
	sse2(c, 0xeb, T1, REG(T3), -1);			/* por */
	sse2(c, 0x76, T2, REG(T2), -1);			/* pcmpeqd: all ones */
	sse2(c, 0x73, 3, REG(T2), 8);			/* psrldq $8: low qword set */
	emit(c, 0x66, 2, 0x17, T2, REG(T1), 0, -1);	/* ptest */
}

/*
 * vpsllvd/q, vpsrlvd/q, vpsravd: each element shifted by its own count. The
 * xmm-count forms of the SSE shifts take a 64-bit count and already give 0
 * (or the sign, for psrad) for counts past the element width, as AVX2 does;
 * so each count is zero-extended into its own register, the whole lane
 * shifted by it, and the one element kept with pblendw.
 */
static void do_varshift(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int A = in->vvvv, q = in->W, l, i;
	int opc = in->opcode == 0x47 ? (q ? 0xf3 : 0xf2) :	/* psllq / pslld */
		  in->opcode == 0x45 ? (q ? 0xd3 : 0xd2) : 0xe2;	/* psrlq / psrld, psrad */
	int R = xscratch(c), T = xscratch(c), S = xscratch(c);

	for (l = in->L; l >= 0; l--) {
		for (i = 0; i < (q ? 2 : 4); i++) {
			int dst = i ? S : R;

			lane_rm(c, T, l);			/* counts */
			if (i)
				sse2(c, 0x73, 3, REG(T), (q ? 8 : 4) * i);	/* psrldq */
			if (!q)
				emit(c, 0x66, 2, 0x35, T, REG(T), 0, -1);	/* pmovzxdq */
			lane_reg(c, dst, A, l);
			sse2(c, opc, dst, REG(T), -1);
			if (i)
				emit(c, 0x66, 3, 0x0e, R, REG(S), 0,	/* pblendw */
				     q ? 0xf0 : 3 << (2 * i));
		}
		lane_out(c, R, l);
	}
	if (!in->L)
		zero_hi(c, in->reg);
}

/* Which inline sequence, if any, replaces the helper for this instruction. */
enum { IL_NONE, IL_PCMPGTQ, IL_VTEST, IL_VARSHIFT };

static int inline_kind(const struct fa_insn *in, int no_sse41)
{
	if (in->kind != FA_K_VEX || in->map != 2 || in->pp != 1)
		return IL_NONE;
	switch (in->opcode) {
	case 0x37:					/* vpcmpgtq: SSE2 only */
		return IL_PCMPGTQ;
	case 0x17:					/* vptest ymm */
		return !no_sse41 && in->L ? IL_VTEST : IL_NONE;
	case 0x0e: case 0x0f:				/* vtestps, vtestpd */
		return !no_sse41 ? IL_VTEST : IL_NONE;
	case 0x45: case 0x47:				/* vpsrlvd/q, vpsllvd/q */
		return !no_sse41 ? IL_VARSHIFT : IL_NONE;
	case 0x46:					/* vpsravd */
		return !no_sse41 && !in->W ? IL_VARSHIFT : IL_NONE;
	}
	return IL_NONE;
}

int fa_scratch_needed(const struct fa_insn *in, int no_sse41)
{
	return inline_kind(in, no_sse41) ? 3 : 2;
}

/* Returns 1 if the instruction was translated inline instead of by the helper. */
static int do_inline(struct ctx *c)
{
	const struct fa_insn *in = c->in;
	int k = inline_kind(in, c->env->no_sse41);

	if (!k)
		return 0;
	fix_riprel(c, 0);
	if (k == IL_PCMPGTQ)
		do_pcmpgtq(c);
	else if (k == IL_VTEST)
		do_vtest(c, in->opcode == 0x17 ? 0 : in->opcode == 0x0e ? 32 : 64);
	else
		do_varshift(c);
	return 1;
}

int fa_translate(const struct fa_insn *in, const struct fa_env *env)
{
	struct ctx ctx, *c = &ctx;
	const struct fa_op *o;
	u16 fl;

	memset(c, 0, sizeof(*c));
	c->in = in;
	c->env = env;
	c->p = env->out;
	c->cap = env->cap;
	c->mem = in->mem;
	c->imm_lo = c->imm_hi = -1;

	if (in->kind == FA_K_POPCNT) {
		fix_riprel(c, (1 << 0) | (1 << (in->reg & 15)));
		do_popcnt(c);
		finish(c);
		return c->err ? -1 : (int)c->len;
	}
	if (in->kind == FA_K_SSE4) {
		if (in->map == 2 && (in->opcode == 0xf0 || in->opcode == 0xf1) && in->pp == 3) {
			fix_riprel(c, 0x7 | (1 << (in->reg & 15)));
			do_crc32(c);
			finish(c);
		} else if (!env->no_sse41 && fa_is_sse41(in->map, in->opcode)) {
			do_native(c);
			finish(c);
		} else {
			do_helper(c);
		}
		return c->err ? -1 : (int)c->len;
	}
	if (in->kind != FA_K_VEX)
		return -1;

	o = fa_lookup(in->map, in->pp, in->opcode);
	if (!o)
		return -1;
	c->op = o;
	fl = o->flags;

	if (o->alt_map) {
		c->pfx = pp_prefix[o->alt_pp];
		c->map = o->alt_map;
		c->opc = o->alt_opcode;
	} else {
		c->pfx = pp_prefix[in->pp];
		c->map = in->map;
		c->opc = in->opcode;
	}
	c->rexw = (fl & FA_F_W) ? in->W : 0;
	if (in->has_imm) {
		if (fl & FA_F_SPLIT4) {
			c->imm_lo = in->imm & 0xf;
			c->imm_hi = in->imm >> 4;
		} else if (fl & FA_F_SPLIT2) {
			c->imm_lo = in->imm & 3;
			c->imm_hi = (in->imm >> 2) & 3;
		} else if (fl & FA_F_SPLIT3) {
			c->imm_lo = in->imm & 7;
			c->imm_hi = (in->imm >> 3) & 7;
		} else {
			c->imm_lo = c->imm_hi = in->imm;
		}
	}

	c->used = (1 << in->reg) | (1 << in->vvvv);
	if (env->map)
		c->used |= env->map->reserved;
	if (in->mod == 3)
		c->used |= 1 << in->rm;
	if (o->shape == FA_S_BLENDV)
		c->used |= 1 << (in->imm >> 4);

	/* SSSE3-only host: anything that would need an SSE4.1 instruction */
	if (env->no_sse41) {
		if ((in->map == 2 || in->map == 3) && in->opcode != 0x2a &&
		    (fa_is_sse41(in->map, in->opcode) || (in->map == 3 && in->opcode == 0x02))) {
			do_helper(c);
			return c->err ? -1 : (int)c->len;
		}
		if (in->map == 2 && in->opcode == 0x2a) {	/* movntdqa -> movdqa */
			c->map = 1;
			c->opc = 0x6f;
		}
	}
	if ((o->shape == FA_S_HELPER || (o->shape == FA_S_PTEST && in->L)) && do_inline(c)) {
		finish(c);
		return c->err ? -1 : (int)c->len;
	}
	if (o->shape == FA_S_HELPER ||
	    (o->shape == FA_S_PTEST && in->L)) {
		do_helper(c);
		return c->err ? -1 : (int)c->len;
	}

	if (fl & FA_F_CMP) {
		static const s8 swapped[16] = {
			-1, -1, -1, -1, -1, -1, -1, -1, -1, 6, 5, -1, -1, 2, 1, -1
		};
		int p = in->imm & 0xf;

		if (p >= 8) {
			if (swapped[p] < 0 || (fl & FA_F_SCALAR)) {
				do_helper(c);
				return c->err ? -1 : (int)c->len;
			}
			c->imm_lo = c->imm_hi = swapped[p];
			fix_riprel(c, 0);
			do_rvm_swapped(c);
			finish(c);
			return c->err ? -1 : (int)c->len;
		}
		c->imm_lo = c->imm_hi = p;
	}

	/* the gpr operands of this instruction must not become scratch */
	fix_riprel(c, (1 << (in->reg & 15)) | (1 << in->vvvv));

	switch (o->shape) {
	case FA_S_RVM:		do_rvm(c); break;
	case FA_S_RM:		do_rm(c); break;
	case FA_S_SHIFTI:	do_shifti(c); break;
	case FA_S_LOAD:		do_load(c); break;
	case FA_S_STORE:	do_store(c); break;
	case FA_S_PASS:		do_pass(c); break;
	case FA_S_MOVSS:	do_movss(c); break;
	case FA_S_MOVMSK:	do_movmsk(c); break;
	case FA_S_ZERO:		do_zero(c); break;
	case FA_S_CVT_WIDEN:	do_cvt_widen(c); break;
	case FA_S_CVT_NARROW:	do_cvt_narrow(c); break;
	case FA_S_BCAST:	do_bcast(c); break;
	case FA_S_PMOVX:	do_pmovx(c); break;
	case FA_S_PTEST:	do_ptest(c); break;
	case FA_S_PERM2:	do_perm2(c); break;
	case FA_S_INSERT128:	do_insert128(c); break;
	case FA_S_EXTRACT128:	do_extract128(c); break;
	case FA_S_BLENDV:	do_blendv(c); break;
	case FA_S_PERMILPD:	do_permilpd(c); break;
	case FA_S_PERMQ:	do_permq(c); break;
	default:
		return -1;
	}
	finish(c);
	return c->err ? -1 : (int)c->len;
}

int fa_emit_jmp(u8 *out, u64 at, u64 target)
{
	s64 rel = (s64)(target - (at + 5));

	if (rel == (s32)rel) {
		s32 r = (s32)rel;

		out[0] = 0xe9;
		memcpy(out + 1, &r, 4);
		return 5;
	}
	out[0] = 0xff;
	out[1] = 0x25;
	memset(out + 2, 0, 4);
	memcpy(out + 6, &target, 8);
	return 14;
}
