/* SPDX-License-Identifier: GPL-2.0 */
/*
 * helperbench runners. struct hb_state layout (see helperbench.c):
 *   0    xmm[16][16]   low halves (upper halves live in the %gs block)
 *   256  gp[16]        by register number; rsp (4), r13-r15 unused
 *   384  flags
 *   392  mxcsr
 * Reserved while running: r13 (counter), r14 (state), r15 (stub).
 *
 * void hb_once(struct hb_state *s, void *stub)   run once, store everything
 * void hb_loop(struct hb_state *s, void *stub, u64 n)   run n times
 */
	.text
	.macro LOAD
	ldmxcsr	392(%r14)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movdqu	16*\n(%r14), %xmm\n
	.endr
	pushq	384(%r14)
	popfq
	mov	256+0*8(%r14), %rax
	mov	256+1*8(%r14), %rcx
	mov	256+2*8(%r14), %rdx
	mov	256+3*8(%r14), %rbx
	mov	256+5*8(%r14), %rbp
	mov	256+6*8(%r14), %rsi
	mov	256+7*8(%r14), %rdi
	mov	256+8*8(%r14), %r8
	mov	256+9*8(%r14), %r9
	mov	256+10*8(%r14), %r10
	mov	256+11*8(%r14), %r11
	mov	256+12*8(%r14), %r12
	.endm

	.globl hb_once
hb_once:
	push	%rbx; push %rbp; push %r12; push %r13; push %r14; push %r15
	mov	%rdi, %r14
	mov	%rsi, %r15
	LOAD
	call	*%r15
	pushfq
	popq	384(%r14)
	cld			/* the C caller expects DF clear */
	stmxcsr	392(%r14)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movdqu	%xmm\n, 16*\n(%r14)
	.endr
	mov	%rax, 256+0*8(%r14)
	mov	%rcx, 256+1*8(%r14)
	mov	%rdx, 256+2*8(%r14)
	mov	%rbx, 256+3*8(%r14)
	mov	%rbp, 256+5*8(%r14)
	mov	%rsi, 256+6*8(%r14)
	mov	%rdi, 256+7*8(%r14)
	mov	%r8, 256+8*8(%r14)
	mov	%r9, 256+9*8(%r14)
	mov	%r10, 256+10*8(%r14)
	mov	%r11, 256+11*8(%r14)
	mov	%r12, 256+12*8(%r14)
	pop	%r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx
	ret

/*
 * void hb_mrun(struct hb_state *s, void *code): for running under the
 * fakeavx module (no %gs of our own): full ymm registers from xmm[] and
 * hi[] (offset 400), the code executed natively, everything stored back.
 */
	.globl hb_mrun
hb_mrun:
	push	%rbx; push %rbp; push %r12; push %r13; push %r14; push %r15
	mov	%rdi, %r14
	mov	%rsi, %r15
	ldmxcsr	392(%r14)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movdqu	16*\n(%r14), %xmm\n
	vinsertf128 $1, 400+16*\n(%r14), %ymm\n, %ymm\n
	.endr
	pushq	384(%r14)
	popfq
	mov	256+0*8(%r14), %rax
	mov	256+1*8(%r14), %rcx
	mov	256+2*8(%r14), %rdx
	mov	256+3*8(%r14), %rbx
	mov	256+5*8(%r14), %rbp
	mov	256+6*8(%r14), %rsi
	mov	256+7*8(%r14), %rdi
	mov	256+8*8(%r14), %r8
	mov	256+9*8(%r14), %r9
	mov	256+10*8(%r14), %r10
	mov	256+11*8(%r14), %r11
	mov	256+12*8(%r14), %r12
	call	*%r15
	pushfq
	popq	384(%r14)
	cld
	stmxcsr	392(%r14)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movdqu	%xmm\n, 16*\n(%r14)
	vextractf128 $1, %ymm\n, 400+16*\n(%r14)
	.endr
	vzeroupper
	mov	%rax, 256+0*8(%r14)
	mov	%rcx, 256+1*8(%r14)
	mov	%rdx, 256+2*8(%r14)
	mov	%rbx, 256+3*8(%r14)
	mov	%rbp, 256+5*8(%r14)
	mov	%rsi, 256+6*8(%r14)
	mov	%rdi, 256+7*8(%r14)
	mov	%r8, 256+8*8(%r14)
	mov	%r9, 256+9*8(%r14)
	mov	%r10, 256+10*8(%r14)
	mov	%r11, 256+11*8(%r14)
	mov	%r12, 256+12*8(%r14)
	pop	%r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx
	ret

	.globl hb_loop
hb_loop:
	push	%rbx; push %rbp; push %r12; push %r13; push %r14; push %r15
	mov	%rdi, %r14
	mov	%rsi, %r15
	mov	%rdx, %r13
	LOAD
	.p2align 4
1:	call	*%r15
	sub	$1, %r13
	jnz	1b
	ldmxcsr	392(%r14)	/* undo exception flags the stubs set */
	pop	%r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx
	ret
	.section .note.GNU-stack, "", @progbits
