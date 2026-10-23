#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check fixture transport only; this is NOT OBS/RTMP acceptance."""

import asyncio
import json
import pathlib
import sys
import time


async def check(report_path):
    checks = []
    events = []
    clients = set()
    children = set()

    def require(passed, name):
        checks.append({"check": name, "passed": bool(passed)})
        if not passed:
            raise AssertionError(name)

    async def echo(reader, writer):
        task = asyncio.current_task()
        children.add(task)
        clients.add(writer)
        try:
            while data := await reader.read(8192):
                writer.write(data)
                await writer.drain()
        except (OSError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
            clients.discard(writer)
            children.discard(task)

    server = await asyncio.start_server(echo, "127.0.0.1", 0)
    target = server.sockets[0].getsockname()[1]
    process = await asyncio.create_subprocess_exec(
        sys.executable, str(pathlib.Path(__file__).with_name("proxy.py")),
        "--target-port", str(target), stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)

    async def event(kind):
        while True:
            raw = await asyncio.wait_for(process.stdout.readline(), timeout=5)
            if not raw:
                raise AssertionError("Proxy terminated before " + kind)
            value = json.loads(raw)
            events.append(value)
            if value["event"] == kind:
                return value

    async def command(value, kind):
        process.stdin.write(json.dumps(value).encode() + b"\n")
        await process.stdin.drain()
        return await event(kind)

    failure = ""
    proxy_writer = direct_writer = None
    try:
        ready = await event("ready")
        require(ready["address"] == "127.0.0.1", "proxy binds only IPv4 loopback")
        reader, proxy_writer = await asyncio.open_connection("127.0.0.1", ready["port"])
        await event("connected")
        initial = bytes(range(256)) * 16
        proxy_writer.write(initial)
        await proxy_writer.drain()
        require(await asyncio.wait_for(reader.readexactly(len(initial)), 3) == initial,
                "unrestricted handshake-sized bytes round-trip exactly")
        await command({"rate_bps": 4096}, "rate")
        payload = bytes(reversed(range(256))) * 128
        proxy_writer.write(payload)
        await proxy_writer.drain()
        began = time.monotonic()
        received = b""
        while time.monotonic() - began < 1.0:
            try:
                received += await asyncio.wait_for(reader.read(8192), timeout=0.1)
            except asyncio.TimeoutError:
                pass
        elapsed = time.monotonic() - began
        limited_bytes = len(received)
        require(2500 <= len(received) <= 6500, "configured4096B/s causes measurable bounded forwarding")
        direct_reader, direct_writer = await asyncio.open_connection("127.0.0.1", target)
        direct_writer.write(initial)
        await direct_writer.drain()
        require(await asyncio.wait_for(direct_reader.readexactly(len(initial)), 2) == initial,
                "independent direct connection progresses while proxy is limited")
        await command({"rate_bps": 0}, "rate")
        received += await asyncio.wait_for(reader.readexactly(len(payload) - len(received)), 3)
        require(received == payload, "lifting limit drains queued bytes without corruption or duplication")
        process.stdin.write(b"not-json\n")
        await process.stdin.drain()
        require((await event("command_error"))["error"] == "Expected a JSON object",
                "invalid command is rejected without closing established connection")
        await command({"rate_bps": -1}, "command_error")
        proxy_writer.close()
        await proxy_writer.wait_closed()
        await event("disconnected")
        stats = (await command({"snapshot": True}, "snapshot"))["sessions"][0]
        require(stats["publisher_read"] == stats["publisher_forwarded"] == len(initial) + len(payload),
                "publisher byte counters account for every forwarded byte")
        require(stats["receiver_read"] == stats["receiver_forwarded"] == len(initial) + len(payload),
                "reverse path preserves all acknowledged data")
        require(stats["publisher_max_chunk"] <= 4096 and stats["receiver_max_chunk"] <= 4096,
                "relay uses bounded chunks in both directions")
        require(stats["publisher_receive_buffer"] > 0, "actual kernel receive buffer is recorded")
        stopped = await command({"stop": True}, "stopped")
        process.stdin.close()
        require(stopped["tasks_remaining"] == 0, "fixture awaits all owned connection tasks")
        require(await asyncio.wait_for(process.wait(), 5) == 0, "fixture exits cleanly")
        require(not await process.stderr.read(), "fixture emits no unexpected errors")
        measurements = {"limited_bytes": limited_bytes,
                        "window_seconds": elapsed}
    except Exception as error:
        failure = str(error)
        measurements = {}
    finally:
        if process.returncode is None:
            process.stdin.close()
            try:
                await asyncio.wait_for(process.wait(), 5)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
        for writer in (proxy_writer, direct_writer, *list(clients)):
            if writer:
                writer.close()
        server.close()
        await server.wait_closed()
        await asyncio.gather(*list(children), return_exceptions=True)
        result = {"passed": not failure, "error": failure, "checks": checks, "events": events,
                  "measurements": measurements,
                  "scope": "Synthetic TCP echo validates congestion fixture only; no OBS, RTMP, encoded media or Windows certification"}
        report_path.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"passed": result["passed"], "checks": len(checks), "error": failure}))
    return 1 if failure else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(check(pathlib.Path(sys.argv[1]))))
