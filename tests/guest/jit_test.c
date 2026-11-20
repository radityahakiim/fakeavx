/*
 * JIT-style code, rewritten in place (with the module loaded).
 *
 * fakeavx checks a translation only against the bytes of the instruction it
 * starts at. A JIT rewrites generated code in place, often leaving the first
 * instruction as it was, so code that is not read-only and file-backed must
 * be translated one instruction at a time (kmod/main.c, fa_translate_region()):
 * a region would keep running its copy of the instructions after the first.
 *
 * Each case runs version 1 of a small function from an anonymous executable
 * page, overwrites it with version 2 (same first AVX instruction), and runs
 * that. Both results must be right.
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

struct jcase {
	const char *what;
	unsigned char v1[32], v2[32];
	int n, want1, want2;
};

/* each returns eax; xmm/ymm registers are scratch */
static const struct jcase cases[] = {
	{ "integer instruction after the AVX one changes",
	  { 0x31, 0xc0, 0xc5, 0xed, 0xfe, 0xc1, 0x83, 0xc0, 0x01, 0xc3 },	/* xor; vpaddd; add $1; ret */
	  { 0x31, 0xc0, 0xc5, 0xed, 0xfe, 0xc1, 0x83, 0xc0, 0x02, 0xc3 },	/* ...           add $2 */
	  10, 1, 2 },
	{ "second AVX instruction changes",
	  /* vpcmpeqd ymm0,ymm0,ymm0; vpsrld $31,ymm0,ymm1 (ones); vpaddd ymm1,ymm1,ymm2; vmovd xmm2,eax; vzeroupper; ret */
	  { 0xc5, 0xfd, 0x76, 0xc0, 0xc5, 0xf5, 0x72, 0xd0, 0x1f, 0xc5, 0xf5, 0xfe, 0xd1,
	    0xc5, 0xf9, 0x7e, 0xd0, 0xc5, 0xf8, 0x77, 0xc3 },
	  /* same, but vpsubd ymm1,ymm1,ymm2 (0 - ... = 0) -> 1 - 1 = 0 */
	  { 0xc5, 0xfd, 0x76, 0xc0, 0xc5, 0xf5, 0x72, 0xd0, 0x1f, 0xc5, 0xf5, 0xfa, 0xd1,
	    0xc5, 0xf9, 0x7e, 0xd0, 0xc5, 0xf8, 0x77, 0xc3 },
	  21, 2, 0 },
	{ "immediate of a later integer instruction changes (inline cache patch)",
	  /* vpxor xmm0,xmm0,xmm0; mov $1234,eax; ret */
	  { 0xc5, 0xf9, 0xef, 0xc0, 0xb8, 0xd2, 0x04, 0x00, 0x00, 0xc3 },
	  { 0xc5, 0xf9, 0xef, 0xc0, 0xb8, 0x2e, 0x16, 0x00, 0x00, 0xc3 },	/* mov $5678 */
	  10, 1234, 5678 },
};

int main(void)
{
	unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int (*f)(void) = (int (*)(void))p;
	int bad = 0;

	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		const struct jcase *c = &cases[k];
		int a = 0, b = 0, ok;

		memcpy(p, c->v1, c->n);
		for (int i = 0; i < 100; i++)
			a = f();
		memcpy(p, c->v2, c->n);		/* the JIT replaces the code */
		for (int i = 0; i < 100; i++)
			b = f();
		ok = a == c->want1 && b == c->want2;
		printf("  %-70s %d, %d (want %d, %d)%s\n", c->what, a, b, c->want1, c->want2,
		       ok ? "" : "  WRONG");
		bad += !ok;
	}
	printf(bad ? "RESULT: FAIL (stale translation)\n" : "RESULT: PASS\n");
	return bad != 0;
}
