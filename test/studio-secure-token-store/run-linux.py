#!/usr/bin/python3
"""Isolated Secret Service checks. Never use the normal desktop bus/keyring."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>"""


def probe(exe, mode, root, env):
    start = time.monotonic()
    result = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True, timeout=20)
    (root / (mode + ".stdout")).write_text(result.stdout)
    (root / (mode + ".stderr")).write_text(result.stderr)
    facts = json.loads(result.stdout) if result.stdout.strip() else {}
    facts.update(exit_code=result.returncode, elapsed_ms=round(1000 * (time.monotonic() - start), 3))
    return facts


def wait_name(env, process):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError("Private Secret Service exited before acquiring name")
        reply = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                                "--object-path", "/org/freedesktop/DBus", "--method",
                                "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.secrets"],
                               env=env, capture_output=True, text=True)
        if "true" in reply.stdout:
            return
        time.sleep(.02)
    raise RuntimeError("Private Secret Service readiness timed out")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def inner(args):
    root = args.root
    env = os.environ.copy()
    env["OTAKUDOBS_PRIVATE_BUS_ADDRESS"] = env["DBUS_SESSION_BUS_ADDRESS"]
    results = []
    if args.inner == "unavailable":
        results.append(probe(args.probe, "unavailable", root, env))
    elif args.inner == "real":
        def start_keyring(index):
            log = open(root / ("keyring-" + str(index) + ".log"), "wb")
            daemon = subprocess.Popen(["gnome-keyring-daemon", "--foreground", "--unlock",
                                       "--components=secrets", "--control-directory=" + str(root / "control")],
                                      env=env, stdin=subprocess.PIPE, stdout=log, stderr=log)
            daemon.stdin.write(b"private-qa-collection-password\n")
            daemon.stdin.close()
            wait_name(env, daemon)
            return daemon, log
        daemon, log = start_keyring(1)
        try:
            for mode in ["roundtrip", "parallel", "persist-write"]:
                results.append(probe(args.probe, mode, root, env))
        finally:
            stop(daemon)
            log.close()
        # All service/client processes from the first session have exited. A
        # newly launched daemon must decrypt the same private persistent file.
        time.sleep(.1)
        daemon, log = start_keyring(2)
        try:
            results.append(probe(args.probe, "persist-read", root, env))
        finally:
            stop(daemon)
            log.close()
        files = list((root / "data" / "keyrings").glob("*"))
        canary = b"OtakudOBS-QA-private-canary"
        encrypted = [p for p in files if p.suffix == ".keyring"]
        results.append({"mode": "private-encrypted-storage", "checks": 2, "failures":
                        int(not encrypted) + int(any(canary in p.read_bytes() for p in files)),
                        "passed": bool(encrypted) and not any(canary in p.read_bytes() for p in files)})
    else:
        ready = root / "fake.ready"
        log = open(root / "fake.log", "wb")
        daemon = subprocess.Popen([sys.executable, str(HERE / "fake-service.py"), "--mode", args.inner,
                                   "--ready", str(ready)], env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if ready.exists():
                    break
                if daemon.poll() is not None:
                    raise RuntimeError("Private fake Secret Service exited")
                time.sleep(.02)
            if not ready.exists():
                raise RuntimeError("Private fake Secret Service readiness timed out")
            modes = ["timeout", "cancel", "pre-cancel", "expired"] if args.inner == "hang" else [args.inner]
            for mode in modes:
                results.append(probe(args.probe, mode, root, env))
        finally:
            stop(daemon)
            log.close()
    (root / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if all(r.get("passed") and r.get("exit_code", 0) == 0 for r in results) else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--inner", choices=["real", "hang", "locked", "remote-error", "commit-cancel", "commit-timeout", "unavailable"])
    parser.add_argument("--cases", default="real,hang,locked,remote-error,commit-cancel,commit-timeout,unavailable")
    args = parser.parse_args()
    args.probe = args.probe.resolve()
    args.root = args.root.resolve()
    if args.inner:
        return inner(args)
    # Fresh output roots are mandatory: failures are never overwritten.
    args.root.mkdir(mode=0o700, parents=True, exist_ok=False)
    (args.root / "bus.conf").write_text(BUS_CONFIG)
    all_results = []
    for case in args.cases.split(","):
        root = args.root / case
        root.mkdir(mode=0o700)
        for directory in ["data", "config", "cache", "runtime", "control"]:
            (root / directory).mkdir(mode=0o700)
        env = os.environ.copy()
        for key in ["DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
                    "GNOME_KEYRING_CONTROL", "GNOME_KEYRING_PID", "SECRET_BACKEND"]:
            env.pop(key, None)
        env.update(XDG_DATA_HOME=str(root / "data"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"), XDG_RUNTIME_DIR=str(root / "runtime"))
        command = ["dbus-run-session", "--config-file=" + str(args.root / "bus.conf"), "--",
                   sys.executable, str(Path(__file__).resolve()), "--probe", str(args.probe),
                   "--root", str(root), "--inner", case]
        run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=100)
        (root / "runner.stdout").write_text(run.stdout)
        (root / "runner.stderr").write_text(run.stderr)
        entries = json.loads((root / "result.json").read_text()) if (root / "result.json").exists() else []
        all_results.append({"case": case, "exit_code": run.returncode, "results": entries})
    result = {"passed": all(x["exit_code"] == 0 for x in all_results),
              "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(), "cases": all_results}
    (args.root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
