"""Run native fixtures against an exact private runtime without changing installed files."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,time,sys
p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--core-file',type=Path,required=True);p.add_argument('--config',default='RelWithDebInfo');p.add_argument('--runtime-dir',type=Path,action='append',default=[]);p.add_argument('--with-cost',action='store_true');p.add_argument('--timeout',type=int,default=45);a=p.parse_args();root=a.output_dir.resolve();build=a.build_dir.resolve();core=a.core_file.resolve();sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
if root.exists():raise SystemExit('Use a new output directory; no cleanup or overwrite is performed')
if not core.is_file():raise SystemExit('Missing private runtime core file')
if os.name=='nt':expected_basename='obs.dll'
elif sys.platform.startswith('linux'):expected_basename='libobs.so.30'
else:raise SystemExit('This runtime selector supports Windows and Linux only; run other platforms manually')
argument_basename=a.core_file.name.lower() if os.name=='nt' else a.core_file.name
resolved_basename=core.name.lower() if os.name=='nt' else core.name
if argument_basename!=expected_basename or resolved_basename!=expected_basename:
 raise SystemExit('Pass the exact runtime basename '+expected_basename+'; arbitrary aliases are not accepted')
identity_scope='Hashes identify the named runtime artifact and its Windows copy, not each helper mapped module. Library search-path setup is not process-map proof.'
suites=[('graph','studio-active-graph-check',43),('scene','studio-active-scene-check',21),('lifecycle','studio-active-cycle-check',15),('pcm','studio-audio-hide-group-check',23),('contention','studio-active-contention-check',24),('overflow','studio-active-overflow-check',23),('preflight','studio-active-preflight-lock-check',8),('transition','studio-active-transition-check',60)]
if a.with_cost:suites.append(('cost','studio-active-gate-cost-check',4))
exes=[]
for label,name,count in suites:
 name+= '.exe' if os.name=='nt' else ''
 candidates=[build/name,build/a.config/name];exe=next((x for x in candidates if x.is_file()),None)
 if exe is None:raise SystemExit('Missing fixture '+name)
 exes.append((label,exe,count))
root.mkdir(parents=True);runtime=root/'bin';runtime.mkdir();core_before=sha(core)
# Windows loader searches beside the executable first. Place the exact pinned
# private DLL there; never run beside an installed/default obs.dll.
if os.name=='nt':shutil.copy2(core,runtime/core.name)
rows=[]
for label,exe,expected in exes:
 out=root/label;out.mkdir();child=runtime/exe.name;shutil.copy2(exe,child)
 env=os.environ.copy();paths=[str(core.parent)]+[str(x.resolve()) for x in a.runtime_dir]
 if os.name=='nt':
  env['PATH']=os.pathsep.join([str(runtime)]+paths+[env.get('PATH','')]);env['APPDATA']=str(out/'appdata');env['LOCALAPPDATA']=str(out/'localappdata')
  Path(env['APPDATA']).mkdir();Path(env['LOCALAPPDATA']).mkdir()
 else:env['LD_LIBRARY_PATH']=os.pathsep.join(paths+[env.get('LD_LIBRARY_PATH','')])
 for key,leaf in [('XDG_CONFIG_HOME','config'),('XDG_CACHE_HOME','cache'),('XDG_DATA_HOME','data')]:
  env[key]=str(out/leaf);Path(env[key]).mkdir()
 env['PULSE_SERVER']='unix:'+str(out/'denied-physical-audio.sock')
 cmd=[str(child),str(out/'result.json')];started=time.monotonic();timed_out=False
 try:
  with (out/'stdout.log').open('w') as stdout,(out/'stderr.log').open('w') as stderr:r=subprocess.run(cmd,env=env,stdout=stdout,stderr=stderr,timeout=a.timeout);code=r.returncode
 except subprocess.TimeoutExpired:timed_out=True;code=None
 report=out/'result.json';data=json.loads(report.read_text()) if report.is_file() else None;checks=data.get('checks',[]) if data else [];failed=sum(not x.get('passed',False) for x in checks)
 row={'suite':label,'command':cmd,'exit_code':code,'timeout':timed_out,'elapsed_seconds':time.monotonic()-started,'expected_checks':expected,'actual_checks':len(checks),'failed_checks':failed,'passed':bool(data and code==0 and not timed_out and len(checks)==expected and failed==0 and data.get('passed')),'exe_sha256':sha(exe),'private_core_sha256':core_before,'runtime_identity_scope':identity_scope,'report_sha256':sha(report) if report.is_file() else None}
 (out/'receipt.json').write_text(json.dumps(row,indent=2));rows.append(row);print(json.dumps({k:row[k] for k in ['suite','exit_code','actual_checks','failed_checks','passed']}),flush=True)
summary={'scope':'Native headless synthetic tests of exact private core; no install, normal profile, device, account, GPU or visual transition certification','core_path':str(core),'expected_runtime_basename':expected_basename,'runtime_identity_scope':identity_scope,'core_sha256_before':core_before,'core_sha256_after':sha(core),'core_preserved':core_before==sha(core),'platform':os.name,'suites':rows,'passed':all(x['passed'] for x in rows) and core_before==sha(core)};(root/'summary.json').write_text(json.dumps(summary,indent=2));raise SystemExit(0 if summary['passed'] else 1)
