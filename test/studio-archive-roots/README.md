# Local roots and global settings regression

This standalone Qt Core target compiles the real Archive implementation, encryption/serialization code and a small App-path adapter. The credential dependencies refuse and count every call; no account, keyring, browser, network, media device or OBS instance is used.

Build against the source snapshot being tested, with a fresh build directory:

```sh
cmake -S test/studio-archive-roots -B /tmp/studio-archive-roots -G Ninja
cmake --build /tmp/studio-archive-roots -j1
STUDIO_ARCHIVE_TEST_ROOT=/absolute/new/private-directory /tmp/studio-archive-roots/studio-archive-roots-check
```

`STUDIO_SOURCE_ROOT` may identify another frozen source tree explicitly. The fixture rejects an existing cases directory. On Linux it sets private XDG directories before constructing Qt. Windows requires an isolated QA account/VM and `STUDIO_ARCHIVE_PRIVATE_WINDOWS=1`; Qt standard-path test mode is enabled before the application. Windows also requires the normal MbedTLS development package. Never run against normal App/configuration/credential paths.

The first scenario seeds an existing pending scene restore, removes only its temporary local global.ini, then imports a global.ini containing valid paths to a different existing temporary configuration. The new Stage must fail with a useful local-read error, preserving the old marker, stage and live scene graph. A readable-local control accepts Stage/Apply, preserves its custom Locations section and restores other incoming settings. A transport without global.ini can still restore user.ini when the local global file is absent. Locations in user.ini remain ordinary content; only the application's global settings select storage roots.

Keep stdout, stderr, result.json, exit status and source/binary hashes, including failing before results. Counts include setup preconditions and overlap other Archive suites. This test does not run a real frontend startup, certify encrypted transport or credentials, prove multi-process locking, inject filesystem faults or cover hostile concurrent filesystem changes. Root-binding, rollback, GUI roundtrip and native Windows execution need their own evidence.
