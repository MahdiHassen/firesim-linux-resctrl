#!/bin/bash
# Reference copy of the FireSim manager staging script used for run 82 (2026-11-02): builds this kernel with FireMarshal and registers the bitstream.
# usage: c5-stage56.sh <agfi> <hw_config_name> <driver_bundle_dir> <pair 1|2|3>
# and stage FireSim for it. Usage: scp -r kernel-7.2 ubuntu@$C5:~/ ; bash ~/kernel-7.2/c5-stage72.sh <agfi> [nodeploy]
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CY=/home/ubuntu/chipyard; FS=$CY/sims/firesim; M=$CY/software/firemarshal
source /opt/conda/etc/profile.d/conda.sh; conda activate $CY/.conda-env
export PATH=$CY/.conda-env/riscv-tools/bin:$PATH
W=cbqri-irq82
AGFI=${1:?usage: c5-stage56.sh <agfi> <hwname> <driverdir> <pair>}; HWN=${2:?hwname}; DRVD=${3:?driver dir}; PAIR=${4:?pair}
KT=/home/ubuntu/fustini-linux

echo "=== [0] kernel tree ==="
GITID="-c user.name=firesim -c user.email=firesim@localhost"
if [ ! -d $KT/.git ]; then   # fresh manager: Linux 7.2 from GitHub + the 24 patches (18 RFC + 6 FireSim) from this directory
    git clone --depth 1 -b v7.2 https://github.com/torvalds/linux.git $KT ||   # GitHub rate-limits (429) -> kernel.org
    git clone --depth 1 -b v7.2 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git $KT
    ( cd $KT && git checkout -q -b cbqri-v7.2 && git $GITID am "$HERE"/patches/*.patch ) || { echo "FAIL: kernel patches"; exit 1; }
elif ! grep -q cbqri_dt_max_ids $KT/drivers/resctrl/cbqri_dt.c; then   # existing tree from run 35: add patch 0024 only
    ( cd $KT && git $GITID am "$HERE"/patches/0024-*.patch ) || { echo "FAIL: patch 0024"; exit 1; }
fi
( cd $KT && git log --oneline v7.2..HEAD | cat && git status --short | head -5 )
grep -q cbqri_dt_max_ids $KT/drivers/resctrl/cbqri_dt.c && echo "kernel tree has cbqri_dt.max_ids" || { echo "FAIL: max_ids missing"; exit 1; }
grep -q QOS_IRQ_IPI_SENDER $KT/arch/riscv/kernel/qos_work.c || ( cd $KT && git $GITID am "$HERE"/patches/0025-*.patch ) || { echo "FAIL: patch 0025"; exit 1; }
grep -q QOS_IRQ_IPI_SENDER $KT/arch/riscv/kernel/qos_work.c && echo "kernel tree has causer accounting (0025)" || { echo "FAIL: 0025 missing"; exit 1; }
grep -q "irq_policy = 0xfd" $KT/arch/riscv/kernel/qos_work.c || ( cd $KT && git $GITID am "$HERE"/patches/0026-*.patch ) || { echo "FAIL: patch 0026"; exit 1; }
grep -q "irq_policy = 0xfd" $KT/arch/riscv/kernel/qos_work.c && echo "kernel tree defaults to causer accounting (0026)" || { echo "FAIL: 0026 missing"; exit 1; }
grep -q qos_srmcfg_valid $KT/arch/riscv/kernel/qos_work.c || ( cd $KT && git $GITID am "$HERE"/patches/0027-*.patch ) || { echo "FAIL: patch 0027"; exit 1; }
grep -q qos_srmcfg_valid $KT/arch/riscv/kernel/qos_work.c && echo "kernel tree has the U32_MAX sentinel fix (0027)" || { echo "FAIL: 0027 missing"; exit 1; }
grep -q qos_kernel_rcid0 $KT/arch/riscv/kernel/qos.c || ( cd $KT && git $GITID am "$HERE"/patches/0028-*.patch ) || { echo "FAIL: patch 0028"; exit 1; }
grep -q qos_kernel_rcid0 $KT/arch/riscv/kernel/qos.c && echo "kernel tree has qos_kernel_rcid0 (0028)" || { echo "FAIL: 0028 missing"; exit 1; }
grep -q "QOS_L3_MBM_TOTAL_EVENT_ID, true" $KT/drivers/resctrl/cbqri_resctrl.c || ( cd $KT && git $GITID am "$HERE"/patches/0031-*.patch ) || { echo "FAIL: patch 0031"; exit 1; }
grep -q "QOS_L3_MBM_TOTAL_EVENT_ID, true" $KT/drivers/resctrl/cbqri_resctrl.c && echo "kernel tree reads mbm counters on any CPU (0031)" || { echo "FAIL: 0031 missing"; exit 1; }

echo "=== [1] marshal workload $W ==="
rm -rf $M/workloads/$W; mkdir -p $M/workloads/$W/overlay/root
cp -r "$HERE"/../overlay/root/* $M/workloads/$W/overlay/root/
echo $PAIR > $M/workloads/$W/overlay/root/pair.txt; echo "pair $PAIR, hw $HWN, agfi $AGFI"

cp "$HERE/cbqri-linux-config-7.2" $M/workloads/$W/
chmod +x $M/workloads/$W/overlay/root/*
cp "$HERE/../cbqri-irq82.json" $M/workloads/; : <<EOJ
{
  "name": "$W",
  "base": "br-base.json",
  "command": "bash /root/cbqri-72boot-test.sh",
  "linux": {
    "source": "$KT",
    "config": "cbqri-linux-config-7.2",
    "modules": { "icenet": null, "iceblk": null }
  },
  "overlay": "overlay"
}
EOJ
( cd $M && ./marshal clean workloads/$W.json; ./marshal -v -d build workloads/$W.json ) 2>&1 | tail -5
ls -la $M/images/firechip/$W/ | grep nodisk
echo "--- .config checks:"
grep -E "^CONFIG_(HZ|NO_HZ_FULL|RCU_NOCB_CPU|RISCV_ISA_SSQOSID|RISCV_FORCE_SSQOSID|RESCTRL_FS|RISCV_CBQRI_DRIVER|RISCV_CBQRI_DT|RISCV_CBQRI_RESCTRL_FS|SCHED_CLASS_EXT|DEBUG_INFO_BTF|BPF_JIT|HZ|DEVMEM|DEBUG_FS|CMDLINE)=" $KT/.config
echo "kernel strings in image: dt $(grep -a -c 'cbqri-dt' $M/images/firechip/$W/$W-bin-nodisk) irq $(grep -a -c 'qos: irq_policy=' $M/images/firechip/$W/$W-bin-nodisk) forced $(grep -a -c 'Ssqosid forced on' $M/images/firechip/$W/$W-bin-nodisk)"

[ "$2" = nodeploy ] && { echo "=== built, not deployed ==="; exit 0; }
echo "=== [2] firesim workload json, runtime config ==="
cd $FS/deploy
grep -q "^$HWN:" config_hwdb.yaml || cat >> config_hwdb.yaml <<EOH

$HWN:
    agfi: $AGFI
    driver_tar: $DRVD/driver-bundle.tar.gz
    deploy_quintuplet_override: null
    custom_runtime_config: null
EOH
mkdir -p workloads/$W
cat > workloads/$W.json <<EOJ
{
  "benchmark_name": "$W",
  "common_bootbinary": "../../../../../software/firemarshal/images/firechip/$W/$W-bin-nodisk",
  "common_rootfs": "../../../../../software/firemarshal/boards/default/installers/firesim/dummy.rootfs",
  "common_outputs": ["/root/cbqri_pct8_results.csv"],
  "common_simulation_outputs": ["uartlog", "memory_stats*.csv"]
}
EOJ
sed -i -E "s/^(\s*workload_name:).*/\1 $W.json/" config_runtime.yaml
sed -i -E "s/^(\s*default_hw_config:).*/\1 $HWN/" config_runtime.yaml
grep -n -E "default_hw_config|workload_name|plusarg_passthrough" config_runtime.yaml
echo "=== staged ==="
