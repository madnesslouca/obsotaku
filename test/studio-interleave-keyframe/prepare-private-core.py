#!/usr/bin/env python3
"""Freeze core objects and metadata; --build compiles only the private replacement TU.
Never writes installed/shared build outputs. A private core is not distributable.
"""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-root',type=pathlib.Path,required=True)
p.add_argument('--output-directory',type=pathlib.Path,required=True)
p.add_argument('--build',action='store_true')
p.add_argument('--obs-output-source',type=pathlib.Path,help='Explicit preserved before/after source; defaults to repository production file')
a=p.parse_args();build=a.build_root.resolve();out=a.output_directory.resolve();here=pathlib.Path(__file__).resolve().parent;repo=here.parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
manifest=out/'manifest.json'
if not manifest.exists():
 out.mkdir(parents=True,exist_ok=False);src=out/'source';src.mkdir();objects=out/'objects';objects.mkdir()
 commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','libobs'],text=True).splitlines()
 (out/'ninja-commands.txt').write_text('\n'.join(commands)+'\n')
 compile=shlex.split(next(x for x in commands if ' -c ' in x and x.rstrip().endswith('/obs-output.c')))
 linkline=next(x for x in commands if ' -shared ' in x and ' -o libobs/libobs.so.30 ' in x)
 tokens=shlex.split(linkline);start=next(i for i,x in enumerate(tokens)if x.endswith('/cc'));end=tokens.index('&&',start)if'&&'in tokens[start:]else len(tokens)
 link=[x for x in tokens[start:end]if not x.startswith('-Wl,--dependency-file=')]
 link[link.index('-o')+1]=str(out/'libobs.so.30');frozen={}
 for i,x in enumerate(link):
  q=pathlib.Path(x)
  if not x.startswith('-') and q.suffix in ['.o','.a']:
   original=q if q.is_absolute()else build/q;target=objects/x if not q.is_absolute() else objects/q.name
   target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original,target);frozen[str(original)]={'copy':str(target),'sha256':sha(target)};link[i]=str(target)
 source=(a.obs_output_source.resolve() if a.obs_output_source else repo/'libobs/obs-output.c');shutil.copy2(source,src/'obs-output-original.c');shutil.copy2(here/'adapter.c',src/'adapter.c');shutil.copy2(here/'probe.h',src/'probe.h')
 (src/'obs-output-with-adapter.c').write_bytes(source.read_bytes()+b'\n'+(src/'adapter.c').read_bytes())
 replacement=str(out/'obs-output-adapter.o')
 # Replace exactly the original implementation; never link duplicate public symbols.
 original_copies=[v['copy']for k,v in frozen.items()if k.endswith('/obs-output.c.o')]
 assert len(original_copies)==1
 link=[replacement if x==original_copies[0] else x for x in link]
 for key in ['-MF','-MT']:
  if key in compile:i=compile.index(key);del compile[i:i+2]
 compile[compile.index('-o')+1]=replacement;compile[compile.index('-c')+1]=str(src/'obs-output-with-adapter.c')
 meta={'prepared_only':True,'product_source_sha256':sha(source),'obs_output_source':str(source),'adapter_sha256':sha(src/'adapter.c'),'probe_header_sha256':sha(src/'probe.h'),'frozen_objects':frozen,'compile':compile,'link':link,'build_cwd':str(build),'scope':'Exact production obs-output.c plus private adapter; replace only its copied object. No installed files or production source mutation.'}
 manifest.write_text(json.dumps(meta,indent=2)+'\n')
else:meta=json.loads(manifest.read_text())
if a.build:
 assert not (out/'libobs.so.30').exists(),'Fresh build result required; preserve previous outcomes'
 for v in meta['frozen_objects'].values():assert sha(pathlib.Path(v['copy']))==v['sha256']
 for name,cmd in [('compile',meta['compile']),('link',meta['link'])]:
  with (out/(name+'.log')).open('w')as log:subprocess.run(cmd,cwd=meta['build_cwd'],check=True,stdout=log,stderr=subprocess.STDOUT)
 (out/'libobs.so').symlink_to('libobs.so.30');meta['prepared_only']=False;meta['private_core_sha256']=sha(out/'libobs.so.30');manifest.write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps({'prepared':True,'built':not meta['prepared_only'],'manifest':str(manifest),'frozen_objects':len(meta['frozen_objects'])}))
