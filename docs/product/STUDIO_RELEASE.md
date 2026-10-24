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
- Extend the active-child accounting checks to third-party plugin compatibility,
  long-running graphs and physical capture. The directed group-hide PCM defect
  is corrected and has separate native Linux/Windows acceptance below.
- Complete real Drive-account validation, plugin-settings restore coverage and
  a user-facing failed-restore recovery workflow. Native Windows restore/restart
  acceptance is recorded below with its limited scope.
  Credential backup/restore is temporarily disabled pending separate vault
  isolation validation; ordinary file backup/restore remains available.
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

## Restore destination and recovery follow-up

Backup and restore operations now receive immutable local storage roots captured
on the GUI thread. Worker/startup checks bind those roots to native filesystem
identities, reject symbolic links/reparse points and duplicate effective targets,
and coordinate operations with a per-user process lock. Imported archive paths
cannot select absolute local destinations. A missing root has an explicit
ancestor/relative-path contract; clearing the volatile cache does not invalidate
an otherwise matching pending restore.

Restoring global settings also requires a readable local global.ini so its
existing Locations section can be preserved. If it is absent or unreadable,
staging refuses the import, keeps the previous pending restore and explains how
to repair/reopen the local configuration. It neither imports another machine's
paths nor silently selects default roots. A transport that excludes global.ini
can still restore other settings under its separate checks.

Each attempt owns a separate staging directory. A validated pending pointer is
published atomically; failed or canceled staging preserves an existing pending
restore. Before the first live write, application creates a fresh recovery copy
and journal from the current destination bytes. A failed application preserves
those recovery files and refuses silent automatic repetition. Payload integrity
is checked against the exact byte array consumed by each write. Successful
application removes the pending pointer before cleaning the staging directory.

Credential export and restoration currently fail closed before credential access
or changes to live configuration, with disabled controls and an explanation in the interface.
This restriction is separate from support for reading encrypted transport files:
Windows native tests exercised real encryption/decryption, wrong-passphrase
rejection and file-only restore without accessing the credential store.

Linux V3.1 real-Archive tests passed 172 criteria, including changed-root rejection,
fresh rollback after a post-preflight failure, incomplete rollback reporting,
payload tampering, interprocess locking and canceled staging. An independent
supplement passed 52 contract criteria plus 106 preconditions over ten scenarios.
Windows native V3.1 real-Archive tests passed 170 criteria across basic, actual
sharing-violation rollback and paths/encryption/process-lock suites. Setup checks
and overlapping cases are counted within these scoped totals.

The V3.2 Linux frontend was rebuilt (SHA256
`5304b53ab8b2e726b1524e8cc72ae1b4dc99594b623ff4edea34d96f48cb8b31`). A separate
native interface roundtrip passed 95 phase checks: import through the real file
picker and options, staging, automatic restart, startup application and another
restart without reapplication. Main/Portrait graph UUIDs, sources, transforms,
colors, dimensions and settings matched the saved project. Its normal 126 files
and Pulse device/default/mute/volume state were preserved. This verifies project
structure and settings; widget screenshots do not certify rendered pixels.
Cloud fake-backend adapters separately retained 433 checks plus two null-config
booleans and added 18 root/options criteria; they do not validate real Archive.

These accepted results use temporary configurations and synthetic projects;
invalid fixture attempts are excluded from their counts. They do not validate
public providers, normal credentials, physical capture or concurrent-media restore. The path checks do not provide hostile-filesystem check/open atomicity,
crash-atomic multi-file recovery or protection against native identifier reuse.
Legitimate aliases can be refused conservatively. Failed recovery remains
explicit and requires manual handling; no recovery wizard has been added.
The earlier V3.2 Linux deployment updated the frontend and two locale files; core, API and
plugins retain the earlier media baseline. Installed frontend startup passed six
separate empty-profile criteria with relative RPATH and no library-path override.
That V3.2 installed frontend SHA256 was
`027af42c9aa4db6cc206d131b91970de6d47efd970b51bd6499b0e7edddcbaa8`;
it is superseded by the V3.3 deployment below.
No normal project was opened, and normal configuration/Pulse state was preserved.
This deployment does not certify new media or real-account behavior.

A focused real-Archive regression was added under `test/studio-archive-roots`.
Its Linux before run failed three of 50 checks with all preconditions satisfied;
the fail-closed correction passed the same 50 checks (37 preconditions and 13
contract assertions). It verifies rejection and preservation with a missing
local global file, custom local Locations with a readable file, and a transport
that does not include global.ini. Counts overlap the broader restore suites.

Windows native validation repeated the 170 basic/sharing/path criteria against
the final readability-guard source and passed the additional focused 50 checks,
for 220 scoped checks (138 preconditions and 82 contract criteria). The earlier
V3.1 results remain separate. The Linux 50-check target also built and passed
from its canonical test directory with the default source-root configuration;
that repeat validates fixture integration and adds no distinct behavioral scope.

## Portable configuration path follow-up

Native Windows startup testing exposed a capture-boundary issue that the
absolute-path Archive fixtures did not cover: portable configuration paths were
still relative when strict root validation ran. Capture now converts nonempty
local paths to normalized absolute paths lexically, using the process working
directory. Empty bases, unavailable App state and embedded NUL remain invalid;
worker identity, alias and locking checks remain strict. A captured snapshot
continues targeting the same directories after the working directory changes.

The expanded Qt Core fixture retains the original 50 controls. Before this
correction, relative capture failed after 86 checks; 34 dependent checks were
not reached. The corrected source passed all 120 checks on Linux, including
relative paths, invalid inputs, current-context rejection after CWD changes and
Create/Stage/Apply using the original snapshot. Windows native tests passed the
same 120 plus the 170 basic/sharing/path checks, totaling 290 scoped criteria.
Earlier V3.1/V3.2 evidence remains attached to its original source snapshots.
The earlier Windows frontend failed startup and its post-dismissal actor result
was excluded from acceptance; a successful algorithm build did not certify that
application boundary. Concurrent changes to process CWD during capture have not
been validated; no guarantee of atomic multi-field CWD capture is made.

The final V3.3 Linux frontend (build SHA256
`0c27ebff4da285ac30201153245cf7451844bef8bb02feb1632a82740cb89155`)
passed a new 95-phase native import/stage/automatic-restart/restore/restart run,
including the repeated 30-check verification. Its source and transport helper
both used the final capture implementation. Graph, UUIDs, settings and normal
126 files/Pulse state were preserved; no rendered-pixel or media acceptance is
assigned to those widget/structure checks.

Only the installed executable was then replaced, retaining its prior backup;
the other 1810 installed files, including core, API, plugins and locales, retained
bytes and modes. The installed V3.3 frontend SHA256 is
`647407ef2dfb5dc065916166878c383270e1cb7b9f58bbc9f3cf8b214a63f336`.
Six isolated startup/GUI-close criteria passed without a library-path override,
with the real relative RPATH, private config/bus/audio endpoint and PID/starttime
identity checks. Repeated observations cover the same six criteria. An earlier
observer failure was excluded; its exact cause was not inferred without captured
environment bytes. No normal project was opened and the application was left
closed without test modules or streams.

The final native Windows frontend linked SHA256
`5a49793307f76993a748e77aeb05979594db579578c92a79938cb6a302080759`.
Its last incremental build changed only the Archive translation unit after the
preceding four-unit integration build; core/API remained unchanged. A fresh
portable empty-profile startup passed four scoped criteria, exited normally and
produced no restore-startup error, with no dialog dismissal or bypass. The GUI
heartbeat's maximum observed gap was 33 ms. This is a startup boundary check,
not a new credential-store, full restore-interface or media acceptance. Original
configuration/runtime, locales and source files were preserved and test modules
removed from loading paths. Windows still requires full distribution and real
provider/hardware validation; the earlier movement failure remains open.


## Active-child accounting and output lifetime follow-up

Hiding a group no longer traverses plugin enumeration while adjusting activation
counts. Active relationships own child references and propagate credits in
O(V+E), preserving duplicate paths and MAIN/AUX roots. The complete affected
subgraph is validated before publication; invalid credits reject the operation
without partial changes. Real group insertion, reparenting, reorder and ungroup
transfer accounting before changing visual parent links. Generic preflight
runs before the scene's own audio locks.

Plugins must balance successful `obs_source_add_active_child` calls with removal,
including relationships created while a parent is inactive. Enumeration alone
does not register activation. The bundled slideshows now remove replaced child
relationships before releasing their transition. Slideshow mk2 also acquires a
strong transition snapshot under a short mutex; callbacks and releases occur
outside it. Empty-list Next/Previous behavior remains unchanged.

Stopping a non-final or absent callback no longer clears buffered composition
timing for another consumer of the same encoder. Only the final callback removal
clears that history after producer disconnection. This small correction is
independent of the pending output-start transaction work.

| Native scenario | Linux | Windows VM |
| --- | --- | --- |
| Shared encoder timing, buffered synthetic codec | 35 checks: four failures before, none after | Same 35 checks and four reproduced failures, none after |
| Active graph / real scene operations / lifecycle | 43 / 21 / 15, all pass | 43 / 21 / 15, all pass |
| Group-hide PCM / independent-root contention | 23 / 24, all pass | 23 / 24, all pass |
| Overflow and deep chain / preflight / transition lifecycle | 23 / 8 / 60, all pass | 23 / 8 / 60, all pass |
| Slideshow ownership | 49 pass | 49 pass |
| Slideshow snapshot lifetime | 38 checks: ten failures before, none after | Same 38 checks: ten failures before, none after |

The native counts overlap and are not unique feature coverage. Graph tests use
synthetic sources and PCM. The transition test uses real core and Cut paths, and
a Fade translation unit with shader creation replaced; it does not certify
rendered fades, GPU effects, Stinger or PCM crossfades. Timing observations are
not hard-real-time guarantees. A layered 51-node graph no longer expands its
millions of paths under the accounting gate, but generic inactive plugin
enumeration outside that gate can still take hundreds of milliseconds.

The clean installed Linux core is SHA256
`67ce155ffe6f3bf9ee8b4901fe0901a3a766b633bae5225351a8743cbb546f05`;
image-source is
`c172616be6e9c92cf0730d67eb36a079cc1b51a60599362af351a505ca98dbcc`.
Previous binaries were preserved. The frontend remains `647407ef` and the API
remains `1ce64049`. Only these two runtime files changed; no test adapters were
installed. Linkage adjustment changed only `.dynstr` compared with built files.

This installed combination passed a new isolated simultaneous-output run:
434 executed assertions, nine fully decoded moving-video/997 Hz media files,
96 PTS/DTS checks and 27 first-keyframe/initial-A/V checks. It uses the same
15 fps Main/Portrait dimensions, local receivers and unchanged 85% movement
criterion as above; all sampled movement windows contained 100% distinct frames.
The immutable module performed three fewer repeated wait-poll assertions than
the historical 437-check run, with identical distinct assertions and deadlines.
One destination was interrupted while the other continued. Normal 126 files,
Pulse device/default/mute/volume state and the other 1,809 installed files were
preserved. The process closed normally and its opt-in module was archived.
No physical device, public service or sustained 60 fps behavior was certified.

The separate native Windows restore workflow completed 96 phase checks: seed15,
import21, verify30 and repeated verify30. Preferences, profiles, scenes and
canvases were exercised across actual restart/restore; plugin settings remained
unselected. Google API, credential transport and crash-atomic recovery remain
outside that acceptance. These restore checks do not certify media behavior.

The earlier Windows Main RTMP movement failure remains open. Passing native
accounting, lifetime and timing checks does not constitute another successful
Windows six-output media run or a complete Windows distribution.


## Capture-start refusal and transport ownership

RTMP, FFmpeg recording and replay now propagate an explicit refusal from
`obs_output_begin_data_capture` instead of retaining provisional resources or
reporting a successful start. The recording caller retires only its own helper
pipe; replay retains its existing packet ring and last saved path.

An RTMP sender that exits while initialization is still running transfers its
resources to the connection owner and stays joinable. It never waits for the
begin callback to return. Cleanup and terminal signaling have one owner; startup
failure does not use `end_data_capture` as a generic rollback. Reentrant Stop
also avoids joining the calling connection thread while preserving joins from
other threads.

The portable checks compile actual plugin translation units. Begin/preflight,
metadata and protocol operations are injected; native workers, synchronization,
loopback sockets and helper processes remain real. Negative controls preserve
the original startup-refusal and early-worker failures. Windows secondary-worker
checks use a controlled worker, not the complete socket backend. These checks
do not certify an RTMP handshake, core attach rollback, arbitrary concurrent
start/destruction or a generation-aware output transaction.

Native Linux and Windows checks passed, including the27-criterion mux/replay
suite and all eight platform-specific RTMP scenarios. Controls reproduce the
original failures and the early candidate's incomplete cleanup; the successful
path retains the other output. The canonical standalone adapter was compiled
and executed natively on Windows as well. Counts overlap and are not aggregate
feature coverage.

The installed Linux combination then passed436 native assertions, nine fully
decoded moving-video/997 Hz files,96 PTS/DTS checks and27 first-keyframe/initial
A/V checks. It used unchanged15 fps dimensions and movement thresholds, two
local receivers, two recordings and two replay buffers. The Portrait receiver
was interrupted while Main continued; all sampled movement windows had100%
distinct frames. Only obs-outputs and obs-ffmpeg changed; the previous binaries
were preserved and the other1,809 runtime files remained identical.

The126 normal configuration/keyring files were preserved. Pulse names, defaults,
mutes and volumes were identical, and no application streams remained. The raw
snapshot comparator reported changed runtime indices for one sink and source;
that failure was retained and stable settings were reviewed separately. No
claim is made that runtime indices or other applications' streams stayed equal.
The test process closed normally and its opt-in module was archived.

The clean Linux modules are SHA256
`c80e6334ef86c7200a74c554888d214482ceab2f39e4ca0ecd31fe66695ac358`
(obs-outputs) and
`85b17d1a956becc7e9b871227f9043da2d9791148fb634c12af763e3abc04c18`
(obs-ffmpeg). The frontend/core/API remain the preceding accepted combination.
These short synthetic tests do not certify physical capture, sustained60 fps,
public services or a Windows distribution. The earlier Windows Main RTMP
movement failure remains open pending a separate clean-runtime media acceptance.
Other begin callers and the complete RAW startup transaction remain pending.


## File-output startup failure

Null and FLV outputs now report capture refusal instead of reporting a successful
start. FLV closes only its provisional file and clears its file pointer after
normal termination, preventing a second close during refusal cleanup. An
independent active output keeps its file and remains writable.

Portable Linux and native Windows checks exercise real plugin sources, native
files and threads, with baseline and intermediate negative controls. Each
candidate scenario passes 31 criteria. Capture admission and notifications are
injected; these tests do not certify the entire output lifetime protocol.

The installed Linux regression also passed six simultaneous local outputs, nine
files with moving video and audio, 96 timestamp checks and 27 initial-keyframe/A/V
checks at 15 fps. Physical devices, 60 fps and public services remain separate.
