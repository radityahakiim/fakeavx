/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fakeavx core: VEX/POPCNT decoder and AVX -> SSE4.1 translator.
 * OS-agnostic: builds in the kernel module, the helper blob and userspace tests.
 */
#ifndef FAKEAVX_H
#define FAKEAVX_H

#ifndef __ASSEMBLY__
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif
#endif /* !__ASSEMBLY__ */

/*
 * Per-thread state block, addressed through %gs.
 */
#define FA_ST_HI	0x000	/* 16 x 16 bytes: upper halves of ymm0..15      */
#define FA_ST_LO	0x100	/* 16 x 16 bytes: xmm save area for the helper  */
#define FA_ST_XTMP	0x200	/* 6 x 16 bytes: xmm scratch spill slots         */
#define FA_ST_GTMP	0x260	/* 4 x 8 bytes: gpr scratch spill slots          */
#define FA_ST_SELF	0x280	/* linear address of this block                  */
#define FA_ST_MAGIC	0x288
#define FA_ST_SAVE	0x300	/* 16 x 16 bytes: xmm registers borrowed by a region */
#define FA_ST_SIZE	0x1000
#define FA_MAGIC	0x31585641656b6146ULL	/* "FakeAVX1" */

/* Helper blob entry points (offsets from blob base). */
#define FA_BLOB_HELPER	0x00
#define FA_BLOB_POPCNT	0x08

#define FA_MAX_STUB	512	/* upper bound for one translated instruction */

#ifndef __ASSEMBLY__

enum fa_kind {
	FA_K_VEX = 1,
	FA_K_POPCNT,
	FA_K_XGETBV,
	FA_K_SSE42,	/* legacy-encoded pcmpgtq, crc32, pcmp[ei]str[im] */
};

struct fa_mem {
	u8 has_base, base;
	u8 has_index, index;	/* index may be a vector register (VSIB) */
	u8 scale;		/* 0..3 */
	u8 riprel;
	u8 seg;			/* 0, 0x64 or 0x65 */
	u8 addr32;
	s32 disp;
};

struct fa_insn {
	u8 kind;
	u8 len;
	u8 map;			/* 1 = 0F, 2 = 0F38, 3 = 0F3A */
	u8 pp;			/* 0 none, 1 = 66, 2 = F3, 3 = F2 */
	u8 L, W;
	u8 opcode;
	u8 vvvv;
	u8 mod, reg, rm;	/* reg/rm include the REX/VEX extension bit */
	u8 has_imm, imm;
	u8 opsize16;		/* POPCNT / CRC32 with 66 prefix */
	u8 rex;			/* a REX prefix was present (byte register naming) */
	struct fa_mem mem;
	u64 rip;		/* guest address of the instruction */
};

/*
 * Register mapping in force inside a translated region (see region.c).
 * Host xmm registers the region's guest code never mentions are borrowed:
 * some hold the upper half of a guest ymm register, some are free scratch.
 * Their own guest values sit in FA_ST_SAVE meanwhile.
 */
struct fa_regmap {
	s8 hi[16];		/* host xmm holding ymmN[255:128], or -1: in memory */
	u8 nscratch;
	u8 scratch[3];
	u16 reserved;		/* all borrowed host registers */
};

/* Translation environment. */
struct fa_env {
	u8 *out;		/* buffer receiving the stub              */
	u32 cap;
	u64 out_addr;		/* guest address the stub will execute at */
	u64 blob_addr;		/* guest address of the helper blob       */
	u8 no_jmp;		/* fall through instead of jumping back   */
	const struct fa_regmap *map;	/* NULL: no borrowed registers    */
};

/*
 * Decode one instruction. Returns 0 on success, <0 if this is not an
 * instruction fakeavx handles (the caller should let SIGILL happen).
 */
int fa_decode(const u8 *code, u32 avail, u64 rip, struct fa_insn *in);

/*
 * Translate a decoded instruction. The stub ends with a jump back to
 * rip + len, unless env->no_jmp is set: then execution falls off its end, so
 * stubs of consecutive instructions can be concatenated. Returns stub length, or <0 if unsupported.
 */
int fa_translate(const struct fa_insn *in, const struct fa_env *env);

/* Emit "jmp target" at guest address `at`. Returns its length (5 or 14). */
int fa_emit_jmp(u8 *out, u64 at, u64 target);
#define FA_JMP_MAX	14

/*
 * Integer instructions a region may carry along (intdec.c). Only
 * instructions that are positively recognised and that touch no vector
 * state are accepted; anything else ends the region.
 */
enum fa_ikind {
	FA_I_PLAIN,	/* copy verbatim (fixing up a rip-relative operand) */
	FA_I_JCC,	/* conditional branch, cc + target */
	FA_I_JMP,	/* unconditional direct jump */
};

struct fa_int {
	u8 kind;
	u8 len;
	u8 cc;
	u8 disp_off;	/* offset of a rip-relative disp32, 0 if none */
	u64 target;	/* branch target, or address the rip-relative operand names */
};

/* Returns 0 if the instruction at ip is one a region can relocate. */
int fa_decode_int(const u8 *code, u32 avail, u64 ip, struct fa_int *o);

/*
 * Regions (region.c): the code reachable from one entry point through direct
 * branches, translated as a unit so that loops run entirely in the cache.
 */
#define FA_RG_ITEMS	96	/* instructions per region          */
#define FA_RG_PEND	16	/* branch targets waiting to be explored */
#define FA_RG_EXITS	40
#define FA_RG_SPAN	4096	/* only follow branches this close to the entry */
#define FA_ENTRY_MAX	340	/* upper bound for one entry stub   */

enum { FA_R_OURS, FA_R_PLAIN, FA_R_JCC, FA_R_JMP };

struct fa_ritem {
	u64 ip;
	u64 target;	/* branch target, or rip-relative operand address        */
	u64 link;	/* if has_link: control continues at this guest address  */
	u32 off;	/* offset of this instruction's translation in the body  */
	u8 kind, len, cc, disp_off, has_link, nread;
	u8 bytes[15];
	struct fa_insn in;	/* FA_R_OURS only */
};

struct fa_rgn {
	/* in */
	int (*read)(void *ctx, u64 addr, u8 *buf, int n);	/* returns bytes read */
	void *ctx;
	u64 entry_ip;
	u8 *out;
	u32 cap;
	u64 out_addr, blob_addr;
	u64 exit_thunk;		/* tests only: leave through this address */
	u32 max_items;		/* 0: FA_RG_ITEMS */
	u8 no_map;		/* do not borrow registers */
	/* out */
	u32 len;		/* body length */
	u32 n;
	u8 has_map;
	struct fa_regmap map;
	struct fa_ritem it[FA_RG_ITEMS];
	/* scratch */
	u8 tmp[FA_MAX_STUB];
	u64 pend[FA_RG_PEND];
	struct { u32 pos; u64 ip; } fix[2 * FA_RG_ITEMS];
	struct { u64 ip; u32 off; } exits[FA_RG_EXITS];
};

/*
 * Build the body of a region. Code inside the body runs with r->map in
 * force; it must be entered through an entry stub (fa_emit_entry) aimed at
 * out_addr + it[i].off. Returns 0, or <0 if nothing could be built.
 */
int fa_region_build(struct fa_rgn *r);

/* Entry stub: borrow the map's registers, then jump to target. Returns length. */
int fa_emit_entry(u8 *out, u64 at, const struct fa_regmap *map, u64 target);

/* ---- op table, shared with the helper blob and the tests ---- */

enum fa_shape {
	FA_S_NONE = 0,
	FA_S_RVM,	/* dst = reg, src1 = vvvv, src2 = rm; lane-wise     */
	FA_S_RM,	/* dst = reg, src = rm; lane-wise unary             */
	FA_S_SHIFTI,	/* dst = vvvv, src = rm, /ext in reg, imm8          */
	FA_S_LOAD,	/* full-width move, reg <- rm                       */
	FA_S_STORE,	/* full-width move, rm <- reg                       */
	FA_S_PASS,	/* re-emit as legacy SSE unchanged                  */
	FA_S_MOVSS,	/* vmovss / vmovsd                                  */
	FA_S_MOVMSK,
	FA_S_ZERO,	/* vzeroupper / vzeroall                            */
	FA_S_CVT_WIDEN,	/* xmm/m128 -> ymm  (cvtps2pd, cvtdq2pd)            */
	FA_S_CVT_NARROW,/* ymm/m256 -> xmm  (cvtpd2ps, cvt(t)pd2dq)         */
	FA_S_BCAST,
	FA_S_PMOVX,
	FA_S_PTEST,
	FA_S_PERM2,
	FA_S_INSERT128,
	FA_S_EXTRACT128,
	FA_S_BLENDV,
	FA_S_PERMILPD,
	FA_S_PERMQ,
	FA_S_HELPER,	/* out-of-line C helper                             */
};

#define FA_F_COMM	0x0001	/* src1/src2 may be swapped                  */
#define FA_F_SCALAR	0x0002	/* only xmm is computed, upper half zeroed   */
#define FA_F_BGPR	0x0004	/* rm operand is a GPR / integer memory      */
#define FA_F_FOLD	0x0008	/* memory operand may be folded (no #GP)     */
#define FA_F_SPLIT4	0x0010	/* imm: bits 0-3 low lane, 4-7 high lane     */
#define FA_F_SPLIT2	0x0020	/* imm: bits 0-1 low lane, 2-3 high lane     */
#define FA_F_SPLIT3	0x0040	/* imm: bits 0-2 low lane, 3-5 high lane     */
#define FA_F_BSAME	0x0080	/* src2 is an xmm shift count for both lanes */
#define FA_F_W		0x0100	/* propagate VEX.W as REX.W                  */
#define FA_F_ZHI	0x0200	/* PASS: zero upper half of reg afterwards   */
#define FA_F_ZHIRM	0x0400	/* PASS: zero upper half of rm (if register) */
#define FA_F_L0		0x0800	/* only valid with VEX.L = 0                 */
#define FA_F_L1		0x1000	/* only valid with VEX.L = 1                 */
#define FA_F_MEMONLY	0x2000
#define FA_F_REGONLY	0x4000
#define FA_F_CMP	0x8000	/* imm is a compare predicate (0..31)        */

struct fa_op {
	u8 map, opcode;
	u8 ppmask;	/* bit n set: valid with pp == n */
	u8 shape;
	u16 flags;
	u8 has_imm;
	u8 alt_map, alt_pp, alt_opcode;	/* legacy op to emit if alt_map != 0 */
};

const struct fa_op *fa_lookup(u8 map, u8 pp, u8 opcode);

#endif /* !__ASSEMBLY__ */

#endif
