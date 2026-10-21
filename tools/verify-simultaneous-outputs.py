#!/usr/bin/env python3
"""Validate files produced by simultaneous native streaming, recording and clips."""
import argparse
from array import array
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess


def execute(*args):
    return subprocess.run(args, capture_output=True, check=True, timeout=90)


run = execute


def inspect_local(entry):
    path = Path(entry['path'])
    data = json.loads(execute('ffprobe', '-v', 'error', '-show_streams', '-show_packets',
                          '-show_entries', 'stream=index,codec_name,codec_type,width,height,avg_frame_rate:packet=stream_index,pts_time,size',
                          '-of', 'json', str(path)).stdout)
    video = next(s for s in data['streams'] if s['codec_type'] == 'video')
    audio = next(s for s in data['streams'] if s['codec_type'] == 'audio')
    assert (video['width'], video['height']) == (entry['width'], entry['height']), entry
    assert Fraction(video['avg_frame_rate']) == 60, (path, video)
    assert video['codec_name'] == 'h264' and audio['codec_name'] == 'aac'
    tracks = {}
    for stream in (video, audio):
        packets = [p for p in data['packets'] if p['stream_index'] == stream['index'] and 'pts_time' in p]
        stamps = sorted(float(p['pts_time']) for p in packets)
        duration = stamps[-1] - stamps[0]
        gap = max(b-a for a, b in zip(stamps, stamps[1:]))
        assert .8 < duration < 30, (path, 'invalid duration', duration)
        assert gap < .09, (path, 'packet gap', stream['codec_type'], gap)
        tracks[stream['codec_type']] = {'duration_seconds': round(duration, 3), 'max_gap_seconds': round(gap, 3),
                                       'packets': len(stamps), 'measured_kbps': round(sum(int(p['size']) for p in packets)*8/duration/1000)}
    assert abs(tracks['video']['duration_seconds'] - tracks['audio']['duration_seconds']) < .25, (path, 'A/V drift', tracks)
    decode = execute('ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-')
    assert not decode.stderr.strip(), decode.stderr.decode(errors='replace')[:400]
    pcm = array('f', execute('ffmpeg', '-nostdin', '-v', 'error', '-ss', '1', '-i', str(path), '-t', '1', '-vn', '-ac', '1', '-ar', '48000', '-f', 'f32le', '-').stdout)
    assert len(pcm) > 40000
    rms = math.sqrt(sum(x*x for x in pcm)/len(pcm))
    real = sum(x*math.cos(2*math.pi*440*i/48000) for i, x in enumerate(pcm)) / len(pcm)
    imag = sum(x*math.sin(2*math.pi*440*i/48000) for i, x in enumerate(pcm)) / len(pcm)
    tone_peak = 2*math.hypot(real, imag)
    assert rms > .025 and tone_peak > .035, (path, 'synthetic audio missing', rms, tone_peak)
    def sample(second, filter):
        return execute('ffmpeg', '-nostdin', '-v', 'error', '-ss', str(second), '-i', str(path), '-vf', filter,
                   '-frames:v', '1', '-f', 'rawvideo', '-').stdout
    movement = sample(1, 'scale=32:32,format=rgb24') != sample(2, 'scale=32:32,format=rgb24')
    assert movement, (path, 'moving image missing')
    portrait_marker = False
    if entry['width'] == 1080:
        pixel = sample(1, 'crop=4:4:80:80,format=rgb24')[:3]
        red, green, blue = pixel
        portrait_marker = green > 140 and green > red+50 and green > blue+50
        assert portrait_marker, (path, 'independent portrait layer missing', list(pixel))
    return {**entry, 'fps': 60, 'fully_decoded': True, 'moving_image': movement, 'portrait_layer': portrait_marker,
            'audio_rms': round(rms, 4), 'audio_440hz_peak': round(tone_peak, 4), 'streams': tracks,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def inspect_received(path, root):
    parts = path.relative_to(root).parts
    session = int(parts[0].removeprefix('phase-'))
    primary = parts[1] == 'a'
    portrait = not primary
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
    return {'file': str(path), 'phase': session, 'destination': parts[1], 'canvas': 'portrait' if portrait else 'main',
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
    local = [inspect_local(entry) for entry in runtime['files']]
    received = [inspect_received(path, args.root) for path in sorted(args.root.glob('phase-*/[ab]/**/*.mp4')) if path.stat().st_size > 0]
    assert len(local) == 16, len(local)
    assert len(received) == 9, len(received)
    for phase in range(4):
        current = [entry for entry in local if entry['phase'] == phase]
        assert len([f for f in current if f['kind'] == 'clip']) == 2
        assert len([f for f in current if f['kind'] != 'clip']) == 2
        streams = [entry for entry in received if entry['phase'] == phase]
        assert len([f for f in streams if f['destination'] == 'a']) == 1
        assert len([f for f in streams if f['destination'] == 'b']) == (2 if phase == 0 else 1)
    primary = next(m for m in received if m['phase'] == 0 and m['destination'] == 'a')
    assert primary['streams']['video']['duration_seconds'] > 14
    report = {'passed': True, 'runtime': runtime, 'local_media': local, 'received_media': received,
              'native_frontend_pipeline_tested': True, 'simultaneous_outputs': ['horizontal RTMP', 'portrait RTMP', 'horizontal recording', 'portrait recording', 'one native replay buffer selected main or portrait per run'],
              'modes': ['Simple', 'Advanced'], 'native_linux_validated': platform.system() == 'Linux',
              'native_windows_validated': platform.system() == 'Windows',
              'limits': ['One clip canvas per native replay buffer; main and portrait clip selection verified in separate simultaneous runs.', 'Public ingest/OAuth and native Windows execution are not validated.']}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'passed': True, 'native_checks': len(runtime['checks']), 'local_files': len(local), 'received_files': len(received), 'audio_confirmed_in_all_files': True}))


if __name__ == '__main__':
    main()
