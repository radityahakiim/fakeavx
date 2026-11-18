/*
 * Functions for tests/evict_test.c, linked non-PIE: the text sits at a few
 * MB, so a 3/4-byte instruction followed by a VEX prefix has a short-patch
 * window below address 0 and needs eviction (kmod/main.c, fa_ev_window()).
 * Instruction lengths matter here; each is noted.
 *
 * u32 ev_a(const u8 *p, const u8 *q): movemask(p[0..31] == q[0..31])
 *   vmovdqu (4) -> vpcmpeqb (4, gets evicted) -> vpmovmskb (4, evicts
 *   vzeroupper) -> vzeroupper (3) -> ret. vpcmpeqb is also a short site whose
 *   neighbor (vpmovmskb) carries an eviction of its own: it must not be
 *   evicted from there (forked children would lose vzeroupper's record).
 * u32 ev_b(const u8 *p, const u8 *q): movemask((u8)(p[i] + q[i]), i < 16)
 *   vzeroupper (3, evicts 2 bytes of vmovdqu) -> vmovdqu (4) -> vpaddb (4)
 *   -> vpmovmskb (4) -> ret.
 * ev_ga / ev_gb: same results, but jump straight into the evicted
 *   instruction (ev_a_n / ev_b_n), the way code from elsewhere would.
 */
	.text

	.globl ev_a
	.p2align 4
ev_a:
	vmovdqu (%rsi), %ymm1			/* c5 fe 6f 0e */
ev_a_n:
	vpcmpeqb (%rdi), %ymm1, %ymm2		/* c5 f5 74 17 */
	vpmovmskb %ymm2, %eax			/* c5 fd d7 c2 */
	vzeroupper				/* c5 f8 77 */
	ret

	.globl ev_ga
	.p2align 4
ev_ga:
	.byte 0xc5, 0xfe, 0x6f, 0x4e, 0x00	/* vmovdqu 0(%rsi),%ymm1: 5 bytes, patched directly */
	jmp ev_a_n

	.globl ev_b
	.p2align 4
ev_b:
	vzeroupper				/* c5 f8 77 */
ev_b_n:
	vmovdqu (%rsi), %xmm0			/* c5 fa 6f 06 */
	vpaddb (%rdi), %xmm0, %xmm0		/* c5 f9 fc 07 */
	vpmovmskb %xmm0, %eax			/* c5 f9 d7 c0 */
	ret

	.globl ev_gb
	.p2align 4
ev_gb:
	.byte 0xc5, 0xfe, 0x6f, 0x7e, 0x00	/* vmovdqu 0(%rsi),%ymm7: 5 bytes, patched directly */
	jmp ev_b_n

	.section .note.GNU-stack,"",@progbits
