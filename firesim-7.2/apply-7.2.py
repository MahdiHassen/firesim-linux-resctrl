#!/usr/bin/env python3
"""FireSim CBQRI hacks on top of Linux 7.2 + Fustini's CBQRI RFC (branch cbqri-v7.2 on the manager).
Every edit is an exact-anchor replacement that fails loudly; new files are copied in. Groups (for one commit each):
  dt       device-tree discovery, FireSim regulators (cap semantics, enable/period), per-regulator mbm_total_bytes
  force    CONFIG_RISCV_FORCE_SSQOSID
  irq      qos_resctrl.irq_policy / irq_rcid hooks
usage: apply-7.2.py <kernel tree> <this dir> [dt|force|irq ...]   (default: all)"""
import os, re, shutil, sys
K, SRC = sys.argv[1], sys.argv[2]
GROUPS = sys.argv[3:] or ["dt", "force", "irq"]

def edit(path, pairs):
    p = os.path.join(K, path); s = open(p).read()
    for old, new, n in pairs:
        c = s.count(old)
        if c != n: sys.exit(f"FAIL {path}: anchor found {c}x, want {n}: {old[:70]!r}")
        s = s.replace(old, new)
    open(p, "w").write(s); print(f"edit  {path}: {len(pairs)}")

def copy(src, dst):
    shutil.copy(os.path.join(SRC, src), os.path.join(K, dst)); print(f"copy  {dst}")

def first_include(path):
    s = open(os.path.join(K, path)).read()
    return re.search(r"#include <linux/[^>]+>\n", s).group(0)

# ------------------------------------------------------------------ dt: discovery + FireSim regulators + per-BC monitoring
if "dt" in GROUPS:
    copy("cbqri_dt.c", "drivers/resctrl/cbqri_dt.c")
    edit("include/linux/riscv_cbqri.h", [
        ("#if IS_ENABLED(CONFIG_RISCV_CBQRI_DRIVER)\nint riscv_cbqri_register_controller(const struct cbqri_controller_info *info);\n",
         "/*\n * Device-tree registration (drivers/resctrl/cbqri_dt.c): there are no firmware\n"
         " * tables to resolve topology from, so the discovery layer supplies it.\n"
         " * @dom_id is the resctrl domain id (cache id for a capacity controller,\n"
         " * bandwidth domain for a bandwidth controller); every controller serves\n"
         " * every CPU. @fs_regulator marks a FireSim bandwidth regulator, which has\n"
         " * the non-spec enable / period registers and treats Rbwb as a cap.\n */\n"
         "struct cbqri_dt_info {\n\tint\tdom_id;\n\tu32\tcache_level;\n\tu32\tcache_size;\n\tbool\tfs_regulator;\n};\n\n"
         "#if IS_ENABLED(CONFIG_RISCV_CBQRI_DRIVER)\nint riscv_cbqri_register_controller(const struct cbqri_controller_info *info);\n"
         "int riscv_cbqri_register_controller_dt(const struct cbqri_controller_info *info,\n\t\t\t\t       const struct cbqri_dt_info *dt);\n", 1),
        ("static inline void riscv_cbqri_unregister_last(unsigned int n) { }\n",
         "static inline void riscv_cbqri_unregister_last(unsigned int n) { }\n\n"
         "static inline int\nriscv_cbqri_register_controller_dt(const struct cbqri_controller_info *info,\n"
         "\t\t\t\t   const struct cbqri_dt_info *dt)\n{\n\treturn -ENODEV;\n}\n", 1)])
    edit("drivers/resctrl/cbqri_internal.h", [
        ("/* Capacity usage monitoring event IDs (CBQRI spec Table 4) */\n",
         "/*\n * FireSim (crsullivan13) bandwidth regulators: registers outside the CBQRI\n"
         " * spec. Regulation is off at reset; the period is in regulator clock cycles\n"
         " * (25 bits). The counters count 64-byte line fills per MCID; Rbwb is a cap of\n"
         " * fills per bank per period, not a reservation.\n */\n"
         "#define CBQRI_FS_GLOBAL_EN_OFF   0x100\n#define CBQRI_FS_PERIOD_LEN_OFF  0x108\n#define CBQRI_FS_PERIOD_MAX      ((1u << 25) - 1)\n\n"
         "/* Capacity usage monitoring event IDs (CBQRI spec Table 4) */\n", 1),
        ("\tu32 rcid_count;\n\tu32 mcid_count;\n\n\t/*\n\t * Per-RCID cache of the most recent Rbwb",
         "\tu32 rcid_count;\n\tu32 mcid_count;\n\n\t/* FireSim regulator: Rbwb is a cap; has the CBQRI_FS_* registers */\n\tbool fs_regulator;\n\n\t/*\n\t * Per-RCID cache of the most recent Rbwb", 1)])
    edit("drivers/resctrl/cbqri_devices.c", [
        ("\terr = cbqri_probe_controller(ctrl);\n\tif (err) {\n\t\tcbqri_controller_destroy(ctrl);\n\t\treturn err;\n\t}\n\n\tlist_add_tail(&ctrl->list, &cbqri_controllers);\n\treturn 0;\n}\n",
         "\terr = cbqri_probe_controller(ctrl);\n\tif (err) {\n\t\tcbqri_controller_destroy(ctrl);\n\t\treturn err;\n\t}\n\n\tlist_add_tail(&ctrl->list, &cbqri_controllers);\n\treturn 0;\n}\n\n"
         "/*\n * Device-tree registration (cbqri_dt.c): the caller resolved the topology.\n"
         " * Every controller serves every CPU; dt->dom_id becomes the cache id of a\n"
         " * capacity controller or the proximity-domain stand-in (= resctrl domain id)\n"
         " * of a bandwidth controller.\n */\n"
         "int riscv_cbqri_register_controller_dt(const struct cbqri_controller_info *info,\n\t\t\t\t       const struct cbqri_dt_info *dt)\n{\n"
         "\tstruct cbqri_controller *ctrl;\n\tint err;\n\n"
         "\tif (!info->addr) {\n\t\tpr_warn(\"skipping controller with invalid addr=0x0\\n\");\n\t\treturn -EINVAL;\n\t}\n\n"
         "\tctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);\n\tif (!ctrl)\n\t\treturn -ENOMEM;\n\n\tmutex_init(&ctrl->lock);\n\n"
         "\tctrl->addr = info->addr;\n\tctrl->size = info->size;\n\tctrl->type = info->type;\n\tctrl->rcid_count = info->rcid_count;\n\tctrl->mcid_count = info->mcid_count;\n\n"
         "\tif (WARN_ON_ONCE(ctrl->rcid_count > FIELD_MAX(SRMCFG_RCID_MASK) + 1) ||\n"
         "\t    WARN_ON_ONCE(ctrl->mcid_count > FIELD_MAX(CBQRI_MON_CTL_MCID_MASK) + 1)) {\n\t\tcbqri_controller_destroy(ctrl);\n\t\treturn -EINVAL;\n\t}\n\n"
         "\tswitch (info->type) {\n"
         "\tcase CBQRI_CONTROLLER_TYPE_CAPACITY:\n\t\tctrl->cache.cache_id = dt->dom_id;\n\t\tctrl->cache.cache_level = dt->cache_level;\n\t\tctrl->cache.cache_size = dt->cache_size;\n"
         "\t\tcpumask_copy(&ctrl->cache.cpu_mask, cpu_possible_mask);\n\t\tbreak;\n"
         "\tcase CBQRI_CONTROLLER_TYPE_BANDWIDTH:\n\t\tctrl->mem.prox_dom = dt->dom_id;\n\t\tcpumask_copy(&ctrl->mem.cpu_mask, cpu_possible_mask);\n\t\tctrl->fs_regulator = dt->fs_regulator;\n\t\tbreak;\n"
         "\tdefault:\n\t\tpr_warn(\"controller at %pa: unknown type %u, skipping\\n\",\n\t\t\t&ctrl->addr, info->type);\n\t\tcbqri_controller_destroy(ctrl);\n\t\treturn -EINVAL;\n\t}\n\n"
         "\terr = cbqri_probe_controller(ctrl);\n\tif (err) {\n\t\tcbqri_controller_destroy(ctrl);\n\t\treturn err;\n\t}\n\n"
         "\tlist_add_tail(&ctrl->list, &cbqri_controllers);\n\treturn 0;\n}\n", 1)])
    edit("drivers/resctrl/Kconfig", [
        ("\t  new platform; otherwise leave disabled to avoid log noise.\n\nendif\n",
         "\t  new platform; otherwise leave disabled to avoid log noise.\n\n"
         "config RISCV_CBQRI_DT\n\tbool \"Discover CBQRI controllers from the device tree (FireSim)\"\n\tdepends on OF\n\tdefault y\n\thelp\n"
         "\t  Register the CBQRI controllers described by riscv,cbqri-cache and\n\t  riscv,cbqri-bandwidth[-cache|-memory] device-tree nodes, as the\n"
         "\t  FireSim CBQRI SoC describes them. The -cache / -memory regulators\n\t  have non-spec enable and period registers that are programmed at\n"
         "\t  boot (qos_resctrl.period, qos_resctrl.mb_period, qos_resctrl.regulate).\n\nendif\n", 1)])
    edit("drivers/resctrl/Makefile", [
        ("cbqri-$(CONFIG_RISCV_CBQRI_RESCTRL_FS)\t\t+= cbqri_resctrl.o\n",
         "cbqri-$(CONFIG_RISCV_CBQRI_RESCTRL_FS)\t\t+= cbqri_resctrl.o\ncbqri-$(CONFIG_RISCV_CBQRI_DT)\t\t\t+= cbqri_dt.o\n", 1)])
    R = "drivers/resctrl/cbqri_resctrl.c"
    edit(R, [
        ("#include <linux/io.h>\n#include <linux/resctrl.h>\n", "#include <linux/io.h>\n#include <linux/moduleparam.h>\n#include <linux/resctrl.h>\n", 1),
        # params, FireSim helpers, per-BC monitoring lookup
        ("/* Protects ctrl_domain list mutations across CPU hotplug. */\nstatic DEFINE_MUTEX(cbqri_domain_list_lock);\n",
         "/* Protects ctrl_domain list mutations across CPU hotplug. */\nstatic DEFINE_MUTEX(cbqri_domain_list_lock);\n\n"
         "/*\n * FireSim CBQRI regulators (cbqri_dt.c marks them): Rbwb caps the 64-byte line\n"
         " * fills per bank per period instead of reserving bandwidth, so the\n"
         " * sum(Rbwb) <= MRBWB rule does not apply, every RCID starts uncapped (MRBWB),\n"
         " * and the regulators, which reset to \"off\", are switched on once every RCID\n"
         " * has a budget. Boot parameters (prefix kept from the 6.2 driver so the test\n"
         " * harness and command lines carry over):\n"
         " *   qos_resctrl.period=<cycles>          every regulator's period (default 1e6)\n"
         " *   qos_resctrl.mb_period=<dom0>,<dom1>  per-MB-domain override (0 = period)\n"
         " *   qos_resctrl.regulate=<0|1>           leave regulation off at boot\n */\n"
         "#undef MODULE_PARAM_PREFIX\n#define MODULE_PARAM_PREFIX \"qos_resctrl.\"\n"
         "static unsigned int period = 1000000;\nmodule_param(period, uint, 0444);\n"
         "MODULE_PARM_DESC(period, \"FireSim CBQRI regulator period in regulator clock cycles\");\n"
         "static unsigned int mb_period[8];\nstatic int mb_period_count;\nmodule_param_array(mb_period, uint, &mb_period_count, 0444);\n"
         "MODULE_PARM_DESC(mb_period, \"per-MB-domain regulator period in cycles (overrides period)\");\n"
         "static bool regulate = true;\nmodule_param(regulate, bool, 0444);\n"
         "MODULE_PARM_DESC(regulate, \"switch FireSim bandwidth regulation on at boot\");\n\n"
         "/*\n * mbm_total_bytes read from each bandwidth controller's own counters, exposed\n"
         " * as one L3 monitoring domain per controller (same ids as the MB_MIN\n"
         " * domains). Used when there is no L3 capacity controller to pair with.\n */\n"
         "static bool bc_mon_domains;\n\n"
         "static u64 cbqri_fs_rbwb_default(struct cbqri_controller *ctrl)\n{\n\treturn ctrl->bc.mrbwb;\n}\n\n"
         "static void cbqri_fs_regulator_enable(struct cbqri_controller *ctrl, int dom_id)\n{\n\tunsigned int p = period;\n\n"
         "\tif (!ctrl->fs_regulator)\n\t\treturn;\n"
         "\tif (dom_id >= 0 && dom_id < mb_period_count && mb_period[dom_id])\n\t\tp = mb_period[dom_id];\n"
         "\tif (!p || p > CBQRI_FS_PERIOD_MAX) {\n\t\tpr_warn(\"MB domain %d: period %u out of range (1..%u), using %u\\n\",\n"
         "\t\t\tdom_id, p, CBQRI_FS_PERIOD_MAX, CBQRI_FS_PERIOD_MAX);\n\t\tp = CBQRI_FS_PERIOD_MAX;\n\t}\n"
         "\tiowrite64(p, ctrl->base + CBQRI_FS_PERIOD_LEN_OFF);\n\tiowrite64(regulate ? 1 : 0, ctrl->base + CBQRI_FS_GLOBAL_EN_OFF);\n"
         "\tpr_info(\"MB domain %d at %pa: regulation %s, period %llu cycles, Rbwb = fills/bank/period, max %u (of %u blocks)\\n\",\n"
         "\t\tdom_id, &ctrl->addr, regulate ? \"on\" : \"left off\",\n"
         "\t\t(unsigned long long)ioread64(ctrl->base + CBQRI_FS_PERIOD_LEN_OFF),\n\t\tctrl->bc.mrbwb, ctrl->bc.nbwblks);\n}\n\n"
         "static struct cbqri_controller *cbqri_mon_bc_for_domain(int id)\n{\n\tstruct cbqri_controller *ctrl;\n\n"
         "\tlist_for_each_entry(ctrl, &cbqri_controllers, list) {\n\t\tif (ctrl->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH &&\n"
         "\t\t    ctrl->mon_capable && ctrl->mem.prox_dom == id)\n\t\t\treturn ctrl;\n\t}\n\treturn NULL;\n}\n", 1),
        # reset_rmid: find the BC by monitoring-domain id
        ("\t\tcd = cbqri_find_ctrl_domain(&r->ctrl_domains, d->hdr.id);\n\t\tif (!cd)\n\t\t\tbreak;\n"
         "\t\thw_dom = container_of(cd, struct cbqri_resctrl_dom, resctrl_ctrl_dom);\n\t\tbc = hw_dom->paired_bc;\n\t\tif (!bc)\n\t\t\tbreak;\n",
         "\t\tif (bc_mon_domains) {\n\t\t\tbc = cbqri_mon_bc_for_domain(d->hdr.id);\n\t\t} else {\n"
         "\t\t\tcd = cbqri_find_ctrl_domain(&r->ctrl_domains, d->hdr.id);\n\t\t\tif (!cd)\n\t\t\t\tbreak;\n"
         "\t\t\thw_dom = container_of(cd, struct cbqri_resctrl_dom, resctrl_ctrl_dom);\n\t\t\tbc = hw_dom->paired_bc;\n\t\t}\n\t\tif (!bc)\n\t\t\tbreak;\n", 1),
        # rmid_read: same
        ("\t\td = cbqri_find_ctrl_domain(&r->ctrl_domains, hdr->id);\n\t\tif (!d) {\n\t\t\terr = -ENOENT;\n\t\t\tbreak;\n\t\t}\n"
         "\t\thw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_ctrl_dom);\n\t\tbc = hw_dom->paired_bc;\n\t\tif (!bc) {\n\t\t\terr = -ENOENT;\n\t\t\tbreak;\n\t\t}\n",
         "\t\tif (bc_mon_domains) {\n\t\t\tbc = cbqri_mon_bc_for_domain(hdr->id);\n\t\t} else {\n"
         "\t\t\td = cbqri_find_ctrl_domain(&r->ctrl_domains, hdr->id);\n\t\t\tif (!d) {\n\t\t\t\terr = -ENOENT;\n\t\t\t\tbreak;\n\t\t\t}\n"
         "\t\t\thw_dom = container_of(d, struct cbqri_resctrl_dom, resctrl_ctrl_dom);\n\t\t\tbc = hw_dom->paired_bc;\n\t\t}\n"
         "\t\tif (!bc) {\n\t\t\terr = -ENOENT;\n\t\t\tbreak;\n\t\t}\n", 1),
        # cap semantics
        ("\t\t/* sum(Rbwb) <= MRBWB validation runs inside cbqri_apply_rbwb(). */\n\t\treturn cbqri_apply_rbwb(dom->hw_ctrl, closid, cfg_val, true);\n",
         "\t\t/* sum(Rbwb) <= MRBWB validation runs inside cbqri_apply_rbwb(); not for a FireSim cap */\n"
         "\t\treturn cbqri_apply_rbwb(dom->hw_ctrl, closid, cfg_val, !dom->hw_ctrl->fs_regulator);\n", 1),
        ("\t\t\t\tu64 rbwb = (rcid == 0) ?\n\t\t\t\t\tcbqri_rcid0_rbwb(dom->hw_ctrl) : 1;\n",
         "\t\t\t\tu64 rbwb = dom->hw_ctrl->fs_regulator ?\n\t\t\t\t\tcbqri_fs_rbwb_default(dom->hw_ctrl) :\n\t\t\t\t\t(rcid == 0) ? cbqri_rcid0_rbwb(dom->hw_ctrl) : 1;\n", 1),
        ("\t\t\trbwb = (rcid == 0) ? cbqri_rcid0_rbwb(dom->hw_ctrl) : 1;\n",
         "\t\t\trbwb = dom->hw_ctrl->fs_regulator ? cbqri_fs_rbwb_default(dom->hw_ctrl) :\n\t\t\t       (rcid == 0) ? cbqri_rcid0_rbwb(dom->hw_ctrl) : 1;\n", 1),
        ("\t\tres->membw.default_to_min = true;\n\t\tres->alloc_capable = ctrl->alloc_capable;\n",
         "\t\tres->membw.default_to_min = !ctrl->fs_regulator;\t/* FireSim: new groups uncapped */\n\t\tres->alloc_capable = ctrl->alloc_capable;\n", 1),
        # regulator on once the MB_MIN domain has every RCID programmed
        ("\tresctrl_online_ctrl_domain(res, domain);\n\n\treturn domain;\n}\n",
         "\tresctrl_online_ctrl_domain(res, domain);\n\n\t/* FireSim regulators are off at reset: on once every RCID has a budget */\n"
         "\tif (res->rid == RDT_RESOURCE_MB_MIN)\n\t\tcbqri_fs_regulator_enable(ctrl, dom_id);\n\n\treturn domain;\n}\n", 1),
        # mon-only L3 with one domain per bandwidth controller
        ("static void cbqri_resctrl_accumulate_caps(void)\n{\n",
         "/*\n * FireSim: no capacity controller, but each bandwidth regulator counts the\n"
         " * 64-byte fills per MCID that pass through it. Expose a monitor-only L3\n"
         " * resource with one monitoring domain per regulator (ids as MB_MIN), so\n"
         " * mon_L3_00/mbm_total_bytes is core->LLC traffic and mon_L3_01 LLC->DRAM.\n */\n"
         "static void cbqri_resctrl_pick_bc_mon_domains(void)\n{\n"
         "\tstruct cbqri_resctrl_res *l3 = &cbqri_resctrl_resources[RDT_RESOURCE_L3];\n\tstruct rdt_resource *res = &l3->resctrl_res;\n"
         "\tstruct cbqri_controller *ctrl;\n\tu32 nm = U32_MAX;\n\n\tif (l3->ctrl)\n\t\treturn;\n"
         "\tlist_for_each_entry(ctrl, &cbqri_controllers, list) {\n\t\tif (ctrl->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH || !ctrl->mon_capable)\n\t\t\tcontinue;\n"
         "\t\tnm = min(nm, ctrl->mcid_count);\n\t}\n\tif (nm == U32_MAX)\n\t\treturn;\n\n"
         "\tres->name = \"L3\";\n\tres->ctrl_scope = RESCTRL_L3_CACHE;\n\tres->mon_scope = RESCTRL_L3_CACHE;\n\tres->mon_capable = true;\n"
         "\tINIT_LIST_HEAD(&res->ctrl_domains);\n\tINIT_LIST_HEAD(&res->mon_domains);\n"
         "\tresctrl_enable_mon_event(QOS_L3_MBM_TOTAL_EVENT_ID, false, 0, NULL);\n"
         "\tbc_mon_domains = true;\n\texposed_mon_capable = true;\n\tmax_rmid = min(max_rmid, nm);\n"
         "\tpr_info(\"mbm_total_bytes from each bandwidth controller, %u MCIDs\\n\", nm);\n}\n\n"
         "static void cbqri_resctrl_accumulate_caps(void)\n{\n", 1),
        ("\tcbqri_resctrl_pick_counters();\n\n\tfor (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {\n\t\terr = cbqri_resctrl_control_init",
         "\tcbqri_resctrl_pick_counters();\n\tcbqri_resctrl_pick_bc_mon_domains();\n\n\tfor (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {\n\t\terr = cbqri_resctrl_control_init", 1),
        ("\texposed_alloc_capable = false;\n\texposed_mon_capable = false;\n\tmax_rmid = U32_MAX;\n\tcbqri_resctrl_inited = false;\n",
         "\texposed_alloc_capable = false;\n\texposed_mon_capable = false;\n\tbc_mon_domains = false;\n\tmax_rmid = U32_MAX;\n\tcbqri_resctrl_inited = false;\n", 1),
        ("/*\n * Attach a CPU to every controller that claims it.",
         "/* FireSim: this CPU joins the bandwidth controller's own L3 monitoring domain */\n"
         "static int cbqri_attach_cpu_to_bc_mon(struct cbqri_controller *ctrl, unsigned int cpu)\n{\n"
         "\tstruct rdt_resource *res = &cbqri_resctrl_resources[RDT_RESOURCE_L3].resctrl_res;\n"
         "\tstruct rdt_l3_mon_domain *mon_dom;\n\tstruct list_head *pos = NULL;\n\tint dom_id = ctrl->mem.prox_dom;\n\tint err;\n\n"
         "\tlockdep_assert_held(&cbqri_domain_list_lock);\n\n"
         "\tmon_dom = cbqri_find_l3_mon_domain(&res->mon_domains, dom_id);\n\tif (mon_dom) {\n\t\tcpumask_set_cpu(cpu, &mon_dom->hdr.cpu_mask);\n\t\treturn 0;\n\t}\n\n"
         "\terr = cbqri_init_bc_mon_counters(ctrl);\n\tif (err) {\n\t\tpr_err(\"BC @%pa: mon init failed (%d)\\n\", &ctrl->addr, err);\n\t\treturn err;\n\t}\n\n"
         "\tmon_dom = kzalloc_obj(*mon_dom, GFP_KERNEL);\n\tif (!mon_dom)\n\t\treturn -ENOMEM;\n"
         "\tmon_dom->hdr.id = dom_id;\n\tmon_dom->hdr.type = RESCTRL_MON_DOMAIN;\n\tmon_dom->hdr.rid = RDT_RESOURCE_L3;\n"
         "\tcpumask_set_cpu(cpu, &mon_dom->hdr.cpu_mask);\n\tINIT_LIST_HEAD(&mon_dom->hdr.list);\n"
         "\tresctrl_find_domain(&res->mon_domains, dom_id, &pos);\n\tif (pos)\n\t\tlist_add_tail(&mon_dom->hdr.list, pos);\n\telse\n\t\tlist_add_tail(&mon_dom->hdr.list, &res->mon_domains);\n\n"
         "\terr = resctrl_online_mon_domain(res, &mon_dom->hdr);\n\tif (err) {\n\t\tlist_del(&mon_dom->hdr.list);\n\t\tkfree(mon_dom);\n\t\treturn err;\n\t}\n\treturn 0;\n}\n\n"
         "/*\n * Attach a CPU to every controller that claims it.", 1),
        ("\t\tcase CBQRI_CONTROLLER_TYPE_BANDWIDTH:\n\t\t\tif (!cpumask_test_cpu(cpu, &ctrl->mem.cpu_mask))\n\t\t\t\tcontinue;\n"
         "\t\t\tif (!ctrl->alloc_capable)\n\t\t\t\tcontinue;\n\t\t\terr = cbqri_attach_cpu_to_bw_ctrl(ctrl, cpu);\n\t\t\tbreak;\n",
         "\t\tcase CBQRI_CONTROLLER_TYPE_BANDWIDTH:\n\t\t\tif (!cpumask_test_cpu(cpu, &ctrl->mem.cpu_mask))\n\t\t\t\tcontinue;\n"
         "\t\t\tif (ctrl->alloc_capable)\n\t\t\t\terr = cbqri_attach_cpu_to_bw_ctrl(ctrl, cpu);\n"
         "\t\t\tif (!err && bc_mon_domains && ctrl->mon_capable)\n\t\t\t\terr = cbqri_attach_cpu_to_bc_mon(ctrl, cpu);\n\t\t\tbreak;\n", 1),
        ("\t\tcbqri_detach_cpu_from_ctrl_domains(&hw_res->resctrl_res, cpu);\n\t\tif (rid == RDT_RESOURCE_L3 && hw_res->ctrl->mon_capable)\n"
         "\t\t\tcbqri_detach_cpu_from_l3_mon(&hw_res->resctrl_res, cpu);\n\t}\n}\n",
         "\t\tcbqri_detach_cpu_from_ctrl_domains(&hw_res->resctrl_res, cpu);\n\t\tif (rid == RDT_RESOURCE_L3 && hw_res->ctrl->mon_capable)\n"
         "\t\t\tcbqri_detach_cpu_from_l3_mon(&hw_res->resctrl_res, cpu);\n\t}\n"
         "\tif (bc_mon_domains)\n\t\tcbqri_detach_cpu_from_l3_mon(&cbqri_resctrl_resources[RDT_RESOURCE_L3].resctrl_res, cpu);\n}\n", 1)])

# ------------------------------------------------------------------ force: Ssqosid without firmware advertising it
if "force" in GROUPS:
    edit("arch/riscv/Kconfig", [
        ("config RISCV_ISA_SSQOSID\n",
         "config RISCV_FORCE_SSQOSID\n\tbool \"Assume Ssqosid is implemented even if firmware does not advertise it\"\n\tdepends on RISCV_ISA_SSQOSID\n\thelp\n"
         "\t  The FireSim CBQRI SoC implements the srmcfg CSR but its device tree\n\t  does not list \"ssqosid\". Say Y only for that hardware: on a hart\n"
         "\t  without the CSR the first context switch would trap.\n\nconfig RISCV_ISA_SSQOSID\n", 1)])
    edit("arch/riscv/kernel/cpufeature.c", [
        ("\t/*\n\t * We don't support systems with F but without D, so mask those out\n\t * here.\n\t */\n",
         "\tif (IS_ENABLED(CONFIG_RISCV_FORCE_SSQOSID)) {\n\t\tint cpu;\n\n"
         "\t\t/*\n\t\t * The FireSim CBQRI SoC implements srmcfg but its device tree\n\t\t * does not list ssqosid. The CSR is really there, so force the\n"
         "\t\t * extension on every hart (before the alternatives are patched).\n\t\t */\n"
         "\t\tset_bit(RISCV_ISA_EXT_SSQOSID, riscv_isa);\n\t\tfor_each_possible_cpu(cpu)\n\t\t\tset_bit(RISCV_ISA_EXT_SSQOSID, hart_isa[cpu].isa);\n"
         "\t\tpr_info(\"Ssqosid forced on (CONFIG_RISCV_FORCE_SSQOSID)\\n\");\n\t}\n\n"
         "\t/*\n\t * We don't support systems with F but without D, so mask those out\n\t * here.\n\t */\n", 1)])

# ------------------------------------------------------------------ irq: kernel-work RCID policy
if "irq" in GROUPS:
    copy("qos_work.h", "include/linux/qos_work.h")
    copy("qos_work.c", "arch/riscv/kernel/qos_work.c")
    edit("arch/riscv/kernel/Makefile", [
        ("obj-$(CONFIG_RISCV_ISA_SSQOSID) += qos.o\n", "obj-$(CONFIG_RISCV_ISA_SSQOSID) += qos.o qos_work.o\n", 1)])
    INC = "#include <linux/qos_work.h>\n"
    inc = first_include("arch/riscv/kernel/traps.c")
    edit("arch/riscv/kernel/traps.c", [
        (inc, inc + INC, 1),
        ("static void noinstr handle_riscv_irq(struct pt_regs *regs)\n{\n\tstruct pt_regs *old_regs;\n\n\tirq_enter_rcu();\n",
         "static void noinstr handle_riscv_irq(struct pt_regs *regs)\n{\n\tstruct pt_regs *old_regs;\n"
         "\t/* runs the handler under the RCID picked by qos_resctrl.irq_policy */\n\tunsigned long qc = qos_work_begin(QOS_WORK_HARDIRQ, 0, QOS_NOSWITCH);\n\n\tirq_enter_rcu();\n", 1),
        ("\tset_irq_regs(old_regs);\n\tirq_exit_rcu();\n}\n", "\tset_irq_regs(old_regs);\n\tirq_exit_rcu();\n\tqos_work_end(qc);\n}\n", 1)])
    inc = first_include("kernel/softirq.c")
    edit("kernel/softirq.c", [
        (inc, inc + INC, 1),
        ("static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;\n",
         "static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;\n"
         "/* srmcfg of whoever last raised each softirq on this CPU (qos_resctrl.irq_policy bit 2) */\n"
         "static DEFINE_PER_CPU(unsigned long, softirq_qos_tag[NR_SOFTIRQS]);\n", 1),
        ("\t\ttrace_softirq_entry(vec_nr);\n\t\th->action();\n\t\ttrace_softirq_exit(vec_nr);\n",
         "\t\ttrace_softirq_entry(vec_nr);\n\t\t{\n\t\t\tunsigned long qc = qos_work_begin(QOS_WORK_SOFTIRQ, vec_nr,\n"
         "\t\t\t\t\t\t\t  __this_cpu_read(softirq_qos_tag[vec_nr]));\n\t\t\th->action();\n\t\t\tqos_work_end(qc);\n\t\t}\n\t\ttrace_softirq_exit(vec_nr);\n", 1),
        ("void __raise_softirq_irqoff(unsigned int nr)\n{\n\tlockdep_assert_irqs_disabled();\n\ttrace_softirq_raise(nr);\n",
         "void __raise_softirq_irqoff(unsigned int nr)\n{\n\tlockdep_assert_irqs_disabled();\n\ttrace_softirq_raise(nr);\n\t__this_cpu_write(softirq_qos_tag[nr], qos_work_tag());\n", 1)])
    inc = first_include("kernel/time/timer.c")
    edit("kernel/time/timer.c", [
        (inc, inc + INC, 1),
        ("\tunsigned int idx = UINT_MAX;\n\tint ret = 0;\n\n\tdebug_assert_init(timer);\n",
         "\tunsigned int idx = UINT_MAX;\n\tint ret = 0;\n\n\tdebug_assert_init(timer);\n\ttimer->qos_tag = qos_work_tag();\n", 1),
        ("\tstruct timer_base *new_base, *base;\n\tunsigned long flags;\n\n\tdebug_assert_init(timer);\n",
         "\tstruct timer_base *new_base, *base;\n\tunsigned long flags;\n\n\tdebug_assert_init(timer);\n\ttimer->qos_tag = qos_work_tag();\n", 1),
        ("\ttrace_timer_expire_entry(timer, baseclk);\n\tfn(timer);\n\ttrace_timer_expire_exit(timer);\n",
         "\ttrace_timer_expire_entry(timer, baseclk);\n\t{\n\t\tunsigned long qc = qos_work_begin(QOS_WORK_TIMER, 0, timer->qos_tag);\n\n"
         "\t\tfn(timer);\n\t\tqos_work_end(qc);\n\t}\n\ttrace_timer_expire_exit(timer);\n", 1),
        ("\trcu_sched_clock_irq(user_tick);\n",
         "\t{\n\t\tunsigned long qc = qos_work_begin(QOS_WORK_RCU_TICK, 0, QOS_NOSWITCH);\n\n\t\trcu_sched_clock_irq(user_tick);\n\t\tqos_work_end(qc);\n\t}\n", 1)])
    edit("include/linux/timer_types.h", [
        ("\tu32\t\t\tflags;\n", "\tu32\t\t\tflags;\n\tunsigned long\t\tqos_tag;\t/* srmcfg of the armer (qos_work.h) */\n", 1)])
    inc = first_include("kernel/time/hrtimer.c")
    edit("kernel/time/hrtimer.c", [
        (inc, inc + INC, 1),
        ("\twas_first = cpu_base->next_timer == timer;\n", "\ttimer->qos_tag = qos_work_tag();\n\twas_first = cpu_base->next_timer == timer;\n", 1),
        ("\trestart = fn(timer);\n",
         "\t{\n\t\t/* hard hrtimers (the tick among them) follow the hardirq rule; soft ones their armer */\n"
         "\t\tunsigned long qc = qos_work_begin(QOS_WORK_TIMER, 0, timer->is_soft ? timer->qos_tag : QOS_NOSWITCH);\n\n"
         "\t\trestart = fn(timer);\n\t\tqos_work_end(qc);\n\t}\n", 1)])
    edit("include/linux/hrtimer_types.h", [
        ("\tbool\t\t\t\tis_lazy;\n", "\tbool\t\t\t\tis_lazy;\n\tunsigned long\t\t\tqos_tag;\t/* srmcfg of the armer (qos_work.h) */\n", 1)])

# ------------------------------------------------------------------ bytes: FireSim counters count 64-byte line fills
if "bytes" in GROUPS:
    R = "drivers/resctrl/cbqri_resctrl.c"
    edit(R, [
        ("static struct cbqri_controller *cbqri_mon_bc_for_domain(int id)\n",
         "/* FireSim regulators count 64-byte line fills; the spec's counter is in bytes */\n"
         "static u64 cbqri_bc_bytes(struct cbqri_controller *bc, u64 count)\n{\n\treturn bc->fs_regulator ? count * 64 : count;\n}\n\n"
         "static struct cbqri_controller *cbqri_mon_bc_for_domain(int id)\n", 1),
        ("\t\t\t*val = bc->mbm_total_states[rmid].chunks;\n", "\t\t\t*val = cbqri_bc_bytes(bc, bc->mbm_total_states[rmid].chunks);\n", 1),
        ("\t\t\ts->prev_ctr = 0;\n\t\t\t*val = s->chunks;\n", "\t\t\ts->prev_ctr = 0;\n\t\t\t*val = cbqri_bc_bytes(bc, s->chunks);\n", 1),
        ("\t\t\t*val        = s->chunks;\n", "\t\t\t*val        = cbqri_bc_bytes(bc, s->chunks);\n", 1)])
print("all edits applied:", GROUPS)
