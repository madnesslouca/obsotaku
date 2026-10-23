#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Frozen criteria for the selective-congestion synthetic15fps local fixture.
No late-window substitution: original startup0.3..1.8s is always inspected.
"""
import hashlib,json,pathlib,subprocess,sys
import numpy as np
root=pathlib.Path(sys.argv[1]);out=root/'acceptance.json'
if out.exists():raise SystemExit('Refusing to overwrite acceptance evidence')
r=json.loads((root/'runtime.json').read_text());checks=[];media=[]
def check(value,label):checks.append({'name':label,'passed':bool(value)})
def cmd(args):
    p=subprocess.run(args,capture_output=True,timeout=90)
    if p.returncode:raise RuntimeError(p.stderr.decode(errors='replace')[-4000:])
    return p.stdout
samples=r['samples'];phases={v['stage']:v['at_ms'] for v in r['phases']}
check(r['passed'],'native fixture passed');check(not r['generator']['failed'],'source-clock backlog precondition')
check(r['primary_dbr'] is False,'primary DBR is explicitly disabled in fixed-policy scenario')
limited=[s for s in samples if s['stage']==3];healthy=[s for s in samples if s['stage']==2];recovered=[s for s in samples if s['stage']==4]
check(bool(limited) and bool(healthy) and bool(recovered),'three measured phases present')
if limited and healthy and recovered:
    relevant=healthy+limited+recovered
    ids={s[k]['video_encoder'] for s in relevant for k in ('a','b','record','replay')}
    check(len(ids)==1 and '' not in ids,'single video encoder identity across consumers/phases')
    check(all(s['configured_bitrate']==2500 for s in relevant),'configured2500kbps preserved')
    check(all(s.get('b_dbr_requested') and not s.get('b_dbr_effective') and s.get('b_dbr_reason')==1 for s in relevant),'B requested DBR preserved and effective protected')
    check(all(s['primary_stops']==0 and s['a']['active'] and not s['a']['reconnecting'] for s in relevant),'A session stays active without reconnect')
    check(all(s['record']['active'] and s['replay']['active'] for s in relevant),'record/replay continue during B congestion')
    # Observe every consecutive >=1s interval; do not infer progress from Live.
    checkpoints=[relevant[0]]
    for s in relevant[1:]:
        if s['at_ms']-checkpoints[-1]['at_ms']>=1000:checkpoints.append(s)
    check(all(b['a']['bytes']>a['a']['bytes'] and b['record']['frames']>a['record']['frames'] for a,b in zip(checkpoints,checkpoints[1:])),'A bytes and recorded frames advance in every measured1s interval')
    peak_drop=max(s['b']['drops'] for s in limited)-healthy[-1]['b']['drops'];peak_congestion=max(s['b']['congestion'] for s in limited)
    check(peak_drop>0 and peak_congestion>=.25,'real B sender drops plus congestion prove backpressure')
    check(recovered[-1]['b']['bytes']-recovered[0]['b']['bytes']>250000,'B makes delivery progress after unlimited recovery')
    check(recovered[-1]['b']['congestion']<.25,'B sender congestion clears before recovery deadline')
    check(recovered[-1]['b']['drops']-recovered[-4]['b']['drops']==0,'B drops stop at end of recovery')
    rate_samples=[]
    for e in r['proxy']:
        if e.get('event')=='snapshot' and phases[3]+500<=e['observed_ms']<=phases[4]-500:
            rate_samples.append(e)
    check(len(rate_samples)>=8,'proxy throughput has measured limited-phase snapshots')
    if len(rate_samples)>=2:
        a,b=rate_samples[0],rate_samples[-1]
        def total(e):return sum(s['publisher_forwarded'] for s in e['sessions'])
        actual=(total(b)-total(a))/((b['seconds']-a['seconds']))
        check(3500<=actual<=6000,'actual proxy publisher throughput in3500..6000bytes/s for requested5000')
    else:actual=None
    steady=recovered[-1]['record']['bytes']-healthy[0]['record']['bytes'];seconds=(recovered[-1]['at_ms']-healthy[0]['at_ms'])/1000
    effective_kbps=steady*8/seconds/1000
    check(1800<=effective_kbps<=3400,'recorded effective aggregate bitrate demonstrates sustained source pressure')
else:peak_drop=peak_congestion=actual=effective_kbps=None
files=sorted((root/'a').rglob('*.mp4'))+sorted((root/'b').rglob('*.mp4'))+[root/'recording.mkv']+[pathlib.Path(x['path']) for x in r['clips'] if x['success']]
check(len(list((root/'a').rglob('*.mp4')))==1,'healthy A keeps one receiver recording session')
check(len(files)>=5,'A B recording and two replay files exist')
for path in files:
    item={'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    try:
        probe=json.loads(cmd(['ffprobe','-v','error','-show_streams','-show_packets','-of','json',str(path)]))
        video=next(s for s in probe['streams'] if s['codec_type']=='video');audio=next(s for s in probe['streams'] if s['codec_type']=='audio')
        check(video['codec_name']=='h264' and audio['codec_name']=='aac',str(path.relative_to(root))+':H264/AAC')
        cmd(['ffmpeg','-v','error','-xerror','-i',str(path),'-f','null','-'])
        check(True,str(path.relative_to(root))+':full decode')
        packet_summary={}
        for s in (video,audio):
            packets=[p for p in probe['packets'] if p['stream_index']==s['index']]
            dts=[float(p['dts_time']) for p in packets if 'dts_time'in p];pts=[float(p['pts_time']) for p in packets if 'pts_time'in p]
            check(len(dts)>0 and all(b>a for a,b in zip(dts,dts[1:])),str(path.relative_to(root))+':'+s['codec_type']+' DTS strictly monotonic')
            check(len(pts)>0 and all(b>=a for a,b in zip(pts,pts[1:])),str(path.relative_to(root))+':'+s['codec_type']+' PTS monotonic')
            packet_summary[s['codec_type']]={'first_pts':pts[0],'last_pts':pts[-1],'packets':len(packets)}
            if s['codec_type']=='video':check('K' in packets[0].get('flags',''),str(path.relative_to(root))+':first video packet keyframe')
        av=abs(packet_summary['video']['first_pts']-packet_summary['audio']['first_pts'])
        check(av<=.075,str(path.relative_to(root))+':initial A/V<=75ms')
        hashes=cmd(['ffmpeg','-v','error','-i',str(path),'-map','0:v:0','-vf','scale=64:64','-f','framemd5','-']).decode()
        frames=[];tb=1/15
        for line in hashes.splitlines():
            if line.startswith('#tb 0:'):n,d=line.split(':',1)[1].strip().split('/');tb=int(n)/int(d)
            if not line.startswith('#') and line.strip():
                fields=[x.strip() for x in line.split(',')];frames.append((int(fields[2])*tb,fields[-1]))
        origin=frames[0][0] if frames else 0
        early=[h for t,h in frames if .3<=t-origin<=1.8]
        ratio=len(set(early))/len(early) if early else 0
        # B may discard media under the intentional limitation. Its startup is
        # before limit and still has the same mandatory motion criterion.
        check(len(early)>=15 and ratio>=.85,str(path.relative_to(root))+':original startup0.3..1.8s motion>=85%')
        pcm=np.frombuffer(cmd(['ffmpeg','-v','error','-i',str(path),'-map','0:a:0','-ac','1','-ar','48000','-f','f32le','-']),dtype='<f4')
        check(len(pcm)>=48000,str(path.relative_to(root))+':at least1s audio')
        n=min(len(pcm),48000);segment=pcm[:n].astype(np.float64);spectrum=np.abs(np.fft.rfft(segment*np.hanning(n)));freq=float(np.argmax(spectrum)*48000/n)
        rms=float(np.sqrt(np.mean(pcm.astype(np.float64)**2)));check(abs(freq-997)<=2 and rms>.01,str(path.relative_to(root))+':997Hz audible-amplitude tone')
        item.update(packets=packet_summary,initial_av_seconds=av,video_frames=len(frames),startup_motion_ratio=ratio,tone_hz=freq,rms=rms,passed=True)
    except Exception as e:check(False,str(path.relative_to(root))+':analysis '+str(e));item.update(passed=False,error=str(e))
    media.append(item)
launcher=json.loads((root/'launcher-result.json').read_text());check(launcher['exit']==0 and not launcher['timedout'],'private OBS exits normally');check(launcher['normal_configs_unchanged'] and launcher['binaries_unchanged'] and launcher['module_archived'],'user config binaries preserved and probe archived')
receipt={'date_local':'2026-10-23','scope':'Linux native synthetic15fps fixed-primary-DBR=false selective RTMP congestion; no public/physical/Windows certification','checks':len(checks),'failures':sum(not x['passed'] for x in checks),'passed':all(x['passed'] for x in checks),'backpressure':{'B_new_drops':peak_drop,'B_peak_congestion':peak_congestion,'proxy_actual_bytes_per_second':actual,'effective_record_kbps':effective_kbps},'media':media,'assertions':checks}
out.write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({k:receipt[k] for k in ('checks','failures','passed','backpressure')}));raise SystemExit(0 if receipt['passed'] else 1)
