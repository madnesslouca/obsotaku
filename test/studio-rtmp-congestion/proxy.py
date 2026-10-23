#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in loopback RTMP congestion fixture. Never used by the product.

stdout emits metadata only; stdin accepts one JSON command per line:
  {"rate_bps": 4096}   publisher -> receiver bytes/s; 0 removes the limit
  {"snapshot": true}   current counters
  {"stop": true}       close this fixture's listeners/connections
The reverse direction stays unrestricted for RTMP acknowledgements.
"""

import argparse
import asyncio
import json
import os
import socket
import sys
import threading
import time
from functools import partial


class Proxy:
    def __init__(self, target_port):
        self.target_port = target_port
        self.rate = 0
        self.generation = 0
        self.started = time.monotonic()
        self.stopped = asyncio.Event()
        self.server = None
        self.connections = set()
        self.sessions = []

    def emit(self, kind, **values):
        print(json.dumps({"event": kind, "seconds": time.monotonic() - self.started, **values}), flush=True)

    def command(self, command):
        if "rate_bps" in command:
            value = command["rate_bps"]
            if type(value) is not int or value < 0:
                self.emit("command_error", error="rate_bps must be a nonnegative integer")
                return
            self.rate = value
            self.generation += 1
            self.emit("rate", rate_bps=value, generation=self.generation)
        if command.get("snapshot"):
            self.emit("snapshot", sessions=self.sessions)
        if command.get("stop"):
            self.stopped.set()

    async def pump(self, reader, writer, stats, limited):
        credit = 0.0
        last = time.monotonic()
        generation = self.generation
        while data := await reader.read(4096):
            name = "publisher" if limited else "receiver"
            stats[name + "_read"] += len(data)
            stats[name + "_max_chunk"] = max(stats[name + "_max_chunk"], len(data))
            offset = 0
            while offset < len(data):
                now = time.monotonic()
                rate = self.rate if limited else 0
                if generation != self.generation:
                    generation = self.generation
                    credit = 0.0
                    last = now
                if rate:
                    credit = min(max(4096, rate / 10), credit + (now - last) * rate)
                    last = now
                    count = min(len(data) - offset, int(credit))
                    if count == 0:
                        await asyncio.sleep(min(0.02, 1 / rate))
                        continue
                    credit -= count
                else:
                    count = len(data) - offset
                    last = now
                writer.write(data[offset:offset + count])
                await writer.drain()
                stats[name + "_forwarded"] += count
                offset += count

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        self.connections.add(task)
        upstream = None
        pumps = []
        peer = writer.get_extra_info("socket")
        stats = {
            "id": len(self.sessions) + 1, "started": time.monotonic() - self.started,
            "publisher_read": 0, "publisher_forwarded": 0, "publisher_max_chunk": 0,
            "receiver_read": 0, "receiver_forwarded": 0, "receiver_max_chunk": 0,
        }
        self.sessions.append(stats)
        try:
            # Bound read-ahead while retaining a usable unthrottled TCP window.
            # A4KiB receive buffer on Linux induced delayed-ACK/window stalls
            # even at rate=0; actual kernel value is measured, not assumed.
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            stats["publisher_receive_buffer"] = peer.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            remote, upstream = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", self.target_port, limit=8192), timeout=5)
            writer.transport.set_write_buffer_limits(high=16384, low=4096)
            upstream.transport.set_write_buffer_limits(high=16384, low=4096)
            self.emit("connected", session=stats["id"])
            pumps = [asyncio.create_task(self.pump(reader, upstream, stats, True)),
                     asyncio.create_task(self.pump(remote, writer, stats, False))]
            done, pending = await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
            for result in done:
                result.result()
        except asyncio.CancelledError:
            stats["closed_by_fixture"] = True
            raise
        except (OSError, asyncio.TimeoutError) as error:
            # Do not echo payloads, peer input or arbitrary remote error text.
            stats["error_type"] = type(error).__name__
        finally:
            for pump in pumps:
                pump.cancel()
            await asyncio.gather(*pumps, return_exceptions=True)
            for stream in (writer, upstream):
                if stream:
                    stream.close()
            for stream in (writer, upstream):
                if stream:
                    try:
                        await asyncio.wait_for(stream.wait_closed(), timeout=2)
                    except (OSError, asyncio.TimeoutError):
                        pass
            stats["ended"] = time.monotonic() - self.started
            self.connections.discard(task)
            self.emit("disconnected", session=stats["id"])

    async def run(self, listen_port):
        self.server = await asyncio.start_server(self.handle, "127.0.0.1", listen_port,
                                                 family=socket.AF_INET, limit=8192)
        self.emit("ready", address="127.0.0.1", port=self.server.sockets[0].getsockname()[1])
        await self.stopped.wait()
        self.server.close()
        await self.server.wait_closed()
        # Closing only connections owned by this fixture; always await children.
        active = list(self.connections)
        for connection in active:
            connection.cancel()
        await asyncio.gather(*active, return_exceptions=True)
        self.emit("stopped", sessions=self.sessions, tasks_remaining=len(self.connections))


async def main(args):
    proxy = Proxy(args.target_port)
    loop = asyncio.get_running_loop()

    def read_commands():
        # Raw fd avoids a daemon holding Python's buffered-stdin lock at exit.
        buffered = b""
        while chunk := os.read(sys.stdin.fileno(), 4096):
            buffered += chunk
            if len(buffered) > 8192:
                loop.call_soon_threadsafe(partial(proxy.emit, "command_error", error="Command too large"))
                buffered = b""
                continue
            while b"\n" in buffered:
                line, buffered = buffered.split(b"\n", 1)
                try:
                    command = json.loads(line)
                    if not isinstance(command, dict):
                        raise ValueError()
                    loop.call_soon_threadsafe(proxy.command, command)
                except (ValueError, UnicodeDecodeError):
                    loop.call_soon_threadsafe(partial(proxy.emit, "command_error", error="Expected a JSON object"))
                except RuntimeError:
                    return
        try:
            loop.call_soon_threadsafe(proxy.stopped.set)
        except RuntimeError:
            pass

    threading.Thread(target=read_commands, name="fixture-commands", daemon=True).start()
    await proxy.run(args.listen_port)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-port", type=int, required=True)
    parser.add_argument("--listen-port", type=int, default=0)
    options = parser.parse_args()
    if not 1 <= options.target_port <= 65535 or not 0 <= options.listen_port <= 65535:
        parser.error("Invalid loopback TCP port")
    asyncio.run(main(options))
