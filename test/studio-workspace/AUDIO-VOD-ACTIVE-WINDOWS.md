# Windows-target active RTMP archive and Audio Dock Undo probe

This opt-in fixture is separate from the historical Linux 72-check probe and
from the Windows CUE media probe. Target: `studio-audio-vod-active-windows-check`.
The current fork, its Studio frontend, x264/AAC/RTMP plugins and the standalone
Windows test CMake project maintained by the test operator must be built first. An API
compatibility build on Linux refuses to execute and is not Windows acceptance.

The test operator exclusively controls the VM, installs only this module
for this run and removes it before normal handoff. Do not run alongside the CUE
probe. Use a fresh private result directory and isolated portable configuration:

```powershell
$env:OBS_STUDIO_ACTIVE_VOD_WINDOWS_ROOT = 'C:\StudioQA\active-vod-run-1'
$env:OBS_STUDIO_ACTIVE_VOD_WINDOWS_CONFIG_ROOT = 'C:\StudioQA\current-app\config'
$env:OBS_STUDIO_ACTIVE_VOD_WINDOWS_SERVER = 'C:\StudioQA\tools\mediamtx.exe'
```

The effective profile must lie inside CONFIG_ROOT. Every explicitly configured
`[Locations]` entry (Configuration, SceneCollections, Profiles,
PluginManagerSettings) must equal CONFIG_ROOT; absent portable defaults are
recorded separately. `BasicWindow.StudioWorkspace=true` and audio actually at
48 kHz are preconditions. The parent must seed the private profile in Advanced
mode **before starting OBS**. While the isolated app is closed, its `basic.ini`
must contain these test values (alongside the existing private General/Video/Audio
seed):

```ini
[Output]
Mode=Advanced

[AdvOut]
Encoder=obs_x264
AudioEncoder=ffmpeg_aac
RecType=Standard
RecEncoder=none
RecAudioEncoder=none
RecFormat2=mkv
TrackIndex=1
VodTrackIndex=2
VodTrackEnabled=true
```

Set `Track1Bitrate` through `Track6Bitrate` to 160 in that same AdvOut section.
OBS supplies the remaining profile defaults during normal startup; serialized
empty/missing values must not be mistaken for the effective in-memory defaults.
The probe checks Advanced before marking the fixture ready and again after
duplication. It does not open Settings or switch the output mode through a timer.
The decisive backend check requires the active RTMP video's encoder name to be
`advanced_video_stream`, alongside the actual program/archive audio slots.
No encoder is assumed to be bound to an idle output before SetupStreaming.

The temporary duplicate profile uses an effective
640×360, 30 fps video clock for CPU/WARP and a private ultrafast x264 stream.
The probe verifies the actual clocks, clears/snapshots global audio handles and
never creates a physical input or monitoring capture. Owning source getters use
AutoRelease; source snapshots are released after restoring their original slots.

Coverage:

1. Native Audio Dock gain edit, then external mute/monitor/mix/delay changes.
   Undo/Redo must change gain only.
2. Native routing-cell edit, then an unrelated external routing bit and source
   fields. Undo/Redo must change the edited bit only.
3. Configure a transient destination absent from stored account metadata.
   Prepared VOD UI must follow the manager's actual in-memory list.
4. Start the real frontend RTMP handler against an allocated IPv4 loopback port.
   Require program encoder slot 0/mixer 0 and archive slot 1/mixer 1, active
   delivery, and an effective VOD target in the UI.
5. Change the pending VOD preference to track 4. The active encoder and exclusion
   target must stay mixer 1. Exclusion and Undo must preserve live/record mixes.
6. Stop outputs and the owned receiver; restore profile, service, global audio
   handles, scene, layout and user flags, then remove the QA profile.

The MediaMTX receiver uses the private ROOT as CWD and binds only RTMP
`127.0.0.1:<allocated-port>`. RTSP/HLS/WebRTC/SRT/MoQ/playback/API/metrics/pprof are
explicitly disabled. A readiness poll verifies the local listener before Go Live.
It publishes synthetic silence only; no account store write, physical device or
public endpoint is used. `receiver.log` records listener/start/stop information.
The whole fixture has a 45-second deadline and outputs progress/runtime JSON.

MediaMTX Windows amd64 v1.21.2 is cached privately at
`PRIVATE_TOOLS/windows-v1.21.2/mediamtx.exe` on the host.
The zip SHA256 is
`a84224f5a490855684e62ba6fb71e80ea2a5e16aed61883bb58380290e93e467`;
executable SHA256 is
`31bc3cc425a0278c7a5696c7ba309257235060157eb338eed20bb88dbe0faf87`.
The download matched the official release checksum file and GitHub API asset
digests. Full provenance is in that directory's `download-verification.json`.
This is HTTPS/official checksum verification, not a detached-signature claim.
The Windows binary was not installed or executed on the Linux host.

Analyze the resulting JSON after the parent copies it to a private host directory:

```bash
python3 tools/verify-active-vod-windows.py PRIVATE_ROOT --report PRIVATE_ROOT/validation.json
```

The module records Windows-target metadata. Q_OS_WIN does not establish native
VM provenance; the parent must attach VM/session, Windows binary and execution
evidence separately. This checks actual local archive encoder binding and UI
routing, not published Twitch VOD content, SRT multitrack or restart persistence.

Teardown keeps the synthetic source running until the native output and frontend
streaming session are inactive. A 15-second stop deadline remains a failure.
Only an owning output handle observed active after the fixture's own start may
receive the forced cleanup fallback; it then waits at most 10 more seconds.
Forced cleanup never becomes PASS. If any session remains active, the fixture
retains its resources and reports failure. Linux historical probes are unchanged.

Ownership guard: a failure before private-fixture readiness or before its own
start gesture never stops another streaming session. The post-gesture output
handle is retained even before it becomes active; only an observed-active owned
handle is eligible for stop. Restore waits for that handle, any current native
output and frontend/manager session to become idle. A start still pending without
a terminal lifecycle event times out with retained resources instead of changing
the profile underneath an asynchronous setup.

A private `color_source_v3`640×360 background gives this otherwise audio-only
scene an actual video source. This satisfies native no-video validation without
accepting generic dialogs or changing production validation. The background is
removed with the fixture. The prior run blocked by NoSources remains a failure.

Failure/shutdown boundary: `SCRIPTING_SHUTDOWN` runs before frontend scene
clearing. It quiesces the fixture producer, checks owned outputs are inactive
and releases retained fixture/source/service/global snapshot references before
libobs shutdown. It never restores profiles/UI or stops unrelated outputs during
closing. A separate `shutdown.json` records idleness/reference release without
rewriting or approving the runtime result; still-active outputs retain resources.

Pending-start cancellation is proved by an observed native `StreamingPreparing`
signal, not by idle handles. Both direct signal connections are verified before
the direct Go Live gesture. Only after that gesture returns, with the observer
intact and no Preparing signal, may the probe conclude that StartStreaming never
launched its asynchronous setup. The observer disconnects on return; shutdown
invalidates incomplete observation. Shutdown idleness also requires no pending
start. A cancelled pre-start dialog can resume safe cleanup after an earlier
timeout while preserving its failure, provided shutdown has not begun.

Changing output mode through the Settings dialog is outside this Windows
fixture's acceptance scope; the historical Linux fixture and evidence are unchanged.

The failed Windows ACTIVE run2 remains a failure. A fixed 300 ms Settings timer
ran before the dialog was modal, reported failure and restored/deleted the QA
profile while the Settings constructor remained on the stack. The later crash
was in `LoadAdvOutputAudioSettings`; this does not by itself distinguish stale
raw config pointers from missing effective defaults. Removing that setup step
prevents this fixture from restoring a profile during its own Settings
construction. The probe also never auto-closes an unrelated Settings dialog.
