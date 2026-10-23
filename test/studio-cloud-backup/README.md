# Cloud-backup credential/UI regression

Standalone Qt Widgets helpers compile the production Cloud dialog/client, OAuthTokenSet and SecureTokenStore request context. Configure with CMake, then build with one job. Use a fresh build directory when comparing source snapshots: copying older modification times over a cached build does not prove those bytes were compiled.

`fake/` supplies App/config, translations, AuthListener and a registered browser URL sink. `fakes.cpp` supplies an in-memory delayed credential store, HTTP/refresh/code-exchange responses and Archive. No system credential store, real HTTP client, browser, normal configuration, libobs startup or provider is used. The scheduler adapter changes only its OBSBasic include; it does not represent the complete main window.

Run widgets with `QT_QPA_PLATFORM=offscreen` and private runtime/config directories. Preserve stdout, stderr, exit status and source/binary hashes for every run; keep failed results. Expected Linux scopes are separate suites, with overlapping criteria:

| Executable | Argument | Criteria |
| --- | --- | ---: |
| studio-cloud-backup-check | none | 58, retaining the original 48 |
| studio-cloud-backup-check | --scheduler-queue | 8 |
| studio-cloud-modal-check | none | 111 |
| studio-cloud-modal-check | --baseline19 | 19, the original directed modal reproduction |
| studio-cloud-modal-check | --shutdown-before | 7 |
| studio-cloud-modal-check | --remote-before | 15, stale remote intents after the fix |
| studio-cloud-modal-check | --restore-session | 8 |
| studio-cloud-modal-check | --busy-tree | 7, actual viewport double-click |
| studio-cloud-modal-check | --auth-before | 24, private fake authorization after the fix |
| studio-cloud-modal-check | --auth-shutdown | 9 |
| studio-cloud-format-check | none | 23, retaining 17 frozen format criteria |
| studio-cloud-transaction-check | none | 50 |
| studio-cloud-session-check | none | 65 |
| studio-cloud-settings-check | none | 23 |
| studio-cloud-registration-check | none | 6 |
| studio-cloud-archive-ui-check | none | 18, restore-options root context and credential controls |

The historical `--remote-before` and `--auth-before` names identify preserved before/after scenarios; they execute corrected behavior in the final source. `studio-cloud-registration-check --null-config` is also available for subprocess-only unavailable-config reproduction. The pre-fix process terminated with SIGSEGV; disable core dumps and never run such a crash reproduction in the main test process.

The legacy scheduler happy-path fixture establishes authorization through real `StoreTokens` on a worker before enqueueing. Injecting fake RAW externally after a NotFound observation is a different scenario: the GUI revision becomes stale and the queued operation must refuse to start. The explicit queue test covers this boundary.

Credential tests cover exact RAW recovery, new authorization serialization, unexpected external replacement/absence, durable GUI decisions, stopped or destroyed receivers and late deliveries. Session tests cover old-item/new-token prevention, both upload phases, pagination and retention removal. Draft tests retain valid local edits while refusing stale settings after independent authorization edits. OAuth exchange is counted at entry so shutdown acceptance proves that a queued noncancellable request never started.

`messagebox-contract.cpp` preserves the directed QMessageBox oracle reproduction: use an actual Yes button click, not `done(Yes)`. Real libcurl cancellation has separate loopback coverage in `studio-http-cancel`; the 75-criterion OAuth credential-removal suite lives in `studio-oauth-removal`.

The additional archive UI helper accepts the real restore-options dialog twice: an unchanged synthetic root reaches fake staging, while a scene-root-only change rejects it without changing account metadata or RAW. It also checks that credential restore remains disabled and unchecked with an explanation. Its root capture covers synthetic config/scene paths, not native filesystem identities, every root, or an unavailable App.

These helpers do not certify real services or credentials, Archive/root isolation, performance of the complete app, media or Windows execution. Archive/Create/StageRestore are substituted here; their pending-publication/root contract requires separate real Archive tests.
