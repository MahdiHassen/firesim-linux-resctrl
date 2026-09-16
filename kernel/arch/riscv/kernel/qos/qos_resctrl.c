// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "qos: resctrl: " fmt

#include <linux/slab.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/moduleparam.h>
#include <linux/riscv_qos.h>
#include <linux/resctrl.h>
#include <linux/types.h>
#include <asm/csr.h>
#include <asm/qos.h>
#include "internal.h"

#define MAX_CONTROLLERS 6
static struct cbqri_controller controllers[MAX_CONTROLLERS];
static struct cbqri_resctrl_res cbqri_resctrl_resources[RDT_NUM_RESOURCES];

/*
 * FireSim (crsullivan13) regulators are off at reset and need a period.
 * Boot-time: qos_resctrl.period=<cycles> qos_resctrl.regulate=<0|1>
 * With the default period, MB 100% = nbwblks fills per bank per period.
 */
static unsigned int period = 1000000;
module_param(period, uint, 0444);
MODULE_PARM_DESC(period, "CBQRI bandwidth-regulator period in regulator clock cycles");
static bool regulate = true;
module_param(regulate, bool, 0444);
MODULE_PARM_DESC(regulate, "Turn CBQRI bandwidth regulation on at boot");

static bool exposed_alloc_capable;
static bool exposed_mon_capable;
/* set when a bandwidth controller supports monitoring: mbm_total_bytes */
static bool exposed_mbm_total;
/* set when a capacity controller supports monitoring: llc_occupancy */
static bool exposed_llc_occupancy;
/* CDP (code data prioritization) on x86 is AT (access type) on RISC-V */
static bool exposed_cdp_l2_capable;
static bool exposed_cdp_l3_capable;
static bool is_cdp_l2_enabled;
static bool is_cdp_l3_enabled;

/* used by resctrl_arch_system_num_rmid_idx() */
static u32 max_rmid;

LIST_HEAD(cbqri_controllers);

static int cbqri_wait_busy_flag(struct cbqri_controller *ctrl, int reg_offset);
static int cbqri_bc_mon_op(struct cbqri_controller *ctrl, int operation,
			   u32 mcid, u32 evt_id);

/* number of RCIDs the controller behind a domain implements */
static u32 cbqri_dom_num_rcid(struct rdt_domain *d)
{
	struct cbqri_resctrl_dom *hw_dom;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	return hw_dom->hw_ctrl->ctrl_info->rcid_count;
}

/*
 * Several controllers can back one resctrl resource (e.g. both bandwidth
 * regulators are "MB"). Expose the smallest RCID/MCID space so every id
 * resctrl hands out is valid on every controller.
 */
static void cbqri_res_set_counts(struct cbqri_resctrl_res *hw_res,
				 struct cbqri_controller *ctrl)
{
	u32 nr = ctrl->ctrl_info->rcid_count;
	u32 nm = ctrl->ctrl_info->mcid_count;

	if (!hw_res->max_rcid || nr < hw_res->max_rcid)
		hw_res->max_rcid = nr;
	if (!hw_res->max_mcid || nm < hw_res->max_mcid)
		hw_res->max_mcid = nm;
	hw_res->resctrl_res.num_rmid = hw_res->max_mcid;
}

bool resctrl_arch_alloc_capable(void)
{
	return exposed_alloc_capable;
}

bool resctrl_arch_mon_capable(void)
{
	return exposed_mon_capable;
}

bool resctrl_arch_is_llc_occupancy_enabled(void)
{
	return exposed_llc_occupancy;
}

bool resctrl_arch_is_mbm_local_enabled(void)
{
	return false;
}

bool resctrl_arch_is_mbm_total_enabled(void)
{
	return exposed_mbm_total;
}

bool resctrl_arch_get_cdp_enabled(enum resctrl_res_level rid)
{
	switch (rid) {
	case RDT_RESOURCE_L2:
		return is_cdp_l2_enabled;

	case RDT_RESOURCE_L3:
		return is_cdp_l3_enabled;

	default:
		return false;
	}
}

int resctrl_arch_set_cdp_enabled(enum resctrl_res_level rid, bool enable)
{
	switch (rid) {
	case RDT_RESOURCE_L2:
		if (!exposed_cdp_l2_capable)
			return -ENODEV;
		is_cdp_l2_enabled = enable;
		break;

	case RDT_RESOURCE_L3:
		if (!exposed_cdp_l3_capable)
			return -ENODEV;
		is_cdp_l3_enabled = enable;
		break;

	default:
		return -ENODEV;
	}

	return 0;
}

struct rdt_resource *resctrl_arch_get_resource(enum resctrl_res_level l)
{
	if (l >= RDT_NUM_RESOURCES)
		return NULL;

	return &cbqri_resctrl_resources[l].resctrl_res;
}

struct rdt_domain *resctrl_arch_find_domain(struct rdt_resource *r, int id)
{
	struct rdt_domain *d;

	/* lockdep_assert_cpus_held() */;

	/* v6.2 backport: Morse's API takes rdt_resource* and walks the
	 * single per-resource domain list, instead of a list_head pointer
	 * to one of the split (ctrl/mon) lists. */
	list_for_each_entry(d, &r->domains, list) {
		if (d->id == id)
			return d;
	}

	return NULL;
}

bool resctrl_arch_is_evt_configurable(enum resctrl_event_id evt)
{
	return false;
}

int resctrl_arch_mon_ctx_alloc(struct rdt_resource *r, int evtid)
{
	/* RISC-V can always read an rmid, nothing needs allocating.
	 * v6.2 backport: Morse's signature returns int (a handle/errno),
	 * not void*; 0 is the success value. */
	return 0;
}

void resctrl_arch_mon_ctx_free(struct rdt_resource *r, int evtid,
			       int arch_mon_ctx)
{
	/* not implemented for the RISC-V resctrl interface */
}

/*
 * Called on umount: put every RCID of every alloc-capable resource back to
 * its default (all cache blocks / the maximum reservable bandwidth). The
 * per-CPU and per-task srmcfg values are reset by fs/resctrl itself.
 */
void resctrl_arch_reset_resources(void)
{
	struct rdt_resource *r;
	struct rdt_domain *d;
	u32 closid, num_closid, def;
	int i, err;

	for (i = 0; i < RDT_NUM_RESOURCES; i++) {
		r = &cbqri_resctrl_resources[i].resctrl_res;
		if (!r->alloc_capable)
			continue;
		def = resctrl_get_default_ctrl(r);
		list_for_each_entry(d, &r->domains, list) {
			num_closid = cbqri_dom_num_rcid(d);
			for (closid = 0; closid < num_closid; closid++) {
				err = resctrl_arch_update_one(r, d, closid,
							      CDP_NONE, def);
				if (err)
					pr_warn("%s(): %s domain %d rcid %u: err %d",
						__func__, r->name, d->id,
						closid, err);
			}
		}
	}
}

void resctrl_arch_config_cntr(struct rdt_resource *r, struct rdt_domain *d,
			      enum resctrl_event_id evtid, u32 rmid, u32 closid,
			      u32 cntr_id, bool assign)
{
	/* not implemented for the RISC-V resctrl implementation */
}

int resctrl_arch_cntr_read(struct rdt_resource *r, struct rdt_domain *d,
			   u32 unused, u32 rmid, int cntr_id,
			   enum resctrl_event_id eventid, u64 *val)
{
	/* not implemented for the RISC-V resctrl implementation */
	return 0;
}

bool resctrl_arch_mbm_cntr_assign_enabled(struct rdt_resource *r)
{
	/* not implemented for the RISC-V resctrl implementation */
	return false;
}

int resctrl_arch_mbm_cntr_assign_set(struct rdt_resource *r, bool enable)
{
	/* not implemented for the RISC-V resctrl implementation */
	return 0;
}

void resctrl_arch_reset_cntr(struct rdt_resource *r, struct rdt_domain *d,
			     u32 unused, u32 rmid, int cntr_id,
			     enum resctrl_event_id eventid)
{
	/* not implemented for the RISC-V resctrl implementation */
}

bool resctrl_arch_get_io_alloc_enabled(struct rdt_resource *r)
{
	/* not implemented for the RISC-V resctrl implementation */
	return false;
}

int resctrl_arch_io_alloc_enable(struct rdt_resource *r, bool enable)
{
	/* not implemented for the RISC-V resctrl implementation */
	return 0;
}

/*
 * Note about terminology between x86 (Intel RDT/AMD QoS) and RISC-V:
 *   CLOSID on x86 is RCID on RISC-V
 *     RMID on x86 is MCID on RISC-V
 */
u32 resctrl_arch_get_num_closid(struct rdt_resource *res)
{
	struct cbqri_resctrl_res *hw_res;

	hw_res = container_of(res, struct cbqri_resctrl_res, resctrl_res);

	return hw_res->max_rcid;
}

u32 resctrl_arch_system_num_rmid_idx(void)
{
	return max_rmid;
}

u32 resctrl_arch_rmid_idx_encode(u32 closid, u32 rmid)
{
	return rmid;
}

void resctrl_arch_rmid_idx_decode(u32 idx, u32 *closid, u32 *rmid)
{
	*closid = ((u32)~0); /* refer to X86_RESCTRL_BAD_CLOSID */
	*rmid = idx;
}

/*
 * Per-CPU default RCID/MCID, used for tasks whose own value is zero (the
 * default group). This is what the "cpus" / "cpus_list" files control.
 * The CSR itself is rewritten by resctrl_arch_sync_cpu_defaults() ->
 * resctrl_arch_sched_in(current) on the target CPU.
 */
void resctrl_arch_set_cpu_default_closid_rmid(int cpu, u32 closid, u32 rmid)
{
	WARN_ON_ONCE((closid & SRMCFG_RCID_MASK) != closid);
	WARN_ON_ONCE((rmid & SRMCFG_MCID_MASK) != rmid);

	per_cpu(cpu_default_srmcfg, cpu) = (rmid << SRMCFG_MCID_SHIFT) | closid;
}

void resctrl_arch_sched_in(struct task_struct *tsk)
{
	__switch_to_srmcfg(tsk);
}

void resctrl_arch_set_closid_rmid(struct task_struct *tsk, u32 closid, u32 rmid)
{
	u32 srmcfg;

	WARN_ON_ONCE((closid & SRMCFG_RCID_MASK) != closid);
	WARN_ON_ONCE((rmid & SRMCFG_MCID_MASK) != rmid);

	srmcfg = rmid << SRMCFG_MCID_SHIFT;
	srmcfg |= closid;
	WRITE_ONCE(tsk->thread.srmcfg, srmcfg);
}

void resctrl_arch_sync_cpu_defaults(void *info)
{
	struct resctrl_cpu_sync *r = info;

	lockdep_assert_preemption_disabled();

	if (r) {
		resctrl_arch_set_cpu_default_closid_rmid(smp_processor_id(),
							 r->closid, r->rmid);
	}

	resctrl_arch_sched_in(current);
}

bool resctrl_arch_match_closid(struct task_struct *tsk, u32 closid)
{
	u32 srmcfg;
	bool match;

	srmcfg = READ_ONCE(tsk->thread.srmcfg);
	match = (srmcfg & SRMCFG_RCID_MASK) == closid;
	return match;
}

bool resctrl_arch_match_rmid(struct task_struct *tsk, u32 closid, u32 rmid)
{
	u32 tsk_rmid;

	tsk_rmid = READ_ONCE(tsk->thread.srmcfg);
	tsk_rmid >>= SRMCFG_MCID_SHIFT;
	tsk_rmid &= SRMCFG_MCID_MASK;

	return tsk_rmid == rmid;
}

/*
 * mbm_total_bytes: the bandwidth controller keeps a free-running 62-bit
 * count of 64-byte line fills per MCID (started by a CONFIG_EVENT op, see
 * resctrl_arch_reset_rmid()). Return it in bytes; fs/resctrl keeps the
 * per-group baseline and computes rates. Serialised by rdtgroup_mutex like
 * every other access to the controller's registers.
 */
int resctrl_arch_rmid_read(struct rdt_resource *r, struct rdt_domain *d,
			   u32 closid, u32 rmid, enum resctrl_event_id eventid,
			   u64 *val, int arch_mon_ctx)
{
	struct cbqri_resctrl_dom *hw_dom;
	struct cbqri_controller *ctrl;
	u64 reg;
	int err;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	ctrl = hw_dom->hw_ctrl;

	if (eventid != QOS_L3_MBM_TOTAL_EVENT_ID ||
	    ctrl->ctrl_info->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH ||
	    !ctrl->mon_capable)
		return -EINVAL;

	err = cbqri_bc_mon_op(ctrl, CBQRI_BC_MON_CTL_OP_READ_COUNTER, rmid,
			      CBQRI_BC_MON_EVT_RDWR_COUNT);
	if (err)
		return err;

	reg = ioread64(ctrl->base + CBQRI_BC_MON_CTR_VAL_OFF);
	if (reg & CBQRI_BC_MON_CTR_INV)
		return -EINVAL;

	*val = (reg & CBQRI_BC_MON_CTR_VAL_MASK) * CBQRI_FS_LINE_BYTES;
	return 0;
}

/* Re-arm the MCID's counter: CONFIG_EVENT zeroes it and starts counting. */
void resctrl_arch_reset_rmid(struct rdt_resource *r, struct rdt_domain *d,
			     u32 closid, u32 rmid, enum resctrl_event_id eventid)
{
	struct cbqri_resctrl_dom *hw_dom;
	struct cbqri_controller *ctrl;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	ctrl = hw_dom->hw_ctrl;

	if (eventid != QOS_L3_MBM_TOTAL_EVENT_ID ||
	    ctrl->ctrl_info->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH ||
	    !ctrl->mon_capable)
		return;

	cbqri_bc_mon_op(ctrl, CBQRI_BC_MON_CTL_OP_CONFIG_EVENT, rmid,
			CBQRI_BC_MON_EVT_RDWR_COUNT);
}

void resctrl_arch_mon_event_config_read(void *info)
{
	/* not implemented for the RISC-V resctrl interface */
}

void resctrl_arch_mon_event_config_write(void *info)
{
	/* not implemented for the RISC-V resctrl interface */
}

void resctrl_arch_reset_rmid_all(struct rdt_resource *r, struct rdt_domain *d)
{
	struct cbqri_resctrl_dom *hw_dom;
	u32 mcid;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	for (mcid = 0; mcid < hw_dom->hw_ctrl->ctrl_info->mcid_count; mcid++)
		resctrl_arch_reset_rmid(r, d, 0, mcid, QOS_L3_MBM_TOTAL_EVENT_ID);
}

void resctrl_arch_reset_all_ctrls(struct rdt_resource *r)
{
	struct rdt_domain *d;
	u32 closid, num_closid;
	u32 def = resctrl_get_default_ctrl(r);

	if (!r->alloc_capable)
		return;
	list_for_each_entry(d, &r->domains, list) {
		num_closid = cbqri_dom_num_rcid(d);
		for (closid = 0; closid < num_closid; closid++)
			resctrl_arch_update_one(r, d, closid, CDP_NONE, def);
	}
}

/* Set capacity block mask (cc_block_mask) */
static void cbqri_set_cbm(struct cbqri_controller *ctrl, u64 cbm)
{
		int reg_offset;
		u64 reg;

		reg_offset = CBQRI_CC_BLOCK_MASK_OFF;
		reg = ioread64(ctrl->base + reg_offset);

		reg = cbm;
		iowrite64(reg, ctrl->base + reg_offset);
}

/* Set the Rbwb (reserved bandwidth blocks) field in bc_bw_alloc */
static void cbqri_set_rbwb(struct cbqri_controller *ctrl, u64 rbwb)
{
		int reg_offset;
		u64 reg;

		reg_offset = CBQRI_BC_BW_ALLOC_OFF;
		reg = ioread64(ctrl->base + reg_offset);
		reg &= ~CBQRI_CONTROL_REGISTERS_RBWB_MASK;
		rbwb &= CBQRI_CONTROL_REGISTERS_RBWB_MASK;
		reg |= rbwb;
		iowrite64(reg, ctrl->base + reg_offset);
}

/* Get the Rbwb (reserved bandwidth blocks) field in bc_bw_alloc */
static u64 cbqri_get_rbwb(struct cbqri_controller *ctrl)
{
		int reg_offset;
		u64 reg;

		reg_offset = CBQRI_BC_BW_ALLOC_OFF;
		reg = ioread64(ctrl->base + reg_offset);
		reg &= CBQRI_CONTROL_REGISTERS_RBWB_MASK;
		return reg;
}

static int cbqri_wait_busy_flag(struct cbqri_controller *ctrl, int reg_offset)
{
	unsigned long timeout = jiffies + (HZ / 10); /* Timeout after 100ms */
	int busy;
	u64 reg;

	while (time_before(jiffies, timeout)) {
		reg = ioread64(ctrl->base + reg_offset);
		busy = (reg >> CBQRI_CONTROL_REGISTERS_BUSY_SHIFT) &
			CBQRI_CONTROL_REGISTERS_BUSY_MASK;
		if (!busy)
			return 0;
	}

	pr_warn("%s(): busy timeout", __func__);
	return -EIO;
}

/* Perform capacity allocation control operation on capacity controller */
static int cbqri_cc_alloc_op(struct cbqri_controller *ctrl, int operation, int rcid,
			     enum resctrl_conf_type type)
{
	int reg_offset = CBQRI_CC_ALLOC_CTL_OFF;
	int status;
	u64 reg;

	reg = ioread64(ctrl->base + reg_offset);
	reg &= ~(CBQRI_CONTROL_REGISTERS_OP_MASK << CBQRI_CONTROL_REGISTERS_OP_SHIFT);
	reg |= (operation & CBQRI_CONTROL_REGISTERS_OP_MASK) <<
		CBQRI_CONTROL_REGISTERS_OP_SHIFT;
	reg &= ~(CBQRI_CONTROL_REGISTERS_RCID_MASK <<
		 CBQRI_CONTROL_REGISTERS_RCID_SHIFT);
	reg |= (rcid & CBQRI_CONTROL_REGISTERS_RCID_MASK) <<
		CBQRI_CONTROL_REGISTERS_RCID_SHIFT;

	/* CBQRI capacity AT is only supported on L2 and L3 caches for now */
	if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY &&
	    ((ctrl->ctrl_info->cache.cache_level == 2 && is_cdp_l2_enabled) ||
	    (ctrl->ctrl_info->cache.cache_level == 3 && is_cdp_l3_enabled))) {
		reg &= ~(CBQRI_CONTROL_REGISTERS_AT_MASK <<
			 CBQRI_CONTROL_REGISTERS_AT_SHIFT);
		switch (type) {
		case CDP_CODE:
			reg |= (CBQRI_CONTROL_REGISTERS_AT_CODE &
				CBQRI_CONTROL_REGISTERS_AT_MASK) <<
				CBQRI_CONTROL_REGISTERS_AT_SHIFT;
			break;
		case CDP_DATA:
		default:
			reg |= (CBQRI_CONTROL_REGISTERS_AT_DATA &
				CBQRI_CONTROL_REGISTERS_AT_MASK) <<
				CBQRI_CONTROL_REGISTERS_AT_SHIFT;
			break;
		}
	}

	iowrite64(reg, ctrl->base + reg_offset);

	if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
		pr_err("%s(): BUSY timeout when executing the operation", __func__);
		return -EIO;
	}

	reg = ioread64(ctrl->base + reg_offset);
	status = (reg >> CBQRI_CONTROL_REGISTERS_STATUS_SHIFT) &
		  CBQRI_CONTROL_REGISTERS_STATUS_MASK;
	if (status != 1) {
		pr_err("%s(): operation %d failed: status=%d", __func__, operation, status);
		return -EIO;
	}

	return 0;
}

static int cbqri_apply_cache_config(struct cbqri_resctrl_dom *hw_dom, u32 closid,
				    enum resctrl_conf_type type, struct cbqri_config *cfg)
{
	struct cbqri_controller *ctrl = hw_dom->hw_ctrl;
	int reg_offset;
	int err = 0;
	u64 reg;

	if (cfg->cbm != hw_dom->ctrl_val[closid]) {
		/* Store the new cbm in the ctrl_val array for this closid in this domain */
		hw_dom->ctrl_val[closid] = cfg->cbm;

		/* Set capacity block mask (cc_block_mask) */
		cbqri_set_cbm(ctrl, cfg->cbm);

		/* Capacity config limit operation */
		err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT, closid, type);
		if (err < 0) {
			pr_err("%s(): operation failed: err = %d", __func__, err);
			return err;
		}

		/* Clear cc_block_mask before read limit to verify op works*/
		cbqri_set_cbm(ctrl, 0);

		/* Performa capacity read limit operation to verify blockmask */
		err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT, closid, type);
		if (err < 0) {
			pr_err("%s(): operation failed: err = %d", __func__, err);
			return err;
		}

		/* Read capacity blockmask to verify it matches the requested config */
		reg_offset = CBQRI_CC_BLOCK_MASK_OFF;
		reg = ioread64(ctrl->base + reg_offset);
		if (reg != cfg->cbm) {
			pr_warn("%s(): failed to verify allocation (reg:%llx != cbm:%llx)",
				__func__, reg, cfg->cbm);
			return -EIO;
		}
	}

	return err;
}

/* Perform bandwidth allocation control operation on bandwidth controller */
static int cbqri_bc_alloc_op(struct cbqri_controller *ctrl, int operation, int rcid)
{
	int reg_offset = CBQRI_BC_ALLOC_CTL_OFF;
	int status;
	u64 reg;

	reg = ioread64(ctrl->base + reg_offset);
	reg &= ~(CBQRI_CONTROL_REGISTERS_OP_MASK << CBQRI_CONTROL_REGISTERS_OP_SHIFT);
	reg |=  (operation & CBQRI_CONTROL_REGISTERS_OP_MASK) <<
		 CBQRI_CONTROL_REGISTERS_OP_SHIFT;
	reg &= ~(CBQRI_CONTROL_REGISTERS_RCID_MASK << CBQRI_CONTROL_REGISTERS_RCID_SHIFT);
	reg |=  (rcid & CBQRI_CONTROL_REGISTERS_RCID_MASK) <<
		 CBQRI_CONTROL_REGISTERS_RCID_SHIFT;
	iowrite64(reg, ctrl->base + reg_offset);

	if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
		pr_err("%s(): BUSY timeout when executing the operation", __func__);
		return -EIO;
	}

	reg = ioread64(ctrl->base + reg_offset);
	status = (reg >> CBQRI_CONTROL_REGISTERS_STATUS_SHIFT) &
		  CBQRI_CONTROL_REGISTERS_STATUS_MASK;
	if (status != 1) {
		pr_err("%s(): operation %d failed with status = %d",
		       __func__, operation, status);
		return -EIO;
	}

	return 0;
}

/* Perform a bandwidth monitoring control operation on a bandwidth controller */
static int cbqri_bc_mon_op(struct cbqri_controller *ctrl, int operation,
			   u32 mcid, u32 evt_id)
{
	int reg_offset = CBQRI_BC_MON_CTL_OFF;
	int status;
	u64 reg;

	reg = ioread64(ctrl->base + reg_offset);
	reg &= ~((u64)CBQRI_CONTROL_REGISTERS_OP_MASK << CBQRI_CONTROL_REGISTERS_OP_SHIFT);
	reg |= (u64)(operation & CBQRI_CONTROL_REGISTERS_OP_MASK) <<
		CBQRI_CONTROL_REGISTERS_OP_SHIFT;
	reg &= ~((u64)CBQRI_MON_CTL_MCID_MASK << CBQRI_MON_CTL_MCID_SHIFT);
	reg |= (u64)(mcid & CBQRI_MON_CTL_MCID_MASK) << CBQRI_MON_CTL_MCID_SHIFT;
	reg &= ~((u64)CBQRI_MON_CTL_EVT_ID_MASK << CBQRI_MON_CTL_EVT_ID_SHIFT);
	reg |= (u64)(evt_id & CBQRI_MON_CTL_EVT_ID_MASK) << CBQRI_MON_CTL_EVT_ID_SHIFT;
	iowrite64(reg, ctrl->base + reg_offset);

	if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
		pr_err("%s(): BUSY timeout when executing the operation", __func__);
		return -EIO;
	}

	reg = ioread64(ctrl->base + reg_offset);
	status = (reg >> CBQRI_CONTROL_REGISTERS_STATUS_SHIFT) &
		  CBQRI_CONTROL_REGISTERS_STATUS_MASK;
	if (status != CBQRI_BC_MON_CTL_STATUS_SUCCESS) {
		pr_err("%s(): operation %d mcid %u failed with status = %d",
		       __func__, operation, mcid, status);
		return -EIO;
	}

	return 0;
}

static int cbqri_apply_bw_config(struct cbqri_resctrl_dom *hw_dom, u32 closid,
				 enum resctrl_conf_type type, struct cbqri_config *cfg)
{
	struct cbqri_controller *ctrl = hw_dom->hw_ctrl;
	int ret = 0;
	u64 reg;

	if (cfg->rbwb != hw_dom->ctrl_val[closid]) {
		/* Store the new rbwb in the ctrl_val array for this closid in this domain */
		hw_dom->ctrl_val[closid] = cfg->rbwb;

		/* Set reserved bandwidth blocks */
		cbqri_set_rbwb(ctrl, cfg->rbwb);

		/* Bandwidth config limit operation */
		ret = cbqri_bc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT, closid);
		if (ret < 0) {
			pr_err("%s(): operation failed: ret = %d", __func__, ret);
			return ret;
		}

		/* Clear rbwb before read limit to verify op works*/
		cbqri_set_rbwb(ctrl, 0);

		/* Bandwidth allocation read limit operation to verify */
		ret = cbqri_bc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT, closid);
		if (ret < 0) {
			pr_err("%s(): operation failed: ret = %d", __func__, ret);
			return ret;
		}

		/* Read bandwidth allocation to verify it matches the requested config */
		reg = cbqri_get_rbwb(ctrl);
		if (reg != cfg->rbwb) {
			pr_warn("%s(): failed to verify allocation (reg:%llx != rbwb:%llu)",
				__func__, reg, cfg->rbwb);
			return -EIO;
		}
	}

	return ret;
}

int resctrl_arch_update_one(struct rdt_resource *r, struct rdt_domain *d,
			    u32 closid, enum resctrl_conf_type t, u32 cfg_val)
{
	struct cbqri_controller *ctrl;
	struct cbqri_resctrl_dom *dom;
	struct cbqri_config cfg;
	int err = 0;

	dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	ctrl = dom->hw_ctrl;

	if (!r->alloc_capable)
		return -EINVAL;

	switch (r->rid) {
	case RDT_RESOURCE_L2:
	case RDT_RESOURCE_L3:
		cfg.cbm = cfg_val;
		err = cbqri_apply_cache_config(dom, closid, t, &cfg);
		break;
	case RDT_RESOURCE_MBA:
		/* covert from percentage to bandwidth blocks */
		cfg.rbwb = cfg_val * ctrl->bc.nbwblks / 100;
		err = cbqri_apply_bw_config(dom, closid, t, &cfg);
		break;
	default:
		return -EINVAL;
	}

	return err;
}

int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid)
{
	struct resctrl_staged_config *cfg;
	enum resctrl_conf_type t;
	struct rdt_domain *d;
	int err = 0;

	list_for_each_entry(d, &r->domains, list) {
		for (t = 0; t < CDP_NUM_TYPES; t++) {
			cfg = &d->staged_config[t];
			if (!cfg->have_new_ctrl)
				continue;
			err = resctrl_arch_update_one(r, d, closid, t, cfg->new_ctrl);
			if (err) {
				pr_warn("%s(): update failed (err=%d)", __func__, err);
				return err;
			}
		}
	}
	return err;
}

u32 resctrl_arch_get_config(struct rdt_resource *r, struct rdt_domain *d,
			    u32 closid, enum resctrl_conf_type type)
{
	struct cbqri_resctrl_dom *hw_dom;
	struct cbqri_controller *ctrl;
	int reg_offset;
	u32 percent;
	u32 rbwb;
	u64 reg;
	int err;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);

	ctrl = hw_dom->hw_ctrl;

	if (!r->alloc_capable)
		return -EINVAL;

	switch (r->rid) {
	case RDT_RESOURCE_L2:
	case RDT_RESOURCE_L3:
		/* Clear cc_block_mask before read limit operation */
		cbqri_set_cbm(ctrl, 0);

		/* Capacity read limit operation for RCID (closid).
		 * cbqri_cc_alloc_op(ctrl, operation, rcid, type): pass closid as the
		 * RCID and type as the access-type. These were swapped, which made
		 * the cache schemata read-back always query RCID 0 (type==CDP_NONE==0),
		 * so every sub-group's L2/L3 line showed the root group's value even
		 * though the write path programmed the correct per-RCID limit. */
		err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT, closid, type);
		if (err < 0) {
			pr_err("%s(): operation failed: err = %d", __func__, err);
			return -EIO;
		}

		/* Read capacity block mask for RCID (closid) */
		reg_offset = CBQRI_CC_BLOCK_MASK_OFF;
		reg = ioread64(ctrl->base + reg_offset);

		/* Update the config value for the closid in this domain */
		hw_dom->ctrl_val[closid] = reg;
		return hw_dom->ctrl_val[closid];

	case RDT_RESOURCE_MBA:
		/* Capacity read limit operation for RCID (closid) */
		err = cbqri_bc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT, closid);
		if (err < 0) {
			pr_err("%s(): operation failed: err = %d", __func__, err);
			return -EIO;
		}

		hw_dom->ctrl_val[closid] = cbqri_get_rbwb(ctrl);

		/* Convert from bandwidth blocks to percent */
		rbwb = hw_dom->ctrl_val[closid];
		rbwb *= 100;
		percent = rbwb / ctrl->bc.nbwblks;
		if (rbwb % ctrl->bc.nbwblks)
			percent++;
		return percent;

	default:
		return -EINVAL;
	}
}

static int cbqri_probe_feature(struct cbqri_controller *ctrl, int reg_offset,
			       int operation, int *status, bool *access_type_supported)
{
	u64 reg, saved_reg;
	int at;

	/* Keep the initial register value to preserve the WPRI fields */
	reg = ioread64(ctrl->base + reg_offset);
	saved_reg = reg;

	/* Execute the requested operation to find if the register is implemented */
	reg &= ~(CBQRI_CONTROL_REGISTERS_OP_MASK << CBQRI_CONTROL_REGISTERS_OP_SHIFT);
	reg |= (operation & CBQRI_CONTROL_REGISTERS_OP_MASK) << CBQRI_CONTROL_REGISTERS_OP_SHIFT;
	iowrite64(reg, ctrl->base + reg_offset);
	if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
		pr_err("%s(): BUSY timeout when executing the operation", __func__);
		return -EIO;
	}

	/* Get the operation status */
	reg = ioread64(ctrl->base + reg_offset);
	*status = (reg >> CBQRI_CONTROL_REGISTERS_STATUS_SHIFT) &
		   CBQRI_CONTROL_REGISTERS_STATUS_MASK;

	/*
	 * Check for the AT support if the register is implemented
	 * (if not, the status value will remain 0)
	 */
	if (*status != 0) {
		/* Set the AT field to a valid value */
		reg = saved_reg;
		reg &= ~(CBQRI_CONTROL_REGISTERS_AT_MASK << CBQRI_CONTROL_REGISTERS_AT_SHIFT);
		reg |= CBQRI_CONTROL_REGISTERS_AT_CODE << CBQRI_CONTROL_REGISTERS_AT_SHIFT;
		iowrite64(reg, ctrl->base + reg_offset);
		if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
			pr_err("%s(): BUSY timeout when setting AT field", __func__);
			return -EIO;
		}

		/*
		 * If the AT field value has been reset to zero,
		 * then the AT support is not present
		 */
		reg = ioread64(ctrl->base + reg_offset);
		at = (reg >> CBQRI_CONTROL_REGISTERS_AT_SHIFT) & CBQRI_CONTROL_REGISTERS_AT_MASK;
		if (at == CBQRI_CONTROL_REGISTERS_AT_CODE)
			*access_type_supported = true;
		else
			*access_type_supported = false;
	}

	/* Restore the original register value */
	iowrite64(saved_reg, ctrl->base + reg_offset);
	if (cbqri_wait_busy_flag(ctrl, reg_offset) < 0) {
		pr_err("%s(): BUSY timeout when restoring the original register value", __func__);
		return -EIO;
	}

	return 0;
}

/*
 * Note: for the purposes of the CBQRI proof-of-concept, debug logging
 * has been left in this function that detects the properties of CBQRI
 * capable controllers in the system. pr_info calls would be removed
 * before submitting non-RFC patches.
 */
static int cbqri_probe_controller(struct cbqri_controller_info *ctrl_info,
				  struct cbqri_controller *ctrl)
{
	int err = 0, status;
	u64 reg;

	pr_info("controller info: type=%d addr=0x%lx size=%lu max-rcid=%u max-mcid=%u",
		ctrl_info->type, ctrl_info->addr, ctrl_info->size,
		ctrl_info->rcid_count, ctrl_info->mcid_count);

	/* max_rmid is used by resctrl_arch_system_num_rmid_idx(): smallest MCID space */
	if (!max_rmid || ctrl_info->mcid_count < max_rmid)
		max_rmid = ctrl_info->mcid_count;

	ctrl->ctrl_info = ctrl_info;

	/* Try to access the memory-mapped CBQRI registers */
	if (!request_mem_region(ctrl_info->addr, ctrl_info->size, "cbqri_controller")) {
		pr_warn("%s(): return %d", __func__, err);
		return err;
	}
	ctrl->base = ioremap(ctrl_info->addr, ctrl_info->size);
	if (!ctrl->base) {
		pr_warn("%s(): goto err_release_mem_region", __func__);
		goto err_release_mem_region;
	}

	ctrl->alloc_capable = false;
	ctrl->mon_capable = false;

	/* Probe capacity allocation and monitoring features */
	if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY) {
		pr_info("probe capacity controller");

		/* Make sure the register is implemented */
		reg = ioread64(ctrl->base + CBQRI_CC_CAPABILITIES_OFF);
		if (reg == 0) {
			err = -ENODEV;
			goto err_iounmap;
		}

		ctrl->ver_minor = reg & CBQRI_CC_CAPABILITIES_VER_MINOR_MASK;
		ctrl->ver_major = reg & CBQRI_CC_CAPABILITIES_VER_MAJOR_MASK;

		ctrl->cc.supports_alloc_op_flush_rcid = (reg >> CBQRI_CC_CAPABILITIES_FRCID_SHIFT)
			& CBQRI_CC_CAPABILITIES_FRCID_MASK;

		ctrl->cc.ncblks = (reg >> CBQRI_CC_CAPABILITIES_NCBLKS_SHIFT) &
				   CBQRI_CC_CAPABILITIES_NCBLKS_MASK;

		/* Calculate size of capacity block in bytes */
		ctrl->cc.blk_size = ctrl_info->cache.cache_size / ctrl->cc.ncblks;
		ctrl->cc.cache_level = ctrl_info->cache.cache_level;

		pr_info("version=%d.%d ncblks=%d blk_size=%d cache_level=%d",
			ctrl->ver_major, ctrl->ver_minor,
			ctrl->cc.ncblks, ctrl->cc.blk_size, ctrl->cc.cache_level);

		/* Probe monitoring features */
		err = cbqri_probe_feature(ctrl, CBQRI_CC_MON_CTL_OFF,
					  CBQRI_CC_MON_CTL_OP_READ_COUNTER, &status,
					  &ctrl->cc.supports_mon_at_code);
		if (err) {
			pr_warn("%s() failed to probe cc_mon_ctl feature", __func__);
			goto err_iounmap;
		}

		if (status == CBQRI_CC_MON_CTL_STATUS_SUCCESS) {
			pr_info("cc_mon_ctl is supported");
			ctrl->cc.supports_mon_op_config_event = true;
			ctrl->cc.supports_mon_op_read_counter = true;
			ctrl->mon_capable = true;
		} else {
			pr_info("cc_mon_ctl is NOT supported");
			ctrl->cc.supports_mon_op_config_event = false;
			ctrl->cc.supports_mon_op_read_counter = false;
			ctrl->mon_capable = false;
		}
		/*
		 * AT data is "always" supported as it has the same value
		 * than when AT field is not supported.
		 */
		ctrl->cc.supports_mon_at_data = true;
		pr_info("supports_mon_at_data: %d, supports_mon_at_code: %d",
			ctrl->cc.supports_mon_at_data, ctrl->cc.supports_mon_at_code);

		/* Probe allocation features */
		err = cbqri_probe_feature(ctrl, CBQRI_CC_ALLOC_CTL_OFF,
					  CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
					  &status, &ctrl->cc.supports_alloc_at_code);
		if (err) {
			pr_warn("%s() failed to probe cc_alloc_ctl feature", __func__);
			goto err_iounmap;
		}

		if (status == CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS) {
			pr_info("cc_alloc_ctl is supported");
			ctrl->cc.supports_alloc_op_config_limit = true;
			ctrl->cc.supports_alloc_op_read_limit = true;
			ctrl->alloc_capable = true;
			exposed_alloc_capable = true;
		} else {
			pr_info("cc_alloc_ctl is NOT supported");
			ctrl->cc.supports_alloc_op_config_limit = false;
			ctrl->cc.supports_alloc_op_read_limit = false;
			ctrl->alloc_capable = false;
		}
		/*
		 * AT data is "always" supported as it has the same value
		 * than when AT field is not supported
		 */
		ctrl->cc.supports_alloc_at_data = true;
		pr_info("supports_alloc_at_data: %d, supports_alloc_at_code: %d",
			ctrl->cc.supports_alloc_at_data,
			ctrl->cc.supports_alloc_at_code);
	} else if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH) {
		pr_info("probe bandwidth controller");

		/* Make sure the register is implemented */
		reg = ioread64(ctrl->base + CBQRI_BC_CAPABILITIES_OFF);
		if (reg == 0) {
			err = -ENODEV;
			goto err_iounmap;
		}

		ctrl->ver_minor = reg & CBQRI_BC_CAPABILITIES_VER_MINOR_MASK;
		ctrl->ver_major = reg & CBQRI_BC_CAPABILITIES_VER_MAJOR_MASK;

		ctrl->bc.nbwblks = (reg >> CBQRI_BC_CAPABILITIES_NBWBLKS_SHIFT) &
				    CBQRI_BC_CAPABILITIES_NBWBLKS_MASK;
		ctrl->bc.mrbwb = (reg >> CBQRI_BC_CAPABILITIES_MRBWB_SHIFT) &
				  CBQRI_BC_CAPABILITIES_MRBWB_MASK;

		pr_info("version=%d.%d nbwblks=%d mrbwb=%d",
			ctrl->ver_major, ctrl->ver_minor,
			ctrl->bc.nbwblks, ctrl->bc.mrbwb);

		/* Probe monitoring features */
		err = cbqri_probe_feature(ctrl, CBQRI_BC_MON_CTL_OFF,
					  CBQRI_BC_MON_CTL_OP_READ_COUNTER,
					  &status, &ctrl->bc.supports_mon_at_code);
		if (err) {
			pr_warn("%s() failed to probe bc_mon_ctl feature", __func__);
			goto err_iounmap;
		}

		if (status == CBQRI_BC_MON_CTL_STATUS_SUCCESS) {
			pr_info("bc_mon_ctl is supported");
			ctrl->bc.supports_mon_op_config_event = true;
			ctrl->bc.supports_mon_op_read_counter = true;
			ctrl->mon_capable = true;
			exposed_mon_capable = true;
		} else {
			pr_info("bc_mon_ctl is NOT supported");
			ctrl->bc.supports_mon_op_config_event = false;
			ctrl->bc.supports_mon_op_read_counter = false;
			ctrl->mon_capable = false;
		}
		/*
		 * AT data is "always" supported as it has the same value
		 * than when AT field is not supported
		 */
		ctrl->bc.supports_mon_at_data = true;
		pr_info("supports_mon_at_data: %d, supports_mon_at_code: %d",
			ctrl->bc.supports_mon_at_data, ctrl->bc.supports_mon_at_code);

		/* Probe allocation features */
		err = cbqri_probe_feature(ctrl, CBQRI_BC_ALLOC_CTL_OFF,
					  CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT,
					  &status, &ctrl->bc.supports_alloc_at_code);
		if (err) {
			pr_warn("%s() failed to probe bc_alloc_ctl feature", __func__);
			goto err_iounmap;
		}

		if (status == CBQRI_BC_ALLOC_CTL_STATUS_SUCCESS) {
			pr_warn("bc_alloc_ctl is supported");
			ctrl->bc.supports_alloc_op_config_limit = true;
			ctrl->bc.supports_alloc_op_read_limit = true;
			ctrl->alloc_capable = true;
			exposed_alloc_capable = true;
		} else {
			pr_warn("bc_alloc_ctl is NOT supported");
			ctrl->bc.supports_alloc_op_config_limit = false;
			ctrl->bc.supports_alloc_op_read_limit = false;
			ctrl->alloc_capable = false;
		}

		/*
		 * AT data is "always" supported as it has the same value
		 * than when AT field is not supported
		 */
		ctrl->bc.supports_alloc_at_data = true;
		pr_warn("supports_alloc_at_data: %d, supports_alloc_at_code: %d",
			ctrl->bc.supports_alloc_at_data, ctrl->bc.supports_alloc_at_code);
	} else {
		pr_warn("controller type is UNKNOWN");
		err = -ENODEV;
		goto err_release_mem_region;
	}

	return 0;

err_iounmap:
	pr_warn("%s(): err_iounmap", __func__);
	iounmap(ctrl->base);

err_release_mem_region:
	pr_warn("%s(): err_release_mem_region", __func__);
	release_mem_region(ctrl_info->addr, ctrl_info->size);

	return err;
}

static struct rdt_domain *qos_new_domain(struct cbqri_controller *ctrl)
{
	struct cbqri_resctrl_dom *hw_dom;
	struct rdt_domain *domain;

	hw_dom = kzalloc(sizeof(*hw_dom), GFP_KERNEL);
	if (!hw_dom)
		return NULL;

	/* associate this cbqri_controller with the domain */
	hw_dom->hw_ctrl = ctrl;

	/* the rdt_domain struct from inside the cbqri_resctrl_dom struct */
	domain = &hw_dom->resctrl_dom;

	INIT_LIST_HEAD(&domain->list);

	return domain;
}

static int qos_init_domain_ctrlval(struct rdt_resource *r, struct rdt_domain *d)
{
	struct cbqri_resctrl_res *hw_res;
	struct cbqri_resctrl_dom *hw_dom;
	u32 num_rcid;
	u64 *dc;
	int err = 0;
	int i;

	hw_res = container_of(r, struct cbqri_resctrl_res, resctrl_res);
	if (!hw_res)
		return -ENOMEM;

	hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
	if (!hw_dom)
		return -ENOMEM;

	/* program every RCID the controller has, not just the ones resctrl uses */
	num_rcid = cbqri_dom_num_rcid(d);
	dc = kmalloc_array(num_rcid, sizeof(*hw_dom->ctrl_val), GFP_KERNEL);
	if (!dc)
		return -ENOMEM;

	hw_dom->ctrl_val = dc;

	/*
	 * Give every RCID its default. This must succeed for all of them:
	 * on the FireSim regulators an RCID whose budget was never set stalls
	 * on its first request once regulation is enabled.
	 * resctrl_arch_update_one() records the programmed value (in blocks)
	 * in ctrl_val[]; nothing else must overwrite it with a percentage.
	 */
	for (i = 0; i < num_rcid; i++) {
		err = resctrl_arch_update_one(r, d, i, 0, resctrl_get_default_ctrl(r));
		if (err) {
			pr_err("%s(): %s domain %d: rcid %d default failed (%d)",
			       __func__, r->name, d->id, i, err);
			kfree(dc);
			hw_dom->ctrl_val = NULL;
			return err;
		}
	}
	return 0;
}

/*
 * Bandwidth controllers with per-MCID counters are exposed to fs/resctrl the
 * way x86 does it: as monitoring domains of the L3 resource, which is where
 * resctrl looks for mbm_total_bytes. The domain shares the controller with
 * the MB (allocation) domain of the same id.
 */
static int qos_resctrl_add_bw_mon_domain(struct cbqri_controller *ctrl, int id)
{
	struct cbqri_resctrl_res *l3 = &cbqri_resctrl_resources[RDT_RESOURCE_L3];
	struct rdt_resource *res = &l3->resctrl_res;
	struct rdt_domain *domain;
	u32 mcid;
	int err;

	domain = qos_new_domain(ctrl);
	if (!domain)
		return -ENOMEM;
	domain->id = id;
	cpumask_copy(&domain->cpu_mask, cpu_online_mask);

	if (!res->name) {
		/* no L3 capacity controller filled this in */
		res->rid = RDT_RESOURCE_L3;
		res->name = "L3";
		res->cache_level = 3;
		res->fflags = RFTYPE_RES_CACHE;
		res->format_str = "%d=%0*x";
	}
	res->mon_capable = true;
	cbqri_res_set_counts(l3, ctrl);
	exposed_mbm_total = true;

	/* start every MCID counter so values are valid before the first reset */
	for (mcid = 0; mcid < ctrl->ctrl_info->mcid_count; mcid++) {
		err = cbqri_bc_mon_op(ctrl, CBQRI_BC_MON_CTL_OP_CONFIG_EVENT, mcid,
				      CBQRI_BC_MON_EVT_RDWR_COUNT);
		if (err) {
			kfree(container_of(domain, struct cbqri_resctrl_dom, resctrl_dom));
			return err;
		}
	}

	list_add_tail(&domain->list, &res->domains);
	err = resctrl_online_domain(res, domain);
	if (err) {
		pr_warn("%s(): failed to online L3 mon domain %d", __func__, id);
		list_del(&domain->list);
		kfree(container_of(domain, struct cbqri_resctrl_dom, resctrl_dom));
		return err;
	}
	return 0;
}

/*
 * FireSim regulators: program the period and switch regulation on. Called
 * once every RCID has a valid budget (see qos_init_domain_ctrlval()).
 */
static void cbqri_enable_regulation(struct cbqri_controller *ctrl)
{
	if (!ctrl->ctrl_info->has_regulator_ctl ||
	    ctrl->ctrl_info->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH ||
	    !ctrl->alloc_capable)
		return;

	if (period == 0 || period > CBQRI_FS_PERIOD_MAX) {
		pr_warn("period %u out of range (1..%u), using %u", period,
			CBQRI_FS_PERIOD_MAX, CBQRI_FS_PERIOD_MAX);
		period = CBQRI_FS_PERIOD_MAX;
	}

	iowrite64(period, ctrl->base + CBQRI_FS_PERIOD_LEN_OFF);
	iowrite64(regulate ? 1 : 0, ctrl->base + CBQRI_FS_GLOBAL_EN_OFF);

	pr_info("bandwidth regulation %s at 0x%lx: period=%llu cycles, MB 100%% = %u fills/bank/period, max %u%%",
		regulate ? "enabled" : "left off", ctrl->ctrl_info->addr,
		(unsigned long long)ioread64(ctrl->base + CBQRI_FS_PERIOD_LEN_OFF),
		ctrl->bc.nbwblks, ctrl->bc.mrbwb * 100u / ctrl->bc.nbwblks);
}

static int qos_resctrl_add_controller_domain(struct cbqri_controller *ctrl, int *id)
{
	struct rdt_domain *domain = NULL;
	struct cbqri_resctrl_res *cbqri_res = NULL;
	struct rdt_resource *res = NULL;
	int internal_id = *id;
	int err = 0;

	domain = qos_new_domain(ctrl);
	if (!domain)
		return -ENOSPC;
	if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY) {
		cpumask_copy(&domain->cpu_mask, &ctrl->ctrl_info->cache.cpu_mask);
		if (ctrl->ctrl_info->cache.cache_level == 2) {
			cbqri_res = &cbqri_resctrl_resources[RDT_RESOURCE_L2];
			cbqri_res_set_counts(cbqri_res, ctrl);
			res = &cbqri_res->resctrl_res;
			res->rid = RDT_RESOURCE_L2;
			res->name = "L2";
			res->alloc_capable = ctrl->alloc_capable;
			res->mon_capable = false;
			res->format_str = "%d=%0*x";
			res->fflags = RFTYPE_RES_CACHE;
			res->data_width = (ctrl->cc.ncblks + 3) / 4;
			res->cache_level = 2;
			res->cache.arch_has_sparse_bitmaps = false;
			res->cache.arch_has_per_cpu_cfg = false;
			res->cache.cbm_len = ctrl->cc.ncblks;
			res->cache.min_cbm_bits = 1;
			/* v6.2 backport: cbm_validate compares against
			 * r->default_ctrl. Set it BEFORE shareable_bits, which
			 * derives from it. */
			res->default_ctrl = (ctrl->cc.ncblks > 0) ?
				GENMASK(ctrl->cc.ncblks - 1, 0) : 0;
			res->cache.shareable_bits = res->default_ctrl;
		} else if (ctrl->ctrl_info->cache.cache_level == 3) {
			cbqri_res = &cbqri_resctrl_resources[RDT_RESOURCE_L3];
			cbqri_res_set_counts(cbqri_res, ctrl);
			res = &cbqri_res->resctrl_res;
			res->rid = RDT_RESOURCE_L3;
			res->name = "L3";
			res->format_str = "%d=%0*x";
			res->fflags = RFTYPE_RES_CACHE;
			res->data_width = (ctrl->cc.ncblks + 3) / 4;
			res->cache_level = 3;
			res->alloc_capable = ctrl->alloc_capable;
			res->mon_capable = ctrl->mon_capable;
			res->cache.arch_has_sparse_bitmaps = false;
			res->cache.arch_has_per_cpu_cfg = false;
			res->cache.cbm_len = ctrl->cc.ncblks;
			res->cache.min_cbm_bits = 1;
			/* v6.2 backport: cbm_validate compares against default_ctrl. */
			res->default_ctrl = (ctrl->cc.ncblks > 0) ?
				GENMASK(ctrl->cc.ncblks - 1, 0) : 0;
			res->cache.shareable_bits = res->default_ctrl;
		} else {
			pr_warn("%s(): unknown cache level %d", __func__,
				ctrl->ctrl_info->cache.cache_level);
			err = -ENODEV;
			goto err_free_domain;
		}
	} else if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH) {
		if (ctrl->alloc_capable) {
			cbqri_res = &cbqri_resctrl_resources[RDT_RESOURCE_MBA];
			cbqri_res_set_counts(cbqri_res, ctrl);
			res = &cbqri_res->resctrl_res;
			res->rid = RDT_RESOURCE_MBA;
			res->name = "MB";
			res->format_str = "%d=%*u";
			res->fflags = RFTYPE_RES_MB;
			res->data_width = 4;
			/* v6.2 backport: bw_validate uses default_ctrl as the
			 * upper bound. CBQRI advertises mrbwb (max reservable
			 * blocks) <= nbwblks; converting that ratio to a
			 * percentage gives the correct cap. With the QEMU
			 * defaults (mrbwb=819, nbwblks=1024) this yields 79%,
			 * which prevents resctrl from staging a value that
			 * cbqri_apply_bw_config would translate into more
			 * blocks than the controller will accept. */
			res->default_ctrl = ctrl->bc.nbwblks > 0
				? (ctrl->bc.mrbwb * 100u / ctrl->bc.nbwblks)
				: 0;
			res->cache_level = 3;
			res->alloc_capable = ctrl->alloc_capable;
			res->mon_capable = false;
			res->membw.delay_linear = true;
			res->membw.arch_needs_linear = true;
			res->membw.throttle_mode = THREAD_THROTTLE_UNDEFINED;
			// The minimum percentage allowed by the CBQRI spec
			res->membw.min_bw = 1;
			// The maximum percentage allowed by the CBQRI spec
			/* res->membw.max_bw = 80;  no max_bw in v6.2 */
			res->membw.bw_gran = 1;
		}
	} else {
		pr_warn("%s(): unknown resource %d", __func__, ctrl->ctrl_info->type);
		err = -ENODEV;
		goto err_free_domain;
	}

	domain->id = internal_id;
	if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH) {
		if (!cbqri_res) {
			/* bandwidth controller without allocation: monitor only */
			kfree(container_of(domain, struct cbqri_resctrl_dom, resctrl_dom));
			domain = NULL;
			goto add_mon;
		}
		/* the regulators sit on the whole system's memory path */
		cpumask_copy(&domain->cpu_mask, cpu_online_mask);
	}
	err = qos_init_domain_ctrlval(res, domain);
	if (err)
		goto err_free_domain;

	if (cbqri_res) {
		list_add_tail(&domain->list, &cbqri_res->resctrl_res.domains);
		*id = internal_id;
		err = resctrl_online_domain(res, domain);
		if (err) {
			pr_warn("%s(): failed to online cbqri_res domain", __func__);
			goto err_free_domain;
		}
	}

add_mon:
	if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH &&
	    ctrl->mon_capable) {
		err = qos_resctrl_add_bw_mon_domain(ctrl, internal_id);
		if (err)
			return err;
	}

	return 0;

err_free_domain:
	pr_warn("%s(): err_free_domain", __func__);
	kfree(container_of(domain, struct cbqri_resctrl_dom, resctrl_dom));

	return err;
}

int qos_resctrl_setup(void)
{
	struct rdt_domain *domain, *domain_temp;
	struct cbqri_controller_info *ctrl_info;
	struct cbqri_controller *ctrl;
	struct cbqri_resctrl_res *res;
	static int found_controllers;
	int err = 0;
	int id = 0;
	int i;

	list_for_each_entry(ctrl_info, &cbqri_controllers, list) {
		err = cbqri_probe_controller(ctrl_info, &controllers[found_controllers]);
		if (err) {
			pr_warn("%s(): failed (%d)", __func__, err);
			goto err_unmap_controllers;
		}

		found_controllers++;
		if (found_controllers > MAX_CONTROLLERS) {
			pr_warn("%s(): increase MAX_CONTROLLERS value", __func__);
			break;
		}
	}

	/*
	 * Domain ids follow controller order. Sort by MMIO address so the ids
	 * are stable and don't depend on device-tree node order (on the FireSim
	 * SoC this makes MB:0 the core->LLC regulator at 0x20000000 and MB:1
	 * the LLC->DRAM one at 0x21000000).
	 */
	for (i = 1; i < found_controllers; i++) {
		struct cbqri_controller tmp = controllers[i];
		int j = i - 1;

		while (j >= 0 && controllers[j].ctrl_info->addr > tmp.ctrl_info->addr) {
			controllers[j + 1] = controllers[j];
			j--;
		}
		controllers[j + 1] = tmp;
	}

	for (i = 0; i < RDT_NUM_RESOURCES; i++) {
		res = &cbqri_resctrl_resources[i];
		INIT_LIST_HEAD(&res->resctrl_res.domains);
		/* v6.2 backport: ctrl/mon domains collapsed to one list. Also
		 * init evt_list here — fs/resctrl/monitor.c only initializes
		 * it for L3, but our L2 resource is also mon_capable, and
		 * mkdir_mondata_subdir() walks the list unconditionally. */
		INIT_LIST_HEAD(&res->resctrl_res.evt_list);
		res->resctrl_res.rid = i;
	}

	for (i = 0; i < found_controllers; i++) {
		ctrl = &controllers[i];
		err = qos_resctrl_add_controller_domain(ctrl, &id);
		if (err) {
			pr_warn("%s(): failed to add controller domain (%d)", __func__, err);
			goto err_free_controllers_list;
		}
		id++;

		/*
		 * CDP (code data prioritization) on x86 is similar to
		 * the AT (access type) field in CBQRI. CDP only supports
		 * caches so this must be a CBQRI capacity controller.
		 */
		if (ctrl->ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY &&
		    ctrl->cc.supports_alloc_at_code &&
		    ctrl->cc.supports_alloc_at_data) {
			if (ctrl->ctrl_info->cache.cache_level == 2)
				exposed_cdp_l2_capable = true;
			else
				exposed_cdp_l3_capable = true;
		}
	}

	/* every RCID now has a valid budget: safe to turn the regulators on */
	for (i = 0; i < found_controllers; i++)
		cbqri_enable_regulation(&controllers[i]);

	pr_info("exposed_alloc_capable = %d", exposed_alloc_capable);
	pr_info("exposed_mon_capable = %d", exposed_mon_capable);
	pr_info("exposed_mbm_total = %d", exposed_mbm_total);
	pr_info("exposed_cdp_l2_capable = %d", exposed_cdp_l2_capable);
	pr_info("exposed_cdp_l3_capable = %d", exposed_cdp_l3_capable);

	return resctrl_init();

err_free_controllers_list:
	for (i = 0; i < RDT_NUM_RESOURCES; i++) {
		res = &cbqri_resctrl_resources[i];
		list_for_each_entry_safe(domain, domain_temp, &res->resctrl_res.domains,
					 list) {
			kfree(domain);
		}
	}

err_unmap_controllers:
	for (i = 0; i < found_controllers; i++) {
		iounmap(controllers[i].base);
		release_mem_region(controllers[i].ctrl_info->addr, controllers[i].ctrl_info->size);
	}

	return err;
}

/* bandwidth-controller domains span every CPU */
static void qos_resctrl_update_domain_cpu(unsigned int cpu, bool online)
{
	struct cbqri_resctrl_dom *hw_dom;
	struct rdt_resource *r;
	struct rdt_domain *d;
	int i;

	for (i = 0; i < RDT_NUM_RESOURCES; i++) {
		r = &cbqri_resctrl_resources[i].resctrl_res;
		list_for_each_entry(d, &r->domains, list) {
			hw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_dom);
			if (hw_dom->hw_ctrl->ctrl_info->type !=
			    CBQRI_CONTROLLER_TYPE_BANDWIDTH)
				continue;
			if (online)
				cpumask_set_cpu(cpu, &d->cpu_mask);
			else
				cpumask_clear_cpu(cpu, &d->cpu_mask);
		}
	}
}

int qos_resctrl_online_cpu(unsigned int cpu)
{
	per_cpu(cpu_default_srmcfg, cpu) = 0;
	qos_resctrl_update_domain_cpu(cpu, true);
	resctrl_online_cpu(cpu);
	return 0;
}

int qos_resctrl_offline_cpu(unsigned int cpu)
{
	resctrl_offline_cpu(cpu);
	qos_resctrl_update_domain_cpu(cpu, false);
	return 0;
}

/* ----------------------------------------------------------------
 * v6.2 backport stubs.
 * Morse's fs/resctrl/ calls these symbols; on x86 and ARM MPAM they
 * are provided by the arch backend. The RISC-V CBQRI backend does
 * not (yet) implement them, so we stub them out with safe defaults.
 * ----------------------------------------------------------------
 */

/* Pseudo-lock: x86-cache-locking trick. Not supported on RISC-V. */
int resctrl_arch_get_prefetch_disable_bits(void)
{
	return 0;
}

int resctrl_arch_pseudo_lock_fn(void *_plr)
{
	return -ENOSYS;
}

int resctrl_arch_measure_cycles_lat_fn(void *_plr)
{
	return -ENOSYS;
}

int resctrl_arch_measure_l2_residency(void *_plr)
{
	return -ENOSYS;
}

int resctrl_arch_measure_l3_residency(void *_plr)
{
	return -ENOSYS;
}

/* IOMMU integration: optional, not wired on RISC-V. */
struct iommu_group;

bool resctrl_arch_match_iommu_closid(struct iommu_group *group, u32 closid)
{
	return false;
}

bool resctrl_arch_match_iommu_closid_rmid(struct iommu_group *group, u32 closid,
					  u32 rmid)
{
	return false;
}

int resctrl_arch_set_iommu_closid_rmid(struct iommu_group *group, u32 closid,
				       u32 rmid)
{
	return -ENOSYS;
}

/* Used by fs/resctrl/monitor.c — sleeping / non-sleeping ctx allocator
 * variants. Drew's existing alloc is non-sleeping anyway. */
int resctrl_arch_mon_ctx_alloc_no_wait(struct rdt_resource *r, int evtid)
{
	return 0;
}

/* qos_resctrl.c uses resctrl_get_default_ctrl(); upstream has it as a
 * tiny helper that returns the resource's default mask. v6.2 doesn't
 * declare it publicly. Compute the all-bits-set CBM for caches; for MB
 * return r->default_ctrl (mrbwb as a percentage). Returning 0 here made
 * boot program rbwb=0, which CBQRI hardware rejects (status 5,
 * INVALID_BWB) and which would leave every RCID at a zero budget. */
u32 resctrl_get_default_ctrl(struct rdt_resource *r)
{
	if (r->cache.cbm_len > 0)
		return GENMASK(r->cache.cbm_len - 1, 0);
	return r->default_ctrl;
}

/*
 * fs/resctrl/rdtgroup.c calls resctrl_sched_in() by IPI on the CPU a task is
 * running on after moving it to another group (or after changing the CPU's
 * default). Reload srmcfg now; otherwise the change would only take effect
 * at the task's next context switch.
 */
void resctrl_sched_in(void)
{
	resctrl_arch_sched_in(current);
}

