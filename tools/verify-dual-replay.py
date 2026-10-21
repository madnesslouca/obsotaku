#!/usr/bin/env python3
"""Decode concurrent local-only streams, recordings and two replay buffers.

This examines actual encoder settings and identity from the native probe as well
as final media. A valid file alone does not certify inheritance or independence.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import platform

HELPER = Path(__file__).with_name('verify-simultaneous-outputs.py')
spec = importlib.util.spec_from_file_location('simultaneous_media', HELPER)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def verify_plans(runtime):
    checks = []
    for phase in range(4):
        plans = [p for p in runtime['plans'] if p['phase'] == phase and p['kind'] != 'manager-snapshot']
        # More than one observation is intentional around failure/recovery.
        for canvas in ('main', 'portrait'):
            stream = next(p for p in plans if p['kind'] == 'stream' and p['canvas'] == canvas)
            record = next(p for p in plans if p['kind'] == 'recording' and p['canvas'] == canvas)
            clip = next(p for p in plans if p['kind'] == 'clip' and p['canvas'] == canvas)
            target = (2200 if canvas == 'portrait' else 4500) if phase in (1, 2) else (2800 if canvas == 'portrait' else 6200)
            assert clip['settings']['rate_control'] == 'CBR', clip
            assert clip['settings']['bitrate'] == target, clip
            assert clip['encoder_name'] != record['encoder_name'] != stream['encoder_name'], (clip, record, stream)
            assert clip['encoder_name'] != stream['encoder_name'], (clip, stream)
            assert clip['audio'] and clip['audio'][0]['mixer'] == ((2 if canvas == 'portrait' else 1) if phase in (1, 2) else 0), clip
            assert clip['audio'][0]['name'] != record['audio'][0]['name'], (clip, record)
            expected = (1080, 1920) if canvas == 'portrait' else (1920, 1080)
            assert (clip['width'], clip['height']) == expected, clip
            if phase not in (1, 2):
                assert clip['settings']['bitrate'] == stream['settings']['bitrate'], (clip, stream)
                assert clip['encoder_id'] == stream['encoder_id'], (clip, stream)
                assert clip['scale_type'] == stream['scale_type'], (clip, stream)
                assert clip['fps_divisor'] == stream['fps_divisor'], (clip, stream)
                assert clip['effective_format'] == stream['effective_format'], (clip, stream)
                assert clip['audio'][0]['settings']['bitrate'] == stream['audio'][0]['settings']['bitrate'], (clip, stream)
            if phase == 2 and canvas == 'main':
                assert record['settings']['rate_control'] == 'CBR' and record['settings']['bitrate'] == 9000, record
            if phase == 3 and canvas == 'main':
                assert record['settings']['rate_control'] in ('CQP', 'CRF'), record
                assert (record['width'],record['height']) == (3840,2160), record
                assert (stream['width'],stream['height']) == (1920,1080), stream
                for item in (stream,record,clip):
                    if item['encoder_id'].startswith('obs_nvenc'):
                        assert item['settings']['preset']=='p1' and item['settings']['multipass']=='disabled' and item['settings']['lookahead'] is False, item
                    else:
                        assert item['settings']['preset']=='ultrafast', item
            checks.append({'phase': phase, 'canvas': canvas, 'clip_target_kbps': target,
                           'record_rate_control': record['settings'].get('rate_control'),
                           'record_bitrate_kbps': record['settings'].get('bitrate'),
                           'video_encoders_exclusive': True, 'audio_encoders_exclusive': True})
    return checks


def unique_frames(path):
    data=helper.execute('ffmpeg','-nostdin','-v','error','-ss','0.3','-i',str(path),'-t','1.5',
                        '-map','0:v:0','-an','-vf','scale=64:64','-fps_mode','passthrough','-f','framemd5','-').stdout.decode()
    hashes=[line.rsplit(',',1)[-1].strip() for line in data.splitlines() if line and not line.startswith('#')]
    assert len(hashes)>=75,(path,'too few decoded frames',len(hashes))
    ratio=len(set(hashes))/len(hashes)
    assert ratio>=.90,(path,'excess duplicate decoded frames',ratio)
    return {'sample_frames':len(hashes),'unique_frames':len(set(hashes)),'unique_ratio':round(ratio,4)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    runtime = json.loads((args.root / 'runtime.json').read_text())
    assert runtime['passed'], runtime['error']
    assert not runtime['payload_violation']
    assert runtime['completed_save_groups']==[2,2,2,2],runtime['completed_save_groups']
    assert runtime['normal_stop_preserved_pending_save']
    assert all(c['passed'] for c in runtime['checks'])
    effective = verify_plans(runtime)
    local = [helper.inspect_local(entry) for entry in runtime['files']]
    received = [helper.inspect_received(path, args.root)
                for path in sorted(args.root.glob('phase-*/[ab]/**/*.mp4')) if path.stat().st_size]
    for entry in local:
        entry['decoded_frame_sample']=unique_frames(entry['path'])
    for entry in received:
        entry['decoded_frame_sample']=unique_frames(entry['file'])
    assert len(local) == 24, len(local)
    assert len({f['path'] for f in local}) == 24, 'a later save reused a successful file path'
    assert len(received) == 9, len(received)
    for phase in range(4):
        current = [f for f in local if f['phase'] == phase]
        assert len([f for f in current if f['kind'] != 'clip']) == 2, (phase, current)
        for canvas in ('main', 'portrait'):
            assert len([f for f in current if f['kind'] == 'clip' and f['canvas'] == canvas]) == 2, (phase, canvas, current)
        streams = [f for f in received if f['phase'] == phase]
        assert len([f for f in streams if f['destination'] == 'a']) == 1
        assert len([f for f in streams if f['destination'] == 'b']) == (2 if phase == 0 else 1)
    failures = [s for s in runtime['save_results'] if not s['success']]
    assert len(failures) == 1 and failures[0]['phase'] == 2 and failures[0]['canvas'] == 'portrait', failures
    report = {'passed': True, 'runtime': runtime, 'effective_plans': effective,
              'local_media': local, 'received_media': received,
              'simultaneous_outputs': ['Main RTMP', 'Portrait RTMP', 'Main recording', 'Portrait recording',
                                       'Main replay', 'Portrait replay'],
              'modes': ['Simple', 'Advanced'],
              'native_linux_validated': platform.system() == 'Linux',
              'native_windows_validated': platform.system() == 'Windows',
              'limits': ['Loopback destinations and synthetic moving video/tone only',
                         'No public ingest/OAuth, physical capture or prolonged soak',
                         '60fps packet delivery plus decoded sample frame uniqueness checked; no exact synchronized dual-save boundary claim',
                         'Payload limit excludes allocator/encoder/mux subprocess/metadata memory',
                         'Native Windows certification requires running this probe and validator on Windows']}
    with args.report.open('x') as out:
        json.dump(report, out, ensure_ascii=False, indent=2)
        out.write('\n')
    print(json.dumps({'passed': True, 'native_checks': len(runtime['checks']),
                      'effective_plans': len(effective), 'local_files': len(local),
                      'received_files': len(received), 'decoded_audio_in_all_files': True}))


if __name__ == '__main__':
    main()
