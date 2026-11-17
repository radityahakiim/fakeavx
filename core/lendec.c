// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: instruction length decoder for 64-bit code.
 *
 * Used to walk a whole executable from instruction to instruction, looking
 * for cpuid instructions (the kernel's own decoder is not available to
 * modules). It only has to get lengths right; it does not validate.
 */
#include "fakeavx.h"

/* Returns the length of the instruction at p, 1 for an invalid opcode byte,
 * 0 if fewer than the needed bytes are available. */
int fa_insn_len(const u8 *p, u32 avail)
{
	u32 n = 0;
	int p66 = 0, p67 = 0, rexw = 0, modrm = 0, imm = 0;
	u8 b, m;

	for (;; n++) {
		if (n >= avail)
			return 0;
		b = p[n];
		if (b == 0x66)
			p66 = 1;
		else if (b == 0x67)
			p67 = 1;
		else if (b != 0xf0 && b != 0xf2 && b != 0xf3 && b != 0x2e && b != 0x36 &&
			 b != 0x3e && b != 0x26 && b != 0x64 && b != 0x65)
			break;
		if (n >= 14)
			return 1;
	}
	if ((b & 0xf0) == 0x40) {
		rexw = (b >> 3) & 1;
		if (++n >= avail)
			return 0;
		b = p[n];
	}
	n++;

	if (b == 0xc5 || b == 0xc4 || b == 0x62) {
		/* VEX / EVEX: prefix bytes, opcode, modrm, maybe imm8 */
		int map, pl = b == 0xc5 ? 1 : b == 0xc4 ? 2 : 3;

		if (n + pl + 1 > avail)
			return 0;
		map = b == 0xc5 ? 1 : b == 0xc4 ? (p[n] & 0x1f) : (p[n] & 7);
		n += pl;
		b = p[n++];
		if (map == 1 && b == 0x77)
			return n;
		modrm = 1;
		if (map == 3 || (map == 1 && ((b >= 0x70 && b <= 0x73) || b == 0xc2 ||
					      (b >= 0xc4 && b <= 0xc6))))
			imm = 1;
	} else if (b == 0x0f) {
		if (n >= avail)
			return 0;
		b = p[n++];
		if (b == 0x38) {
			n++;
			modrm = 1;
		} else if (b == 0x3a) {
			n++;
			modrm = 1;
			imm = 1;
		} else if (b >= 0x80 && b <= 0x8f) {
			imm = 4;
		} else if ((b >= 0x05 && b <= 0x0b && b != 0x0a) || b == 0x0e ||
			   (b >= 0x30 && b <= 0x37) || b == 0x77 || (b >= 0xa0 && b <= 0xa2) ||
			   (b >= 0xa8 && b <= 0xaa) || (b >= 0xc8 && b <= 0xcf)) {
			;
		} else if (b == 0x04 || b == 0x0a || b == 0x0c) {
			return 1;
		} else {
			modrm = 1;
			if ((b >= 0x70 && b <= 0x73) || b == 0xa4 || b == 0xac || b == 0xba ||
			    b == 0xc2 || (b >= 0xc4 && b <= 0xc6) || b == 0x0f)
				imm = 1;
		}
	} else if (b < 0x40) {
		switch (b & 7) {
		case 4: imm = 1; break;
		case 5: imm = -1; break;
		case 6: case 7: return 1;
		default: modrm = 1;
		}
	} else if (b >= 0x50 && b <= 0x5f) {
		;
	} else if (b >= 0x70 && b <= 0x7f) {
		imm = 1;
	} else if (b >= 0xb0 && b <= 0xb7) {
		imm = 1;
	} else if (b >= 0xb8 && b <= 0xbf) {
		imm = rexw ? 8 : -1;
	} else if (b >= 0xd8 && b <= 0xdf) {
		modrm = 1;		/* x87 */
	} else {
		switch (b) {
		case 0x63: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
		case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
		case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xfe: case 0xff:
			modrm = 1;
			break;
		case 0x68: case 0xa9:
			imm = -1;
			break;
		case 0x69: case 0x81: case 0xc7:
			modrm = 1;
			imm = -1;
			break;
		case 0x6a: case 0xa8: case 0xcd: case 0xeb:
		case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
			imm = 1;
			break;
		case 0x6b: case 0x80: case 0x83: case 0xc0: case 0xc1: case 0xc6:
			modrm = 1;
			imm = 1;
			break;
		case 0xa0: case 0xa1: case 0xa2: case 0xa3:
			imm = p67 ? 4 : 8;
			break;
		case 0xc2: case 0xca:
			imm = 2;
			break;
		case 0xc8:
			imm = 3;
			break;
		case 0xe8: case 0xe9:
			imm = 4;
			break;
		case 0xf6: case 0xf7:
			modrm = 1;
			if (n >= avail)
				return 0;
			if (((p[n] >> 3) & 7) < 2)
				imm = b == 0xf6 ? 1 : -1;
			break;
		case 0x60: case 0x61: case 0x82: case 0x9a: case 0xce: case 0xd4: case 0xd5:
		case 0xd6: case 0xea:
			return 1;
		default:
			break;		/* one-byte instructions */
		}
	}

	if (modrm) {
		if (n >= avail)
			return 0;
		m = p[n++];
		if ((m >> 6) != 3) {
			int mod = m >> 6, disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;

			if ((m & 7) == 4) {
				if (n >= avail)
					return 0;
				if ((p[n++] & 7) == 5 && mod == 0)
					disp = 4;
			} else if ((m & 7) == 5 && mod == 0) {
				disp = 4;
			}
			n += disp;
		}
	}
	if (imm < 0)
		imm = (p66 && !rexw) ? 2 : 4;
	n += imm;
	if (n > avail)
		return 0;
	return n > 15 ? 1 : n;
}
