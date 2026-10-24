// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device-tree discovery of CBQRI controllers for SoCs without ACPI RQSC,
 * written for the FireSim CBQRI SoC (Chipyard inclusive LLC + memory traffic
 * controller). Every controller there is reachable from every hart, so each
 * one gets all CPUs; bandwidth controllers become resctrl domains numbered
 * in MMIO address order (0 = core->LLC regulator at 0x20000000, 1 = LLC->DRAM
 * at 0x21000000 on the quad-BOOM design), which keeps the ids independent of
 * device-tree node order.
 *
 * Nodes:
 *   riscv,cbqri-cache: reg, cache-level, cache-size, riscv,cbqri-rcid, riscv,cbqri-mcid
 *   riscv,cbqri-bandwidth[-cache|-memory]: reg, riscv,cbqri-rcid, riscv,cbqri-mcid
 * The FireSim generator emits the vendor prefix as "rsicv"; both spellings
 * are accepted. The -cache / -memory variants carry two non-spec registers
 * (regulation enable, period) that the resctrl glue programs at boot.
 */

#define pr_fmt(fmt) "cbqri-dt: " fmt

#include <linux/init.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/riscv_cbqri.h>
#include <linux/sort.h>

#define CBQRI_DT_MAX_CTRLS 8

/*
 * cbqri_dt.max_ids=N: use at most N RCIDs and N MCIDs per controller whatever the
 * device tree advertises. The FireSim quad-BOOM bitstream advertises 16 of each but
 * only honours 0-3, so resctrl must not hand out CLOSIDs or RMIDs above 3.
 */
static unsigned int cbqri_dt_max_ids;
module_param_named(max_ids, cbqri_dt_max_ids, uint, 0444);
MODULE_PARM_DESC(max_ids, "cap the RCID and MCID counts read from the device tree (0 = no cap)");

static const struct of_device_id cbqri_bw_ids[] __initconst = {
	{ .compatible = "riscv,cbqri-bandwidth" },
	{ .compatible = "riscv,cbqri-bandwidth-cache" },
	{ .compatible = "riscv,cbqri-bandwidth-memory" },
	{ .compatible = "rsicv,cbqri-bandwidth" },
	{ .compatible = "rsicv,cbqri-bandwidth-cache" },
	{ .compatible = "rsicv,cbqri-bandwidth-memory" },
	{ }
};

/* FireSim regulators: have the non-spec enable / period registers */
static const struct of_device_id cbqri_fs_regulator_ids[] __initconst = {
	{ .compatible = "riscv,cbqri-bandwidth-cache" },
	{ .compatible = "riscv,cbqri-bandwidth-memory" },
	{ .compatible = "rsicv,cbqri-bandwidth" },
	{ .compatible = "rsicv,cbqri-bandwidth-cache" },
	{ .compatible = "rsicv,cbqri-bandwidth-memory" },
	{ }
};

static const struct of_device_id cbqri_cc_ids[] __initconst = {
	{ .compatible = "riscv,cbqri-cache" },
	{ }
};

struct cbqri_dt_node {
	struct cbqri_controller_info info;
	struct cbqri_dt_info dt;
};

static int __init cbqri_dt_read_common(struct device_node *np, struct cbqri_dt_node *n)
{
	struct resource res;
	int err;

	err = of_address_to_resource(np, 0, &res);
	if (err) {
		pr_err("%pOF: no reg (%d)\n", np, err);
		return err;
	}
	n->info.addr = res.start;
	n->info.size = resource_size(&res);

	err = of_property_read_u32(np, "riscv,cbqri-rcid", &n->info.rcid_count);
	if (err) {
		pr_err("%pOF: missing riscv,cbqri-rcid\n", np);
		return err;
	}
	err = of_property_read_u32(np, "riscv,cbqri-mcid", &n->info.mcid_count);
	if (err) {
		pr_err("%pOF: missing riscv,cbqri-mcid\n", np);
		return err;
	}
	if (n->info.rcid_count > CBQRI_MAX_RCID || n->info.mcid_count > CBQRI_MAX_MCID) {
		pr_err("%pOF: rcid/mcid count out of range\n", np);
		return -EINVAL;
	}
	if (cbqri_dt_max_ids) {
		n->info.rcid_count = min(n->info.rcid_count, cbqri_dt_max_ids);
		n->info.mcid_count = min(n->info.mcid_count, cbqri_dt_max_ids);
		pr_info("%pOF: rcid/mcid capped at %u (cbqri_dt.max_ids)\n", np, cbqri_dt_max_ids);
	}
	return 0;
}

static int __init cbqri_dt_cmp_addr(const void *a, const void *b)
{
	const struct cbqri_dt_node *x = a, *y = b;

	if (x->info.addr == y->info.addr)
		return 0;
	return x->info.addr < y->info.addr ? -1 : 1;
}

static int __init cbqri_dt_init(void)
{
	static struct cbqri_dt_node nodes[CBQRI_DT_MAX_CTRLS] __initdata;
	struct device_node *np;
	int nbw = 0, i, err, registered = 0;

	for_each_matching_node(np, cbqri_bw_ids) {
		struct cbqri_dt_node *n = &nodes[nbw];

		if (!of_device_is_available(np))
			continue;
		if (nbw == CBQRI_DT_MAX_CTRLS) {
			pr_warn("%pOF: more than %d bandwidth controllers, ignoring\n",
				np, CBQRI_DT_MAX_CTRLS);
			break;
		}
		memset(n, 0, sizeof(*n));
		n->info.type = CBQRI_CONTROLLER_TYPE_BANDWIDTH;
		if (cbqri_dt_read_common(np, n))
			continue;
		n->dt.fs_regulator = of_match_node(cbqri_fs_regulator_ids, np) != NULL;
		nbw++;
	}

	/* domain ids follow MMIO address order */
	sort(nodes, nbw, sizeof(nodes[0]), cbqri_dt_cmp_addr, NULL);
	for (i = 0; i < nbw; i++) {
		nodes[i].dt.dom_id = i;
		err = riscv_cbqri_register_controller_dt(&nodes[i].info, &nodes[i].dt);
		if (err) {
			pr_warn("bandwidth controller at %pa: register failed (%d)\n",
				&nodes[i].info.addr, err);
			continue;
		}
		pr_info("bandwidth controller at %pa: domain %d, rcid %u, mcid %u%s\n",
			&nodes[i].info.addr, i, nodes[i].info.rcid_count,
			nodes[i].info.mcid_count,
			nodes[i].dt.fs_regulator ? ", FireSim regulator" : "");
		registered++;
	}

	for_each_matching_node(np, cbqri_cc_ids) {
		struct cbqri_dt_node n = {};

		if (!of_device_is_available(np))
			continue;
		n.info.type = CBQRI_CONTROLLER_TYPE_CAPACITY;
		if (cbqri_dt_read_common(np, &n))
			continue;
		if (of_property_read_u32(np, "cache-level", &n.dt.cache_level) ||
		    of_property_read_u32(np, "cache-size", &n.dt.cache_size)) {
			pr_err("%pOF: missing cache-level / cache-size\n", np);
			continue;
		}
		n.dt.dom_id = 0;	/* one shared cache per level */
		err = riscv_cbqri_register_controller_dt(&n.info, &n.dt);
		if (err) {
			pr_warn("capacity controller at %pa: register failed (%d)\n",
				&n.info.addr, err);
			continue;
		}
		pr_info("L%u capacity controller at %pa: %u bytes, rcid %u, mcid %u\n",
			n.dt.cache_level, &n.info.addr, n.dt.cache_size,
			n.info.rcid_count, n.info.mcid_count);
		registered++;
	}

	return 0;
}
/* before the resctrl glue's late_initcall, which walks cbqri_controllers */
device_initcall(cbqri_dt_init);
