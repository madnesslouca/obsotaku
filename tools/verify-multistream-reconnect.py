#!/usr/bin/env python3
"""Verify media received by the opt-in native two-destination RTMP probe.

Requires FFmpeg/ffprobe on PATH on Windows or Linux. No platform credentials.
The first A file must contain continuous H.264 + AAC throughout B's manual
restart, forced network loss, recovery, rapid toggle and cancellation.
"""
import argparse
import json
from pathlib import Path
import subprocess
import platform


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True, timeout=60).stdout


def inspect(path):
    data = json.loads(run("ffprobe", "-v", "error", "-show_packets", "-show_streams",
                          "-show_entries", "stream=index,codec_name,codec_type,width,height:packet=stream_index,dts_time,pts_time",
                          "-of", "json", str(path)))
    streams = data["streams"]
    assert any(s["codec_name"] == "h264" and s["width"] == 320 and s["height"] == 180 for s in streams), path.name + ": video missing"
    assert any(s["codec_name"] == "aac" for s in streams), path.name + ": audio missing"
    details = {}
    for stream in streams:
        stamps = [float(p["dts_time"]) for p in data["packets"]
                  if p["stream_index"] == stream["index"] and "dts_time" in p]
        assert len(stamps) > 5, path.name + ": too few packets"
        gaps = [b - a for a, b in zip(stamps, stamps[1:])]
        assert min(gaps) >= -0.001, path.name + ": timestamp regression"
        details[stream["codec_type"]] = {"codec": stream["codec_name"], "packets": len(stamps),
                                        "duration_seconds": round(stamps[-1] - stamps[0], 3),
                                        "maximum_packet_gap_seconds": round(max(gaps), 3)}
    decoded = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path),
                              "-map", "0:v:0", "-map", "0:a:0", "-f", "null", "-"],
                             check=True, text=True, capture_output=True, timeout=60)
    assert not decoded.stderr.strip(), path.name + ": decode errors: " + decoded.stderr[:500]
    return {"file": str(path), "bytes": path.stat().st_size, "fully_decoded": True, "streams": details}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    runtime = json.loads((args.root / "runtime.json").read_text())
    assert runtime["passed"], runtime["error"]
    assert all(c["passed"] for c in runtime["checks"])
    media = [inspect(path) for path in sorted(args.root.rglob("*.mp4")) if path.stat().st_size > 0]
    first = next(m for m in media if Path(m["file"]).relative_to(args.root).parts[0] == "a")
    for kind in ("audio", "video"):
        stream = first["streams"][kind]
        assert stream["duration_seconds"] >= 15, "A test too short to cover all failures"
        assert stream["maximum_packet_gap_seconds"] <= .101, "A interrupted while B failed"
    assert len([m for m in media if Path(m["file"]).relative_to(args.root).parts[0] == "b"]) >= 3, "B recovery media missing"
    assert len([m for m in media if Path(m["file"]).relative_to(args.root).parts[0] == "a"]) == 2, "A unexpectedly restarted during B failures"
    report = {"passed": True, "runtime": runtime, "media": media,
              "destination_a_continuous_during_b_failures": True,
              "native_linux_validated": platform.system() == "Linux", "native_windows_validated": platform.system() == "Windows",
              "windows_portability": "Production and probe use shared Qt/libobs APIs; native Windows execution still required.",
              "server_source": "https://github.com/bluenviron/mediamtx",
              "receiver_fixture": "Two independent MediaMTX RTMP servers bind exclusively to 127.0.0.1 and record received media as fMP4; killing B aborts its actual live RTMP socket.",
              "excluded": ["Public platform ingest", "OAuth", "primary-output reconnection policy"]}
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({"passed": True, "runtime_checks": len(runtime["checks"]),
                      "decoded_files": len(media), "continuous_a": first["streams"]}, indent=2))


if __name__ == "__main__":
    main()
