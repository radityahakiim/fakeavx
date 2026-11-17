// SPDX-License-Identifier: GPL-2.0
/*
 * Walk the executable segments of an ELF file with fa_insn_len() and print
 * the file offset of every cpuid instruction - what the kernel module does
 * when it scans a binary. Compare with "tools/fakeavx-cpuid scan" (objdump):
 *   ./lendec_check FILE | diff - <(../tools/fakeavx-cpuid scan FILE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include "../core/fakeavx.h"

int main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
	Elf64_Ehdr eh;
	long size;
	u8 *d;

	if (!f)
		return 1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	d = malloc(size);
	if (fread(d, 1, size, f) != (size_t)size)
		return 1;
	memcpy(&eh, d, sizeof(eh));
	for (int i = 0; i < eh.e_phnum; i++) {
		Elf64_Phdr ph;
		u64 o, end;

		memcpy(&ph, d + eh.e_phoff + i * eh.e_phentsize, sizeof(ph));
		if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X))
			continue;
		end = ph.p_offset + ph.p_filesz;
		for (o = ph.p_offset; o < end; ) {
			int l = fa_insn_len(d + o, end - o > 15 ? 15 : end - o);

			if (!l)
				break;
			if (l == 2 && d[o] == 0x0f && d[o + 1] == 0xa2)
				printf("%lx\n", (unsigned long)o);
			o += l;
		}
	}
	return 0;
}
