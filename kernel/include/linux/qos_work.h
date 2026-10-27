/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_QOS_WORK_H
#define _LINUX_QOS_WORK_H

/*
 * Hooks for running kernel work (interrupt handlers, softirqs, timer
 * callbacks, RCU's tick work) under a resource-control ID other than the
 * interrupted task's. Only RISC-V Ssqosid implements them: see
 * arch/riscv/kernel/qos_work.c (qos_resctrl.irq_policy / irq_rcid).
 *
 * qos_work_tag() is the value to record when queueing work that may later
 * be charged to its requester; qos_work_begin()/qos_work_end() bracket the
 * work. The cookie returned by qos_work_begin() must be passed to
 * qos_work_end() in the same context.
 */
enum qos_work {
	QOS_WORK_HARDIRQ,	/* the whole interrupt (arch entry) */
	QOS_WORK_SOFTIRQ,	/* one softirq vector; tag = its raiser */
	QOS_WORK_TIMER,		/* timer-wheel / soft-hrtimer callback; tag = its armer */
	QOS_WORK_RCU_TICK,	/* rcu_sched_clock_irq() */
	QOS_WORK_HRTIMER,	/* hard-hrtimer callback (in hardirq); tag = its armer */
	QOS_WORK_IPI,		/* smp_call_function / irq_work / remote wakeup; tag = its sender */
	QOS_WORK_DEVIRQ,	/* one device-interrupt action; nr = the Linux irq number */
};

#define QOS_NOSWITCH	(~0UL)		/* no tag / nothing to undo */

#ifdef CONFIG_RISCV_ISA_SSQOSID
/* struct __call_single_node carries the queuer's tag; U32_MAX never matches a real srmcfg */
#define qos_node_set_tag(n)	((n)->qos_tag = (u32)qos_work_tag())
#define qos_node_tag(n)		((n)->qos_tag == U32_MAX ? QOS_NOSWITCH : (unsigned long)(n)->qos_tag)
unsigned long qos_work_tag(void);
unsigned long qos_work_begin(enum qos_work kind, unsigned int nr, unsigned long tag);
void qos_work_end(unsigned long cookie);
#else
#define qos_node_set_tag(n)	do { } while (0)
#define qos_node_tag(n)		QOS_NOSWITCH
static inline unsigned long qos_work_tag(void) { return QOS_NOSWITCH; }
static inline unsigned long qos_work_begin(enum qos_work kind, unsigned int nr, unsigned long tag) { return QOS_NOSWITCH; }
static inline void qos_work_end(unsigned long cookie) { }
#endif

#endif /* _LINUX_QOS_WORK_H */
