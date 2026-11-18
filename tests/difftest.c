// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx differential test. Must run on a CPU with real AVX2 + POPCNT.
 *
 * Every test instruction is executed twice from identical machine state:
 * natively, and through the fakeavx translation (SSE4.1-only code using the
 * %gs state block). Registers, flags, MXCSR and memory must match exactly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include "../core/fakeavx.h"
#include "../helpers/blob.h"

struct regs {
	u64 gpr[16];
	u64 flags;
	u32 mxcsr;
	u8 pad[52];
	u8 ymm[16][32];
};

void fa_run(struct regs *r, void *code);

#define V	0x00001		/* vvvv is an operand */
#define I	0x00002		/* imm8 */
#define L0	0x00004
#define L1	0x00008
#define LB	(L0 | L1)
#define W1	0x00010		/* W = 1 only */
#define WB	0x00020		/* both W */
#define RG	0x00040		/* reg field is a gpr */
#define MG	0x00080		/* rm register is a gpr */
#define NR	0x00100		/* no register form */
#define NM	0x00200		/* no memory form */
#define AL	0x00400		/* memory must be aligned */
#define I4	0x00800		/* imm[7:4] is a register */
#define VS	0x01000		/* VSIB */
#define MW	0x02000		/* memory size is 4 or 8 by W */
#define CMPI	0x04000		/* imm is a compare predicate */
#define NX	0x08000		/* do not compare MXCSR */
#define EX(n)	(((n) + 1) << 20)

struct spec {
	const char *name;
	u8 map, pp, opc;
	u32 fl;
	u8 ms0, ms1;	/* memory operand size for L=0 / L=1 */
};

#define P2(name, opc, fl, a, b) \
	{ name "ps", 1, 0, opc, fl, a, b }, { name "pd", 1, 1, opc, fl, a, b }
#define S2(name, opc, fl) \
	{ name "ss", 1, 2, opc, fl, 4, 0 }, { name "sd", 1, 3, opc, fl, 8, 0 }
#define INT(name, opc) { name, 1, 1, opc, V | LB, 16, 32 }
#define I38(name, opc) { name, 2, 1, opc, V | LB, 16, 32 }

static const struct spec specs[] = {
	P2("vmovu", 0x10, LB, 16, 32),
	{ "vmovss_ld", 1, 2, 0x10, L0 | NR, 4 }, { "vmovsd_ld", 1, 3, 0x10, L0 | NR, 8 },
	{ "vmovss_rr", 1, 2, 0x10, L0 | V | NM }, { "vmovsd_rr", 1, 3, 0x10, L0 | V | NM },
	P2("vmovu_st", 0x11, LB, 16, 32),
	{ "vmovss_st", 1, 2, 0x11, L0 | NR, 4 }, { "vmovsd_st", 1, 3, 0x11, L0 | NR, 8 },
	{ "vmovss_rr2", 1, 2, 0x11, L0 | V | NM }, { "vmovsd_rr2", 1, 3, 0x11, L0 | V | NM },
	{ "vmovlps", 1, 0, 0x12, V | L0 | NR, 8 }, { "vmovhlps", 1, 0, 0x12, V | L0 | NM },
	{ "vmovlpd", 1, 1, 0x12, V | L0 | NR, 8 },
	{ "vmovsldup", 1, 2, 0x12, LB, 16, 32 }, { "vmovddup", 1, 3, 0x12, LB, 8, 32 },
	{ "vmovlps_st", 1, 0, 0x13, L0 | NR, 8 }, { "vmovlpd_st", 1, 1, 0x13, L0 | NR, 8 },
	P2("vunpckl", 0x14, V | LB, 16, 32), P2("vunpckh", 0x15, V | LB, 16, 32),
	{ "vmovhps", 1, 0, 0x16, V | L0 | NR, 8 }, { "vmovlhps", 1, 0, 0x16, V | L0 | NM },
	{ "vmovhpd", 1, 1, 0x16, V | L0 | NR, 8 },
	{ "vmovshdup", 1, 2, 0x16, LB, 16, 32 },
	{ "vmovhps_st", 1, 0, 0x17, L0 | NR, 8 }, { "vmovhpd_st", 1, 1, 0x17, L0 | NR, 8 },
	P2("vmova", 0x28, LB | AL, 16, 32), P2("vmova_st", 0x29, LB | AL, 16, 32),
	{ "vcvtsi2ss", 1, 2, 0x2a, V | L0 | MG | WB | MW }, { "vcvtsi2sd", 1, 3, 0x2a, V | L0 | MG | WB | MW },
	P2("vmovnt", 0x2b, LB | NR | AL, 16, 32),
	{ "vcvttss2si", 1, 2, 0x2c, L0 | RG | WB, 4 }, { "vcvttsd2si", 1, 3, 0x2c, L0 | RG | WB, 8 },
	{ "vcvtss2si", 1, 2, 0x2d, L0 | RG | WB, 4 }, { "vcvtsd2si", 1, 3, 0x2d, L0 | RG | WB, 8 },
	{ "vucomiss", 1, 0, 0x2e, L0, 4 }, { "vucomisd", 1, 1, 0x2e, L0, 8 },
	{ "vcomiss", 1, 0, 0x2f, L0, 4 }, { "vcomisd", 1, 1, 0x2f, L0, 8 },
	P2("vmovmsk", 0x50, LB | RG | NM, 0, 0),
	P2("vsqrt", 0x51, LB, 16, 32), S2("vsqrt", 0x51, V | L0),
	{ "vrsqrtps", 1, 0, 0x52, LB, 16, 32 }, { "vrsqrtss", 1, 2, 0x52, V | L0, 4 },
	{ "vrcpps", 1, 0, 0x53, LB, 16, 32 }, { "vrcpss", 1, 2, 0x53, V | L0, 4 },
	P2("vand", 0x54, V | LB, 16, 32), P2("vandn", 0x55, V | LB, 16, 32),
	P2("vor", 0x56, V | LB, 16, 32), P2("vxor", 0x57, V | LB, 16, 32),
	P2("vadd", 0x58, V | LB, 16, 32), S2("vadd", 0x58, V | L0),
	P2("vmul", 0x59, V | LB, 16, 32), S2("vmul", 0x59, V | L0),
	{ "vcvtps2pd", 1, 0, 0x5a, LB, 8, 16 }, { "vcvtpd2ps", 1, 1, 0x5a, LB, 16, 32 },
	{ "vcvtss2sd", 1, 2, 0x5a, V | L0, 4 }, { "vcvtsd2ss", 1, 3, 0x5a, V | L0, 8 },
	{ "vcvtdq2ps", 1, 0, 0x5b, LB, 16, 32 }, { "vcvtps2dq", 1, 1, 0x5b, LB, 16, 32 },
	{ "vcvttps2dq", 1, 2, 0x5b, LB, 16, 32 },
	P2("vsub", 0x5c, V | LB, 16, 32), S2("vsub", 0x5c, V | L0),
	P2("vmin", 0x5d, V | LB, 16, 32), S2("vmin", 0x5d, V | L0),
	P2("vdiv", 0x5e, V | LB, 16, 32), S2("vdiv", 0x5e, V | L0),
	P2("vmax", 0x5f, V | LB, 16, 32), S2("vmax", 0x5f, V | L0),
	INT("vpunpcklbw", 0x60), INT("vpunpcklwd", 0x61), INT("vpunpckldq", 0x62),
	INT("vpacksswb", 0x63), INT("vpcmpgtb", 0x64), INT("vpcmpgtw", 0x65),
	INT("vpcmpgtd", 0x66), INT("vpackuswb", 0x67), INT("vpunpckhbw", 0x68),
	INT("vpunpckhwd", 0x69), INT("vpunpckhdq", 0x6a), INT("vpackssdw", 0x6b),
	INT("vpunpcklqdq", 0x6c), INT("vpunpckhqdq", 0x6d),
	{ "vmovd_ld", 1, 1, 0x6e, L0 | MG | WB | MW },
	{ "vmovdqa", 1, 1, 0x6f, LB | AL, 16, 32 }, { "vmovdqu", 1, 2, 0x6f, LB, 16, 32 },
	{ "vpshufd", 1, 1, 0x70, I | LB, 16, 32 }, { "vpshufhw", 1, 2, 0x70, I | LB, 16, 32 },
	{ "vpshuflw", 1, 3, 0x70, I | LB, 16, 32 },
	{ "vpsrlw_i", 1, 1, 0x71, V | I | LB | NM | EX(2) }, { "vpsraw_i", 1, 1, 0x71, V | I | LB | NM | EX(4) },
	{ "vpsllw_i", 1, 1, 0x71, V | I | LB | NM | EX(6) },
	{ "vpsrld_i", 1, 1, 0x72, V | I | LB | NM | EX(2) }, { "vpsrad_i", 1, 1, 0x72, V | I | LB | NM | EX(4) },
	{ "vpslld_i", 1, 1, 0x72, V | I | LB | NM | EX(6) },
	{ "vpsrlq_i", 1, 1, 0x73, V | I | LB | NM | EX(2) }, { "vpsrldq", 1, 1, 0x73, V | I | LB | NM | EX(3) },
	{ "vpsllq_i", 1, 1, 0x73, V | I | LB | NM | EX(6) }, { "vpslldq", 1, 1, 0x73, V | I | LB | NM | EX(7) },
	INT("vpcmpeqb", 0x74), INT("vpcmpeqw", 0x75), INT("vpcmpeqd", 0x76),
	{ "vhaddpd", 1, 1, 0x7c, V | LB, 16, 32 }, { "vhaddps", 1, 3, 0x7c, V | LB, 16, 32 },
	{ "vhsubpd", 1, 1, 0x7d, V | LB, 16, 32 }, { "vhsubps", 1, 3, 0x7d, V | LB, 16, 32 },
	{ "vmovd_st", 1, 1, 0x7e, L0 | MG | WB | MW },
	{ "vmovq_ld", 1, 2, 0x7e, L0, 8 },
	{ "vmovdqa_st", 1, 1, 0x7f, LB | AL, 16, 32 }, { "vmovdqu_st", 1, 2, 0x7f, LB, 16, 32 },
	{ "vstmxcsr", 1, 0, 0xae, L0 | NR | EX(3), 4 },
	P2("vcmp", 0xc2, V | I | LB | CMPI | NX, 16, 32), S2("vcmp", 0xc2, V | I | L0 | CMPI | NX),
	{ "vpinsrw", 1, 1, 0xc4, V | I | L0 | MG, 2 },
	{ "vpextrw", 1, 1, 0xc5, I | L0 | RG | NM },
	P2("vshuf", 0xc6, V | I | LB, 16, 32),
	{ "vaddsubpd", 1, 1, 0xd0, V | LB, 16, 32 }, { "vaddsubps", 1, 3, 0xd0, V | LB, 16, 32 },
	{ "vpsrlw", 1, 1, 0xd1, V | LB, 16, 16 }, { "vpsrld", 1, 1, 0xd2, V | LB, 16, 16 },
	{ "vpsrlq", 1, 1, 0xd3, V | LB, 16, 16 },
	INT("vpaddq", 0xd4), INT("vpmullw", 0xd5),
	{ "vmovq_st", 1, 1, 0xd6, L0, 8 },
	{ "vpmovmskb", 1, 1, 0xd7, LB | RG | NM },
	INT("vpsubusb", 0xd8), INT("vpsubusw", 0xd9), INT("vpminub", 0xda), INT("vpand", 0xdb),
	INT("vpaddusb", 0xdc), INT("vpaddusw", 0xdd), INT("vpmaxub", 0xde), INT("vpandn", 0xdf),
	INT("vpavgb", 0xe0),
	{ "vpsraw", 1, 1, 0xe1, V | LB, 16, 16 }, { "vpsrad", 1, 1, 0xe2, V | LB, 16, 16 },
	INT("vpavgw", 0xe3), INT("vpmulhuw", 0xe4), INT("vpmulhw", 0xe5),
	{ "vcvttpd2dq", 1, 1, 0xe6, LB, 16, 32 }, { "vcvtdq2pd", 1, 2, 0xe6, LB, 8, 16 },
	{ "vcvtpd2dq", 1, 3, 0xe6, LB, 16, 32 },
	{ "vmovntdq", 1, 1, 0xe7, LB | NR | AL, 16, 32 },
	INT("vpsubsb", 0xe8), INT("vpsubsw", 0xe9), INT("vpminsw", 0xea), INT("vpor", 0xeb),
	INT("vpaddsb", 0xec), INT("vpaddsw", 0xed), INT("vpmaxsw", 0xee), INT("vpxor", 0xef),
	{ "vlddqu", 1, 3, 0xf0, LB | NR, 16, 32 },
	{ "vpsllw", 1, 1, 0xf1, V | LB, 16, 16 }, { "vpslld", 1, 1, 0xf2, V | LB, 16, 16 },
	{ "vpsllq", 1, 1, 0xf3, V | LB, 16, 16 },
	INT("vpmuludq", 0xf4), INT("vpmaddwd", 0xf5), INT("vpsadbw", 0xf6),
	{ "vmaskmovdqu", 1, 1, 0xf7, L0 | NM },
	INT("vpsubb", 0xf8), INT("vpsubw", 0xf9), INT("vpsubd", 0xfa), INT("vpsubq", 0xfb),
	INT("vpaddb", 0xfc), INT("vpaddw", 0xfd), INT("vpaddd", 0xfe),

	I38("vpshufb", 0x00), I38("vphaddw", 0x01), I38("vphaddd", 0x02), I38("vphaddsw", 0x03),
	I38("vpmaddubsw", 0x04), I38("vphsubw", 0x05), I38("vphsubd", 0x06), I38("vphsubsw", 0x07),
	I38("vpsignb", 0x08), I38("vpsignw", 0x09), I38("vpsignd", 0x0a), I38("vpmulhrsw", 0x0b),
	I38("vpermilps_v", 0x0c), I38("vpermilpd_v", 0x0d),
	{ "vtestps", 2, 1, 0x0e, LB, 16, 32 }, { "vtestpd", 2, 1, 0x0f, LB, 16, 32 },
	{ "vpermps", 2, 1, 0x16, V | L1, 0, 32 },
	{ "vptest", 2, 1, 0x17, LB, 16, 32 },
	{ "vbroadcastss", 2, 1, 0x18, LB, 4, 4 }, { "vbroadcastsd", 2, 1, 0x19, L1, 0, 8 },
	{ "vbroadcastf128", 2, 1, 0x1a, L1 | NR, 0, 16 },
	{ "vpabsb", 2, 1, 0x1c, LB, 16, 32 }, { "vpabsw", 2, 1, 0x1d, LB, 16, 32 },
	{ "vpabsd", 2, 1, 0x1e, LB, 16, 32 },
	{ "vpmovsxbw", 2, 1, 0x20, LB, 8, 16 }, { "vpmovsxbd", 2, 1, 0x21, LB, 4, 8 },
	{ "vpmovsxbq", 2, 1, 0x22, LB, 2, 4 }, { "vpmovsxwd", 2, 1, 0x23, LB, 8, 16 },
	{ "vpmovsxwq", 2, 1, 0x24, LB, 4, 8 }, { "vpmovsxdq", 2, 1, 0x25, LB, 8, 16 },
	I38("vpmuldq", 0x28), I38("vpcmpeqq", 0x29),
	{ "vmovntdqa", 2, 1, 0x2a, LB | NR | AL, 16, 32 },
	I38("vpackusdw", 0x2b),
	{ "vmaskmovps_ld", 2, 1, 0x2c, V | LB | NR, 16, 32 }, { "vmaskmovpd_ld", 2, 1, 0x2d, V | LB | NR, 16, 32 },
	{ "vmaskmovps_st", 2, 1, 0x2e, V | LB | NR, 16, 32 }, { "vmaskmovpd_st", 2, 1, 0x2f, V | LB | NR, 16, 32 },
	{ "vpmovzxbw", 2, 1, 0x30, LB, 8, 16 }, { "vpmovzxbd", 2, 1, 0x31, LB, 4, 8 },
	{ "vpmovzxbq", 2, 1, 0x32, LB, 2, 4 }, { "vpmovzxwd", 2, 1, 0x33, LB, 8, 16 },
	{ "vpmovzxwq", 2, 1, 0x34, LB, 4, 8 }, { "vpmovzxdq", 2, 1, 0x35, LB, 8, 16 },
	{ "vpermd", 2, 1, 0x36, V | L1, 0, 32 },
	I38("vpcmpgtq", 0x37),
	I38("vpminsb", 0x38), I38("vpminsd", 0x39), I38("vpminuw", 0x3a), I38("vpminud", 0x3b),
	I38("vpmaxsb", 0x3c), I38("vpmaxsd", 0x3d), I38("vpmaxuw", 0x3e), I38("vpmaxud", 0x3f),
	I38("vpmulld", 0x40),
	{ "vphminposuw", 2, 1, 0x41, L0, 16 },
	{ "vpsrlv", 2, 1, 0x45, V | LB | WB, 16, 32 }, { "vpsravd", 2, 1, 0x46, V | LB, 16, 32 },
	{ "vpsllv", 2, 1, 0x47, V | LB | WB, 16, 32 },
	{ "vpbroadcastd", 2, 1, 0x58, LB, 4, 4 }, { "vpbroadcastq", 2, 1, 0x59, LB, 8, 8 },
	{ "vbroadcasti128", 2, 1, 0x5a, L1 | NR, 0, 16 },
	{ "vpbroadcastb", 2, 1, 0x78, LB, 1, 1 }, { "vpbroadcastw", 2, 1, 0x79, LB, 2, 2 },
	{ "vpmaskmov_ld", 2, 1, 0x8c, V | LB | NR | WB, 16, 32 },
	{ "vpmaskmov_st", 2, 1, 0x8e, V | LB | NR | WB, 16, 32 },
	{ "vpgatherd", 2, 1, 0x90, V | LB | WB | VS }, { "vpgatherq", 2, 1, 0x91, V | LB | WB | VS },
	{ "vgatherdp", 2, 1, 0x92, V | LB | WB | VS }, { "vgatherqp", 2, 1, 0x93, V | LB | WB | VS },

	{ "vpermq", 3, 1, 0x00, I | L1 | W1, 0, 32 }, { "vpermpd", 3, 1, 0x01, I | L1 | W1, 0, 32 },
	{ "vpblendd", 3, 1, 0x02, V | I | LB, 16, 32 },
	{ "vpermilps", 3, 1, 0x04, I | LB, 16, 32 }, { "vpermilpd", 3, 1, 0x05, I | LB, 16, 32 },
	{ "vperm2f128", 3, 1, 0x06, V | I | L1, 0, 32 },
	{ "vroundps", 3, 1, 0x08, I | LB, 16, 32 }, { "vroundpd", 3, 1, 0x09, I | LB, 16, 32 },
	{ "vroundss", 3, 1, 0x0a, V | I | L0, 4 }, { "vroundsd", 3, 1, 0x0b, V | I | L0, 8 },
	{ "vblendps", 3, 1, 0x0c, V | I | LB, 16, 32 }, { "vblendpd", 3, 1, 0x0d, V | I | LB, 16, 32 },
	{ "vpblendw", 3, 1, 0x0e, V | I | LB, 16, 32 }, { "vpalignr", 3, 1, 0x0f, V | I | LB, 16, 32 },
	{ "vpextrb", 3, 1, 0x14, I | L0 | MG, 1 }, { "vpextrw2", 3, 1, 0x15, I | L0 | MG, 2 },
	{ "vpextrd", 3, 1, 0x16, I | L0 | MG | WB | MW }, { "vextractps", 3, 1, 0x17, I | L0 | MG, 4 },
	{ "vinsertf128", 3, 1, 0x18, V | I | L1, 0, 16 }, { "vextractf128", 3, 1, 0x19, I | L1, 0, 16 },
	{ "vpinsrb", 3, 1, 0x20, V | I | L0 | MG, 1 }, { "vinsertps", 3, 1, 0x21, V | I | L0, 4 },
	{ "vpinsrd", 3, 1, 0x22, V | I | L0 | MG | WB | MW },
	{ "vinserti128", 3, 1, 0x38, V | I | L1, 0, 16 }, { "vextracti128", 3, 1, 0x39, I | L1, 0, 16 },
	{ "vdpps", 3, 1, 0x40, V | I | LB, 16, 32 }, { "vdppd", 3, 1, 0x41, V | I | L0, 16 },
	{ "vmpsadbw", 3, 1, 0x42, V | I | LB, 16, 32 },
	{ "vperm2i128", 3, 1, 0x46, V | I | L1, 0, 32 },
	{ "vblendvps", 3, 1, 0x4a, V | I | I4 | LB, 16, 32 }, { "vblendvpd", 3, 1, 0x4b, V | I | I4 | LB, 16, 32 },
	{ "vpblendvb", 3, 1, 0x4c, V | I | I4 | LB, 16, 32 },
};

/* ---- environment ---- */

static u8 *code;	/* RWX: blob, native buffer, stub buffer, rip-relative data */
static u8 *nat, *stub, *ripdata, *blobaddr;
static u8 *st;		/* %gs state block */
static u8 *data;	/* 2 data pages followed by a PROT_NONE guard page */
#define DATA_SZ 8192
#define RIPDATA_SZ 256

static u64 rs = 0x9e3779b97f4a7c15ULL;
static u64 rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return rs;
}

static const u32 fspecial[] = {
	0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000,
	0x7fa00000, 0x00000001, 0x807fffff, 0x7f7fffff, 0x40490fdb, 0x4f000000,
	0xcf000000, 0x3fc00000, 0x40200000, 0xc0200000, 0x42f6e979, 0xffc12345,
};
static const u64 dspecial[] = {
	0, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
	0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL,
	0x7ff4000000000000ULL, 1, 0x800fffffffffffffULL, 0x7fefffffffffffffULL,
	0x400921fb54442d18ULL, 0x41e0000000000000ULL, 0xc1e0000000000000ULL,
	0x3ff8000000000000ULL, 0x4004000000000000ULL, 0xc004000000000000ULL,
	0x47efffffe0000000ULL, 0x36a0000000000000ULL, 0xfff8000000012345ULL,
};

static void fill_vec(u8 *p, int n)
{
	/* 6-9: all zero, all ones, one half zero: ZF/CF = 1 for (v)ptest/vtest */
	int kind = rnd() % 10, i;

	for (i = 0; i < n; i += 8) {
		u64 v;

		switch (kind) {
		case 0: case 1:
			v = rnd();
			break;
		case 2: {
			u32 a = fspecial[rnd() % 19], b = fspecial[rnd() % 19];

			if (rnd() % 3 == 0) a = 0x3f000000 + (rnd() & 0x3ffffff);
			if (rnd() % 3 == 0) b = 0xc0000000 + (rnd() & 0x3ffffff);
			v = a | ((u64)b << 32);
			break;
		}
		case 3:
			v = dspecial[rnd() % 20];
			if (rnd() % 3 == 0)
				v = 0x3fe0000000000000ULL + (rnd() & 0x7fffffffffffffULL) + ((rnd() & 1) << 63);
			break;
		case 4:
			v = rnd() % 70;
			break;
		case 5:
			v = (rnd() % 40) | ((rnd() % 40) << 32);
			if (rnd() & 1) v |= 0x8000000080000000ULL & rnd();
			break;
		case 6:
			v = 0;
			break;
		case 7:
			v = ~0ULL;
			break;
		default:	/* 8: low 16 bytes zero, 9: the rest zero */
			v = (i < 16) == (kind == 8) ? 0 : rnd();
			break;
		}
		memcpy(p + i, &v, n - i < 8 ? n - i : 8);
	}
}

struct enc {
	u8 b[16];
	int len;
};

struct memop {
	int form;	/* 0 [rax] 1 [rbx+d8] 2 [r13+rcx*s+d32] 3 [r12] 4 [rip+d32] */
	u8 *addr;
};

/* build one VEX instruction; returns the offset of a rip-relative disp32 or -1 */
static int encode(struct enc *e, const struct spec *s, int L, int W, int reg, int vvvv,
		  int rmreg, const struct memop *m, int vsib_idx, int scale, int imm,
		  struct regs *r, int legacy)
{
	int n = 0, ripoff = -1;
	int R = reg >> 3, X = 0, B = 0;
	u8 modrm[8];
	int ml = 0;

	if (!m) {
		B = rmreg >> 3;
		modrm[ml++] = 0xc0 | ((reg & 7) << 3) | (rmreg & 7);
	} else if (vsib_idx >= 0) {
		X = vsib_idx >> 3;
		modrm[ml++] = 0x40 | ((reg & 7) << 3) | 4;
		modrm[ml++] = (scale << 6) | ((vsib_idx & 7) << 3) | 0;	/* base rax */
		modrm[ml++] = 0x10;
		r->gpr[0] = (u64)(m->addr - 0x10);
	} else {
		switch (m->form) {
		case 0:
			modrm[ml++] = ((reg & 7) << 3) | 0;
			r->gpr[0] = (u64)m->addr;
			break;
		case 1:
			modrm[ml++] = 0x40 | ((reg & 7) << 3) | 3;
			modrm[ml++] = 0xf0;
			r->gpr[3] = (u64)m->addr + 0x10;
			break;
		case 2: {
			s32 d = 0x1234;

			B = 1;
			modrm[ml++] = 0x80 | ((reg & 7) << 3) | 4;
			modrm[ml++] = (2 << 6) | (1 << 3) | 5;	/* r13 + rcx*4 */
			memcpy(modrm + ml, &d, 4);
			ml += 4;
			r->gpr[1] = 0x40;
			r->gpr[13] = (u64)m->addr - 0x100 - 0x1234;
			break;
		}
		case 3:
			B = 1;
			modrm[ml++] = ((reg & 7) << 3) | 4;
			modrm[ml++] = 0x24;
			r->gpr[12] = (u64)m->addr;
			break;
		default:
			modrm[ml++] = ((reg & 7) << 3) | 5;
			ripoff = ml;
			ml += 4;
			break;
		}
	}

	if (legacy) {
		/* 66 [REX] 0F 38/3A: the two-operand SSE4.1 form */
		e->b[n++] = 0x66;
		if (R || X || B || W)
			e->b[n++] = 0x40 | (W << 3) | (R << 2) | (X << 1) | B;
		e->b[n++] = 0x0f;
		e->b[n++] = s->map == 2 ? 0x38 : 0x3a;
	} else if (s->map == 1 && !X && !B && !W && (rnd() & 1)) {
		e->b[n++] = 0xc5;
		e->b[n++] = ((!R) << 7) | ((~vvvv & 15) << 3) | (L << 2) | s->pp;
	} else {
		e->b[n++] = 0xc4;
		e->b[n++] = ((!R) << 7) | ((!X) << 6) | ((!B) << 5) | s->map;
		e->b[n++] = (W << 7) | ((~vvvv & 15) << 3) | (L << 2) | s->pp;
	}
	e->b[n++] = s->opc;
	if (ripoff >= 0)
		ripoff += n;
	memcpy(e->b + n, modrm, ml);
	n += ml;
	if (s->fl & I)
		e->b[n++] = imm;
	e->len = n;
	if (ripoff >= 0) {
		s32 d = (s32)(m->addr - (nat + n));

		memcpy(e->b + ripoff, &d, 4);
	}
	return ripoff;
}

static const char *cur_name;
static struct enc cur_enc;
static int cur_phase;

static void crash(int sig, siginfo_t *si, void *uc)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "\nCRASH signal %d addr %p in %s phase of %s:",
			 sig, si->si_addr, cur_phase ? "translated" : "native", cur_name);
	int i;

	for (i = 0; i < cur_enc.len; i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %02x", cur_enc.b[i]);
	buf[n++] = '\n';
	if (write(2, buf, n)) {}
	_exit(2);
}

static void hexdump(const char *t, const u8 *p, int n)
{
	int i;

	printf("    %s", t);
	for (i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("\n");
}

static int nfail, ntests, nunsupported;

/* run one encoded instruction both ways and compare */
static int run_one(const char *name, const struct enc *e, const struct regs *init,
		   const u8 *datainit, const u8 *ripinit, u32 fl, u32 opmask)
{
	struct regs a = *init, b = *init;
	static u8 dsnap[DATA_SZ], rsnap[RIPDATA_SZ];
	struct fa_insn in;
	struct fa_env env = { 0 };
	struct fa_regmap map;
	u8 lows[16][16];
	int i, len, bad = 0;

	cur_name = name;
	cur_enc = *e;
	ntests++;

	/* native */
	memcpy(nat, e->b, e->len);
	nat[e->len] = 0xc3;
	memcpy(data, datainit, DATA_SZ);
	memcpy(ripdata, ripinit, RIPDATA_SZ);
	cur_phase = 0;
	fa_run(&a, nat);
	memcpy(dsnap, data, DATA_SZ);
	memcpy(rsnap, ripdata, RIPDATA_SZ);

	/* translated */
	if (fa_decode(e->b, e->len, (u64)nat, &in) < 0 || in.len != e->len) {
		printf("FAIL %s: decode failed\n", name);
		hexdump("insn ", e->b, e->len);
		nunsupported++;
		return 1;
	}
	env.out = stub;
	env.cap = FA_MAX_STUB;
	env.out_addr = (u64)stub;
	env.blob_addr = (u64)blobaddr;
	env.no_jmp = rnd() & 1;
	env.no_sse41 = !!getenv("FA_NOSSE41");
	/*
	 * Half of the time, pretend to be inside a region: registers the
	 * instruction does not mention are borrowed to hold upper halves of
	 * others, or as free scratch.
	 */
	memset(&map, 0, sizeof(map));
	memset(map.hi, -1, sizeof(map.hi));
	if (opmask != 0xffff && !getenv("FA_NOMAP") && (rnd() & 1)) {
		u8 freer[16];
		int nf = 0, g;

		for (i = 0; i < 16; i++)
			if (!(opmask & (1 << i)))
				freer[nf++] = i;
		for (i = nf - 1; i > 0; i--) {
			int j = rnd() % (i + 1);
			u8 t = freer[i];

			freer[i] = freer[j];
			freer[j] = t;
		}
		/* like region.c: the scratch registers come first */
		while (nf && map.nscratch < fa_scratch_needed(&in, env.no_sse41)) {
			map.scratch[map.nscratch] = freer[--nf];
			map.reserved |= 1 << map.scratch[map.nscratch++];
		}
		for (g = 0; g < 16 && nf; g++) {
			if (rnd() % ((opmask & (1 << g)) ? 3 : 6))
				continue;
			map.hi[g] = freer[--nf];
			map.reserved |= 1 << map.hi[g];
		}
		while (nf && map.nscratch < 3 && (rnd() & 1)) {
			map.scratch[map.nscratch] = freer[--nf];
			map.reserved |= 1 << map.scratch[map.nscratch++];
		}
		env.map = &map;
	}
	len = fa_translate(&in, &env);
	if (len >= 0 && env.no_jmp)
		len += fa_emit_jmp(stub + len, (u64)stub + len, (u64)nat + e->len);
	if (len < 0) {
		printf("FAIL %s: translate failed\n", name);
		hexdump("insn ", e->b, e->len);
		nunsupported++;
		return 1;
	}
	memcpy(data, datainit, DATA_SZ);
	memcpy(ripdata, ripinit, RIPDATA_SZ);
	for (i = 0; i < 16; i++)
		memcpy(st + FA_ST_HI + 16 * i, init->ymm[i] + 16, 16);
	memset(st + FA_ST_LO, 0xa5, 0x160);
	for (i = 0; i < 16; i++) {
		if (map.hi[i] < 0)
			continue;
		memcpy(b.ymm[map.hi[i]], init->ymm[i] + 16, 16);	/* cached upper half */
		memset(st + FA_ST_HI + 16 * i, 0x5a, 16);		/* stale by design */
	}
	cur_phase = 1;
	fa_run(&b, stub);
	for (i = 0; i < 16; i++)
		memcpy(lows[i], b.ymm[i], 16);
	for (i = 0; i < 16; i++) {
		if (map.hi[i] >= 0)
			memcpy(b.ymm[i] + 16, lows[map.hi[i]], 16);
		else
			memcpy(b.ymm[i] + 16, st + FA_ST_HI + 16 * i, 16);
		/* a borrowed register's own value is restored when the region is left */
		if (map.reserved & (1 << i))
			memcpy(b.ymm[i], init->ymm[i], 16);
	}

	for (i = 0; i < 16; i++) {
		if (i != 4 && a.gpr[i] != b.gpr[i]) {
			if (!bad++) printf("FAIL %s\n", name);
			printf("    gpr%d native %016lx translated %016lx\n", i,
			       (unsigned long)a.gpr[i], (unsigned long)b.gpr[i]);
		}
		if (memcmp(a.ymm[i], b.ymm[i], 32)) {
			if (!bad++) printf("FAIL %s\n", name);
			printf("    ymm%d\n", i);
			hexdump("init   ", init->ymm[i], 32);
			hexdump("native ", a.ymm[i], 32);
			hexdump("transl ", b.ymm[i], 32);
		}
	}
	if ((a.flags ^ b.flags) & 0x8d5) {
		if (!bad++) printf("FAIL %s\n", name);
		printf("    flags native %lx translated %lx\n",
		       (unsigned long)a.flags & 0x8d5, (unsigned long)b.flags & 0x8d5);
	}
	if (env.no_sse41 && in.kind == FA_K_VEX && in.map >= 2)
		fl |= NX;	/* the C implementations do not reproduce exception flags */
	if (!(fl & NX) && a.mxcsr != b.mxcsr) {
		if (!bad++) printf("FAIL %s\n", name);
		printf("    mxcsr native %x translated %x\n", a.mxcsr, b.mxcsr);
	}
	if (memcmp(dsnap, data, DATA_SZ) || memcmp(rsnap, ripdata, RIPDATA_SZ)) {
		if (!bad++) printf("FAIL %s\n", name);
		printf("    memory differs\n");
	}
	if (bad) {
		hexdump("insn ", e->b, e->len);
		hexdump("stub ", stub, len > 96 ? 96 : len);
		nfail++;
	}
	return bad;
}

static void init_regs(struct regs *r)
{
	int i;

	for (i = 0; i < 16; i++) {
		r->gpr[i] = rnd();
		if (rnd() % 4 == 0)
			r->gpr[i] &= 0xff;
		fill_vec(r->ymm[i], 32);
	}
	r->gpr[7] = (u64)data + 64;	/* maskmovdqu target */
	r->flags = 0x202 | (rnd() & 0x8d5);
	r->mxcsr = 0x1f80;
}

static int pick_reg(void)
{
	static const u8 fav[] = { 0, 1, 2, 3, 8, 15 };

	return (rnd() & 1) ? fav[rnd() % 6] : (int)(rnd() & 15);
}

static int pick_gpr(void)
{
	int r;

	do
		r = rnd() & 15;
	while (r == 4);
	return r;
}

static void test_spec(const struct spec *s, int iters)
{
	static u8 dinit[DATA_SZ], rinit[RIPDATA_SZ];
	int L, W, it, fails = 0;

	for (L = 0; L < 2; L++) {
		if (!(s->fl & (L ? L1 : L0)))
			continue;
		for (W = 0; W < 2; W++) {
			if (W && !(s->fl & (W1 | WB)))
				continue;
			if (!W && (s->fl & W1))
				continue;
			for (it = 0; it < iters && fails < 3; it++) {
				struct regs r;
				struct enc e;
				struct memop m, *mp = NULL;
				int reg, vvvv, rm, imm, ms, i, vs = -1, scale = 0;
				int ext = (s->fl >> 20) & 15;

				init_regs(&r);
				for (i = 0; i < DATA_SZ; i += 32)
					fill_vec(dinit + i, 32);
				for (i = 0; i < RIPDATA_SZ; i += 32)
					fill_vec(rinit + i, 32);

				reg = (s->fl & RG) ? pick_gpr() : pick_reg();
				if (ext)
					reg = ext - 1;
				vvvv = (s->fl & V) ? pick_reg() : 0;
				rm = (s->fl & MG) ? pick_gpr() : pick_reg();
				imm = rnd() & 0xff;
				if (s->fl & CMPI)
					imm &= 0x1f;
				if (s->fl & I4)
					imm = (pick_reg() << 4) | (imm & 15);
				ms = (s->fl & MW) ? (W ? 8 : 4) : (L ? s->ms1 : s->ms0);

				if (s->fl & VS) {
					do {
						reg = pick_reg();
						vvvv = pick_reg();
						vs = pick_reg();
					} while (reg == vvvv || reg == vs || vvvv == vs);
					fill_vec(r.ymm[vs], 0);
					for (i = 0; i < 32; i += 8) {
						u64 v = (s->opc & 1) ? rnd() % 60 :
							(rnd() % 60) | ((rnd() % 60) << 32);
						memcpy(r.ymm[vs] + i, &v, 8);
					}
					scale = rnd() % 4;
					m.form = 0;
					m.addr = data + 256;
					mp = &m;
				} else if (!(s->fl & NM) && ms &&
					   ((s->fl & NR) || (rnd() & 1))) {
					int al = (s->fl & AL) ? 32 : 1;

					m.form = rnd() % 5;
					if (m.form == 4) {
						m.addr = ripdata + 32 * (rnd() % 4);
						if (al == 1)
							m.addr += rnd() % 8;
					} else if (al == 1 && (rnd() & 1)) {
						m.addr = data + DATA_SZ - ms;	/* touch the guard page */
					} else {
						m.addr = data + 1024 + 32 * (rnd() % 64);
						if (al == 1)
							m.addr += rnd() % 16;
					}
					mp = &m;
				}
				encode(&e, s, L, W, reg, vvvv, rm, mp, vs, scale, imm, &r, 0);
				fails += run_one(s->name, &e, &r, dinit, rinit, s->fl,
						 (s->fl & (RG | MG | NM)) == (RG | MG | NM) ? 0xffff :
						 (1u << reg) | (1u << vvvv) | (1u << rm) |
						 ((s->fl & I4) ? 1u << (imm >> 4) : 0) |
						 (vs >= 0 ? 1u << vs : 0));
				/* the same operation in its legacy SSE4.1 encoding */
				/* (legacy packed memory operands must be aligned) */
				if (!L && s->pp == 1 && s->map >= 2 && fa_is_sse41(s->map, s->opc) &&
				    !(mp && ms >= 16 && ((u64)m.addr & 15))) {
					encode(&e, s, 0, W, reg, 0, rm, mp, -1, 0, imm, &r, 1);
					/* with SSE4.1 it is copied as is, so registers can be borrowed around it */
					fails += run_one(s->name, &e, &r, dinit, rinit, s->fl | NX,
							 getenv("FA_NOSSE41") ? 0xffff :
							 (1u << reg) | (mp ? 0 : 1u << rm));
				}
			}
		}
	}
}

static void test_special(int iters)
{
	static u8 dinit[DATA_SZ], rinit[RIPDATA_SZ];
	static const u8 zu[] = { 0xc5, 0xf8, 0x77 }, za[] = { 0xc5, 0xfc, 0x77 };
	int it, fails = 0;

	for (it = 0; it < iters && fails < 3; it++) {
		struct regs r;
		struct enc e;
		int n = 0, reg = pick_gpr(), rm = pick_gpr(), W = rnd() & 1, p66 = !W && (rnd() & 1);
		int mem = rnd() % 3;

		init_regs(&r);
		memcpy(e.b, (it & 1) ? zu : za, 3);
		e.len = 3;
		fails += run_one((it & 1) ? "vzeroupper" : "vzeroall", &e, &r, dinit, rinit, 0,
				 (it & 1) ? 0 : 0xffff);

		/* popcnt */
		init_regs(&r);
		fill_vec(dinit, 64);
		if (rnd() % 8 == 0) {
			r.gpr[rm] = 0;
			memset(dinit, 0, 64);
		}
		if (p66)
			e.b[n++] = 0x66;
		e.b[n++] = 0xf3;
		if (W || reg > 7 || (mem != 1 && rm > 7))
			e.b[n++] = 0x40 | (W << 3) | ((reg >> 3) << 2) | (mem != 1 ? rm >> 3 : 0);
		e.b[n++] = 0x0f;
		e.b[n++] = 0xb8;
		if (mem == 1) {
			e.b[n++] = ((reg & 7) << 3) | 0;	/* [rax] */
			r.gpr[0] = (u64)data + 8 * (rnd() % 4);
		} else if (mem == 2 && (rm & 7) != 4 && (rm & 7) != 5) {
			e.b[n++] = ((reg & 7) << 3) | (rm & 7);	/* [rm] */
			r.gpr[rm] = (u64)data + 8 * (rnd() % 4);
		} else {
			e.b[n++] = 0xc0 | ((reg & 7) << 3) | (rm & 7);
		}
		e.len = n;
		fails += run_one("popcnt", &e, &r, dinit, rinit, 0, 0);

		/* SSE4.2 in its legacy encoding: pcmpgtq, pcmpXstrX, crc32 */
		{
			static const char *names[] = { "pcmpgtq", "pcmpestrm", "pcmpestri",
						       "pcmpistrm", "pcmpistri", "crc32",
						       "pblendvb", "blendvps", "blendvpd" };
			static const char *vnames[] = { 0, "vpcmpestrm", "vpcmpestri", "vpcmpistrm", "vpcmpistri" };
			int vex = 0;
			static const u8 bv[] = { 0x10, 0x14, 0x15 };
			int which = rnd() % 9, xr = rnd() & 15, xm = rnd() & 15;
			int usemem = rnd() & 1, w = rnd() & 1, rex;

			init_regs(&r);
			for (n = 0; n < 64; n += 32)
				fill_vec(dinit + n, 32);
			if (which >= 1 && which <= 4) {
				/* strings: short alphabets so that matches and terminators occur */
				int k, wide = rnd() & 1;

				for (k = 0; k < 16; k++) {
					r.ymm[xr][k] = wide && (k & 1) ? 0 : rnd() % 5;
					r.ymm[xm][k] = wide && (k & 1) ? 0 : rnd() % 5;
					dinit[k] = wide && (k & 1) ? 0 : rnd() % 5;
				}
				r.gpr[0] = (u64)(s64)((int)(rnd() % 40) - 20);
				r.gpr[2] = (u64)(s64)((int)(rnd() % 40) - 20);
				if (rnd() % 16 == 0)
					r.gpr[0] = 0x80000000u;
			}
			n = 0;
			if (which == 5) {
				int byte = rnd() & 1, p16 = !byte && !w && (rnd() & 1);
				int gr = pick_gpr(), gm = pick_gpr();

				if (gm == 3)
					gm = 6;
				if (p16)
					e.b[n++] = 0x66;
				e.b[n++] = 0xf2;
				rex = 0x40 | (w << 3) | ((gr >> 3) << 2) | (usemem ? 0 : gm >> 3);
				if (rex != 0x40 || (rnd() & 1))
					e.b[n++] = rex;
				e.b[n++] = 0x0f; e.b[n++] = 0x38; e.b[n++] = byte ? 0xf0 : 0xf1;
				e.b[n++] = (usemem ? 0x00 : 0xc0) | ((gr & 7) << 3) | (usemem ? 3 : (gm & 7));
				r.gpr[3] = (u64)data + (rnd() % 8);
			} else if (which >= 1 && which <= 4 && (rnd() & 1)) {
				/* the VEX.128 form: c4 RXB.00011 W.1111.0.01 op modrm imm */
				vex = 1;
				e.b[n++] = 0xc4;
				e.b[n++] = 0x03 | (xr < 8 ? 0x80 : 0) | 0x40 | (usemem || xm < 8 ? 0x20 : 0);
				e.b[n++] = ((which <= 2 ? w : 0) << 7) | 0x79;
				e.b[n++] = 0x60 + which - 1;
				e.b[n++] = (usemem ? 0x00 : 0xc0) | ((xr & 7) << 3) | (usemem ? 3 : (xm & 7));
				e.b[n++] = rnd();
				r.gpr[3] = (u64)data + rnd() % 8;
			} else {
				if (which == 1 || which == 2)
					usemem = usemem && 1;
				e.b[n++] = 0x66;
				rex = 0x40 | ((which >= 1 && which <= 2 ? w : 0) << 3) | ((xr >> 3) << 2) |
				      (usemem ? 0 : xm >> 3);
				if (rex != 0x40)
					e.b[n++] = rex;
				e.b[n++] = 0x0f;
				e.b[n++] = (which && which < 6) ? 0x3a : 0x38;
				e.b[n++] = which >= 6 ? bv[which - 6] : which ? 0x60 + which - 1 : 0x37;
				e.b[n++] = (usemem ? 0x00 : 0xc0) | ((xr & 7) << 3) | (usemem ? 3 : (xm & 7));
				if (which && which < 6)
					e.b[n++] = rnd();
				/* legacy pcmpgtq needs an aligned memory operand */
				r.gpr[3] = (u64)data + ((which && which < 6) ? rnd() % 8 : 0);
			}
			e.len = n;
			/* blendv (which >= 6) is SSE4.1: copied as is on SSE4.1 hosts; it reads xmm0 */
			fails += run_one(vex ? vnames[which] : names[which], &e, &r, dinit, rinit, 0,
					 which >= 6 && !getenv("FA_NOSSE41") ?
					 1u | (1u << xr) | (usemem ? 0 : 1u << xm) : 0xffff);
		}
	}
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_sigaction = crash, .sa_flags = SA_SIGINFO };
	int iters = argc > 1 ? atoi(argv[1]) : 300;
	const char *only = argc > 2 ? argv[2] : NULL;
	u64 self;
	u32 i;

	if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("popcnt")) {
		fprintf(stderr, "this test needs a CPU with real AVX2 and POPCNT\n");
		return 77;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);

	code = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	st = mmap(NULL, FA_ST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	data = mmap(NULL, DATA_SZ + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED || st == MAP_FAILED || data == MAP_FAILED)
		return 1;
	mprotect(data + DATA_SZ, 4096, PROT_NONE);
	memcpy(code, fa_blob, sizeof(fa_blob));
	nat = code + 0x8000;
	ripdata = code + 0x8100;
	stub = code + 0x9000;
	blobaddr = code;
	if (getenv("FA_FAR")) {
		/* stubs more than 2 GB away from the guest code: exercises the
		 * rip-relative rewrite and the absolute jump back */
		u8 *far = mmap((void *)0x100000000000ULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

		if (far == MAP_FAILED)
			return 1;
		memcpy(far, fa_blob, sizeof(fa_blob));
		blobaddr = far;
		stub = far + 0x9000;
	}
	self = (u64)st;
	memcpy(st + FA_ST_SELF, &self, 8);
	self = FA_MAGIC;
	memcpy(st + FA_ST_MAGIC, &self, 8);
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, st)) {
		perror("arch_prctl");
		return 1;
	}

	for (i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
		if (only && !strstr(specs[i].name, only))
			continue;
		test_spec(&specs[i], iters);
	}
	if (!only || strstr("popcnt vzero sse42", only))
		test_special(iters * 4);

	printf("%d instruction instances tested, %d mismatches, %d unsupported\n",
	       ntests, nfail, nunsupported);
	return nfail || nunsupported ? 1 : 0;
}
