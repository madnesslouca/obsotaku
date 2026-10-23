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
other platform build. Those hashes identify the previous media-validation baseline; the credential
frontend candidate below has a separate build and acceptance scope.

## Remaining work and validation

- Resolve the Windows WARP movement failure and repeat the same media criteria.
- Complete the Windows distribution with browser, OAuth, virtual camera and
  third-party VST3 plugins; test real capture/audio devices on both platforms.
- Validate sustained 60 fps capture, long-duration stress and public-service VOD.
- Complete distribution and public-account validation; native credential UI has
  isolated Linux and Windows evidence and does not certify real provider accounts.
- Resolve scene/group deactivation while its video lock is held. A directed
  headless diagnostic reproduced a 400 ms global PCM gap during a queued group
  hide; the earlier root traversal does not cover activation reference walks.
  That diagnostic did not use a GPU, physical device or normal application.
- Bind a staged restore to its original local roots before applying or rolling
  back files. A real archive test on temporary A/B trees reproduced destination
  redirection after changing only the scene root, including a rollback that
  wrote A's original bytes into B. No normal configuration was involved; the
  root-binding correction is separate from credential-session guards. A separate
  same-root test also reproduced rollback replacing a legitimate post-staging
  edit with older bytes; fresh recovery at application time remains required.
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


## Credential and destination follow-up

Linux now uses the desktop Secret Service through libsecret instead of the
unsupported placeholder. Windows retains Credential Manager. Other platforms
fail closed. Keys remain outside the application INI; there is no plaintext
fallback. Linux builds require libsecret development headers (version 0.18 or
newer) and pkg-config. Runtime access requires an available desktop credential
service. Container and Flatpak integration have not been certified.

Destination metadata is available without credential I/O. Credential reads and
transactions run on a lazy serial worker with cancellation and a shared local
deadline. Backups precede mutations; the GUI revalidates profile, configuration
revision and active outputs before committing metadata. Failed or canceled writes
reconcile with a fresh bounded context. Incomplete reconciliation is reported.
These steps do not provide crash atomicity across separate services.

Cards and the persisted selection remain visible while credentials are pending,
locked or unavailable; runtime availability is reported separately. Manual
destination keys reopen masked. Account drafts and local registration prompts
now validate the profile and metadata captured before opening the dialog. An
external change rejects an old draft; a matching successful registration
subtransaction may authorize its own updated revision.

Linux credential validation uses isolated D-Bus services/keyrings and synthetic
values, with no normal account access. Separate scopes passed 102 backend
criteria, 75 account-removal lease criteria, 48 transaction criteria and 17 account
dialog criteria. Native add/save/restart/edit and failure presentation passed 33
criteria on an earlier candidate. The modal follow-up passed 18 offscreen Qt
cases with 150 criteria; native follow-up passed 49 Accounts criteria and 14 manual add/restart/edit
criteria with frontend SHA256
`56ebad406275c6aa66ac64dd26de82951427cf7debae151bac84ed13efbc65e3`.
A subsequent Add/Remove correction passed the same 53 native criteria that
previously produced 27 failures. Valid Add/Edit/Remove/restart controls passed
25 criteria, and unchanged Accounts controls passed seven. The versioned native
actor repeated those 53 criteria without adding distinct coverage. Frontend
SHA256 for this stage was
`ab24a6558f8e7f8c98f59aa26a2eef6a32e5cb90b6b9348c2d6fa896a3ed8b13`.
All native credential stages preserved 126 normal configuration/keyring files.

Windows standalone Credential Manager checks passed 31 criteria in the intended
desktop logon. The same helper first failed 8 criteria under a scheduler token;
a direct native write also returned 1312. Matching user and SessionId alone did
not establish a usable credential logon. Native frontend follow-up then passed 44 manual criteria, 49 Accounts criteria
and 53 Add/Remove modal criteria across 18 executions, using frontend SHA256
`e71c75c653fa718c03e2ad82cd86ae02b220a3024a1a060f01403edce5f61554`.
Seventeen child logon identities and exit-zero statuses were verified. The first
Add passed nine actor criteria but its earlier wrapper lost the exit status;
that limitation remains recorded, and later restarts verified its persisted
credential without repeating Add. The tested edit path reopened the masked key
and rejected an oversized replacement, preserving the original credential.
A separate 20-criterion native extension then saved a valid new key/name and
reopened it after restart, preserving the original UUID/server and disabled
intent. Its 21 setup/cleanup criteria overlap earlier scenarios and are reported
separately. Both native acceptance records remain immutable.
Exact synthetic targets were removed, probes archived and original configuration,
runtime, locale and source files preserved. These tests exercised the credential
interface, not Cloud, public OAuth, capture or simultaneous media outputs.

Optional Drive HTTP operations observe operation/shutdown cancellation and report
a canceled remote result as unconfirmed. Real libcurl tests against loopback
passed 42 criteria; the offscreen backup flow passed 58. Token-refresh form
requests finish outside that cancellation scope so durable credential rotation
is not abandoned. DNS, refresh and local archive work do not have a universal
shutdown deadline. No Google account or public API was exercised.

Cloud account changes now keep a worker-owned credential lease through the GUI's
durable metadata decision. Stale confirmations and replaced sessions are rejected;
canceled mutations recover the prior raw value, while an unexpected external
replacement is preserved. Remote calls retain the same session across history
pages, upload phases and retention deletion. Scheduler intent is pinned before
enqueueing; settings drafts retain their own revision. Busy history double-clicks
cannot start a second restore, and expired authorization work does not begin a
new code exchange. Registration safely handles unavailable application/config
state while retaining configured environment/build precedence.

Linux adapter validation retained the original failure cases and passed the
expanded 111 modal, 58 backup-flow (48 original), 50 credential-transaction,
65 session, 23 settings, six registration and 75 account-removal criteria.
Directed suites separately exercised queued authorization/shutdown and source
session changes during restore options. Counts overlap and are not a unique
coverage total. All provider, credential, archive and browser dependencies in
these workflow suites were synthetic. The full Linux frontend compiled/linked;
a separate native startup/GUI-close smoke preserved 126 normal files. Native
Windows workflow tests passed the same frozen-source criteria under native Qt
qminimal: 16 cases, 433 counted checks and two registration-null booleans. Their
HTTP, credential, archive and browser dependencies were synthetic; this is not
a full visible Cloud interface or real-service certification. An incremental
Windows frontend build recompiled all 16 expected dependent units and linked
SHA256 `9fa1ab270e007f00a8f36532bddf27bc34e92f5ac041ac68242f8fc909a41b73`;
core/API binaries remained unchanged. Earlier native credential-interface tests
remain scoped to their original e71c frontend. Archive-root binding and staging
publication remain separate from these session guards.

## Selective congestion validation on Linux

A real loopback test passed 781 native assertions and 76 media/transport criteria.
A single x264 2500 kbps/AAC 128 kbps encoder pair served two Main RTMP destinations, a
recording and a replay buffer at 640×360/15 fps. Destination B was limited to
5000 bytes/s for 18 seconds (measured 4999.98 bytes/s), accumulated 130 dropped
frames and reached congestion 1. Destination A kept one receiver session and
continued progressing, as did recording/replay. B recovered after the limit
was removed. Five complete files passed decode, 997 Hz audio, initial keyframe,
PTS/DTS, initial A/V difference ≤75 ms and the unchanged 85% startup movement
requirement. Two clips were saved during the combined run.

Primary bitrate adaptation was explicitly disabled; the additional destination
requested adaptation but remained protected by shared-encoder authority. This
is evidence for that fixed-policy scenario, not coordinated adaptive bitrate,
a landscape/portrait test, Windows, 60 fps, physical capture or public services.
Earlier fixture failures remain preserved. An automatically segmenting receiver
created two files with excessive initial A/V offsets despite a continuous
connection; the final acceptance used an integral copy receiver with the same
media criteria and no corrective remux or audio discard. Three generic leaked
objects were reported at shutdown without an isolated cause; no leak-free claim
is made.
