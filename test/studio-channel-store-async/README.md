# Asynchronous channel credential transactions

Build this standalone target against an existing Linux OBS development prefix:

```sh
cmake -S test/studio-channel-store-async -B /tmp/studio-channel-store-async -G Ninja -DCMAKE_PREFIX_PATH=/path/to/obs-prefix
cmake --build /tmp/studio-channel-store-async -j2
ctest --test-dir /tmp/studio-channel-store-async --output-on-failure
```

The test runs the real channel store, configuration library, request contexts and
OAuth removal lease in a Qt Core application. A small application adapter owns
only a temporary configuration. An in-memory credential service supplies delays,
locked reads, writes whose reply is cancelled after commit and recovery errors.
No OBS instance, desktop credential collection, OAuth endpoint or physical device
is accessed. Tokens and stream keys are never printed.

Checks cover backup ordering and a single transaction context, GUI responsiveness,
cache identity, Busy metadata changes, cancellation, config-save recovery, profile
and metadata conflicts, shared-account preservation, orphan removal, receiver
destruction, a state change after durable commit but before runtime delivery, and
shutdown without pumping a nested event loop. The configuration failure adapter
exists only in the test translation unit.

This is a Linux transaction regression test. It does not certify Windows native
Credential Manager behavior, a real Secret Service implementation or application
widgets. Native application gestures and credential persistence require separate
integration evidence.
