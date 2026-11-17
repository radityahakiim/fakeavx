// SPDX-License-Identifier: GPL-2.0
/* Robustness fuzz: the decoder and translator see attacker-controlled bytes in
 * kernel context, so they must never read or write out of bounds.
 * Build with ASan/UBSan (make fuzz). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fakeavx.h"

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

/* region builder over a small random "address space" full of branches */
#define IMG 192
static u8 *img;
static const u64 img_base = 0x555555100000ULL;

static int img_read(void *ctx, u64 addr, u8 *buf, int n)
{
	u64 off = addr - img_base;

	if (off >= IMG)
		return 0;
	if (off + n > IMG)
		n = IMG - off;
	memcpy(buf, img + off, n);
	return n;
}

static int fuzz_regions(long iters)
{
	static const u8 seeds[][5] = {
		{ 0xc5, 0xfd, 0xfe, 0xc1 }, { 0xc5, 0xf8, 0x77 }, { 0x75, 0xf0 }, { 0xeb, 0x05 },
		{ 0x48, 0x83, 0xc0, 0x20 }, { 0xc5, 0xfe, 0x6f, 0x07 }, { 0x0f, 0x85, 0xe0, 0xff, 0xff },
		{ 0xc4, 0xe2, 0x7d, 0x58, 0xc1 }, { 0x48, 0x39, 0xd0 }, { 0xf3, 0x48, 0x0f, 0xb8, 0xc1 },
		{ 0x72, 0x10 }, { 0xc5, 0xfd, 0x7f, 0x06 }, { 0xc5, 0xf5, 0xef, 0xc9 },
	};
	struct fa_rgn *r = malloc(sizeof(*r));
	long built = 0, mapped = 0, maxn = 0, maxlen = 0;

	for (long i = 0; i < iters; i++) {
		u8 *out = malloc(16384);
		int k;

		img = malloc(IMG);
		for (k = 0; k < IMG; ) {
			if (rnd() % 5 == 0) {
				img[k++] = rnd();
			} else {
				const u8 *sd = seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))];
				int l = 2 + rnd() % 4;

				if (k + l > IMG)
					l = IMG - k;
				memcpy(img + k, sd, l);
				if (rnd() % 3 == 0)
					img[k + l - 1] = rnd();
				k += l;
			}
		}
		memset(r, 0, sizeof(*r));
		r->read = img_read;
		r->entry_ip = img_base + rnd() % 32;
		r->out = out;
		r->cap = 16384;
		r->out_addr = (rnd() & 1) ? 0x555550000000ULL : 0x7f0000000000ULL;
		r->blob_addr = r->out_addr - 0x10000;
		r->no_map = rnd() % 4 == 0;
		r->max_items = rnd() % 8 ? 0 : 1 + rnd() % 4;
		if (!fa_region_build(r)) {
			u8 stub[FA_ENTRY_MAX];

			built++;
			mapped += r->has_map;
			if (r->n > maxn) maxn = r->n;
			if (r->len > maxlen) maxlen = r->len;
			if (r->len > 16384 || r->n > FA_RG_ITEMS ||
			    fa_emit_entry(stub, 0x1000, r->has_map ? &r->map : NULL, 0x2000) > FA_ENTRY_MAX) {
				printf("region overflow\n");
				return 1;
			}
		}
		free(out);
		free(img);
	}
	free(r);
	printf("%ld regions built of %ld, %ld borrowing registers, largest %ld instructions / %ld bytes\n",
	       built, iters, mapped, maxn, maxlen);
	return 0;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 5000000, ok = 0, tr = 0, maxlen = 0;

	for (long i = 0; i < iters; i++) {
		u8 *code = malloc(15), *out = malloc(FA_MAX_STUB);
		struct fa_insn in;
		struct fa_env env = { out, FA_MAX_STUB, 0, 0, 0 };
		int n = 1 + rnd() % 15, k, len;

		for (k = 0; k < 15; k++)
			code[k] = rnd();
		switch (rnd() % 8) {
		case 0: case 1: case 2: code[0] = 0xc5; break;
		case 3: case 4: case 5: code[0] = 0xc4; code[1] = (code[1] & 0xe0) | (1 + rnd() % 3); break;
		case 6: code[0] = 0xf3; code[1] = 0x0f; code[2] = 0xb8; break;
		case 7: if (rnd() & 1) { code[0] = 0x66; code[1] = 0x0f; } else { code[0] = 0x0f; } break;
		default: break;
		}
		/* stub address: sometimes near the code, sometimes far away */
		env.out_addr = (rnd() & 1) ? 0x555555000000ULL : 0x7f0000000000ULL;
		env.blob_addr = env.out_addr - 0x10000;
		env.no_jmp = rnd() & 1;
		code = realloc(code, n);	/* exact-size buffer so ASan sees over-reads */
		{
			struct fa_int si;
			u16 xm;

			if (fa_decode_sse(code, n, 0x555555100000ULL, &si, &xm) == 0 && si.len > n) {
				printf("sse decoder claims %d bytes of %d\n", si.len, n);
				return 1;
			}
		}
		if (fa_decode(code, n, 0x555555100000ULL, &in) == 0) {
			ok++;
			if (in.len > n) {
				printf("decoder claims %d bytes of %d\n", in.len, n);
				return 1;
			}
			len = fa_translate(&in, &env);
			if (len > FA_MAX_STUB) {
				printf("stub overflow\n");
				return 1;
			}
			if (len > 0)
				tr++;
			if (len > maxlen)
				maxlen = len;
		}
		free(code);
		free(out);
	}
	printf("%ld inputs, %ld decoded, %ld translated, longest stub %ld bytes\n", iters, ok, tr, maxlen);
	return fuzz_regions(iters / 20);
}
