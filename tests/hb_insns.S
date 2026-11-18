/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Instructions helperbench translates (never executed here: only their bytes
 * are read). Conventions as in tools/prof/prof_tests.h: %rsi points to a
 * 512-byte buffer, %ymm6 holds small gather indices, %ymm5 is a mask.
 * Table entry: name, start, end.
 */
	.macro I name, insn:vararg
	.section .rodata.hbstr, "a"
.Ls_\name: .asciz "\name"
	.section .text.hbcode, "ax"	/* executable: the m* modes run it under the module */
.Lb_\name: \insn
.Le_\name:
	ret
	.section .data.rel.ro.hbtab, "aw"
	.quad .Ls_\name, .Lb_\name, .Le_\name
	.endm

	/* gather with an all-ones mask: ";" cannot be used inside a macro argument */
	.macro G name, insn:vararg
	.section .rodata.hbstr, "a"
.Ls_\name: .asciz "\name"
	.section .text.hbcode, "ax"	/* executable: the m* modes run it under the module */
.Lb_\name: vpcmpeqd %ymm5, %ymm5, %ymm5
	\insn
.Le_\name:
	ret
	.section .data.rel.ro.hbtab, "aw"
	.quad .Ls_\name, .Lb_\name, .Le_\name
	.endm

	.section .data.rel.ro.hbtab, "aw"
	.p2align 3
	.globl hb_table
hb_table:
	/* the generic C helper */
	I pcmpgtq,		pcmpgtq %xmm2, %xmm1
	I pcmpistri,		pcmpistri $0x0c, %xmm2, %xmm1
	I pcmpistrm,		pcmpistrm $0x40, %xmm2, %xmm1
	I pcmpestri,		pcmpestri $0x0c, %xmm2, %xmm1
	I pcmpestrm_mem,	pcmpestrm $0x44, 16(%rsi), %xmm1
	I vcmpps_eq_uq_y,	vcmpps $8, %ymm1, %ymm2, %ymm0
	I vcmppd_neq_oq_x,	vcmppd $12, %xmm1, %xmm2, %xmm0
	I vcmpss_eq_uq,		vcmpss $8, %xmm1, %xmm2, %xmm0
	I vpermilps_var_y,	vpermilps %ymm2, %ymm1, %ymm0
	I vpermilpd_var_x,	vpermilpd %xmm2, %xmm1, %xmm0
	I vpermd_y,		vpermd %ymm1, %ymm6, %ymm0
	I vpermps_y,		vpermps %ymm1, %ymm6, %ymm0
	I vpermd_y_rand,	vpermd %ymm1, %ymm2, %ymm0
	I vpmaskmovd_ld_y,	vpmaskmovd (%rsi), %ymm5, %ymm0
	I vpmaskmovq_ld_x,	vpmaskmovq 8(%rsi), %xmm5, %xmm0
	I vmaskmovps_ld_y,	vmaskmovps 4(%rsi), %ymm5, %ymm0
	I vpmaskmovd_st_y,	vpmaskmovd %ymm1, %ymm5, 64(%rsi)
	I vmaskmovpd_st_y,	vmaskmovpd %ymm1, %ymm5, 72(%rsi)
	G vpgatherdd_y,		vpgatherdd %ymm5, (%rsi,%ymm6,4), %ymm0
	G vpgatherqq_y,		vpgatherqq %ymm5, (%rsi,%ymm6,8), %ymm0
	G vgatherdps_y,		vgatherdps %ymm5, (%rsi,%ymm6,4), %ymm0
	G vgatherdpd_y,		vgatherdpd %ymm5, (%rsi,%xmm6,8), %ymm0
	I vpgatherdd_x_rmask,	vpgatherdd %xmm5, 4(%rsi,%xmm6,4), %xmm0
	I vpgatherdq_y_rmask,	vpgatherdq %ymm5, (%rsi,%xmm6,8), %ymm0
	I vgatherqps_y_rmask,	vgatherqps %xmm5, 8(%rsi,%ymm6,4), %xmm0
	/* own entry points */
	I crc32q,		crc32q %rcx, %rax
	I crc32l,		crc32l %ecx, %eax
	I crc32b,		crc32b %cl, %eax
	I crc32w_mem,		crc32w 6(%rsi), %r8d
	I popcnt64,		popcnt %rcx, %rax
	I popcnt32_r9,		popcnt %r10d, %r9d
	I popcnt16,		popcnt %cx, %ax
	/* SSE4.1: native on Penryn, C helper on SSSE3-only hosts (HB_NOSSE41=1) */
	I s41_pmovzxbw,		pmovzxbw %xmm2, %xmm1
	I s41_pmovsxbq_mem,	pmovsxbq 3(%rsi), %xmm1
	I s41_pmovsxwd,		pmovsxwd %xmm2, %xmm1
	I s41_pmovzxdq_mem,	pmovzxdq 8(%rsi), %xmm1
	I s41_pmulld,		pmulld %xmm2, %xmm1
	I s41_pminsd,		pminsd %xmm2, %xmm1
	I s41_pmaxuw,		pmaxuw %xmm2, %xmm1
	I s41_ptest,		ptest %xmm2, %xmm1
	I s41_pblendvb,		pblendvb %xmm0, %xmm2, %xmm1
	I s41_blendvpd,		blendvpd %xmm0, %xmm2, %xmm1
	I s41_blendps,		blendps $5, %xmm2, %xmm1
	I s41_pblendw,		pblendw $0x5a, %xmm2, %xmm1
	I s41_roundps,		roundps $1, %xmm2, %xmm1
	I s41_roundsd,		roundsd $10, %xmm2, %xmm1
	I s41_dpps,		dpps $0xf1, %xmm2, %xmm1
	I s41_dppd,		dppd $0x31, %xmm2, %xmm1
	I s41_insertps,		insertps $0x1d, %xmm2, %xmm1
	I s41_extractps,	extractps $2, %xmm2, %eax
	I s41_pextrb,		pextrb $3, %xmm2, %eax
	I s41_pextrq,		pextrq $1, %xmm2, %rax
	I s41_pinsrd,		pinsrd $1, %ecx, %xmm1
	I s41_pinsrb_mem,	pinsrb $9, 5(%rsi), %xmm1
	I s41_packusdw,		packusdw %xmm2, %xmm1
	I s41_pcmpeqq,		pcmpeqq %xmm2, %xmm1
	I s41_phminposuw,	phminposuw %xmm2, %xmm1
	I s41_mpsadbw,		mpsadbw $5, %xmm2, %xmm1
	I s41_movntdqa,		movntdqa 16(%rsi), %xmm1
	I s41_v_pmulld_y,	vpmulld %ymm1, %ymm2, %ymm0
	I s41_v_pmovsxwd_y,	vpmovsxwd %xmm1, %ymm0
	I s41_v_pminud_y,	vpminud %ymm1, %ymm2, %ymm0
	I s41_v_blendvps_y,	vblendvps %ymm3, %ymm1, %ymm2, %ymm0
	I s41_v_roundpd_y,	vroundpd $9, %ymm1, %ymm0
	I s41_v_dpps_y,		vdpps $0xff, %ymm1, %ymm2, %ymm0
	I s41_v_ptest_x,	vptest %xmm1, %xmm2
	I s41_v_mpsadbw_y,	vmpsadbw $5, %ymm1, %ymm2, %ymm0
	I s41_v_packusdw_y,	vpackusdw %ymm1, %ymm2, %ymm0
	I s41_v_pextrd,		vpextrd $2, %xmm1, %eax
	I s41_v_insertps,	vinsertps $0x1d, %xmm1, %xmm2, %xmm0
	/* inline references */
	I ref_vpaddd_y,		vpaddd %ymm1, %ymm2, %ymm0
	I ref_vpcmpgtq_y,	vpcmpgtq %ymm1, %ymm2, %ymm0
	.section .data.rel.ro.hbtab, "aw"
	.globl hb_table_end
hb_table_end:

/*
 * Loops for region mode (helperbench rdump / rtime): translated as one
 * region from the first instruction, as the module does; %r12 counts.
 */
	/* one loop: up to 8 instructions, then dec %r12; jnz back; ret */
	.macro R name, a, b, c, d, e, f, g, h
	.section .rodata.hbstr, "a"
.Lrs_\name: .asciz "\name"
	.section .text.hbcode, "ax"	/* executable: the m* modes run it under the module */
	.p2align 4
.Lrb_\name:
	\a
	\b
	\c
	\d
	\e
	\f
	\g
	\h
	dec	%r12
	jnz	.Lrb_\name
	ret
.Lre_\name:
	.section .data.rel.ro.hbloops, "aw"
	.quad .Lrs_\name, .Lrb_\name, .Lre_\name
	.endm

	.section .data.rel.ro.hbloops, "aw"
	.p2align 3
	.globl hb_loops
hb_loops:
	R sse42_mix, "pcmpgtq %xmm2, %xmm1", "vpaddd %ymm4, %ymm3, %ymm3", "pcmpistri $0x0c, %xmm2, %xmm1", "add %rcx, %r8", "crc32q %r9, %rax", "vpxor %ymm3, %ymm5, %ymm5", "pcmpistrm $0x40, %xmm7, %xmm6", "vpaddd %ymm0, %ymm5, %ymm5"
	R pcmpestri_loop, "pcmpestri $0x0c, %xmm2, %xmm1", "vpaddd %ymm4, %ymm3, %ymm3", "add %rcx, %r9", "vpxor %ymm3, %ymm8, %ymm8"
	R crc32_loop, "crc32q (%rsi), %rax", "crc32b %cl, %eax", "vpaddd %ymm4, %ymm3, %ymm3", "crc32l %r8d, %r9d", "vpxor %ymm3, %ymm5, %ymm5"
	R crc32_loop_b, "vpaddd %ymm4, %ymm3, %ymm3", "crc32q (%rsi), %rax", "crc32b %cl, %eax", "crc32l %r8d, %r9d", "vpxor %ymm3, %ymm5, %ymm5"
	R crc32_loop_c, "crc32q (%rsi), %rax", "vpaddd %ymm4, %ymm3, %ymm3", "vpxor %ymm3, %ymm5, %ymm5", "crc32b %cl, %eax", "crc32l %r8d, %r9d"
	R crc32_loop_d, "crc32l %r8d, %r9d", "vpaddd %ymm4, %ymm3, %ymm3", "crc32q (%rsi), %rax", "vpxor %ymm3, %ymm5, %ymm5", "crc32b %cl, %eax", "nop"
	R crc32_only, "crc32q (%rsi), %rax", "crc32b %cl, %eax", "crc32l %r8d, %r9d"
	R pcmpgtq_loop, "pcmpgtq %xmm2, %xmm1", "vpaddd %ymm1, %ymm3, %ymm3", "pcmpgtq %xmm9, %xmm8", "vpxor %ymm8, %ymm3, %ymm3"
	R vex_helpers, "vpaddd %ymm1, %ymm0, %ymm0", "vpermd %ymm0, %ymm6, %ymm2", "vpmaskmovd (%rsi), %ymm3, %ymm4", "vpxor %ymm4, %ymm2, %ymm2", "vpcmpeqd %ymm5, %ymm5, %ymm5", "vpgatherdd %ymm5, (%rsi,%ymm6,4), %ymm7", "vpaddd %ymm7, %ymm2, %ymm1", "vpmaskmovd %ymm2, %ymm3, 64(%rsi)"
	R avx_only, "vpaddd %ymm1, %ymm0, %ymm0", "vpxor %ymm0, %ymm2, %ymm2", "vpshufb %ymm3, %ymm2, %ymm4", "vpaddq %ymm4, %ymm1, %ymm1"
	.section .data.rel.ro.hbloops, "aw"
	.globl hb_loops_end
hb_loops_end:
	.section .note.GNU-stack, "", @progbits
