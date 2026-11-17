/*
 * fakeavx-prof test code. For every T() in prof_tests.h:
 *
 *   bench_NAME(iters, state): load the registers from state, then call a loop
 *       of iters iterations, each executing the test unit PROF_UNROLL times.
 *       The loop is a separate function so that fakeavx builds a region for
 *       it alone, which can keep upper halves in spare host registers, as
 *       in a real program's hot loop.
 *       This lives in the executable file on purpose: fakeavx only patches
 *       file-backed code, so this is what a real program's hot loop sees.
 *   check_NAME(state): load the registers (and rflags) from state, execute
 *       the test unit once, store registers and rflags back.
 *
 * bench_empty is the same loop without the unit, subtracted as overhead.
 */
#include "prof.h"

#define LOAD_YMM(b)						\
	vmovdqu ST_YMM+0*32(b), %ymm0;  vmovdqu ST_YMM+1*32(b), %ymm1;	\
	vmovdqu ST_YMM+2*32(b), %ymm2;  vmovdqu ST_YMM+3*32(b), %ymm3;	\
	vmovdqu ST_YMM+4*32(b), %ymm4;  vmovdqu ST_YMM+5*32(b), %ymm5;	\
	vmovdqu ST_YMM+6*32(b), %ymm6;  vmovdqu ST_YMM+7*32(b), %ymm7;	\
	vmovdqu ST_YMM+8*32(b), %ymm8;  vmovdqu ST_YMM+9*32(b), %ymm9;	\
	vmovdqu ST_YMM+10*32(b), %ymm10; vmovdqu ST_YMM+11*32(b), %ymm11; \
	vmovdqu ST_YMM+12*32(b), %ymm12; vmovdqu ST_YMM+13*32(b), %ymm13; \
	vmovdqu ST_YMM+14*32(b), %ymm14; vmovdqu ST_YMM+15*32(b), %ymm15

#define LOAD_GP(b)						\
	mov ST_GP+0(b), %rax; mov ST_GP+8(b), %rcx; mov ST_GP+16(b), %rdx; \
	mov ST_GP+24(b), %r8; mov ST_GP+32(b), %r9; mov ST_GP+40(b), %r10; \
	mov ST_GP+48(b), %r11

#define STORE_ALL(b)						\
	mov %rax, ST_OGP+0(b); mov %rcx, ST_OGP+8(b); mov %rdx, ST_OGP+16(b); \
	mov %r8, ST_OGP+24(b); mov %r9, ST_OGP+32(b); mov %r10, ST_OGP+40(b); \
	mov %r11, ST_OGP+48(b);						\
	vmovdqu %ymm0, ST_OYMM+0*32(b);  vmovdqu %ymm1, ST_OYMM+1*32(b);	\
	vmovdqu %ymm2, ST_OYMM+2*32(b);  vmovdqu %ymm3, ST_OYMM+3*32(b);	\
	vmovdqu %ymm4, ST_OYMM+4*32(b);  vmovdqu %ymm5, ST_OYMM+5*32(b);	\
	vmovdqu %ymm6, ST_OYMM+6*32(b);  vmovdqu %ymm7, ST_OYMM+7*32(b);	\
	vmovdqu %ymm8, ST_OYMM+8*32(b);  vmovdqu %ymm9, ST_OYMM+9*32(b);	\
	vmovdqu %ymm10, ST_OYMM+10*32(b); vmovdqu %ymm11, ST_OYMM+11*32(b); \
	vmovdqu %ymm12, ST_OYMM+12*32(b); vmovdqu %ymm13, ST_OYMM+13*32(b); \
	vmovdqu %ymm14, ST_OYMM+14*32(b); vmovdqu %ymm15, ST_OYMM+15*32(b)

#define T(name, flags, ...)						\
	.text;								\
	.p2align 6;							\
bench_##name:								\
	LOAD_YMM(%rsi); LOAD_GP(%rsi);					\
	lea ST_MEM(%rsi), %rsi;						\
	call .Lloop_##name;						\
	ret;								\
	/* a function of its own, so that it is a region of its own */	\
	.p2align 6;							\
.Lloop_##name:								\
	.rept PROF_UNROLL; __VA_ARGS__; .endr;				\
	dec %rdi;							\
	jnz .Lloop_##name;						\
.Lloop_end_##name:							\
	ret;								\
	.p2align 4;							\
check_##name:								\
	LOAD_YMM(%rdi); LOAD_GP(%rdi);					\
	lea ST_MEM(%rdi), %rsi;						\
	pushq ST_FLAGS(%rdi); popfq;					\
.Lunit_##name:								\
	__VA_ARGS__;							\
.Lunit_end_##name:							\
	pushfq; popq ST_OFLAGS(%rdi);					\
	STORE_ALL(%rdi);						\
	vzeroupper;							\
	ret;								\
	.section .data.rel.ro.prof, "aw";				\
	.quad bench_##name, check_##name;				\
	.quad .Lunit_##name, .Lunit_end_##name;				\
	.quad .Lloop_##name, .Lloop_end_##name;				\
	.text

	.section .data.rel.ro.prof, "aw"
	.p2align 3
	.globl prof_table
prof_table:
	.text

T(empty, 0, )
#include "prof_tests.h"

	.section .data.rel.ro.prof, "aw"
	.globl prof_table_end
prof_table_end:

	.section .note.GNU-stack, "", @progbits
