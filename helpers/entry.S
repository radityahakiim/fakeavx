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
	jmp	popcnt_entry		/* FA_BLOB_POPCNT */
	.balign	8

/*
 * In:  rdi = descriptor (struct fa_insn), [rsp] = return, [rsp+8] = guest rdi,
 *      guest rsp = rsp + 16 + 128.
 * All guest registers and flags are preserved except as modified by the C
 * helper through the frame.
 */
helper_entry:
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
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movaps	%xmm\n, %gs:FA_ST_LO + 16 * \n
	.endr
	mov	%rsp, %rsi
	lea	128(%rsp), %rdx
	mov	%gs:FA_ST_SELF, %rcx
	mov	%rsp, %rbp
	and	$-16, %rsp
	cld
	call	fa_helper_c
	mov	%rbp, %rsp
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	movaps	%gs:FA_ST_LO + 16 * \n, %xmm\n
	.endr
	mov	56(%rsp), %rax
	mov	%rax, 128+16(%rsp)
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
	add	$128, %rsp
	popfq
	ret

/*
 * In: rax = value (zero-extended). Out: rax = population count, EFLAGS exactly
 * as POPCNT leaves them (ZF = source was zero, CF/OF/SF/AF/PF clear).
 */
popcnt_entry:
	push	%rcx
	push	%rdx
	mov	%rax, %rdx
	shr	%rdx
	movabs	$0x5555555555555555, %rcx
	and	%rcx, %rdx
	sub	%rdx, %rax
	movabs	$0x3333333333333333, %rcx
	mov	%rax, %rdx
	shr	$2, %rdx
	and	%rcx, %rax
	and	%rcx, %rdx
	add	%rdx, %rax
	mov	%rax, %rdx
	shr	$4, %rdx
	add	%rdx, %rax
	movabs	$0x0f0f0f0f0f0f0f0f, %rcx
	and	%rcx, %rax
	movabs	$0x0101010101010101, %rcx
	imul	%rcx, %rax
	shr	$56, %rax
	mov	%rax, %rdx
	xor	%eax, %eax
	test	%rdx, %rdx
	sete	%al
	shl	$14, %eax		/* AH = ZF image */
	test	%eax, %eax		/* clear OF */
	sahf
	mov	%rdx, %rax
	pop	%rdx
	pop	%rcx
	ret
