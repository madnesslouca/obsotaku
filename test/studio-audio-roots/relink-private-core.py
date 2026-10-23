#!/usr/bin/env python3
"""Linux-only relink with a route/lock adapter; never install this private core."""
import argparse,hashlib,json,pathlib,shlex,subprocess,sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-root',type=pathlib.Path,required=True)
p.add_argument('--output-directory',type=pathlib.Path,required=True)
p.add_argument('--frozen-baseline',type=pathlib.Path)
p.add_argument('--registry',action='store_true')
a=p.parse_args();assert sys.platform.startswith('linux')
b=a.build_root.resolve();out=a.output_directory.resolve();out.mkdir(parents=True,exist_ok=True)
assert not (out/'libobs.so.30').exists(),'Fresh output directory required'
source=pathlib.Path(__file__).resolve().parent/'route-adapter.c'
lines=(a.frozen_baseline/'ninja-commands.txt').read_text().splitlines() if a.frozen_baseline else subprocess.check_output(['ninja','-C',str(b),'-t','commands','libobs'],text=True).splitlines()
comp=shlex.split(next(x for x in lines if ' -c ' in x and '/obs-source.c' in x))
comp[comp.index('-o')+1]=str(out/'route-adapter.o');comp[comp.index('-c')+1]=str(source)
for key in ['-MF','-MT']:
 if key in comp:i=comp.index(key);del comp[i:i+2]
if a.frozen_baseline:comp.insert(1,'-I'+str(a.frozen_baseline.resolve()/'source/libobs'))
if a.registry:comp[1:1]=['-DSTUDIO_TEST_AUDIO_REGISTRY','-DSTUDIO_TEST_AUDIO_REGISTRY_ATTACH=obs_audio_view_add','-DSTUDIO_TEST_AUDIO_REGISTRY_DETACH=obs_audio_view_remove','-DSTUDIO_TEST_AUDIO_REGISTRY_COLLECT=obs_audio_roots_collect']
with (out/'compile.log').open('w') as log:subprocess.run(comp,cwd=b,check=True,stdout=log,stderr=subprocess.STDOUT)
line=next(x for x in lines if ' -shared ' in x and ' -o libobs/libobs.so.30 ' in x)
t=shlex.split(line);start=next(i for i,v in enumerate(t) if v.endswith('/cc'));end=t.index('&&',start) if '&&' in t[start:] else len(t);link=[v for v in t[start:end] if not v.startswith('-Wl,--dependency-file=')]
link[link.index('-o')+1]=str(out/'libobs.so.30')
if a.frozen_baseline:
 old='libobs/CMakeFiles/libobs.dir/'
 link=[str(a.frozen_baseline.resolve()/'objects'/v[len(old):]) if v.startswith(old) and v.endswith('.o') else v for v in link]
link.append(str(out/'route-adapter.o'))
with (out/'link.log').open('w') as log:subprocess.run(link,cwd=b,check=True,stdout=log,stderr=subprocess.STDOUT)
(out/'libobs.so').symlink_to('libobs.so.30')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'registry':a.registry,'adapter_sha256':sha(source),'core_sha256':sha(out/'libobs.so.30'),'compile':comp,'link':link,'scope':'Private real core plus graphics-free route adapter and controlled lock hooks. Not installed core, GPU or native Windows certification.'}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps({'core':str(out/'libobs.so.30'),'sha256':manifest['core_sha256']}))
