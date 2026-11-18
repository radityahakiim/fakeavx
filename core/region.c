// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx: region builder.
 *
 * Starting from one guest address, follow direct branches and collect the
 * instructions reached: AVX/POPCNT instructions (translated) and plain
 * integer instructions (relocated). The result is laid out as one piece of
 * code in which branches between collected instructions stay inside the
 * cache, so a loop is entered once and then never leaves until it is done.
 *
 * If the region contains a loop, xmm registers that none of its
 * instructions mention are borrowed for its duration: to hold the upper
 * halves of the ymm registers it does use, and as scratch. Entry stubs save
 * the borrowed registers and load the upper halves; every exit undoes that.
 */
#include "fakeavx.h"

static struct fa_ritem *find(struct fa_rgn *r, u64 ip)
{
	u32 i;

	for (i = 0; i < r->n; i++)
		if (r->it[i].ip == ip)
			return &r->it[i];
	return NULL;
}

/* movaps between an xmm register and a state block slot */
static int xmm_gs(u8 *p, int store, int reg, u32 off)
{
	int n = 0;

	p[n++] = 0x65;
	if (reg & 8)
		p[n++] = 0x44;
	p[n++] = 0x0f;
	p[n++] = store ? 0x29 : 0x28;
	p[n++] = ((reg & 7) << 3) | 4;
	p[n++] = 0x25;
	memcpy(p + n, &off, 4);
	return n + 4;
}

static int emit_enter(u8 *p, const struct fa_regmap *m)
{
	int n = 0, g;

	if (!m)
		return 0;
	for (g = 0; g < 16; g++) {
		if (m->hi[g] < 0)
			continue;
		n += xmm_gs(p + n, 1, m->hi[g], FA_ST_SAVE + 16 * m->hi[g]);
		n += xmm_gs(p + n, 0, m->hi[g], FA_ST_HI + 16 * g);
	}
	for (g = 0; g < m->nscratch; g++)
		n += xmm_gs(p + n, 1, m->scratch[g], FA_ST_SAVE + 16 * m->scratch[g]);
	return n;
}

static int emit_leave(u8 *p, const struct fa_regmap *m)
{
	int n = 0, g;

	if (!m)
		return 0;
	for (g = 0; g < 16; g++) {
		if (m->hi[g] < 0)
			continue;
		n += xmm_gs(p + n, 1, m->hi[g], FA_ST_HI + 16 * g);
		n += xmm_gs(p + n, 0, m->hi[g], FA_ST_SAVE + 16 * m->hi[g]);
	}
	for (g = 0; g < m->nscratch; g++)
		n += xmm_gs(p + n, 0, m->scratch[g], FA_ST_SAVE + 16 * m->scratch[g]);
	return n;
}

int fa_emit_entry(u8 *out, u64 at, const struct fa_regmap *map, u64 target)
{
	int n = emit_enter(out, map);

	return n + fa_emit_jmp(out + n, at + n, target);
}

static int reachable(struct fa_rgn *r, u64 target)
{
	s64 d = (s64)(target - r->out_addr);

	return d > -0x7f000000LL && d < 0x7f000000LL;
}

/* xmm registers an instruction names; *wide: it works on 256-bit values */
static u16 xmm_used(const struct fa_insn *in, int *wide, int no_sse41)
{
	const struct fa_op *op;
	u16 m;

	*wide = 0;
	if (in->kind == FA_K_SSE4) {
		/* crc32: general registers only (do_crc32 and the blob's entry) */
		if (in->map == 2 && (in->opcode == 0xf0 || in->opcode == 0xf1) && in->pp == 3)
			return 0;
		/*
		 * pcmpgtq, pcmpXstrX: the lean helper, which saves no xmm
		 * register and touches exactly these (stored and reloaded by
		 * the stub), so the others may be borrowed.
		 */
		if (fa_helper_lean(in))
			return fa_helper_xmm(in);
		if (no_sse41)
			return 0xffff;	/* SSE4.1 emulated by the full helper; keep it simple */
		/* runs natively (see do_native); rm may be a gpr: harmless */
		m = 1 << in->reg;
		if (in->mod == 3)
			m |= 1 << in->rm;
		if (in->map == 2 && (in->opcode == 0x10 || in->opcode == 0x14 || in->opcode == 0x15))
			m |= 1;		/* pblendvb, blendvps, blendvpd: xmm0 */
		return m;
	}
	if (in->kind != FA_K_VEX)
		return 0;
	op = fa_lookup(in->map, in->pp, in->opcode);
	if (op->shape == FA_S_ZERO)
		return in->L ? 0xffff : 0;	/* vzeroall / vzeroupper */
	*wide = in->L;
	m = (1 << in->reg) | (1 << in->vvvv);
	if (in->mod == 3)
		m |= 1 << in->rm;
	else if (in->mem.has_index)
		m |= 1 << in->mem.index;	/* VSIB index; a gpr otherwise */
	if (op->shape == FA_S_BLENDV)
		m |= 1 << (in->imm >> 4);
	return m;
}

static void discover(struct fa_rgn *r)
{
	u32 max = r->max_items && r->max_items < FA_RG_ITEMS ? r->max_items : FA_RG_ITEMS;
	u32 np = 0, head = 0;
	struct fa_env env = { 0 };

	env.out = r->tmp;
	env.cap = FA_MAX_STUB;
	env.out_addr = r->out_addr;
	env.blob_addr = r->blob_addr;
	env.no_jmp = 1;
	env.no_sse41 = r->no_sse41;

	r->n = 0;
	r->pend[np++] = r->entry_ip;
	while (head < np) {
		u64 ip = r->pend[head++];
		struct fa_ritem *prev = NULL;

		if (find(r, ip))
			continue;
		for (;;) {
			struct fa_ritem *it;
			struct fa_int ii;

			if (r->n >= max || find(r, ip))
				break;
			it = &r->it[r->n];
			memset(it, 0, sizeof(*it));
			it->ip = ip;
			it->nread = r->read(r->ctx, ip, it->bytes, 15);
			if (!it->nread)
				break;
			if (!fa_decode(it->bytes, it->nread, ip, &it->in) &&
			    it->in.kind != FA_K_XGETBV) {
				if (fa_translate(&it->in, &env) < 0)
					break;
				it->kind = FA_R_OURS;
				it->len = it->in.len;
			} else if (!fa_decode_int(it->bytes, it->nread, ip, &ii) ||
				   !fa_decode_sse(it->bytes, it->nread, ip, &ii, &it->xmm)) {
				if (ii.kind == FA_I_PLAIN && ii.disp_off && !reachable(r, ii.target))
					break;
				it->kind = ii.kind == FA_I_PLAIN ? FA_R_PLAIN :
					   ii.kind == FA_I_JCC ? FA_R_JCC : FA_R_JMP;
				it->len = ii.len;
				it->cc = ii.cc;
				it->disp_off = ii.disp_off;
				it->target = ii.target;
			} else {
				break;
			}
			r->n++;
			prev = it;
			ip += it->len;
			if (it->kind == FA_R_JCC || it->kind == FA_R_JMP) {
				s64 d = (s64)(it->target - r->entry_ip);

				if (np < FA_RG_PEND && d > -FA_RG_SPAN && d < FA_RG_SPAN)
					r->pend[np++] = it->target;
				if (it->kind == FA_R_JMP) {
					it->has_link = 1;
					it->link = it->target;
					prev = NULL;
					break;
				}
			}
		}
		/* the block stopped: whatever comes next is reached by a jump */
		if (prev) {
			prev->has_link = 1;
			prev->link = ip;
		}
	}
}

/* Decide which registers to borrow. */
static void make_map(struct fa_rgn *r)
{
	struct fa_regmap *m = &r->map;
	int cnt[16] = { 0 }, cycle = 0, nfree = 0, need = 2, g;
	u16 used = 0, avail;
	u8 freer[16];
	u32 i;

	memset(m, 0, sizeof(*m));
	memset(m->hi, -1, sizeof(m->hi));
	r->has_map = 0;
	if (r->no_map)
		return;

	for (i = 0; i < r->n; i++) {
		struct fa_ritem *it = &r->it[i], *t;

		if (it->kind == FA_R_PLAIN)
			used |= it->xmm;	/* legacy SSE copied as is */
		if (it->kind == FA_R_OURS) {
			int wide;
			u16 u = xmm_used(&it->in, &wide, r->no_sse41);

			if (fa_scratch_needed(&it->in, r->no_sse41) > need)
				need = fa_scratch_needed(&it->in, r->no_sse41);

			used |= u;
			if (wide)
				for (g = 0; g < 16; g++)
					if (u & (1 << g))
						cnt[g]++;
		}
		/* a jump to something already laid out closes a loop */
		if (it->kind == FA_R_JCC && (t = find(r, it->target)) && t <= it)
			cycle = 1;
		if (it->has_link && (t = find(r, it->link)) && t <= it)
			cycle = 1;
	}
	if (!cycle)
		return;
	avail = ~used;
	for (g = 15; g >= 0; g--)
		if (avail & (1 << g))
			freer[nfree++] = g;
	if (nfree < need)
		return;		/* not even the scratch registers: do not borrow */

	/*
	 * Scratch registers first, as many as the hungriest instruction's
	 * translation needs (2, or 3 for the inline helper replacements), so it
	 * never has to spill inside the loop and always finds a register even
	 * if the rest are all in use.
	 */
	g = 0;	/* next free register */
	while (g < nfree && m->nscratch < need)
		m->scratch[m->nscratch++] = freer[g++];
	while (g < nfree) {
		int best = -1, j;

		for (j = 0; j < 16; j++)
			if (cnt[j] && m->hi[j] < 0 && (best < 0 || cnt[j] > cnt[best]))
				best = j;
		if (best < 0)
			break;
		m->hi[best] = freer[g++];
	}
	while (g < nfree && m->nscratch < 3)
		m->scratch[m->nscratch++] = freer[g++];
	for (g = 0; g < 16; g++)
		if (m->hi[g] >= 0)
			m->reserved |= 1 << m->hi[g];
	for (g = 0; g < m->nscratch; g++)
		m->reserved |= 1 << m->scratch[g];
	r->has_map = 1;
}

static int emit_body(struct fa_rgn *r)
{
	const struct fa_regmap *map = r->has_map ? &r->map : NULL;
	struct fa_env env = { 0 };
	u32 len = 0, nfix = 0, nexit = 0, i, j;
	u8 *p = r->out;

	env.blob_addr = r->blob_addr;
	env.no_jmp = 1;
	env.map = map;
	env.no_sse41 = r->no_sse41;

	for (i = 0; i < r->n; i++) {
		struct fa_ritem *it = &r->it[i];

		if (len + FA_MAX_STUB + 16 > r->cap)
			return -1;
		it->off = len;
		switch (it->kind) {
		case FA_R_OURS: {
			int l;

			env.out = p + len;
			env.cap = FA_MAX_STUB;
			env.out_addr = r->out_addr + len;
			l = fa_translate(&it->in, &env);
			if (l < 0)
				return -1;
			len += l;
			break;
		}
		case FA_R_PLAIN:
			memcpy(p + len, it->bytes, it->len);
			if (it->disp_off) {
				s64 d = (s64)(it->target - (r->out_addr + len + it->len));
				s32 d32 = (s32)d;

				if (d != d32)
					return -1;
				memcpy(p + len + it->disp_off, &d32, 4);
			}
			len += it->len;
			break;
		case FA_R_JCC:
			p[len++] = 0x0f;
			p[len++] = 0x80 | it->cc;
			r->fix[nfix].pos = len;
			r->fix[nfix++].ip = it->target;
			len += 4;
			break;
		default:
			break;
		}
		if (it->has_link && !(i + 1 < r->n && r->it[i + 1].ip == it->link)) {
			p[len++] = 0xe9;
			r->fix[nfix].pos = len;
			r->fix[nfix++].ip = it->link;
			len += 4;
		}
	}

	for (i = 0; i < nfix; i++) {
		struct fa_ritem *t = find(r, r->fix[i].ip);
		u32 dest;
		s32 rel;

		if (t) {
			dest = t->off;
		} else {
			for (j = 0; j < nexit; j++)
				if (r->exits[j].ip == r->fix[i].ip)
					break;
			if (j == nexit) {
				if (nexit >= FA_RG_EXITS || len + FA_ENTRY_MAX + 40 > r->cap)
					return -1;
				r->exits[nexit].ip = r->fix[i].ip;
				r->exits[nexit++].off = len;
				len += emit_leave(p + len, map);
				if (r->exit_thunk) {
					/* mov dword %gs:GTMP, lo ; mov dword %gs:GTMP+4, hi */
					u32 w[2] = { (u32)r->fix[i].ip, (u32)(r->fix[i].ip >> 32) };
					int k;

					for (k = 0; k < 2; k++) {
						u32 off = FA_ST_GTMP + 4 * k;

						p[len++] = 0x65; p[len++] = 0xc7;
						p[len++] = 0x04; p[len++] = 0x25;
						memcpy(p + len, &off, 4);
						memcpy(p + len + 4, &w[k], 4);
						len += 8;
					}
					len += fa_emit_jmp(p + len, r->out_addr + len, r->exit_thunk);
				} else {
					len += fa_emit_jmp(p + len, r->out_addr + len, r->fix[i].ip);
				}
			}
			dest = r->exits[j].off;
		}
		rel = (s32)(dest - (r->fix[i].pos + 4));
		memcpy(p + r->fix[i].pos, &rel, 4);
	}
	r->len = len;
	return 0;
}

int fa_region_build(struct fa_rgn *r)
{
	discover(r);
	if (!r->n)
		return -1;
	make_map(r);
	return emit_body(r);
}
