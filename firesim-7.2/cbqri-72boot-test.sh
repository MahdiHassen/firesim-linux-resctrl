#!/bin/bash
# Boot test for the Linux 7.2 + CBQRI port on the FireSim quad-BOOM: does resctrl come up, do the regulators cap,
# do the per-regulator counters count, is sched_ext there, do the irq_policy knobs take writes.
# Lines "B72,<text>" on the console.
R=/sys/fs/resctrl; P=/sys/module/qos_resctrl/parameters
say() { echo "B72,$*"; }
up() { cut -d' ' -f1 /proc/uptime; }
taskset -p 0x1 $$ >/dev/null 2>&1
echo "==================== CBQRI 7.2 boot test ===================="
say "uname: $(uname -r) | $(grep -m1 isa /proc/cpuinfo)"
dmesg | grep -a -i "ssqosid\|cbqri\|qos\|resctrl\|sched_ext\|MB domain" | sed 's/^/B72,dmesg: /' | head -40
mount -t resctrl resctrl $R 2>&1 && say "resctrl mounted" || say "FAIL resctrl mount"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
say "info dirs: $(ls $R/info 2>/dev/null | tr '\n' ' ')"
for f in $R/info/*/*; do [ -f "$f" ] && say "$f = $(tr '\n' ' ' < $f 2>/dev/null)"; done
say "root schemata: $(tr '\n' '|' < $R/schemata 2>/dev/null)"
say "root mon_data: $(ls $R/mon_data 2>/dev/null | tr '\n' ' ') | mon_L3_00: $(ls $R/mon_data/mon_L3_00 2>/dev/null | tr '\n' ' ')"
say "sched_ext: $(ls /sys/kernel/sched_ext 2>/dev/null | tr '\n' ' ') | state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) | ops=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"
say "btf: $(ls -la /sys/kernel/btf/vmlinux 2>/dev/null | awk '{print $5}') bytes"
say "params: irq_policy=$(cat $P/irq_policy 2>/dev/null) irq_rcid=$(cat $P/irq_rcid 2>/dev/null) period=$(cat $P/period 2>/dev/null) mb_period=$(cat $P/mb_period 2>/dev/null) regulate=$(cat $P/regulate 2>/dev/null)"
say "srmcfg: $(cat /sys/kernel/debug/cbqri/per_cpu_srmcfg 2>/dev/null | tr '\n' ' ')"

# --- regulator check: group g1 on core 1 running the 64 KB LLC-resident attacker; cap MB domain 0 (core->LLC) -----------
mkdir -p $R/g1 && say "g1 created: $(ls $R/g1 | tr '\n' ' ')" || say "FAIL mkdir g1"
say "g1 schemata default: $(tr '\n' '|' < $R/g1/schemata)"
MAX=$(sed -n 's/^MB_MIN:0=\([0-9]*\).*/\1/p' $R/schemata); [ -z "$MAX" ] && MAX=52428
mon() { cat $R/g1/mon_data/mon_L3_0$1/mbm_total_bytes 2>/dev/null || echo 0; }
rate() { awk -v a=$1 -v b=$2 -v x=$3 -v y=$4 'BEGIN{d=y-x; if(d<=0)d=0.01; printf "%.1f", (b-a)/d/1e6}'; }
taskset -c 1 /root/BkPLL -c 1 -l 6 -b 0x40 -e 0 -m 128 -x -a write -i 100000000 > /tmp/bk.log 2>&1 &
BK=$!
echo $BK > $R/g1/tasks && say "attacker pid $BK in g1 (core 1)" || say "FAIL g1 tasks"
sleep 3
for cap in 1526 16 1 1526; do
    echo "MB_MIN:0=$cap;1=$MAX" > $R/g1/schemata || say "FAIL write MB_MIN:0=$cap"
    usleep 300000
    a0=$(mon 0); a1=$(mon 1); t0=$(up); sleep 2; b0=$(mon 0); b1=$(mon 1); t1=$(up)
    say "cap MB_MIN:0=$cap (reads back: $(tr '\n' '|' < $R/g1/schemata)) -> core->LLC $(rate $a0 $b0 $t0 $t1) MB/s, LLC->DRAM $(rate $a1 $b1 $t0 $t1) MB/s | ticks $(grep riscv-timer /proc/interrupts | awk '{print $2,$3,$4,$5}')"
done
kill $BK 2>/dev/null; wait $BK 2>/dev/null
say "attacker log: $(tail -c 200 /tmp/bk.log | tr '\n' ' ')"

# --- irq_policy knobs ---------------------------------------------------------------------------------------------------
for pol in 3 0x1c 0; do
    echo $((pol)) > $P/irq_policy && usleep 200000 && say "irq_policy write $pol -> $(cat $P/irq_policy) ticks $(grep riscv-timer /proc/interrupts | awk '{print $2,$3,$4,$5}')" || say "FAIL irq_policy write $pol"
done
echo 4 > $P/irq_rcid && say "irq_rcid write 4 -> $(cat $P/irq_rcid)"; echo 0 > $P/irq_rcid
rmdir $R/g1 2>/dev/null
umount $R 2>/dev/null && say "resctrl unmounted" || say "FAIL umount"
say "dmesg tail: $(dmesg | tail -5 | tr '\n' '|' | cut -c1-400)"
echo "==================== 7.2 boot test done ===================="
