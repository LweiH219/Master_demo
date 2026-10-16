#!/usr/bin/env bash
# Measured profile for tide / i7-13620H / r8169 enp3s0, 2026-10-15.
# Usage: sudo ./scripts/rt-control-run.sh PROGRAM [ARGS...]
set -euo pipefail
if ((EUID != 0 || $# == 0)); then
  echo "Usage: sudo $0 PROGRAM [ARGS...]" >&2; exit 2
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec 9>/run/lock/master-demo-e2e.lock
flock -n 9 || { echo "Another EtherCAT profile/test is running" >&2; exit 2; }
test -e /sys/devices/system/cpu/cpu4
test -e /sys/devices/system/cpu/cpu6
test -w /sys/devices/system/cpu/intel_pstate/no_turbo
test -w /sys/class/net/enp3s0/threaded
mapfile -t irqs < <(ls /sys/class/net/enp3s0/device/msi_irqs)
if ((${#irqs[@]} != 1)) || [[ "$(cat "/proc/irq/${irqs[0]}/effective_affinity_list")" != 6 ]]; then
  echo "enp3s0 IRQ topology changed; revalidate the CPU6 NAPI profile before use." >&2; exit 2
fi
umask 022
state="$(mktemp -d "$root/build/rt-control-$(date +%Y%m%d-%H%M%S)-XXXXXX")"
chmod 755 "$state"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  gov=$(cat "$p/scaling_governor" 2>/dev/null) || continue
  epp=$(cat "$p/energy_performance_preference" 2>/dev/null) || continue
  printf '%s %s %s\n' "$p" "$gov" "$epp"
done > "$state/power.before"
cat /sys/devices/system/cpu/intel_pstate/no_turbo > "$state/no-turbo.before"
cat /sys/class/net/enp3s0/threaded > "$state/napi.before"
child= monitor=
cleanup() {
  rc=$?
  trap - EXIT INT TERM
  if [[ -n "$child" ]]; then kill -TERM "$child" 2>/dev/null || true; wait "$child" 2>/dev/null || true; fi
  if [[ -n "$monitor" ]]; then kill -TERM "$monitor" 2>/dev/null || true; wait "$monitor" 2>/dev/null || true; fi
  if [[ -f "$state/napi-threads.before.json" ]]; then
    if ! python3 - "$state/napi-threads.before.json" <<'PY'
import sys,os,json
for row in json.load(open(sys.argv[1])):
    try:
        os.sched_setscheduler(row['pid'],row['policy'],os.sched_param(row['prio']))
        os.sched_setaffinity(row['pid'],row['affinity'])
    except ProcessLookupError:
        pass
PY
    then rc=2; fi
  fi
  if ! cat "$state/napi.before" > /sys/class/net/enp3s0/threaded; then rc=2; fi
  if ! cat "$state/no-turbo.before" > /sys/devices/system/cpu/intel_pstate/no_turbo; then rc=2; fi
  while read -r p gov epp; do
    if ! printf '%s\n' "$gov" > "$p/scaling_governor"; then rc=2; fi
    if ! printf '%s\n' "$epp" > "$p/energy_performance_preference"; then rc=2; fi
  done < "$state/power.before"
  if [[ -f "$state/thermal-stop" ]]; then rc=2; fi
  echo "Profile released; original settings restored. State: $state; exit=$rc"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf '1\n' > /sys/devices/system/cpu/intel_pstate/no_turbo
while read -r p gov epp; do printf 'performance\n' > "$p/scaling_governor"; done < "$state/power.before"
printf '1\n' > /sys/class/net/enp3s0/threaded
python3 - "$state/napi-threads.before.json" <<'PY'
import sys,pathlib,os,json,time
rows=[]
for retry in range(20):
    for p in pathlib.Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            if not (p/'comm').read_text().strip().startswith('napi/enp3s0-'): continue
            pid=int(p.name)
            rows.append(dict(pid=pid,policy=os.sched_getscheduler(pid),
                prio=os.sched_getparam(pid).sched_priority,affinity=sorted(os.sched_getaffinity(pid))))
        except (OSError,ProcessLookupError): pass
    if rows: break
    time.sleep(.05)
if not rows: raise RuntimeError('No enp3s0 NAPI thread appeared')
json.dump(rows,open(sys.argv[1],'w'),indent=2)
for row in rows:
    os.sched_setaffinity(row['pid'],{6})
    os.sched_setscheduler(row['pid'],os.SCHED_FIFO,os.sched_param(90))
print('NAPI FIFO90 on CPU6:',[r['pid'] for r in rows],flush=True)
PY
echo "Profile: performance, turbo off, application FIFO40/CPU4; NAPI FIFO90/CPU6."
echo "State: $state; temperature stop: 85C."
chrt -f 40 taskset -c 4 "$@" & child=$!
python3 - "$child" "$state" <<'PY' &
import os,sys,time,csv,pathlib,signal
pid=int(sys.argv[1]); state=pathlib.Path(sys.argv[2])
with (state/'temperature.csv').open('w') as f:
    w=csv.writer(f);w.writerow(['unix_time','cpu_temp_C'])
    while pathlib.Path('/proc',str(pid)).exists():
        temp=int(pathlib.Path('/sys/class/thermal/thermal_zone1/temp').read_text())/1000
        w.writerow([time.time(),temp]);f.flush()
        if temp>=85:
            (state/'thermal-stop').write_text(str(temp))
            try: os.kill(pid,signal.SIGTERM)
            except ProcessLookupError: pass
            break
        time.sleep(1)
PY
monitor=$!
set +e
wait "$child"
rc=$?
set -e
child=
exit "$rc"
