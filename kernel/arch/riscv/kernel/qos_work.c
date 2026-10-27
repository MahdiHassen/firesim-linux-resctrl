// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which RCID kernel work runs under (include/linux/qos_work.h).
 *
 * srmcfg holds the current task's RCID, so by default a capped task's caps
 * also throttle the timer tick, RCU, IPIs and softirqs on its CPU, and that
 * work is charged to the task. irq_policy picks a rule per kind of work
 * (bits combine); irq_rcid is the kernel's own RCID/MCID for the *_KERNEL
 * rules. Both are writable at run time under
 * /sys/module/qos_resctrl/parameters/. Syscalls and exceptions always keep
 * the task's RCID: that work is done on the task's behalf.
 *
 * "Causer" accounting (irq_policy=0xfd): every piece of asynchronous kernel
 * work runs under the RCID of the task that caused it, and only work with no
 * causer runs under irq_rcid:
 *   - interrupt entry and the scheduler tick: irq_rcid (bit 0; the tick's
 *     hrtimer is marked qos_kernel so it is never charged to an armer)
 *   - softirqs: their raiser (bit 2), RCU's softirq and tick work: irq_rcid
 *     (bit 4; grace periods serve every task)
 *   - timer-wheel and soft-hrtimer callbacks: their armer (bit 3)
 *   - hard-hrtimer callbacks (nanosleep/poll/epoll timeouts, ...): their
 *     armer (bit 5)
 *   - smp_call_function(), irq_work and remote wakeups: their sender
 *     (bit 6; the tag rides in struct __call_single_node)
 *   - device interrupts: the line's owner from irq_owner (bit 7)
 * Known gaps: kthreads and workqueue items run under their own task's RCID
 * (root unless moved), and a softirq vector raised by several RCIDs before it
 * runs is charged to the last raiser.
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/qos_work.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/csr.h>
#include <asm/fence.h>
#include <asm/qos.h>

#define QOS_IRQ_HARDIRQ_KERNEL	BIT(0)	/* interrupt handlers run under irq_rcid */
#define QOS_IRQ_SOFTIRQ_KERNEL	BIT(1)	/* softirqs run under irq_rcid */
#define QOS_IRQ_SOFTIRQ_RAISER	BIT(2)	/* softirqs run under the RCID that raised them (beats bit 1) */
#define QOS_IRQ_TIMER_ARMER	BIT(3)	/* timer / soft-hrtimer callbacks run under the RCID that armed them */
#define QOS_IRQ_RCU_KERNEL	BIT(4)	/* RCU's tick work and the RCU softirq run under irq_rcid (beats bit 2) */
#define QOS_IRQ_HRTIMER_ARMER	BIT(5)	/* hard-hrtimer callbacks run under the RCID that armed them (not the tick) */
#define QOS_IRQ_IPI_SENDER	BIT(6)	/* smp_call_function / irq_work / remote-wakeup callbacks run under their sender */
#define QOS_IRQ_DEVICE_OWNER	BIT(7)	/* device-interrupt handlers run under their line's owner (irq_owner) */

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "qos_resctrl."
static int irq_rcid = -1;
module_param(irq_rcid, int, 0644);
MODULE_PARM_DESC(irq_rcid, "the kernel's own RCID/MCID for irq_policy (-1 = none)");
static unsigned int irq_policy;
module_param(irq_policy, uint, 0644);
MODULE_PARM_DESC(irq_policy, "1 hardirq, 2 softirq, 16 RCU under irq_rcid; 4 softirq under its raiser, 8 timer, "
		 "32 hard hrtimer under its armer, 64 IPI/irq_work/wakeup under its sender, 128 device irq under its owner; "
		 "0xfd = causer accounting");

/* device-interrupt owners: srmcfg + 1 per Linux irq number (0 = no owner: the hardirq rule applies) */
#define QOS_NR_OWNED_IRQS	256
static u32 irq_owner_tag[QOS_NR_OWNED_IRQS];

static unsigned long rcid_srmcfg(unsigned int rcid)
{
	return FIELD_PREP(SRMCFG_RCID_MASK, rcid) | FIELD_PREP(SRMCFG_MCID_MASK, rcid);
}

/* "irq:rcid[,irq:rcid...]" replaces the whole map; "" clears it */
static int irq_owner_set(const char *val, const struct kernel_param *kp)
{
	char *buf, *p, *tok;
	int ret = 0, i;
	u32 *map;

	map = kcalloc(QOS_NR_OWNED_IRQS, sizeof(*map), GFP_KERNEL);
	buf = kstrdup(val, GFP_KERNEL);
	if (!map || !buf) {
		kfree(map);
		kfree(buf);
		return -ENOMEM;
	}
	p = strim(buf);
	while ((tok = strsep(&p, ",")) && *tok) {
		unsigned int irq, rcid;

		if (sscanf(tok, "%u:%u", &irq, &rcid) != 2 || irq >= QOS_NR_OWNED_IRQS ||
		    rcid > FIELD_MAX(SRMCFG_RCID_MASK) || rcid > FIELD_MAX(SRMCFG_MCID_MASK)) {
			ret = -EINVAL;
			break;
		}
		map[irq] = (u32)rcid_srmcfg(rcid) + 1;
	}
	if (!ret)
		for (i = 0; i < QOS_NR_OWNED_IRQS; i++)
			WRITE_ONCE(irq_owner_tag[i], map[i]);
	kfree(map);
	kfree(buf);
	return ret;
}

static int irq_owner_get(char *buffer, const struct kernel_param *kp)
{
	int i, n = 0;

	for (i = 0; i < QOS_NR_OWNED_IRQS; i++) {
		u32 t = READ_ONCE(irq_owner_tag[i]);

		if (t)
			n += scnprintf(buffer + n, PAGE_SIZE - n, "%s%d:%lu", n ? "," : "", i,
				       FIELD_GET(SRMCFG_RCID_MASK, (unsigned long)(t - 1)));
	}
	n += scnprintf(buffer + n, PAGE_SIZE - n, "\n");
	return n;
}

static const struct kernel_param_ops irq_owner_ops = { .set = irq_owner_set, .get = irq_owner_get };
module_param_cb(irq_owner, &irq_owner_ops, NULL, 0644);
MODULE_PARM_DESC(irq_owner, "device-interrupt owners for irq_policy bit 128: irq:rcid[,irq:rcid...]");

static DEFINE_PER_CPU(unsigned int, qos_work_depth);	/* nested qos_work_begin() switches on this CPU */

static inline unsigned long kernel_srmcfg(void)
{
	int rcid = READ_ONCE(irq_rcid);

	if (rcid < 0 || rcid > FIELD_MAX(SRMCFG_RCID_MASK) || rcid > FIELD_MAX(SRMCFG_MCID_MASK))
		return QOS_NOSWITCH;
	return FIELD_PREP(SRMCFG_RCID_MASK, rcid) | FIELD_PREP(SRMCFG_MCID_MASK, rcid);
}

/* same fences as __switch_to_srmcfg(): drain the old tag's stores, order the new tag's accesses */
static inline void qos_write_srmcfg(u32 val)
{
	RISCV_FENCE(rw, o);
	__this_cpu_write(cpu_srmcfg, val);
	csr_write(CSR_SRMCFG, val);
	RISCV_FENCE(o, rw);
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

	if (likely(!policy) || !has_srmcfg())
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
	case QOS_WORK_HRTIMER:
		if ((policy & QOS_IRQ_HRTIMER_ARMER) && tag != QOS_NOSWITCH)
			want = tag;
		break;
	case QOS_WORK_IPI:
		if ((policy & QOS_IRQ_IPI_SENDER) && tag != QOS_NOSWITCH)
			want = tag;
		break;
	case QOS_WORK_DEVIRQ:
		if ((policy & QOS_IRQ_DEVICE_OWNER) && nr < QOS_NR_OWNED_IRQS) {
			u32 t = READ_ONCE(irq_owner_tag[nr]);

			if (t)
				want = t - 1;
		}
		break;
	}
	if (want == QOS_NOSWITCH)
		return QOS_NOSWITCH;

	prev = __this_cpu_read(cpu_srmcfg);
	__this_cpu_inc(qos_work_depth);
	if ((u32)want != prev)
		qos_write_srmcfg((u32)want);
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
		qos_write_srmcfg((u32)cookie);
	}
}

static int __init qos_work_init(void)
{
	if (!has_srmcfg())
		return 0;
	pr_info("qos: irq_policy=0x%x irq_rcid=%d (runtime: /sys/module/qos_resctrl/parameters/)\n",
		irq_policy, irq_rcid);
	return 0;
}
late_initcall(qos_work_init);
