# Headless real-libobs sync-offset regression

This process exercises real source audio placement, the libobs audio thread,
root view, mixing and `audio_output_connect` PCM. It starts no OBS application,
Qt, graphics, video, encoder or audio endpoint and changes no user configuration.

Contract and thresholds, declared before the first run: two continuous-clock
48 kHz mono sources emit gated997/2003 Hz pulses (200 ms on per1000 ms period).
The longer period disambiguates positive/negative offsets. The sequence is
initial+100 →0 →+250 →0 →+100 →−100 →0 →0(no-op) →+250 →+100 →0 ms.
Every change uses only the public sync-offset setter/getter while the producer
continues. Each stage settles900 ms then captures2600 ms. PCM needs>=96000frames;
trim exactly4800frames at each boundary. Both tones need coherent amplitude>=.001,
RMS>=.004, peak<=.08; 10ms pulse-envelope correlation>=.90. Each measured delay,
relative to zero_baseline, must equal its configured offset within30 ms. Failure
is retained for every stage; an initial false case does not skip later cases.
No threshold changes or selective windows are permitted after seeing results.

A dedicated20 ms producer uses one monotonic sample clock, zero retimestamping
or drops; >100 ms lateness or >3consecutive catch-ups fails the capture. PCM is
buffered in RAM on the callback and written outside the audio thread.

## Test-only routing adapter

Without a video mix the current public canvas API supplies no audio root view.
`audio-view-adapter.c` registers only a zeroed audio-view entry under the real
mixes mutex, then removes it before shutdown. It does not access source buffers,
timestamps, sync-offset fields or the mixing algorithm. The real audio callback
and source implementation run unchanged. The adapter requires version-matched
internal headers; it is never compiled/installed into the product.

The Linux driver relinks frozen real core object files plus this adapter in a
fresh private directory. It preserves object/internal-header/source/library
hashes and exact compile/link arguments. Hidden symbols are not exposed in the
production binary and private ELF offsets are not accessed. Therefore evidence
is a **private real-core relink**, not direct execution of the installed library.
The C++ capture API is portable in design; the private relink driver is Linux
only. Windows requires its own isolated test-core adapter build and execution;
shared code does not certify Windows.

```bash
python3 test/studio-audio-sync/relink-private-core.py --build-root BUILD_CACHE --output-directory PRIVATE_CORE
cmake -S test/studio-audio-sync -B PRIVATE_BUILD -G Ninja -Dlibobs_DIR=PREFIX/lib64/cmake/libobs -DSTUDIO_AUDIO_TEST_CORE=PRIVATE_CORE/libobs.so.30
cmake --build PRIVATE_BUILD
LD_LIBRARY_PATH=PRIVATE_CORE PRIVATE_BUILD/studio-audio-sync-check FRESH_CAPTURE_ROOT
python3 test/studio-audio-sync/analyze.py FRESH_CAPTURE_ROOT --report FRESH_CAPTURE_ROOT/analysis.json
```

The same executable and adapter must run against before/after private cores.
The analyzer records the library actually loaded and its hash. The parent owns
production builds/installations and all OBS app/VM processes.
