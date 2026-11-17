/*
 * Provokes a refused short patch: the stub window of a 4-byte AVX
 * instruction followed by "paddd" (first byte 0x66) lies ~1.6 GB above it.
 * This program reserves that area first, so fakeavx cannot place the stub;
 * the instruction must still run correctly (trapping every time), and
 * /proc/fakeavx/unpatched must name it, with the reason.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>

__attribute__((noinline)) static void loop(int n, int *out)
{
	int a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[4] = { 10, 20, 30, 40 };

	__asm__ volatile(
		"vmovdqu (%1), %%ymm1\n\t"
		"vmovdqu (%1), %%ymm2\n\t"
		"movdqu (%2), %%xmm4\n\t"
		"movdqu (%2), %%xmm7\n\t"
		"1:\n\t"
		"vpaddd %%ymm1, %%ymm2, %%ymm0\n\t"	/* c5 ed fe c1: 4 bytes */
		"paddd %%xmm7, %%xmm4\n\t"		/* 66 0f fe e7 */
		"dec %0\n\t"
		"jnz 1b\n\t"
		"vmovdqu %%ymm0, (%3)\n\t"
		"movdqu %%xmm4, 32(%3)\n\t"
		"vzeroupper"
		: "+r"(n) : "r"(a), "r"(b), "r"(out) : "xmm0", "xmm1", "xmm2", "xmm4", "xmm7", "memory");
}

int main(void)
{
	uintptr_t text = (uintptr_t)loop & ~(uintptr_t)0xfffff;
	int out[12], n = 0, ok = 1;

	/* reserve [text + 1 GB, text + 2.25 GB) in 16 MB pieces, around anything already there */
	for (uintptr_t a = text + (1ul << 30); a < text + (9ul << 28); a += 16ul << 20)
		n += mmap((void *)a, 16ul << 20, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
			  -1, 0) != MAP_FAILED;
	printf("reserved %d x 16 MB above the program\n", n);
	loop(1000, out);
	for (int i = 0; i < 8; i++)
		ok &= out[i] == 2 * (i + 1);
	for (int i = 0; i < 4; i++)
		ok &= out[8 + i] == 10 * (i + 1) * 1001;
	printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
	return !ok;
}
