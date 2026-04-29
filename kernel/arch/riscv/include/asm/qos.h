/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_QOS_H
#define _ASM_RISCV_QOS_H

#ifdef CONFIG_RISCV_ISA_SSQOSID

#include <linux/sched.h>
#include <linux/jump_label.h>

#include <asm/barrier.h>
#include <asm/csr.h>
#include <asm/hwcap.h>

/* cached value of srmcfg csr for each cpu */
DECLARE_PER_CPU(u32, cpu_srmcfg);

static inline void __switch_to_srmcfg(struct task_struct *next)
{
	u32 *cpu_srmcfg_ptr = this_cpu_ptr(&cpu_srmcfg);
	u32 thread_srmcfg;

	thread_srmcfg = READ_ONCE(next->thread.srmcfg);

	if (thread_srmcfg != *cpu_srmcfg_ptr) {
		*cpu_srmcfg_ptr = thread_srmcfg;
		csr_write(CSR_SRMCFG, thread_srmcfg);
	}
}

static __always_inline bool has_srmcfg(void)
{
	/* v6.2 lacks the static-branch riscv_has_extension_unlikely; fall
	 * back to the runtime bitmap test (one extra load on the context-
	 * switch hot path). */
	return riscv_isa_extension_available(NULL, SSQOSID);
}

#else /* ! CONFIG_RISCV_ISA_SSQOSID  */

static __always_inline bool has_srmcfg(void) { return false; }
#define __switch_to_srmcfg(__next) do { } while (0)

#endif /* CONFIG_RISCV_ISA_SSQOSID */
#endif /* _ASM_RISCV_QOS_H */
