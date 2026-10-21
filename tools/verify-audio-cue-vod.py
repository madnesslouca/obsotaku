#!/usr/bin/env python3
"""Measure private CUE monitor recordings and actual locally encoded mixes.

Local live/VOD labels certify mixer isolation only, never public ingest.
Linux Pulse's source mute/offset behavior differs from an acoustic measurement.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np

FREQUENCIES = (431, 997, 2003)


def command(*args):
    return subprocess.run(args, check=True, capture_output=True)


def decoded(path, track=0, start=None, duration=None):
    args=['ffmpeg','-nostdin','-v','error']
    if start is not None: args += ['-ss',str(start)]
    args += ['-i',str(path),'-map',f'0:a:{track}']
    if duration is not None: args += ['-t',str(duration)]
    args += ['-ac','1','-ar','48000','-f','f32le','-']
    return np.frombuffer(command(*args).stdout,dtype='<f4')


def amplitudes(samples):
    assert len(samples)>48000,('too few samples',len(samples))
    assert np.isfinite(samples).all(),'non-finite audio samples'
    x=samples.astype(float); window=np.hanning(len(x)); axis=np.arange(len(x))/48000
    values={str(hz):float(2*abs(np.sum(x*window*np.exp(-2j*np.pi*hz*axis)))/window.sum()) for hz in FREQUENCIES}
    return {'samples':len(x),'rms':float(np.sqrt(np.mean(x*x))),'peak':float(np.max(np.abs(x))),
            'tone_amplitudes':values}


def presence(metrics,hz,expected,where):
    amplitude=metrics['tone_amplitudes'][str(hz)]
    if expected: assert amplitude>=.001,(where,hz,'expected tone absent',amplitude)
    else: assert amplitude<=.00015,(where,hz,'unwanted tone leaked',amplitude)


def envelope(samples,hz):
    x=np.asarray(samples[:len(samples)//480*480],dtype=float).reshape(-1,480)
    carrier=np.exp(-2j*np.pi*hz*np.arange(480)/48000)
    return 2*np.abs(x@carrier)/480


def pulse_lag(samples):
    a=envelope(samples,997);b=envelope(samples,2003);candidates=[]
    # The shared pulse period is 500ms. Restrict to [-100,+390]ms to avoid
    # treating the physically distinct +250ms delay as its -250ms alias.
    for lag in range(-10,40):
        x,y=(a[lag:],b[:-lag]) if lag>0 else ((a[:lag],b[-lag:]) if lag<0 else (a,b))
        if len(x)>80: candidates.append((float(np.corrcoef(x,y)[0,1]),lag))
    score,lag=max(candidates)
    assert score>.75,('pulse correlation too low',score,lag)
    return {'lag_ms':lag*10,'correlation':score}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path);parser.add_argument('--report',required=True,type=Path)
    args=parser.parse_args();runtime=json.loads((args.root/'runtime.json').read_text())
    assert runtime['passed'],runtime['error'];assert all(x['passed'] for x in runtime['checks'])
    path=args.root/'paths.mkv';probe=json.loads(command('ffprobe','-v','error','-show_streams','-show_format','-of','json',str(path)).stdout)
    audio=[s for s in probe['streams'] if s['codec_type']=='audio']
    assert len(audio)==4 and all(s['codec_name']=='aac' and int(s['sample_rate'])==48000 for s in audio),audio
    full=command('ffmpeg','-nostdin','-v','error','-i',str(path),'-map','0','-f','null','-')
    assert not full.stderr.strip(),full.stderr.decode()
    results=[];delays={};encoded_delays={};raws={}
    for stage in runtime['stages']:
        name=stage['name'];entry={'name':name,'mixed_pcm':[],'encoded_aac':[]}
        for mix in range(4):
            raw=np.fromfile(args.root/f'{name}-mix{mix}.f32',dtype='<f4');raws[(name,mix)]=raw
            # Trim boundaries so a device/source start cannot masquerade as
            # steady-state exclusion. Every capture still needs >=2s total.
            assert len(raw)>=96000,(name,mix,len(raw))
            metrics=amplitudes(raw[9600:-9600]);entry['mixed_pcm'].append(metrics)
            if mix<3:
                presence(metrics,997,stage['expected_music'][mix],(name,'PCM',mix))
                presence(metrics,431,stage['expected_voice'],(name,'PCM',mix))
                presence(metrics,2003,stage['expected_voice'] and mix==2,(name,'PCM',mix))
            elif not stage['physical_input']:
                assert metrics['rms']<1e-6,(name,'physical input leaked before its capture',metrics)
            start=stage['file_start_seconds']+.5;duration=min(2.,stage['file_end_seconds']-start-.3)
            assert duration>=1.4,(name,'local file stage window too short',duration)
            encoded_raw=decoded(path,mix,start,duration)
            encoded=amplitudes(encoded_raw);entry['encoded_aac'].append(encoded)
            if mix==2 and name in ('delay_zero','delay_250'):encoded_delays[name]=pulse_lag(encoded_raw)
            if mix<3:
                presence(encoded,997,stage['expected_music'][mix],(name,'AAC',mix))
                presence(encoded,431,stage['expected_voice'],(name,'AAC',mix))
                presence(encoded,2003,stage['expected_voice'] and mix==2,(name,'AAC',mix))
            elif stage['physical_input']:
                assert metrics['rms']>1e-7 and encoded['rms']>1e-7,(name,'physical input has no measurable ambient samples',metrics,encoded)
                assert metrics['peak']<.999,(name,'physical input clips',metrics)
        monitor=amplitudes(decoded(args.root/f'{name}-monitor.wav'))
        entry['sink_monitor']=monitor
        presence(monitor,997,stage['expected_monitor_music'],(name,'sink monitor'))
        presence(monitor,431,False,(name,'unmonitored voice leaked to sink'))
        presence(monitor,2003,False,(name,'unmonitored reference leaked to sink'))
        if stage['physical_endpoint']:
            assert monitor['peak']<=.0317,(name,'physical test tone exceeds -30dBFS',monitor['peak'])
        if name in ('delay_zero','delay_250'):
            delays[name]=pulse_lag(raws[(name,2)])
        results.append(entry)
    assert len(results)==14,len(results)
    measured=delays['delay_250']['lag_ms']-delays['delay_zero']['lag_ms']
    assert abs(measured-250)<=40,('program source delay not applied',measured,delays)
    encoded_measured=encoded_delays['delay_250']['lag_ms']-encoded_delays['delay_zero']['lag_ms']
    assert abs(encoded_measured-250)<=40,('encoded program source delay not applied',encoded_measured,encoded_delays)
    report={'passed':True,'native_checks':len(runtime['checks']),'stages':results,'program_pulse_delay':delays,
            'measured_program_delay_change_ms':measured,'encoded_program_pulse_delay':encoded_delays,
            'measured_encoded_delay_change_ms':encoded_measured,'four_audio_tracks_fully_decoded':True,
            'file_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'native_linux':True,'native_windows':False,
            'limits':['Local AAC tracks and actual mixer PCM labeled live/VOD/record; no public Twitch ingest',
                      'Pulse sink monitor confirms samples rendered to the selected virtual/physical endpoint; no acoustic audition or latency claim',
                      'Pulse monitor mute callback and source sync offset behavior differ from the program path',
                      'Physical microphone ambient audio is private and never streamed or monitored',
                      'No long duration USB/driver stress, Windows WASAPI execution remains pending']}
    with args.report.open('x') as out:json.dump(report,out,indent=2);out.write('\n')
    print(json.dumps({'passed':True,'native_checks':len(runtime['checks']),'stages':len(results),'program_delay_change_ms':measured,'tracks':4}))


if __name__=='__main__':main()
