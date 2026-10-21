#!/usr/bin/env python3
"""Validate files from the opt-in native audio/multistream integration probe."""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np


def run(*args):
    return subprocess.run(args, capture_output=True, check=True)


def validate(directory):
    results = []
    expectations = {
        "audio-multitrack.mkv": [440, 880, 660],
        "rtmp-a.flv": [440],
        "rtmp-b.flv": [880],
        "rtmp-primary.flv": [660],
    }
    for name, expected in expectations.items():
        path = (directory / name).resolve()
        probe = json.loads(run("ffprobe", "-v", "error", "-count_frames", "-show_streams", "-show_format", "-of", "json", str(path)).stdout)
        decode = run("ffmpeg", "-v", "error", "-i", str(path), "-f", "null", "-")
        tracks = []
        for index, frequency in enumerate(expected):
            raw = run("ffmpeg", "-v", "error", "-ss", "2", "-i", str(path), "-map", f"0:a:{index}", "-t", "3", "-ac", "1", "-ar", "48000", "-f", "f32le", "-").stdout
            samples = np.frombuffer(raw, dtype=np.float32)
            if not len(samples):
                raise ValueError(f"Empty audio track {index + 1} in {name}")
            spectrum = np.abs(np.fft.rfft(samples * np.hanning(len(samples))))
            frequencies = np.fft.rfftfreq(len(samples), 1 / 48000)
            dominant = float(frequencies[np.argmax(spectrum)])
            peak = float(np.max(np.abs(samples)))
            desired = float(spectrum[np.abs(frequencies - frequency) <= 2].max())
            leakage = {}
            for other in [440, 660, 880]:
                if other != frequency:
                    amplitude = float(spectrum[np.abs(frequencies - other) <= 2].max())
                    leakage[str(other)] = float(20 * np.log10(max(amplitude, 1e-20) / max(desired, 1e-20)))
            passed = peak > .001 and abs(dominant - frequency) < 2 and all(v < -40 for v in leakage.values())
            tracks.append({"file_audio_track": index + 1, "expected_hz": frequency, "dominant_hz": dominant,
                           "peak_dbfs": float(20 * np.log10(peak)) if peak else None,
                           "other_test_tones_relative_db": leakage, "passed": passed})
        audio_streams = [s for s in probe["streams"] if s["codec_type"] == "audio"]
        videos = [{k: s.get(k) for k in ["codec_name", "width", "height", "r_frame_rate", "nb_read_frames"]}
                  for s in probe["streams"] if s["codec_type"] == "video"]
        results.append({"file": str(path), "audio": tracks, "audio_stream_count": len(audio_streams),
                        "video": videos, "duration": probe["format"]["duration"],
                        "full_decode_passed": not decode.stderr.strip(),
                        "passed": len(audio_streams) == len(expected) and not decode.stderr.strip() and all(t["passed"] for t in tracks)})
    return {"passed": all(r["passed"] for r in results), "receivers": "loopback RTMP only", "files": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = validate(args.directory)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": result["passed"], "files": len(result["files"])}))
    raise SystemExit(0 if result["passed"] else 1)
