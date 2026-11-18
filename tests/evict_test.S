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
 *
 * Integer neighbors (eviction of an instruction the region only carries,
 * undone when it is entered directly too often):
 * u32 ev_c(u32 x, u32 y) = popcount(x) + y: popcnt (4) -> mov (89: evicted)
 * u32 ev_d(u32 x, u32 y) = popcount(y) - x: popcnt (4) -> mov (89: evicted)
 * u32 ev_e(const u8 *p, const u8 *q): movemask(p[0..15] == q[0..15])
 *   vpxor (4) -> vmovdqu (5: patched directly, before vpxor is handled, so
 *   its first byte is already e9 when vpxor gets to evict it).
 * ev_gc / ev_gd: same results, jumping straight into the mov (ev_c_n,
 *   ev_d_n). The test enters ev_c_n in the parent until the eviction is
 *   undone, and ev_d_n only in a forked child, which has no records.
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

	.globl ev_c
	.p2align 4
ev_c:
	popcnt %edi, %eax			/* f3 0f b8 c7 */
ev_c_n:
	mov %esi, %edx				/* 89 f2 */
	add %edx, %eax
	ret

	.globl ev_gc
	.p2align 4
ev_gc:
	popcnt %rdi, %rax			/* f3 48 0f b8 c7: 5 bytes, patched directly */
	jmp ev_c_n

	.globl ev_d
	.p2align 4
ev_d:
	popcnt %esi, %eax			/* f3 0f b8 c6 */
ev_d_n:
	mov %edi, %edx				/* 89 fa */
	sub %edx, %eax
	ret

	.globl ev_gd
	.p2align 4
ev_gd:
	popcnt %rsi, %rax			/* f3 48 0f b8 c6: 5 bytes, patched directly */
	jmp ev_d_n

	.globl ev_e
	.p2align 4
ev_e:
	vpxor %xmm3, %xmm3, %xmm3		/* c5 e1 ef db */
	.byte 0xc5, 0xfa, 0x6f, 0x4e, 0x00	/* vmovdqu 0(%rsi),%xmm1 */
	vpcmpeqb (%rdi), %xmm1, %xmm2		/* c5 f1 74 17 */
	vpmovmskb %xmm2, %eax			/* c5 f9 d7 c2 */
	ret

	.section .note.GNU-stack,"",@progbits
