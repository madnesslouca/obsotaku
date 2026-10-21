# Studio workspace — implementation and validation

This fork extends OBS with a Studio workspace built on Qt and libobs. It retains
OBS licensing, upstream contributor credits and third-party notices. The shared
implementation targets Windows and Linux; the validation below has a defined
scope and does not certify every device, plugin or service on both systems.

## Included functionality

- Main and Multi Canvas workspace switching, portrait layers and inspector;
  direct selection, transforms, crop, group editing and Undo/Redo.
- Output-specific canvas selection and encoder settings, concurrent landscape
  and portrait recording, replay buffers, clips and multistream destinations.
- Audio mixer strips with source gain, mute, monitoring/CUE, effects, per-scene
  audio settings and recording/VOD routing. Undo changes the edited fields while
  preserving unrelated source changes.
- Built-in audio processing and optional VST3 hosting with plugin state storage.
  Plugin availability and compatibility depend on the operating system and the
  installed plugin. Windows plugin binaries cannot run as Linux plugins.
- Unified chat with platform events and emotes, browser sources and widgets.
- Portable source tests and local media validators under `test/` and `tools/`.

## Latest audio and profile checks

Validation date: 2026-10-20. Tests used isolated configurations and local receivers.

| Environment | Executed checks | Scope |
| --- | --- | --- |
| Linux | 648 CUE/VOD checks across 14 stages | PCM, AAC, gain, mute, monitoring and routing; digital endpoint capture |
| Linux | 72 active archive checks | Actual local RTMP program/archive encoder binding |
| Linux | 11 audio-buffer stages and 48 profile-default checks | Delay changes through zero and profile duplication/settings regression |
| Windows VM | 683 CUE/VOD checks across 15 stages | PCM, four fully decoded AAC outputs, virtual monitoring endpoint |
| Windows VM | 83 active archive checks | Actual local RTMP delivery and program/archive encoder slots |
| Windows VM | 48 profile-default checks | Duplicate profile, open output/audio settings, cancel and remove test profile |

The Windows execution used Windows 10 Pro 22H2 build 19045, MSVC 19.44,
Qt/libobs, WARP graphics and virtual HDA audio. Its QA build omitted browser,
virtual camera, scripting and OAuth. These results establish the tested audio,
profile and local RTMP behavior; they do not certify a complete Windows package.

Corrections covered by these checks:

- Mute remains independent of monitoring-only mode; changing CUE cannot silently
  reopen a monitoring-only source to the program mix.
- VOD exclusion follows the active archive encoder rather than a pending setting.
- Audio delay changes from zero correctly reposition buffered audio.
- Duplicated profiles receive effective module-dependent codec defaults before
  output settings are read.
- The Windows secondary-architecture build retains the selected Visual Studio
  installation.

## Remaining validation

- Complete Windows distribution with browser, OAuth, virtual camera and third-party
  VST3 plugins.
- Physical Windows capture/audio devices and sustained 60 fps capture.
- Full simultaneous landscape/portrait streaming, recording and replay acceptance
  on Windows, plus long-duration stress tests.
- Public-service VOD delivery; local encoder routing is not a hosted-service test.
- Acoustic monitoring quality and hardware latency; digital endpoint measurements
  do not measure sound at the listener's ears.
- Improve the selected Main Canvas card contrast on Windows.

## Build and test notes

Initialize the upstream submodules before building. Use the OBS build instructions
for the target platform and retain the bundled dependency and license notices.
The optional VST3 module requires a compatible SDK; `tools/fetch-vst3-sdk.py`
fetches and verifies pinned revisions without changing an existing modified SDK.

The launch helpers accept an isolated runtime/configuration location through
`OBS_STUDIO_PREFIX` and `OBS_STUDIO_CONFIG_HOME` on Linux. The Windows helper is
`tools/run-studio-windows.ps1`. Do not install opt-in QA modules in a production
profile: follow each fixture's setup and teardown requirements and use local
receivers. Generated helper reports belong in ignored `validation/` directories,
not in the source documentation.
