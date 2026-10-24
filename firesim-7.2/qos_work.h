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
enum qos_work { QOS_WORK_HARDIRQ, QOS_WORK_SOFTIRQ, QOS_WORK_TIMER, QOS_WORK_RCU_TICK };

#define QOS_NOSWITCH	(~0UL)		/* no tag / nothing to undo */

#ifdef CONFIG_RISCV_ISA_SSQOSID
unsigned long qos_work_tag(void);
unsigned long qos_work_begin(enum qos_work kind, unsigned int nr, unsigned long tag);
void qos_work_end(unsigned long cookie);
#else
static inline unsigned long qos_work_tag(void) { return QOS_NOSWITCH; }
static inline unsigned long qos_work_begin(enum qos_work kind, unsigned int nr, unsigned long tag) { return QOS_NOSWITCH; }
static inline void qos_work_end(unsigned long cookie) { }
#endif

#endif /* _LINUX_QOS_WORK_H */
