#!/usr/bin/env python3
"""Linux-only private relink of frozen real libobs objects plus audio-view adapter.
No original object, build output, dependency file or installed library is written.
"""
import argparse,hashlib,json,pathlib,shlex,subprocess,sys
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-root',type=pathlib.Path,required=True);p.add_argument('--output-directory',type=pathlib.Path,required=True);a=p.parse_args()
assert sys.platform.startswith('linux'),'This private linker driver is Linux-only; no Windows portability claim'
b=a.build_root.resolve();out=a.output_directory.resolve();out.mkdir(parents=True,exist_ok=True)
assert not (out/'libobs.so.30').exists(),'Use a fresh output directory to preserve before/after evidence'
source=pathlib.Path(__file__).resolve().parent/'audio-view-adapter.c'
cmds=subprocess.check_output(['ninja','-C',str(b),'-t','commands','libobs'],text=True).splitlines()
compileline=next(x for x in cmds if ' -c ' in x and '/obs-source.c' in x)
comp=shlex.split(compileline);oi=comp.index('-o');comp[oi+1]=str(out/'audio-view-adapter.o');ci=comp.index('-c');comp[ci+1]=str(source)
# Strip dependency-output switches so the original build tree remains read-only.
for key in ['-MF','-MT']:
 if key in comp:
  i=comp.index(key);del comp[i:i+2]
subprocess.run(comp,cwd=b,check=True)
line=next(x for x in cmds if ' -shared ' in x and ' -o libobs/libobs.so.30 ' in x)
tokens=shlex.split(line);start=next(i for i,t in enumerate(tokens) if t.endswith('/cc'));end=tokens.index('&&',start) if '&&' in tokens[start:] else len(tokens);link=tokens[start:end];link=[x for x in link if not x.startswith('-Wl,--dependency-file=')];i=link.index('-o');link[i+1]=str(out/'libobs.so.30');link.append(str(out/'audio-view-adapter.o'))
objects=[x for x in link if x.endswith('.o') and x!=str(out/'audio-view-adapter.o')]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
dependencies=(out/'audio-view-adapter.d').read_text().replace('\\\n',' ').split(':',1)[1]
headers={}
for dependency in shlex.split(dependencies):
 path=pathlib.Path(dependency)
 if path.is_file() and (str(path).startswith(str(source.parents[2])) or str(path).startswith(str(b))):headers[str(path)]=sha(path)
manifest={'kind':'Private real core relink with test-only route adapter; not installed core execution','adapter_source_sha256':sha(source),'internal_headers':headers,'objects':{x:sha(b/x) for x in objects},'compile_args':comp,'link_args':link}
subprocess.run(link,cwd=b,check=True)
(out/'libobs.so').symlink_to('libobs.so.30');manifest['core_sha256']=sha(out/'libobs.so.30');(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps({'path':str(out/'libobs.so.30'),'sha256':manifest['core_sha256'],'obs_source_object_sha256':manifest['objects']['libobs/CMakeFiles/libobs.dir/obs-source.c.o']},indent=2))
