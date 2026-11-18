/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel API differences between the versions fakeavx builds on (6.1 to
 * 7.x). Every boundary below was checked against that version's headers.
 */
#ifndef FA_COMPAT_H
#define FA_COMPAT_H

#include <linux/version.h>

/*
 * The CPUID helpers (cpuid_count, cpuid_eax) moved to <asm/cpuid/api.h> in
 * 6.15; before that they came with <asm/processor.h>.
 */
#if __has_include(<asm/cpuid/api.h>)
#include <asm/cpuid/api.h>
#else
#include <asm/processor.h>
#endif

/* rdmsrl/wrmsrl were renamed rdmsrq/wrmsrq in 6.16 (the old names were later removed) */
#include <asm/msr.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define fa_rdmsrq(msr, val)	rdmsrq(msr, val)
#define fa_wrmsrq(msr, val)	wrmsrq(msr, val)
#else
#define fa_rdmsrq(msr, val)	rdmsrl(msr, val)
#define fa_wrmsrq(msr, val)	wrmsrl(msr, val)
#endif

/*
 * uprobes:
 *  - 6.12: uprobe_register() returns a struct uprobe * (and takes a
 *    reference counter offset); unregistering is uprobe_unregister_nosync()
 *    per probe plus one uprobe_unregister_sync(). Before: an int, and
 *    uprobe_unregister(inode, offset, consumer).
 *  - 6.13: the consumer's handler gets a third argument, __u64 *data.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define FA_UPROBE_HANDLE	1
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define FA_UPROBE_DATA_ARG	, __u64 *data
#else
#define FA_UPROBE_DATA_ARG
#endif

#endif
