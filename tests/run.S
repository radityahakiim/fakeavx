/* SPDX-License-Identifier: GPL-2.0 */
/* void fa_run(struct regs *r, void *code): load full machine state, call code, save state. */
#define R_FLAGS 128
#define R_MXCSR 136
#define R_YMM   192

	.text
	.globl fa_run
fa_run:
	push	%rbx
	push	%rbp
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	sub	$8, %rsp
	mov	%rdi, saved_regs(%rip)
	mov	%rsi, target(%rip)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	vmovdqu	R_YMM + 32 * \n(%rdi), %ymm\n
	.endr
	ldmxcsr	R_MXCSR(%rdi)
	pushq	R_FLAGS(%rdi)
	popfq
	mov	0(%rdi), %rax
	mov	8(%rdi), %rcx
	mov	16(%rdi), %rdx
	mov	24(%rdi), %rbx
	mov	40(%rdi), %rbp
	mov	48(%rdi), %rsi
	mov	64(%rdi), %r8
	mov	72(%rdi), %r9
	mov	80(%rdi), %r10
	mov	88(%rdi), %r11
	mov	96(%rdi), %r12
	mov	104(%rdi), %r13
	mov	112(%rdi), %r14
	mov	120(%rdi), %r15
	mov	56(%rdi), %rdi
	call	*target(%rip)
	mov	%rdi, tmp_rdi(%rip)
	mov	saved_regs(%rip), %rdi
	mov	%rax, 0(%rdi)
	mov	%rcx, 8(%rdi)
	mov	%rdx, 16(%rdi)
	mov	%rbx, 24(%rdi)
	mov	%rbp, 40(%rdi)
	mov	%rsi, 48(%rdi)
	mov	%r8, 64(%rdi)
	mov	%r9, 72(%rdi)
	mov	%r10, 80(%rdi)
	mov	%r11, 88(%rdi)
	mov	%r12, 96(%rdi)
	mov	%r13, 104(%rdi)
	mov	%r14, 112(%rdi)
	mov	%r15, 120(%rdi)
	mov	tmp_rdi(%rip), %rax
	mov	%rax, 56(%rdi)
	pushfq
	popq	R_FLAGS(%rdi)
	stmxcsr	R_MXCSR(%rdi)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	vmovdqu	%ymm\n, R_YMM + 32 * \n(%rdi)
	.endr
	vzeroupper
	cld
	add	$8, %rsp
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%rbp
	pop	%rbx
	ret

	.bss
saved_regs:	.quad 0
target:		.quad 0
tmp_rdi:	.quad 0
	.section .note.GNU-stack,"",@progbits
