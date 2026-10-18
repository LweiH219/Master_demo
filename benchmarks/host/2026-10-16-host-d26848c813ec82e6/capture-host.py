import pathlib,subprocess,json,hashlib,datetime
root=pathlib.Path('/home/tide/Master_demo')
def read(p):
 try:return pathlib.Path(p).read_text().strip()
 except OSError:return None
def cmd(s):
 p=subprocess.run(s,shell=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 return {'exit':p.returncode,'output':p.stdout.strip()}
dmi={n:read('/sys/class/dmi/id/'+n) for n in ['sys_vendor','product_name','product_version','board_vendor','board_name','board_version','bios_vendor','bios_version','bios_date']}
identity=read('/sys/class/dmi/id/product_uuid') or read('/etc/machine-id')
fingerprint=hashlib.sha256(identity.encode()).hexdigest()
data={'schema':1,'captured_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'host_id':'host-'+fingerprint[:16],'identity_sha256':fingerprint,'identity_basis':'DMI product_uuid' if read('/sys/class/dmi/id/product_uuid') else 'OS machine-id fallback','dmi':dmi}
commands={'hostname':'hostname','os':'cat /etc/os-release','kernel':'uname -a','cmdline':'cat /proc/cmdline','cpu':'lscpu -J','cpu_topology':'lscpu -e=CPU,CORE,SOCKET,ONLINE','memory':'free -b','memory_modules':'dmidecode --type 17','pci':'lspci -nnk','nic':'ethtool -i enp3s0','nic_link':'ethtool enp3s0','nic_coalesce':'ethtool -c enp3s0','disks':'lsblk -d -o NAME,MODEL,SIZE,ROTA','clocksource':'cat /sys/devices/system/clocksource/clocksource0/current_clocksource','rt_runtime':'sysctl kernel.sched_rt_period_us kernel.sched_rt_runtime_us','aspm':'cat /sys/module/pcie_aspm/parameters/policy','cpuidle':'cat /sys/devices/system/cpu/cpuidle/current_driver','kernel_config':"grep -E 'CONFIG_(PREEMPT_RT|NO_HZ_FULL|RCU_NOCB_CPU|HZ_1000)=' /boot/config-$(uname -r)",'project_commit':'git rev-parse HEAD','project_status':'git status --short','project_diff':'git diff --stat'}
data['inventory']={k:cmd(v) for k,v in commands.items()}
data['restored_current']={'online':read('/sys/devices/system/cpu/online'),'no_turbo':read('/sys/devices/system/cpu/intel_pstate/no_turbo'),'napi_threaded':read('/sys/class/net/enp3s0/threaded'),'workqueue_mask':read('/sys/devices/virtual/workqueue/cpumask'),'power':{p.name:{f:read(p/f) for f in ['scaling_driver','scaling_governor','energy_performance_preference','cpuinfo_max_freq','cpuinfo_min_freq']} for p in pathlib.Path('/sys/devices/system/cpu/cpufreq').glob('policy*')}}
data['sha256']={}
for name in ['build/ecat2can_master','src/rt_probe.h','src/ecat2can_master.c','scripts/rt-control-run.sh','build/rt-tests/20261015-e2e/load-low.py','NEW_CAN_RT_REPORT_2026-10-16.md','build/rt-tests/new-can-evidence-20261016.tar.gz']:
 p=root/name
 data['sha256'][name]=hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
out=root/'build/rt-tests/20261016-new-can/host-inventory.json'
out.write_text(json.dumps(data,ensure_ascii=False,indent=2))
print(data['host_id'])
