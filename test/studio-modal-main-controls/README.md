# Main-window destination modal regression

This opt-in module drives the actual frontend Add picker, manual form and Remove confirmation through Qt actions. It inserts external ProfileDir or destination metadata changes during the modal and expects a visible rejection, no credential transaction, and preservation of the existing destination/card and external state.

Build against a matching OBS development prefix with Qt6 Widgets:

```sh
cmake -S test/studio-modal-main-controls -B /tmp/studio-modal-main-controls -G Ninja -DCMAKE_PREFIX_PATH=/path/to/private/obs-prefix
cmake --build /tmp/studio-modal-main-controls -j1
```

There is intentionally no install target or launcher. The operator must use a disposable app/profile, private platform credential service or QA logon, no public service/capture/outputs, and preserve the normal application. Never load this module into the user's normal profile. This actor mutates only the independently prepared QA profile and can trigger a credential write if run against a regressed frontend. Only an embedded synthetic canary is used; the module never prints credential values.

The independently prepared configuration must contain one manual metadata row with Id `qa-manual-id`, Count1 and a `[PrivateCredentialUI] Nonce` equal to a fresh32hex `OTAKUDOBS_PRIVATE_CREDENTIAL_UI_NONCE`. `OTAKUDOBS_PRIVATE_CREDENTIAL_UI_ROOT` must be a new private evidence directory and `XDG_CONFIG_HOME` exactly that directory plus `/config`. Seed the QA profile/scene outside this module, disable its websocket and preview, and supply a private credential service. The nonce and known seed prevent unintended execution; they do not replace process/configuration isolation.

`OTAKUDOBS_PRIVATE_CREDENTIAL_UI_MODE` selects exactly one of `add-picker-profile`, `add-picker-metadata`, `add-form-profile`, `add-form-metadata`, `remove-profile`, or `remove-metadata`. Run each in a fresh private profile, wait for the actor's runtime.json and application exit, and archive the module outside all plugin search paths. Retain failed receipts. The JSON contains criteria and heartbeat; screenshots capture rejection/card state. The operator separately verifies backend identity and preservation of normal files/credential store.

This target does not claim credential-store failure recovery, authenticated OAuth, streaming, media correctness or full platform parity. Native Linux execution and native Windows execution require separate receipts; compiling shared C++ alone certifies neither.
