#!/usr/bin/env python3
"""Seed a fresh private profile-defaults fixture. Never starts or stops OBS."""
import argparse,hashlib,json,pathlib,os
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=pathlib.Path);a=p.parse_args();root=a.root.resolve()
if root.exists():raise SystemExit('Refusing an existing fixture root; use a fresh private directory')
root.mkdir(parents=True,mode=0o700);config=root/'config';obs=config/'obs-studio';profile=obs/'basic/profiles/Profile_defaults_QA';profile.mkdir(parents=True);(obs/'basic/scenes').mkdir();ws=obs/'plugin_config/obs-websocket';ws.mkdir(parents=True)
(obs/'global.ini').write_text('[General]\nLanguage=en-US\nEnableAutoUpdates=false\nLastVersion=537001986\n[Locations]\n'+''.join(f'{key}={config}\n' for key in ['Configuration','SceneCollections','Profiles','PluginManagerSettings']))
(obs/'user.ini').write_text('[General]\nFirstRun=true\nLanguage=en-US\n[BasicWindow]\nStudioWorkspace=true\nStudioMultiCanvas=false\nWarnBeforeStartingStream=false\nWarnBeforeStoppingStream=false\nWarnBeforeStoppingRecord=false\n[Basic]\nProfile=Profile defaults QA\nProfileDir=Profile_defaults_QA\nSceneCollection=Profile defaults QA\nSceneCollectionFile=Profile_defaults_QA\nConfigOnNewProfile=false\n')
(profile/'basic.ini').write_text('[General]\nName=Profile defaults QA\n[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSType=0\nFPSCommon=30\n[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n')
(ws/'config.json').write_text(json.dumps({'server_enabled':False,'first_load':False,'alerts_enabled':False,'auth_required':True,'server_password':'','server_port':4455},indent=2)+'\n')
files={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in root.rglob('*') if f.is_file()}
manifest={'prepared':True,'root':str(root),'config_root':str(config),'environment':{'XDG_CONFIG_HOME':str(config)},'studio_workspace':True,'studio_multi_canvas':False,'video':{'width':640,'height':360,'fps':30},'sample_rate':48000,'websocket_server_enabled':False,'output_keys_seeded':False,'profile':str(profile),'files':files,'scope':'Fresh private seed, no copied user config/secrets/plugins/scenes. FirstRun=true prevents first-run audio device sources. No OBS launched, stopped, installed or changed.'}
(root/'prepare.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
