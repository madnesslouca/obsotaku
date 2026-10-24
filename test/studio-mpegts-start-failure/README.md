# MPEGTS start and retirement checks

This optional standalone harness compiles the complete MPEGTS output translation
unit from an explicitly selected source root. It uses native threads, locks,
FFmpeg contexts and AVIO, with injected OBS begin/metadata and transport seams.
It checks start refusal, terminal ordering, public Stop during retirement,
late metadata callbacks, disposal outside the data mutex and AVIO flush lifetime.
The fixtures do not certify live SRT/RIST transport, decoded media, arbitrary
concurrent controls, global shutdown or uninstrumented dependency race freedom.

Configure with CMake3.28+, C17 and a matching complete OBS SDK source/build,
libobs CMake package, exact core/import library and FFmpeg/SRT/RIST development
packages. Supply absolute existing SDK_SOURCE, SDK_BUILD, CORE_LIBRARY,
AFTER_SOURCE_ROOT and libobs_DIR. AFTER_SOURCE_ROOT contains
plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c and obs-ffmpeg-output.h. Dependency roots
can be supplied through CMAKE_PREFIX_PATH. Nothing is installed or loaded into
an existing OBS application. On Windows use a matching MSVC/Windows SDK.

Build with cmake --build <fresh-build> --config Release. Run Python3 run.py
--build <binary-directory> --output <new-result-directory>, adding one or more
--runtime-dir arguments for matching native dependencies if needed. The runner
accepts24 jobs: one native preflight plus23 fixed output cases, each with a12s
deadline. It stops on unexpected exit, count, failure or timeout. Input hashes
are written at configure time; executable hashes and raw case output are saved
by the runner. Named input hashes are not loaded-module map attestation.

An optional CONTROL_SOURCE_ROOT may supply the preceding disposal implementation
with the same ownership layout. Use --with-control only with that defined
control: it must preserve the18 earlier modes and two public Stop checks, while
failing exactly3 predicates for each heavy-close gate and2 for AVIO opaque
lifetime. No control source is bundled, and an arbitrary old release is not a
compatible control. Negative witnesses guard invalid access instead of invoking
undefined behavior. Keep controls separate from successful candidate cases.

For sanitizers use the matching compiler/link flags and native dependency
runtime. Instrumentation of the translation unit and fixture does not instrument
all libraries or prove absence of every race or leak in OBS.

The result reader requires a single final TOTAL, one row per check and the exact
ordered failed-check names for each negative control; matching counts alone do
not accept a different failure. Malformed or extra summary rows stop the run.
