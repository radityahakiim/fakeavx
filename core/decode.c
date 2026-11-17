// SPDX-License-Identifier: GPL-2.0
/* fakeavx: decoder for VEX-encoded instructions, POPCNT and XGETBV (64-bit mode). */
#include "fakeavx.h"

static int decode_modrm(const u8 *p, u32 avail, struct fa_insn *in,
			int rex_r, int rex_x, int rex_b, int vsib)
{
	u32 n = 0;
	u8 modrm, mod, rm;

	if (avail < 1)
		return -1;
	modrm = p[n++];
	mod = modrm >> 6;
	rm = modrm & 7;
	in->mod = mod;
	in->reg = ((modrm >> 3) & 7) | (rex_r << 3);
	in->rm = rm | (rex_b << 3);
	if (mod == 3)
		return n;

	if (rm == 4) {
		u8 sib, idx, base;

		if (avail < n + 1)
			return -1;
		sib = p[n++];
		in->mem.scale = sib >> 6;
		idx = ((sib >> 3) & 7) | (rex_x << 3);
		base = (sib & 7) | (rex_b << 3);
		if (idx != 4 || vsib) {
			in->mem.has_index = 1;
			in->mem.index = idx;
		}
		if ((sib & 7) == 5 && mod == 0) {
			if (avail < n + 4)
				return -1;
			memcpy(&in->mem.disp, p + n, 4);
			n += 4;
		} else {
			in->mem.has_base = 1;
			in->mem.base = base;
		}
	} else if (rm == 5 && mod == 0) {
		in->mem.riprel = 1;
		if (avail < n + 4)
			return -1;
		memcpy(&in->mem.disp, p + n, 4);
		n += 4;
	} else {
		in->mem.has_base = 1;
		in->mem.base = in->rm;
	}

	if (mod == 1) {
		if (avail < n + 1)
			return -1;
		in->mem.disp = (s8)p[n++];
	} else if (mod == 2) {
		if (avail < n + 4)
			return -1;
		memcpy(&in->mem.disp, p + n, 4);
		n += 4;
	}
	return n;
}

int fa_decode(const u8 *code, u32 avail, u64 rip, struct fa_insn *in)
{
	u32 n = 0;
	int r, rex = 0, f3 = 0, f2 = 0, p66 = 0;
	const struct fa_op *op;

	memset(in, 0, sizeof(*in));
	in->rip = rip;
	if (avail > 15)
		avail = 15;

	/* Legacy prefixes that may precede VEX: segment and address-size only. */
	for (; n < avail; n++) {
		u8 b = code[n];

		if (b == 0x64 || b == 0x65)
			in->mem.seg = b;
		else if (b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36)
			;	/* null segment prefixes in 64-bit mode */
		else if (b == 0x67)
			in->mem.addr32 = 1;
		else if (b == 0xf3)
			f3 = 1;
		else if (b == 0xf2)
			f2 = 1;
		else if (b == 0x66)
			p66 = 1;
		else
			break;
	}
	if (n >= avail)
		return -1;

	if (code[n] == 0xc4 || code[n] == 0xc5) {
		int rr, rx = 0, rb = 0;

		if (f3 || f2 || p66)
			return -1;	/* #UD by definition */
		in->kind = FA_K_VEX;
		if (code[n] == 0xc5) {
			u8 b1;

			if (avail < n + 3)
				return -1;
			b1 = code[n + 1];
			rr = !(b1 & 0x80);
			in->vvvv = (~b1 >> 3) & 15;
			in->L = (b1 >> 2) & 1;
			in->pp = b1 & 3;
			in->map = 1;
			n += 2;
		} else {
			u8 b1, b2;

			if (avail < n + 4)
				return -1;
			b1 = code[n + 1];
			b2 = code[n + 2];
			rr = !(b1 & 0x80);
			rx = !(b1 & 0x40);
			rb = !(b1 & 0x20);
			in->map = b1 & 0x1f;
			in->W = b2 >> 7;
			in->vvvv = (~b2 >> 3) & 15;
			in->L = (b2 >> 2) & 1;
			in->pp = b2 & 3;
			n += 3;
		}
		if (in->map < 1 || in->map > 3)
			return -1;
		in->opcode = code[n++];
		op = fa_lookup(in->map, in->pp, in->opcode);
		if (!op)
			return -1;

		if (op->shape == FA_S_ZERO) {
			in->len = n;
			return 0;
		}
		r = decode_modrm(code + n, avail - n, in, rr, rx, rb,
				 in->map == 2 && in->opcode >= 0x90 && in->opcode <= 0x93);
		if (r < 0)
			return -1;
		n += r;
		if (op->has_imm) {
			if (avail < n + 1)
				return -1;
			in->has_imm = 1;
			in->imm = code[n++];
		}
		if ((op->flags & FA_F_L0) && in->L)
			return -1;
		if ((op->flags & FA_F_L1) && !in->L)
			return -1;
		if ((op->flags & FA_F_MEMONLY) && in->mod == 3)
			return -1;
		if ((op->flags & FA_F_REGONLY) && in->mod != 3)
			return -1;
		if (in->mem.seg == 0x65 && in->mod != 3)
			return -1;	/* %gs is ours */
		in->len = n;
		return 0;
	}

	/* REX */
	if ((code[n] & 0xf0) == 0x40) {
		rex = code[n++];
		if (n >= avail)
			return -1;
	}
	if (avail < n + 3 || code[n] != 0x0f)
		return -1;

	if (f2 && f3)
		return -1;
	if (f3 && !f2 && code[n + 1] == 0xb8) {
		n += 2;
		in->kind = FA_K_POPCNT;
		in->W = (rex >> 3) & 1;
		in->opsize16 = p66 && !in->W;
		r = decode_modrm(code + n, avail - n, in,
				 (rex >> 2) & 1, (rex >> 1) & 1, rex & 1, 0);
		if (r < 0)
			return -1;
		n += r;
		if (in->mem.seg == 0x65 && in->mod != 3)
			return -1;
		in->len = n;
		return 0;
	}
	/* SSE4.1 and SSE4.2, legacy encoding */
	if (!f3 && avail >= n + 4 && (code[n + 1] == 0x38 || code[n + 1] == 0x3a)) {
		u8 esc = code[n + 1], opc = code[n + 2];
		int ok;

		if (esc == 0x38)
			ok = (opc == 0x37 && p66 && !f2) ||		/* pcmpgtq */
			     ((opc == 0xf0 || opc == 0xf1) && f2) ||	/* crc32 */
			     (p66 && !f2 && fa_is_sse41(2, opc));
		else
			ok = p66 && !f2 && ((opc >= 0x60 && opc <= 0x63) ||	/* pcmpXstrX */
					    fa_is_sse41(3, opc));
		if (!ok)
			return -1;
		n += 3;
		in->kind = FA_K_SSE4;
		in->map = esc == 0x38 ? 2 : 3;
		in->pp = f2 ? 3 : 1;
		in->opcode = opc;
		in->W = (rex >> 3) & 1;
		in->rex = !!rex;
		in->opsize16 = p66 && !in->W;
		r = decode_modrm(code + n, avail - n, in,
				 (rex >> 2) & 1, (rex >> 1) & 1, rex & 1, 0);
		if (r < 0)
			return -1;
		n += r;
		if (esc == 0x3a) {
			if (avail < n + 1)
				return -1;
			in->has_imm = 1;
			in->imm = code[n++];
		}
		if (in->mem.seg == 0x65 && in->mod != 3)
			return -1;
		in->len = n;
		return 0;
	}
	if (!f3 && !f2 && !p66 && !rex && code[n + 1] == 0x01 && code[n + 2] == 0xd0) {
		in->kind = FA_K_XGETBV;
		in->len = n + 3;
		return 0;
	}
	return -1;
}
