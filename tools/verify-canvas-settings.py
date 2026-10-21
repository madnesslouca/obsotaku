#!/usr/bin/env python3
"""Decode the two recordings from the opt-in native canvas settings probe."""
import argparse
import json
from pathlib import Path
import platform
import subprocess


def execute(*command):
    return subprocess.run(command, check=True, capture_output=True, timeout=60)


def inspect(path, fps):
    data = json.loads(execute("ffprobe", "-v", "error", "-show_streams", "-show_packets",
                              "-show_entries", "stream=index,codec_type,codec_name,width,height,avg_frame_rate:packet=stream_index,pts_time,dts_time",
                              "-of", "json", str(path)).stdout)
    video = next(s for s in data["streams"] if s["codec_type"] == "video")
    audio = next(s for s in data["streams"] if s["codec_type"] == "audio")
    assert (video["width"], video["height"]) == (1080, 1920)
    assert video["codec_name"] == "h264" and audio["codec_name"] == "aac"
    numerator, denominator = map(int, video["avg_frame_rate"].split("/"))
    assert numerator == fps * denominator
    tracks = {}
    for stream in (video, audio):
        stamps = sorted(float(p["pts_time"]) for p in data["packets"]
                        if p["stream_index"] == stream["index"] and "pts_time" in p)
        assert len(stamps) >= 30
        duration = stamps[-1] - stamps[0]
        assert 4 <= duration <= 7, "Incorrect canvas timing"
        tracks[stream["codec_type"]] = {"packets": len(stamps), "duration_seconds": round(duration, 3),
                                        "max_packet_gap_seconds": round(max(b-a for a,b in zip(stamps, stamps[1:])), 3)}
    assert abs(tracks["video"]["duration_seconds"] - tracks["audio"]["duration_seconds"]) < .2, "A/V drift"
    decoded = execute("ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-map", "0:v:0", "-map", "0:a:0", "-f", "null", "-")
    assert not decoded.stderr.strip(), decoded.stderr.decode(errors="replace")[:500]
    sample = execute("ffmpeg", "-nostdin", "-v", "error", "-ss", "1", "-i", str(path), "-vf",
                     "crop=4:4:80:80,format=rgb24", "-frames:v", "1", "-f", "rawvideo", "pipe:1").stdout
    red, green, blue = sample[:3]
    assert green > 140 and green > red + 50 and green > blue + 50, "Independent portrait layer missing"
    return {"file": str(path), "fps": fps, "resolution": "1080x1920", "fully_decoded": True,
            "independent_vertical_layer_present": True, "streams": tracks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    runtime = json.loads((args.root / "runtime.json").read_text())
    assert runtime["passed"], runtime["error"]
    media = [inspect(args.root / f"portrait-{fps}.mkv", fps) for fps in (30, 60)]
    result = {"passed": True, "runtime": runtime, "media": media,
              "native_platform": platform.system(), "native_windows_validated": platform.system() == "Windows"}
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"passed": True, "runtime_checks": len(runtime["checks"]), "media": media}, indent=2))


if __name__ == "__main__":
    main()
