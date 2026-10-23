# Native credential-store regression

This standalone helper links the real SecureTokenStore backend and its portable request context. It does not install OBS modules or access media devices. Install the normal development headers for libsecret on Linux, then configure this directory with CMake and build with one job.

On Linux, use run-linux.py with --probe pointing to the built studio-secure-token-store-check and --root pointing to a NEW private artifact directory. The runner refuses to overwrite earlier results. It creates a separate dbus-run-session for each scenario, with no activation service directories, private XDG directories, and private keyring control sockets. It starts and stops only its own foreground services. gnome-keyring-daemon, dbus-run-session, gdbus and Python GObject introspection are required.

Never run the native executable directly against the normal desktop bus/keyring. Its Linux guard requires an address marker installed only inside the private runner. The real-service scenario tests persistence after restarting the private daemon; protocol fixtures test locked/dismissed unlock, stalled calls, sanitized errors, and ambiguous write completion after cancellation or timeout. Synthetic canaries are intentionally public fixture constants. No credential values or remote error strings are printed.

Windows uses the standalone CMake target with native Credential Manager. Run ONLY the windows-native mode under an isolated QA VM/account and set OTAKUDOBS_PRIVATE_WINDOWS_STORE=1 explicitly. This mode writes only the otakudobs-qa-private fixture targets and removes them. It checks typed statuses, exact read/update, pre-call cancellation/deadline and rejection of NUL identifiers before OS calls. Expected: 31 criteria. Windows native calls cannot be interrupted midway.

Do not run the Linux roundtrip mode as Windows validation: its structured namespace assertion is specific to Linux attributes. The existing Windows target concatenation is preserved and requires a separate migration plan if changed.

The platform validation summary is in `docs/product/STUDIO_RELEASE.md` in published source snapshots. Passing this helper is not proof of UI responsiveness, config transaction correctness, real-account authentication, or media behavior. On Windows, verify the test process uses the intended desktop logon: matching user and SessionId alone do not establish that its token can access Credential Manager. A direct native write/read/remove probe can distinguish a logon precondition from a backend defect.
