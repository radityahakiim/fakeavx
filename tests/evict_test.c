// SPDX-License-Identifier: GPL-2.0
/*
 * Eviction test (kmod/main.c, fa_ev_window()), with the module loaded.
 *
 *   make -C tests evict_test && tests/evict_test [rounds]
 *
 * Runs the functions of evict_test.S (a non-PIE binary) on random inputs
 * and checks every result against C:
 *   1. warm-up through the normal entries (this is where eviction happens);
 *   2. jumps straight into the evicted instructions (they must trap into
 *      their records and still compute the right thing);
 *   3. steady state: repeated traps per call (0 with eviction, 1 without);
 *   4. fork(): a child, which has the memory but none of the records, jumps
 *      into the evicted instructions first, then uses the normal entries.
 * Exit status 0 = all results right (and, with a module that evicts, no
 * repeated traps at the entries).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

typedef uint8_t u8;
typedef uint32_t u32;

u32 ev_a(const u8 *p, const u8 *q);
u32 ev_ga(const u8 *p, const u8 *q);
u32 ev_b(const u8 *p, const u8 *q);
u32 ev_gb(const u8 *p, const u8 *q);

static u32 ref_a(const u8 *p, const u8 *q)
{
	u32 m = 0;

	for (int i = 0; i < 32; i++)
		m |= (u32)(p[i] == q[i]) << i;
	return m;
}

static u32 ref_b(const u8 *p, const u8 *q)
{
	u32 m = 0;

	for (int i = 0; i < 16; i++)
		m |= (u32)((u8)(p[i] + q[i]) >> 7) << i;
	return m;
}

static long st(const char *n)
{
	char path[128];
	long v = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/module/fakeavx/parameters/stat_%s", n);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static u8 bufp[64] __attribute__((aligned(64))), bufq[64] __attribute__((aligned(64)));
static unsigned long long rng = 0x9e3779b97f4a7c15ULL;

static void fill(void)
{
	for (int i = 0; i < 32; i++) {
		rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
		bufp[i] = rng >> 56;
		/* equal bytes in about half the places, so the masks vary */
		bufq[i] = (rng >> 40) & 1 ? bufp[i] : (u8)(rng >> 48);
	}
}

/* run each function n times on fresh inputs; returns the number of wrong results */
static int check(const char *what, int n, int direct)
{
	int bad = 0;

	for (int k = 0; k < n; k++) {
		u32 ga, gb, ea, eb;

		fill();
		ea = ref_a(bufp, bufq);
		eb = ref_b(bufp, bufq);
		ga = direct ? ev_ga(bufp, bufq) : ev_a(bufp, bufq);
		gb = direct ? ev_gb(bufp, bufq) : ev_b(bufp, bufq);
		if (ga != ea || gb != eb) {
			if (bad++ < 3)
				printf("  %s: WRONG %s a=%08x want %08x  b=%04x want %04x\n", what,
				       direct ? "(direct)" : "", ga, ea, gb, eb);
		}
	}
	return bad;
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 2000, bad = 0, status;
	int new_module = st("evicted") >= 0;
	long ev0 = st("evicted"), f0, f1, et0, et1, s0, s1;
	pid_t pid;

	if (st("fast") < 0) {
		printf("evict_test: fakeavx is not loaded\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	bad += check("warm-up", 3, 0);
	printf("  evicted by this process: %ld%s\n", new_module ? st("evicted") - ev0 : 0,
	       new_module ? "" : " (module without eviction)");

	et0 = st("evict_traps");
	bad += check("direct", rounds, 1);
	et1 = st("evict_traps");
	printf("  direct entries into evicted instructions: %d calls, %ld traps there\n",
	       2 * rounds, new_module ? et1 - et0 : 0);

	f0 = st("fast");
	bad += check("steady", rounds, 0);
	f1 = st("fast");
	printf("  steady state: %.2f repeated traps per call (2 functions per round)\n",
	       (double)(f1 - f0) / rounds / 2);

	fflush(stdout);
	s0 = st("slow");
	pid = fork();
	if (pid == 0) {
		long c0 = st("evict_traps");
		int b = check("child direct", rounds / 4 + 1, 1);

		if (new_module)
			printf("  child: %ld traps at evicted instructions (found through the records in the code cache)\n",
			       st("evict_traps") - c0);
		b += check("child", rounds / 4 + 1, 0);
		_exit(b ? 1 : 0);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid) {
		printf("  fork failed\n");
		return 1;
	}
	s1 = st("slow");
	if (WIFSIGNALED(status)) {
		printf("  child: KILLED by signal %d\n", WTERMSIG(status));
		bad++;
	} else if (WEXITSTATUS(status)) {
		printf("  child: wrong results\n");
		bad++;
	} else {
		printf("  child (fork, no records): results right, %ld slow traps\n", s1 - s0);
	}

	if (bad)
		printf("evict_test: FAIL (%d wrong)\n", bad);
	else if (new_module && f1 - f0 > rounds / 10)
		printf("evict_test: FAIL (results right, but the entries still trap)\n"), bad = 1;
	else
		printf("evict_test: ok%s\n", new_module ? "" : " (results; eviction not in this module)");
	return bad ? 1 : 0;
}
