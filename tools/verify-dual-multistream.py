#!/usr/bin/env python3
"""Decode media actually received by the two loopback RTMP servers."""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess


def run(*args):
    return subprocess.run(args, capture_output=True, check=True, timeout=90)


def inspect(path, root):
    parts = path.relative_to(root).parts
    session = int(parts[0].removeprefix('session-'))
    primary = parts[1] == 'a'
    portrait = (session == 1) == primary
    target = 2800 if portrait else 6200
    expected = (1080, 1920) if portrait else (1920, 1080)
    data = json.loads(run('ffprobe', '-v', 'error', '-show_streams', '-show_packets',
                          '-show_entries', 'stream=index,codec_type,codec_name,width,height,avg_frame_rate,r_frame_rate:packet=stream_index,dts_time,pts_time,size',
                          '-of', 'json', str(path)).stdout)
    video = next(s for s in data['streams'] if s['codec_type'] == 'video')
    audio = next(s for s in data['streams'] if s['codec_type'] == 'audio')
    assert (video['width'], video['height']) == expected, (path, video)
    assert video['codec_name'] == 'h264' and audio['codec_name'] == 'aac'
    streams = {}
    for stream in (video, audio):
        packets = [p for p in data['packets'] if p['stream_index'] == stream['index'] and 'dts_time' in p]
        stamps = [float(p['dts_time']) for p in packets]
        gaps = [b-a for a, b in zip(stamps, stamps[1:])]
        duration = stamps[-1]-stamps[0]
        assert len(stamps) > 30 and duration > 1, (path, stream, duration)
        assert min(gaps) > -.001 and max(gaps) < .101, (path, stream, max(gaps))
        streams[stream['codec_type']] = {'duration_seconds': round(duration, 3), 'max_gap_seconds': round(max(gaps), 3),
                                       'packets': len(stamps), 'measured_kbps': round(sum(int(p['size']) for p in packets)*8/duration/1000)}
    actual_fps = (streams['video']['packets']-1)/streams['video']['duration_seconds']
    assert 58.5 < actual_fps < 61.5, (path, actual_fps)
    assert abs(streams['video']['duration_seconds']-streams['audio']['duration_seconds']) < .3, (path, streams)
    assert target*.65 < streams['video']['measured_kbps'] < target*1.4, (path, target, streams)
    decoded = run('ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-')
    assert not decoded.stderr.strip(), decoded.stderr.decode(errors='replace')[:400]
    pcm = array('f', run('ffmpeg', '-nostdin', '-v', 'error', '-ss', '0.3', '-i', str(path), '-t', '0.8', '-vn', '-ac', '1', '-ar', '48000', '-f', 'f32le', '-').stdout)
    assert len(pcm) > 10000
    rms = math.sqrt(sum(x*x for x in pcm)/len(pcm))
    real = sum(x*math.cos(2*math.pi*440*i/48000) for i, x in enumerate(pcm))/len(pcm)
    imag = sum(x*math.sin(2*math.pi*440*i/48000) for i, x in enumerate(pcm))/len(pcm)
    peak = 2*math.hypot(real, imag)
    assert rms > .025 and peak > .035, (path, 'audio tone missing', rms, peak)
    def frame(second, vf):
        return run('ffmpeg', '-nostdin', '-v', 'error', '-ss', str(second), '-i', str(path), '-vf', vf, '-frames:v', '1', '-f', 'rawvideo', '-').stdout
    assert frame(.3, 'scale=32:32,format=rgb24') != frame(1, 'scale=32:32,format=rgb24'), 'moving video missing'
    if portrait:
        r, g, b = frame(.3, 'crop=4:4:80:80,format=rgb24')[:3]
        assert g > 140 and g > r+50 and g > b+50, 'independent portrait layer missing'
    return {'file': str(path), 'session': session, 'destination': parts[1], 'canvas': 'portrait' if portrait else 'main',
            'resolution': f'{expected[0]}x{expected[1]}', 'target_bitrate_kbps': target, 'received_fps': round(actual_fps, 3),
            'fully_decoded': True, 'audio_rms': round(rms, 4), 'audio_440hz_peak': round(peak, 4), 'streams': streams,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('--report', required=True, type=Path)
    args = p.parse_args()
    runtime = json.loads((args.root/'runtime.json').read_text())
    assert runtime['passed'], runtime['error']
    media = [inspect(f, args.root) for f in sorted(args.root.glob('session-*/*/**/*.mp4')) if f.stat().st_size > 0]
    for session in (0, 1):
        for destination in ('a', 'b'):
            selected = [m for m in media if m['session'] == session and m['destination'] == destination]
            assert len(selected) == (2 if session == 0 and destination == 'b' else 1), selected
    a = next(m for m in media if m['session'] == 0 and m['destination'] == 'a')
    assert a['streams']['video']['duration_seconds'] >= 10 and a['streams']['audio']['duration_seconds'] >= 10
    result = {'passed': True, 'runtime': runtime, 'media': media, 'horizontal_continuous_during_vertical_receiver_failure': True,
              'tested_primary_canvases': ['main', 'portrait'], 'native_linux_validated': platform.system() == 'Linux',
              'native_windows_validated': platform.system() == 'Windows', 'receiver': 'Two MediaMTX RTMP servers on 127.0.0.1 only',
              'limits': ['Public platform ingest/OAuth not tested', 'Native Windows execution pending']}
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'passed': True, 'native_checks': len(runtime['checks']), 'received_files': len(media),
                      'bitrates_kbps': [6200, 2800], 'audio_confirmed': True, 'horizontal_continues_when_vertical_disconnects': True}))


if __name__ == '__main__':
    main()
