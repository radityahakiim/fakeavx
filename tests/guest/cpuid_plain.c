/*
 * A program that asks CPUID but contains nothing the CPU lacks (built for
 * core2): fakeavx must leave its CPUID alone, so it sees the real CPU.
 * Dynamically linked: a static glibc carries its own AVX string functions.
 * Prints the SSE4.2 and POPCNT bits; on a Penryn guest both must be 0.
 */
#include <stdio.h>
#include <cpuid.h>

int main(void)
{
	unsigned a, b, c, d;

	__cpuid(1, a, b, c, d);
	printf("cpuid_plain: sse4.2=%d popcnt=%d\n", !!(c & bit_SSE4_2), !!(c & bit_POPCNT));
	printf((c & (bit_SSE4_2 | bit_POPCNT)) ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return 0;
}
