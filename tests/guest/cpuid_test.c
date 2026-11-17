/*
 * fakeavx CPUID + SSE4.2 test, for a CPU without SSE4.2.
 * Register this binary's cpuid sites first (tools/fakeavx-cpuid add), then run
 * with FAKEAVX=1 (expects SSE4.2 + POPCNT reported, AVX not) or FAKEAVX=avx2.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cpuid.h>
#include <nmmintrin.h>

static uint32_t crc_ref(uint32_t crc, const uint8_t *p, int n)
{
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0x82f63b78 & -(crc & 1));
	}
	return crc;
}

int main(void)
{
	unsigned a, b, c, d, fails = 0;
	const char *mode = getenv("FAKEAVX");
	int want_avx = mode && !strcmp(mode, "avx2");
	uint8_t buf[256];
	uint64_t x = 88172645463325252ULL;

	__cpuid(1, a, b, c, d);
	printf("cpuid.1:ecx  sse4.2=%d popcnt=%d osxsave=%d avx=%d\n",
	       !!(c & bit_SSE4_2), !!(c & bit_POPCNT), !!(c & bit_OSXSAVE), !!(c & bit_AVX));
	fails += !(c & bit_SSE4_2) + !(c & bit_POPCNT) + (!!(c & bit_AVX) != want_avx);
	__cpuid(0, a, b, c, d);
	if (a >= 7) {
		__cpuid_count(7, 0, a, b, c, d);
		printf("cpuid.7:ebx  avx2=%d\n", !!(b & bit_AVX2));
		fails += !!(b & bit_AVX2) != want_avx;
	} else {
		printf("cpuid.7 not available\n");
		fails += want_avx;
	}
	printf("__builtin_cpu_supports: sse4.2=%d popcnt=%d avx2=%d\n",
	       !!__builtin_cpu_supports("sse4.2"), !!__builtin_cpu_supports("popcnt"),
	       !!__builtin_cpu_supports("avx2"));

	for (int i = 0; i < 256; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		buf[i] = x;
	}
	/* crc32: 8, 16, 32 and 64-bit forms */
	for (int i = 0; i < 200; i++) {
		uint32_t r = 0x12345678 + i, s = r;
		uint16_t w; uint32_t l; uint64_t q;

		r = _mm_crc32_u8(r, buf[i]);			s = crc_ref(s, buf + i, 1);
		memcpy(&w, buf + i + 1, 2); r = _mm_crc32_u16(r, w); s = crc_ref(s, buf + i + 1, 2);
		memcpy(&l, buf + i + 3, 4); r = _mm_crc32_u32(r, l); s = crc_ref(s, buf + i + 3, 4);
		memcpy(&q, buf + i + 7, 8); r = _mm_crc32_u64(r, q); s = crc_ref(s, buf + i + 7, 8);
		if (r != s) { fails++; printf("crc32 mismatch at %d\n", i); break; }
	}
	/* pcmpgtq */
	for (int i = 0; i < 200; i++) {
		int64_t p[2], q[2], r[2];

		memcpy(p, buf + i, 16); memcpy(q, buf + i + 16, 16);
		if (i & 1) q[0] = p[0];
		_mm_storeu_si128((void *)r, _mm_cmpgt_epi64(_mm_loadu_si128((void *)p), _mm_loadu_si128((void *)q)));
		if (r[0] != (p[0] > q[0] ? -1 : 0) || r[1] != (p[1] > q[1] ? -1 : 0)) {
			fails++; printf("pcmpgtq mismatch at %d\n", i); break;
		}
	}
	/* pcmpistri / pcmpestri: first byte from a set; first differing byte */
	for (int i = 0; i < 200; i++) {
		char hay[17], set[17] = "aeiou";
		int ref = 16, r;

		for (int k = 0; k < 16; k++) hay[k] = 'a' + buf[i + k] % 26;
		hay[16] = 0;
		if (i % 5 == 0) hay[i % 16] = 0;
		for (int k = 0; k < 16 && hay[k]; k++)
			if (strchr("aeiou", hay[k])) { ref = k; break; }
		r = _mm_cmpistri(_mm_loadu_si128((void *)set), _mm_loadu_si128((void *)hay),
				 _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_LEAST_SIGNIFICANT);
		if (r != ref) { fails++; printf("pcmpistri mismatch at %d: %d vs %d\n", i, r, ref); break; }
		r = _mm_cmpestri(_mm_loadu_si128((void *)hay), 16, _mm_loadu_si128((void *)(buf + i)), 16,
				 _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_NEGATIVE_POLARITY);
		ref = 16;
		for (int k = 0; k < 16; k++)
			if ((uint8_t)hay[k] != buf[i + k]) { ref = k; break; }
		if (r != ref) { fails++; printf("pcmpestri mismatch at %d: %d vs %d\n", i, r, ref); break; }
	}
	/* popcnt, and libc string functions (which now pick their SSE4.2 versions) */
	{
		volatile uint64_t v = 0xf0f0123456789abcULL;
		char s1[64] = "the quick brown fox jumps over the lazy dog", s2[64];

		fails += __builtin_popcountll(v) != 30;
		strcpy(s2, s1);
		fails += strcmp(s1, s2) != 0 || strcspn(s1, "xyz") != 18 || strspn(s1, "the quick") != 10 ||
			 strstr(s1, "lazy") != s1 + 35 || strpbrk(s1, "jv") != s1 + 20;
	}
	printf(fails ? "RESULT: FAIL (%u)\n" : "RESULT: PASS\n", fails);
	return !!fails;
}
