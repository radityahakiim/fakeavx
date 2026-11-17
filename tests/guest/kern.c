/* Compiled twice: plain C with -mno-avx (PFX=ref) and AVX2 (PFX=avx, -DUSE_AVX). */
#include "kern.h"
#include <math.h>
#ifdef USE_AVX
#include <immintrin.h>
#endif
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define FN(n) CAT(PFX, n)

void FN(_float)(struct data *d)
{
	for (int i = 0; i < N; i++)
		d->fo[i] = __builtin_sqrtf(d->fa[i] * d->fb[i] + d->fc[i] * d->fc[i]) / (d->fb[i] * d->fb[i] + 1.5f)
			- (d->fa[i] > d->fc[i] ? d->fa[i] : d->fc[i]);
}

void FN(_double)(struct data *d)
{
	for (int i = 0; i < N; i++)
		d->dout[i] = (d->da[i] - d->db[i]) * (d->da[i] + 0.25) / (d->db[i] * d->db[i] + 3.0) + (double)d->ia[i];
}

void FN(_int)(struct data *d)
{
	uint64_t s = 0;

	for (int i = 0; i < N; i++) {
		int32_t a = d->ia[i], b = d->ib[i];

		d->io[i] = ((a * b) ^ (a >> 3)) + (a > b ? a : b) - (b < 0 ? -b : b);
		s += (uint32_t)d->io[i];
	}
	for (int i = 0; i < N; i++)
		d->qo[i] = (d->qa[i] << 7) ^ (d->qa[i] >> 11) ^ (uint64_t)d->ib[i];
	d->sum = s;
}

void FN(_bytes)(struct data *d)
{
	for (int i = 0; i < N; i++) {
		unsigned a = d->ba[i], b = d->bb[i], s = a + b;

		d->bo[i] = (s > 255 ? 255 : s) ^ (a > b ? a - b : b - a);
	}
}

void FN(_popcnt)(struct data *d)
{
	uint64_t s = 0;

	for (int i = 0; i < N; i++) {
		volatile uint64_t v = d->qa[i];	/* keep it scalar: one popcnt per element */

		s += __builtin_popcountll(v) * (uint64_t)(i + 1);
		s += __builtin_popcount((uint32_t)v & 0xffff00) + __builtin_popcount((uint16_t)(v >> 7));
	}
	d->sum = s;
}

#ifndef USE_AVX
void FN(_simd)(struct data *d)
{
	uint64_t s = 0;

	for (int i = 0; i < N; i += 8) {
		int32_t g[8], p[8], sh[8];
		int mask = 0;

		for (int j = 0; j < 8; j++)
			g[j] = d->ia[d->idx[i + j] & (N - 1)];		/* gather */
		for (int j = 0; j < 8; j++)
			p[j] = g[d->ib[i + j] & 7];			/* vpermd */
		for (int j = 0; j < 8; j++) {
			uint32_t c = (uint32_t)d->idx[i + j] & 63;

			sh[j] = c < 32 ? (int32_t)((uint32_t)p[j] << c) : 0;	/* vpsllvd */
			sh[j] = sh[j] > d->ib[i + j] ? sh[j] : d->ib[i + j];	/* vpmaxsd */
		}
		for (int j = 0; j < 8; j++)
			d->io[i + j] = d->ia[i + j] < 0 ? sh[j] : g[j];	/* blendv */
		for (int j = 0; j < 32; j++)
			mask |= (d->ba[(i * 4 + j) & (N - 1)] == d->bb[(i * 4 + j) & (N - 1)]) << j;
		s += (uint32_t)mask;
	}
	for (int i = 0; i < N; i += 4) {
		uint64_t q[4];

		for (int j = 0; j < 4; j++)
			q[j] = (int64_t)d->qa[i + j] > (int64_t)d->qa[i + (j ^ 1)] ? ~0ULL : 0;	/* vpcmpgtq */
		d->qo[i + 0] = q[2] ^ d->qa[i + 2];	/* vpermq 0x4e */
		d->qo[i + 1] = q[3] ^ d->qa[i + 3];
		d->qo[i + 2] = q[0] ^ d->qa[i + 0];
		d->qo[i + 3] = q[1] ^ d->qa[i + 1];
	}
	for (int i = 0; i < N; i += 32)
		for (int j = 0; j < 32; j++) {
			uint8_t m = d->bb[i + j];

			d->bo[i + j] = (m & 0x80) ? 0 : d->ba[i + (j & 16) + (m & 15)];	/* vpshufb */
		}
	d->sum = s;
}
#else
void FN(_simd)(struct data *d)
{
	uint64_t s = 0;

	for (int i = 0; i < N; i += 8) {
		__m256i idx = _mm256_loadu_si256((void *)&d->idx[i]);
		__m256i ib = _mm256_loadu_si256((void *)&d->ib[i]);
		__m256i ia = _mm256_loadu_si256((void *)&d->ia[i]);
		__m256i g = _mm256_i32gather_epi32(d->ia, _mm256_and_si256(idx, _mm256_set1_epi32(N - 1)), 4);
		__m256i p = _mm256_permutevar8x32_epi32(g, ib);
		__m256i sh = _mm256_sllv_epi32(p, _mm256_and_si256(idx, _mm256_set1_epi32(63)));
		__m256i r;

		sh = _mm256_max_epi32(sh, ib);
		r = _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(g), _mm256_castsi256_ps(sh),
							 _mm256_castsi256_ps(ia)));
		_mm256_storeu_si256((void *)&d->io[i], r);
		s += (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(
			_mm256_loadu_si256((void *)&d->ba[(i * 4) & (N - 1)]),
			_mm256_loadu_si256((void *)&d->bb[(i * 4) & (N - 1)])));
	}
	for (int i = 0; i < N; i += 4) {
		__m256i a = _mm256_loadu_si256((void *)&d->qa[i]);
		__m256i sw = _mm256_shuffle_epi32(a, 0x4e);
		__m256i q = _mm256_xor_si256(_mm256_cmpgt_epi64(a, sw), a);

		_mm256_storeu_si256((void *)&d->qo[i], _mm256_permute4x64_epi64(q, 0x4e));
	}
	for (int i = 0; i < N; i += 32)
		_mm256_storeu_si256((void *)&d->bo[i],
			_mm256_shuffle_epi8(_mm256_loadu_si256((void *)&d->ba[i]),
					    _mm256_loadu_si256((void *)&d->bb[i])));
	_mm256_zeroupper();
	d->sum = s;
}
#endif
