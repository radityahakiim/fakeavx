/*
 * Provokes refused short patches: the stub window of a 4-byte AVX
 * instruction followed by "paddd" (first byte 0x66) lies ~1.6 GB above it.
 * This program reserves that area first, so fakeavx cannot place the stub.
 *
 *  - loop(): the instruction is a loop head, reached by a jump. It must
 *    still run correctly (trapping every time), and /proc/fakeavx/unpatched
 *    must name it, with the reason.
 *  - dv_func(): the instruction is reached by falling through from a 5-byte
 *    "mov", in a function laid out like LLVM's (int3 padding between
 *    functions). After a few traps fakeavx must patch the "mov" instead
 *    (fa_divert()), and the traps must stop.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>

/* Three function starts with int3 padding in front, as core/divert.c requires. */
__asm__(
	".text\n"
	".p2align 4, 0xcc\n"
	"int3\n"
	".p2align 4, 0xcc\n"
	"dv_f1:\n"
	"	xor %eax, %eax\n"
	"	ret\n"
	"int3\n"
	".p2align 4, 0xcc\n"
	"dv_f2:\n"
	"	mov $1, %eax\n"
	"	ret\n"
	"int3\n"
	".p2align 4, 0xcc\n"
	"dv_func:\n"
	"	mov $0x12345678, %eax\n"	/* b8 imm32: 5 bytes, gets the jump */
	"	vpaddd %ymm1, %ymm2, %ymm0\n"	/* c5 ed fe c1: window refused */
	"	paddd %xmm7, %xmm4\n"		/* 66 0f fe e7 */
	"	ret\n"
	"int3\n"
	".p2align 4, 0xcc\n");

static unsigned long stat(const char *name)
{
	char path[128];
	unsigned long v = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/module/fakeavx/parameters/%s", name);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

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

__attribute__((noinline)) static void calls(int n, int *out)
{
	int a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[4] = { 10, 20, 30, 40 };

	__asm__ volatile(
		"vmovdqu (%1), %%ymm1\n\t"
		"vmovdqu (%1), %%ymm2\n\t"
		"movdqu (%2), %%xmm4\n\t"
		"movdqu (%2), %%xmm7\n\t"
		"1:\n\t"
		"call dv_func\n\t"
		"dec %0\n\t"
		"jnz 1b\n\t"
		"vmovdqu %%ymm0, (%3)\n\t"
		"movdqu %%xmm4, 32(%3)\n\t"
		"vzeroupper"
		: "+r"(n) : "r"(a), "r"(b), "r"(out)
		: "rax", "xmm0", "xmm1", "xmm2", "xmm4", "xmm7", "memory");
}

static int check(const int *out, int n)
{
	int ok = 1;

	for (int i = 0; i < 8; i++)
		ok &= out[i] == 2 * (i + 1);
	for (int i = 0; i < 4; i++)
		ok &= out[8 + i] == 10 * (i + 1) * (n + 1);
	return ok;
}

int main(void)
{
	uintptr_t text = (uintptr_t)loop & ~(uintptr_t)0xfffff;
	int out[12], n = 0, ok;
	unsigned long fast, dv;
	char line[1024];
	FILE *f;

	/* reserve [text + 1 GB, text + 2.25 GB) in 16 MB pieces, around anything already there */
	for (uintptr_t a = text + (1ul << 30); a < text + (9ul << 28); a += 16ul << 20)
		n += mmap((void *)a, 16ul << 20, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
			  -1, 0) != MAP_FAILED;
	printf("reserved %d x 16 MB above the program\n", n);

	loop(1000, out);
	ok = check(out, 1000);
	printf("loop head: %s\n", ok ? "correct" : "WRONG");

	fast = stat("stat_fast");
	dv = stat("stat_diverted");
	calls(100000, out);
	ok &= check(out, 100000);
	fast = stat("stat_fast") - fast;
	dv = stat("stat_diverted") - dv;
	printf("fall-through: %s, %lu traps in 100000 calls, %lu diverted\n",
	       check(out, 100000) ? "correct" : "WRONG", fast, dv);
	ok &= dv == 1 && fast < 100;

	f = fopen("/proc/fakeavx/unpatched", "r");
	while (f && fgets(line, sizeof(line), f))
		fputs(line, stdout);
	if (f)
		fclose(f);
	printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
	return !ok;
}
