# Active-child accounting checks

Opt-in native tests for child reference accounting, scene/group reparenting,
visibility audio actions and transitions. Use a disposable test core with the
matching external SDK headers. No normal application, profile, plugin directory
or installed library is modified. Shared source is not Windows certification.

The fixtures use synthetic997Hz/2003Hz PCM, source graphs and real libobs
lifecycle operations. They do not capture devices, authenticate services or
render GPU effects. The original group-hide criteria retain the150ms maximum
callback gap and the existing amplitude/frame thresholds.

## Dependencies and build

Configure this directory as a standalone CMake project using C17/C++17 and a
libobs SDK package. The package supplies transitive SIMDe and pthread support;
the explicit library argument selects the test core. Supply matching source,
generated headers and library artifacts through these cache arguments:

- `STUDIO_GRAPH_CORE_SOURCE_DIR`: full external OBS SDK source tree.
- `STUDIO_GRAPH_CORE_LIBRARY`: exact test shared library or Windows import `.lib`.
- `STUDIO_GRAPH_CORE_CONFIG_INCLUDE_DIR`: SDK `build/config` with `obsconfig.h`.
- `STUDIO_GRAPH_CORE_GENERATED_INCLUDE_DIR`: SDK `build/libobs` generated headers.
- `STUDIO_GRAPH_TRANSITION_SOURCE_DIR`: SDK Cut/Fade source directory; defaults
  to `plugins/obs-transitions` under the supplied source tree.
- `CMAKE_PREFIX_PATH` or `libobs_DIR`: matching SDK package/dependency locations.

Configuration records source and link-library hashes in `sdk-manifest.json`
inside the build directory. The transition implementations are compiled directly
from that external SDK;
this directory does not duplicate product sources. On MSVC use x64, matching
`w32-pthreads_DIR` and the SDK's `cmake/finders` in `CMAKE_MODULE_PATH`. Prefer
forward slashes in CMake paths. Build with one job; do not install the test core.

The test core needs the wrappers from `adapters/graph-shim.c` and
`adapters/route-adapter.c`. In an owned build, explicitly include
`cmake/AttachTestAdapters.cmake` and call
`studio_active_graph_attach_adapters(libobs "<SDK-source-root>")` after creating
the libobs target. That hook affects only its specified test target. It calls
the real audio registry from the graphics-free route adapter; it does not
change PCM, timestamps, visibility actions or the accounting algorithm. There
is no algorithm feature flag. Do not attach duplicate adapters. Hide/preflight
mutex readers are compiled into the executable and need no production export.

## Run and interpret

Each executable accepts a new absolute JSON path and refuses an existing file.
Run only against the intended test runtime and supply its dependency search
paths. Optional `run.py` requires Python3 and accepts `--build-dir`, a fresh
`--output-dir`, `--core-file` and repeatable `--runtime-dir`. The core argument is
the runtime DLL/shared library, not its import library. Windows runs use the
exact core DLL beside the copied executable; Linux puts its directory first in
the child library path. The runner requires the exact runtime basename
`obs.dll` on Windows or `libobs.so.30` on Linux, including the resolved filename;
arbitrary aliases are rejected before any output directory is created. Its
hashes identify that named artifact, not each helper's mapped module. Search
paths and a pinned file do not replace external process-map evidence. Per-suite
configuration roots are isolated and Pulse is denied. No user project is opened.

| Suite | Criteria | Scope |
| --- | ---: | --- |
| Graph | 43 | Multiplicity, MAIN/AUX, cycles, concurrent edges, rollback, callbacks and ownership |
| Scene | 21 | Actual group insertion, reparenting, reorder, ungroup, nesting and cycle refusal |
| Lifecycle | 15 | Three complete startup/shutdown cycles |
| PCM | 23 | Queued group hide while its video lock remains held400ms |
| Contention | 24 | Same audio deadline while an independent layered graph changes |
| Overflow | 23 | Signed32 guards, failed root/edge/batch cleanup and4096-source chain |
| Preflight | 8 | Generic enumeration precedes caller-owned scene audio locks |
| Transition | 60 | Actual transition state, start/clear/swap and callback symmetry |
| Cost, optional | 4 | Count checks and reported layered-graph timing samples |

These suites overlap; their counts are not unique product coverage. Cost samples
are observations, not worst-case timing guarantees. `STUDIO_GRAPH_BUILD_COST`
controls the optional cost executable; use `--with-cost` in the runner. The
`legacy_recursive_node_visits` metric describes the old path-expansion formula,
not a measured visit counter for the optimized implementation.

The graph adapter drains the actual test destruction queue. The public destroy
wait refuses graphics-free hosts, so using that wait for immediate destruction
assertions would be invalid. Callback delivery stays the existing tick-based
reconciliation; callbacks can reenter accounting outside its mutex.

Cut uses the actual SDK implementation and factory. Fade compiles the actual
SDK translation unit with only shader creation replaced by an allocated
`fade_info` sentinel. No shader, visual fade, Stinger, file decoding or PCM
crossfade is certified. The overflow fixture requests a64KiB pthread stack;
platform-specific stack and clock preconditions need native evidence.

Generic full-tree enumeration remains outside the accounting mutex and can be
slow or reentrant. A process-wide mutex is not a hard-real-time guarantee for
arbitrary graph sizes or batches. A precondition failure, missing JSON, timeout
or changed criterion is not a passing regression. Diagnose those separately
from product defects and preserve the failing run.
