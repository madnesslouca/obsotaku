# Native Windows endpoint recorder

Standalone test helper for the Windows VM, using the Windows SDK and MSVC.
It lists render endpoints or captures a specifically selected endpoint through
WASAPI loopback. It never selects a new default or changes endpoint volume.
No OBS SDK, microphone, account or Internet service is required.

```powershell
cmake -S test/studio-wasapi -B build-wasapi -G Ninja
cmake --build build-wasapi
build-wasapi/studio-wasapi-loopback.exe --list-devices
build-wasapi/studio-wasapi-loopback.exe --device-id "ID_FROM_LIST" --seconds 3 --output C:\StudioTest\cue.wav
```

The recorder uses the endpoint's actual shared mix format and writes a WAV.
If an inactive endpoint supplies fewer frames, the end is padded with silence;
JSON stdout reports received, written and padded frames, discontinuities and
timestamp errors. A successful recording alone does not prove a tone is
present, CUE routing is correct, or audio reached a physical speaker. Decode
the file and compare known tones against the expected routing state. This
short helper is not a synchronized clock/latency measurement tool: gaps are
reported as discontinuity flags, not reconstructed by device timestamps.

Errors go to stderr with a nonzero exit. Existing captures are not overwritten.
Do not run this outside the isolated test VM without choosing the intended
endpoint and local private output path. The helper's build/execution status
must be recorded separately from the OBS fork's acceptance.

Primary API references:

- [Microsoft loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [GetMixFormat](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat)
- [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)
