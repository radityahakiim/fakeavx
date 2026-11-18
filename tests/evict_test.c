// SPDX-License-Identifier: GPL-2.0
/*
 * Eviction test (kmod/main.c, fa_ev_window()), with the module loaded.
 *
 *   make -C tests evict_test && tests/evict_test [rounds]
 *
 * Runs the functions of evict_test.S (a non-PIE binary) on random inputs
 * and checks every result against C:
 *   1. warm-up through the normal entries (this is where eviction happens);
 *   2. steady state: repeated traps per call (0 with eviction, 1 without);
 *   3. jumps straight into evicted AVX instructions (they trap into their
 *      records and still compute the right thing);
 *   4. jumps straight into an evicted integer instruction until the
 *      eviction is undone, then the normal entry again;
 *   5. fork(): a child, which has the memory but none of the records, jumps
 *      into the evicted instructions first (and undoes the integer one it
 *      never saw), then uses the normal entries.
 * Exit status 0 = all results right (and, with a module that evicts, no
 * repeated traps at the entries and the integer eviction undone).
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
u32 ev_c(u32 x, u32 y);
u32 ev_gc(u32 x, u32 y);
u32 ev_d(u32 x, u32 y);
u32 ev_gd(u32 x, u32 y);
u32 ev_e(const u8 *p, const u8 *q);

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

static u32 pop(u32 x)
{
	u32 n = 0;

	for (; x; x &= x - 1)
		n++;
	return n;
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
static u32 vx, vy;
static unsigned long long rng = 0x9e3779b97f4a7c15ULL;

static void fill(void)
{
	for (int i = 0; i < 32; i++) {
		rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
		bufp[i] = rng >> 56;
		/* equal bytes in about half the places, so the masks vary */
		bufq[i] = (rng >> 40) & 1 ? bufp[i] : (u8)(rng >> 48);
	}
	vx = rng >> 13;
	vy = rng >> 29;
}

enum { F_AB = 1, F_C = 2, F_D = 4, F_E = 8, F_ALL = 15 };

/* run the chosen functions n times on fresh inputs; returns the number of wrong results */
static int check(const char *what, int n, int direct, int which)
{
	int bad = 0;

	for (int k = 0; k < n; k++) {
		u32 got[5] = { 0 }, want[5] = { 0 };

		fill();
		if (which & F_AB) {
			want[0] = ref_a(bufp, bufq);
			want[1] = ref_b(bufp, bufq);
			got[0] = direct ? ev_ga(bufp, bufq) : ev_a(bufp, bufq);
			got[1] = direct ? ev_gb(bufp, bufq) : ev_b(bufp, bufq);
		}
		if (which & F_C) {
			want[2] = pop(vx) + vy;
			got[2] = direct ? ev_gc(vx, vy) : ev_c(vx, vy);
		}
		if (which & F_D) {
			want[3] = pop(vy) - vx;
			got[3] = direct ? ev_gd(vx, vy) : ev_d(vx, vy);
		}
		if (which & F_E) {
			want[4] = ref_a(bufp, bufq) & 0xffff;
			got[4] = ev_e(bufp, bufq);
		}
		for (int j = 0; j < 5; j++)
			if (got[j] != want[j] && bad++ < 3)
				printf("  %s: WRONG %s function %c: %08x, want %08x\n", what,
				       direct ? "(direct)" : "", 'a' + j, got[j], want[j]);
	}
	return bad;
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 2000, bad = 0, status;
	int new_module = st("evicted") >= 0, int_module = st("evict_undone") >= 0;
	long ev0 = st("evicted"), ei0 = st("evicted_int"), f0, f1, et0, et1, s0, s1, u0, u1;
	pid_t pid;

	if (st("fast") < 0) {
		printf("evict_test: fakeavx is not loaded\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	bad += check("warm-up", 3, 0, F_ALL);
	printf("  evicted by this process: %ld%s", new_module ? st("evicted") - ev0 : 0,
	       new_module ? "" : " (module without eviction)");
	if (int_module)
		printf(", of them integer instructions: %ld", st("evicted_int") - ei0);
	printf("\n");

	f0 = st("fast");
	bad += check("steady", rounds, 0, F_ALL);
	f1 = st("fast");
	printf("  steady state: %.2f repeated traps per call (5 functions per round)\n",
	       (double)(f1 - f0) / rounds / 5);

	et0 = st("evict_traps");
	bad += check("direct", rounds, 1, F_AB);
	et1 = st("evict_traps");
	printf("  direct entries into evicted AVX instructions: %d calls, %ld traps there\n",
	       2 * rounds, new_module ? et1 - et0 : 0);

	et0 = st("evict_traps");
	u0 = st("evict_undone");
	bad += check("direct int", rounds, 1, F_C);
	u1 = st("evict_undone");
	et1 = st("evict_traps");
	bad += check("after undo", rounds, 0, F_C);
	if (int_module)
		printf("  direct entries into the evicted integer instruction: %d calls, %ld traps there, undone %ld time(s)\n",
		       rounds, et1 - et0, u1 - u0);

	fflush(stdout);
	s0 = st("slow");
	u0 = st("evict_undone");
	pid = fork();
	if (pid == 0) {
		long c0 = st("evict_traps");
		int b = check("child direct", rounds / 4 + 1, 1, F_AB | F_D);

		if (new_module)
			printf("  child: %ld traps at evicted instructions (found through the records in the code cache)\n",
			       st("evict_traps") - c0);
		b += check("child", rounds / 4 + 1, 0, F_ALL);
		_exit(b ? 1 : 0);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid) {
		printf("  fork failed\n");
		return 1;
	}
	s1 = st("slow");
	u1 = st("evict_undone");
	if (WIFSIGNALED(status)) {
		printf("  child: KILLED by signal %d\n", WTERMSIG(status));
		bad++;
	} else if (WEXITSTATUS(status)) {
		printf("  child: wrong results\n");
		bad++;
	} else {
		printf("  child (fork, no records): results right, %ld slow traps%s\n", s1 - s0,
		       int_module ? "" : "");
		if (int_module)
			printf("  child: integer eviction it never saw undone %ld time(s)\n", u1 - u0);
	}

	if (bad)
		printf("evict_test: FAIL (%d wrong)\n", bad);
	else if (new_module && f1 - f0 > rounds / 20)
		printf("evict_test: FAIL (results right, but the entries still trap)\n"), bad = 1;
	else
		printf("evict_test: ok%s\n", new_module ? "" : " (results; eviction not in this module)");
	return bad ? 1 : 0;
}
