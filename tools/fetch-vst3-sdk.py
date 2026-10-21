#!/usr/bin/env python3
"""Fetch the pinned MIT VST3 host SDK on Windows or Linux, without root."""
import argparse
import pathlib
import subprocess

SDK_REV = '9fad9770f2ae8542ab1a548a68c1ad1ac690abe0'
SUBMODULES = {
    'base': '3d2e82f8e6bff59c1d8b7a27491a29c2286b5206',
    'pluginterfaces': '31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c',
    'public.sdk': 'a3911a4615dabbfdfd9d181ee26b05c70c289a95',
}

def git(*args, cwd=None):
    return subprocess.check_output(['git', *args], cwd=cwd, text=True).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=pathlib.Path, required=True,
                        help='Parent directory containing vst3sdk; pass this to STEINBERG_VST3_PATH')
    args = parser.parse_args()
    prefix = args.prefix.expanduser().resolve()
    root = prefix / 'vst3sdk'
    prefix.mkdir(parents=True, exist_ok=True)
    if not root.exists():
        git('clone', '--depth', '1', '--branch', 'v3.8.0_build_66',
            'https://github.com/steinbergmedia/vst3sdk.git', str(root))
    if git('rev-parse', 'HEAD', cwd=root) != SDK_REV:
        raise SystemExit('Existing SDK has a different revision. Choose a fresh prefix; no checkout reset performed.')
    git('submodule', 'update', '--init', '--depth', '1', *SUBMODULES, cwd=root)
    for name, revision in SUBMODULES.items():
        if git('rev-parse', 'HEAD', cwd=root / name) != revision:
            raise SystemExit(f'Unexpected {name} revision')
        if git('status', '--porcelain', cwd=root / name):
            raise SystemExit(f'Existing {name} SDK sources are modified; choose a clean prefix.')
    print(f'Pinned VST3 SDK ready: {root}')
    print(f'-DENABLE_VST3=ON -DSTEINBERG_VST3_PATH={prefix}')

if __name__ == '__main__':
    main()
