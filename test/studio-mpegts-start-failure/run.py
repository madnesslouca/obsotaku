#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run fixed native MPEGTS ownership checks; this is not decoded-media proof."""
import argparse, hashlib, json, os, re, subprocess
from pathlib import Path
CONTROL_FAILURES = {
 'slow-codec': [
  'FAIL\tnative heavy disposal executor owns no data mutex',
  'FAIL\tlive AV and transport fields detached before heavy close',
  'FAIL\tactual late data callback returns before heavy close gate release'],
 'slow-avio': [
  'FAIL\tnative heavy disposal executor owns no data mutex',
  'FAIL\tlive AV and transport fields detached before heavy close',
  'FAIL\tactual late data callback returns before heavy close gate release'],
 'opaque': [
  'FAIL\tAVIO writer runs before protocol opaque storage is freed',
  'FAIL\tnative flush sees live URLContext and transport private storage']}

def validate_result(text, expected_total, expected_failed):
 """Validate every check row and the single final summary, including controls."""
 lines=text.splitlines()
 summaries=[line for line in lines if line.startswith('TOTAL')]
 if len(summaries)!=1 or not lines or lines[-1]!=summaries[0]:
  raise ValueError('Expected one final TOTAL summary')
 match=re.fullmatch(r'TOTAL\t(\d+)\tFAILURES\t(\d+)',summaries[0])
 if match is None:
  raise ValueError('Malformed TOTAL summary')
 checks=lines[:-1]
 if any(not (line.startswith('PASS\t') or line.startswith('FAIL\t')) for line in checks):
  raise ValueError('Unexpected check row')
 failed=[line for line in checks if line.startswith('FAIL\t')]
 if len(checks)!=expected_total or int(match[1])!=expected_total:
  raise ValueError('Unexpected check count')
 if failed!=list(expected_failed) or int(match[2])!=len(expected_failed):
  raise ValueError('Unexpected failed check names, order or count')
 return {'total':int(match[1]),'failures':int(match[2]),'failed':failed}

def main():
 p=argparse.ArgumentParser()
 p.add_argument('--build',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--runtime-dir',type=Path,action='append',default=[])
 p.add_argument('--with-control',action='store_true')
 a=p.parse_args()
 a.output.mkdir(exist_ok=False)
 env=os.environ.copy()
 if a.runtime_dir:
  key='PATH' if os.name=='nt' else 'LD_LIBRARY_PATH'
  env[key]=os.pathsep.join([str(v.resolve()) for v in a.runtime_dir]+([env[key]] if env.get(key) else []))
 rows=[]
 def execute(name,target,arguments,total=None,failures=0,expected_failed=None):
  binary=a.build/(target+('.exe' if os.name=='nt' else ''))
  try:
   done=subprocess.run([str(binary),*[str(v) for v in arguments]],env=env,capture_output=True,text=True,timeout=12)
  except subprocess.TimeoutExpired as e:
   (a.output/(name+'.log')).write_bytes((e.stdout or b'')+(e.stderr or b''))
   raise SystemExit('STOP: unexpected timeout: '+name)
  (a.output/(name+'.log')).write_text(done.stdout+done.stderr)
  row={'name':name,'executable_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'exit':done.returncode}
  if total is not None:
   try:
    if len(expected_failed or [])!=failures:
     raise ValueError('Runner control specification inconsistent')
    row.update(validate_result((a.output/(name+'.tsv')).read_text(),total,expected_failed or []))
   except (ValueError,OSError) as error:
    row['validation_error']=str(error)
    rows.append(row)
    (a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
    raise SystemExit('STOP: invalid result: '+str(row))
  rows.append(row)
  (a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
  if done.returncode!=(1 if failures else 0) or (total is not None and (row['total']!=total or row['failures']!=failures)):
   raise SystemExit('STOP: unexpected result: '+str(row))
 execute('preflight','preflight',[a.output/'preflight.ts'])
 modes={'false':29,'true':31,'early-false':29,'early-true':29,'early-before':29,'stop-false':29,'stop-true':29,'header-false':29,'header-true':29,'worker-create-fail':29,'open-fail':29,'open-fail-notify-stop':29,'retry-false':33,'metadata-true':31,'metadata-false':29,'metadata-drain-false':35,'late-after-stop':38,'late-during-stop':39}
 for label in (['control','after'] if a.with_control else ['after']):
  for mode in ['retiring-stop','before-claim-stop']:
   name=label+'-'+mode
   execute(name,'overlap-'+label,[a.output/(name+'.tsv'),a.output/name,mode],24)
  for family,mode,total in [('slow','slow-codec',26),('slow','slow-avio',26),('opaque',None,13)]:
   name=label+'-'+(mode or 'opaque')
   failures=(3 if family=='slow' else 2) if label=='control' else 0
   execute(name,family+'-'+label,[a.output/(name+'.tsv'),a.output/name]+([mode] if mode else []),total,failures,CONTROL_FAILURES[mode or 'opaque'] if label=='control' else [])
  for mode,total in modes.items():
   name=label+'-'+mode
   execute(name,'check-'+label,[a.output/(name+'.tsv'),a.output/name,mode],total)
 print('PASS: all fixed expected outcomes; controls are not successful product cases')

if __name__=='__main__':
 main()
