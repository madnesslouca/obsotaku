#!/usr/bin/env python3
"""Measure scene-local gain and mute in the generated 440 Hz recording."""
import argparse
import json
import math
import subprocess
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[1] / 'validation/scene-audio-recording.json')
parser.add_argument('--report', type=Path)
args = parser.parse_args()
data = json.loads(args.manifest.read_text(encoding='utf-8'))
recording = str(Path(data['result']['outputPath']).resolve())
probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', recording]))
video = next(s for s in probe['streams'] if s['codec_type'] == 'video')
audio = next(s for s in probe['streams'] if s['codec_type'] == 'audio')
segments = []
for segment in data['segments']:
    start = (segment['startMs'] + 300) / 1000
    duration = (segment['endMs'] - segment['startMs'] - 600) / 1000
    if duration <= 0:
        raise ValueError('Segment too short for stable audio measurement')
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-ss', str(start), '-i', recording, '-t', str(duration), '-map', '0:a:0', '-ac', '1', '-ar', '48000', '-f', 'f32le', '-'])
    samples = np.frombuffer(raw, dtype='<f4').astype(np.float64)
    if samples.size == 0:
        raise ValueError('Empty audio segment')
    rms = float(np.sqrt(np.mean(samples * samples)))
    peak = float(np.max(np.abs(samples)))
    spectrum = np.abs(np.fft.rfft(samples * np.hanning(len(samples))))
    frequency = float(np.fft.rfftfreq(len(samples), 1 / 48000)[np.argmax(spectrum)]) if rms > 1e-8 else None
    segments.append({'label': segment['label'], 'rms_dbfs': 20 * math.log10(max(rms, 1e-12)), 'peak_dbfs': 20 * math.log10(max(peak, 1e-12)), 'dominant_frequency_hz': frequency, 'samples': len(samples)})
expected = ['global-minus-6', 'local-muted', 'local-minus-12', 'global-return-minus-6']
if [s['label'] for s in segments] != expected:
    raise ValueError('Unexpected recording phases')
decode = subprocess.run(['ffmpeg', '-v', 'error', '-i', recording, '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'], capture_output=True, text=True)
gain_delta = segments[0]['rms_dbfs'] - segments[2]['rms_dbfs']
return_delta = segments[3]['rms_dbfs'] - segments[0]['rms_dbfs']
checks = {
    'full_decode': decode.returncode == 0 and not decode.stderr,
    '1080p60': video['width'] == 1920 and video['height'] == 1080 and video['avg_frame_rate'] == '60/1',
    'aac_48khz': audio['codec_name'] == 'aac' and audio['sample_rate'] == '48000',
    'six_db_scene_gain_difference': abs(gain_delta - 6) < .35,
    'local_mute_silent': segments[1]['rms_dbfs'] < -80,
    'global_level_restored': abs(return_delta) < .3,
    'tone_is_440hz': all(segments[i]['dominant_frequency_hz'] is not None and abs(segments[i]['dominant_frequency_hz'] - 440) < 2 for i in (0, 2, 3)),
}
report = {
    'passed': all(checks.values()), 'checks': checks, 'recording': recording, 'segments': segments,
    'global_minus_local_db': gain_delta, 'global_return_difference_db': return_delta,
    'video': {k: video.get(k) for k in ['codec_name', 'width', 'height', 'avg_frame_rate', 'nb_read_frames']},
    'duration_seconds': probe['format']['duration'], 'decode_errors': decode.stderr,
    'scope': 'Generated 440 Hz media source shared between two scenes; measured recorded gain/mute, no microphone or public stream.',
}
report_path = args.report or args.manifest.with_name('scene-audio-recording-validation.json')
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'passed': report['passed'], 'checks': checks, 'gain_difference_db': gain_delta, 'report': str(report_path.resolve())}, indent=2))
raise SystemExit(0 if report['passed'] else 1)
