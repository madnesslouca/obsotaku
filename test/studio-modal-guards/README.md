# Account registration modal guards

Build against an existing OBS development prefix:

```sh
cmake -S test/studio-modal-guards -B /tmp/studio-modal-guards -G Ninja -DCMAKE_PREFIX_PATH=/path/to/obs-prefix
cmake --build /tmp/studio-modal-guards -j2
ctest --test-dir /tmp/studio-modal-guards --output-on-failure
```

This target links the actual Accounts dialog and drives its Qt input prompts.
The application/config adapter uses a temporary INI. Credential storage,
registration transaction delivery, HTTP and artwork are test substitutes;
no desktop credential collection, public service, browser or OBS instance is
used. The configuration guard adapter checks scope/profile and registration
fields. The separate channel-store transaction regression exercises the real
Store guard and durable transaction implementation.

Cases change profile, client registration, effective environment client,
exchange endpoint and callback port during the first or second prompt. Late
conflicts are inserted at the simulated transaction boundary. Unchanged input
commits; canceled prompts write nothing. Operation signals are checked for a
matching token and are emitted only after a successful current commit.

This is a Qt boundary regression test, initially executed on Linux offscreen.
It does not certify Windows native credential access, real Secret Service
transactions, an authenticated provider or the main-window Accounts caller.
Native application tests cover that caller separately.
