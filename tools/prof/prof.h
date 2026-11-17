/* fakeavx-prof: layout shared by prof.S and fakeavx-prof.c */
#ifndef PROF_H
#define PROF_H

#define PROF_UNROLL	8	/* copies of the test unit per loop iteration */
#define NOCHK		1	/* result differs between CPUs by design */

/* struct prof_state (64-byte aligned) */
#define ST_YMM		0	/* 16 x 32 bytes in                  */
#define ST_GP		512	/* rax rcx rdx r8 r9 r10 r11 in      */
#define ST_FLAGS	568	/* rflags in                         */
#define ST_MEM		640	/* 512 bytes, %rsi points here       */
#define ST_OYMM		1152	/* 16 x 32 bytes out                 */
#define ST_OGP		1664	/* 7 gp registers out                */
#define ST_OFLAGS	1720	/* rflags out                        */
#define ST_SIZE		1792
#define ST_MEMSZ	512
#define NGP		7

#ifndef __ASSEMBLER__
#include <stdint.h>

struct prof_state {
	uint8_t ymm[16][32];
	uint64_t gp[NGP];
	uint64_t flags;
	uint8_t pad0[ST_MEM - ST_FLAGS - 8];
	uint8_t mem[ST_MEMSZ];
	uint8_t oymm[16][32];
	uint64_t ogp[NGP];
	uint64_t oflags;
	uint8_t pad1[ST_SIZE - ST_OFLAGS - 8];
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct prof_state) == ST_SIZE, "layout");
_Static_assert(__builtin_offsetof(struct prof_state, mem) == ST_MEM, "layout");
_Static_assert(__builtin_offsetof(struct prof_state, oymm) == ST_OYMM, "layout");
_Static_assert(__builtin_offsetof(struct prof_state, oflags) == ST_OFLAGS, "layout");

/* one per test, emitted by prof.S in prof_tests.h order */
struct prof_ent {
	uint64_t bench;		/* void bench(uint64_t iters, struct prof_state *) */
	uint64_t check;		/* void check(struct prof_state *)                 */
	uint64_t unit, unit_end;	/* the test unit's bytes inside check()      */
	uint64_t loop, loop_end;	/* the measured loop inside bench()          */
};
#endif
#endif
