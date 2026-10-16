#!/usr/bin/env bash
# Host-only 1 kHz timing test. No EtherCAT socket or CAN traffic.
# Usage: sudo ./scripts/rt-host-test.sh [seconds=30] [current|performance|performance-no-turbo]
set -euo pipefail
seconds="${1:-30}"
mode="${2:-current}"
if [[ ! "$seconds" =~ ^[0-9]+$ ]] || ((seconds < 1 || seconds > 3600)); then
  echo "seconds must be 1..3600" >&2; exit 2
fi
if [[ "$mode" != current && "$mode" != performance && "$mode" != performance-no-turbo ]]; then
  echo "mode must be current, performance, or performance-no-turbo" >&2; exit 2
fi
if ((EUID != 0)); then
  echo "Run with sudo; FIFO scheduling and memory locking require privileges." >&2; exit 1
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
for tool in cyclictest chrt python3 flock; do command -v "$tool" >/dev/null; done
test -x "$root/build/ecat2can_master"
exec 9>/run/lock/master-demo-rt-host.lock
flock -n 9 || { echo "Another host test is running" >&2; exit 1; }
out="$(mktemp -d "$root/build/rt-host-${mode}-$(date +%Y%m%d-%H%M%S)-XXXXXX")"
cleanup() {
  rc=$?
  trap - EXIT
  while read -r policy governor epp; do
    if ! printf '%s\n' "$governor" > "$policy/scaling_governor"; then rc=1; fi
    if [[ "$epp" != - ]]; then
      if ! printf '%s\n' "$epp" > "$policy/energy_performance_preference"; then rc=1; fi
    fi
  done < "$out/power-state.tsv"
  if [[ -f "$out/no-turbo.before" ]]; then
    if ! cat "$out/no-turbo.before" > /sys/devices/system/cpu/intel_pstate/no_turbo; then rc=1; fi
  fi
  echo "Results: $out (power restore exit status: $rc)"
  exit "$rc"
}
: > "$out/power-state.tsv"
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
  [[ -d "$policy" ]] || continue
  epp=-
  if [[ -r "$policy/energy_performance_preference" ]]; then epp="$(cat "$policy/energy_performance_preference")"; fi
  printf '%s %s %s\n' "$policy" "$(cat "$policy/scaling_governor")" "$epp" >> "$out/power-state.tsv"
done
{ date -Is; uname -a; cat /proc/cmdline; lscpu; } > "$out/environment.txt"
if [[ "$mode" == performance-no-turbo ]]; then
  cat /sys/devices/system/cpu/intel_pstate/no_turbo > "$out/no-turbo.before"
  printf "1\n" > /sys/devices/system/cpu/intel_pstate/no_turbo
fi
if [[ "$mode" == performance || "$mode" == performance-no-turbo ]]; then
  while read -r policy _ _; do printf 'performance\n' > "$policy/scaling_governor"; done < "$out/power-state.tsv"
fi
{ for policy in /sys/devices/system/cpu/cpufreq/policy*; do [[ -d "$policy" ]] || continue; printf "%s %s\n" "$policy" "$(cat "$policy/scaling_governor")"; done;
  if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then printf "no_turbo="; cat /sys/devices/system/cpu/intel_pstate/no_turbo; fi; } > "$out/active-power-state.txt"
echo "Project host timing: ${seconds}s, FIFO 80, mode=$mode"
chrt -f 80 "$root/build/ecat2can_master" enp3s0 rt "$seconds" host | tee "$out/project.txt"
# Keep cpu_dma_latency unchanged so the comparison measures governor effects.
cpus="$(cat /sys/devices/system/cpu/online)"
threads="$(getconf _NPROCESSORS_ONLN)"
echo "Absolute wakeup latency: ${seconds}s, $threads CPUs, FIFO 80, memory locked"
cyclictest --default-system -m -a "$cpus" -t "$threads" -p 80 -i 1000 \
  -D "${seconds}s" -q -h 2000 --histfile="$out/cyclic.hist" --json="$out/cyclic.json" \
  > "$out/cyclic.txt" 2>&1
cat "$out/cyclic.txt"
python3 - "$out/cyclic.json" <<'PY'
import sys,json,math
d=json.load(open(sys.argv[1]))
threads=list(d["thread"].values())
hist={}
for t in threads:
    for latency,count in t["histogram"].items():
        v=int(latency); hist[v]=hist.get(v,0)+count
n=sum(t["cycles"] for t in threads)
if sum(hist.values()) != n:
    print("Histogram overflow: use thread maximum; percentiles omitted")
else:
    for q in (.5,.99,.999):
        count=0
        for latency in sorted(hist):
            count+=hist[latency]
            if count>=math.ceil(n*q):
                print(f"wakeup p{q*100:g}: {latency} us"); break
print("wakeup max:",max(t["max"] for t in threads),"us; samples:",n)
print("Finite observations only; this does not certify a hard realtime deadline.")
PY
