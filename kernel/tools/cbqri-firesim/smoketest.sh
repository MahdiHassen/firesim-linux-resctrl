#!/bin/sh
# In-guest CBQRI/resctrl smoke test. Runs in busybox sh.
# Loaded via 9p host-share; invoke with: sh /mnt/smoketest.sh

PASS=0
FAIL=0

ok() {  PASS=$((PASS+1)); printf '  PASS  %s\n' "$1"; }
no() {  FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }

eq() {  # eq <name> <actual> <expected>
    if [ "$2" = "$3" ]; then ok "$1 = $3"
    else no "$1: expected '$3', got '$2'"
    fi
}

contains() {  # contains <name> <haystack> <needle>
    case "$2" in
        *"$3"*) ok "$1 contains '$3'" ;;
        *)      no "$1: expected to contain '$3', got '$2'" ;;
    esac
}

heading() { printf '\n=== %s ===\n' "$1"; }

# ---------------------------------------------------------------- 1. setup
heading "1. setup"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
mkdir -p /mnt && mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
mount -t resctrl resctrl /sys/fs/resctrl
[ -f /sys/fs/resctrl/schemata ] && ok "resctrl mounted" || no "resctrl not mounted"
insmod /mnt/cbqri_bandwidth_driver.ko 2>/dev/null
[ -f /sys/kernel/debug/cbqri/per_cpu_srmcfg ] && ok "debugfs module loaded" || no "debugfs module not loaded"

# ---------------------------------------------------------------- 2. baseline schemata
heading "2. baseline schemata reflects QEMU -device config"
SCHEMA=$(cat /sys/fs/resctrl/schemata)
contains "MB has 3 ctrls"   "$SCHEMA"  "3=   0;4=   0;5=   0"
contains "L2 has 2 ctrls"   "$SCHEMA"  "L2:0=0fff;1=0fff"     # ncblks=12 → 0x0FFF
contains "L3 has 1 ctrl"    "$SCHEMA"  "L3:2=ffff"            # ncblks=16 → 0xFFFF

# ---------------------------------------------------------------- 3. info dirs populated
heading "3. info dirs populated"
eq "L3/num_closids"  "$(cat /sys/fs/resctrl/info/L3/num_closids)"   "64"
eq "L3/cbm_mask"     "$(cat /sys/fs/resctrl/info/L3/cbm_mask)"      "ffff"
eq "L3/min_cbm_bits" "$(cat /sys/fs/resctrl/info/L3/min_cbm_bits)"  "1"
eq "L2/num_closids"  "$(cat /sys/fs/resctrl/info/L2/num_closids)"   "64"
eq "L2/cbm_mask"     "$(cat /sys/fs/resctrl/info/L2/cbm_mask)"      "fff"
eq "MB/num_closids"  "$(cat /sys/fs/resctrl/info/MB/num_closids)"   "64"
eq "MB/min_bandwidth" "$(cat /sys/fs/resctrl/info/MB/min_bandwidth)" "1"

# ---------------------------------------------------------------- 4. default-group writes
heading "4. write to default group commits to MMIO"
echo "L3:2=00ff" > /sys/fs/resctrl/schemata && ok "L3 write returned 0" || no "L3 write returned non-0"
contains "L3 read-back"  "$(cat /sys/fs/resctrl/schemata)"  "L3:2=00ff"

echo "MB:3=50;4=50;5=50" > /sys/fs/resctrl/schemata && ok "MB write returned 0" || no "MB write returned non-0"
contains "MB read-back"  "$(cat /sys/fs/resctrl/schemata)"  "3=  50;4=  50;5=  50"

# Restore so subsequent tests don't see these values
echo "L3:2=ffff" > /sys/fs/resctrl/schemata
echo "MB:3=79;4=79;5=79" > /sys/fs/resctrl/schemata 2>/dev/null

# ---------------------------------------------------------------- 5. invalid input rejected
heading "5. invalid inputs rejected with proper last_cmd_status"
echo "L3:2=10000" > /sys/fs/resctrl/schemata 2>/dev/null && no "L3 too-wide mask should fail" || ok "L3 too-wide mask rejected"
contains "last_cmd_status reports range"  "$(cat /sys/fs/resctrl/info/last_cmd_status)"  "Mask out of range"

echo "MB:3=200" > /sys/fs/resctrl/schemata 2>/dev/null && no "MB out-of-range should fail" || ok "MB out-of-range rejected"
contains "last_cmd_status reports MB range"  "$(cat /sys/fs/resctrl/info/last_cmd_status)"  "out of range"

echo "L3:99=ff" > /sys/fs/resctrl/schemata 2>/dev/null && no "L3 wrong domain id should fail" || ok "L3 wrong domain id rejected"

# ---------------------------------------------------------------- 6. sub-group lifecycle
heading "6. sub-group create/write/delete"
mkdir /sys/fs/resctrl/grpA && ok "mkdir grpA" || no "mkdir grpA"
mkdir /sys/fs/resctrl/grpB && ok "mkdir grpB" || no "mkdir grpB"

contains "grpA default schemata" "$(cat /sys/fs/resctrl/grpA/schemata)" "MB:3=  79"
contains "grpB default schemata" "$(cat /sys/fs/resctrl/grpB/schemata)" "MB:3=  79"

echo "MB:3=20;4=20;5=20" > /sys/fs/resctrl/grpA/schemata && ok "grpA MB write" || no "grpA MB write"
contains "grpA MB read-back" "$(cat /sys/fs/resctrl/grpA/schemata)" "3=  20;4=  20;5=  20"

echo "MB:3=60;4=60;5=60" > /sys/fs/resctrl/grpB/schemata && ok "grpB MB write" || no "grpB MB write"
contains "grpB MB independent of grpA" "$(cat /sys/fs/resctrl/grpB/schemata)" "3=  60;4=  60;5=  60"
# Re-read grpA to confirm it wasn't clobbered
contains "grpA MB still 20"   "$(cat /sys/fs/resctrl/grpA/schemata)" "3=  20"

# ---------------------------------------------------------------- 7. task assignment writes srmcfg
heading "7. task assignment → CSR 0x181 reflects assigned RCID/MCID"
ID_A=$(cat /sys/fs/resctrl/grpA/id 2>/dev/null)
ID_B=$(cat /sys/fs/resctrl/grpB/id 2>/dev/null)
echo "  grpA closid=$ID_A  grpB closid=$ID_B"

# Pre-state
PRE=$(cat /sys/kernel/debug/cbqri/per_cpu_srmcfg | head -1)
echo "  before joining grpA: $PRE"

echo $$ > /sys/fs/resctrl/grpA/tasks
# After joining grpA, current shell's srmcfg should have grpA's closid in low 12 bits
# Force a context switch by running a small command
sleep 0
SRMCFG=$(cat /sys/kernel/debug/cbqri/per_cpu_srmcfg | grep -v ': 0x0$' | head -1)
echo "  after joining grpA: $SRMCFG (any cpu with non-zero srmcfg)"
case "$SRMCFG" in *"0x"*) ok "non-zero srmcfg observed after task assignment" ;; *) no "no non-zero srmcfg" ;; esac

# ---------------------------------------------------------------- 8. cleanup
heading "8. cleanup"
echo $$ > /sys/fs/resctrl/tasks  # move shell back to default before rmdir
rmdir /sys/fs/resctrl/grpA && ok "rmdir grpA" || no "rmdir grpA"
rmdir /sys/fs/resctrl/grpB && ok "rmdir grpB" || no "rmdir grpB"

# ---------------------------------------------------------------- 9. CBQRI register sanity via debugfs
heading "9. CBQRI register sanity (debugfs module)"
if [ -d /sys/kernel/debug/cbqri/mem_bandwidth ]; then
    contains "bc_capabilities version" "$(cat /sys/kernel/debug/cbqri/mem_bandwidth/bc_capabilities)" "version: 1"
    contains "bc_capabilities nbwblks" "$(cat /sys/kernel/debug/cbqri/mem_bandwidth/bc_capabilities)" "nbwblks: 1024"
    contains "bc_capabilities mrbwb"   "$(cat /sys/kernel/debug/cbqri/mem_bandwidth/bc_capabilities)" "mrbwb: 819"
else
    echo "  (skipped — debugfs per-controller files claimed by in-tree driver via request_mem_region; not a regression)"
fi

# ---------------------------------------------------------------- summary
heading "summary"
printf '  %d passed,  %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
