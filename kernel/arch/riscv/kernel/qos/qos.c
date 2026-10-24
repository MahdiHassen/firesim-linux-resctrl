// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/riscv_qos.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/bits.h>
#include <linux/qos_work.h>

#include <asm/csr.h>
#include <asm/qos.h>

#include "internal.h"

/* cached value of sqoscfg csr for each cpu */
DEFINE_PER_CPU(u32, cpu_srmcfg);
/* per-CPU default rcid/mcid (resctrl "cpus" assignment) */
DEFINE_PER_CPU(u32, cpu_default_srmcfg);

/*
 * Which RCID kernel work runs under. srmcfg holds the current task's RCID,
 * so by default a capped task's caps also throttle the timer tick, RCU,
 * IPIs and softirqs on its CPU, and that work is charged to the task.
 * irq_policy picks a rule per kind of work (bits combine); irq_rcid is the
 * kernel's own RCID/MCID for the *_KERNEL rules. Both are writable at run
 * time under /sys/module/qos_resctrl/parameters/. Syscalls and exceptions
 * always keep the task's RCID: that work is done on the task's behalf.
 */
#define QOS_IRQ_HARDIRQ_KERNEL	BIT(0)	/* interrupt handlers run under irq_rcid */
#define QOS_IRQ_SOFTIRQ_KERNEL	BIT(1)	/* softirqs run under irq_rcid */
#define QOS_IRQ_SOFTIRQ_RAISER	BIT(2)	/* softirqs run under the RCID that raised them (beats bit 1) */
#define QOS_IRQ_TIMER_ARMER	BIT(3)	/* timer / soft-hrtimer callbacks run under the RCID that armed them */
#define QOS_IRQ_RCU_KERNEL	BIT(4)	/* RCU's tick work and the RCU softirq run under irq_rcid (beats bit 2) */

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "qos_resctrl."
static int irq_rcid = -1;
module_param(irq_rcid, int, 0644);
MODULE_PARM_DESC(irq_rcid, "the kernel's own RCID/MCID for irq_policy (-1 = none)");
static unsigned int irq_policy;
module_param(irq_policy, uint, 0644);
MODULE_PARM_DESC(irq_policy, "1 hardirq, 2 softirq, 16 RCU under irq_rcid; 4 softirq under its raiser, 8 timer under its armer");

static bool qos_ready;					/* Ssqosid present, resctrl set up */
static DEFINE_PER_CPU(unsigned int, qos_work_depth);	/* nested qos_work_begin() switches on this CPU */

static inline unsigned long kernel_srmcfg(void)
{
	int rcid = READ_ONCE(irq_rcid);

	if (rcid < 0 || rcid > (int)SRMCFG_RCID_MASK || rcid > (int)SRMCFG_MCID_MASK)
		return QOS_NOSWITCH;
	return ((unsigned long)rcid << SRMCFG_MCID_SHIFT) | rcid;
}

unsigned long qos_work_tag(void)
{
	return __this_cpu_read(cpu_srmcfg);
}

/* returns the cookie for qos_work_end(); QOS_NOSWITCH when the policy leaves srmcfg alone */
unsigned long qos_work_begin(enum qos_work kind, unsigned int nr, unsigned long tag)
{
	unsigned int policy = READ_ONCE(irq_policy);
	unsigned long want = QOS_NOSWITCH;
	u32 prev;

	if (likely(!policy) || !READ_ONCE(qos_ready))
		return QOS_NOSWITCH;

	switch (kind) {
	case QOS_WORK_HARDIRQ:
		if (policy & QOS_IRQ_HARDIRQ_KERNEL)
			want = kernel_srmcfg();
		break;
	case QOS_WORK_SOFTIRQ:
		if ((policy & QOS_IRQ_RCU_KERNEL) && nr == RCU_SOFTIRQ)
			want = kernel_srmcfg();
		else if ((policy & QOS_IRQ_SOFTIRQ_RAISER) && tag != QOS_NOSWITCH)
			want = tag;
		else if (policy & QOS_IRQ_SOFTIRQ_KERNEL)
			want = kernel_srmcfg();
		break;
	case QOS_WORK_TIMER:
		if ((policy & QOS_IRQ_TIMER_ARMER) && tag != QOS_NOSWITCH)
			want = tag;
		break;
	case QOS_WORK_RCU_TICK:
		if (policy & QOS_IRQ_RCU_KERNEL)
			want = kernel_srmcfg();
		break;
	}
	if (want == QOS_NOSWITCH)
		return QOS_NOSWITCH;

	prev = __this_cpu_read(cpu_srmcfg);
	__this_cpu_inc(qos_work_depth);
	if ((u32)want != prev) {
		__this_cpu_write(cpu_srmcfg, (u32)want);
		csr_write(CSR_SRMCFG, (u32)want);
	}
	return prev;
}

void qos_work_end(unsigned long cookie)
{
	if (cookie == QOS_NOSWITCH)
		return;
	if (__this_cpu_dec_return(qos_work_depth) == 0) {
		/* outermost: recomputed, not restored, as resctrl may have moved current meanwhile */
		__switch_to_srmcfg(current);
	} else if (__this_cpu_read(cpu_srmcfg) != (u32)cookie) {
		__this_cpu_write(cpu_srmcfg, (u32)cookie);
		csr_write(CSR_SRMCFG, (u32)cookie);
	}
}

/* entry.S: interrupts come here instead of generic_handle_arch_irq */
asmlinkage void noinstr riscv_qos_handle_irq(struct pt_regs *regs)
{
	unsigned long cookie = qos_work_begin(QOS_WORK_HARDIRQ, 0, QOS_NOSWITCH);

	generic_handle_arch_irq(regs);
	qos_work_end(cookie);
}

static int __init qos_arch_late_init(void)
{
	int err;

	if (!riscv_isa_extension_available(NULL, SSQOSID))
		return -ENODEV;

	err = qos_resctrl_setup();
	if (err != 0)
		return err;

	WRITE_ONCE(qos_ready, true);
	pr_info("qos: irq_policy=0x%x irq_rcid=%d (runtime: /sys/module/qos_resctrl/parameters/)\n",
		irq_policy, irq_rcid);

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "qos:online", qos_resctrl_online_cpu,
			  qos_resctrl_offline_cpu);

	return err;
}
late_initcall(qos_arch_late_init);
