// SPDX-License-Identifier: GPL-2.0
/*
 * Check core/divert.c against objdump.
 *
 * fa_find_divert() decides where an instruction starts by decoding forward
 * from function starts. Here its answers are compared with objdump's
 * disassembly of the whole executable section:
 *
 *   objdump -d --insn-width=16 -j .text BINARY | awk -f divert_truth.awk > truth.txt
 *   divert_check BINARY truth.txt
 *
 * Each line of truth.txt is a short instruction's address followed by the
 * addresses of the instructions before it, nearest first. For every one that
 * fakeavx translates, the diversion point found, if any, must be one of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include "../core/fakeavx.h"

static const char *const names[] = {
	"diverted", "no function start", "decoding misses it", "no long instruction before it",
	"function starts disagree",
};

int main(int argc, char **argv)
{
	unsigned long counts[5] = { 0 }, wrong = 0, skipped = 0;
	char line[4096];
	FILE *f, *t;
	Elf64_Ehdr eh;
	Elf64_Phdr ph[64];
	u8 *buf;
	long size;
	int i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s binary truth.txt\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	t = fopen(argv[2], "r");
	if (!f || !t) {
		perror("open");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	buf = malloc(size);
	if (!buf || fread(buf, 1, size, f) != (size_t)size)
		return 2;
	memcpy(&eh, buf, sizeof(eh));
	if (eh.e_phnum > 64)
		return 2;
	memcpy(ph, buf + eh.e_phoff, eh.e_phnum * sizeof(ph[0]));

	while (fgets(line, sizeof(line), t)) {
		u64 ip, prev[64], x, base = 0, end = 0;
		int np = 0, n, r, ok;
		char *p = line, *q;
		struct fa_insn in;
		const u8 *code;

		ip = strtoull(p, &q, 16);
		for (p = q; np < 64; p = q) {
			prev[np] = strtoull(p, &q, 16);
			if (q == p)
				break;
			np++;
		}
		for (i = 0; i < eh.e_phnum; i++)
			if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) &&
			    ip >= ph[i].p_vaddr && ip < ph[i].p_vaddr + ph[i].p_filesz)
				break;
		if (i == eh.e_phnum)
			return 2;
		code = buf + ph[i].p_offset - ph[i].p_vaddr;	/* indexed by address */
		if (fa_decode(code + ip, 15, ip, &in) || in.kind == FA_K_XGETBV ||
		    in.len < 3 || in.len > 4) {
			skipped++;
			continue;
		}
		base = ip > ph[i].p_vaddr + 70000 ? ip - 70000 : ph[i].p_vaddr;
		end = ph[i].p_vaddr + ph[i].p_filesz;
		n = end - base < 70100 ? end - base : 70100;
		r = fa_find_divert(code + base, base, n, ip, &x);
		counts[r]++;
		if (r)
			continue;
		ok = 0;
		for (i = 0; i < np; i++)
			ok |= prev[i] == x;
		if (!ok && np == 24 && x < prev[23])
			ok = -1;	/* further back than the truth file reaches */
		if (!ok) {
			wrong++;
			printf("WRONG %#llx: diverts at %#llx, objdump disagrees\n",
			       (unsigned long long)ip, (unsigned long long)x);
		} else if (ok < 0) {
			printf("unverified %#llx: diverts at %#llx\n",
			       (unsigned long long)ip, (unsigned long long)x);
		}
	}
	for (i = 0; i < 5; i++)
		printf("%-32s %lu\n", names[i], counts[i]);
	printf("%-32s %lu\n%-32s %lu\n", "not translated or not short", skipped, "WRONG", wrong);
	return wrong ? 1 : 0;
}
