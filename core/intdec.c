// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: whitelist decoder for general-purpose instructions (64-bit mode).
 *
 * A region carries the integer instructions between AVX instructions along
 * into the code cache. This decoder only says yes to encodings it fully
 * understands - length, branch target, rip-relative operand - and that
 * cannot touch xmm registers or %gs.
 */
#include "fakeavx.h"

enum { NONE = 0, M = 1, I8 = 2, IZ = 4, IV = 8 };	/* modrm, imm8, imm16/32, imm16/32/64 */

static int op1(u8 b, int rep)
{
	if (rep && !((b >= 0xa4 && b <= 0xa7) || (b >= 0xaa && b <= 0xaf) || b == 0x90))
		return -1;
	if (b < 0x40) {
		switch (b & 7) {
		case 0: case 1: case 2: case 3: return M;
		case 4: return I8;
		case 5: return IZ;
		default: return -1;
		}
	}
	if (b >= 0x50 && b <= 0x5f) return NONE;		/* push/pop reg */
	if (b >= 0x84 && b <= 0x8b) return M;			/* test/xchg/mov */
	if (b >= 0x90 && b <= 0x99) return NONE;		/* nop/xchg/cdqe/cqo */
	if ((b >= 0xa4 && b <= 0xa7) || (b >= 0xaa && b <= 0xaf)) return NONE;
	if (b >= 0xb0 && b <= 0xb7) return I8;
	if (b >= 0xb8 && b <= 0xbf) return IV;
	if (b >= 0xd0 && b <= 0xd3) return M;
	switch (b) {
	case 0x63: return M;
	case 0x68: return IZ;
	case 0x69: return M | IZ;
	case 0x6a: return I8;
	case 0x6b: return M | I8;
	case 0x80: case 0x83: return M | I8;
	case 0x81: return M | IZ;
	case 0x8d: return M;
	case 0xa8: return I8;
	case 0xa9: return IZ;
	case 0xc0: case 0xc1: return M | I8;
	case 0xc6: return M | I8;
	case 0xc7: return M | IZ;
	case 0xc9: return NONE;
	case 0xf6: case 0xf7: case 0xfe: case 0xff: return M;	/* refined below */
	}
	return -1;
}

static int op2(u8 b, int rep)
{
	if (rep && b != 0xbc && b != 0xbd && b != 0x1e)
		return -1;
	if (b >= 0x40 && b <= 0x4f) return M;			/* cmovcc */
	if (b >= 0x90 && b <= 0x9f) return M;			/* setcc */
	if (b >= 0xc8 && b <= 0xcf) return NONE;		/* bswap */
	switch (b) {
	case 0x1e: case 0x1f: return M;				/* nop / endbr */
	case 0x31: case 0xa2: return NONE;			/* rdtsc, cpuid */
	case 0xa3: case 0xab: case 0xb3: case 0xbb: return M;	/* bt* */
	case 0xa4: case 0xac: return M | I8;			/* shld/shrd */
	case 0xa5: case 0xad: return M;
	case 0xaf: return M;					/* imul */
	case 0xb0: case 0xb1: return M;				/* cmpxchg */
	case 0xb6: case 0xb7: case 0xbe: case 0xbf: return M;	/* movzx/movsx */
	case 0xba: return M | I8;
	case 0xbc: case 0xbd: return M;				/* bsf/bsr (tzcnt/lzcnt) */
	case 0xc0: case 0xc1: return M;				/* xadd */
	}
	return -1;
}

int fa_decode_int(const u8 *code, u32 avail, u64 ip, struct fa_int *o)
{
	u32 n = 0;
	int p66 = 0, rep = 0, rexw = 0, lock = 0, fl, two = 0;
	u8 b, modrm = 0;

	memset(o, 0, sizeof(*o));
	if (avail > 15)
		avail = 15;
	for (; n < avail; n++) {
		b = code[n];
		if (b == 0x66)
			p66 = 1;
		else if (b == 0x67)
			return -1;	/* 32-bit addressing: not worth the corner cases */
		else if (b == 0xf2 || b == 0xf3)
			rep = 1;
		else if (b == 0xf0)
			lock = 1;
		else if (b != 0x64 && b != 0x2e && b != 0x3e && b != 0x26 && b != 0x36)
			break;	/* %fs, and segment prefixes that mean nothing in 64-bit mode */
	}
	if (n >= avail)
		return -1;
	if ((code[n] & 0xf0) == 0x40) {
		rexw = (code[n] >> 3) & 1;
		if (rexw)
			p66 = 0;	/* REX.W wins over the operand-size prefix */
		if (++n >= avail)
			return -1;
	}
	b = code[n++];

	/* branches */
	if (b >= 0x70 && b <= 0x7f) {
		if (p66 || rep || lock || n + 1 > avail)
			return -1;
		o->kind = FA_I_JCC;
		o->cc = b & 15;
		o->len = n + 1;
		o->target = ip + o->len + (s8)code[n];
		return 0;
	}
	if (b == 0xeb || b == 0xe9) {
		s32 d;

		if (p66 || rep || lock || n + (b == 0xeb ? 1 : 4) > avail)
			return -1;
		o->kind = FA_I_JMP;
		if (b == 0xeb) {
			o->len = n + 1;
			o->target = ip + o->len + (s8)code[n];
		} else {
			memcpy(&d, code + n, 4);
			o->len = n + 4;
			o->target = ip + o->len + d;
		}
		return 0;
	}
	if (b == 0x0f) {
		if (n >= avail)
			return -1;
		b = code[n++];
		if (b >= 0x80 && b <= 0x8f) {
			s32 d;

			if (p66 || rep || lock || n + 4 > avail)
				return -1;
			memcpy(&d, code + n, 4);
			o->kind = FA_I_JCC;
			o->cc = b & 15;
			o->len = n + 4;
			o->target = ip + o->len + d;
			return 0;
		}
		two = 1;
		fl = op2(b, rep);
	} else {
		fl = op1(b, rep);
	}
	if (fl < 0)
		return -1;

	if (fl & M) {
		u8 mod, rm;

		if (n >= avail)
			return -1;
		modrm = code[n++];
		mod = modrm >> 6;
		rm = modrm & 7;
		if (mod != 3) {
			int disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;

			if (rm == 4) {
				if (n >= avail)
					return -1;
				if ((code[n++] & 7) == 5 && mod == 0)
					disp = 4;
			} else if (rm == 5 && mod == 0) {
				disp = 4;
				o->disp_off = n;
			}
			n += disp;
		}
		if (two) {
			if (b == 0xba && ((modrm >> 3) & 7) < 4)
				return -1;
			if (b == 0x1e && !(rep && (modrm == 0xfa || modrm == 0xfb)))
				return -1;	/* only endbr64/endbr32 */
		} else {
			int ext = (modrm >> 3) & 7;

			switch (b) {
			case 0x8d:
				if (mod == 3)
					return -1;
				break;
			case 0xc6: case 0xc7:
				if (ext)
					return -1;
				break;
			case 0xf6:
				if (ext < 2)
					fl |= I8;
				break;
			case 0xf7:
				if (ext < 2)
					fl |= IZ;
				break;
			case 0xfe:
				if (ext > 1)
					return -1;
				break;
			case 0xff:
				if (ext != 0 && ext != 1 && ext != 6)
					return -1;	/* indirect call/jmp */
				break;
			}
		}
	}
	if (lock) {
		/* LOCK is only valid on a few read-modify-write memory forms */
		int ext = (modrm >> 3) & 7, ok;

		if (!(fl & M) || (modrm >> 6) == 3)
			return -1;
		if (two)
			ok = b == 0xab || b == 0xb3 || b == 0xbb || b == 0xb0 || b == 0xb1 ||
			     b == 0xc0 || b == 0xc1 || (b == 0xba && ext >= 5);
		else
			ok = (b < 0x38 && (b & 7) < 2) || b == 0x86 || b == 0x87 ||
			     ((b == 0x80 || b == 0x81 || b == 0x83) && ext != 7) ||
			     ((b == 0xf6 || b == 0xf7) && (ext == 2 || ext == 3)) ||
			     ((b == 0xfe || b == 0xff) && ext < 2);
		if (!ok)
			return -1;
	}
	if (fl & I8)
		n += 1;
	if (fl & IZ)
		n += p66 ? 2 : 4;
	if (fl & IV)
		n += rexw ? 8 : p66 ? 2 : 4;
	if (n > avail)
		return -1;
	o->kind = FA_I_PLAIN;
	o->len = n;
	if (o->disp_off) {
		s32 d;

		memcpy(&d, code + o->disp_off, 4);
		o->target = ip + n + d;
	}
	return 0;
}
