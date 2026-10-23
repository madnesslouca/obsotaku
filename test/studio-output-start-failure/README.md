# Output startup refusal checks

Opt-in standalone native tests for the RTMP, FFmpeg mux and replay callers of
`obs_output_begin_data_capture`. Build actual plugin translation units from
explicit external before/after source roots; no product source is duplicated
here and no installed application, profile or plugin directory is modified.

These tests inject the OBS preflight/begin result and metadata/RTMP protocol.
Native threads, mutexes, events, semaphores, owned loopback TCP sockets, libobs
settings/deques/process pipes and the compiled helper remain real. They test
caller refusal handling, startup-resource ownership and callback stop contexts.
They do not test core attach rollback, encoded media, an RTMP handshake, GUI
state, authenticated services, physical devices or a complete concurrent-output
startup. Shared code is not native Windows certification.

## Build

Use CMake3.28+, C17 and a matching complete libobs SDK. Required dependencies are
Threads, jansson, MbedTLS and FFmpeg avformat/avcodec/avutil. Supply these absolute
cache inputs, and `libobs_DIR` or `CMAKE_PREFIX_PATH` for the matching SDK package:

- `STUDIO_FAILURE_SDK_SOURCE_ROOT`: full external project source, including plugin
  headers, FLV/AV1 helpers and the AMF implementation.
- `STUDIO_FAILURE_CORE_LIBRARY`: exact runtime shared library on Linux, or the
  matching import library on Windows.
- `STUDIO_FAILURE_CONFIG_INCLUDE_DIR`: generated config directory with obsconfig.h.
- `STUDIO_FAILURE_GENERATED_INCLUDE_DIR`: generated libobs headers.
- `STUDIO_FAILURE_BEFORE_SOURCE_ROOT` and `STUDIO_FAILURE_AFTER_SOURCE_ROOT`: roots
  containing plugins/obs-outputs/rtmp-stream.c, its matching header, and
  plugins/obs-ffmpeg/obs-ffmpeg-mux.c. Partial source snapshots are sufficient.
- Optional `STUDIO_FAILURE_INTERMEDIATE_V1_SOURCE_ROOT` and
  `STUDIO_FAILURE_INTERMEDIATE_V4_SOURCE_ROOT`: additional RTMP negative controls
  with matching rtmp-stream.c/header, built without duplicating product code.

The SDK finders provide transitive dependencies; on MSVC also supply matching
w32-pthreads and cached dependency paths. The build uses /utf-8, /Gy, /Gw and
/OPT:REF on MSVC; other builds use function/data sections and linker collection
for unused backend paths. MSVC can retain callback entry points: the Windows
build adds real log, network-interface and HEVC helper translation units plus
iphlpapi. Seven excluded connector setup/connect/initialization/destruction
entry points abort with FORBIDDEN if reached; they cannot report fake success.
The tested caller, cleanup, metadata injection and owned workers are unchanged.
STUDIO_FAILURE_VERIFY_RETAINED_LINK_SUPPORT enables this linkage in an explicit
non-Windows diagnostic build; it defaults off. Build with one job.
Configuration records input hashes in sdk-manifest.json. The import-library hash
is not proof of the runtime DLL mapped by a process: pin the runtime and record
loaded-module evidence separately. Do not install a test runtime.

## Run

Use a new absolute result TSV path for every run. On Windows run copied helpers
beside the exact runtime obs.dll, with matching dependency bins on private PATH.
On Linux select the exact runtime through the child library path. Test executables
return0 only when every assertion passes,1 for failed criteria, and another code
for an input/fixture error. Parse PASS/FAIL/TOTAL lines; an exit code alone is not
coverage. A baseline is expected to fail the criteria its correction addresses.

The mux executables accept RESULT.tsv, a fresh owned media-probe path, and the
absolute path of `studio-output-start-failure-helper` (append .exe on Windows).
Existing media probes or process markers are refused. The helper reads stdin
until EOF and creates a fresh completion marker. No shell or external command
is needed. A healthy helper stays open while another mux start is refused; the
replay's retained ring and previous saved path must remain intact.

RTMP executables accept RESULT.tsv and one scenario:

| Scenario | Linux criteria | Windows criteria | Purpose |
| --- | ---: | ---: | --- |
| basic | 16 | 34 | Explicit begin refusal and success control |
| early-false | 20 | 42 | Worker exits while begin is blocked |
| stop-true | 20 | 42 | Actual Stop callback while connecting=false |
| meta-fail | 20 | 42 | Worker retirement plus failed metadata before begin |
| meta-retire | 20 | 42 | Known worker error prevents later begin |
| connecting-stop | 20 | 42 | Actual Stop with current-thread connector ID |
| other-connecting-stop | 21 | 44 | Stop still joins a different owned connector |
| unsupported | 16 | — | Linux rejects the unsupported socket-loop startup |
| socket-fail | — | 22 | Windows secondary-worker creation failure |

Mux/replay has27 criteria on either platform. Counts overlap, including repeated
success controls, and are not unique feature coverage. The Windows RTMP variants
also exercise a controlled secondary worker using native exit events: this is
ownership/join coverage, not execution of the full Windows socket backend.

The connecting-stop test sets a current-thread ID and calls the actual plugin
Stop function from the injected begin context. The external-thread case uses a
real owned worker released by an event. Neither is execution of the original
network connect_thread or arbitrary concurrent start/stop/destruction. Negative
controls record and suppress detach/invalid/self-join requests instead of
intentionally executing an unsafe native join; fixture cleanup retains the
right to join every owned thread. Early worker failure uses an error injected
after a real semaphore wake. It does not contact a failing remote service.

The exact legacy caller contract matters: a false return is handled without
calling end_data_capture as generic rollback. A committed successful Stop may
end capture normally. These tests cannot establish that a core begin operation
is itself transactional or that every transport/backend failure is isolated.
