# firesim-linux-resctrl

Linux 7.2 with RISC-V Ssqosid / CBQRI resctrl support, ported to the FireSim CBQRI SoC (quad-core BOOM, Chipyard inclusive LLC
with a per-RCID, per-LLC-bank request regulator, and a memory-traffic controller with a per-RCID, per-DRAM-bank regulator).

- `kernel/`: the kernel tree. It is Drew Fustini's upstream Ssqosid/CBQRI resctrl RFC (`srmcfg` CSR, resctrl `MB_MIN`/`MB_WGHT`,
  CBQRI capacity and bandwidth controller drivers), plus the FireSim and kernel-work patches listed below.
- `firesim-7.2/`: FireMarshal config fragment (`cbqri-linux-config-7.2`), boot test, the patch applier, reference copies of
  `cbqri_dt.c` / `qos_work.[ch]` (identical to the tree), and the manager staging script used for the runs (`c5-stage.sh`).

## Patches on top of the upstream RFC

| Change | What it does |
|---|---|
| FireSim device-tree discovery (`cbqri_dt.c`) | Finds the regulators without ACPI. Bandwidth controllers become resctrl domains in MMIO order: `MB_MIN:0` = core->LLC request regulator (per LLC bank), `MB_MIN:1` = LLC->DRAM regulator (per DRAM bank). |
| `CONFIG_RISCV_FORCE_SSQOSID` | BOOM does not advertise Ssqosid; force it on. |
| Regulator semantics | Hardware implements only `Rbwb` (a hard cap of 64-B fills per bank per period), no `Mweight`. Every RCID starts uncapped (MRBWB); regulators are enabled once every RCID has a budget. Period and enable come from boot parameters. Counters count 64-B fills and are reported in bytes. |
| `cbqri.max_ids=N` | Use at most N RCIDs/MCIDs per controller (the bitstreams advertise 16 but honour 4). |
| Counter reads on any CPU | `mbm_total_bytes` is read on the caller's CPU instead of via `smp_call_on_cpu` kworkers on the domain's first CPU. |
| Kernel-work tagging | `qos_resctrl.irq_policy` / `irq_rcid` (which RCID asynchronous kernel work runs under, "causer accounting") and `qos_kernel_rcid0` (all kernel-mode work under RCID 0). See below. |
| `srmcfg` sentinel fix | Never record or write the U32_MAX sentinel as a tag. |
| DEBUG: `HZ_20/50/83/125/167/500` | Extra tick options used only for tick-period experiments. |

## Boot parameters

| Parameter | Meaning | Default |
|---|---|---|
| `qos_resctrl.mb_period=<llc>,<dram>` | Regulator period in cycles, per `MB_MIN` domain (overrides `period`) | unset |
| `qos_resctrl.period=<cycles>` | One period for every regulator | 1000000 |
| `qos_resctrl.regulate=0\|1` | Switch regulation on at boot | 1 |
| `qos_resctrl.irq_policy=<bits>` | Which kernel work is re-tagged: 1 hardirq, 2 softirq, 16 RCU under `irq_rcid`; 4 softirq under its raiser, 8 timer, 32 hard hrtimer under its armer, 64 IPI/irq_work/wakeup under its sender, 128 device irq under its owner. `0` = never re-tag. Writable at run time in `/sys/module/qos_resctrl/parameters/`. | `0xfd` (causer accounting) |
| `qos_resctrl.irq_rcid=<rcid>` | The kernel's own RCID/MCID for the `irq_rcid` rules | 0 |
| `qos_kernel_rcid0=1` | Every entry from user mode (syscall, exception, interrupt) switches `srmcfg` to 0; the task's tag is restored on return to user mode | 0 (off) |
| `cbqri.max_ids=N` | Cap on RCIDs/MCIDs per controller | no cap |

A cap converts to bandwidth as MB/s per bank = blocks x 64 B x f_clk / period. At a 1 GHz target clock: 1 block = 160 MB/s per
bank at period 400, 13.3 MB/s per bank at 4800, 0.064 MB/s per bank at 1e6. The largest budget is 52428 blocks (3.36 GB/s per
bank at a 1 ms period).

## Kernel configurations

All variants use the same fragment, `firesim-7.2/cbqri-linux-config-7.2`, merged onto FireMarshal's RISC-V base config:

```
CONFIG_RISCV_ISA_SSQOSID=y
CONFIG_RISCV_FORCE_SSQOSID=y
CONFIG_RESCTRL_FS=y
CONFIG_RISCV_CBQRI_DRIVER=y
CONFIG_RISCV_CBQRI_DT=y
CONFIG_DEBUG_FS=y
CONFIG_DEVMEM=y
# CONFIG_STRICT_DEVMEM is not set
CONFIG_HZ_250=y
CONFIG_HZ=250
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
CONFIG_BPF_EVENTS=y
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_SCHED_DEBUG=y
CONFIG_CMDLINE="<one of the command lines below>"
```

`DEVMEM` without `STRICT_DEVMEM` lets the experiment harness program LLC way masks and regulator periods and read the latency
counters with `devmem`; BPF/BTF/sched_ext are for tooling. `HZ_250` is a 4 ms tick.

### 1. Kernel work on RCID 0 (used for the experiments since run 59)

Dual-regulator standard periods (LLC 400, DRAM 4800 cycles). This is the command line in the fragment:

```
CONFIG_CMDLINE="console=ttyS0 console=ttySIF0,3686400 earlycon qos_resctrl.mb_period=400,4800 qos_resctrl.irq_rcid=0 qos_resctrl.irq_policy=0 qos_kernel_rcid0=1 cbqri.max_ids=4"
```

Both regulators at 1 ms (runs 81-82):

```
CONFIG_CMDLINE="console=ttyS0 console=ttySIF0,3686400 earlycon qos_resctrl.mb_period=1000000,1000000 qos_resctrl.irq_rcid=0 qos_resctrl.irq_policy=0 qos_kernel_rcid0=1 cbqri.max_ids=4"
```

All kernel-mode work (syscalls, softirqs, interrupts, the tick, kernel threads, idle) runs under RCID/MCID 0, which is
unregulated and owns all LLC ways. A task's RCID tags only its user-mode accesses, so `mbm_total_bytes` of a group counts
user-mode traffic only and kernel traffic is never capped.

### 2. Upstream-equivalent tagging

```
CONFIG_CMDLINE="console=ttyS0 console=ttySIF0,3686400 earlycon qos_resctrl.mb_period=400,4800 qos_resctrl.irq_policy=0 cbqri.max_ids=4"
```

`irq_policy=0` disables all re-tagging and `qos_kernel_rcid0` stays off, so `srmcfg` changes only on context switch, as in the
upstream RFC: kernel work done while a task is current (its syscalls and faults, interrupts and softirqs that land on its CPU)
runs under that task's RCID; kernel threads run under the default group (RCID 0). Omitting `irq_policy` is NOT equivalent,
because this kernel's default is `0xfd`.

### 3. Causer accounting (this kernel's default)

```
CONFIG_CMDLINE="console=ttyS0 console=ttySIF0,3686400 earlycon qos_resctrl.mb_period=400,4800 qos_resctrl.irq_rcid=0 qos_resctrl.irq_policy=0xfd cbqri.max_ids=4"
```

Asynchronous kernel work runs under the RCID of the task that caused it (hard hrtimers under the task that armed them,
softirqs under their raiser, IPIs/irq_work/wakeups under the sender, device interrupts under their owner); only work with no
causer runs under `irq_rcid`. Syscalls and exceptions always keep the task's RCID.

## Using it from user space

- Classes are resctrl groups (`mkdir /sys/fs/resctrl/<group>`); CLOSID -> RCID, RMID -> MCID; tasks join through `tasks`.
- Caps: `echo "MB_MIN:0=<llc blocks>;1=<dram blocks>" > /sys/fs/resctrl/<group>/schemata` (errors in `info/last_cmd_status`).
- Monitoring: `mon_data/mon_L3_00/mbm_total_bytes` (core->LLC requests) and `mon_L3_01/mbm_total_bytes` (LLC->DRAM fills).
- Containers: `runc` places a container in a group through `linux.intelRdt.closID`; this works unmodified on top of this driver.
