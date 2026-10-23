# Directed initial-keyframe regression

Contract declared before execution: after startup pruning, the **first emitted
video must remain an IDR/keyframe**. Each audio/video DTS sequence is monotonic;
normalized initial A/V timestamps differ by at most one15fps video interval plus
one1024-sample AAC packet (88ms rounded). Emitted A/V durations differ by<300ms.
These bounds do not replace the stricter original media acceptance of another
fixture.

The fixture calls the actual static `interleave_packets`, startup pruning,
initialization and `reset_packet_data` in an exact private copy of
`libobs/obs-output.c`. Encoded-packet metadata and tiny payloads are controlled;
no codec, GPU, physical device, RTMP server, account, wall-clock wait or thread is
needed. It is a directed packet-state test, not encoder/network certification.

Video input starts with IDR0,15fps/GOP30. AAC is48kHz/1024 samples. Cases compare
aligned delivery, audio starting800ms later, an initial45-video-packet burst
before AAC delivery, two audio tracks10ms out of phase, and real packet-state
reset. A burst models queued video already encoded before first AAC is delivered;
it deliberately does not infer that this is the exact scheduling cause on WARP.
The payload preserves the original frame index to prove which input survived
pruning. Baseline FAIL must be preserved before any production patch.

`prepare-private-core.py` without `--build` snapshots core objects, source,
commands and hashes only. With `--build`, it compiles one private replacement TU
and links a private core. It never edits production source, invokes a shared
build, or installs anything. The adapter symbols must never enter the installed
core. Source/object and installed core identities must be checked before/after.
Use isolated CMake with `STUDIO_INTERLEAVE_TEST_CORE`, build j1 only after the
operator releases the CPU, and point `LD_LIBRARY_PATH` at that private directory.
The executable writes JSON and exits1 when the declared contract fails.

A future fix must handle received flags, discarded packet timing references,
audio-track offsets and reset/reconnect semantics. It must preserve queued
packet ownership; merely changing receiver settings, reducing GOP or shifting
expected timestamps is not a fix for this contract.

The expanded private comparison has19 cases/184 criteria plus allocation balance:
baseline10 failing criteria, private candidate184/184 and `bnum_allocs`0→0.
The original seven-case/35-criterion source was preserved and separately rebuilt
against the expanded adapter header: baseline3 failures, candidate35/35.
Additional checks include absolute content timestamps reconstructed from the
payload (no acceptance based only on independently normalized zero), two video
tracks with15/30fps, two AAC tracks10ms out of phase, negative AAC priming,
reordered PTS distinct from DTS, EPT correspondence, delayed GOP, and bounded
controlled missing-IDR input followed by reset. Peak205 in that finite test is
not a production cap or TTL. Those19-case receipts precede production application. The platform validation summary is in `docs/product/STUDIO_RELEASE.md`.


After independent review, three active-delay ownership cases were added:22 cases,
214 criteria plus two allocation-balance checks, baseline12 failures→214/214.
The owning delayed packet is copied with `obs_encoder_packet_create_instance`
then transferred into the real interleaver, with no second caller release; reset
and destruction both balance. The reviewed algorithm was applied only to
`libobs/obs-output.c`, preserving the other six core files. Build/deployment and real media acceptance
are recorded separately; they are not performed by this fixture.
No shared build/deployment is performed by this fixture. The old private19-case
receipts remain historical. Use `--obs-output-source` with a preserved original
source to reproduce the baseline; never replace the repository file to do so.

Windows portability: `adapter.c` defines `STUDIO_INTERLEAVE_PROBE_CORE` before
including `probe.h`; the private header marks declarations dllexport in the core
and dllimport in the helper on Windows. MSVC requires declarations and exported
definitions to have consistent linkage (C2375 otherwise). Non-Windows declarations
remain plain. This only affects private test exports; production has none.
