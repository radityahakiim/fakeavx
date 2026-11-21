// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: finding the cpuid instructions of an executable segment.
 *
 * The answer is defined by a linear sweep: decode from the segment's start,
 * one instruction after the other (what objdump does), and keep the cpuid
 * instructions met. Walking all of a 100 MB executable that way takes a
 * while, yet the two bytes of cpuid (0f a2) occur only a few hundred times
 * in it. So only those places are looked at: for each, the sweep is redone
 * from FA_SCAN_BACK bytes before it (or from where the previous one ended,
 * if that is closer). x86 code resynchronizes within a few instructions, so
 * the short sweep lands where the full one would; tests/scan_check.c
 * compares both on whole binaries (3,781 files of a Linux system, Claude
 * Code, Bun, Node: identical lists, even with a 64-byte window).
 */
#include "fakeavx.h"

/*
 * Length of the instruction at p for the sweep: as fa_insn_len(), except
 * that what needs more than 15 bytes (e.g. a run of 15 prefixes, data in
 * the code) is one invalid byte, as objdump's "(bad)", so the sweep goes on.
 * 0 only if the data ends first.
 */
int fa_sweep_len(const u8 *p, u32 avail)
{
	int l = fa_insn_len(p, avail);

	return !l && avail >= 15 ? 1 : l;
}

/* Is there a byte 0xa2 in the 8 bytes of x? */
static inline u64 has_a2(u64 x)
{
	x ^= 0xa2a2a2a2a2a2a2a2ULL;
	return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
}

static int check(const struct fa_scan *s, u64 p)
{
	u64 j = p > s->base + FA_SCAN_BACK ? p - FA_SCAN_BACK : s->base;
	u64 bend = s->base + s->n;

	if (j < s->seg_start)
		j = s->seg_start;
	if (s->done > j)
		j = s->done;		/* where the last sweep stopped: a boundary */
	while (j < p) {
		u64 left = bend - j;
		int l = fa_sweep_len(s->buf + (j - s->base), left < 15 ? (u32)left : 15);

		if (!l)
			break;
		j += l;
	}
	return j == p ? 0 : (int)(j - p);	/* 0: p is an instruction start */
}

int fa_scan_cpuid(struct fa_scan *s, u64 from, u64 to)
{
	u64 bend = s->base + s->n, q, x;
	int ret;

	/* the second byte (0xa2, rarer than 0x0f) at q, so cpuid at q - 1 */
	for (q = from + 1; q < to + 1 && q < bend; ) {
		if (q + 8 <= to + 1 && q + 8 <= bend) {
			memcpy(&x, s->buf + (q - s->base), 8);
			if (!has_a2(x)) {
				q += 8;
				continue;
			}
		}
		if (s->buf[q - s->base] == 0xa2 && s->buf[q - 1 - s->base] == 0x0f &&
		    q - 1 >= s->done) {
			int d = check(s, q - 1);

			if (!d) {
				s->done = q + 1;
				ret = s->hit(s->ctx, q - 1);
				if (ret)
					return ret;
			} else {
				s->done = q - 1 + d;
			}
		}
		q++;
	}
	return 0;
}
