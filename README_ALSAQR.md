# resctrl on AlSaqr (VCU118)

Linux 7.2 with the vanilla RISC-V QoS series (Ssqosid `srmcfg` context switching, the `riscv_cbqri` driver, its
resctrl glue; commits up to 165c966e5 on `cbqri-7.2`, none of the FireSim-specific ones) plus a device-tree front end
for CBQRI capacity controllers, booted on the AlSaqr SoC (he-soc, four CVA6 cores). The LLC's CBQRI capacity
controller (1 MB, 16 ways, 16 RCIDs, `CONFIG_LIMIT` / `READ_LIMIT`, code/data AT, no monitoring) appears as the
resctrl `L3` resource.

## What is added on top of the series

- `drivers/resctrl/cbqri_dt.c` (`CONFIG_RISCV_CBQRI_DT`): registers each `riscv,cbqri-cache` node (the property
  names of the FireSim device trees: `reg`, `cache-level`, `riscv,cbqri-rcid`, `riscv,cbqri-mcid`) as a capacity
  controller of a cache shared by every CPU (domain id 0). Bandwidth controllers are not described this way.
- `struct cbqri_controller_info` gains `cache_level` and `cpus`: a discovery layer that knows them (device tree)
  passes them; ACPI leaves them unset and the driver resolves them through PPTT as before.
- `arch/riscv/configs/alsaqr_resctrl_defconfig`: AlSaqr's 6.18 kernel config (alsaqr-software branch `abdur-6.18`,
  `linux64_defconfig`) plus `RISCV_ISA_SSQOSID`, `RESCTRL_FS` (needs `MISC_FILESYSTEMS`), and `DEVTMPFS` / `TMPFS`,
  which Buildroot normally adds when it builds the kernel.
- `dts/alsaqr-resctrl.dts`: the four-CPU AlSaqr device tree with `_ssqosid` in `riscv,isa`, the CPUs' L1 caches
  (CVA6 WB config: I$ 8 KB 2-way, D$ 16 KB 8-way, 16-byte lines) and the LLC as their L3 (`next-level-cache`), so
  cacheinfo knows it, and the CBQRI node at `0x1040_1800`.

## Build

```
SDK=~/alsaqr-software/linux/cva6-sdk; export PATH=$SDK/buildroot/output/host/bin:$PATH
B=../build-alsaqr; cd kernel
make O=$B ARCH=riscv CROSS_COMPILE=riscv64-buildroot-linux-gnu- alsaqr_resctrl_defconfig
scripts/config --file $B/.config --set-str INITRAMFS_SOURCE $SDK/buildroot/output/images/rootfs.cpio
make O=$B ARCH=riscv CROSS_COMPILE=riscv64-buildroot-linux-gnu- olddefconfig
make O=$B ARCH=riscv CROSS_COMPILE=riscv64-buildroot-linux-gnu- -j$(nproc) Image
make -C $SDK/opensbi O=$(realpath $B)/opensbi PLATFORM=fpga/alsaqr CROSS_COMPILE=riscv64-buildroot-linux-gnu- \
  PLATFORM_RISCV_ISA=rv64imafdc PLATFORM_RISCV_XLEN=64 TARGET_FREQ=50000000 NUM_HARTS=4 \
  FW_PAYLOAD_PATH=$(realpath $B)/arch/riscv/boot/Image
dtc -I dts -O dtb -o $B/alsaqr-resctrl.dtb ../dts/alsaqr-resctrl.dts
```

(OpenSBI builds into its own output directory: `make` does not track `NUM_HARTS`, and the SDK's directory may hold
an eight-hart build.)

## Boot (alsaqr-mcp, network boot)

```
python3 -m alsaqr_mcp linux_boot netboot=true program=true bitstream=<build> \
  payload=$B/opensbi/platform/fpga/alsaqr/firmware/fw_payload.elf dtb=$B/alsaqr-resctrl.dtb
```

Use the plain loader (`software/netboot/netboot.riscv`): with the privilege-association loader (`QOS_KERNEL=`)
the kernel's own accesses carry `hqosid` instead of the running task's `srmcfg`. On 7.2 the Ethernet link sometimes
does not come up at boot; `ip link set eth0 down; ip link set eth0 up` on the console brings it up. The Xilinx DMA
driver's "failed to get axi_aclk" is harmless (axienet uses its own DMA code).

## Use

```
mount -t resctrl resctrl /sys/fs/resctrl
mkdir /sys/fs/resctrl/app; echo "L3:0=00ff" > /sys/fs/resctrl/app/schemata; echo <pid> > /sys/fs/resctrl/app/tasks
```

Checked on the board (2026-11-18, bitstream qosio_vm_4c): the groups' masks read back from the controller
(`READ_LIMIT`), a task's user and kernel accesses carry its group's RCID (alsaqr-software `linux/qosprobe`), and a
128 KB victim in ways 0-7 keeps its lines while an aggressor in ways 8-9 streams 4 MB (reads, writes, or its own
page faults): 96-140 of 2048 lines refetched, as alone (88), against all of them when the two share ways
(alsaqr-software `linux/resctrl/rdtpart`). A victim of half its partition (256 KB in 8 ways) does not stay resident
even alone: random physical pages overflow some sets.
