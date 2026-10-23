# OAuth credential-removal lease checks

Standalone source-level regression of the production ConnectedAccountManager, OAuthTokenSet, SecureTokenStore request context and StreamPlatform mapping.

Configure this directory with CMake and OBS_CONFIG_DIR pointing to an existing generated obsconfig.h directory. Build with one job, then run studio-oauth-removal-check. It requires Qt Core headers/libraries only; it never creates an application window, starts libobs, links an HTTP client, uses a system keyring, or calls a public service.

The deterministic storage substitute records calls, can commit before returning failure/cancellation, and can reject recovery. The OAuth refresh substitute blocks under the real manager account mutex. The test checks cooperative cancellation/deadline, same-mutex serialization, backup after rotation, exact RAW recovery, explicit incomplete rollback, absence of destructor credential I/O and lock release.

Expected Linux result: 75 criteria, zero failures; output includes measured cancelled/expired lock waits. Backend/native-keyring tests live in studio-secure-token-store. GUI and Windows acceptance are separate.
