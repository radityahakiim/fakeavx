// SPDX-License-Identifier: GPL-2.0
/*
 * The module's cpuid scan (core/scan.c, read piece by piece as in
 * kmod/main.c: fa_scan_range()) against a full linear sweep of the same
 * segments, on whole ELF files:
 *
 *   ./scan_check FILE...      one line per file, exit status 1 if any differ
 *   ./scan_check -p FILE      the offsets only, one per line, to compare with
 *                             objdump: diff <(./scan_check -p F) <(../tools/fakeavx-cpuid scan F)
 *
 * The scan runs with 1 MB pieces (the module's) and with small, odd-sized
 * ones, so that candidates near the ends of pieces are covered too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../core/fakeavx.h"

struct list {
	u64 *v;
	long n, cap;
};

static int add(void *ctx, u64 off)
{
	struct list *l = ctx;

	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 256;
		l->v = realloc(l->v, l->cap * sizeof(*l->v));
		if (!l->v)
			return -1;
	}
	l->v[l->n++] = off;
	return 0;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

/* the definition: every instruction from the segment's start */
static void sweep(const u8 *d, u64 start, u64 end, struct list *l)
{
	u64 o = start;

	while (o < end) {
		int n = fa_sweep_len(d + o, end - o > 15 ? 15 : end - o);

		if (!n)
			break;
		if (n == 2 && d[o] == 0x0f && d[o + 1] == 0xa2)
			add(l, o);
		o += n;
	}
}

/* as the module: pieces of the file, each with the bytes around it */
static void scan(const u8 *d, u64 start, u64 end, u64 chunk, struct list *l)
{
	struct fa_scan s = { .seg_start = start, .done = start, .hit = add, .ctx = l };

	for (u64 from = start; from < end; from += chunk) {
		u64 to = from + chunk < end ? from + chunk : end;
		u64 lo = from > start + FA_SCAN_BACK ? from - FA_SCAN_BACK : start;
		u64 hi = to + 16 < end ? to + 16 : end;
		u8 *piece = malloc(hi - lo);	/* a copy: reads past it are caught by ASan */

		memcpy(piece, d + lo, hi - lo);
		s.buf = piece;
		s.base = lo;
		s.n = hi - lo;
		fa_scan_cpuid(&s, from, to);
		free(piece);
	}
}

static int same(const struct list *a, const struct list *b)
{
	return a->n == b->n && (!a->n || !memcmp(a->v, b->v, a->n * sizeof(*a->v)));
}

int main(int argc, char **argv)
{
	static const u64 chunks[] = { 1 << 20, 65537, 4099 };
	int print = argc > 1 && !strcmp(argv[1], "-p"), bad = 0;

	for (int a = 1 + print; a < argc; a++) {
		struct list full = { 0 }, part[3] = { { 0 } };
		double tf = 0, tp = 0, t;
		int fd = open(argv[a], O_RDONLY), ok = 1;
		struct stat st;
		Elf64_Ehdr *eh;
		u8 *d;

		if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)sizeof(*eh)) {
			if (fd >= 0)
				close(fd);
			continue;
		}
		d = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
		close(fd);
		if (d == MAP_FAILED)
			continue;
		eh = (Elf64_Ehdr *)d;
		if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
		    eh->e_machine != EM_X86_64 || eh->e_phoff + (u64)eh->e_phnum * sizeof(Elf64_Phdr) > (u64)st.st_size) {
			munmap(d, st.st_size);
			continue;
		}
		for (int i = 0; i < eh->e_phnum; i++) {
			Elf64_Phdr *ph = (Elf64_Phdr *)(d + eh->e_phoff) + i;
			u64 end = ph->p_offset + ph->p_filesz;

			if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || end > (u64)st.st_size)
				continue;
			t = now();
			sweep(d, ph->p_offset, end, &full);
			tf += now() - t;
			for (int c = 0; c < 3; c++) {
				t = now();
				scan(d, ph->p_offset, end, chunks[c], &part[c]);
				if (!c)
					tp += now() - t;
			}
		}
		if (print) {
			for (long i = 0; i < part[0].n; i++)
				printf("%llx\n", (unsigned long long)part[0].v[i]);
		} else {
			for (int c = 0; c < 3; c++)
				ok &= same(&full, &part[c]);
			printf("%s: %ld cpuid, sweep %.1f ms, scan %.1f ms: %s\n", argv[a], full.n,
			       tf * 1e3, tp * 1e3, ok ? "same" : "DIFFERENT");
		}
		bad |= !ok;
		free(full.v);
		for (int c = 0; c < 3; c++)
			free(part[c].v);
		munmap(d, st.st_size);
	}
	return bad;
}
