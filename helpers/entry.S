/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fakeavx helper blob entry points. Position independent, mapped read+exec
 * into every process that uses fakeavx. Runs in user mode on the guest stack
 * (the caller has already stepped over the red zone).
 */
#include "../core/fakeavx.h"

	.section .text.entry, "ax"
	.globl _start
_start:
	jmp	helper_entry		/* FA_BLOB_HELPER */
	.balign	8
	ud2				/* FA_BLOB_POPCNT: unused, popcnt is inline */
	.balign	8
	jmp	crc32_entry		/* FA_BLOB_CRC32 */
	.balign	8
	jmp	helper_lean_entry	/* FA_BLOB_HELPER_LEAN */
	.balign	8

/*
 * Constants for inline translations, at fixed offsets (fakeavx.h), read
 * RIP-relative from the stubs. Each is placed with .org at its offset in
 * fakeavx.h, which fails to assemble if what comes before has grown into it.
 */
	.org	FA_BLOB_PERMD_BCAST
permd_bcast:		/* vpermd: the low byte of each dword to its 4 bytes */
	.byte	0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12
	.org	FA_BLOB_PERMD_OFFS
permd_offs:		/* vpermd: byte offsets within a dword, + 0x70 (see do_permd) */
	.byte	0x70, 0x71, 0x72, 0x73, 0x70, 0x71, 0x72, 0x73
	.byte	0x70, 0x71, 0x72, 0x73, 0x70, 0x71, 0x72, 0x73
	.org	FA_BLOB_PERMD_SEL
permd_sel:		/* vpermd: (index & 7) * 4 */
	.fill	16, 1, 0x1c
	.org	FA_BLOB_HIBIT
hibit:			/* 0x80 in every byte */
	.fill	16, 1, 0x80
	.org	FA_BLOB_LOQ
lo_qword:		/* the low qword set: vptest's flag mask */
	.fill	8, 1, 0xff
	.fill	8, 1, 0
	.org	FA_BLOB_ZERO
zero16:			/* pshufb: byte 0 everywhere (vpbroadcastb) */
	.fill	16, 1, 0
	.org	FA_BLOB_BCASTW
bcast_w:		/* pshufb: word 0 everywhere (vpbroadcastw) */
	.byte	0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1
	.org	FA_BLOB_POPC
popc:			/* popcnt (do_popcnt): the bit-count masks, and ZF for sahf */
	.quad	0x5555555555555555, 0x3333333333333333
	.quad	0x0f0f0f0f0f0f0f0f, 0x0101010101010101
	.long	0x4000

/*
 * In:  rdi = descriptor (struct fa_insn), [rsp] = return, [rsp+8] = guest rdi,
 *      guest rsp = rsp + 16 + 128.
 * All guest registers and flags are preserved except as modified by the C
 * helper through the frame.
 *
 * helper_entry saves all xmm registers to FA_ST_LO for the C code to read and
 * write (the C code may use any of them). helper_lean_entry saves none: its C
 * code is compiled with -mgeneral-regs-only, and the translator has stored
 * the operand registers to FA_ST_LO itself and loads them back afterwards.
 */
	.macro	HELPER_ENTRY cfunc, xmm
	pushfq
	sub	$128, %rsp
	mov	%rax, 0(%rsp)
	mov	%rcx, 8(%rsp)
	mov	%rdx, 16(%rsp)
	mov	%rbx, 24(%rsp)
	lea	128+24+128(%rsp), %rax
	mov	%rax, 32(%rsp)
	mov	%rbp, 40(%rsp)
	mov	%rsi, 48(%rsp)
	mov	128+16(%rsp), %rax
	mov	%rax, 56(%rsp)
	mov	%r8, 64(%rsp)
	mov	%r9, 72(%rsp)
	mov	%r10, 80(%rsp)
	mov	%r11, 88(%rsp)
	mov	%r12, 96(%rsp)
	mov	%r13, 104(%rsp)
	mov	%r14, 112(%rsp)
	mov	%r15, 120(%rsp)
	.if \xmm
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movaps	%xmm\n, %gs:FA_ST_LO + 16 * \n
	.endr
	.endif
	mov	%rsp, %rsi
	lea	128(%rsp), %rdx
	mov	%gs:FA_ST_SELF, %rcx
	mov	%rsp, %rbp
	and	$-16, %rsp
	cld
	call	\cfunc		/* saves MXCSR itself where it computes in floating point */
	mov	%rbp, %rsp
	.if \xmm
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movaps	%gs:FA_ST_LO + 16 * \n, %xmm\n
	.endr
	.endif
	mov	56(%rsp), %rax
	mov	%rax, 128+16(%rsp)
	/*
	 * Flags, without popfq (~20 cycles on Core 2). Only the arithmetic
	 * flags (the C code, and results it wrote into the saved image) and DF
	 * (cld above) can differ from the saved image; TF, AC and the rest are
	 * as they were. OF first (the add sets all flags), then the low byte
	 * with sahf, which leaves OF alone; nothing after this touches flags.
	 */
	mov	128(%rsp), %rax
	test	$0x400, %eax
	jz	1f
	std
1:	mov	%eax, %ecx
	shr	$11, %ecx
	and	$1, %ecx		/* OF of the image */
	add	$0x7f, %cl		/* sets OF exactly when cl was 1 */
	mov	%al, %ah
	sahf				/* SF ZF AF PF CF of the image */
	mov	0(%rsp), %rax
	mov	8(%rsp), %rcx
	mov	16(%rsp), %rdx
	mov	24(%rsp), %rbx
	mov	40(%rsp), %rbp
	mov	48(%rsp), %rsi
	mov	64(%rsp), %r8
	mov	72(%rsp), %r9
	mov	80(%rsp), %r10
	mov	88(%rsp), %r11
	mov	96(%rsp), %r12
	mov	104(%rsp), %r13
	mov	112(%rsp), %r14
	mov	120(%rsp), %r15
	lea	128+8(%rsp), %rsp	/* drop the frame and the flags image; lea keeps flags */
	ret
	.endm

helper_entry:
	HELPER_ENTRY fa_helper_c, 1
helper_lean_entry:
	HELPER_ENTRY fa_helper_lean_c, 0

/*
 * CRC32C of the low ecx bytes of rdx (1, 2, 4 or 8), continuing from eax.
 * In: eax = crc, rdx = data, ecx = byte count. Out: eax = crc (upper half of
 * rax zero). Clobbers rcx and rdx (the caller restores them); everything
 * else, including EFLAGS, is preserved, as the crc32 instruction does.
 */
crc32_entry:
	pushfq
	push	%rsi
	push	%rdi
	lea	crc32c_table(%rip), %rsi
1:	movzbl	%dl, %edi
	xor	%eax, %edi
	movzbl	%dil, %edi
	shr	$8, %eax
	xor	(%rsi,%rdi,4), %eax
	shr	$8, %rdx
	dec	%ecx
	jnz	1b
	pop	%rdi
	pop	%rsi
	popfq
	ret

	.balign	64
crc32c_table:
	.long	0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c, 0x26a1e7e8, 0xd4ca64eb
	.long	0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b, 0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24
	.long	0x105ec76f, 0xe235446c, 0xf165b798, 0x030e349b, 0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384
	.long	0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54, 0x5d1d08bf, 0xaf768bbc, 0xbc267848, 0x4e4dfb4b
	.long	0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a, 0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35
	.long	0xaa64d611, 0x580f5512, 0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa
	.long	0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad, 0x1642ae59, 0xe4292d5a
	.long	0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a, 0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595
	.long	0x417b1dbc, 0xb3109ebf, 0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957
	.long	0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c, 0xfe53516f, 0xed03a29b, 0x1f682198
	.long	0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927, 0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38
	.long	0xdbfc821c, 0x2997011f, 0x3ac7f2eb, 0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7
	.long	0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e, 0x4767748a, 0xb50cf789
	.long	0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859, 0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46
	.long	0x7198540d, 0x83f3d70e, 0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6
	.long	0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de, 0xdde0eb2a, 0x2f8b6829
	.long	0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c, 0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93
	.long	0x082f63b7, 0xfa44e0b4, 0xe9141340, 0x1b7f9043, 0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c
	.long	0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3, 0x55326b08, 0xa759e80b, 0xb4091bff, 0x466298fc
	.long	0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c, 0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033
	.long	0xa24bb5a6, 0x502036a5, 0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d
	.long	0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975, 0x0e330a81, 0xfc588982
	.long	0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d, 0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622
	.long	0x38cc2a06, 0xcaa7a905, 0xd9f75af1, 0x2b9cd9f2, 0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed
	.long	0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530, 0x0417b1db, 0xf67c32d8, 0xe52cc12c, 0x1747422f
	.long	0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff, 0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0
	.long	0xd3d3e1ab, 0x21b862a8, 0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540
	.long	0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78, 0x7fab5e8c, 0x8dc0dd8f
	.long	0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee, 0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1
	.long	0x69e9f0d5, 0x9b8273d6, 0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e
	.long	0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a, 0xc69f7b69, 0xd5cf889d, 0x27a40b9e
	.long	0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e, 0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351
