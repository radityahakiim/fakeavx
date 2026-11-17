// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: whitelist decoder for legacy-encoded SSE, SSE2, SSE3, SSSE3 and
 * MMX instructions, and prefetches (64-bit mode).
 *
 * Every Core 2 has these, so inside a region they are copied as they are,
 * like the integer instructions intdec.c accepts; before, any of them ended
 * the region, so a loop mixing them with AVX never ran entirely in the code
 * cache. The region must know which xmm registers such an instruction uses,
 * so as not to borrow them: *xmm receives the registers named by the reg and
 * (register form) rm fields. That may include an mm or general-purpose
 * register number, which only costs a register the region could have
 * borrowed. None of the accepted instructions uses an xmm register
 * implicitly.
 *
 * Only encodings that are valid as written are accepted: an invalid prefix
 * combination or operand form would raise #UD in the code cache instead of
 * in the program. Not accepted: lock, %gs (fakeavx's), address-size
 * override, more than one of 66/F2/F3, MXCSR and state instructions (0F AE),
 * EMMS, and everything from SSE4 on (SSE4.1 is handled by the translator).
 */
#include "fakeavx.h"

enum { P_NONE = 1, P_66 = 2, P_F3 = 4, P_F2 = 8 };
enum { F_I8 = 0x10, F_MEM = 0x20, F_REG = 0x40 };	/* imm8, memory only, register only */

/* map 0F: prefixes the opcode is valid with, plus F_ flags; 0 = not accepted */
static int op0f(u8 b, int pfx)
{
	const int SSE = P_NONE | P_66;			/* ps / pd forms */
	const int ALL = P_NONE | P_66 | P_F3 | P_F2;	/* ps / pd / ss / sd */

	switch (b) {
	case 0x10: case 0x11: return ALL;		/* movups/upd/ss/sd */
	case 0x18: return P_NONE | F_MEM;		/* prefetchnta/t0/t1/t2 (ext checked) */
	case 0x12: return pfx == P_66 ? P_66 | F_MEM : ALL;	/* movlps/hlps, movlpd, movsldup, movddup */
	case 0x13: case 0x17: return SSE | F_MEM;	/* movlps/pd, movhps/pd store */
	case 0x14: case 0x15: return SSE;		/* unpcklps/pd, unpckhps/pd */
	case 0x16: return pfx == P_66 ? P_66 | F_MEM : P_NONE | P_F3;	/* movhps/lhps, movhpd, movshdup */
	case 0x28: case 0x29: return SSE;		/* movaps/pd */
	case 0x2a: return ALL;				/* cvtpi2ps/pd, cvtsi2ss/sd */
	case 0x2b: return SSE | F_MEM;			/* movntps/pd */
	case 0x2c: case 0x2d: return ALL;		/* cvt(t)ps2pi/pd2pi/ss2si/sd2si */
	case 0x2e: case 0x2f: return SSE;		/* ucomis, comis */
	case 0x50: return SSE | F_REG;			/* movmskps/pd */
	case 0x51: return ALL;				/* sqrt */
	case 0x52: case 0x53: return P_NONE | P_F3;	/* rsqrt, rcp */
	case 0x54: case 0x55: case 0x56: case 0x57: return SSE;	/* and, andn, or, xor */
	case 0x58: case 0x59: return ALL;		/* add, mul */
	case 0x5a: return ALL;				/* cvtps2pd, cvtpd2ps, cvtss2sd, cvtsd2ss */
	case 0x5b: return P_NONE | P_66 | P_F3;		/* cvtdq2ps, cvtps2dq, cvttps2dq */
	case 0x5c: case 0x5d: case 0x5e: case 0x5f: return ALL;	/* sub, min, div, max */
	case 0x60: case 0x61: case 0x62: case 0x63:
	case 0x64: case 0x65: case 0x66: case 0x67:
	case 0x68: case 0x69: case 0x6a: case 0x6b:
		return SSE;				/* punpck*, pack*, pcmpgt* (MMX and SSE2) */
	case 0x6c: case 0x6d: return P_66;		/* punpckl/hqdq */
	case 0x6e: return SSE;				/* movd/movq to mm/xmm */
	case 0x6f: return P_NONE | P_66 | P_F3;		/* movq mm, movdqa, movdqu */
	case 0x70: return ALL | F_I8;			/* pshufw, pshufd, pshufhw, pshuflw */
	case 0x71: case 0x72: case 0x73: return SSE | F_I8 | F_REG;	/* shifts by imm (ext checked) */
	case 0x74: case 0x75: case 0x76: return SSE;	/* pcmpeq* */
	case 0x7c: case 0x7d: return P_66 | P_F2;	/* haddpd/ps, hsubpd/ps */
	case 0x7e: return P_NONE | P_66 | P_F3;		/* movd/movq from mm/xmm, movq xmm */
	case 0x7f: return P_NONE | P_66 | P_F3;		/* movq mm, movdqa, movdqu store */
	case 0xc2: return ALL | F_I8;			/* cmpps/pd/ss/sd */
	case 0xc4: return SSE | F_I8;			/* pinsrw */
	case 0xc5: return SSE | F_I8 | F_REG;		/* pextrw */
	case 0xc6: return SSE | F_I8;			/* shufps/pd */
	case 0xd0: return P_66 | P_F2;			/* addsubpd/ps */
	case 0xd6: return pfx == P_66 ? P_66 : P_F3 | P_F2 | F_REG;	/* movq store, movq2dq, movdq2q */
	case 0xd7: return SSE | F_REG;			/* pmovmskb */
	case 0xe6: return P_66 | P_F3 | P_F2;		/* cvttpd2dq, cvtdq2pd, cvtpd2dq */
	case 0xe7: return SSE | F_MEM;			/* movntq, movntdq */
	case 0xf0: return P_F2 | F_MEM;			/* lddqu */
	case 0xf7: return SSE | F_REG;			/* maskmovq, maskmovdqu */
	case 0xff: return 0;				/* ud0 */
	}
	if (b >= 0xd1 && b <= 0xfe)
		return SSE;				/* the rest of the MMX/SSE2 integer ops */
	return 0;
}

/* map 0F 38: SSSE3 */
static int op38(u8 b)
{
	if (b <= 0x0b || (b >= 0x1c && b <= 0x1e))
		return P_NONE | P_66;
	return 0;
}

int fa_decode_sse(const u8 *code, u32 avail, u64 ip, struct fa_int *o, u16 *xmm)
{
	u32 n = 0;
	int p66 = 0, pf2 = 0, pf3 = 0, seg = 0, rex = 0, pfx, f, mod, reg, rm;

	if (avail > 15)
		avail = 15;
	for (; n < avail; n++) {
		u8 b = code[n];

		if (b == 0x66)
			p66 = 1;
		else if (b == 0xf2)
			pf2 = 1;
		else if (b == 0xf3)
			pf3 = 1;
		else if (b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36 || b == 0x64)
			seg = b;
		else if (b == 0x65 || b == 0x67 || b == 0xf0)
			return -1;	/* %gs is ours; no addr32, no lock */
		else
			break;
	}
	if (p66 + pf2 + pf3 > 1)
		return -1;
	pfx = pf2 ? P_F2 : pf3 ? P_F3 : p66 ? P_66 : P_NONE;
	if (n < avail && (code[n] & 0xf0) == 0x40)
		rex = code[n++];
	if (n + 2 >= avail || code[n] != 0x0f)
		return -1;
	n++;
	if (code[n] == 0x38) {
		f = op38(code[n + 1]);
		n += 2;
	} else if (code[n] == 0x3a) {
		f = code[n + 1] == 0x0f ? P_NONE | P_66 | F_I8 : 0;	/* palignr */
		n += 2;
	} else {
		f = op0f(code[n], pfx);
		n++;
	}
	if (!(f & pfx) || n >= avail)
		return -1;

	/* ModRM */
	mod = code[n] >> 6;
	reg = ((code[n] >> 3) & 7) | ((rex & 4) << 1);
	rm = (code[n] & 7) | ((rex & 1) << 3);
	if ((f & F_MEM) && mod == 3)
		return -1;
	if ((f & F_REG) && mod != 3)
		return -1;
	if (code[n - 1] >= 0x71 && code[n - 1] <= 0x73 && code[n - 2] == 0x0f) {
		int ext = reg & 7, ok = ext == 2 || ext == 6 || (code[n - 1] != 0x73 && ext == 4) ||
			  (code[n - 1] == 0x73 && pfx == P_66 && (ext == 3 || ext == 7));

		if (!ok)
			return -1;
		reg = 16;	/* opcode extension, not a register */
	}
	if (code[n - 1] == 0x18 && code[n - 2] == 0x0f) {
		if ((reg & 7) > 3)
			return -1;	/* hint nops, not prefetches */
		reg = 16;
	}
	o->disp_off = 0;
	o->target = 0;
	n++;
	if (mod != 3) {
		if ((rm & 7) == 4) {		/* SIB */
			if (n >= avail)
				return -1;
			if (mod == 0 && (code[n] & 7) == 5)
				n += 4;
			n++;
		} else if (mod == 0 && (rm & 7) == 5) {	/* rip-relative */
			s32 d;

			if (n + 4 > avail)
				return -1;
			memcpy(&d, code + n, 4);
			o->disp_off = n;
			n += 4;
			o->target = d;	/* completed below, once the length is known */
		}
		if (mod == 1)
			n += 1;
		else if (mod == 2)
			n += 4;
	}
	if (f & F_I8)
		n++;
	if (n > avail)
		return -1;
	if (seg && seg != 0x64 && mod == 3)
		seg = 0;	/* harmless */
	o->kind = FA_I_PLAIN;
	o->len = n;
	o->cc = 0;
	if (o->disp_off)
		o->target = ip + n + (s64)(s32)o->target;
	*xmm = (reg < 16 ? 1 << reg : 0) | (mod == 3 ? 1 << rm : 0);
	return 0;
}
