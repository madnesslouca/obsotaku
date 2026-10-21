#!/usr/bin/env python3
"""Fetch the CEF build pinned by this OBS tree and verify its SHA-256."""
import argparse
import hashlib
import json
import tarfile
import urllib.request
from pathlib import Path


def find_cef(value):
    if isinstance(value, dict):
        if "cef_binary_6533_linux_x86_64_v6" in value.get("url", ""):
            return value
        children = value.values()
    elif isinstance(value, list):
        children = value
    else:
        return None
    for child in children:
        result = find_cef(child)
        if result:
            return result
    return None


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/obs-zyon-deps")
    args = parser.parse_args()
    manifest = Path(__file__).resolve().parents[1] / "build-aux/com.obsproject.Studio.json"
    entry = find_cef(json.loads(manifest.read_text()))
    if not entry:
        raise SystemExit("No pinned Linux x86_64 CEF build found in this tree")
    args.cache.mkdir(parents=True, exist_ok=True)
    archive = args.cache / entry["url"].rsplit("/", 1)[1]
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + ".part")
        urllib.request.urlretrieve(entry["url"], temporary)
        if sha256(temporary) != entry["sha256"]:
            raise SystemExit("CEF download hash mismatch; archive not extracted")
        temporary.replace(archive)
    if sha256(archive) != entry["sha256"]:
        raise SystemExit("Cached CEF archive hash mismatch; archive not extracted")
    root = args.cache / "cef_binary_6533_linux_x86_64"
    if not root.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(args.cache, filter="data")
    if not (root / "include/cef_version.h").exists() or not (root / "build/libcef_dll_wrapper/libcef_dll_wrapper.a").exists():
        raise SystemExit("CEF extraction is incomplete; inspect the cache before retrying")
    print(root)
