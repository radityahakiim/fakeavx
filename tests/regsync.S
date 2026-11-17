/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test glue for regtest.c. On the development machine untranslated code runs
 * as real AVX, so the upper ymm halves have to be carried between the real
 * registers and the %gs state block whenever control crosses over.
 */
#include "../core/fakeavx.h"
	.text
	.globl fa_enter_sync, fa_exit_thunk, fa_sync_target
/* called like the function under test; jumps to the region entry stub */
fa_enter_sync:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	vextractf128 $1, %ymm\n, %gs:FA_ST_HI + 16 * \n
	.endr
	jmp	*fa_sync_target(%rip)
/* region exits come here with the guest target address in %gs:FA_ST_GTMP */
fa_exit_thunk:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	vinsertf128 $1, %gs:FA_ST_HI + 16 * \n, %ymm\n, %ymm\n
	.endr
	jmp	*%gs:FA_ST_GTMP
	.bss
fa_sync_target:	.quad 0
	.section .note.GNU-stack,"",@progbits
