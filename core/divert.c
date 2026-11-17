// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: find where to divert into the code cache ahead of an instruction
 * that cannot be patched itself.
 *
 * A 3- or 4-byte instruction gets a jump only if a stub can be placed in the
 * window that the following bytes fix (see fa_window_get() in kmod/main.c).
 * When that fails, the instruction traps on every execution. If the
 * instruction is reached by falling through from a long enough instruction
 * just before it, that one can carry the jump instead: a region starting
 * there runs both. Only that one instruction is overwritten, so a jump into
 * any instruction after it still finds the original bytes, and merely traps.
 *
 * The catch is finding that instruction: x86 cannot be decoded backwards. A
 * wrong guess would overwrite the middle of an instruction and break the
 * program, so the guess has to be checked from several sides:
 *
 *  1. Decoding starts at a function start, which compilers mark by padding
 *     the gap before it with int3 (0xcc) up to a 16-byte boundary, and must
 *     arrive exactly at ip. (Unwind tables would say where functions start,
 *     but Zig and some others emit none for most of their code.)
 *  2. Decoding from the two function starts before that one must arrive at
 *     the same candidate. A false function start (0xcc inside an operand)
 *     or data among the code would have to fool all three. Checked with
 *     objdump over Claude Code, Bun and Node (tests/divert_check.c): with
 *     step 1 alone, 2 of 11,862 answers were wrong; with both, none.
 *  3. The candidate and everything between it and ip are instructions the
 *     region builder accepts, without a call or unconditional jump between.
 *
 * Works on the bytes of the file, not of memory, where earlier patches have
 * changed instruction lengths.
 */
#include "fakeavx.h"

#define FA_DV_SYNC_MAX	(64 << 10)	/* how far back to look for a function start */
#define FA_DV_STARTS	3		/* function starts that must agree */

static u32 left(u64 base, u32 n, u64 at)
{
	return base + n - at < 15 ? base + n - at : 15;
}

/* Decode from s up to stop; returns where it ends up (0 on failure). */
static u64 sweep(const u8 *buf, u64 base, u32 n, u64 s, u64 stop, u64 *bound, int *nb, u64 near)
{
	while (s < stop) {
		int l = fa_insn_len(buf + (s - base), left(base, n, s));

		if (!l)
			return 0;
		if (bound && stop - s <= near) {
			if (*nb >= FA_DV_BACK)
				return 0;
			bound[(*nb)++] = s;
		}
		s += l;
	}
	return s;
}

/* Is the instruction at p (len l) one the region builder carries along? */
static int carried(const u8 *p, u32 avail, u64 at, int l)
{
	struct fa_int ii;
	struct fa_insn in;
	u16 xmm;

	if (!fa_decode(p, avail, at, &in) && in.kind != FA_K_XGETBV)
		return in.len == l;
	if (!fa_decode_int(p, avail, at, &ii) || !fa_decode_sse(p, avail, at, &ii, &xmm)) {
		return ii.len == l && ii.kind != FA_I_JMP;
	}
	return 0;
}

/*
 * buf holds the original bytes of [base, base + n), which must include the
 * instruction at ip. On success *x is the instruction to divert at.
 */
int fa_find_divert(const u8 *buf, u64 base, u32 n, u64 ip, u64 *x)
{
	u64 s, starts[FA_DV_STARTS], bound[FA_DV_BACK], cand = 0;
	int nb = 0, i, k;

	if (ip < base + 32 || ip >= base + n)
		return FA_DV_NOSYNC;

	/* 1. the nearest function starts before ip; decode from the first to ip */
	s = (ip - 1) & ~15ULL;
	for (k = 0; k < FA_DV_STARTS; k++) {
		for (; s > base + 16 && ip - s < FA_DV_SYNC_MAX; s -= 16)
			if (buf[s - 1 - base] == 0xcc && buf[s - base] != 0xcc)
				break;
		if (s <= base + 16 || ip - s >= FA_DV_SYNC_MAX)
			break;
		starts[k] = s;
		s -= 16;
	}
	if (k < FA_DV_STARTS)
		return FA_DV_NOSYNC;	/* the agreement in step 2 is needed */
	if (sweep(buf, base, n, starts[0], ip, bound, &nb, FA_DV_BACK) != ip)
		return FA_DV_DESYNC;

	/* 3. walk back over instructions the region carries */
	for (i = nb - 1; i >= 0; i--) {
		u64 at = bound[i], next = i + 1 < nb ? bound[i + 1] : ip;
		int l = next - at;

		if (!carried(buf + (at - base), left(base, n, at), at, l))
			break;
		if (l >= 5) {
			cand = at;
			break;
		}
	}
	if (!cand)
		return FA_DV_NOLONG;

	/* 2. decoding from the function starts before that one agrees */
	for (i = 1; i < k; i++) {
		u64 e = sweep(buf, base, n, starts[i], cand, NULL, NULL, 0);

		if (e != cand)
			return FA_DV_AMBIG;
	}
	*x = cand;
	return 0;
}
