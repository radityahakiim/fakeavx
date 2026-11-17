// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx region test. Needs a CPU with real AVX2.
 *
 * Each AVX test kernel from tests/guest is turned into regions starting at
 * its first instruction, and the region is called in place of the function.
 * Whatever the region does not cover runs natively. Results must match the
 * plain C reference bit for bit.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include "../core/fakeavx.h"
#include "../helpers/blob.h"
#include "guest/kern.h"

void fa_enter_sync(struct data *d);
void fa_exit_thunk(void);
extern u64 fa_sync_target;

static int rd(void *ctx, u64 addr, u8 *buf, int n)
{
	memcpy(buf, (void *)addr, n);
	return n;
}

static u64 rs;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static void fill(struct data *d, u64 seed)
{
	rs = seed * 0x9e3779b97f4a7c15ULL + 1;
	for (int i = 0; i < N; i++) {
		d->fa[i] = (float)(rnd() % 20000) / 37.0f - 250.0f;
		d->fb[i] = (float)(rnd() % 1000) / 9.0f;
		d->fc[i] = (float)(int)(rnd() % 4000 - 2000) / 3.0f;
		d->da[i] = (double)(rnd() % 1000000) / 1234.5 - 300.0;
		d->db[i] = (double)(rnd() % 5000) / 77.0;
		d->ia[i] = (int32_t)rnd();
		d->ib[i] = (int32_t)rnd() >> (rnd() % 20);
		d->ba[i] = rnd();
		d->bb[i] = (rnd() & 3) ? rnd() : d->ba[i];
		d->qa[i] = rnd();
		d->idx[i] = (int32_t)rnd();
	}
}

typedef void (*kfn)(struct data *);
static const struct { const char *name; kfn ref, avx; } kernels[] = {
	{ "float", ref_float, avx_float }, { "double", ref_double, avx_double },
	{ "int", ref_int, avx_int }, { "bytes", ref_bytes, avx_bytes },
	{ "popcnt", ref_popcnt, avx_popcnt }, { "simd", ref_simd, avx_simd },
	{ "mixsse", ref_mixsse, avx_mixsse },
};

int main(void)
{
	u8 *code = mmap((void *)0x10000000, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	u8 *st = mmap(NULL, FA_ST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct fa_rgn *r = malloc(sizeof(*r));
	struct data *a = malloc(sizeof(*a)), *b = malloc(sizeof(*b));
	u32 used = (sizeof(fa_blob) + 63) & ~63u;
	int fails = 0;
	u64 v;

	if (code == MAP_FAILED || st == MAP_FAILED)
		return 1;
	memcpy(code, fa_blob, sizeof(fa_blob));
	v = (u64)st; memcpy(st + FA_ST_SELF, &v, 8);
	v = FA_MAGIC; memcpy(st + FA_ST_MAGIC, &v, 8);
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, st))
		return 1;

	for (int mode = 0; mode < 2; mode++) {
		for (unsigned k = 0; k < sizeof(kernels) / sizeof(kernels[0]); k++) {
			int nours = 0, nint = 0, ncached = 0, bad = 0, len;
			u8 *entry;

			memset(r, 0, sizeof(*r));
			r->read = rd;
			r->entry_ip = (u64)kernels[k].avx;
			r->out = code + used;
			r->cap = 16384;
			r->out_addr = (u64)r->out;
			r->blob_addr = (u64)code;
			r->exit_thunk = (u64)fa_exit_thunk;
			r->no_map = mode;
			if (fa_region_build(r) < 0) {
				printf("%-8s region build failed\n", kernels[k].name);
				fails++;
				continue;
			}
			used += (r->len + 15) & ~15u;
			entry = code + used;
			len = fa_emit_entry(entry, (u64)entry, r->has_map ? &r->map : NULL, r->out_addr);
			used += (len + 15) & ~15u;
			for (u32 i = 0; i < r->n; i++)
				r->it[i].kind == FA_R_OURS ? nours++ : nint++;
			for (int g = 0; g < 16; g++)
				ncached += r->map.hi[g] >= 0;

			fa_sync_target = (u64)entry;
			for (int seed = 1; seed <= 20; seed++) {
				fill(a, seed + 31 * k);
				memcpy(b, a, sizeof(*a));
				kernels[k].ref(a);
				fa_enter_sync(b);
				if (memcmp(a, b, sizeof(*a)))
					bad++;
			}
			printf("%-8s %s: %3u instructions (%d AVX, %d integer), %4u bytes, "
			       "%d upper halves cached, %d scratch: %s\n",
			       kernels[k].name, mode ? "no borrowing" : "borrowing   ", r->n, nours, nint,
			       r->len, ncached, r->has_map ? r->map.nscratch : 0, bad ? "MISMATCH" : "ok");
			fails += !!bad;
		}
	}
	printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return !!fails;
}
