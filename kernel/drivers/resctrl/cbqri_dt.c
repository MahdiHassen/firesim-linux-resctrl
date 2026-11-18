// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device-tree discovery of CBQRI capacity controllers, for systems without
 * the ACPI RQSC table. A node
 *
 *	cbqri@10401800 {
 *		compatible = "riscv,cbqri-cache";
 *		reg = <0x0 0x10401800 0x0 0x1000>;
 *		cache-level = <3>;
 *		riscv,cbqri-rcid = <16>;
 *		riscv,cbqri-mcid = <16>;
 *	};
 *
 * (the property names of the FireSim CBQRI device trees) describes the
 * register interface of a cache that every CPU shares: one resctrl domain
 * per level, id 0, the id cacheinfo gives a cache shared with CPU 0. The
 * cache's size comes from cacheinfo (a cache node with that cache-level
 * on the CPUs' next-level-cache chain). Bandwidth controllers are not
 * described this way.
 */

#define pr_fmt(fmt) "cbqri-dt: " fmt

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/riscv_cbqri.h>

static const struct of_device_id cbqri_dt_cc_ids[] __initconst = {
	{ .compatible = "riscv,cbqri-cache" },
	{ }
};

static int __init cbqri_dt_init(void)
{
	struct device_node *np;

	for_each_matching_node(np, cbqri_dt_cc_ids) {
		struct cbqri_controller_info info = {
			.type = CBQRI_CONTROLLER_TYPE_CAPACITY,
			.cache_id = 0,
			.cpus = cpu_possible_mask,
		};
		struct resource res;
		int err;

		if (!of_device_is_available(np))
			continue;
		if (of_address_to_resource(np, 0, &res)) {
			pr_err("%pOF: no reg\n", np);
			continue;
		}
		info.addr = res.start;
		info.size = resource_size(&res);
		if (of_property_read_u32(np, "riscv,cbqri-rcid", &info.rcid_count) ||
		    of_property_read_u32(np, "riscv,cbqri-mcid", &info.mcid_count) ||
		    of_property_read_u32(np, "cache-level", &info.cache_level)) {
			pr_err("%pOF: needs riscv,cbqri-rcid, riscv,cbqri-mcid and cache-level\n", np);
			continue;
		}
		if (info.rcid_count > CBQRI_MAX_RCID || info.mcid_count > CBQRI_MAX_MCID ||
		    info.cache_level < 2 || info.cache_level > 3) {
			pr_err("%pOF: rcid %u / mcid %u / cache-level %u out of range\n", np,
			       info.rcid_count, info.mcid_count, info.cache_level);
			continue;
		}
		err = riscv_cbqri_register_controller(&info);
		if (err) {
			pr_warn("%pOF: register failed (%d)\n", np, err);
			continue;
		}
		pr_info("L%u capacity controller at %pa: rcid %u, mcid %u\n", info.cache_level,
			&info.addr, info.rcid_count, info.mcid_count);
	}
	return 0;
}
/* before the resctrl glue's late_initcall, which walks the registered controllers */
device_initcall(cbqri_dt_init);
