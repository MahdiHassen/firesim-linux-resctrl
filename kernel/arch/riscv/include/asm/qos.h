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
/*
 * Per-CPU default RCID/MCID, set by resctrl when a CPU is assigned to a
 * group through its "cpus" file. As on x86, a task's own RCID (closid) and
 * MCID (rmid) take precedence when non-zero; zero means "use the CPU's".
 */
DECLARE_PER_CPU(u32, cpu_default_srmcfg);

static inline void __switch_to_srmcfg(struct task_struct *next)
{
	u32 *cpu_srmcfg_ptr = this_cpu_ptr(&cpu_srmcfg);
	u32 def = __this_cpu_read(cpu_default_srmcfg);
	u32 thread_srmcfg, rcid, mcid, srmcfg;

	thread_srmcfg = READ_ONCE(next->thread.srmcfg);

	rcid = thread_srmcfg & SRMCFG_RCID_MASK;
	if (!rcid)
		rcid = def & SRMCFG_RCID_MASK;
	mcid = (thread_srmcfg >> SRMCFG_MCID_SHIFT) & SRMCFG_MCID_MASK;
	if (!mcid)
		mcid = (def >> SRMCFG_MCID_SHIFT) & SRMCFG_MCID_MASK;
	srmcfg = (mcid << SRMCFG_MCID_SHIFT) | rcid;

	if (srmcfg != *cpu_srmcfg_ptr) {
		*cpu_srmcfg_ptr = srmcfg;
		csr_write(CSR_SRMCFG, srmcfg);
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
