#!/usr/bin/env python3
"""Analyze the separate Windows-native CUE/VOD fixture, including VM skips.

Can analyze copied guest artifacts on Linux. Runtime platform comes from the
native module report, not from the machine doing FFT/decoding.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import subprocess
import numpy as np

spec=importlib.util.spec_from_file_location('cue_linux_math',Path(__file__).with_name('verify-audio-cue-vod.py'))
math=importlib.util.module_from_spec(spec);spec.loader.exec_module(math)


def measured(samples):
    metrics=math.amplitudes(samples)
    x=np.asarray(samples,dtype=float);w=np.hanning(len(x));axis=np.arange(len(x))/48000
    metrics['tone_amplitudes']['3037']=float(2*abs(np.sum(x*w*np.exp(-2j*np.pi*3037*axis)))/w.sum())
    return metrics


def endpoint_liveness(stage,metrics):
    stat=stage['endpoint_capture']
    assert stat['written_frames']>=stat['sample_rate']>0 and stat['received_frames']>=stat['sample_rate'],(stage['name'],'endpoint evidence was padded/empty')
    assert stat['device_id']==stage['endpoint_id'],(stage['name'],'capture ID differs from explicit monitored endpoint')
    math.presence(metrics,3037,True,(stage['name'],'permanent endpoint pilot missing'))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    runtime=json.loads((a.root/'runtime.json').read_text())
    assert runtime['passed'],runtime['error'];assert runtime['windows_target'] and not runtime['linux_native'],runtime
    assert all(x['passed'] for x in runtime['checks'])
    endpoint_class=runtime['facts']['endpoint_class'];assert endpoint_class in ('virtual','physical')
    path=a.root/'paths.mkv';probe=json.loads(math.command('ffprobe','-v','error','-show_streams','-show_format','-of','json',str(path)).stdout)
    audio=[x for x in probe['streams'] if x['codec_type']=='audio'];assert len(audio)==4 and all(x['codec_name']=='aac' and int(x['sample_rate'])==48000 for x in audio)
    decoded=math.command('ffmpeg','-nostdin','-v','error','-i',str(path),'-map','0','-f','null','-');assert not decoded.stderr.strip(),decoded.stderr.decode()
    stages=[];pcm_delays={};aac_delays={};cue_delays={}
    for s in runtime['stages']:
        name=s['name'];entry={'name':name,'mixed_pcm':[],'encoded_aac':[]};raws={}
        for mix in range(4):
            raw=np.fromfile(a.root/f'{name}-mix{mix}.f32',dtype='<f4');assert len(raw)>=96000,(name,mix,len(raw));raws[mix]=raw
            pcm=measured(raw[9600:-9600]);entry['mixed_pcm'].append(pcm)
            start=s['file_start_seconds']+.5;duration=min(2.,s['file_end_seconds']-start-.3);assert duration>=1.4,(name,duration)
            encraw=math.decoded(path,mix,start,duration);aac=measured(encraw);entry['encoded_aac'].append(aac)
            for metrics,route in ((pcm,'PCM'),(aac,'AAC')):
                math.presence(metrics,3037,False,(name,route,mix,'pilot must never reach program mixes'))
                if mix<3:
                    math.presence(metrics,997,s['expected_music'][mix],(name,route,mix))
                    math.presence(metrics,431,s['expected_voice'],(name,route,mix))
                    math.presence(metrics,2003,s['expected_voice'] and mix==2,(name,route,mix))
                elif s['physical_input']:
                    assert metrics['rms']>1e-7 and metrics['peak']<.999,(name,route,'explicit input silent or clipped',metrics)
                else:assert metrics['rms']<1e-6,(name,route,'unconfigured microphone mix leaked')
            if mix==2 and name in ('delay_zero','delay_250'):
                pcm_delays[name]=math.pulse_lag(raw);aac_delays[name]=math.pulse_lag(encraw)
        monitor_samples=math.decoded(a.root/f'{name}-monitor.wav');monitor=measured(monitor_samples);entry['endpoint_monitor']=monitor
        math.presence(monitor,997,s['expected_monitor_music'],(name,'WASAPI endpoint'))
        math.presence(monitor,431,False,(name,'unmonitored voice'))
        math.presence(monitor,2003,s['expected_monitor_reference'],(name,'monitor reference'))
        endpoint_liveness(s,monitor)
        assert s['endpoint_class']==endpoint_class
        if s['physical_endpoint']:assert endpoint_class=='physical' and monitor['peak']<=.0317,(name,'physical endpoint amplitude/class')
        if name in ('cue_delay_zero','cue_delay_250'):cue_delays[name]=math.pulse_lag(monitor_samples)
        stages.append(entry)
    names={x['name'] for x in runtime['stages']};required={'baseline','cue_both','muted_both','unmuted_both','monitor_only','muted_only','unmuted_only','protected_only','delay_zero','delay_250','vod_excluded','vod_undo','cue_delay_zero','cue_delay_250','endpoint_monitor'}
    assert required<=names and len(names)==len(stages),(names,required)
    if 'physical_input' not in names:assert any(x['case']=='physical_input' for x in runtime['skipped'])
    if endpoint_class=='virtual':assert any(x['case']=='physical_endpoint_hardware' for x in runtime['skipped'])
    pcm_delta=pcm_delays['delay_250']['lag_ms']-pcm_delays['delay_zero']['lag_ms'];aac_delta=aac_delays['delay_250']['lag_ms']-aac_delays['delay_zero']['lag_ms']
    assert abs(pcm_delta-250)<=40,(pcm_delta,pcm_delays);assert abs(aac_delta-250)<=40,(aac_delta,aac_delays)
    cue_delta=cue_delays['cue_delay_250']['lag_ms']-cue_delays['cue_delay_zero']['lag_ms']
    # WASAPI process_audio_delay releases within a 75ms video sync window;
    # 30fps render cadence and endpoint queue add quantization. This verifies
    # a substantive delayed endpoint response, not exact/acoustic latency.
    assert 100<=cue_delta<=350,('video-synchronized WASAPI CUE delay missing/out of declared bound',cue_delta,cue_delays)
    result={'passed':True,'windows_target_artifacts':True,'native_windows_vm_verified':False,'native_environment_provenance':'Parent external VM/session/binary evidence required; compile target is not execution-environment proof','native_checks':len(runtime['checks']),'analysis_host':platform.system(),
            'endpoint_class':endpoint_class,'physical_endpoint_validated':endpoint_class=='physical','explicit_input_samples_validated':'physical_input' in names,'physical_microphone_certified':False,
            'skipped':runtime['skipped'],'stages':stages,'program_pcm_delay':pcm_delays,'program_aac_delay':aac_delays,'cue_av_delay':cue_delays,
            'delay_changes_ms':{'program_pcm':pcm_delta,'program_aac':aac_delta,'cue_av':cue_delta},
            'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'limits':['Permanent3037Hz monitor-only pilot must be present in every endpoint WAV, including negative997Hz cases; no PASS from padding',
                      'Explicit Windows render endpoint loopback; virtual HDA never certifies physical speakers/headsets',
                      'Optional microphone missing is SKIP, not PASS; all samples private and never streamed',
                      'Source uses async video frames to enter WASAPI video-synchronized monitoring branch; audio-only monitoring is a separate path',
                      'CUE delay bound100..350ms for requested250ms reflects native75ms release window/30fps cadence; no acoustic latency claim',
                      'Four local encoded mixes labeled live/VOD/record/input; no public Twitch archive ingest',
                      'Windows driver/hotplug, real USB endpoints and long soak remain separate tests']}
    with a.report.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({'passed':True,'native_checks':len(runtime['checks']),'stages':len(stages),'endpoint_class':endpoint_class,'skipped':runtime['skipped'],'delay_changes_ms':result['delay_changes_ms']}))


if __name__=='__main__':main()
