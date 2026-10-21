# Windows CUE/VOD native probe

`studio-audio-cue-vod-windows-check` is a separate opt-in module. Its Linux build
checks shared C++/Qt/libobs API compatibility and refuses to load there. A Linux
build is not Windows execution. The historical Linux probe, media and thresholds
are retained independently.

The parent controls the Windows app/VM lifecycle, installs only this module for
the run, then removes it before handoff. Build the current fork and its plugins
first. The module needs its real native frontend/Studio controls, `obs-x264`,
AAC/mux and WASAPI plugins. It does not work inside an upstream OBS binary.
Build this target through the existing `test/studio-workspace/CMakeLists.txt`
using the current fork's installed `OBS::libobs` and frontend API packages.

Set these environment variables before starting the isolated portable app:

```powershell
$env:OBS_STUDIO_CUE_VOD_ROOT = 'C:\StudioQA\cue-vod-run-1'
$env:OBS_STUDIO_CUE_VOD_CONFIG_ROOT = 'C:\StudioQA\current-app\config'
$env:OBS_STUDIO_CUE_VOD_SINK = 'EXPLICIT_IMMDEVICE_RENDER_ID'
$env:OBS_STUDIO_CUE_VOD_ENDPOINT_CLASS = 'virtual'
$env:OBS_STUDIO_CUE_VOD_CAPTURE_TOOL = 'C:\StudioQA\wasapi-build\studio-wasapi-loopback.exe'
# Omit OBS_STUDIO_CUE_VOD_MIC when the VM has no actual input.
# OBS_STUDIO_CUE_VOD_PHYSICAL_SINK must be omitted: one explicit endpoint per run.
```

Select the render ID from the helper's `--list-devices` JSON. Confirm that the
OBS monitoring-device enumeration exposes the same ID. The endpoint class is
explicit runner evidence: the VM HDA is `virtual`, not a physical headset test.
`physical` requires a separately identified physical endpoint. Each stage records the same explicit ID/class and verifies the helper ID.
An alternate PHYSICAL_SINK is refused before mutation; a physical endpoint uses
a separate run with SINK/class=physical. No OS defaults or endpoint gains change. Any optional microphone is captured only in local
mix 4; never monitored or streamed.

The effective profile path must be inside CONFIG_ROOT before any mutation.
Every explicitly configured global `[Locations]` entry among Configuration,
SceneCollections, Profiles and PluginManagerSettings must equal that root.
Absent entries are recorded as portable defaults; the actual profile guard
still applies. The isolated fixture must have `BasicWindow.StudioWorkspace=true`
and audio actually running at 48 kHz. The probe duplicates the profile and uses
640×360 at 30 fps for CPU/WARP, verifies those effective values, then restores the
original profile, global audio handles, monitor ID, scene and layout.

The common core exercises MUTE, CUE, monitor only, refused CUE off in ONLY,
Undo/Redo, prepared local VOD exclusion and Save/Load source persistence. Four
independent AAC encoders use mixes 0–3. No public stream is started. The fourth
mix stays silent when no microphone was explicitly supplied; that case is
recorded as SKIP, not physical-input PASS. Source persistence here is native
Save/Load, not an app-restart certification.

The Windows source also emits 32×18 RGBA async video frames. This is deliberate:
WASAPI calls `process_audio_delay` only for sources with video and without async
decoupling. Audio-only sources do not enter that branch. Two extra stages monitor
both the 997 Hz pulse and a separate 2003 Hz reference pulse. This measures the
relative CUE change for requested offsets 0/250 ms in the same endpoint WAV.
The common program PCM/AAC offset criteria stay ±40 ms. For the Windows A/V CUE
branch, the declared change bound is 100–350 ms, accounting for its 75 ms release
window, 30 fps cadence and endpoint queue. It is a digital routing measurement,
not acoustic or exact clock latency. A permanent 3037 Hz pilot stays MONITOR_ONLY with mix mask zero, independently
of the music CUE controls. Every endpoint WAV must contain this pilot and every
program PCM/AAC mix must exclude it. The helper must report at least one second
of actual received and written frames in **every** case, including negative
music controls. Missing endpoint data or an all-padding WAV cannot satisfy a
negative-control PASS. Source peaks are music .010, reference .006 and pilot
.003; the conservative combined mono-monitor bound is below −30 dBFS.

The VM run has 15 stages when microphone is omitted. An explicit input adds one
stage. `runtime.json` contains Windows-target flags, endpoint class/ID, per-stage helper
JSON, skips and effective state. All WAV/PCM/MKV files stay in private ROOT. Copy
artifacts to a private analysis directory if analysis runs on the Linux host:

```bash
python3 tools/verify-audio-cue-vod-windows.py PRIVATE_ROOT --report PRIVATE_ROOT/media-validation.json
```

The analyzer requires ffmpeg/ffprobe and NumPy, fully decodes four AAC tracks,
checks presence/absence of 431/997/2003 Hz in every program mix and endpoint,
measures program and CUE pulse delays, and preserves hardware/microphone skips.
Q_OS_WIN identifies the compile target; it cannot distinguish Wine from a native
VM. The analyzer records Windows-target artifacts and explicitly leaves native
VM verification to the parent's separate VM/session/binary provenance evidence.
The analysis host OS is recorded separately.
A successful virtual endpoint test does not close real Windows USB/headset,
physical input, hotplug, long soak or public VOD-ingest requirements.

Ownership and history regression coverage: owning source getters and encoder
creates use AutoRelease arrays; global-source snapshots are released after
restoration. The BOTH→NONE CUE transition is exercised by an actual OFF click,
then Undo/Redo and an ON click return it to BOTH before measuring. This is in
addition to ON/Undo/Redo and the refused ONLY-off gesture.

Teardown keeps the synthetic feed running while the private recording performs
a normal stop. Only after the output becomes inactive does it stop the generator
and release sources/encoders. A 15-second normal-stop deadline is always a
fixture failure; it records `forced_owned_output_stop`, forces only its own
started recording, then waits at most 10 more seconds. Forced cleanup never
changes that failure to PASS. If the output remains active, it retains fixture
resources and reports failure rather than destroying an active output.

The synthetic source now runs on its own worker instead of the GUI timer. Its
20 ms blocks share one monotonic sample clock across all four audio/video
sources, with exact oscillator/pulse phase continuity. It can catch up at most
three consecutive overdue blocks; lateness above 100 ms is a fixture failure,
not an excuse to drop, retimestamp or pad samples. Per-stage begin/end and final
metrics include frames, blocks, lateness, catch-ups, dispatch cost and backlog
failure. The worker stays alive through output stop and is joined before source
removal; frontend exit/module unload also joins. Presence/absence and delay
acceptance thresholds are unchanged. Historical run2 remains a media failure.

Failure/shutdown boundary: `SCRIPTING_SHUTDOWN` runs before frontend scene
clearing. It quiesces the fixture producer, checks owned outputs are inactive
and releases retained fixture/source/service/global snapshot references before
libobs shutdown. It never restores profiles/UI or stops unrelated outputs during
closing. A separate `shutdown.json` records idleness/reference release without
rewriting or approving the runtime result; still-active outputs retain resources.
