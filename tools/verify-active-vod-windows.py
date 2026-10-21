#!/usr/bin/env python3
"""Validate the Windows-target active archive/Undo fixture's actual snapshots.

Execution-environment provenance is supplied by the parent separately.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def receiver_listener(log, expected_bind):
    """Accept only the known RTMP startup grammars and one exact loopback bind."""
    endpoint=re.fullmatch(r'127\.0\.0\.1:([1-9][0-9]{0,4})',expected_bind)
    assert endpoint and int(endpoint[1])<=65535,('invalid explicit loopback RTMP bind',expected_bind)
    assert '[MoQ]' not in log,'MoQ activity is forbidden in this local-only fixture'
    starts=[line for line in log.splitlines()
            if re.search(r'\blisteners?\b',line) and re.search(r'\b(?:started|opened|listening)\b',line)]
    assert len(starts)==1,('expected exactly one receiver listener startup',starts)
    prefix=r'[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} INF \[RTMP\] '
    bind=re.escape(expected_bind)
    legacy=re.fullmatch(prefix+r'listener opened on '+bind,starts[0])
    current=re.fullmatch(prefix+r'started with listener on '+bind+r' \(TCP/RTMP\)',starts[0])
    assert legacy or current,('unexpected receiver protocol/address/startup grammar',starts[0])
    return {'bind':expected_bind,'transport':'TCP/RTMP',
            'grammar':'started with listener on (v1.21.2)' if current else 'listener opened on',
            'listener_start_count':len(starts)}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    path=a.root/'runtime.json';runtime=json.loads(path.read_text())
    assert runtime['passed'],runtime['error'];assert runtime['windows_target']
    assert runtime['runtime_environment_verified'] is False
    assert runtime['checks'] and all(x['passed'] for x in runtime['checks'])
    f=runtime['facts'];v=f['qa_video'];assert (v['base_width'],v['base_height'],v['output_width'],v['output_height'])==(640,360,640,360)
    assert v['fps_num']==30*v['fps_den'] and f['audio_sample_rate']==48000
    assert f['active_stream_output_type']=='rtmp_output'
    slots=f['active_encoder_slots'];assert [(x['slot'],x['mixer']) for x in slots]==[(0,0),(1,1)],slots
    assert all(x['encoder_id'] and x['encoder_name'] for x in slots)
    for key in ['active_initial','active_with_pending_pref']:
        assert f[key]=={'active':True,'effective':True,'live_mask':1,'vod_mask':2},(key,f[key])
    prepared=f['memory_only_prepared'];assert prepared['active'] is False and prepared['effective'] is False and prepared['live_mask']==1
    assert prepared['vod_mask'] in (8,16) and prepared['vod_mask']!=f['stored_vod_mask']
    assert f['pending_vod_index_one_based']==4 and f['active_stream_bytes']>=4000
    assert f['receiver']['rtmp_bind'].startswith('127.0.0.1:')
    log=(a.root/'receiver.log').read_text(errors='replace')
    listener=receiver_listener(log,f['receiver']['rtmp_bind'])
    result={'passed':True,'windows_target_artifacts':True,'native_vm_verified':False,
            'external_provenance_required':'Parent VM/session plus Windows binary/execution evidence',
            'native_checks':len(runtime['checks']),'facts':f,'receiver_listener':listener,
            'analyzer_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'runtime_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'limits':['Synthetic silence only; no physical endpoint/input capture',
                      'Actual local RTMP program/archive encoder slots and effective UI masks; no published Twitch VOD content',
                      'No SRT runtime, long soak or restart persistence certification']}
    with a.report.open('x') as out:json.dump(result,out,indent=2);out.write('\n')
    print(json.dumps({'passed':True,'native_checks':len(runtime['checks']),'active_stream_bytes':f['active_stream_bytes']}))


if __name__=='__main__':main()
