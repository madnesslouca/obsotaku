#!/usr/bin/env python3
"""Prepare local-only dual replay media. Never launches or terminates OBS.

The parent orchestrator backs up the user's config, waits for the identified idle
OBS to exit, builds/installs exact artifacts, and launches the opt-in probe with
returned environment. MediaMTX receivers belong to that probe and bind loopback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--server', required=True, type=Path, help='Local MediaMTX executable')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    root = args.root.resolve()
    server = args.server.resolve()
    if not server.is_file():
        raise SystemExit(f'MediaMTX executable missing: {server}')
    root.mkdir(parents=True, exist_ok=True)
    fixture = root / 'fixture.mp4'
    manifest = root / 'prepare.json'
    if fixture.exists() or manifest.exists() or (root / 'runtime.json').exists():
        raise SystemExit('Refusing to overwrite a fixture/report; use a fresh private stage directory.')
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        raise SystemExit('ffmpeg not found')
    # Common FFmpeg implementation on Linux/Windows. Default font discovery is
    # delegated to the installed FFmpeg/fontconfig. Explicit drawtext failure
    # stops preparation rather than silently dropping the visible frame counter.
    command = [ffmpeg, '-nostdin', '-hide_banner', '-v', 'error', '-n',
               '-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=60',
               '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
               '-vf', "drawtext=text='FRAME %{n}':x=32:y=32:fontsize=72:fontcolor=white:box=1:boxcolor=black@0.8",
               '-t', '12', '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '15',
               '-g', '60', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k',
               '-ar', '48000', '-ac', '2', '-movflags', '+faststart', str(fixture)]
    subprocess.run(command, check=True, timeout=180)
    data = {'prepared': True, 'fixture': str(fixture),
            'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
            'pattern': '1920x1080 60 fps moving testsrc2 plus visible frame counter; 440Hz stereo 48kHz',
            'environment': {'OBS_STUDIO_DUAL_REPLAY_ROOT': str(root),
                            'OBS_STUDIO_DUAL_REPLAY_SERVER': str(server),
                            'OBS_STUDIO_DUAL_REPLAY_CONFIG_ROOT': 'REQUIRED: private root matching all four global.ini Locations keys' },
            'probe': 'studio-dual-replay-check.so (platform module extension varies)',
            'runtime_timeout_seconds': 180,
            'safety': ['Does not start OBS', 'Does not change user config',
                       'Probe duplicates the current profile and preserves sources/services/destinations',
                       'Only two temporary loopback RTMP receivers are started by the probe']}
    with manifest.open('x') as out:
        json.dump(data, out, ensure_ascii=False, indent=2)
        out.write('\n')
    print(json.dumps(data, ensure_ascii=False))


if __name__ == '__main__':
    main()
