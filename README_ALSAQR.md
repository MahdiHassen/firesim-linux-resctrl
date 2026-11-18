# resctrl on AlSaqr (VCU118)

Linux 7.2 with the vanilla RISC-V QoS series (Ssqosid `srmcfg` context switching, the `riscv_cbqri` driver, its
resctrl glue; commits up to 165c966e5 on `cbqri-7.2`, none of the FireSim-specific ones) plus a device-tree front end
for CBQRI capacity controllers, booted on the AlSaqr SoC (he-soc, four CVA6 cores). The LLC's CBQRI capacity
controller (1 MB, 16 ways, 16 RCIDs, `CONFIG_LIMIT` / `READ_LIMIT`, code/data AT, no monitoring) appears as the
resctrl `L3` resource.

## What is added on top of the series

- `drivers/resctrl/cbqri_dt.c` (`CONFIG_RISCV_CBQRI_DT`): registers each `riscv,cbqri-cache` node (the property
  names of the FireSim device trees: `reg`, `cache-level`, `riscv,cbqri-rcid`, `riscv,cbqri-mcid`) as a capacity
  controller of a cache shared by every CPU (domain id 0), and each `riscv,cbqri-bandwidth` node as a bandwidth
  controller of every CPU (domain id 0): the LLC's CBQRI bandwidth controller at `0x1040_1C00` (he-soc branch
  `qos-bc`), which resctrl exposes as `MB_MIN` (Rbwb, 1..819 of 1024 blocks) and `MB_WGHT` (Mweight, 0 = hard limit).
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

## Memory bandwidth (he-soc `qos-bc`, bitstream qosbc2_4c)

The controller's implementation registers are not CBQRI and the driver leaves them alone: set the period, the bytes
per period that the 1024 blocks stand for, and enable regulation with pacing (1 SoC cycle = 1 target cycle):

```
devmem 0x10401C88 32 50000      # BC_PERIOD: 1 ms at 50 MHz
devmem 0x10401C90 32 40000      # BC_CAPACITY: 40 KB per period = 40 MB/s
devmem 0x10401C80 32 3          # BC_CTRL: EN | PACE
echo "MB_MIN:0=100" > /sys/fs/resctrl/schemata     # the root group holds 804 blocks: lower it first (sum <= 819)
echo "MB_MIN:0=410" > /sys/fs/resctrl/g1/schemata; echo "MB_WGHT:0=0" > /sys/fs/resctrl/g1/schemata
```

Checked on the board (2026-11-18), three readers on CPUs 1-3 in groups g1-g3 (alsaqr-software `linux/resctrl/rdtbw`,
one read per 64-byte line over 8 MB each): regulation off 28.3 / 28.3 / 28.2 MB/s (85 MB/s, the CCU's limit); on at
40 MB/s 12.2 each; `MB_MIN` 410 / 205 with `MB_WGHT` 0 (16 / 8 MB/s hard) and g3 best effort 14.9 / 7.7 / 11.8,
and 15.2 / 7.8 with g3 idle (the limits hold); `MB_WGHT` 32 / 64 / 128: 17.5 / 34.2 / 48.3 % (compressed toward
equal: each core has one miss in flight and the arbiter is work-conserving); g1 `MB_MIN` 410 `MB_WGHT` 1 against
two 255s: 41.8 %.

The series counted any mon-capable controller as making the system mon-capable; with a mon-capable BC next to an L3
CC without monitoring (this SoC) no event is exposed, `max_rmid` stays `U32_MAX` and the mount fails with ENOMEM.
Fixed here: only a mon-capable L3 CC makes the system mon-capable.
