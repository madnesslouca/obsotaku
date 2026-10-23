# Studio workspace — implementation and validation

This fork extends OBS with a Studio workspace built on Qt and libobs. OBS
licensing, upstream contributor credits and third-party notices remain intact.
The shared implementation targets Windows and Linux. The results below certify
only the executed scenarios; they do not establish a complete release on either
platform.

## Included functionality

- Main and Multi Canvas switching, portrait layers and inspector, direct
  selection, transforms, crop, group editing and Undo/Redo.
- Canvas and encoder settings per output, landscape/portrait recording, two
  replay buffers, clips and multiple streaming destinations.
- Mixer gain, mute, monitoring/CUE, effects, per-scene audio and recording/VOD
  routing. Optional VST3 hosting stores plugin state; compatible plugins must be
  installed for the target operating system.
- Unified chat with platform events and emotes, browser sources and widgets.
- Portable tests and local media validators under `test/` and `tools/`.

## Changes in this snapshot

Audio root discovery uses source references in immutable snapshots rather than
waiting for view channels or video mixes during rendering. Scene/group traversal
collects child references under the audio lock and recurses after releasing it.
Registry cleanup completes before plugin type-data teardown. Tests cover PCM,
removed/shared roots, resets and the order of source destruction.

Startup packet pruning now removes predictive frames before the first retained
keyframe on each video track. Payload ownership and packet timing entries remain
paired. Directed tests exercised 22 cases and 214 criteria, including two video
tracks, two AAC tracks, reordered presentation timestamps, delay ownership and
reset. This does not certify every independent multi-video GOP arrangement.

Additional streaming outputs leave encoder bitrate adaptation to the primary
output, preserving the configured preference and avoiding competing controllers.
The interface describes this as managed bitrate and explains that other outputs
can change when an encoder is reused. This is a conservative ownership policy,
not a congestion benchmark or a promise of fixed bitrate.

The Outputs dock gives delivery status its own wrapped line. Native Linux checks
confirmed readable status at a 280-pixel dock width with no horizontal scrolling;
canvas selection stayed locked while live and Edit remained accessible. Very long
names without spaces can still clip; their full text is available in the tooltip.

## Validation completed on 2026-10-23

All runs used isolated configurations, synthetic content and local receivers.
The simultaneous-output scenario used Main 640×360 and Portrait 1080×1920 at
**15 fps**, software x264 ultrafast, and 997 Hz audio. Two RTMP destinations, two
recordings and two replay buffers ran together. One receiver was interrupted to
exercise independent recovery. Four clips, two recordings and three received
segments produced nine media files per platform.

| Environment | Executed checks | Result and limits |
| --- | --- | --- |
| Linux, native GPU | 437 combined-output assertions; nine full media decodes | PASS: all nine met the movement requirement, with 100% unique frames in the sampled windows |
| Linux, native GPU | 96 PTS/DTS criteria; 27 initial-keyframe/A/V criteria | PASS: maximum initial A/V PTS difference 21 ms |
| Windows VM, WARP | 331 combined-output assertions; nine full media decodes | Runtime PASS; movement acceptance incomplete: eight files passed and one failed |
| Windows VM, WARP | 96 PTS/DTS criteria; 27 initial-keyframe/A/V criteria | PASS: maximum initial A/V PTS difference 21 ms |
| Linux | 540 CUE/VOD assertions over 12 virtual stages; 332 media and 14 producer-clock criteria | PASS: PCM/AAC routing, own virtual monitoring endpoint and 250 ms requested delay; physical stages omitted |
| Linux | 75 active archive checks | PASS: local RTMP program/archive encoder routing, including silent input |
| Windows VM | 711 CUE/VOD assertions over 15 stages; four decoded AAC tracks and PCM | PASS: virtual WASAPI/HDA endpoint, 250 ms source delay and 160 ms CUE buffering |
| Windows VM | 79 active archive checks | PASS: local RTMP program/archive routing, including silent input |

All nine Windows files decoded fully, contained 997 Hz audio and retained the
15 fps timestamp grid. The failing Main RTMP movement window, from 0.3 to 1.8
seconds, contained only one unique frame among 22 sampled frames (4.55%), below
the unchanged 85% requirement. Therefore **Windows simultaneous-output media
acceptance remains incomplete**, despite passing file, audio and timing checks.
A valid file and regular timestamps do not prove that rendered content advances.

The Windows VM used Windows 10 Pro 22H2, MSVC, Qt/libobs, WARP graphics and virtual
HDA audio. Its validation package omitted browser, virtual camera, scripting and
OAuth. It is not a complete Windows distribution. Monitoring measurements used
digital endpoint capture, not listening tests or physical hardware latency.

The installed Linux validation core had SHA256
`6f1580cb0565d3a04f433c4dcd724e8ef4a14ce414739af11a2a3e16ea68192e`.
The frontend had SHA256
`ab32ce6a4748ca2e93626f26ecff365c7295be1dd67f39d542aad250c5ccc841`.
Installed binary hashes include platform linkage/RPATH and do not identify every
other platform build. No credential-storage work still in progress is included
in this snapshot.

## Remaining work and validation

- Resolve the Windows WARP movement failure and repeat the same media criteria.
- Complete the Windows distribution with browser, OAuth, virtual camera and
  third-party VST3 plugins; test real capture/audio devices on both platforms.
- Validate sustained 60 fps capture, long-duration stress and public-service VOD.
- Finish and validate native Linux credential persistence; in-memory local RTMP
  tests do not certify saving an account or stream key through the interface.
- Reproduce scene/group deactivation while rendering is blocked. The private
  audio traversal does not remove every lock used by activation or plugins.
- Measure acoustic monitoring quality and hardware latency independently.

The audio traversal acquires/releases child references per block; scratch storage
can grow when the graph grows. Snapshot acquire/release does not allocate, but
the entire audio callback is not universally allocation-free. Removed sources
can retain registry references until the relevant view changes or is destroyed,
although removed/inactive roots are excluded from audible mixing. No broad
performance percentage or general race/leak-free claim is made.

## Build and test notes

Initialize upstream submodules recursively and follow the OBS build instructions
for the target platform. Retain all dependency licenses and notices. The optional
VST3 module requires a compatible SDK; `tools/fetch-vst3-sdk.py` verifies pinned
revisions and preserves an existing modified SDK.

Linux launch helpers accept an isolated installation/configuration through
`OBS_STUDIO_PREFIX` and `OBS_STUDIO_CONFIG_HOME`; Windows uses
`tools/run-studio-windows.ps1`. Use fresh test configurations and local receivers.
Follow each fixture's setup, producer-clock and teardown requirements, and remove
opt-in QA modules before returning to normal use. Generated reports belong in
ignored `validation/` directories. Executing one test harness does not transfer
acceptance to untested devices, services, presets or package configurations.
