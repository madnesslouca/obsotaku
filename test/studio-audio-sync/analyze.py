#!/usr/bin/env python3
"""Frozen acceptance for real headless libobs sync-offset PCM (no Qt/device)."""
import argparse,hashlib,json,pathlib,platform,numpy as np
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=pathlib.Path);p.add_argument('--report',type=pathlib.Path,required=True);a=p.parse_args();r=json.loads((a.root/'capture.json').read_text());failures=[];results=[]
def require(ok,message):
 if not ok:failures.append(message)
def amp(x,hz):
 w=np.hanning(len(x));return float(2*abs(np.sum(x*w*np.exp(-2j*np.pi*hz*np.arange(len(x))/48000)))/w.sum())
def env(x,hz):
 x=x[:len(x)//480*480].reshape(-1,480);return 2*np.abs(x@np.exp(-2j*np.pi*hz*np.arange(480)/48000))/480
def lag(x):
 m=env(x,997);ref=env(x,2003);scores=[]
 for k in range(-40,41):
  v,w=(m[k:],ref[:-k]) if k>0 else ((m[:k],ref[-k:]) if k<0 else (m,ref))
  score=float(np.corrcoef(v,w)[0,1]);scores.append((score,k))
 score,k=max(scores);return {'lag_ms':k*10,'correlation':score}
require(r['capture_passed'],r['error']);require(not r['backlog_failure'],'producer clock backlog failed')
for s in r['stages']:
 x=np.fromfile(a.root/s['file'],dtype='<f4');require(len(x)>=96000,f"{s['name']}: PCM<2s");require(np.isfinite(x).all(),f"{s['name']}: nonfinite PCM")
 # Boundaries excluded identically for all stages, not selected after results.
 x=x[4800:-4800];m={'997':amp(x,997),'2003':amp(x,2003)};rms=float(np.sqrt(np.mean(x*x)));peak=float(np.max(np.abs(x)));l=lag(x)
 for hz,v in m.items():require(v>=.001,f"{s['name']}: tone{hz} below.001 ({v})")
 require(rms>=.004 and peak<=.08,f"{s['name']}: unexpected PCM energy/clipping {rms}/{peak}");require(l['correlation']>=.90,f"{s['name']}: pulse correlation<.90 ({l})")
 results.append({**s,**l,'tone_amplitudes':m,'rms':rms,'peak':peak})
base=next((v['lag_ms'] for v in results if v['name']=='zero_baseline'),None);require(base is not None,'missing zero baseline')
for v in results:
 v['relative_delay_ms']=v['lag_ms']-base;v['error_ms']=v['relative_delay_ms']-v['offset_ms'];v['delay_passed']=abs(v['error_ms'])<=30
 require(v['delay_passed'],f"{v['name']}: measured{v['relative_delay_ms']}ms, expected{v['offset_ms']}±30ms")
require(len(results)==11,'expected11 declared stages')
core=pathlib.Path(r['loaded_core']);require(core.name.startswith('libobs.so') or core.name.lower()=='obs.dll','loaded_core is not an actual libobs library path');out={'passed':not failures,'analysis_host':platform.system(),'scope':r['scope'],'capture_passed':r['capture_passed'],'loaded_core':str(core),'core_sha256':hashlib.sha256(core.read_bytes()).hexdigest(),'stages':results,'failures':failures,'thresholds':{'minimum_frames':96000,'trim_frames_each_side':4800,'tone_min':.001,'rms_min':.004,'peak_max':.08,'correlation_min':.90,'delay_tolerance_ms':30,'pulse_period_ms':1000},'producer':{k:r[k] for k in ['max_lateness_ms','max_dispatch_ms','blocks','catchups','backlog_failure']},'gpu_or_endpoint_used':False,'Windows_native_validated':False}
with a.report.open('x') as f:json.dump(out,f,indent=2);f.write('\n')
print(json.dumps({'passed':out['passed'],'failures':failures,'delays':[(v['name'],v['relative_delay_ms'],v['offset_ms']) for v in results]},indent=2));raise SystemExit(0 if out['passed'] else 1)
