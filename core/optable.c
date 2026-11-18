// SPDX-License-Identifier: GPL-2.0
/* fakeavx: table of supported VEX-encoded instructions. */
#include "fakeavx.h"

#define P0 1
#define P66 2
#define PF3 4
#define PF2 8

#define C FA_F_COMM
#define SC (FA_F_SCALAR | FA_F_FOLD)

#define OP(map, opc, pp, shape, flags) { map, opc, pp, shape, flags, 0, 0, 0, 0 }
#define OPI(map, opc, pp, shape, flags) { map, opc, pp, shape, flags, 1, 0, 0, 0 }
#define OPA(map, opc, pp, shape, flags, imm, am, ap, ao) { map, opc, pp, shape, flags, imm, am, ap, ao }

static const struct fa_op fa_ops[] = {
	/* ---------------- map 0F ---------------- */
	OP(1, 0x10, P0 | P66, FA_S_LOAD, 0),			/* movups/upd */
	OP(1, 0x10, PF3 | PF2, FA_S_MOVSS, 0),
	OP(1, 0x11, P0 | P66, FA_S_STORE, 0),
	OP(1, 0x11, PF3 | PF2, FA_S_MOVSS, 0),
	OP(1, 0x12, P0, FA_S_RVM, SC),				/* movlps / movhlps */
	OP(1, 0x12, P66, FA_S_RVM, SC | FA_F_MEMONLY),		/* movlpd */
	OP(1, 0x12, PF3, FA_S_RM, 0),				/* movsldup */
	OP(1, 0x12, PF2, FA_S_RM, FA_F_FOLD),			/* movddup */
	OP(1, 0x13, P0 | P66, FA_S_PASS, FA_F_MEMONLY),
	OP(1, 0x14, P0 | P66, FA_S_RVM, 0),			/* unpckl */
	OP(1, 0x15, P0 | P66, FA_S_RVM, 0),			/* unpckh */
	OP(1, 0x16, P0, FA_S_RVM, SC),				/* movhps / movlhps */
	OP(1, 0x16, P66, FA_S_RVM, SC | FA_F_MEMONLY),		/* movhpd */
	OP(1, 0x16, PF3, FA_S_RM, 0),				/* movshdup */
	OP(1, 0x17, P0 | P66, FA_S_PASS, FA_F_MEMONLY),
	OP(1, 0x28, P0 | P66, FA_S_LOAD, 0),
	OP(1, 0x29, P0 | P66, FA_S_STORE, 0),
	OP(1, 0x2a, PF3 | PF2, FA_S_RVM, SC | FA_F_BGPR | FA_F_W),	/* cvtsi2ss/sd */
	OP(1, 0x2b, P0 | P66, FA_S_STORE, FA_F_MEMONLY),	/* movntps/pd */
	OP(1, 0x2c, PF3 | PF2, FA_S_PASS, FA_F_W),
	OP(1, 0x2d, PF3 | PF2, FA_S_PASS, FA_F_W),
	OP(1, 0x2e, P0 | P66, FA_S_PASS, 0),			/* ucomis */
	OP(1, 0x2f, P0 | P66, FA_S_PASS, 0),			/* comis */
	OP(1, 0x50, P0 | P66, FA_S_MOVMSK, FA_F_REGONLY),
	OP(1, 0x51, P0 | P66, FA_S_RM, 0),			/* sqrt */
	OP(1, 0x51, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x52, P0, FA_S_RM, 0),				/* rsqrt */
	OP(1, 0x52, PF3, FA_S_RVM, SC),
	OP(1, 0x53, P0, FA_S_RM, 0),				/* rcp */
	OP(1, 0x53, PF3, FA_S_RVM, SC),
	OP(1, 0x54, P0 | P66, FA_S_RVM, C),			/* and */
	OP(1, 0x55, P0 | P66, FA_S_RVM, 0),			/* andn */
	OP(1, 0x56, P0 | P66, FA_S_RVM, C),			/* or */
	OP(1, 0x57, P0 | P66, FA_S_RVM, C),			/* xor */
	OP(1, 0x58, P0 | P66, FA_S_RVM, 0),			/* add */
	OP(1, 0x58, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x59, P0 | P66, FA_S_RVM, 0),			/* mul */
	OP(1, 0x59, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x5a, P0, FA_S_CVT_WIDEN, 0),			/* cvtps2pd */
	OP(1, 0x5a, P66, FA_S_CVT_NARROW, 0),			/* cvtpd2ps */
	OP(1, 0x5a, PF3 | PF2, FA_S_RVM, SC),			/* cvtss2sd/sd2ss */
	OP(1, 0x5b, P0 | P66 | PF3, FA_S_RM, 0),		/* cvtdq2ps etc */
	OP(1, 0x5c, P0 | P66, FA_S_RVM, 0),			/* sub */
	OP(1, 0x5c, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x5d, P0 | P66, FA_S_RVM, 0),			/* min */
	OP(1, 0x5d, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x5e, P0 | P66, FA_S_RVM, 0),			/* div */
	OP(1, 0x5e, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x5f, P0 | P66, FA_S_RVM, 0),			/* max */
	OP(1, 0x5f, PF3 | PF2, FA_S_RVM, SC),
	OP(1, 0x60, P66, FA_S_RVM, 0), OP(1, 0x61, P66, FA_S_RVM, 0),
	OP(1, 0x62, P66, FA_S_RVM, 0), OP(1, 0x63, P66, FA_S_RVM, 0),
	OP(1, 0x64, P66, FA_S_RVM, 0), OP(1, 0x65, P66, FA_S_RVM, 0),
	OP(1, 0x66, P66, FA_S_RVM, 0), OP(1, 0x67, P66, FA_S_RVM, 0),
	OP(1, 0x68, P66, FA_S_RVM, 0), OP(1, 0x69, P66, FA_S_RVM, 0),
	OP(1, 0x6a, P66, FA_S_RVM, 0), OP(1, 0x6b, P66, FA_S_RVM, 0),
	OP(1, 0x6c, P66, FA_S_RVM, 0), OP(1, 0x6d, P66, FA_S_RVM, 0),
	OP(1, 0x6e, P66, FA_S_PASS, FA_F_W | FA_F_ZHI | FA_F_L0),	/* movd/q load */
	OP(1, 0x6f, P66 | PF3, FA_S_LOAD, 0),
	OPI(1, 0x70, P66 | PF3 | PF2, FA_S_RM, 0),		/* pshufd/hw/lw */
	OPI(1, 0x71, P66, FA_S_SHIFTI, FA_F_REGONLY),
	OPI(1, 0x72, P66, FA_S_SHIFTI, FA_F_REGONLY),
	OPI(1, 0x73, P66, FA_S_SHIFTI, FA_F_REGONLY),
	OP(1, 0x74, P66, FA_S_RVM, C), OP(1, 0x75, P66, FA_S_RVM, C),
	OP(1, 0x76, P66, FA_S_RVM, C),
	OP(1, 0x77, P0, FA_S_ZERO, 0),
	OP(1, 0x7c, P66 | PF2, FA_S_RVM, 0),			/* hadd */
	OP(1, 0x7d, P66 | PF2, FA_S_RVM, 0),			/* hsub */
	OP(1, 0x7e, P66, FA_S_PASS, FA_F_W | FA_F_L0),		/* movd/q store */
	OP(1, 0x7e, PF3, FA_S_PASS, FA_F_ZHI | FA_F_L0),	/* movq xmm, xmm/m64 */
	OP(1, 0x7f, P66 | PF3, FA_S_STORE, 0),
	OP(1, 0xae, P0, FA_S_PASS, FA_F_MEMONLY | FA_F_L0),	/* ldmxcsr/stmxcsr */
	OPI(1, 0xc2, P0 | P66, FA_S_RVM, FA_F_CMP),
	OPI(1, 0xc2, PF3 | PF2, FA_S_RVM, SC | FA_F_CMP),
	OPI(1, 0xc4, P66, FA_S_RVM, SC | FA_F_BGPR | FA_F_L0),	/* pinsrw */
	OPI(1, 0xc5, P66, FA_S_PASS, FA_F_REGONLY | FA_F_L0),	/* pextrw */
	OPI(1, 0xc6, P0, FA_S_RVM, 0),				/* shufps */
	OPI(1, 0xc6, P66, FA_S_RVM, FA_F_SPLIT2),		/* shufpd */
	OP(1, 0xd0, P66 | PF2, FA_S_RVM, 0),			/* addsub */
	OP(1, 0xd1, P66, FA_S_RVM, FA_F_BSAME), OP(1, 0xd2, P66, FA_S_RVM, FA_F_BSAME),
	OP(1, 0xd3, P66, FA_S_RVM, FA_F_BSAME),
	OP(1, 0xd4, P66, FA_S_RVM, C), OP(1, 0xd5, P66, FA_S_RVM, C),
	OP(1, 0xd6, P66, FA_S_PASS, FA_F_ZHIRM | FA_F_L0),	/* movq store */
	OP(1, 0xd7, P66, FA_S_MOVMSK, FA_F_REGONLY),
	OP(1, 0xd8, P66, FA_S_RVM, 0), OP(1, 0xd9, P66, FA_S_RVM, 0),
	OP(1, 0xda, P66, FA_S_RVM, C), OP(1, 0xdb, P66, FA_S_RVM, C),
	OP(1, 0xdc, P66, FA_S_RVM, C), OP(1, 0xdd, P66, FA_S_RVM, C),
	OP(1, 0xde, P66, FA_S_RVM, C), OP(1, 0xdf, P66, FA_S_RVM, 0),
	OP(1, 0xe0, P66, FA_S_RVM, C),
	OP(1, 0xe1, P66, FA_S_RVM, FA_F_BSAME), OP(1, 0xe2, P66, FA_S_RVM, FA_F_BSAME),
	OP(1, 0xe3, P66, FA_S_RVM, C), OP(1, 0xe4, P66, FA_S_RVM, C),
	OP(1, 0xe5, P66, FA_S_RVM, C),
	OP(1, 0xe6, P66 | PF2, FA_S_CVT_NARROW, 0),		/* cvt(t)pd2dq */
	OP(1, 0xe6, PF3, FA_S_CVT_WIDEN, 0),			/* cvtdq2pd */
	OP(1, 0xe7, P66, FA_S_STORE, FA_F_MEMONLY),		/* movntdq */
	OP(1, 0xe8, P66, FA_S_RVM, 0), OP(1, 0xe9, P66, FA_S_RVM, 0),
	OP(1, 0xea, P66, FA_S_RVM, C), OP(1, 0xeb, P66, FA_S_RVM, C),
	OP(1, 0xec, P66, FA_S_RVM, C), OP(1, 0xed, P66, FA_S_RVM, C),
	OP(1, 0xee, P66, FA_S_RVM, C), OP(1, 0xef, P66, FA_S_RVM, C),
	OP(1, 0xf0, PF2, FA_S_LOAD, FA_F_MEMONLY),		/* lddqu */
	OP(1, 0xf1, P66, FA_S_RVM, FA_F_BSAME), OP(1, 0xf2, P66, FA_S_RVM, FA_F_BSAME),
	OP(1, 0xf3, P66, FA_S_RVM, FA_F_BSAME),
	OP(1, 0xf4, P66, FA_S_RVM, C), OP(1, 0xf5, P66, FA_S_RVM, C),
	OP(1, 0xf6, P66, FA_S_RVM, C),
	OP(1, 0xf7, P66, FA_S_PASS, FA_F_REGONLY | FA_F_L0),	/* maskmovdqu */
	OP(1, 0xf8, P66, FA_S_RVM, 0), OP(1, 0xf9, P66, FA_S_RVM, 0),
	OP(1, 0xfa, P66, FA_S_RVM, 0), OP(1, 0xfb, P66, FA_S_RVM, 0),
	OP(1, 0xfc, P66, FA_S_RVM, C), OP(1, 0xfd, P66, FA_S_RVM, C),
	OP(1, 0xfe, P66, FA_S_RVM, C),

	/* ---------------- map 0F38 ---------------- */
	OP(2, 0x00, P66, FA_S_RVM, 0), OP(2, 0x01, P66, FA_S_RVM, 0),
	OP(2, 0x02, P66, FA_S_RVM, 0), OP(2, 0x03, P66, FA_S_RVM, 0),
	OP(2, 0x04, P66, FA_S_RVM, 0), OP(2, 0x05, P66, FA_S_RVM, 0),
	OP(2, 0x06, P66, FA_S_RVM, 0), OP(2, 0x07, P66, FA_S_RVM, 0),
	OP(2, 0x08, P66, FA_S_RVM, 0), OP(2, 0x09, P66, FA_S_RVM, 0),
	OP(2, 0x0a, P66, FA_S_RVM, 0), OP(2, 0x0b, P66, FA_S_RVM, C),
	OP(2, 0x0c, P66, FA_S_HELPER, 0),			/* vpermilps var */
	OP(2, 0x0d, P66, FA_S_HELPER, 0),			/* vpermilpd var */
	OP(2, 0x0e, P66, FA_S_HELPER, 0),			/* vtestps */
	OP(2, 0x0f, P66, FA_S_HELPER, 0),			/* vtestpd */
	OP(2, 0x16, P66, FA_S_HELPER, FA_F_L1),			/* vpermps */
	OP(2, 0x17, P66, FA_S_PTEST, 0),
	OP(2, 0x18, P66, FA_S_BCAST, 0),			/* vbroadcastss */
	OP(2, 0x19, P66, FA_S_BCAST, FA_F_L1),			/* vbroadcastsd */
	OP(2, 0x1a, P66, FA_S_BCAST, FA_F_L1 | FA_F_MEMONLY),	/* vbroadcastf128 */
	OP(2, 0x1c, P66, FA_S_RM, 0), OP(2, 0x1d, P66, FA_S_RM, 0),
	OP(2, 0x1e, P66, FA_S_RM, 0),
	OP(2, 0x20, P66, FA_S_PMOVX, 0), OP(2, 0x21, P66, FA_S_PMOVX, 0),
	OP(2, 0x22, P66, FA_S_PMOVX, 0), OP(2, 0x23, P66, FA_S_PMOVX, 0),
	OP(2, 0x24, P66, FA_S_PMOVX, 0), OP(2, 0x25, P66, FA_S_PMOVX, 0),
	OP(2, 0x28, P66, FA_S_RVM, C), OP(2, 0x29, P66, FA_S_RVM, C),
	OP(2, 0x2a, P66, FA_S_LOAD, FA_F_MEMONLY),		/* movntdqa */
	OP(2, 0x2b, P66, FA_S_RVM, 0),
	OP(2, 0x2c, P66, FA_S_HELPER, FA_F_MEMONLY),		/* vmaskmovps load */
	OP(2, 0x2d, P66, FA_S_HELPER, FA_F_MEMONLY),
	OP(2, 0x2e, P66, FA_S_HELPER, FA_F_MEMONLY),		/* vmaskmovps store */
	OP(2, 0x2f, P66, FA_S_HELPER, FA_F_MEMONLY),
	OP(2, 0x30, P66, FA_S_PMOVX, 0), OP(2, 0x31, P66, FA_S_PMOVX, 0),
	OP(2, 0x32, P66, FA_S_PMOVX, 0), OP(2, 0x33, P66, FA_S_PMOVX, 0),
	OP(2, 0x34, P66, FA_S_PMOVX, 0), OP(2, 0x35, P66, FA_S_PMOVX, 0),
	OP(2, 0x36, P66, FA_S_HELPER, FA_F_L1),			/* vpermd */
	OP(2, 0x37, P66, FA_S_HELPER, 0),			/* vpcmpgtq */
	OP(2, 0x38, P66, FA_S_RVM, C), OP(2, 0x39, P66, FA_S_RVM, C),
	OP(2, 0x3a, P66, FA_S_RVM, C), OP(2, 0x3b, P66, FA_S_RVM, C),
	OP(2, 0x3c, P66, FA_S_RVM, C), OP(2, 0x3d, P66, FA_S_RVM, C),
	OP(2, 0x3e, P66, FA_S_RVM, C), OP(2, 0x3f, P66, FA_S_RVM, C),
	OP(2, 0x40, P66, FA_S_RVM, C),
	OP(2, 0x41, P66, FA_S_RM, FA_F_L0),			/* phminposuw */
	OP(2, 0x45, P66, FA_S_HELPER, 0),			/* vpsrlvd/q */
	OP(2, 0x46, P66, FA_S_HELPER, 0),			/* vpsravd */
	OP(2, 0x47, P66, FA_S_HELPER, 0),			/* vpsllvd/q */
	OP(2, 0x58, P66, FA_S_BCAST, 0),			/* vpbroadcastd */
	OP(2, 0x59, P66, FA_S_BCAST, 0),			/* vpbroadcastq */
	OP(2, 0x5a, P66, FA_S_BCAST, FA_F_L1 | FA_F_MEMONLY),	/* vbroadcasti128 */
	OP(2, 0x78, P66, FA_S_BCAST, 0),			/* vpbroadcastb */
	OP(2, 0x79, P66, FA_S_BCAST, 0),			/* vpbroadcastw */
	OP(2, 0x8c, P66, FA_S_HELPER, FA_F_MEMONLY),		/* vpmaskmov load */
	OP(2, 0x8e, P66, FA_S_HELPER, FA_F_MEMONLY),		/* vpmaskmov store */
	OP(2, 0x90, P66, FA_S_HELPER, FA_F_MEMONLY),		/* gathers */
	OP(2, 0x91, P66, FA_S_HELPER, FA_F_MEMONLY),
	OP(2, 0x92, P66, FA_S_HELPER, FA_F_MEMONLY),
	OP(2, 0x93, P66, FA_S_HELPER, FA_F_MEMONLY),

	/* ---------------- map 0F3A ---------------- */
	OPI(3, 0x00, P66, FA_S_PERMQ, FA_F_L1),			/* vpermq */
	OPI(3, 0x01, P66, FA_S_PERMQ, FA_F_L1),			/* vpermpd */
	OPA(3, 0x02, P66, FA_S_RVM, FA_F_SPLIT4, 1, 3, 1, 0x0c),	/* vpblendd -> blendps */
	OPA(3, 0x04, P66, FA_S_RM, 0, 1, 1, 1, 0x70),		/* vpermilps imm -> pshufd */
	OPI(3, 0x05, P66, FA_S_PERMILPD, 0),
	OPI(3, 0x06, P66, FA_S_PERM2, FA_F_L1),
	OPI(3, 0x08, P66, FA_S_RM, 0), OPI(3, 0x09, P66, FA_S_RM, 0),	/* roundps/pd */
	OPI(3, 0x0a, P66, FA_S_RVM, SC), OPI(3, 0x0b, P66, FA_S_RVM, SC),
	OPI(3, 0x0c, P66, FA_S_RVM, FA_F_SPLIT4),		/* blendps */
	OPI(3, 0x0d, P66, FA_S_RVM, FA_F_SPLIT2),		/* blendpd */
	OPI(3, 0x0e, P66, FA_S_RVM, 0),				/* pblendw */
	OPI(3, 0x0f, P66, FA_S_RVM, 0),				/* palignr */
	OPI(3, 0x14, P66, FA_S_PASS, FA_F_L0),			/* pextrb */
	OPI(3, 0x15, P66, FA_S_PASS, FA_F_L0),			/* pextrw */
	OPI(3, 0x16, P66, FA_S_PASS, FA_F_W | FA_F_L0),		/* pextrd/q */
	OPI(3, 0x17, P66, FA_S_PASS, FA_F_L0),			/* extractps */
	OPI(3, 0x18, P66, FA_S_INSERT128, FA_F_L1),
	OPI(3, 0x19, P66, FA_S_EXTRACT128, FA_F_L1),
	OPI(3, 0x20, P66, FA_S_RVM, SC | FA_F_BGPR | FA_F_L0),	/* pinsrb */
	OPI(3, 0x21, P66, FA_S_RVM, SC | FA_F_L0),		/* insertps */
	OPI(3, 0x22, P66, FA_S_RVM, SC | FA_F_BGPR | FA_F_W | FA_F_L0),
	OPI(3, 0x38, P66, FA_S_INSERT128, FA_F_L1),
	OPI(3, 0x39, P66, FA_S_EXTRACT128, FA_F_L1),
	OPI(3, 0x40, P66, FA_S_RVM, 0),				/* dpps */
	OPI(3, 0x41, P66, FA_S_RVM, FA_F_L0),			/* dppd */
	OPI(3, 0x42, P66, FA_S_RVM, FA_F_SPLIT3),		/* mpsadbw */
	OPI(3, 0x46, P66, FA_S_PERM2, FA_F_L1),
	OPI(3, 0x4a, P66, FA_S_BLENDV, 0),
	OPI(3, 0x4b, P66, FA_S_BLENDV, 0),
	OPI(3, 0x4c, P66, FA_S_BLENDV, 0),
	/* SSE4.2 string compares, VEX.128 only (VEX.vvvv must be 1111: decode.c) */
	OPI(3, 0x60, P66, FA_S_HELPER, FA_F_L0),		/* vpcmpestrm */
	OPI(3, 0x61, P66, FA_S_HELPER, FA_F_L0),		/* vpcmpestri */
	OPI(3, 0x62, P66, FA_S_HELPER, FA_F_L0),		/* vpcmpistrm */
	OPI(3, 0x63, P66, FA_S_HELPER, FA_F_L0),		/* vpcmpistri */
};

const struct fa_op *fa_lookup(u8 map, u8 pp, u8 opcode)
{
	u32 i;

	for (i = 0; i < sizeof(fa_ops) / sizeof(fa_ops[0]); i++) {
		const struct fa_op *o = &fa_ops[i];

		if (o->map == map && o->opcode == opcode && (o->ppmask & (1 << pp)))
			return o;
	}
	return NULL;
}

int fa_is_sse41(u8 map, u8 opcode)
{
	if (map == 2)
		return opcode == 0x10 || opcode == 0x14 || opcode == 0x15 || opcode == 0x17 ||
		       (opcode >= 0x20 && opcode <= 0x25) || (opcode >= 0x28 && opcode <= 0x2b) ||
		       (opcode >= 0x30 && opcode <= 0x35) || (opcode >= 0x38 && opcode <= 0x41);
	if (map == 3)
		return (opcode >= 0x08 && opcode <= 0x0e) || (opcode >= 0x14 && opcode <= 0x17) ||
		       (opcode >= 0x20 && opcode <= 0x22) || (opcode >= 0x40 && opcode <= 0x42);
	return 0;
}
