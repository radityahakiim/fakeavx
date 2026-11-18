// SPDX-License-Identifier: GPL-2.0
/*
 * Benchmark kernels for bench/fakeavx-bench.ts, built three times:
 *
 *   core2  -march=core2               runs natively on a Core 2: the reference
 *   sse42  -march=core2 -msse4.2 -mpopcnt   POPCNT, CRC32, PCMPGTQ, PCMPISTRM:
 *                                     translated by fakeavx
 *   avx2   -march=core2 -mavx2 -msse4.2 -mpopcnt   256-bit code, auto-vectorized
 *                                     and through intrinsics (gathers, masked
 *                                     loads, permutes, variable shifts)
 *
 * No FMA, F16C, BMI or LZCNT: fakeavx does not implement them. Every kernel
 * has the signature
 *
 *   u64 k(void *a, void *b, void *c, u64 n, u64 p)
 *
 * and returns its result: a reduction's value, or 0 for kernels that write an
 * array (the caller hashes that array afterwards, outside the timing). Each
 * kernel is written so that all three builds compute bit-identical results (no
 * reassociated floating-point reductions), so the results must match exactly.
 */
#include <stdint.h>
#include <string.h>
#include <immintrin.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define K(name) __attribute__((visibility("default"), noinline)) u64 name(void *pa, void *pb, void *pc, u64 n, u64 p)

static u64 mix(u64 h, u64 v)
{
	h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	return h;
}


/* which build this is: 0 core2, 1 sse42, 2 avx2 */
K(fa_build)
{
	(void)pa; (void)pb; (void)pc; (void)n; (void)p;
#if defined(__AVX2__)
	return 2;
#elif defined(__SSE4_2__)
	return 1;
#else
	return 0;
#endif
}

/*
 * What CPUID says to this file, through fakeavx's uprobe once the file is
 * known (it is scanned when it first executes an instruction the CPU lacks):
 * bit 0 SSE4.2, 1 POPCNT, 2 AVX, 3 AVX2, 4 OSXSAVE.
 */
K(fa_cpuid)
{
	u32 a, b, c, d, r = 0;

	(void)pa; (void)pb; (void)pc; (void)n; (void)p;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	r |= (c >> 20 & 1) << 0 | (c >> 23 & 1) << 1 | (c >> 28 & 1) << 2 | (c >> 27 & 1) << 4;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
	r |= (b >> 5 & 1) << 3;
	return r;
}

/* ---- element-wise floating point: exact in every build ---- */

/* y = a*x + y (float), a = p/16; no FMA, so mul and add round separately */
K(f32_saxpy)
{
	float *x = pa, *y = pb, a = (float)p / 16.0f;

	(void)pc;
	for (u64 i = 0; i < n; i++)
		y[i] = a * x[i] + y[i];
	return 0;	/* output checked by the caller, outside the timing */
}

/* z = x * y + z (double) */
K(f64_muladd)
{
	double *x = pa, *y = pb, *z = pc;

	(void)p;
	for (u64 i = 0; i < n; i++)
		z[i] = x[i] * y[i] + z[i];
	return 0;	/* output checked by the caller, outside the timing */
}

/* c = sqrt(|a|) / (b + 2) (float): divider and square root throughput */
K(f32_sqrt_div)
{
	float *a = pa, *b = pb, *c = pc;

	(void)p;
	for (u64 i = 0; i < n; i++)
		c[i] = __builtin_sqrtf(__builtin_fabsf(a[i])) / (b[i] + 2.0f);
	return 0;	/* output checked by the caller, outside the timing */
}

/* c = a > b ? a : b * 0.5 (float): compare + blend */
K(f32_select)
{
	float *a = pa, *b = pb, *c = pc;

	(void)p;
	for (u64 i = 0; i < n; i++)
		c[i] = a[i] > b[i] ? a[i] : b[i] * 0.5f;
	return 0;	/* output checked by the caller, outside the timing */
}

/* 64x64 single-precision matrix product, i-k-j order: same rounding everywhere */
K(f32_matmul64)
{
	float *a = pa, *b = pb, *c = pc;

	for (u64 r = 0; r < n; r++) {
		memset(c, 0, 64 * 64 * 4);
		for (int i = 0; i < 64; i++)
			for (int k = 0; k < 64; k++) {
				float aik = a[i * 64 + k] + (float)(r & 1);

				for (int j = 0; j < 64; j++)
					c[i * 64 + j] += aik * b[k * 64 + j];
			}
	}
	(void)p;
	return 0;	/* c, checked by the caller */
}

/* ---- integer ---- */

/* c = a * b + c (32-bit): pmulld */
K(i32_muladd)
{
	u32 *a = pa, *b = pb, *c = pc;

	(void)p;
	for (u64 i = 0; i < n; i++)
		c[i] = a[i] * b[i] + c[i];
	return 0;	/* output checked by the caller, outside the timing */
}

/* sum of a[i]*b[i] (16-bit -> 32-bit lanes, then 64-bit): pmaddwd */
K(i16_dot)
{
	s16 *a = pa, *b = pb;
	s64 s = 0;

	(void)pc; (void)p;
	for (u64 i = 0; i < n; i++)
		s += (s32)a[i] * b[i];
	return (u64)s;
}

/* sum of max(a,b) - min(a,b) over bytes: pmaxub, pminub, psadbw */
K(u8_absdiff_sum)
{
	u8 *a = pa, *b = pb;
	u64 s = 0;

	(void)pc; (void)p;
	for (u64 i = 0; i < n; i++)
		s += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
	return s;
}

/* c = a << (b & 31) ^ (a >> (b & 7)) (32-bit): variable shifts (vpsllvd / vpsrlvd) */
K(i32_varshift)
{
	u32 *a = pa, *b = pb, *c = pc;

	(void)p;
	for (u64 i = 0; i < n; i++)
		c[i] = (a[i] << (b[i] & 31)) ^ (a[i] >> (b[i] & 7));
	return 0;	/* output checked by the caller, outside the timing */
}

/* count of a[i] > b[i] over signed 64-bit: pcmpgtq (SSE4.2) / vpcmpgtq */
K(i64_cmpgt_count)
{
	s64 *a = pa, *b = pb;
	u64 cnt = 0, i = 0;

	(void)pc; (void)p;
#if defined(__AVX2__)
	__m256i acc = _mm256_setzero_si256();

	for (; i + 4 <= n; i += 4) {
		__m256i x = _mm256_loadu_si256((const __m256i *)(a + i));
		__m256i y = _mm256_loadu_si256((const __m256i *)(b + i));

		acc = _mm256_sub_epi64(acc, _mm256_cmpgt_epi64(x, y));
	}
	{
		u64 t[4];

		_mm256_storeu_si256((__m256i *)t, acc);
		cnt = t[0] + t[1] + t[2] + t[3];
	}
#elif defined(__SSE4_2__)
	__m128i acc = _mm_setzero_si128();

	for (; i + 2 <= n; i += 2) {
		__m128i x = _mm_loadu_si128((const __m128i *)(a + i));
		__m128i y = _mm_loadu_si128((const __m128i *)(b + i));

		acc = _mm_sub_epi64(acc, _mm_cmpgt_epi64(x, y));
	}
	{
		u64 t[2];

		_mm_storeu_si128((__m128i *)t, acc);
		cnt = t[0] + t[1];
	}
#endif
	for (; i < n; i++)
		cnt += a[i] > b[i];
	return cnt;
}

/* number of bytes equal to p: pcmpeqb + pmovmskb + popcnt */
K(u8_count_byte)
{
	u8 *a = pa, c = (u8)p;
	u64 cnt = 0, i = 0;

	(void)pb; (void)pc;
#if defined(__AVX2__)
	__m256i v = _mm256_set1_epi8((char)c);

	for (; i + 32 <= n; i += 32)
		cnt += __builtin_popcount((u32)_mm256_movemask_epi8(
			_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(a + i)), v)));
#else
	__m128i v = _mm_set1_epi8((char)c);

	for (; i + 16 <= n; i += 16)
		cnt += __builtin_popcount((u32)_mm_movemask_epi8(
			_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(a + i)), v)));
#endif
	for (; i < n; i++)
		cnt += a[i] == c;
	return cnt;
}

/* table lookup per nibble (pshufb): popcount of every byte, summed */
K(u8_pshufb_popcnt)
{
	u8 *a = pa;
	u64 s = 0, i = 0;

	(void)pb; (void)pc; (void)p;
#if defined(__AVX2__)
	const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
					     0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
	const __m256i low = _mm256_set1_epi8(0x0f);
	__m256i acc = _mm256_setzero_si256();

	for (; i + 32 <= n; i += 32) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(a + i));
		__m256i c = _mm256_add_epi8(_mm256_shuffle_epi8(lut, _mm256_and_si256(v, low)),
					    _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), low)));

		acc = _mm256_add_epi64(acc, _mm256_sad_epu8(c, _mm256_setzero_si256()));
	}
	{
		u64 t[4];

		_mm256_storeu_si256((__m256i *)t, acc);
		s = t[0] + t[1] + t[2] + t[3];
	}
#else
	const __m128i lut = _mm_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
	const __m128i low = _mm_set1_epi8(0x0f);
	__m128i acc = _mm_setzero_si128();

	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(a + i));
		__m128i c = _mm_add_epi8(_mm_shuffle_epi8(lut, _mm_and_si128(v, low)),
					 _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(v, 4), low)));

		acc = _mm_add_epi64(acc, _mm_sad_epu8(c, _mm_setzero_si128()));
	}
	{
		u64 t[2];

		_mm_storeu_si128((__m128i *)t, acc);
		s = t[0] + t[1];
	}
#endif
	for (; i < n; i++)
		s += __builtin_popcount(a[i]);
	return s;
}

/* sum of popcounts of 64-bit words: POPCNT (sse42, avx2) or SWAR (core2) */
K(u64_popcount)
{
	u64 *a = pa, s = 0;

	(void)pb; (void)pc; (void)p;
	for (u64 i = 0; i < n; i++)
		s += __builtin_popcountll(a[i]);
	return s;
}

/* CRC-32C of n bytes: CRC32 instruction (sse42, avx2) or a table (core2) */
K(crc32c)
{
	u8 *a = pa;
	u64 i = 0;
	u32 crc = ~0u;

	(void)pb; (void)pc; (void)p;
#if defined(__SSE4_2__)
	u64 c64 = crc;

	for (; i + 8 <= n; i += 8) {
		u64 w;

		memcpy(&w, a + i, 8);
		c64 = _mm_crc32_u64(c64, w);
	}
	crc = (u32)c64;
	for (; i < n; i++)
		crc = _mm_crc32_u8(crc, a[i]);
#else
	static u32 table[256];

	if (!table[1])
		for (u32 b = 0; b < 256; b++) {
			u32 r = b;

			for (int k = 0; k < 8; k++)
				r = r & 1 ? (r >> 1) ^ 0x82f63b78u : r >> 1;
			table[b] = r;
		}
	for (; i < n; i++)
		crc = (crc >> 8) ^ table[(crc ^ a[i]) & 0xff];
#endif
	return ~crc;
}

/*
 * Count of text bytes that are in a set of 16 characters: pcmpistrm (SSE4.2
 * string compare, through the fakeavx helper) or a table. The text has no
 * zero bytes, so implicit-length strings see whole 16-byte blocks.
 */
K(sse42_strset_count)
{
	u8 *a = pa;
	const u8 *set = pb;
	u64 cnt = 0, i = 0;

	(void)pc; (void)p;
#if defined(__SSE4_2__)
	__m128i s = _mm_loadu_si128((const __m128i *)set);

	for (; i + 16 <= n; i += 16) {
		__m128i m = _mm_cmpistrm(s, _mm_loadu_si128((const __m128i *)(a + i)),
					 _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_BIT_MASK);

		cnt += __builtin_popcount((u32)_mm_cvtsi128_si32(m));
	}
#endif
	{
		u8 in[256] = { 0 };

		for (int k = 0; k < 16 && set[k]; k++)
			in[set[k]] = 1;
		for (; i < n; i++)
			cnt += in[a[i]];
	}
	return cnt;
}

/* sum of t[idx[i]] (32-bit): gathers */
K(i32_gather_sum)
{
	u32 *t = pa, *idx = pb;
	u64 s = 0, i = 0;

	(void)pc; (void)p;
#if defined(__AVX2__)
	__m256i acc = _mm256_setzero_si256();

	for (; i + 8 <= n; i += 8) {
		__m256i ix = _mm256_loadu_si256((const __m256i *)(idx + i));
		__m256i v = _mm256_i32gather_epi32((const int *)t, ix, 4);

		acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(v)));
		acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_extracti128_si256(v, 1)));
	}
	{
		u64 w[4];

		_mm256_storeu_si256((__m256i *)w, acc);
		s = w[0] + w[1] + w[2] + w[3];
	}
#endif
	for (; i < n; i++)
		s += t[idx[i]];
	return s;
}

/* sum of t[idx[i]] (double, as bits): 64-bit gathers */
K(f64_gather_sum)
{
	double *t = pa;
	u32 *idx = pb;
	u64 s = 0, i = 0;

	(void)pc; (void)p;
#if defined(__AVX2__)
	__m256i acc = _mm256_setzero_si256();

	for (; i + 4 <= n; i += 4) {
		__m128i ix = _mm_loadu_si128((const __m128i *)(idx + i));
		__m256d v = _mm256_i32gather_pd(t, ix, 8);

		acc = _mm256_add_epi64(acc, _mm256_castpd_si256(v));
	}
	{
		u64 w[4];

		_mm256_storeu_si256((__m256i *)w, acc);
		s = w[0] + w[1] + w[2] + w[3];
	}
#endif
	for (; i < n; i++) {
		u64 b;

		memcpy(&b, &t[idx[i]], 8);
		s += b;
	}
	return s;
}

/*
 * Sum of a[] over blocks of 8, of which only the first (p + block) % 9
 * elements count: masked loads (vpmaskmovd) in the AVX2 build.
 */
K(i32_maskload_sum)
{
	s32 *a = pa;
	s64 s = 0;
	u64 blocks = n / 8;

	(void)pb; (void)pc;
	for (u64 k = 0; k < blocks; k++) {
		int m = (int)((p + k) % 9);
#if defined(__AVX2__)
		__m256i mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(m), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
		__m256i v = _mm256_maskload_epi32(a + 8 * k, mask);
		s32 t[8];

		_mm256_storeu_si256((__m256i *)t, v);
		for (int j = 0; j < 8; j++)
			s += t[j];
#else
		for (int j = 0; j < m; j++)
			s += a[8 * k + j];
#endif
	}
	return (u64)s;
}

/* c = blocks of 8 dwords permuted by the pattern in p: vpermd */
K(i32_permute)
{
	u32 *a = pa, *c = pc;
	u32 perm[8];
	u64 i = 0;

	(void)pb;
	for (int j = 0; j < 8; j++)
		perm[j] = (u32)(p >> (4 * j)) & 7;
#if defined(__AVX2__)
	__m256i pv = _mm256_loadu_si256((const __m256i *)perm);

	for (; i + 8 <= n; i += 8)
		_mm256_storeu_si256((__m256i *)(c + i),
				    _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i *)(a + i)), pv));
#endif
	for (; i + 8 <= n; i += 8)
		for (int j = 0; j < 8; j++)
			c[i + j] = a[i + perm[j]];
	for (; i < n; i++)
		c[i] = a[i];
	return 0;	/* output checked by the caller, outside the timing */
}

/* find the first byte > 127 in each 64-byte line (vpmovmskb + tzcnt-free scan) */
K(u8_first_high)
{
	u8 *a = pa;
	u64 h = 0;

	(void)pb; (void)pc; (void)p;
	for (u64 off = 0; off + 64 <= n; off += 64) {
		u64 m = 0;

#if defined(__AVX2__)
		m = (u32)_mm256_movemask_epi8(_mm256_loadu_si256((const __m256i *)(a + off))) |
		    (u64)(u32)_mm256_movemask_epi8(_mm256_loadu_si256((const __m256i *)(a + off + 32))) << 32;
#else
		for (int k = 0; k < 4; k++)
			m |= (u64)(u32)_mm_movemask_epi8(_mm_loadu_si128((const __m128i *)(a + off + 16 * k)))
			     << (16 * k);
#endif
		h = mix(h, m ? (u64)__builtin_ctzll(m) : 64);	/* bsf: never tzcnt here */
	}
	return h;
}
