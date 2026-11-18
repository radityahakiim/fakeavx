// SPDX-License-Identifier: GPL-2.0
/*
 * fakeavx - AVX/AVX2/POPCNT for CPUs without them, by translating to SSE4.1.
 *
 * An unsupported instruction raises #UD. We hook the die notifier chain,
 * translate the instruction once into SSE-only code placed in a per-process
 * code cache, and resume the task there. Where possible the original
 * instruction is then overwritten with a jump to the translation, so it never
 * traps again.
 *
 * The kernel never executes or emulates guest SIMD code and never touches
 * user FPU state: it only decodes, emits code and redirects the task.
 */
#define pr_fmt(fmt) "fakeavx: " fmt

#include <linux/module.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/mmu_notifier.h>
#include <linux/hashtable.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/ptrace.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/uprobes.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/elf.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <asm/trapnr.h>
#include <asm/cpufeature.h>
#include <asm/processor-flags.h>

#include "compat.h"
#include "../core/fakeavx.h"
#include "../helpers/blob.h"

#define FA_REGION_SIZE	(4UL << 20)
#define FA_NEAR		0x7f000000L		/* rel32 reach, with margin */
#define FA_POOL_BLOCKS	64
#define FA_POOL_LOW	32
#define FA_MAX_REGIONS	2048
#define FA_BODY_MAX	16384			/* translated code of one region */

static int policy = 1;
module_param(policy, int, 0644);
MODULE_PARM_DESC(policy, "1: every process, unless it has FAKEAVX=0 in its environment (default); 0: only processes with FAKEAVX=1");

static bool autocpuid = true;
module_param(autocpuid, bool, 0644);
MODULE_PARM_DESC(autocpuid, "find the cpuid instructions of a binary by itself the first time it hits an unsupported instruction (default on)");

static bool no_sse41;	/* host has SSSE3 but not SSE4.1 */

static bool patch = true;
module_param(patch, bool, 0644);
MODULE_PARM_DESC(patch, "overwrite translated instructions with a jump so they stop trapping (default on)");

static bool divert = true;
module_param(divert, bool, 0644);
MODULE_PARM_DESC(divert, "patch the instruction before one that cannot be patched itself, if it keeps trapping (default on)");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "load even if the CPU already supports AVX (for testing only)");

static unsigned long stat_fast, stat_slow, stat_translated, stat_patched, stat_patched_short,
	stat_patched_int, stat_regions, stat_regions_borrowing, stat_unsupported,
	stat_short_refused, stat_unpatched, stat_diverted;
module_param(stat_fast, ulong, 0444);
module_param(stat_slow, ulong, 0444);
module_param(stat_translated, ulong, 0444);
module_param(stat_patched, ulong, 0444);
module_param(stat_patched_short, ulong, 0444);
module_param(stat_patched_int, ulong, 0444);
module_param(stat_regions, ulong, 0444);
module_param(stat_regions_borrowing, ulong, 0444);
module_param(stat_unsupported, ulong, 0444);
module_param(stat_short_refused, ulong, 0444);
module_param(stat_unpatched, ulong, 0444);
module_param(stat_diverted, ulong, 0444);

/* One translated guest instruction. stub == 0: known to be unsupported. */
struct fa_tr {
	u64 stub;
	u8 len;
	u8 bytes[15];		/* what guest memory holds at this address */
	u8 dv_tried;		/* fa_divert() has been tried */
	u32 unp;		/* its entry in /proc/fakeavx/unpatched, if any */
	atomic_long_t hits;	/* traps since then */
	struct rcu_head rcu;
};

/* Unpatched instructions that trap this often get fa_divert() tried. */
#define FA_DIVERT_HITS	16

struct fa_region {
	struct list_head list;
	unsigned long start, size, used;
	bool has_blob;
};

struct fa_pool {
	struct list_head list;
	unsigned long base;
	u64 free;
	pid_t owner[FA_POOL_BLOCKS];
};

struct fa_mm {
	struct hlist_node node;
	struct mm_struct *mm;
	struct mmu_notifier mn;
	unsigned long dead;
	bool enabled;
	u8 cpuid_mode;			/* 0: per binary, 1: never report AVX, 2: always */
	struct mutex lock;		/* translation, regions, patching */
	struct xarray tr;		/* guest rip -> struct fa_tr */
	struct list_head regions;
	int nregions;
	spinlock_t pool_lock;
	struct list_head pools;
	int nfree;
	struct work_struct free_work;
	struct work_struct refill_work;
	unsigned long refill_pending;
	u8 *buf;			/* FA_BODY_MAX bytes */
	struct fa_rgn *rg;
};

static DEFINE_HASHTABLE(fa_mms, 8);
static DEFINE_SPINLOCK(fa_mms_lock);
static DEFINE_MUTEX(fa_create_lock);
static struct workqueue_struct *fa_wq;
static atomic_t fa_inflight;

static void fa_note_file(unsigned long ip, bool vex);

/* ------------------------------------------------------------------ */
/* user GS base                                                        */

static unsigned long fa_gs_read(void)
{
	unsigned long v;

	if (!boot_cpu_has(X86_FEATURE_FSGSBASE))
		return current->thread.gsbase;
	fa_rdmsrq(MSR_KERNEL_GS_BASE, v);
	return v;
}

static void fa_gs_write(unsigned long v)
{
	preempt_disable();
	current->thread.gsbase = v;
	fa_wrmsrq(MSR_KERNEL_GS_BASE, v);
	preempt_enable();
}

/* ------------------------------------------------------------------ */
/* per-mm context                                                      */

static struct fa_mm *fa_mm_find(struct mm_struct *mm)
{
	struct fa_mm *fm;

	hash_for_each_possible_rcu(fa_mms, fm, node, (unsigned long)mm)
		if (fm->mm == mm)
			return fm;
	return NULL;
}

static void fa_mm_free(struct fa_mm *fm)
{
	struct fa_region *r, *rn;
	struct fa_pool *p, *pn;
	struct fa_tr *tr;
	unsigned long idx;

	cancel_work_sync(&fm->refill_work);
	mmu_notifier_unregister(&fm->mn, fm->mm);
	synchronize_rcu();
	xa_for_each(&fm->tr, idx, tr)
		kfree(tr);
	xa_destroy(&fm->tr);
	list_for_each_entry_safe(r, rn, &fm->regions, list)
		kfree(r);
	list_for_each_entry_safe(p, pn, &fm->pools, list)
		kfree(p);
	kvfree(fm->buf);
	kvfree(fm->rg);
	kfree(fm);
}

static void fa_mm_free_work(struct work_struct *w)
{
	fa_mm_free(container_of(w, struct fa_mm, free_work));
}

static void fa_mn_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct fa_mm *fm = container_of(mn, struct fa_mm, mn);

	if (test_and_set_bit(0, &fm->dead))
		return;
	spin_lock(&fa_mms_lock);
	hash_del_rcu(&fm->node);
	spin_unlock(&fa_mms_lock);
	queue_work(fa_wq, &fm->free_work);
}

static const struct mmu_notifier_ops fa_mn_ops = {
	.release = fa_mn_release,
};

/*
 * What does the environment the process was exec'ed with say?
 *   FAKEAVX=0     -1  leave this process alone
 *   (nothing)      0  up to the module's policy
 *   FAKEAVX=1      1  handle it; CPUID reports AVX if the binary is known to use it
 *   FAKEAVX=sse4   2  handle it; CPUID never reports AVX
 *   FAKEAVX=avx2   3  handle it; CPUID always reports AVX and AVX2
 */
static int fa_env_opt_in(struct mm_struct *mm)
{
	static const struct { const char *s; int v; } modes[] = {
		{ "FAKEAVX=0", -1 }, { "FAKEAVX=1", 1 }, { "FAKEAVX=sse4", 2 }, { "FAKEAVX=avx2", 3 },
	};
	unsigned long start = READ_ONCE(mm->env_start), end = READ_ONCE(mm->env_end);
	int found = 0, k;
	size_t len, i;
	char *buf;

	if (end <= start)
		return 0;
	len = min_t(size_t, end - start, 256 * 1024);
	buf = kvmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return 0;
	len = access_process_vm(current, start, buf, len, 0);
	buf[len] = 0;
	for (i = 0; i < len && !found; i += strlen(buf + i) + 1)
		for (k = 0; k < ARRAY_SIZE(modes); k++)
			if (!strcmp(buf + i, modes[k].s))
				found = modes[k].v;
	kvfree(buf);
	return found;
}

static int fa_pool_grow(struct fa_mm *fm);
static void fa_refill_work(struct work_struct *w);

static struct fa_mm *fa_mm_get(struct mm_struct *mm)
{
	struct fa_mm *fm;
	int i;

	rcu_read_lock();
	fm = fa_mm_find(mm);
	rcu_read_unlock();
	if (fm)
		return fm;

	mutex_lock(&fa_create_lock);
	rcu_read_lock();
	fm = fa_mm_find(mm);
	rcu_read_unlock();
	if (fm)
		goto out;
	fm = kzalloc(sizeof(*fm), GFP_KERNEL);
	if (!fm)
		goto out;
	fm->buf = kvmalloc(FA_BODY_MAX, GFP_KERNEL);
	fm->rg = kvzalloc(sizeof(*fm->rg), GFP_KERNEL);
	if (!fm->buf || !fm->rg) {
		kvfree(fm->buf);
		kvfree(fm->rg);
		kfree(fm);
		fm = NULL;
		goto out;
	}
	fm->mm = mm;
	mutex_init(&fm->lock);
	xa_init(&fm->tr);
	INIT_LIST_HEAD(&fm->regions);
	INIT_LIST_HEAD(&fm->pools);
	spin_lock_init(&fm->pool_lock);
	INIT_WORK(&fm->free_work, fa_mm_free_work);
	INIT_WORK(&fm->refill_work, fa_refill_work);
	i = fa_env_opt_in(mm);
	fm->enabled = i > 0 || (policy == 1 && i == 0);
	fm->cpuid_mode = i == 2 ? 1 : i == 3 ? 2 : 0;
	fm->mn.ops = &fa_mn_ops;
	if (mmu_notifier_register(&fm->mn, mm)) {
		kvfree(fm->buf);
		kvfree(fm->rg);
		kfree(fm);
		fm = NULL;
		goto out;
	}
	spin_lock(&fa_mms_lock);
	hash_add_rcu(fa_mms, &fm->node, (unsigned long)mm);
	spin_unlock(&fa_mms_lock);
out:
	mutex_unlock(&fa_create_lock);
	return fm;
}

/* ------------------------------------------------------------------ */
/* per-thread state blocks                                             */

/* current->mm must be fm->mm (or adopted through kthread_use_mm) */
static int fa_pool_grow(struct fa_mm *fm)
{
	struct fa_pool *p;
	unsigned long base;
	int i;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	base = vm_mmap(NULL, 0, FA_POOL_BLOCKS * FA_ST_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(base)) {
		kfree(p);
		return (int)base;
	}
	for (i = 0; i < FA_POOL_BLOCKS; i++) {
		u64 hdr[2] = { base + i * FA_ST_SIZE, FA_MAGIC };

		if (copy_to_user((void __user *)(base + i * FA_ST_SIZE + FA_ST_SELF),
				 hdr, sizeof(hdr))) {
			vm_munmap(base, FA_POOL_BLOCKS * FA_ST_SIZE);
			kfree(p);
			return -EFAULT;
		}
	}
	p->base = base;
	p->free = ~0ULL;
	spin_lock_irq(&fm->pool_lock);
	list_add(&p->list, &fm->pools);
	fm->nfree += FA_POOL_BLOCKS;
	spin_unlock_irq(&fm->pool_lock);
	return 0;
}

/* atomic context safe */
static unsigned long fa_pool_take(struct fa_mm *fm, pid_t owner)
{
	struct fa_pool *p;
	unsigned long flags, blk = 0;

	spin_lock_irqsave(&fm->pool_lock, flags);
	list_for_each_entry(p, &fm->pools, list) {
		if (p->free) {
			int i = __ffs64(p->free);

			p->free &= ~(1ULL << i);
			p->owner[i] = owner;
			fm->nfree--;
			blk = p->base + i * FA_ST_SIZE;
			break;
		}
	}
	spin_unlock_irqrestore(&fm->pool_lock, flags);
	return blk;
}

static bool fa_pool_owns(struct fa_mm *fm, unsigned long gs, pid_t owner, bool release)
{
	struct fa_pool *p;
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&fm->pool_lock, flags);
	list_for_each_entry(p, &fm->pools, list) {
		if (gs >= p->base && gs < p->base + FA_POOL_BLOCKS * FA_ST_SIZE &&
		    !((gs - p->base) % FA_ST_SIZE)) {
			int i = (gs - p->base) / FA_ST_SIZE;

			ret = !(p->free & (1ULL << i));
			if (ret && release && p->owner[i] == owner) {
				p->free |= 1ULL << i;
				fm->nfree++;
			}
			break;
		}
	}
	spin_unlock_irqrestore(&fm->pool_lock, flags);
	return ret;
}

static void fa_refill_work(struct work_struct *w)
{
	struct fa_mm *fm = container_of(w, struct fa_mm, refill_work);

	if (!test_bit(0, &fm->dead) && mmget_not_zero(fm->mm)) {
		kthread_use_mm(fm->mm);
		if (fm->nfree < FA_POOL_LOW)
			fa_pool_grow(fm);
		kthread_unuse_mm(fm->mm);
		mmput(fm->mm);
	}
	clear_bit(0, &fm->refill_pending);
}

/* Make sure the current thread has a state block behind %gs. */
static bool fa_thread_setup(struct fa_mm *fm)
{
	unsigned long gs = fa_gs_read();
	u64 hdr[2];

	if (fm->nfree < FA_POOL_LOW)
		fa_pool_grow(fm);
	if (gs) {
		if (fa_pool_owns(fm, gs, 0, false))
			return true;
		/* inherited over fork(): the block is a private copy */
		if (!copy_from_user(hdr, (void __user *)(gs + FA_ST_SELF), sizeof(hdr)) &&
		    hdr[0] == gs && hdr[1] == FA_MAGIC)
			return true;
		pr_info("%s[%d] uses %%gs itself, fakeavx disabled for it\n",
			current->comm, current->pid);
		fm->enabled = false;
		return false;
	}
	if (current->thread.gsindex)
		return false;
	gs = fa_pool_take(fm, current->pid);
	if (!gs)
		return false;
	fa_gs_write(gs);
	return true;
}

/*
 * A new thread must not share its creator's state block. Runs in atomic
 * context in the creator, before the new task first runs.
 */
static int fa_new_task(struct kprobe *kp, struct pt_regs *regs)
{
	struct task_struct *p = (struct task_struct *)regs->di;
	struct fa_mm *fm;

	if (!p || !p->mm || p->mm != current->mm || p->vfork_done)
		return 0;
	rcu_read_lock();
	fm = fa_mm_find(p->mm);
	if (fm && fm->enabled && fa_gs_read()) {
		unsigned long blk = fa_pool_take(fm, p->pid);

		if (!blk)
			pr_warn_ratelimited("%s[%d]: no free state block for new thread\n",
					    current->comm, current->pid);
		p->thread.gsbase = blk;
		if (fm->nfree < FA_POOL_LOW && !test_and_set_bit(0, &fm->refill_pending))
			queue_work(fa_wq, &fm->refill_work);
	}
	rcu_read_unlock();
	return 0;
}

static int fa_task_exit(struct kprobe *kp, struct pt_regs *regs)
{
	struct fa_mm *fm;
	unsigned long gs;

	if (!current->mm)
		return 0;
	gs = fa_gs_read();
	if (!gs)
		return 0;
	rcu_read_lock();
	fm = fa_mm_find(current->mm);
	if (fm)
		fa_pool_owns(fm, gs, current->pid, true);
	rcu_read_unlock();
	return 0;
}

static struct kprobe fa_kp_new = {
	.symbol_name = "wake_up_new_task",
	.pre_handler = fa_new_task,
};

static struct kprobe fa_kp_exit = {
	.symbol_name = "do_exit",
	.pre_handler = fa_task_exit,
};

/* ------------------------------------------------------------------ */
/* code cache                                                          */

static bool fa_near(unsigned long a, unsigned long b)
{
	long d = (long)(a - b);

	return d > -FA_NEAR && d < FA_NEAR;
}

/*
 * Room left above the heap for it to grow. Code cache regions can be placed
 * anywhere within reach, so they keep a wide berth. A short-patch window has
 * no choice: its place is dictated by the bytes after the instruction, and
 * the kernel puts the heap up to 1 GB above the program at random, so a
 * 1 GB gap refused most windows above the program in some runs and none in
 * others. If the heap ever grows into a window, brk() fails and malloc
 * carries on with mmap().
 */
#define FA_HEAP_GAP		(1UL << 30)
#define FA_HEAP_GAP_WINDOW	(16UL << 20)

/* Why an instruction that should have been patched was not (it then traps every time). */
enum {
	FW_OK, FW_HEAP, FW_STACK, FW_RANGE, FW_BUSY, FW_LIMIT, FW_NOMEM,
	FW_BORROWED, FW_BYTES, FW_REL, FW_POKE,
};

static const char *const fa_why[] = {
	[FW_OK] = "?",
	[FW_HEAP] = "its stub window is too close above the heap",
	[FW_STACK] = "its stub window is too close to the stack",
	[FW_RANGE] = "its stub window is outside the usable address range",
	[FW_BUSY] = "its stub window is taken by another mapping",
	[FW_LIMIT] = "the process has too many code cache regions",
	[FW_NOMEM] = "out of memory",
	[FW_BORROWED] = "a short patch just before it already uses its first bytes",
	[FW_BYTES] = "the bytes after it do not match the jump (they changed)",
	[FW_REL] = "its stub is out of jump range",
	[FW_POKE] = "the code could not be written",
};

/* Keep clear of where the heap and the stack may still want to grow. */
static int fa_addr_ok(struct mm_struct *mm, unsigned long a, unsigned long size,
		      unsigned long heap_gap)
{
	if (a < (1UL << 20) || a + size > TASK_SIZE_MAX - PAGE_SIZE || a + size < a)
		return FW_RANGE;
	if (a + size > mm->start_brk && a < mm->brk + heap_gap)
		return FW_HEAP;
	if (a + size > mm->start_stack - (1UL << 30) && a < mm->start_stack + (1UL << 20))
		return FW_STACK;
	return FW_OK;
}

/* addr == 0: anywhere. *why (optional) receives the reason for a failure. */
static struct fa_region *fa_region_map_why(struct fa_mm *fm, unsigned long addr,
					   unsigned long size, bool blob, int *why)
{
	unsigned long flags = MAP_PRIVATE | MAP_ANONYMOUS;
	struct fa_region *r;
	int w;

	if (fm->nregions >= FA_MAX_REGIONS) {
		if (why)
			*why = FW_LIMIT;
		return NULL;
	}
	if (addr) {
		w = fa_addr_ok(fm->mm, addr, size, blob ? FA_HEAP_GAP : FA_HEAP_GAP_WINDOW);
		if (w) {
			if (why)
				*why = w;
			return NULL;
		}
		flags |= MAP_FIXED_NOREPLACE;
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		if (why)
			*why = FW_NOMEM;
		return NULL;
	}
	addr = vm_mmap(NULL, addr, size, PROT_READ | PROT_EXEC, flags, 0);
	if (IS_ERR_VALUE(addr)) {
		if (why)
			*why = (long)addr == -EEXIST ? FW_BUSY : FW_NOMEM;
		kfree(r);
		return NULL;
	}
	if (blob && access_process_vm(current, addr, (void *)fa_blob, sizeof(fa_blob),
				      FOLL_WRITE | FOLL_FORCE) != sizeof(fa_blob)) {
		vm_munmap(addr, size);
		kfree(r);
		return NULL;
	}
	r->start = addr;
	r->size = size;
	r->has_blob = blob;
	if (why)
		*why = FW_OK;
	r->used = blob ? ALIGN(sizeof(fa_blob), 64) : 0;
	list_add(&r->list, &fm->regions);
	fm->nregions++;
	return r;
}

static struct fa_region *fa_region_map(struct fa_mm *fm, unsigned long addr,
				       unsigned long size, bool blob)
{
	return fa_region_map_why(fm, addr, size, blob, NULL);
}

/* First free stub address in r that lies in [lo, hi), or 0. */
static unsigned long fa_alloc_in(struct fa_region *r, unsigned long lo, unsigned long hi,
				 unsigned long size)
{
	unsigned long a = ALIGN(max(r->start + r->used, lo), 16);

	if (a >= hi || a + size > r->start + r->size)
		return 0;
	return a;
}

/* A region that starts with the helper blob, within rel32 reach of ip if possible. */
static struct fa_region *fa_region_get(struct fa_mm *fm, unsigned long ip)
{
	const unsigned long step = 32UL << 20;
	unsigned long base = ip & ~(step - 1);
	struct fa_region *r, *any = NULL;
	int i, j;

	list_for_each_entry(r, &fm->regions, list) {
		if (!r->has_blob || r->used + FA_BODY_MAX + FA_ENTRY_MAX * FA_RG_ITEMS > r->size)
			continue;
		if (fa_near(r->start, ip) && fa_near(r->start + r->size, ip))
			return r;
		any = r;
	}
	for (i = 1; i < 58; i++) {
		for (j = 0; j < 2; j++) {
			if (j == 0 && i * step + (1UL << 20) > base)
				continue;
			r = fa_region_map(fm, j ? base + i * step : base - i * step,
					  FA_REGION_SIZE, true);
			if (r)
				return r;
		}
	}
	/* nothing free nearby: anywhere will do, but such sites are never patched */
	return any ? any : fa_region_map(fm, 0, FA_REGION_SIZE, true);
}

/*
 * A stub address in [lo, hi), used for instructions shorter than a 5-byte
 * jump: the displacement bytes that would spill into the following
 * instruction are chosen to be the bytes that are already there, which
 * confines the stub to a 16 MB (4-byte instruction) or 64 KB (3-byte
 * instruction) window.
 */
static struct fa_region *fa_window_get(struct fa_mm *fm, unsigned long lo,
				       unsigned long hi, unsigned long *addr, int *why)
{
	struct fa_region *r;
	int k, w = FW_RANGE;

	list_for_each_entry(r, &fm->regions, list) {
		*addr = fa_alloc_in(r, lo, hi, FA_ENTRY_MAX);
		if (*addr)
			return r;
	}
	if (hi - lo > (1UL << 20)) {
		for (k = 0; k < 15; k++) {
			r = fa_region_map_why(fm, PAGE_ALIGN(lo) + ((unsigned long)k << 20),
					      256UL << 10, false, &w);
			if (r)
				goto got;
			if (w == FW_HEAP || w == FW_STACK || w == FW_RANGE || w == FW_LIMIT)
				break;	/* the rest of the window is no better */
		}
		*why = w;
		return NULL;
	}
	r = fa_region_map_why(fm, PAGE_ALIGN(lo), 64UL << 10, false, &w);
	if (!r)
		r = fa_region_map_why(fm, PAGE_ALIGN(lo), 8UL << 10, false, &w);
	if (!r) {
		*why = w;
		return NULL;
	}
got:
	*addr = fa_alloc_in(r, lo, hi, FA_ENTRY_MAX);
	if (!*addr)
		*why = FW_BUSY;
	return *addr ? r : NULL;
}

static int fa_poke(unsigned long addr, const void *buf, int len)
{
	return access_process_vm(current, addr, (void *)buf, len,
				 FOLL_WRITE | FOLL_FORCE) == len ? 0 : -EFAULT;
}

static void fa_nop(void *unused)
{
}

/* Only read-only file-backed text is patched: JITs may copy or checksum their code. */
static bool fa_patchable(struct fa_mm *fm, unsigned long ip, int len)
{
	struct vm_area_struct *vma;
	bool ok;

	mmap_read_lock(fm->mm);
	vma = vma_lookup(fm->mm, ip);
	ok = vma && vma->vm_file && !(vma->vm_flags & (VM_WRITE | VM_SHARED)) &&
	     ip + len <= vma->vm_end;
	mmap_read_unlock(fm->mm);
	return ok;
}

/*
 * Does a short patch just before ip borrow the first bytes of the instruction
 * at ip as part of its jump displacement? Such a patch is a jump located 3
 * or 4 bytes back that lands in one of our code cache regions (anonymous,
 * executable, not writable). This is derived from memory rather than
 * remembered, so it also holds for patches inherited over fork().
 */
static bool fa_borrowed(struct fa_mm *fm, unsigned long ip)
{
	bool ret = false;
	u8 b[8];
	int j;

	if (copy_from_user(b, (void __user *)(ip - 4), 8))
		return true;
	mmap_read_lock(fm->mm);
	for (j = 3; j <= 4 && !ret; j++) {
		struct vm_area_struct *vma;
		s32 rel;

		if (b[4 - j] != 0xe9)
			continue;
		memcpy(&rel, b + 5 - j, 4);
		vma = vma_lookup(fm->mm, ip - j + 5 + (long)rel);
		ret = vma && !vma->vm_file &&
		      (vma->vm_flags & (VM_EXEC | VM_WRITE)) == VM_EXEC;
	}
	mmap_read_unlock(fm->mm);
	return ret;
}

/*
 * Replace the guest instruction with "jmp stub".
 *
 * Other threads may be executing this code. The same scheme as the kernel's
 * text_poke_bp() is used: first make the instruction's leading byte one that
 * traps (a VEX prefix always raises #UD on this CPU, so VEX instructions are
 * already in that state), then write the displacement, then the jump opcode,
 * with a serializing IPI on every CPU between the steps. A thread that traps
 * in between is sent to the stub by the #UD handler. tr->bytes always
 * describes what memory currently holds.
 *
 * For instructions shorter than 5 bytes only the instruction's own bytes are
 * written; the caller placed the stub so that the rest of the displacement
 * equals the bytes that follow (see fa_window_get()). Nothing that overlaps
 * those borrowed bytes may be patched afterwards, or that jump would silently
 * change its target: see fa_borrowed().
 */
static int fa_patch_site(struct fa_mm *fm, struct fa_tr *tr, unsigned long ip,
			 const u8 *bytes)
{
	long rel = (long)(tr->stub - (ip + 5));
	s32 rel32 = (s32)rel;
	int plen = min_t(int, tr->len, 5);
	u8 jmp[5];
	u8 b;

	if (rel != rel32)
		return FW_REL;
	jmp[0] = 0xe9;
	memcpy(jmp + 1, &rel32, 4);
	if (memcmp(jmp + plen, bytes + plen, 5 - plen))
		return FW_BYTES;
	if (fa_borrowed(fm, ip))
		return FW_BORROWED;

	if (tr->bytes[0] != 0xc4 && tr->bytes[0] != 0xc5) {
		b = 0xc4;
		if (fa_poke(ip, &b, 1))
			return FW_POKE;
		tr->bytes[0] = b;
		on_each_cpu(fa_nop, NULL, 1);
	}
	if (fa_poke(ip + 1, jmp + 1, plen - 1))
		return FW_POKE;
	memcpy(tr->bytes + 1, jmp + 1, plen - 1);
	on_each_cpu(fa_nop, NULL, 1);
	if (fa_poke(ip, jmp, 1))
		return FW_POKE;
	tr->bytes[0] = jmp[0];
	on_each_cpu(fa_nop, NULL, 1);
	stat_patched++;
	if (tr->len < 5)
		stat_patched_short++;
	return FW_OK;
}

/*
 * An instruction meant to be patched was not: it will trap on every
 * execution. Say which one and why, in dmesg (rate limited) and in
 * /proc/fakeavx/unpatched (the last FA_UNP of them).
 */
#define FA_UNP	64

/* fa_divert() outcomes, after core/divert.c's own */
enum {
	FA_DV_FILE = FA_DV_AMBIG + 1, FA_DV_TAKEN, FA_DV_CHANGED, FA_DV_REGION, FA_DV_PATCH,
	FA_DV_N
};

static const char *const fa_dv_why[FA_DV_N] = {
	[FA_DV_OK] = "diverted",
	[FA_DV_NOSYNC] = "no function start found before it",
	[FA_DV_DESYNC] = "decoding from the function start does not reach it",
	[FA_DV_NOLONG] = "no instruction of 5 bytes or more leads straight to it",
	[FA_DV_AMBIG] = "function starts disagree on the instructions before it",
	[FA_DV_FILE] = "its file could not be read",
	[FA_DV_TAKEN] = "the instruction before it is translated already",
	[FA_DV_CHANGED] = "the code in memory differs from the file",
	[FA_DV_REGION] = "the code before it cannot be translated",
	[FA_DV_PATCH] = "the instruction before it could not be patched",
};

struct fa_unp {
	char comm[TASK_COMM_LEN];
	pid_t pid;
	char path[128];
	unsigned long off, ip, lo, hi, heap_lo, heap_hi;
	u8 bytes[15], len, why;
	u8 dv;			/* fa_divert() outcome + 1, 0: not tried */
	long dv_back;		/* bytes before ip it diverted at */
	u32 seq;
	atomic_long_t hits;
};

static struct fa_unp fa_unp[FA_UNP];
static u32 fa_unp_seq;
static DEFINE_SPINLOCK(fa_unp_lock);

/* Count a trap at an instruction reported unpatched. Lock-free, approximate. */
static void fa_unp_hit(u32 seq)
{
	struct fa_unp *u = &fa_unp[seq % FA_UNP];

	if (READ_ONCE(u->seq) == seq)
		atomic_long_inc(&u->hits);
}

static void fa_unp_divert(u32 seq, int dv, long back)
{
	struct fa_unp *u = &fa_unp[seq % FA_UNP];

	spin_lock(&fa_unp_lock);
	if (u->seq == seq) {
		u->dv = dv + 1;
		u->dv_back = back;
	}
	spin_unlock(&fa_unp_lock);
}

/* Returns the entry's sequence number, for fa_unp_hit(). */
static u32 fa_report_unpatched(struct fa_mm *fm, unsigned long ip, const u8 *bytes, int len,
				int why, unsigned long lo, unsigned long hi)
{
	struct vm_area_struct *vma;
	struct fa_unp u = { 0 };
	char *buf = kmalloc(PATH_MAX, GFP_KERNEL), *p = NULL;

	stat_unpatched++;
	get_task_comm(u.comm, current);
	u.pid = task_tgid_nr(current);
	u.ip = ip;
	u.len = min(len, 15);
	memcpy(u.bytes, bytes, u.len);
	u.why = why;
	u.lo = lo;
	u.hi = hi;
	mmap_read_lock(fm->mm);
	u.heap_lo = fm->mm->start_brk;
	u.heap_hi = fm->mm->brk;
	vma = vma_lookup(fm->mm, ip);
	if (vma && vma->vm_file) {
		u.off = ip - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT);
		if (buf)
			p = file_path(vma->vm_file, buf, PATH_MAX);
	} else {
		u.off = ip;
	}
	mmap_read_unlock(fm->mm);
	strscpy(u.path, !IS_ERR_OR_NULL(p) ? p : "?", sizeof(u.path));
	kfree(buf);

	spin_lock(&fa_unp_lock);
	if (!++fa_unp_seq)
		fa_unp_seq = 1;
	u.seq = fa_unp_seq;
	fa_unp[u.seq % FA_UNP] = u;
	spin_unlock(&fa_unp_lock);

	if (lo)
		pr_info_ratelimited("%s[%d] %s+%#lx: %d-byte instruction %*ph not patched, traps every time: %s (window %#lx-%#lx, heap %#lx-%#lx)\n",
				    u.comm, u.pid, u.path, u.off, u.len, u.len, u.bytes,
				    fa_why[why], lo, hi, u.heap_lo, u.heap_hi);
	else
		pr_info_ratelimited("%s[%d] %s+%#lx: %d-byte instruction %*ph not patched, traps every time: %s\n",
				    u.comm, u.pid, u.path, u.off, u.len, u.len, u.bytes,
				    fa_why[why]);
	return u.seq;
}

static int fa_unp_show(struct seq_file *m, void *v)
{
	unsigned int i, n;

	seq_puts(m, "# program[pid] file+offset bytes: traps since; reason [stub window] [heap]; diverted?\n");
	spin_lock(&fa_unp_lock);
	n = min(fa_unp_seq, (u32)FA_UNP);
	for (i = 0; i < n; i++) {
		const struct fa_unp *u = &fa_unp[(fa_unp_seq - n + 1 + i) % FA_UNP];

		seq_printf(m, "%s[%d] %s+%#lx %*ph: %ld traps; %s", u->comm, u->pid, u->path,
			   u->off, u->len, u->bytes, atomic_long_read(&u->hits), fa_why[u->why]);
		if (u->lo)
			seq_printf(m, " [window %#lx-%#lx] [heap %#lx-%#lx, window starts %+ld MB from heap end]",
				   u->lo, u->hi, u->heap_lo, u->heap_hi,
				   ((long)u->lo - (long)u->heap_hi) / (1L << 20));
		if (u->dv == FA_DV_OK + 1)
			seq_printf(m, "; diverted %ld bytes before it, at +%#lx", u->dv_back,
				   u->off - u->dv_back);
		else if (u->dv)
			seq_printf(m, "; not diverted: %s", fa_dv_why[u->dv - 1]);
		seq_putc(m, '\n');
	}
	spin_unlock(&fa_unp_lock);
	return 0;
}

static int fa_unp_open(struct inode *inode, struct file *file)
{
	return single_open(file, fa_unp_show, NULL);
}

static const struct proc_ops fa_unp_proc_ops = {
	.proc_open = fa_unp_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------ */
/* #UD handling                                                        */

static int fa_read_insn(unsigned long ip, u8 *buf)
{
	int n = 15;

	if (!copy_from_user(buf, (void __user *)ip, n))
		return n;
	n = PAGE_SIZE - (ip & ~PAGE_MASK);
	if (n >= 15 || copy_from_user(buf, (void __user *)ip, n))
		return 0;
	return n;
}

static struct fa_tr *fa_tr_store(struct fa_mm *fm, unsigned long ip, u64 stub,
				 const u8 *bytes, int len)
{
	struct fa_tr *tr = kzalloc(sizeof(*tr), GFP_KERNEL), *old;

	if (!tr)
		return NULL;
	tr->stub = stub;
	tr->len = len;
	memcpy(tr->bytes, bytes, len);
	old = xa_store(&fm->tr, ip, tr, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(tr);
		return NULL;
	}
	if (old)
		kfree_rcu(old, rcu);
	return tr;
}

static int fa_rd(void *ctx, u64 addr, u8 *buf, int n)
{
	return fa_read_insn(addr, buf);
}

/*
 * Give one guest instruction of a freshly built region its way in: an entry
 * stub (borrow registers, jump to the instruction's place in the body), a
 * translation record, and if possible a jump patched over the instruction.
 */
static struct fa_tr *fa_make_entry(struct fa_mm *fm, struct fa_region *main,
				   const struct fa_rgn *rg, const struct fa_ritem *it,
				   bool may_patch)
{
	const struct fa_regmap *map = rg->has_map ? &rg->map : NULL;
	unsigned long ip = it->ip, target = rg->out_addr + it->off, addr = 0;
	bool want_patch = may_patch && patch && it->nread >= 5 && it->len >= 3 &&
			  fa_patchable(fm, ip, it->len);
	struct fa_region *r = NULL;
	u8 stub[FA_ENTRY_MAX], cur[5];
	struct fa_tr *tr;
	int n, why = FW_OK;
	u32 unp = 0;

	/* what follows may have been patched since the region was read */
	if (want_patch && copy_from_user(cur, (void __user *)ip, 5))
		want_patch = false;

	if (want_patch && it->len < 5) {
		/* displacement bytes fixed by what follows the instruction */
		int shift = 8 * (it->len - 1);
		u32 fixed = 0;
		unsigned long lo;

		memcpy(&fixed, cur + it->len, 5 - it->len);
		lo = ip + 5 + (long)(s32)(fixed << shift);
		r = fa_window_get(fm, lo, lo + (1UL << shift), &addr, &why);
		if (!r && it->kind == FA_R_OURS) {
			stat_short_refused++;	/* will trap on every execution */
			unp = fa_report_unpatched(fm, ip, it->bytes, it->len, why, lo,
						  lo + (1UL << shift));
		}
		if (!r)
			want_patch = false;
	}
	if (!r && !map) {
		addr = target;		/* nothing to set up: enter the body directly */
	} else {
		if (!r) {
			r = main;
			addr = fa_alloc_in(r, 0, ULONG_MAX, FA_ENTRY_MAX);
			if (!addr)
				return NULL;
		}
		n = fa_emit_entry(stub, addr, map, target);
		if (fa_poke(addr, stub, n))
			return NULL;
		r->used = addr + ALIGN(n, 16) - r->start;
	}
	tr = fa_tr_store(fm, ip, addr, it->bytes, it->len);
	if (!tr)
		return NULL;
	if (want_patch) {
		why = fa_patch_site(fm, tr, ip, cur);
		/* an integer instruction that is not patched just keeps running as it is */
		if (why && it->kind == FA_R_OURS)
			unp = fa_report_unpatched(fm, ip, it->bytes, it->len, why, 0, 0);
	}
	tr->unp = unp;
	return tr;
}

static bool fa_is_ours(unsigned long ip)
{
	struct fa_insn in;
	u8 buf[16];
	int n = fa_read_insn(ip, buf);

	return n && !fa_decode(buf, n, ip, &in) && in.kind != FA_K_XGETBV;
}

/* Does the first block of the region run straight from its entry into ip? */
static bool fa_rgn_reaches(const struct fa_rgn *rg, unsigned long ip)
{
	int k;

	for (k = 0; k < rg->n; k++) {
		const struct fa_ritem *it = &rg->it[k];

		if (it->ip == ip)
			return it->kind == FA_R_OURS;
		if (it->has_link || k + 1 == rg->n || it->ip + it->len != rg->it[k + 1].ip)
			return false;
	}
	return false;
}

/*
 * Translate the code around ip as a region (see core/region.c) and hook its
 * instructions up. Returns the translation record for ip.
 *
 * With via, the region starts there instead, at an instruction that falls
 * through to ip, which is translated already (see fa_divert()). Returns the
 * record for via, or NULL if the region does not run into ip.
 */
static struct fa_tr *fa_translate_region(struct fa_mm *fm, unsigned long ip,
					 const u8 *buf, int nread, unsigned long via)
{
	static const struct { u8 max_items, no_map; } tries[] = {
		{ 0, 0 }, { 0, 1 }, { 1, 1 },
	};
	struct fa_region *main = fa_region_get(fm, ip);
	struct fa_rgn *rg = fm->rg;
	u8 order[FA_RG_ITEMS];
	unsigned long addr, fresh = via ? 0 : ip;	/* gets an entry even if known */
	int i, j, cand, ret = -1;

	if (!main)
		return NULL;
	addr = fa_alloc_in(main, 0, ULONG_MAX, FA_BODY_MAX);
	if (!addr)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(tries) && ret < 0; i++) {
		rg->read = fa_rd;
		rg->ctx = NULL;
		rg->entry_ip = via ?: ip;
		rg->out = fm->buf;
		rg->cap = FA_BODY_MAX;
		rg->out_addr = addr;
		rg->blob_addr = main->start;
		rg->exit_thunk = 0;
		rg->max_items = tries[i].max_items;
		rg->no_map = tries[i].no_map;
		rg->no_sse41 = no_sse41;
		ret = fa_region_build(rg);
	}
	if (via && (ret < 0 || !fa_rgn_reaches(rg, ip)))
		return NULL;
	if (!via && (ret < 0 || rg->it[0].kind != FA_R_OURS))
		return fa_tr_store(fm, ip, 0, buf, min(nread, 15));
	if (fa_poke(addr, fm->buf, rg->len))
		return NULL;
	main->used = addr + ALIGN(rg->len, 16) - main->start;
	stat_translated += rg->n;
	stat_regions++;
	if (rg->has_map)
		stat_regions_borrowing++;

	/*
	 * Highest address first: an instruction too short for a 5-byte jump
	 * borrows displacement bytes from the one after it, which therefore
	 * has to reach its final form first.
	 */
	for (i = 0; i < rg->n; i++) {
		for (j = i; j > 0 && rg->it[order[j - 1]].ip < rg->it[i].ip; j--)
			order[j] = order[j - 1];
		order[j] = i;
	}
	for (i = 0; i < rg->n; i++) {
		const struct fa_ritem *it = &rg->it[order[i]];
		bool may_patch = true;

		if (it->kind != FA_R_OURS || (it->ip != fresh && xa_load(&fm->tr, it->ip)))
			continue;
		if (it->len < 5 && !xa_load(&fm->tr, it->ip + it->len) &&
		    fa_is_ours(it->ip + it->len)) {
			/* its successor is not settled yet: leave it for a later trap */
			if (it->ip != fresh)
				continue;
			may_patch = false;
		}
		fa_make_entry(fm, main, rg, it, may_patch);
	}

	/*
	 * An AVX instruction that could not be patched would trap every time
	 * its block is entered from untranslated code. Divert the block
	 * earlier instead, at an integer instruction long enough for a jump.
	 */
	cand = -1;
	for (i = 0; patch && i < rg->n; i++) {
		const struct fa_ritem *it = &rg->it[i];
		struct fa_tr *tr;

		if (i && rg->it[i - 1].has_link)
			cand = -1;	/* new block */
		if (it->kind != FA_R_OURS) {
			if (cand == -1 && it->kind != FA_R_JMP && it->len >= 5 &&
			    !xa_load(&fm->tr, it->ip))
				cand = i;
			continue;
		}
		tr = xa_load(&fm->tr, it->ip);
		if (cand >= 0 && (!tr || (tr->stub && tr->bytes[0] != 0xe9))) {
			tr = fa_make_entry(fm, main, rg, &rg->it[cand], true);
			if (tr && tr->bytes[0] == 0xe9)
				stat_patched_int++;
			cand = -2;
		}
	}
	return xa_load(&fm->tr, via ?: ip);
}

/*
 * The instruction at ip (translated, record tr) could not be patched and
 * keeps trapping. Patch the instruction before it instead, if that one is
 * long enough and certainly an instruction (see core/divert.c), with a jump
 * to a region that runs both. Called with fm->lock held.
 */
#define FA_DV_SPAN	(64 << 10)

static int fa_divert(struct fa_mm *fm, unsigned long ip, struct fa_tr *tr)
{
	struct vm_area_struct *vma;
	struct file *file = NULL;
	unsigned long base = 0, end = 0;
	loff_t pos = 0;
	u64 x;
	u8 *buf, mem[FA_DV_BACK];
	int n = 0, ret;
	struct fa_tr *xt;

	mmap_read_lock(fm->mm);
	vma = vma_lookup(fm->mm, ip);
	if (vma && vma->vm_file && !(vma->vm_flags & (VM_WRITE | VM_SHARED))) {
		file = get_file(vma->vm_file);
		base = max(vma->vm_start, ip > FA_DV_SPAN ? ip - FA_DV_SPAN : 0);
		end = min(vma->vm_end, ip + 32);
		pos = (vma->vm_pgoff << PAGE_SHIFT) + (base - vma->vm_start);
	}
	mmap_read_unlock(fm->mm);
	if (!file)
		return FA_DV_FILE;
	buf = kvmalloc(end - base, GFP_KERNEL);
	if (buf)
		n = kernel_read(file, buf, end - base, &pos);
	fput(file);
	if (n <= 0 || ip + tr->len > base + n) {
		kvfree(buf);
		return FA_DV_FILE;
	}

	ret = fa_find_divert(buf, base, n, ip, &x);
	if (ret)
		goto out;
	ret = FA_DV_TAKEN;
	if (xa_load(&fm->tr, x))
		goto out;
	/* what will run must be what was decoded */
	ret = FA_DV_CHANGED;
	if (ip - x > sizeof(mem) || copy_from_user(mem, (void __user *)x, ip - x) ||
	    memcmp(mem, buf + (x - base), ip - x) ||
	    memcmp(tr->bytes, buf + (ip - base), tr->len))
		goto out;
	ret = FA_DV_REGION;
	xt = fa_translate_region(fm, ip, NULL, 0, x);
	if (!xt)
		goto out;
	ret = FA_DV_PATCH;
	if (xt->bytes[0] != 0xe9)
		goto out;
	ret = FA_DV_OK;
	stat_diverted++;
out:
	kvfree(buf);
	if (tr->unp)
		fa_unp_divert(tr->unp, ret, ret ? 0 : ip - x);
	return ret;
}

/*
 * Sleepable part of the #UD handler. Returns with regs->ip either pointing
 * at a translation or unchanged; in the latter case the instruction is
 * re-executed and the next trap takes whatever decision was recorded here.
 */
static void fa_slow(struct pt_regs *regs)
{
	unsigned long ip = regs->ip;
	struct fa_mm *fm;
	struct fa_insn in;
	struct fa_tr *tr;
	u8 buf[16];
	int n;

	stat_slow++;
	fm = fa_mm_get(current->mm);
	if (!fm) {
		force_sig(SIGILL);
		return;
	}
	if (!fm->enabled)
		return;

	/* which binary is this, and does CPUID need answering for it? */
	n = fa_read_insn(ip, buf);
	if (n && !fa_decode(buf, n, ip, &in) && in.kind != FA_K_XGETBV)
		fa_note_file(ip, in.kind == FA_K_VEX);

	mutex_lock(&fm->lock);
	n = fa_read_insn(ip, buf);
	if (!n || buf[0] == 0xe9)
		goto out;	/* unreadable, or patched in the meantime */

	tr = xa_load(&fm->tr, ip);
	if (!tr || n < tr->len || memcmp(buf, tr->bytes, tr->len)) {
		if (fa_decode(buf, n, ip, &in) < 0) {
			/* an encoding the decoder does not know: say so, the program dies */
			stat_unsupported++;
			pr_info_ratelimited("%s[%d]: unsupported instruction at %lx: %*ph\n",
					    current->comm, current->pid, ip, min(n, 15), buf);
			if (!fa_tr_store(fm, ip, 0, buf, min(n, 15)))
				force_sig(SIGILL);
			goto out;
		}
		if (in.kind == FA_K_XGETBV) {
			/* XCR0: x87, SSE and AVX state enabled */
			if (regs->cx == 0) {
				regs->ax = 7;
				regs->dx = 0;
				regs->ip += in.len;
			} else if (!fa_tr_store(fm, ip, 0, buf, in.len)) {
				force_sig(SIGILL);
			}
			goto out;
		}
		tr = fa_translate_region(fm, ip, buf, n, 0);
		if (!tr) {
			force_sig(SIGILL);
			goto out;
		}
		if (!tr->stub) {
			stat_unsupported++;
			pr_info_ratelimited("%s[%d]: unsupported instruction at %lx: %*ph\n",
					    current->comm, current->pid, ip, in.len, buf);
		}
	}
	if (!tr->stub)
		goto out;
	if (tr->unp && !tr->dv_tried && atomic_long_read(&tr->hits) >= FA_DIVERT_HITS &&
	    divert && patch) {
		tr->dv_tried = 1;
		fa_divert(fm, ip, tr);
	}
	if (!fa_thread_setup(fm)) {
		if (fm->enabled)
			force_sig(SIGILL);
		goto out;
	}
	regs->ip = tr->stub;
out:
	mutex_unlock(&fm->lock);
}

static int fa_die(struct notifier_block *nb, unsigned long val, void *data)
{
	struct die_args *args = data;
	struct pt_regs *regs = args->regs;
	struct fa_mm *fm;
	struct fa_tr *tr;
	u8 buf[16];

	if (val != DIE_TRAP || args->trapnr != X86_TRAP_UD)
		return NOTIFY_DONE;
	if (!user_mode(regs) || !user_64bit_mode(regs) ||
	    !(regs->flags & X86_EFLAGS_IF) || !current->mm ||
	    (current->flags & PF_KTHREAD))
		return NOTIFY_DONE;

	/* Fast path: already translated. We are inside the chain's RCU section. */
	fm = fa_mm_find(current->mm);
	if (fm) {
		if (!fm->enabled)
			return NOTIFY_DONE;
		tr = xa_load(&fm->tr, regs->ip);
		if (tr && !copy_from_user_nofault(buf, (void __user *)regs->ip, tr->len) &&
		    !memcmp(buf, tr->bytes, tr->len)) {
			if (!tr->stub)
				return NOTIFY_DONE;	/* genuinely unsupported: SIGILL */
			if (fa_gs_read()) {
				if (tr->unp) {
					/* reported unpatched: count, and try fa_divert() at some point */
					fa_unp_hit(tr->unp);
					if (atomic_long_inc_return(&tr->hits) >= FA_DIVERT_HITS &&
					    !READ_ONCE(tr->dv_tried) && READ_ONCE(divert) && patch)
						goto slow;
				}
				regs->ip = tr->stub;
				stat_fast++;
				return NOTIFY_STOP;
			}
		}
	}

	/*
	 * Slow path: must sleep (user memory, mmap). The notifier chain is
	 * walked under rcu_read_lock(); step out of it for the duration. This
	 * is only sound because we then return NOTIFY_STOP, which makes
	 * notifier_call_chain() stop without looking at the chain again, and
	 * because fa_inflight keeps the module around (see fa_exit()).
	 */
slow:
	atomic_inc(&fa_inflight);
	rcu_read_unlock();
	local_irq_enable();
	fa_slow(regs);
	local_irq_disable();
	rcu_read_lock();
	atomic_dec(&fa_inflight);
	return NOTIFY_STOP;
}

static struct notifier_block fa_nb = {
	.notifier_call = fa_die,
	.priority = INT_MAX,
};

/* ------------------------------------------------------------------ */
/* CPUID                                                               */

/*
 * CPUID cannot be made to trap on this hardware, so a uprobe is placed on
 * every cpuid instruction of the binaries that matter, and its handler
 * answers in the CPU's place.
 *
 * Which binaries: any file in which a handled process hit an unsupported
 * instruction. The first time that happens the file's executable segments
 * are walked instruction by instruction (fa_scan_file) to find its cpuid
 * instructions. Userspace can also supply sites (tools/fakeavx-cpuid), and
 * /proc/fakeavx/sites lists everything known so that it can be saved and
 * given back after a reboot.
 *
 * What is added to the real answer: SSE4.1, SSE4.2 and POPCNT always.
 * AVX/AVX2/OSXSAVE only if the binary has been seen executing a VEX-encoded
 * instruction (it uses AVX whatever CPUID says), or the process asked with
 * FAKEAVX=avx2: a program that can do without AVX is faster on its own SSE
 * code paths than on translated AVX.
 */
struct fa_file {
	struct list_head list;
	struct inode *inode;
	char *path;
	bool scanned;
	bool avx;		/* seen executing VEX-encoded instructions */
	int nsites;
};

struct fa_site {
	struct list_head list;
	struct uprobe_consumer uc;
	struct uprobe *up;
	struct fa_file *ff;
	loff_t off;
};

#define FA_MAX_SITES	4096	/* per file */

static LIST_HEAD(fa_files);
static LIST_HEAD(fa_sites);
static DEFINE_MUTEX(fa_sites_lock);
static struct proc_dir_entry *fa_proc_dir;
static unsigned long stat_cpuid, stat_scans;
module_param(stat_cpuid, ulong, 0444);
module_param(stat_scans, ulong, 0444);

static int fa_cpuid_handler(struct uprobe_consumer *uc, struct pt_regs *regs FA_UPROBE_DATA_ARG)
{
	struct fa_site *st = container_of(uc, struct fa_site, uc);
	u32 leaf = regs->ax, sub = regs->cx, a, b, c, d, max;
	struct fa_mm *fm;
	bool avx;

	if (!current->mm || !user_64bit_mode(regs))
		return 0;
	fm = fa_mm_get(current->mm);
	if (!fm || !fm->enabled)
		return 0;	/* the real instruction runs */
	avx = fm->cpuid_mode == 2 || (fm->cpuid_mode == 0 && READ_ONCE(st->ff->avx));

	cpuid_count(leaf, sub, &a, &b, &c, &d);
	max = cpuid_eax(0);
	switch (leaf) {
	case 0:
		if (avx && a < 7)
			a = 7;
		break;
	case 1:
		c |= BIT(19) | BIT(20) | BIT(23);		/* SSE4.1, SSE4.2, POPCNT */
		if (avx)
			c |= BIT(26) | BIT(27) | BIT(28);	/* XSAVE, OSXSAVE, AVX */
		break;
	case 7:
		if (avx && sub == 0) {
			if (max < 7)
				a = b = c = d = 0;
			b |= BIT(5);				/* AVX2 */
		}
		break;
	}
	regs->ax = a;
	regs->bx = b;
	regs->cx = c;
	regs->dx = d;
	regs->ip += 2;		/* a changed ip makes uprobes skip the instruction */
	stat_cpuid++;
	return 0;
}

/* fa_sites_lock held. path may be NULL if the file is already known. */
static struct fa_file *fa_file_get(struct inode *inode, const char *path)
{
	struct fa_file *ff;

	list_for_each_entry(ff, &fa_files, list)
		if (ff->inode == inode)
			return ff;
	if (!path || !S_ISREG(inode->i_mode))
		return NULL;
	ff = kzalloc(sizeof(*ff), GFP_KERNEL);
	if (!ff)
		return NULL;
	ff->path = kstrdup(path, GFP_KERNEL);
	ff->inode = igrab(inode);
	if (!ff->path || !ff->inode) {
		if (ff->inode)
			iput(ff->inode);
		kfree(ff->path);
		kfree(ff);
		return NULL;
	}
	list_add_tail(&ff->list, &fa_files);
	return ff;
}

/* fa_sites_lock held */
/* uprobe (un)registration, in both API generations (compat.h) */
static int fa_site_register(struct fa_site *st)
{
#ifdef FA_UPROBE_HANDLE
	st->up = uprobe_register(st->ff->inode, st->off, 0, &st->uc);
	return IS_ERR(st->up) ? PTR_ERR(st->up) : 0;
#else
	return uprobe_register(st->ff->inode, st->off, &st->uc);
#endif
}

/* every site in fa_sites; returns once no handler can run any more */
static void fa_sites_unregister(void)
{
	struct fa_site *st;

#ifdef FA_UPROBE_HANDLE
	list_for_each_entry(st, &fa_sites, list)
		uprobe_unregister_nosync(st->up, &st->uc);
	uprobe_unregister_sync();
#else
	list_for_each_entry(st, &fa_sites, list)
		uprobe_unregister(st->ff->inode, st->off, &st->uc);	/* synchronous */
#endif
}

static int fa_site_add(struct fa_file *ff, loff_t off)
{
	struct fa_site *st;
	int ret;

	list_for_each_entry(st, &fa_sites, list)
		if (st->ff == ff && st->off == off)
			return 0;
	if (ff->nsites >= FA_MAX_SITES)
		return -ENOSPC;
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->ff = ff;
	st->off = off;
	st->uc.handler = fa_cpuid_handler;
	ret = fa_site_register(st);
	if (ret) {
		kfree(st);
		return ret;
	}
	list_add_tail(&st->list, &fa_sites);
	ff->nsites++;
	return 0;
}

static void fa_sites_clear(void)
{
	struct fa_site *st, *tmp;
	struct fa_file *ff, *ftmp;

	mutex_lock(&fa_sites_lock);
	fa_sites_unregister();
	list_for_each_entry_safe(st, tmp, &fa_sites, list) {
		list_del(&st->list);
		kfree(st);
	}
	list_for_each_entry_safe(ff, ftmp, &fa_files, list) {
		list_del(&ff->list);
		iput(ff->inode);
		kfree(ff->path);
		kfree(ff);
	}
	mutex_unlock(&fa_sites_lock);
}

/* Walk [start, end) of the file one instruction at a time. fa_sites_lock held. */
static int fa_scan_range(struct fa_file *ff, struct file *file, loff_t start, loff_t end)
{
	const size_t chunk = 1 << 20;
	u8 *buf = kvmalloc(chunk, GFP_KERNEL);
	loff_t o = start;
	int ret = 0;

	if (!buf)
		return -ENOMEM;
	while (o < end && !ret) {
		loff_t pos = o;
		ssize_t have = kernel_read(file, buf, min_t(loff_t, chunk, end - o), &pos);
		bool last = o + have >= end;
		ssize_t i = 0;

		if (have <= 0) {
			ret = have ? have : -EIO;
			break;
		}
		/* stop 15 bytes short of the chunk so no instruction is cut off */
		while (i < have && (last || i + 15 <= have)) {
			int l = fa_insn_len(buf + i, min_t(ssize_t, 15, have - i));

			if (!l)
				break;
			if (l == 2 && buf[i] == 0x0f && buf[i + 1] == 0xa2) {
				ret = fa_site_add(ff, o + i);
				if (ret)
					break;
			}
			i += l;
		}
		if (!i)
			break;		/* truncated tail */
		o += i;
		if (fatal_signal_pending(current))
			ret = -EINTR;
		cond_resched();
	}
	kvfree(buf);
	return ret;
}

/* Find the cpuid instructions in the executable segments of an ELF file. */
static int fa_scan_file(struct fa_file *ff, struct file *file)
{
	struct elf64_hdr eh;
	loff_t pos = 0;
	int i, ret = 0;

	if (kernel_read(file, &eh, sizeof(eh), &pos) != sizeof(eh) ||
	    memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_machine != EM_X86_64 || eh.e_phentsize != sizeof(struct elf64_phdr) ||
	    eh.e_phnum > 128)
		return -ENOEXEC;
	for (i = 0; i < eh.e_phnum && !ret; i++) {
		struct elf64_phdr ph;

		pos = eh.e_phoff + (loff_t)i * sizeof(ph);
		if (kernel_read(file, &ph, sizeof(ph), &pos) != sizeof(ph))
			return -EIO;
		if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
			ret = fa_scan_range(ff, file, ph.p_offset, ph.p_offset + ph.p_filesz);
	}
	return ret;
}

/*
 * A handled process hit an unsupported instruction at ip. Remember the file
 * it belongs to, whether it was AVX, and look for the file's cpuid
 * instructions if that has not been done yet.
 */
static void fa_note_file(unsigned long ip, bool vex)
{
	struct vm_area_struct *vma;
	struct file *file = NULL;
	struct fa_file *ff;
	char *tmp, *path;

	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, ip);
	if (vma && vma->vm_file)
		file = get_file(vma->vm_file);
	mmap_read_unlock(current->mm);
	if (!file)
		return;		/* JIT code */

	mutex_lock(&fa_sites_lock);
	ff = fa_file_get(file_inode(file), NULL);
	if (!ff) {
		tmp = kmalloc(PATH_MAX, GFP_KERNEL);
		path = tmp ? file_path(file, tmp, PATH_MAX) : NULL;
		if (!IS_ERR_OR_NULL(path))
			ff = fa_file_get(file_inode(file), path);
		kfree(tmp);
	}
	if (ff) {
		if (vex && !ff->avx) {
			ff->avx = true;
			pr_info("%s uses AVX; CPUID will say so to it\n", ff->path);
		}
		if (autocpuid && !ff->scanned) {
			int ret = fa_scan_file(ff, file);

			if (ret != -EINTR)
				ff->scanned = true;
			stat_scans++;
			pr_info("%s: %d cpuid instructions found%s\n", ff->path, ff->nsites,
				ret ? " (scan incomplete)" : "");
		}
	}
	mutex_unlock(&fa_sites_lock);
	fput(file);
}

/*
 * Written by tools/fakeavx-cpuid, one line per write:
 *   <hex file offset> <path>    a cpuid instruction
 *   scanned <path>              the file needs no scan
 *   avx <path>                  the file uses AVX
 */
static int fa_cpuid_site_set(const char *val, const struct kernel_param *kp)
{
	unsigned long long off = 0;
	struct fa_file *ff;
	struct path path;
	char *line, *name;
	int kind, ret;

	line = kstrdup(val, GFP_KERNEL);
	if (!line)
		return -ENOMEM;
	strim(line);
	name = strchr(line, ' ');
	if (!name) {
		kfree(line);
		return -EINVAL;
	}
	*name++ = 0;
	name = skip_spaces(name);
	if (!strcmp(line, "scanned")) {
		kind = 1;
	} else if (!strcmp(line, "avx")) {
		kind = 2;
	} else {
		kind = 0;
		ret = kstrtoull(line, 16, &off);
		if (ret)
			goto out;
	}
	ret = kern_path(name, LOOKUP_FOLLOW, &path);
	if (ret)
		goto out;
	mutex_lock(&fa_sites_lock);
	ff = fa_file_get(d_inode(path.dentry), name);
	if (!ff)
		ret = -EINVAL;
	else if (kind == 1)
		ff->scanned = true;
	else if (kind == 2)
		ff->avx = true;
	else
		ret = fa_site_add(ff, off);
	mutex_unlock(&fa_sites_lock);
	path_put(&path);
out:
	kfree(line);
	return ret;
}

static int fa_cpuid_clear_set(const char *val, const struct kernel_param *kp)
{
	fa_sites_clear();
	return 0;
}

static const struct kernel_param_ops fa_cpuid_site_ops = { .set = fa_cpuid_site_set };
static const struct kernel_param_ops fa_cpuid_clear_ops = { .set = fa_cpuid_clear_set };
module_param_cb(cpuid_site, &fa_cpuid_site_ops, NULL, 0200);
MODULE_PARM_DESC(cpuid_site, "write-only, see tools/fakeavx-cpuid");
module_param_cb(cpuid_clear, &fa_cpuid_clear_ops, NULL, 0200);
MODULE_PARM_DESC(cpuid_clear, "write anything to forget all cpuid sites");

/* /proc/fakeavx/sites: everything known, in the format cpuid_site accepts */
static int fa_sites_show(struct seq_file *m, void *v)
{
	struct fa_file *ff;
	struct fa_site *st;

	mutex_lock(&fa_sites_lock);
	list_for_each_entry(ff, &fa_files, list) {
		if (ff->scanned)
			seq_printf(m, "scanned %s\n", ff->path);
		if (ff->avx)
			seq_printf(m, "avx %s\n", ff->path);
	}
	list_for_each_entry(st, &fa_sites, list)
		seq_printf(m, "%llx %s\n", (unsigned long long)st->off, st->ff->path);
	mutex_unlock(&fa_sites_lock);
	return 0;
}

static int fa_sites_open(struct inode *inode, struct file *file)
{
	return single_open(file, fa_sites_show, NULL);
}

static const struct proc_ops fa_sites_proc_ops = {
	.proc_open = fa_sites_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------ */

static int __init fa_init(void)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SSSE3)) {
		pr_err("this CPU has no SSSE3; Core 2 or later is required\n");
		return -ENODEV;
	}
	no_sse41 = !boot_cpu_has(X86_FEATURE_XMM4_1);
	if (boot_cpu_has(X86_FEATURE_AVX) && !force) {
		pr_err("this CPU already supports AVX\n");
		return -ENODEV;
	}
	if (boot_cpu_has(X86_FEATURE_OSXSAVE))
		pr_warn("XSAVE is enabled, so XGETBV cannot be intercepted; boot with \"noxsave\" if programs fail to detect AVX\n");

	fa_wq = alloc_workqueue("fakeavx", WQ_UNBOUND, 0);
	if (!fa_wq)
		return -ENOMEM;
	fa_proc_dir = proc_mkdir("fakeavx", NULL);
	if (fa_proc_dir) {
		proc_create("sites", 0444, fa_proc_dir, &fa_sites_proc_ops);
		proc_create("unpatched", 0444, fa_proc_dir, &fa_unp_proc_ops);
	}
	ret = register_kprobe(&fa_kp_new);
	if (ret) {
		pr_err("cannot hook wake_up_new_task: %d\n", ret);
		goto err_wq;
	}
	ret = register_kprobe(&fa_kp_exit);
	if (ret) {
		pr_err("cannot hook do_exit: %d\n", ret);
		goto err_kp;
	}
	ret = register_die_notifier(&fa_nb);
	if (ret)
		goto err_kp2;
	pr_info("loaded (policy=%d patch=%d, host has %s)\n", policy, patch,
		no_sse41 ? "SSSE3: SSE4.1 is emulated too" : "SSE4.1");
	return 0;

err_kp2:
	unregister_kprobe(&fa_kp_exit);
err_kp:
	unregister_kprobe(&fa_kp_new);
err_wq:
	proc_remove(fa_proc_dir);
	destroy_workqueue(fa_wq);
	return ret;
}

static void __exit fa_exit(void)
{
	struct fa_mm *fm;
	int bkt;

	proc_remove(fa_proc_dir);
	fa_sites_clear();
	unregister_die_notifier(&fa_nb);
	do {
		synchronize_rcu();
		while (atomic_read(&fa_inflight))
			msleep(10);
		synchronize_rcu();
	} while (atomic_read(&fa_inflight));
	unregister_kprobe(&fa_kp_new);
	unregister_kprobe(&fa_kp_exit);

	for (;;) {
		struct fa_mm *victim = NULL;

		spin_lock(&fa_mms_lock);
		hash_for_each(fa_mms, bkt, fm, node) {
			/* if already dead, its free_work is queued */
			if (!test_and_set_bit(0, &fm->dead)) {
				hash_del_rcu(&fm->node);
				victim = fm;
				break;
			}
		}
		spin_unlock(&fa_mms_lock);
		if (!victim)
			break;
		fa_mm_free(victim);
	}
	destroy_workqueue(fa_wq);
	pr_info("unloaded; processes that were using it will now get SIGILL\n");
}

module_init(fa_init);
module_exit(fa_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AVX/AVX2/POPCNT for pre-AVX CPUs by translation to SSE4.1");
