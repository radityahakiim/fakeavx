/* fakeavx end-to-end test: runs inside a machine whose CPU has no AVX. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include "kern.h"

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static void fill(struct data *d, uint64_t seed)
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
};
#define NK (int)(sizeof(kernels) / sizeof(kernels[0]))

/* returns the number of kernels whose AVX result differs from the reference */
static int run_all(uint64_t seed, int verbose)
{
	struct data *a = malloc(sizeof(*a)), *b = malloc(sizeof(*b));
	int bad = 0;

	for (int k = 0; k < NK; k++) {
		fill(a, seed + k);
		memcpy(b, a, sizeof(*a));
		kernels[k].ref(a);
		kernels[k].avx(b);
		if (memcmp(a, b, sizeof(*a))) {
			bad++;
			if (verbose)
				printf("  MISMATCH in kernel %s (seed %lu)\n", kernels[k].name, (unsigned long)seed);
		}
	}
	free(a);
	free(b);
	return bad;
}

static int repeats = 200;

static void *thread_fn(void *arg)
{
	long bad = 0;

	for (int i = 0; i < repeats / 4; i++)
		bad += run_all((uint64_t)(long)arg * 1000 + i, 0);
	return (void *)bad;
}

static int check(const char *what, int bad)
{
	printf("%-28s %s\n", what, bad ? "FAIL" : "ok");
	return bad;
}

int main(int argc, char **argv)
{
	int fails = 0, bad;
	pthread_t th[6];
	struct timespec t0, t1;
	pid_t pid;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (getenv("FA_REPEATS"))
		repeats = atoi(getenv("FA_REPEATS"));
	if (argc > 1 && !strcmp(argv[1], "xgetbv")) {
		unsigned a, d;

		__asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
		printf("xgetbv: XCR0 = %#x\n", a);
		return (a & 6) == 6 ? 0 : 1;
	}

	fails += check("single thread, first run", run_all(1, 1));
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bad = 0;
	for (int i = 0; i < repeats; i++)
		bad += run_all(100 + i, bad == 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fails += check("single thread, repeated", bad);
	printf("  (%.1f ms)\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

	bad = 0;
	for (long i = 0; i < 6; i++)
		pthread_create(&th[i], NULL, thread_fn, (void *)(i + 1));
	for (int i = 0; i < 6; i++) {
		void *r;

		pthread_join(th[i], &r);
		bad += (long)r;
	}
	fails += check("6 threads", bad);

	/* threads created in a burst after everything is already patched */
	bad = 0;
	for (int round = 0; round < (repeats >= 40 ? 20 : 4); round++) {
		for (long i = 0; i < 6; i++)
			pthread_create(&th[i], NULL, thread_fn, (void *)(i + 10 * round));
		for (int i = 0; i < 6; i++) {
			void *r;

			pthread_join(th[i], &r);
			bad += (long)r;
		}
	}
	fails += check("bursts of short-lived threads", bad);

	pid = fork();
	if (pid == 0)
		_exit(run_all(7777, 1) + (long)thread_fn((void *)99) ? 1 : 0);
	else {
		int st;

		waitpid(pid, &st, 0);
		fails += check("fork child", !WIFEXITED(st) || WEXITSTATUS(st));
	}
	fails += check("parent after fork", run_all(4242, 1));

	printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return fails ? 1 : 0;
}
