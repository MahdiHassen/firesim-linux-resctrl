/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_RISCV_QOS_H
#define __LINUX_RISCV_QOS_H

#include <linux/resctrl_types.h>
#include <linux/iommu.h>
#include <linux/types.h>

#include <asm/qos.h>

enum cbqri_controller_type {
	CBQRI_CONTROLLER_TYPE_CAPACITY,
	CBQRI_CONTROLLER_TYPE_BANDWIDTH,
	CBQRI_CONTROLLER_TYPE_UNKNOWN
};

struct cbqri_controller_info {
	unsigned long addr;
	unsigned long size;
	enum cbqri_controller_type type;
	u32 rcid_count;
	u32 mcid_count;
	struct list_head list;

	struct cache_controller {
		u32 cache_level;
		u32 cache_size; /* in bytes */
		struct cpumask cpu_mask;
	} cache;
};

extern struct list_head cbqri_controllers;

bool resctrl_arch_alloc_capable(void);
bool resctrl_arch_mon_capable(void);
bool resctrl_arch_is_llc_occupancy_enabled(void);
bool resctrl_arch_is_mbm_local_enabled(void);
bool resctrl_arch_is_mbm_total_enabled(void);

struct rdt_resource;
/*
 * Note about terminology between x86 (Intel RDT/AMD QoS) and RISC-V:
 *   CLOSID on x86 is RCID on RISC-V
 *     RMID on x86 is MCID on RISC-V
 *      CDP on x86 is AT (access type) on RISC-V
 */
u32  resctrl_arch_rmid_idx_encode(u32 closid, u32 rmid);
void resctrl_arch_rmid_idx_decode(u32 idx, u32 *closid, u32 *rmid);
void resctrl_arch_set_cpu_default_closid_rmid(int cpu, u32 closid, u32 pmg);
void resctrl_arch_sched_in(struct task_struct *tsk);
void resctrl_arch_set_closid_rmid(struct task_struct *tsk, u32 closid, u32 rmid);
bool resctrl_arch_match_closid(struct task_struct *tsk, u32 closid);
bool resctrl_arch_match_rmid(struct task_struct *tsk, u32 closid, u32 rmid);
void resctrl_arch_reset_resources(void);
/* v6.2 backport: resctrl_arch_mon_ctx_alloc/free and resctrl_arch_find_domain
 * are declared by Morse's <linux/resctrl.h> (single-domain era) with
 * signatures that differ from Drew's RFC v1 (split-domain era).
 * The implementations in qos_resctrl.c match Morse's signatures; do not
 * redeclare here. */

static inline bool resctrl_arch_event_is_free_running(enum resctrl_event_id evt)
{
	/* must be true for resctrl L3 monitoring files to be created */
	return true;
}

static inline unsigned int resctrl_arch_round_mon_val(unsigned int val)
{
	return val;
}

/* Not needed for RISC-V */
static inline void resctrl_arch_enable_mon(void) { }
static inline void resctrl_arch_disable_mon(void) { }
static inline void resctrl_arch_enable_alloc(void) { }
static inline void resctrl_arch_disable_alloc(void) { }

/* v6.2 backport stubs/forward-decls — implementations live in
 * arch/riscv/kernel/qos/qos_resctrl.c. Declared here so fs/resctrl/
 * (which includes <asm/resctrl.h>, which includes us) can reach them. */
struct iommu_group;
int  resctrl_arch_pseudo_lock_fn(void *_plr);
int  resctrl_arch_measure_cycles_lat_fn(void *_plr);
int  resctrl_arch_measure_l2_residency(void *_plr);
int  resctrl_arch_measure_l3_residency(void *_plr);
int  resctrl_arch_get_prefetch_disable_bits(void);
bool resctrl_arch_match_iommu_closid(struct iommu_group *group, u32 closid);
bool resctrl_arch_match_iommu_closid_rmid(struct iommu_group *group, u32 closid, u32 rmid);
int  resctrl_arch_set_iommu_closid_rmid(struct iommu_group *group, u32 closid, u32 rmid);
int  resctrl_arch_mon_ctx_alloc_no_wait(struct rdt_resource *r, int evtid);
u32  resctrl_get_default_ctrl(struct rdt_resource *r);
void resctrl_sched_in(void);

#endif /* __LINUX_RISCV_QOS_H */
