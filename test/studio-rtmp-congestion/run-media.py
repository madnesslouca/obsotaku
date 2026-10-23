#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run only in a new private directory after the coordinator frees the GPU window.
No account/device fixtures; the output module owns all local RTMP processes.
"""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=pathlib.Path);p.add_argument('--prefix',type=pathlib.Path,required=True)
p.add_argument('--module',type=pathlib.Path,required=True);p.add_argument('--server',type=pathlib.Path,required=True)
p.add_argument('--python',default='python3');p.add_argument('--ffmpeg-receiver',action='store_true')
p.add_argument('--preserve-config-root',type=pathlib.Path,action='append',default=[],
               help='Additional normal configuration directory to hash before and after the isolated run; repeat as needed')
args=p.parse_args()
root=args.root.resolve();prefix=args.prefix.resolve();repo=pathlib.Path(__file__).resolve().parents[2]
if root.exists():raise SystemExit('Refusing to reuse any evidence directory')
if not args.module.is_file() or not args.server.is_file():raise SystemExit('Missing module or MediaMTX')
root.mkdir(parents=True);cfg=root/'config';base=cfg/'obs-studio';profile=base/'basic/profiles/Congestion_QA';profile.mkdir(parents=True)
for part in ('cache','data','runtime'): (root/part).mkdir()
(root/'runtime').chmod(0o700)
plugin=base/'plugins/studio-rtmp-congestion-check';(plugin/'bin/64bit').mkdir(parents=True);(plugin/'data').mkdir()
shutil.copy2(args.module,plugin/'bin/64bit'/args.module.name)
(base/'global.ini').write_text(f'''[General]
Language=en-US
EnableAutoUpdates=false
LastVersion=537001986
[Locations]
Configuration={cfg}
SceneCollections={cfg}
Profiles={cfg}
PluginManagerSettings={cfg}
[Video]
Renderer=OpenGL
''')
(base/'user.ini').write_text('''[General]
FirstRun=true
Language=en-US
[Basic]
Profile=Congestion QA
ProfileDir=Congestion_QA
SceneCollection=Congestion QA
SceneCollectionFile=Congestion_QA
[BasicWindow]
StudioWorkspace=true
StudioMultiCanvas=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
WarnBeforeStoppingRecord=false
PreviewEnabled=false
SysTrayEnabled=false
''')
profile.joinpath('basic.ini').write_text(f'''[General]
Name=Congestion QA
[Video]
BaseCX=640
BaseCY=360
OutputCX=640
OutputCY=360
FPSType=1
FPSCommon=15
FPSInt=15
FPSNum=15
FPSDen=1
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial
[Audio]
SampleRate=48000
ChannelSetup=Stereo
[Output]
Mode=Simple
DynamicBitrate=true
[SimpleOutput]
FilePath={root}
RecFormat2=mkv
StreamEncoder=x264
RecEncoder=x264
VBitrate=2500
ABitrate=128
''')
ws=base/'plugin_config/obs-websocket';ws.mkdir(parents=True);(ws/'config.json').write_text('{"server_enabled":false,"alerts_enabled":false}')
sha=lambda path:hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
normal_roots=[pathlib.Path.home()/'.config/obs-studio',*[d.expanduser().resolve() for d in args.preserve_config_root]]
def normal_hashes():
    return {str(f):sha(f) for d in normal_roots if d.exists() for f in sorted(d.rglob('*')) if f.is_file() and f.suffix in ('.json','.ini') and '/logs/' not in str(f)}
before=normal_hashes();(root/'normal-before.json').write_text(json.dumps(before,indent=2)+'\n')
env=os.environ.copy();env.update(XDG_CONFIG_HOME=str(cfg),XDG_CACHE_HOME=str(root/'cache'),XDG_DATA_HOME=str(root/'data'),
 XDG_RUNTIME_DIR=os.environ.get('XDG_RUNTIME_DIR',str(root/'runtime')),LD_LIBRARY_PATH=str(prefix/'lib64'),QT_QPA_PLATFORM='wayland',
 OTAKUDOBS_CONGESTION_RECEIVER='ffmpeg' if args.ffmpeg_receiver else 'mediamtx',OTAKUDOBS_CONGESTION_ROOT=str(root),OTAKUDOBS_CONGESTION_SERVER=str(args.server.resolve()),
 OTAKUDOBS_CONGESTION_PYTHON=shutil.which(args.python) or args.python,OTAKUDOBS_CONGESTION_PROXY=str(repo/'test/studio-rtmp-congestion/proxy.py'))
tracked=[prefix/'bin/obs',prefix/'lib64/libobs.so.30',prefix/'lib64/libobs-frontend-api.so.30',args.module,args.server]
manifest={'receiver':'ffmpeg-listen-copy-single-file' if args.ffmpeg_receiver else 'mediamtx-fmp4-recorder','command':[str(prefix/'bin/obs'),'--multi','--disable-shutdown-check','--disable-updater'],'hashes':{str(f):sha(f) for f in tracked},'config':str(cfg),'source_hashes':{str(f.relative_to(repo)):sha(f) for f in [repo/'test/studio-rtmp-congestion/media-check.cpp',repo/'test/studio-rtmp-congestion/proxy.py',repo/'test/studio-rtmp-congestion/analyze-media.py',repo/'test/studio-rtmp-congestion/run-media.py',repo/'frontend/utility/MultiStreamManager.cpp']},'scope':'Private config, fixed primary DBR=false; additional DBR requested=true protected=false. Not public API, physical,60fps,UI parity or native Windows.'}
started=time.monotonic();timedout=False
with (root/'stdout.log').open('w') as out,(root/'stderr.log').open('w') as err:
    process=subprocess.Popen(manifest['command'],env=env,stdout=out,stderr=err)
    manifest['pid']=process.pid;(root/'launcher.json').write_text(json.dumps(manifest,indent=2)+'\n')
    maps_seen=False
    while process.poll() is None and time.monotonic()-started<110:
        maps=pathlib.Path(f'/proc/{process.pid}/maps')
        if not maps_seen and maps.exists():
            content=maps.read_text()
            if args.module.name in content:
                (root/'process-maps.txt').write_text(content);maps_seen=True
        time.sleep(.25)
    if process.poll() is None:
        timedout=True;process.terminate()
        try:process.wait(8)
        except subprocess.TimeoutExpired:process.kill();process.wait()
archive=root/'modules-archive';archive.mkdir()
for module in (plugin/'bin/64bit').glob('*'):shutil.move(str(module),archive/module.name)
after=normal_hashes();(root/'normal-after.json').write_text(json.dumps(after,indent=2)+'\n')
result={'exit':process.returncode,'timedout':timedout,'elapsed_seconds':time.monotonic()-started,'normal_configs_unchanged':before==after,'normal_file_count':len(before),'binaries_unchanged':all(sha(f)==manifest['hashes'][str(f)] for f in tracked),'module_archived':not any((plugin/'bin/64bit').iterdir()),'runtime_exists':(root/'runtime.json').exists(),'maps_recorded':maps_seen}
(root/'launcher-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
raise SystemExit(0 if result['exit']==0 and not timedout and result['normal_configs_unchanged'] and result['runtime_exists'] else 1)
