#!/bin/bash
# Build the Linux 7.2 + CBQRI port (manager tree ~/fustini-linux, branch cbqri-v7.2) into FireMarshal workload cbqri-72boot
# and stage FireSim for it. Usage: scp -r kernel-7.2 ubuntu@$C5:~/ ; bash ~/kernel-7.2/c5-stage72.sh <agfi> [nodeploy]
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CY=/home/ubuntu/chipyard; FS=$CY/sims/firesim; M=$CY/software/firemarshal
source /opt/conda/etc/profile.d/conda.sh; conda activate $CY/.conda-env
export PATH=$CY/.conda-env/riscv-tools/bin:$PATH
W=cbqri-72boot
AGFI=${1:?usage: c5-stage72.sh <agfi> [nodeploy]}
KT=/home/ubuntu/fustini-linux

echo "=== [0] kernel tree ==="
( cd $KT && git log --oneline v7.2..HEAD | cat && git status --short | head -5 )

echo "=== [1] marshal workload $W ==="
rm -rf $M/workloads/$W; mkdir -p $M/workloads/$W/overlay/root
cp "$HERE/cbqri-72boot-test.sh" $M/workloads/$W/overlay/root/
cp ~/run34/overlay/root/BkPLL $M/workloads/$W/overlay/root/
cp "$HERE/cbqri-linux-config-7.2" $M/workloads/$W/
chmod +x $M/workloads/$W/overlay/root/*
cat > $M/workloads/$W.json <<EOJ
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
grep -E "^CONFIG_(RISCV_ISA_SSQOSID|RISCV_FORCE_SSQOSID|RESCTRL_FS|RISCV_CBQRI_DRIVER|RISCV_CBQRI_DT|RISCV_CBQRI_RESCTRL_FS|SCHED_CLASS_EXT|DEBUG_INFO_BTF|BPF_JIT|HZ|DEVMEM|DEBUG_FS|CMDLINE)=" $KT/.config
echo "kernel strings in image: dt $(grep -a -c 'cbqri-dt' $M/images/firechip/$W/$W-bin-nodisk) irq $(grep -a -c 'qos: irq_policy=' $M/images/firechip/$W/$W-bin-nodisk) forced $(grep -a -c 'Ssqosid forced on' $M/images/firechip/$W/$W-bin-nodisk)"

[ "$2" = nodeploy ] && { echo "=== built, not deployed ==="; exit 0; }
echo "=== [2] firesim workload json, runtime config ==="
cd $FS/deploy
grep -q "^cbqri_quad_boom_16_v3_60:" config_hwdb.yaml || cat >> config_hwdb.yaml <<EOH

cbqri_quad_boom_16_v3_60:
    agfi: $AGFI
    driver_tar: /home/ubuntu/cbqri-quad16-v3-driver/driver-bundle.tar.gz
    deploy_quintuplet_override: null
    custom_runtime_config: null
EOH
mkdir -p workloads/$W
cat > workloads/$W.json <<EOJ
{
  "benchmark_name": "$W",
  "common_bootbinary": "../../../../../software/firemarshal/images/firechip/$W/$W-bin-nodisk",
  "common_rootfs": "../../../../../software/firemarshal/boards/default/installers/firesim/dummy.rootfs",
  "common_simulation_outputs": ["uartlog"]
}
EOJ
sed -i -E "s/^(\s*workload_name:).*/\1 $W.json/" config_runtime.yaml
sed -i -E "s/^(\s*default_hw_config:).*/\1 cbqri_quad_boom_16_v3_60/" config_runtime.yaml
grep -n -E "default_hw_config|workload_name|plusarg_passthrough" config_runtime.yaml
echo "=== staged ==="
