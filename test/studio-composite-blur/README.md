# Standalone Composite Blur GPU probe

Linux fixture with native libobs/OpenGL and the optional plugin built into a
private stage. It does not load the frontend, profiles or sources from the user.
The image source is native; input pixels are a generated checker pattern.

```bash
cmake -S test/studio-composite-blur -B /tmp/obs-blur-check \
  -G Ninja -DCMAKE_PREFIX_PATH=/path/to/native/obs/sdk
cmake --build /tmp/obs-blur-check --parallel 2
/tmp/obs-blur-check/studio-composite-blur-gpu-check \
  /path/to/native/obs /path/to/plugin/stage /tmp/blur-wayland wayland
```

The final argument can be `x11` to use X11/EGL instead of a private Wayland
connection. Evidence includes GPU readback PNGs, pixel statistics, save/reload
and bypass checks, plus JSON results. Source and filter ownership must be
released before reload, with an absent-old-UUID assertion to avoid silently
testing against duplicate identities.

Needs Qt Core/Gui, Wayland client development files, the fork's libobs SDK,
installed OpenGL/image-source resources, and a working graphics session.
`obs_add_data_path` is used as a deprecated compatibility API for explicit SDK
resources; its compiler warning is visible and does not affect production code.
No physical webcam, audio, full-resolution performance, mask coverage, output
concurrency or Windows/Direct3D certification is implied.
