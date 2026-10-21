#!/usr/bin/env python3
"""Validate real native main/portrait recordings and saved clips, including PCM."""
import argparse
from array import array
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess


def run(*args):
    return subprocess.run(args, capture_output=True, check=True, timeout=90)


def inspect(entry):
    path = Path(entry['path'])
    data = json.loads(run('ffprobe', '-v', 'error', '-show_streams', '-show_packets',
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
        assert 2 < duration < 15, (path, 'invalid duration', duration)
        assert gap < .09, (path, 'packet gap', stream['codec_type'], gap)
        tracks[stream['codec_type']] = {'duration_seconds': round(duration, 3), 'max_gap_seconds': round(gap, 3),
                                       'packets': len(stamps), 'measured_kbps': round(sum(int(p['size']) for p in packets)*8/duration/1000)}
    assert abs(tracks['video']['duration_seconds'] - tracks['audio']['duration_seconds']) < .25, (path, 'A/V drift', tracks)
    decode = run('ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-')
    assert not decode.stderr.strip(), decode.stderr.decode(errors='replace')[:400]
    pcm = array('f', run('ffmpeg', '-nostdin', '-v', 'error', '-ss', '1', '-i', str(path), '-t', '1', '-vn', '-ac', '1', '-ar', '48000', '-f', 'f32le', '-').stdout)
    assert len(pcm) > 40000
    rms = math.sqrt(sum(x*x for x in pcm)/len(pcm))
    real = sum(x*math.cos(2*math.pi*440*i/48000) for i, x in enumerate(pcm)) / len(pcm)
    imag = sum(x*math.sin(2*math.pi*440*i/48000) for i, x in enumerate(pcm)) / len(pcm)
    tone_peak = 2*math.hypot(real, imag)
    assert rms > .025 and tone_peak > .035, (path, 'synthetic audio missing', rms, tone_peak)
    def sample(second, filter):
        return run('ffmpeg', '-nostdin', '-v', 'error', '-ss', str(second), '-i', str(path), '-vf', filter,
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    runtime = json.loads((args.root/'runtime.json').read_text())
    assert runtime['passed'], runtime['error']
    media = [inspect(entry) for entry in runtime['files']]
    assert len(media) == 10
    for phase in (0, 2):
        pair = [m for m in media if m['phase'] == phase and m['kind'] != 'clip']
        assert len(pair) == 2
        assert abs(pair[0]['streams']['video']['duration_seconds'] - pair[1]['streams']['video']['duration_seconds']) < .2
    result = {'passed': True, 'runtime': runtime, 'media': media, 'native_platform': platform.system(),
              'native_windows_validated': platform.system() == 'Windows'}
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'passed': True, 'native_checks': len(runtime['checks']), 'decoded_files': len(media),
                      'all_60_fps': True, 'image_and_audio': True, 'native_platform': platform.system()}))


if __name__ == '__main__':
    main()
