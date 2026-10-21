# Studio VST3 host

Imported from the OBS VST3 proposal [obsproject/obs-studio#12752](https://github.com/obsproject/obs-studio/pull/12752), head `8e57b6223fb076dd7427cd7d8d43f12d58578f21` (19 October 2026). The imported host/scanner commit is `80c155eba6745db046c4b26f2566df86a0492c43`. This is a local integration of a proposal, not a claim that upstream OBS has released it. Original pkv/PatTheMav attribution is retained in the source files. Host code is GPL-3.0-or-later; the pinned Steinberg SDK is MIT. License texts are in `licenses/` and installed with the module.

Studio changes:

- Real per-plugin entries in the mixer's VST3 tab, with class-ID selection and rescan.
- Generic normalized parameter controls, including parameters of effects with no native GUI; values are saved alongside the processor/controller state.
- Native X11 editor container under a Wayland Qt app, via XWayland. The main OBS window retains its native Wayland backend. A functioning XWayland display is needed for an X11-only plug-in GUI; generic controls and audio processing remain available without it.
- Restore state before publishing a new processor to audio; serialize state snapshots with processing using a nonblocking audio try-lock (dry audio on contention).
- Retry failed/moved plugin paths after the catalogue is ready, resolving the class ID on the current OS.
- Safe-mode API backport; the host declines to load in safe mode.
- `/usr/lib64/vst3` search path in addition to the VST3 standard directories.

Build prerequisites are the existing OBS toolchain plus the MIT VST3 SDK sources. Fetch the pinned SDK with Python 3 and Git:

```sh
python tools/fetch-vst3-sdk.py --prefix /path/to/dependencies
cmake -S . -B /path/to/obs-build -DENABLE_VST3=ON -DSTEINBERG_VST3_PATH=/path/to/dependencies
cmake --build /path/to/obs-build --parallel
cmake --install /path/to/obs-build
```

On Windows x64, from the repository root, use the existing OBS Windows build prerequisites and preset:

```powershell
py tools/fetch-vst3-sdk.py --prefix C:/StudioDeps
cmake --preset windows-x64 -DENABLE_VST3=ON -DSTEINBERG_VST3_PATH=C:/StudioDeps
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Install the `obs-vst3` module **and** `obs-vst3-scanner` executable using the OBS install/package target. The scanner must sit beside the OBS executable. A DLL copied on its own is insufficient. Plugin binaries must match the OS and architecture (currently Windows x64 and Linux x86_64). The serialized class ID is shared; paths and native plugin builds differ by OS.

Default plugin directories:

- Windows: `%ProgramFiles%\Common Files\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3`.
- Linux: `~/.vst3`, `/usr/lib/vst3`, `/usr/lib64/vst3`, `/usr/local/lib/vst3`.

Only audio effects are offered. Instruments/MIDI are outside this host's scope. Generic continuous controls use normalized 0–1 values; tooltips show the plugin's formatted value at the time the panel is opened. Discrete parameters use their native labels. Native editors provide their own display and advanced features.

Integration fixture: `test/studio-workspace/vst3-mixer-check.cpp`. It uses real AGain and mda Limiter VST3 binaries from the SDK, clicks the mixer, changes parameters, checks PCM, serializes, then verifies after a complete application restart. Its opt-in module is removed from the normal runtime after testing. Native Windows runtime validation must be performed on Windows; a Linux run does not establish it.
