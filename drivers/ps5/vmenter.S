/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>

.section .noinstr.text, "ax"

SYM_FUNC_START(svm_run_guest)
	push %_ASM_BP
	mov  %_ASM_SP, %_ASM_BP
	push %r15
	push %r14
	push %r13
	push %r12
	push %_ASM_BX

	mov %r8, %_ASM_AX

	clgi
	vmload %_ASM_AX
	vmrun %_ASM_AX
	vmsave %_ASM_AX
	stgi

	pop %_ASM_BX
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %_ASM_BP
	RET
SYM_FUNC_END(svm_run_guest)
