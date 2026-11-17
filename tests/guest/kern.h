#include <stdint.h>
#include <stddef.h>
#define N 1024
struct data {
	float fa[N], fb[N], fc[N], fo[N];
	double da[N], db[N], dout[N];
	int32_t ia[N], ib[N], io[N];
	uint8_t ba[N], bb[N], bo[N];
	uint64_t qa[N], qo[N];
	int32_t idx[N];
	uint64_t sum;
};
#define KERNELS(P) \
	void P##_float(struct data *d); void P##_double(struct data *d); \
	void P##_int(struct data *d); void P##_bytes(struct data *d); \
	void P##_popcnt(struct data *d); void P##_simd(struct data *d); \
	void P##_mixsse(struct data *d);
KERNELS(ref)
KERNELS(avx)
