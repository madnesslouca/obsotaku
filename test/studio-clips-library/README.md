# Isolated clip library check

Builds the production `StudioClipsDock` with Qt/libobs and isolated app/config
adapters. The `ffmpeg_source` fixture only stores settings: it cannot certify
media decoding, audio, graphics, main-window insertion or Windows execution.
All files and libraries belong to a disposable `QTemporaryDir`.

```bash
cmake -S test/studio-clips-library -B /tmp/obs-clips-library-check \
  -G Ninja -DCMAKE_PREFIX_PATH=/path/to/native/obs/sdk
cmake --build /tmp/obs-clips-library-check --parallel 2
QT_QPA_PLATFORM=offscreen /tmp/obs-clips-library-check/studio-clips-library-check /tmp/clips-library-result.json
```

The same CMake target uses native Qt/libobs on Windows; that execution has not
yet been performed. `ctest` runs the executable with the offscreen Qt platform.
