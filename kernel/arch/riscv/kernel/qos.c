// SPDX-License-Identifier: GPL-2.0-only
#include <linux/moduleparam.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuhotplug.h>
#include <linux/notifier.h>
#include <linux/percpu-defs.h>
#include <linux/types.h>

#include <asm/cpufeature-macros.h>
#include <asm/hwcap.h>
#include <asm/qos.h>

/*
 * Cached value of srmcfg csr for each cpu. Seeded to U32_MAX so the next
 * __switch_to_srmcfg() unconditionally writes the CSR; the encoding
 * MCID << 16 | RCID with both fields well under 16 bits can never
 * produce this sentinel. This covers early-boot context switches that
 * happen before riscv_srmcfg_init() runs as an arch_initcall.
 */
DEFINE_PER_CPU(u32, cpu_srmcfg) = U32_MAX;

/* default srmcfg value for each cpu, set via resctrl cpu assignment */
DEFINE_PER_CPU(u32, cpu_srmcfg_default);

/* IDs of kernel-mode accesses (asm/qos.h); the cache starts at a value no encoding produces */
unsigned int riscv_kqos_mode;
u32 riscv_kqos_kernel_id = 0x80008;	/* RCID 8, MCID 8: what the AlSaqr loader gives HS mode */
DEFINE_PER_CPU(u32, cpu_hqosid) = U32_MAX;
EXPORT_SYMBOL_GPL(riscv_kqos_mode);

/* the current task's kernel-mode IDs changed (work item started or ended, mode switched) */
void riscv_kqos_refresh(void)
{
	unsigned long flags;

	if (!READ_ONCE(riscv_kqos_mode))
		return;
	local_irq_save(flags);
	riscv_kqos_write(riscv_kqos_task(current, __this_cpu_read(cpu_srmcfg)));
	local_irq_restore(flags);
}

static void riscv_kqos_refresh_ipi(void *unused)
{
	riscv_kqos_refresh();
}

static int kqos_mode_set(const char *val, const struct kernel_param *kp)
{
	unsigned int mode;
	int err = kstrtouint(val, 0, &mode);

	if (err)
		return err;
	if (mode > KQOS_SELECT_TAG || !riscv_has_extension_unlikely(RISCV_ISA_EXT_SSQOSID))
		return -EINVAL;
	WRITE_ONCE(riscv_kqos_mode, mode);
	on_each_cpu(riscv_kqos_refresh_ipi, NULL, 1);
	return 0;
}

static const struct kernel_param_ops kqos_mode_ops = {
	.set = kqos_mode_set,
	.get = param_get_uint,
};
module_param_cb(kqos_mode, &kqos_mode_ops, &riscv_kqos_mode, 0644);
module_param_named(kqos_kernel_id, riscv_kqos_kernel_id, uint, 0644);

/*
 * Seed the per-CPU srmcfg cache to a sentinel that no real srmcfg encoding
 * can produce (MCID << 16 | RCID, both fields well under 16 bits) so the
 * next __switch_to_srmcfg() unconditionally writes the CSR. Ssqosid v1.0
 * leaves CSR state across hart stop/start implementation-defined, so the
 * cached value cannot be trusted after online.
 */
static int riscv_srmcfg_online(unsigned int cpu)
{
	per_cpu(cpu_srmcfg, cpu) = U32_MAX;
	return 0;
}

/*
 * Invalidate the cache on offline too. The sentinel persists across the
 * offline period, so a CPU brought back online finds the cache already
 * invalidated before it is schedulable. This closes the window where a
 * task scheduled before riscv_srmcfg_online() runs could match a stale
 * cache and skip the CSR write while the hardware CSR was reset across
 * hart stop/start.
 */
static int riscv_srmcfg_offline(unsigned int cpu)
{
	per_cpu(cpu_srmcfg, cpu) = U32_MAX;
	return 0;
}

/*
 * CPU PM notifier: invalidate the cached srmcfg on resume from a deep
 * idle / suspend. Ssqosid v1.0 leaves CSR_SRMCFG state across low-power
 * transitions implementation-defined, and the boot CPU never goes
 * through the cpuhp online callback during system suspend, so without
 * this hook __switch_to_srmcfg() would skip the CSR write when the
 * outgoing task happens to share its srmcfg with the pre-suspend cache.
 */
static int riscv_srmcfg_pm_notify(struct notifier_block *nb,
				  unsigned long action, void *unused)
{
	switch (action) {
	case CPU_PM_EXIT:
	case CPU_PM_ENTER_FAILED:
		__this_cpu_write(cpu_srmcfg, U32_MAX);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block riscv_srmcfg_pm_nb = {
	.notifier_call = riscv_srmcfg_pm_notify,
};

static int __init riscv_srmcfg_init(void)
{
	int err;

	if (!riscv_has_extension_unlikely(RISCV_ISA_EXT_SSQOSID))
		return 0;

	/*
	 * cpuhp_setup_state() invokes the startup callback locally on every
	 * already-online CPU, so no separate seed loop is needed here.
	 */
	err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "riscv/srmcfg:online",
				riscv_srmcfg_online, riscv_srmcfg_offline);
	if (err < 0) {
		pr_warn("srmcfg cpuhp registration failed (%d), cpus brought online after boot will not invalidate the CSR_SRMCFG cache\n",
			err);
		return err;
	}

	cpu_pm_register_notifier(&riscv_srmcfg_pm_nb);
	return 0;
}
arch_initcall(riscv_srmcfg_init);
