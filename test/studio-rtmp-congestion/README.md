# Local congestion fixture

`proxy.py` listens only on IPv4 loopback, forwards to a loopback receiver,
and limits publisher-to-receiver bytes after a JSON command. The reverse path
remains unrestricted. It records counters and actual receive-buffer size,
never packet contents. No firewall or route changes are needed.

Run the transport-only check with Python3.8 or later and a new private report path:

```sh
python3 check-proxy.py /absolute/private/directory/result.json
```

This checks exact forwarding, throttling, recovery, an independent connection,
invalid commands and teardown. It does not start OBS or certify RTMP congestion,
encoded media, encoder policy, performance or Windows. The full media scenario
must additionally prove backpressure in the sender; a configured proxy rate
alone is insufficient.

The proxy prints its ephemeral listening port as a `ready` JSON event.
Commands on stdin are `{"rate_bps":4096}`, `{"rate_bps":0}` (unlimited),
`{"snapshot":true}` and `{"stop":true}`. Close stdin after stopping. A limit is
bytes per second, not bits per second. All listeners and child connections
belong to the fixture and are awaited on shutdown.

## Native media scenario

The opt-in `studio-rtmp-congestion-check` module uses a private640×360/15fps
canvas, deterministic moving noise and a sample-clock997Hz tone. Native RTMP A,
production-manager B, recording and replay share one H264/AAC encoder pair.
Primary DBR is explicitly false; additional B requests DBR and verifies the
conservative shared-encoder policy. Healthy5s, B5000bytes/s18s, recovery12s and
all media criteria are fixed in the source before execution.

Build the module with CMake/Qt6/libobs/frontend-api and one job. Run only after
the coordinator reserves a quiet measurement window, using a *new* evidence
root and the existing immutable test prefix. `run-media.py --help` lists the
required prefix/module/server arguments. `--ffmpeg-receiver` uses the provided
FFmpeg executable as a loopback RTMP listener with packet copy into one
fragmentedMP4; it performs no timestamp correction or audio discard. Without
that flag, the server argument is MediaMTX and its own recorder may segment
under deliberate arrival delay. Preserve either result exactly.

Run `analyze-media.py /absolute/evidence/root` once after normal application
exit. It refuses to overwrite an earlier receipt. It requires actual sender
congestion/drops, measured proxy throughput, healthy A and file/replay progress,
encoder identity/policy preservation, complete decoding, monotonic timestamps,
initial keyframe/A/V, initial-window motion and997Hz audio. Configuration and a
Live label alone never satisfy this scenario. The launcher archives its module
and hashes normal configuration and installed binaries before/after.

Linux local execution does not certify Windows, public ingests, physical
devices,60fps or the policy when primary DBR actively adapts the group.
