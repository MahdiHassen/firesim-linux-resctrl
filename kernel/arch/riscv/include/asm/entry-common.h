/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_ENTRY_COMMON_H
#define _ASM_RISCV_ENTRY_COMMON_H

#include <asm/stacktrace.h>
#include <asm/thread_info.h>
#include <asm/vector.h>

static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	if (ti_work & _TIF_RISCV_V_DEFER_RESTORE) {
		clear_thread_flag(TIF_RISCV_V_DEFER_RESTORE);
		/*
		 * We are already called with irq disabled, so go without
		 * keeping track of riscv_v_flags.
		 */
		riscv_v_vstate_restore(&current->thread.vstate, regs);
	}
}

#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare

#include <asm/qos.h>

/* qos_kernel_rcid0: kernel-mode work under srmcfg 0, user mode under the task's own tag */
static __always_inline void arch_enter_from_user_mode(struct pt_regs *regs)
{
	qos_kernel_enter();
}
#define arch_enter_from_user_mode arch_enter_from_user_mode

static __always_inline void arch_exit_to_user_mode(void)
{
	qos_user_return();
}
#define arch_exit_to_user_mode arch_exit_to_user_mode

void handle_page_fault(struct pt_regs *regs);
void handle_break(struct pt_regs *regs);

#ifdef CONFIG_RISCV_MISALIGNED
int handle_misaligned_load(struct pt_regs *regs);
int handle_misaligned_store(struct pt_regs *regs);
#else
static inline int handle_misaligned_load(struct pt_regs *regs)
{
	return -1;
}

static inline int handle_misaligned_store(struct pt_regs *regs)
{
	return -1;
}
#endif

bool handle_user_cfi_violation(struct pt_regs *regs);

#endif /* _ASM_RISCV_ENTRY_COMMON_H */
