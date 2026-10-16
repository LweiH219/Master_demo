#!/usr/bin/env python3
"""Temporary core reservation experiment; restores every changed setting."""
import os,sys,pathlib,subprocess,json,signal
root=pathlib.Path('/home/tide/Master_demo')
state=pathlib.Path(sys.argv[1]);state.mkdir(parents=True,exist_ok=True)
saved=[]
def change(path,value):
    p=pathlib.Path(path);old=p.read_text().strip()
    try:p.write_text(str(value)+'\n')
    except OSError as e:
        print('UNCHANGED',str(p),str(e),flush=True);return
    saved.append((str(p),old));print('CHANGED',str(p),old,'->',value,flush=True)
def stop(*_):raise KeyboardInterrupt
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
child=None;rc=2
try:
    for cpu in (5,7):change(f'/sys/devices/system/cpu/cpu{cpu}/online','0')
    change('/sys/devices/virtual/workqueue/cpumask','ff0f')
    for p in pathlib.Path('/proc/irq').glob('[0-9]*/effective_affinity_list'):
        if p.parent.name=='134':continue
        effective=p.read_text().strip()
        if effective in ('4','6'):change(p.parent/'smp_affinity_list','0-3,8-15')
    (state/'changes.json').write_text(json.dumps(saved,indent=2))
    child=subprocess.Popen(sys.argv[2:],cwd=root)
    rc=child.wait();child=None
except KeyboardInterrupt:rc=130
finally:
    if child is not None:
        child.terminate()
        try:child.wait(timeout=15)
        except subprocess.TimeoutExpired:child.kill();child.wait()
    errors=[]
    for path,value in reversed(saved):
        try:pathlib.Path(path).write_text(value+'\n')
        except OSError as e:errors.append([path,str(e)])
    (state/'restore.json').write_text(json.dumps({'errors':errors,'values':{p:pathlib.Path(p).read_text().strip() for p,v in saved}},indent=2))
    print('CORE_RESTORE',errors,flush=True)
    if errors:rc=2
sys.exit(rc)
