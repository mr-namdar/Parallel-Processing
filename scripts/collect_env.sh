#!/usr/bin/env bash
set -eu

mkdir -p results/system_info

lscpu > results/system_info/lscpu.txt
lscpu -e > results/system_info/lscpu_e.txt
nproc > results/system_info/nproc.txt
uname -a > results/system_info/uname.txt

if command -v systemd-detect-virt >/dev/null 2>&1; then
    systemd-detect-virt > results/system_info/virtualization.txt 2>&1 || true
else
    printf 'systemd-detect-virt is not installed\n' > results/system_info/virtualization.txt
fi

GOVERNOR_FILE='/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'
if [[ -r "$GOVERNOR_FILE" ]]; then
    cat "$GOVERNOR_FILE" > results/system_info/frequency_governor.txt
else
    printf 'not available in this VM/system\n' > results/system_info/frequency_governor.txt
fi

printf 'System information saved in results/system_info/\n'
printf 'nproc: '
cat results/system_info/nproc.txt
printf 'Virtualization: '
cat results/system_info/virtualization.txt
printf 'Frequency governor: '
cat results/system_info/frequency_governor.txt
