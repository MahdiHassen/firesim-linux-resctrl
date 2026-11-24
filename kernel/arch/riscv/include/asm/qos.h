/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_QOS_H
#define _ASM_RISCV_QOS_H

#include <linux/percpu-defs.h>

#ifdef CONFIG_RISCV_ISA_SSQOSID

#include <linux/bitfield.h>
#include <linux/cpufeature.h>
#include <linux/sched.h>

#include <asm/csr.h>
#include <asm/fence.h>
#include <asm/hwcap.h>

/* cached value of srmcfg csr for each cpu */
DECLARE_PER_CPU(u32, cpu_srmcfg);

/* default srmcfg value for each cpu, set via resctrl cpu assignment */
DECLARE_PER_CPU(u32, cpu_srmcfg_default);

/*
 * Which IDs kernel-mode accesses carry (AlSaqr: hqosid, in effect while the loader has set mqosctl[1]).
 * /sys/module/qos/parameters/kqos_mode:
 *   0  hqosid is left alone
 *   1  task:   the running task's IDs, as on a machine without privilege association
 *   2  kernel: riscv_kqos_kernel_id for all kernel work (what PLZA gives)
 *   3  select: the kernel's IDs, except inside a system call or a page fault taken from user mode, which run
 *              under the task's IDs (interrupts that arrive there are the kernel's again)
 *   4  select + tags: as 3, and a work item runs under the IDs of the task that queued it
 */
enum { KQOS_OFF, KQOS_TASK, KQOS_KERNEL, KQOS_SELECT, KQOS_SELECT_TAG };
#define KQOS_TAG_VALID	BIT(31)
extern unsigned int riscv_kqos_mode;
extern u32 riscv_kqos_kernel_id;
DECLARE_PER_CPU(u32, cpu_hqosid);
void riscv_kqos_refresh(void);

static __always_inline void riscv_kqos_write(u32 v)
{
	if (v != __this_cpu_read(cpu_hqosid)) {
		__this_cpu_write(cpu_hqosid, v);
		csr_write(CSR_HQOSID, v);
	}
}

/* the IDs of @t's kernel-mode accesses in its own context; @srmcfg: its user-mode IDs */
static inline u32 riscv_kqos_task(struct task_struct *t, u32 srmcfg)
{
	unsigned int mode = READ_ONCE(riscv_kqos_mode);
	u32 tag;

	if (mode == KQOS_TASK)
		return srmcfg;
	if (mode == KQOS_KERNEL)
		return riscv_kqos_kernel_id;
	if (!(t->flags & PF_KTHREAD))
		return READ_ONCE(t->thread.kqos_self) ? srmcfg : riscv_kqos_kernel_id;
	tag = READ_ONCE(t->thread.kqos_work);
	if (mode == KQOS_SELECT_TAG && (tag & KQOS_TAG_VALID))
		return tag & ~KQOS_TAG_VALID;
	return riscv_kqos_kernel_id;
}

/* a system call or a user page fault starts (@on) or ends */
static __always_inline void riscv_kqos_self(bool on)
{
	current->thread.kqos_self = on;
	if (READ_ONCE(riscv_kqos_mode) >= KQOS_SELECT)
		riscv_kqos_refresh();
}

static inline void __switch_to_srmcfg(struct task_struct *next)
{
	u32 thread_srmcfg, default_srmcfg;

	thread_srmcfg = READ_ONCE(next->thread.srmcfg);
	default_srmcfg = __this_cpu_read(cpu_srmcfg_default);

	/*
	 * RCID and MCID inherit from cpu_srmcfg_default independently.
	 * RESCTRL_RESERVED_CLOSID and RESCTRL_RESERVED_RMID are both 0,
	 * so a per-field zero means "no task assignment for this
	 * dimension" and the CPU default supplies that field. Matches
	 * x86 RDT's __resctrl_sched_in() per-field logic. The fully
	 * unassigned (thread.srmcfg == 0) and fully assigned (both
	 * fields non-zero) cases short-circuit the field math.
	 */
	if (thread_srmcfg == 0) {
		thread_srmcfg = default_srmcfg;
	} else {
		u32 rcid = FIELD_GET(SRMCFG_RCID_MASK, thread_srmcfg);
		u32 mcid = FIELD_GET(SRMCFG_MCID_MASK, thread_srmcfg);

		if (rcid == 0 || mcid == 0) {
			if (rcid == 0)
				rcid = FIELD_GET(SRMCFG_RCID_MASK, default_srmcfg);
			if (mcid == 0)
				mcid = FIELD_GET(SRMCFG_MCID_MASK, default_srmcfg);
			thread_srmcfg = FIELD_PREP(SRMCFG_RCID_MASK, rcid) |
					FIELD_PREP(SRMCFG_MCID_MASK, mcid);
		}
	}

	if (thread_srmcfg != __this_cpu_read(cpu_srmcfg)) {
		/*
		 * Drain stores from the outgoing task before the CSR write
		 * so they retain the previous RCID/MCID tag at the cache
		 * interconnect.
		 */
		RISCV_FENCE(rw, o);

		__this_cpu_write(cpu_srmcfg, thread_srmcfg);
		csr_write(CSR_SRMCFG, thread_srmcfg);
		/*
		 * Order the csrw before the new task's loads/stores so they
		 * pick up the new tag. Zicsr 6.1.1 makes CSR writes weakly
		 * ordered (device-output) vs memory ops. Ssqosid v1.0 is
		 * silent so honor the general CSR rule.
		 */
		RISCV_FENCE(o, rw);
	}
	if (READ_ONCE(riscv_kqos_mode))
		riscv_kqos_write(riscv_kqos_task(next, thread_srmcfg));
}

static __always_inline bool has_srmcfg(void)
{
	return riscv_has_extension_unlikely(RISCV_ISA_EXT_SSQOSID);
}

#else /* ! CONFIG_RISCV_ISA_SSQOSID  */

struct task_struct;
static __always_inline void riscv_kqos_self(bool on) { }
static __always_inline bool has_srmcfg(void) { return false; }
static inline void __switch_to_srmcfg(struct task_struct *next) { }

#endif /* CONFIG_RISCV_ISA_SSQOSID */
#endif /* _ASM_RISCV_QOS_H */
