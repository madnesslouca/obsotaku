# Null/FLV output startup refusal

This standalone project compiles the actual plugin translation units against
an explicit matching external OBS SDK and libobs. It does not install modules
or open user profiles. Configure the six required `STUDIO_FILE_*` paths shown
in CMakeLists.txt and provide `libobs_DIR` for that SDK's CMake package.

Each executable takes four arguments: a new result TSV path, two nonexistent
temporary FLV paths, and one of `basic`, `stop-refusal`, or
`packet-error-refusal`. A candidate run expects 31 checks and no failures in
each scenario. A pre-fix baseline expects 9, 9, and 6 failures respectively.
An optional intermediate control expects 0, 0, and 1 failures. Exit code 1 is
expected only for those negative controls. Preserve their result files.

The test uses native files, threads, mutexes and OBS settings. It substitutes
capture/preflight and output notifications; it does not encode valid media or
exercise the public output lifetime protocol. A healthy output's original file
must remain writable after the other output refuses capture. Retry and normal
Stop must still work. A rejected capture must not trigger core detach.

The packet-error scenario injects the actual encoded-packet callback during a
substituted begin call before that call returns false. The present core normally
returns false before attaching callbacks. This is a prospective caller test,
not proof of partial-attach rollback. A second fclose request is counted and
suppressed in the negative control rather than executing undefined behavior.

Record the source manifest, result TSVs, compiler, SDK, linked core and runtime
module identities for each platform. A successful Linux build does not validate
Windows. Keep the fixture outside the product startup and recording paths.
